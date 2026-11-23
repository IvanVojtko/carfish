#include "controller.h"

#include "aap/session.h"
#include "apps/appregistry.h"
#include "apps/mpriscontroller.h"
#include "apps/phonemanager.h"
#include "led/ledcontroller.h"
#include "media/carscreen.h"
#include "transport/tcptransport.h"
#include "transport/usbffstransport.h"
#include "transport/usbhelperclient.h"
#include "util/credentials.h"
#include "util/uanetwork.h"
#include "wireless/bluezprofile.h"
#include "wireless/wirelesssetup.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

Q_LOGGING_CATEGORY(lcCtl, "aap.controller")

// Durable log on the device so diagnostics survive closing the app; read it
// with:  ssh defaultuser@<ip> 'cat ~/carfish.log'
static const QString kLogFile = QDir::homePath() + QStringLiteral("/carfish.log");

static constexpr int kMaxLogLines = 200;
// Android's "head unit server" developer option listens on the same port.
static constexpr quint16 kHeadUnitServerPort = 5277;

Controller::Controller(QQmlEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_settings(QStringLiteral("carfish"), QStringLiteral("carfish"))
    , m_apps(new AppRegistry(this))
    , m_mpris(new MprisController(this))
    , m_phone(new PhoneManager(this))
    , m_leds(new LedController(this))
    , m_screen(new CarScreen(engine, this))
{
    m_screen->setMediaSource(m_mpris);
    m_screen->setAppRegistry(m_apps);
    m_screen->setPhone(m_phone);
    m_screen->setLeds(m_leds); // Bluetooth ambient lights, on the car's Settings screen
    // Stamp an identifying User-Agent on every QML network request so OSM serves
    // real tiles instead of its "access blocked" placeholder. Must be set before
    // any networked QML (Image tiles) is instantiated.
    if (m_engine) {
        m_namFactory = new UaNetworkAccessManagerFactory;
        m_engine->setNetworkAccessManagerFactory(m_namFactory);
    }
    // Let the car QML drop diagnostics into the durable log (carScreen.log(...)).
    connect(m_screen, &CarScreen::logMessage, this, &Controller::appendLog);
    connect(m_phone, &PhoneManager::log, this, &Controller::appendLog);
    connect(m_leds, &LedController::log, this, &Controller::appendLog);
    // Push the saved TomTom traffic key + toggle and the map orientation to the car
    // map, and persist them when they are flipped from the on-map buttons.
    m_screen->setTrafficKey(trafficKey());
    m_screen->setTrafficEnabled(trafficEnabled());
    connect(m_screen, &CarScreen::trafficEnabledChanged, this, [this] {
        if (m_screen->trafficEnabled() != trafficEnabled())
            setTrafficEnabled(m_screen->trafficEnabled());
    });
    m_screen->setMapHeadingUp(mapHeadingUp());
    // Car Settings screen: day/night theme and driving side, saved as changed.
    m_screen->setThemeMode(m_settings.value(QStringLiteral("themeMode"), QStringLiteral("auto")).toString());
    m_screen->setRightHandDrive(m_settings.value(QStringLiteral("rightHandDrive"), false).toBool());
    connect(m_screen, &CarScreen::themeModeChanged, this, [this] {
        m_settings.setValue(QStringLiteral("themeMode"), m_screen->themeMode());
        m_settings.sync();
    });
    connect(m_screen, &CarScreen::rightHandDriveChanged, this, [this] {
        m_settings.setValue(QStringLiteral("rightHandDrive"), m_screen->rightHandDrive());
        m_settings.sync();
    });
    m_screen->setEvCapacityKwh(batteryCapacityKwh());
    // Saved places (Home/Work + recent destinations) for the car's search screen.
    m_screen->loadPlaces(m_settings.value(QStringLiteral("places")).toMap(),
                         m_settings.value(QStringLiteral("recentPlaces")).toList());
    connect(m_screen, &CarScreen::placesChanged, this, [this] {
        m_settings.setValue(QStringLiteral("places"), m_screen->places());
        m_settings.setValue(QStringLiteral("recentPlaces"), m_screen->recentPlaces());
        m_settings.sync();
    });
    connect(m_screen, &CarScreen::mapHeadingUpChanged, this, [this] {
        if (m_screen->mapHeadingUp() != mapHeadingUp())
            setMapHeadingUp(m_screen->mapHeadingUp());
    });
    // Carry settings over from before the rename so an upgrade keeps the user's
    // cert/key paths and chosen media app (the Spotify auto-launch).
    migrateLegacySettings();
    m_mode = m_settings.value(QStringLiteral("mode"), int(Wireless)).toInt();
    m_state = tr("Idle");
    refreshCertStatus();

    // Auto-start (poll for a connected car and begin projection on its own) is
    // disabled for now — projection is started from the Start button instead. The
    // machinery is kept so it can be re-enabled later.
    m_autoDetect.setInterval(3000);
    connect(&m_autoDetect, &QTimer::timeout, this, &Controller::checkAutoStart);
}

Controller::~Controller()
{
    stop();
}

// Copy any settings from the pre-rename store ("sailfishauto") that the new
// store ("carfish") does not already have. Without this an upgrade silently
// drops the user's configured cert/key paths and media app (so Spotify no
// longer auto-launches and the car cert check fails). Runs once: after the
// first copy the new store has the keys, so legacy values are never re-applied
// over later edits.
void Controller::migrateLegacySettings()
{
    QSettings legacy(QStringLiteral("sailfishauto"), QStringLiteral("sailfishauto"));
    const QStringList keys = legacy.allKeys();
    int copied = 0;
    for (const QString &k : keys) {
        if (!m_settings.contains(k)) {
            m_settings.setValue(k, legacy.value(k));
            ++copied;
        }
    }
    if (copied) {
        m_settings.sync();
        appendLog(tr("migrated %1 setting(s) from the previous version").arg(copied));
    }
}

QString Controller::certPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return m_settings.value(QStringLiteral("certPath"), dir + QStringLiteral("/phone-cert.pem")).toString();
}

