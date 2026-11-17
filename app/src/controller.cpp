#include "controller.h"

#include "aap/session.h"
#include "apps/appregistry.h"
#include "apps/mpriscontroller.h"
#include "apps/phonemanager.h"
#include "media/carscreen.h"
#include "transport/tcptransport.h"
#include "transport/usbaccessorytransport.h"
#include "util/credentials.h"
#include "util/uanetwork.h"
#include "wireless/bluezprofile.h"
#include "wireless/wirelesssetup.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLoggingCategory>
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
    , m_screen(new CarScreen(engine, this))
{
    m_screen->setMediaSource(m_mpris);
    m_screen->setAppRegistry(m_apps);
    m_screen->setPhone(m_phone);
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
    case Usb:
        setState(tr("Waiting for USB accessory"));
        startSession(new UsbAccessoryTransport(QStringLiteral("/dev/usb_accessory"), this));
        break;
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
    m_session = new aap::Session(transport, m_screen, cfg, this);
    connect(m_session.data(), &aap::Session::stateChanged, this, &Controller::setState);
    connect(m_session.data(), &aap::Session::logMessage, this, &Controller::appendLog);
    // Steering-wheel media buttons drive the chosen media app over MPRIS.
    connect(m_session.data(), &aap::Session::mediaKey, m_mpris, &MprisController::handleMediaKey);
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

    appendLog(tr("Starting session over %1").arg(transport->describe()));
    m_session->start();
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