void Controller::setCertPath(const QString &path)
{
    if (path == certPath())
        return;
    m_settings.setValue(QStringLiteral("certPath"), path);
    emit certPathChanged();
    refreshCertStatus();
}

QString Controller::keyPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return m_settings.value(QStringLiteral("keyPath"), dir + QStringLiteral("/phone-key.pem")).toString();
}

void Controller::setKeyPath(const QString &path)
{
    if (path == keyPath())
        return;
    m_settings.setValue(QStringLiteral("keyPath"), path);
    emit keyPathChanged();
    refreshCertStatus();
}

void Controller::refreshCertStatus()
{
    const QString s = util::inspectCredentials(certPath(), keyPath()).summary(certPath(), keyPath());
    if (s == m_certStatus)
        return;
    m_certStatus = s;
    emit certStatusChanged();
}

QVariantList Controller::apps() const
{
    return m_apps->toVariantList();
}

QString Controller::navApp() const
{
    return m_settings.value(QStringLiteral("navApp")).toString();
}

void Controller::setNavApp(const QString &id)
{
    if (id == navApp())
        return;
    m_settings.setValue(QStringLiteral("navApp"), id);
    emit navAppChanged();
}

QString Controller::mediaApp() const
{
    return m_settings.value(QStringLiteral("mediaApp")).toString();
}

void Controller::setMediaApp(const QString &id)
{
    if (id == mediaApp())
        return;
    m_settings.setValue(QStringLiteral("mediaApp"), id);
    emit mediaAppChanged();
}

bool Controller::autoStart() const
{
    return m_settings.value(QStringLiteral("autoStart"), false).toBool();
}

QString Controller::trafficKey() const
{
    return m_settings.value(QStringLiteral("trafficKey")).toString();
}

void Controller::setTrafficKey(const QString &key)
{
    if (key == trafficKey())
        return;
    m_settings.setValue(QStringLiteral("trafficKey"), key);
    if (m_screen)
        m_screen->setTrafficKey(key);
    emit trafficKeyChanged();
}

bool Controller::trafficEnabled() const
{
    return m_settings.value(QStringLiteral("trafficEnabled"), true).toBool();
}

void Controller::setTrafficEnabled(bool on)
{
    if (on == trafficEnabled())
        return;
    m_settings.setValue(QStringLiteral("trafficEnabled"), on);
    m_settings.sync(); // write now: the app is often killed rather than quit cleanly
    if (m_screen)
        m_screen->setTrafficEnabled(on);
    emit trafficEnabledChanged();
}

double Controller::batteryCapacityKwh() const
{
    return m_settings.value(QStringLiteral("batteryCapacityKwh"), 0.0).toDouble();
}

void Controller::setBatteryCapacityKwh(double kwh)
{
    kwh = qBound(0.0, kwh, 500.0);
    if (qFuzzyCompare(kwh + 1.0, batteryCapacityKwh() + 1.0))
        return;
    m_settings.setValue(QStringLiteral("batteryCapacityKwh"), kwh);
    m_settings.sync();
    if (m_screen)
        m_screen->setEvCapacityKwh(kwh);
    emit batteryCapacityKwhChanged();
}

bool Controller::mapHeadingUp() const
{
    return m_settings.value(QStringLiteral("mapHeadingUp"), false).toBool();
}

void Controller::setMapHeadingUp(bool on)
{
    if (on == mapHeadingUp())
        return;
    m_settings.setValue(QStringLiteral("mapHeadingUp"), on);
    m_settings.sync();
    if (m_screen)
        m_screen->setMapHeadingUp(on);
}

void Controller::setAutoStart(bool on)
{
    if (on == autoStart())
        return;
    m_settings.setValue(QStringLiteral("autoStart"), on);
    emit autoStartChanged();
    m_carPresent = false;
    if (on)
        m_autoDetect.start();
    else
        m_autoDetect.stop();
}

// Poll for a connected car; auto-start projection on the connect transition.
// Edge-triggered (only when the car first appears) so a failed attempt or a
// still-plugged car doesn't loop. Works for any car -- nothing is hardcoded.
static QString readSysLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLatin1(f.readLine()).trimmed();
}

// A car over USB means the Android Open Accessory gadget is actively configured
// by the head unit -- NOT merely that /dev/usb_accessory exists, since that node
// lingers after a session even while unplugged (which made auto-start loop).
bool Controller::usbCarConnected() const
{
    if (!QFile::exists(QStringLiteral("/dev/usb_accessory")))
        return false;
    const QString state = readSysLine(QStringLiteral("/sys/class/android_usb/android0/state"));
    const QString funcs = readSysLine(QStringLiteral("/sys/class/android_usb/android0/functions"));
    if (!state.isEmpty()) // host enumerated us AND we're in accessory mode
        return state == QLatin1String("CONFIGURED") && funcs.contains(QStringLiteral("accessory"));
    // Fallback (no android_usb sysfs): require a UDC configured by a host.
    const QStringList udcs = QDir(QStringLiteral("/sys/class/udc"))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &u : udcs)
        if (readSysLine(QStringLiteral("/sys/class/udc/%1/state").arg(u)) == QLatin1String("configured"))
            return true;
    return false;
}

void Controller::checkAutoStart()
{
    if (!autoStart())
        return;
    const bool usb = usbCarConnected();
    const bool bt = !usb && aaHeadUnitConnected();
    const bool present = usb || bt;
    if (present && !m_carPresent && !m_running) {
        appendLog(tr("car detected (%1) — auto-starting").arg(usb ? QStringLiteral("USB")
                                                                   : QStringLiteral("Bluetooth")));
        start(usb ? Usb : Wireless);
    }
    m_carPresent = present;
}

bool Controller::aaHeadUnitConnected() const
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"), QStringLiteral("/"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;

    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    bool found = false;
    arg.beginMap(); // a{oa{sa{sv}}}
    while (!arg.atEnd() && !found) {
        arg.beginMapEntry();
        QDBusObjectPath path;
        QMap<QString, QVariantMap> ifaces;
        arg >> path >> ifaces;
        arg.endMapEntry();
        const QVariantMap dev = ifaces.value(QStringLiteral("org.bluez.Device1"));
        if (dev.isEmpty() || !dev.value(QStringLiteral("Connected")).toBool())
            continue;
        const QStringList uuids = dev.value(QStringLiteral("UUIDs")).toStringList();
        for (const QString &u : uuids) {
            if (u.compare(BluezProfile::kAndroidAutoUuid, Qt::CaseInsensitive) == 0
                    || u.compare(BluezProfile::kAndroidAutoUuidReversed, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
    }
    arg.endMap();
    return found;
}

void Controller::refreshApps()
{
    m_apps->refresh();
    emit appsChanged();
}

QString Controller::appName(const QString &id) const
{
    return m_apps->name(id);
}

bool Controller::launchApp(const QString &id)
{
    const bool ok = m_apps->launch(id);
    appendLog(ok ? tr("launched %1").arg(appName(id)) : tr("could not launch %1").arg(id));
    return ok;
}

// Start the chosen navigation/media apps as background backends when a session
// begins, so the car UI (MPRIS now-playing, map) has live data behind it.
void Controller::launchBackendApps()
{
    const QString nav = navApp();
    const QString media = mediaApp();
    if (!media.isEmpty())
        launchApp(media);
    if (!nav.isEmpty() && nav != media)
        launchApp(nav);
}

void Controller::start(int mode)
{
    if (m_running)
        return;
    m_mode = mode;
    m_settings.setValue(QStringLiteral("mode"), mode);
    emit modeChanged();

    // The cert/key live in phone storage, supplied by the user (never bundled).
    // Fail early with a readable reason instead of a raw OpenSSL error later.
    refreshCertStatus();
    const util::CredentialInfo cred = util::inspectCredentials(certPath(), keyPath());
    if (!cred.usable()) {
        appendLog(cred.summary(certPath(), keyPath()));
        setState(tr("Cannot start — check the certificate"));
        return;
    }

    m_running = true;
    emit runningChanged();

    // Bring up the chosen navigation/media apps as backends for the car UI.
    launchBackendApps();

    switch (mode) {
    case Wireless: {
        m_wireless = new WirelessSetup(this);
        connect(m_wireless, &WirelessSetup::status, this, [this](const QString &s) {
            setState(s);
            appendLog(s);
        });
        connect(m_wireless, &WirelessSetup::log, this, &Controller::appendLog);
        connect(m_wireless, &WirelessSetup::failed, this, [this](const QString &r) {
            appendLog(tr("Wireless setup failed: %1").arg(r));
            setState(tr("Waiting for head unit"));
        });
        connect(m_wireless, &WirelessSetup::readyForProjection, this, [this](const QString &host, quint16 port) {
            startSession(TcpTransport::client(host, port, this));
        });
        QString error;
        if (!m_wireless->start(&error)) {
            endSession(error);
            return;
        }
        setState(tr("Waiting for paired head unit over Bluetooth"));
        break;
    }
    case Usb: {
        // Wired AA. The car is the USB host; the privileged helper waits for
        // the car's AOA START and rebuilds the gadget as a FunctionFS accessory
        // (18d1:2d00), then hands us the FunctionFS mount dir to drive AAP over.
        // The kernel's /dev/usb_accessory chardev does NOT re-enumerate on these
        // devices, so a direct read of it gets no AAP bytes — hence the switch.
        setState(tr("Waiting for the car (USB)"));
        m_usbHelper = new UsbHelperClient(this);
        connect(m_usbHelper, &UsbHelperClient::log, this, &Controller::appendLog);
        m_usbSwitchTimer.setSingleShot(true);
        m_usbSwitchTimer.setInterval(12000);
        m_usbSwitchTimer.disconnect(this);
        connect(&m_usbSwitchTimer, &QTimer::timeout, this, [this] {
            appendLog(tr("USB: the switch to accessory mode did not finish within 12 s "
                         "(phone USB stuck?). Unplug and replug the cable; details: "
                         "devel-su journalctl -b -u carfish-helper"));
            setState(tr("USB switch stuck: replug the cable"));
        });
        connect(m_usbHelper, &UsbHelperClient::startRequested, this, [this](const QString &model) {
            appendLog(tr("USB: car started Android Auto (%1)").arg(model));
            setState(tr("Switching USB to accessory mode"));
            m_usbSwitchTimer.start();
        });
        connect(m_usbHelper, &UsbHelperClient::failed, this, [this](const QString &r) {
            m_usbSwitchTimer.stop();
            appendLog(tr("USB bring-up failed: %1").arg(r));
            setState(tr("Waiting for the car (USB)"));
        });
        connect(m_usbHelper, &UsbHelperClient::ready, this, [this](const QString &ffsDir) {
            m_usbSwitchTimer.stop();
            appendLog(tr("USB accessory gadget ready (%1)").arg(ffsDir));
            startSession(new UsbFfsTransport(ffsDir, this));
        });
        m_usbHelper->start();
        break;
    }
    case TcpServer:
        setState(tr("Listening on port %1").arg(kHeadUnitServerPort));
        startSession(TcpTransport::server(kHeadUnitServerPort, this));
        break;
    }
}

void Controller::startSession(Transport *transport)
{
    if (m_session)
        m_session->deleteLater();
    if (m_transport)
        m_transport->deleteLater();
    m_transport = transport;

    aap::Session::Config cfg;
    cfg.certPath = certPath();
    cfg.keyPath = keyPath();
    cfg.carQml = m_carQml;
    if (m_audioDevice == QLatin1String("none"))
        cfg.mediaAudio = false;
    else if (!m_audioDevice.isEmpty())
        cfg.audioDevice = m_audioDevice;
    m_session = new aap::Session(transport, m_screen, cfg, this);
    connect(m_session.data(), &aap::Session::stateChanged, this, &Controller::setState);
    connect(m_session.data(), &aap::Session::logMessage, this, &Controller::appendLog);
    // Steering-wheel media buttons drive the chosen media app over MPRIS.
    connect(m_session.data(), &aap::Session::mediaKey, m_mpris, &MprisController::handleMediaKey);

    // Car fuel/energy level (EV battery + range) -> the map UI, for a
    // battery-at-arrival estimate alongside the ETA.
    connect(m_session.data(), &aap::Session::energyStatus, m_screen,
            [this](int pct, int rangeKm, bool electric, bool low) {
                m_screen->setEnergy(pct, rangeKm, electric, low);
            });

    // Telephony: report call state to the head unit so an incoming/active call
    // shows inside Android Auto (call audio stays on Bluetooth HFP), and let the
    // head unit's answer/reject (and CALL/ENDCALL wheel buttons) drive the phone.
    auto pushCall = [this]() {
        if (!m_session)
            return;
        int state = 3; // PhoneStatus INACTIVE
        if (m_phone->hasCall())
            state = m_phone->incoming() ? 4 /*INCOMING*/ : 1 /*IN_CALL*/;
        m_session->updateCall(state, m_phone->callNumber(), m_phone->callName());
    };
    connect(m_phone, &PhoneManager::callChanged, m_session.data(), pushCall);
    connect(m_session.data(), &aap::Session::phoneChannelReady, m_session.data(), pushCall);
    connect(m_session.data(), &aap::Session::phoneAnswer, m_phone, &PhoneManager::answer);
    connect(m_session.data(), &aap::Session::phoneReject, m_phone, &PhoneManager::hangup);
    // Now playing -> the car's own screens (when it advertises the service; the
    // session caches and replays once the channel opens, so push right away).
    connect(m_mpris, &MprisController::changed, m_session.data(), [this] {
        pushMediaMetadata();
        pushMediaStatus();
    });
    connect(m_mpris, &MprisController::positionChanged, m_session.data(), [this] { pushMediaStatus(); });
    connect(m_session.data(), &aap::Session::finished, this, [this](const QString &reason) {
        appendLog(tr("Session ended: %1").arg(reason));
        if (m_mode == Wireless && m_running && m_wireless) {
            // Auto-reconnect: re-arm the wireless bring-up so a dropped car comes
            // back on its own, without a manual Stop/Start. Stays registered with
            // BlueZ throughout; the next readyForProjection replaces the session.
            setState(tr("Reconnecting to head unit"));
            m_wireless->retrySoon();
        } else {
            endSession(reason);
        }
    });
    // Re-scan MPRIS now so the now-playing pane reflects whatever is already
    // playing; later appearances are caught live via NameOwnerChanged.
    m_mpris->refresh();
    appendLog(m_mpris->hasPlayer()
                  ? tr("media backend: %1 — %2").arg(m_mpris->title(), m_mpris->artist())
                  : tr("media backend: no MPRIS player yet"));

    pushMediaMetadata();
    pushMediaStatus();

    appendLog(tr("Starting session over %1").arg(transport->describe()));
    m_session->start();
    // Look for a Bluetooth ambient-light kit once the projection has settled
    // (an LE scan during the wireless AA bring-up would compete for the radio).
    QTimer::singleShot(8000, this, [this] {
        if (m_session)
            m_leds->start();
    });
}

void Controller::stop()
{
    endSession(tr("stopped"));
}

void Controller::startPreview(const QSize &size, int fps)
{
    // Render the car UI headlessly and save frames to a PNG so the projected
    // screen can be checked on the desk (no head unit required). Handy for
    // verifying the map, now-playing pane, etc. without a car.
    QString error;
    connect(m_screen, &CarScreen::frameReady, this, &Controller::savePreviewFrame,
            Qt::UniqueConnection);
    if (!m_screen->start(m_carQml, size, fps, &error)) {
        appendLog(tr("preview failed: %1").arg(error));
        return;
    }
    m_previewFrames = 0;
    setState(tr("Preview (%1x%2)").arg(size.width()).arg(size.height()));
    appendLog(tr("preview rendering %1 -> ~/carfish-preview.png").arg(m_carQml.toString()));
    // Once the map has had time to load, run the no-input nav self-test so the
    // preview exercises geocoding + routing end to end.
    QTimer::singleShot(5000, this, [this] {
        if (m_screen) m_screen->setCarProperty(QStringLiteral("previewTest"), true);
    });
}

void Controller::savePreviewFrame(const QImage &rgba)
{
    // Give the scene a moment to load tiles/art, then snapshot roughly once a
    // second (overwriting), so the PNG reflects a settled frame.
    ++m_previewFrames;
    if (m_previewFrames < 10 || m_previewFrames % 5 != 0)
        return;
    const QString path = QDir::homePath() + QStringLiteral("/carfish-preview.png");
    if (rgba.save(path, "PNG") && m_previewFrames % 25 == 0)
        appendLog(tr("preview frame %1 saved").arg(m_previewFrames));
}

void Controller::endSession(const QString &reason)
{
    if (m_session) {
        m_session->disconnect(this);
        m_session->shutdown();
        m_session->deleteLater();
    }
    if (m_transport) {
        m_transport->stop();
        m_transport->deleteLater();
    }
    if (m_wireless) {
        m_wireless->stop();
        m_wireless->deleteLater();
        m_wireless = nullptr;
    }
    m_usbSwitchTimer.stop();
    m_leds->stop();
    if (m_usbHelper) {
        m_usbHelper->stop(); // restores usb-moded / the normal USB gadget
        m_usbHelper->deleteLater();
        m_usbHelper = nullptr;
    }
    if (m_running) {
        m_running = false;
        emit runningChanged();
    }
    setState(tr("Idle (%1)").arg(reason));
}

void Controller::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void Controller::appendLog(const QString &line)
{
    qCInfo(lcCtl).noquote() << line; // journal on device
    QFile f(kLogFile);
    if (f.open(QIODevice::Append | QIODevice::Text))
        QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate) << QLatin1Char(' ') << line << QLatin1Char('\n');
    m_log.append(QTime::currentTime().toString(QStringLiteral("hh:mm:ss ")) + line);
    while (m_log.size() > kMaxLogLines)
        m_log.removeFirst();
    emit logChanged();
}

// ------------------------------------------------------------ now playing (car)

void Controller::pushMediaStatus()
{
    if (!m_session)
        return;
    // MediaPlaybackStatus.State: STOPPED 1, PLAYING 2, PAUSED 3.
    const int state = !m_mpris->hasPlayer() ? 1 : m_mpris->playing() ? 2 : 3;
    m_session->updateMediaStatus(state, m_mpris->playerName(), int(m_mpris->position()));
}

void Controller::pushMediaMetadata()
{
    if (!m_session)
        return;
    if (m_mpris->artUrl() != m_coverUrl)
        loadCoverArt(m_mpris->artUrl()); // async for http; re-pushes when done
    m_session->updateMediaMetadata(m_mpris->title(), m_mpris->artist(), m_mpris->album(),
                                   m_coverPng, int(m_mpris->duration()));
}

void Controller::loadCoverArt(const QString &url)
{
    m_coverUrl = url;
    m_coverPng.clear();
    if (m_coverReply)
        m_coverReply->abort();
    if (url.isEmpty())
        return;
    const QUrl u = QUrl::fromUserInput(url);
    if (u.isLocalFile()) {
        setCoverArt(QImage(u.toLocalFile()));
        return;
    }
    if (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))
        return;
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    QNetworkReply *reply = m_net->get(QNetworkRequest(u));
    m_coverReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || url != m_coverUrl)
            return;
        setCoverArt(QImage::fromData(reply->readAll()));
        pushMediaMetadata();
    });
}

// The car takes a PNG of at most 256x256 (Gearhead rejects anything else).
// Shrink further if the PNG is large, to keep the message light.
void Controller::setCoverArt(const QImage &img)
{
    m_coverPng.clear();
    if (img.isNull())
        return;
    for (int side : { 256, 192, 128 }) {
        const QImage scaled = (img.width() > side || img.height() > side)
                ? img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation) : img;
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!scaled.save(&buf, "PNG"))
            return;
        m_coverPng = png;
        if (png.size() <= 64 * 1024)
            break;
    }
}
