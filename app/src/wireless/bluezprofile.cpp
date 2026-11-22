#include "bluezprofile.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcBt, "aap.bluetooth")

const QString BluezProfile::kAndroidAutoUuid = QStringLiteral("4de17a00-52cb-11e6-bdf4-0800200c9a66");
// Byte-reversed form the head unit may advertise in its SDP record; Gearhead
// treats either as "this device supports Android Auto wireless".
const QString BluezProfile::kAndroidAutoUuidReversed = QStringLiteral("669a0c20-0008-f4bd-e611-cb52007ae14d");

static const QString kBluez = QStringLiteral("org.bluez");
static const QString kProfilePath = QStringLiteral("/org/carfish/aawireless");

BluezProfile::BluezProfile(QObject *parent)
    : QObject(parent)
{
    new BluezProfileAdaptor(this);
}

BluezProfile::~BluezProfile()
{
    unregisterProfile();
}

bool BluezProfile::registerProfile(QString *error)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(kProfilePath, this)) {
        *error = QStringLiteral("cannot export profile object: ") + bus.lastError().message();
        return false;
    }

    QVariantMap opts;
    opts.insert(QStringLiteral("Name"), QStringLiteral("Android Auto Wireless"));
    // The phone is the RFCOMM client: the head unit advertises the UUID via SDP.
    opts.insert(QStringLiteral("Role"), QStringLiteral("client"));
    opts.insert(QStringLiteral("RequireAuthentication"), true);
    opts.insert(QStringLiteral("RequireAuthorization"), false);
    opts.insert(QStringLiteral("AutoConnect"), true);

    QDBusInterface mgr(kBluez, QStringLiteral("/org/bluez"), QStringLiteral("org.bluez.ProfileManager1"), bus);
    QDBusReply<void> r = mgr.call(QStringLiteral("RegisterProfile"),
                                  QVariant::fromValue(QDBusObjectPath(kProfilePath)), kAndroidAutoUuid, opts);
    if (!r.isValid()) {
        *error = QStringLiteral("RegisterProfile failed: ") + r.error().message();
        bus.unregisterObject(kProfilePath);
        return false;
    }
    m_registered = true;
    qCInfo(lcBt) << "registered AA RFCOMM profile" << kAndroidAutoUuid;
    return true;
}

void BluezProfile::unregisterProfile()
{
    closeConnection();
    if (!m_registered)
        return;
    QDBusConnection bus = QDBusConnection::systemBus();
    QDBusInterface mgr(kBluez, QStringLiteral("/org/bluez"), QStringLiteral("org.bluez.ProfileManager1"), bus);
    mgr.call(QStringLiteral("UnregisterProfile"), QVariant::fromValue(QDBusObjectPath(kProfilePath)));
    bus.unregisterObject(kProfilePath);
    m_registered = false;
}

void BluezProfile::connectDevice(const QString &devicePath)
{
    QDBusInterface dev(kBluez, devicePath, QStringLiteral("org.bluez.Device1"), QDBusConnection::systemBus());
    auto *w = new QDBusPendingCallWatcher(dev.asyncCall(QStringLiteral("ConnectProfile"), kAndroidAutoUuid), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, devicePath](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> r = *w;
        if (r.isError()) {
            const QString msg = r.error().message();
            emit log(QStringLiteral("ConnectProfile(%1): %2").arg(devicePath, msg));
            // "Already connected" / "In progress" are benign — the socket still
            // arrives via NewConnection. Anything else (br-connection-page-timeout
            // after the head unit reset its Bluetooth, connection refused/canceled)
            // means the link did NOT come up and no NewConnection will arrive, so
            // surface it as an error to drive a backoff retry instead of stalling.
            const QString low = msg.toLower();
            const bool benign = low.contains(QLatin1String("already connected"))
                    || low.contains(QLatin1String("in progress"));
            if (!benign && m_fd < 0)
                emit error(QStringLiteral("Bluetooth connect failed: %1").arg(msg));
        } else {
            emit log(QStringLiteral("ConnectProfile(%1): ok").arg(devicePath));
        }
        w->deleteLater();
    });
}

// Why a paired device clearly isn't a car, from its Bluetooth Class of Device,
// or an empty string when it may be one (car audio, hands-free, or unknown).
// Used only for the fallback when no device advertises the AA UUID, so we
// don't page the user's headphones, watch or laptop on every attempt.
static QString notACarReason(quint32 cls)
{
    if (cls == 0)
        return QString(); // class unknown: could be the car
    const quint32 major = (cls >> 8) & 0x1f;
    const quint32 minor = (cls >> 2) & 0x3f;
    switch (major) {
    case 1: return QStringLiteral("computer");
    case 2: return QStringLiteral("phone");
    case 5: return QStringLiteral("keyboard/mouse");
    case 6: return QStringLiteral("printer/camera");
    case 7: return QStringLiteral("wearable");
    case 8: return QStringLiteral("toy");
    case 9: return QStringLiteral("health device");
    case 4: // audio/video: keep hands-free (2) and car audio (8)
        switch (minor) {
        case 1: return QStringLiteral("headset");
        case 5: return QStringLiteral("loudspeaker");
        case 6: return QStringLiteral("headphones");
        case 7: return QStringLiteral("portable audio");
        case 12: case 13: case 14: case 15: case 16: return QStringLiteral("video device");
        default: return QString();
        }
    default:
        return QString();
    }
}

void BluezProfile::connectKnownHeadUnits()
{
    if (m_fd >= 0) {
        emit log(QStringLiteral("RFCOMM already connected; not initiating another"));
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        kBluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        emit log(QStringLiteral("cannot list BlueZ devices: %1").arg(reply.errorMessage()));
        return;
    }

    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    QStringList aaDevices;   // advertise the AA UUID
    QStringList allPaired;   // paired fallback (devices that may be a car)
    QStringList skipped;     // paired, but clearly not a car
    arg.beginMap(); // a{oa{sa{sv}}}
    while (!arg.atEnd()) {
        arg.beginMapEntry();
        QDBusObjectPath path;
        QMap<QString, QVariantMap> ifaces;
        arg >> path >> ifaces;
        arg.endMapEntry();

        const QVariantMap dev = ifaces.value(QStringLiteral("org.bluez.Device1"));
        if (dev.isEmpty() || !dev.value(QStringLiteral("Paired")).toBool())
            continue;
        const QString name = dev.value(QStringLiteral("Alias"), dev.value(QStringLiteral("Name"))).toString();
        const QString why = notACarReason(dev.value(QStringLiteral("Class")).toUInt());
        if (why.isEmpty())
            allPaired << path.path();
        else
            skipped << QStringLiteral("%1 (%2)").arg(name, why);
        const QStringList uuids = dev.value(QStringLiteral("UUIDs")).toStringList();
        for (const QString &u : uuids) {
            if (u.compare(kAndroidAutoUuid, Qt::CaseInsensitive) == 0
                    || u.compare(kAndroidAutoUuidReversed, Qt::CaseInsensitive) == 0) {
                aaDevices << path.path();
                break;
            }
        }
    }
    arg.endMap();

    const QStringList &targets = aaDevices.isEmpty() ? allPaired : aaDevices;
    if (aaDevices.isEmpty()) {
        emit log(QStringLiteral("no paired device advertises the AA UUID; trying %1 paired device(s) that may be a car")
                     .arg(allPaired.size()));
        const QString sk = skipped.join(QStringLiteral(", "));
        if (!sk.isEmpty() && sk != m_lastSkipped)
            emit log(QStringLiteral("not trying: %1").arg(sk));
        m_lastSkipped = sk;
        if (allPaired.isEmpty())
            emit log(QStringLiteral("no paired car found; pair the car over Bluetooth first "
                                    "(and check it supports wireless Android Auto)"));
    }
    else
        emit log(QStringLiteral("found %1 AA-capable paired device(s)").arg(aaDevices.size()));

    for (const QString &path : targets) {
        emit log(QStringLiteral("initiating AA RFCOMM to %1").arg(path));
        connectDevice(path);
    }
}

void BluezProfile::onNewConnection(const QDBusObjectPath &device, const QDBusUnixFileDescriptor &fd, const QVariantMap &)
{
    if (m_fd >= 0) {
        // Keep the existing held link; a genuine phone keeps one RFCOMM channel
        // for the whole session. The incoming fd is owned by the
        // QDBusUnixFileDescriptor and closed when this call returns.
        emit log(QStringLiteral("ignoring duplicate RFCOMM connection from %1 (link already up)").arg(device.path()));
        return;
    }
    qCInfo(lcBt) << "RFCOMM connection from" << device.path();

    // QDBusUnixFileDescriptor owns its fd; take a private duplicate.
    m_fd = ::dup(fd.fileDescriptor());
    if (m_fd < 0) {
        emit error(QStringLiteral("cannot adopt RFCOMM socket"));
        return;
    }
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    m_device = device.path();
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] {
        char buf[1024];
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n > 0) {
            emit dataReceived(QByteArray(buf, int(n)));
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            qCInfo(lcBt) << "RFCOMM closed";
            closeConnection();
            emit connectionClosed();
        }
    });
    emit connectionReady(m_device);
}

void BluezProfile::onRequestDisconnection(const QDBusObjectPath &)
{
    closeConnection();
    emit connectionClosed();
}

void BluezProfile::write(const QByteArray &data)
{
    // RFCOMM messages are tiny; a short blocking retry loop is fine.
    int off = 0;
    while (m_fd >= 0 && off < data.size()) {
        const ssize_t n = ::write(m_fd, data.constData() + off, size_t(data.size() - off));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            emit error(QStringLiteral("RFCOMM write failed"));
            return;
        }
        off += int(n);
    }
}

void BluezProfile::closeConnection()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

BluezProfileAdaptor::BluezProfileAdaptor(BluezProfile *parent)
    : QDBusAbstractAdaptor(parent)
    , m_profile(parent)
{
}

void BluezProfileAdaptor::Release()
{
    qCInfo(lcBt) << "profile released by BlueZ";
    m_profile->m_registered = false;
}

void BluezProfileAdaptor::NewConnection(const QDBusObjectPath &device, const QDBusUnixFileDescriptor &fd, const QVariantMap &props)
{
    m_profile->onNewConnection(device, fd, props);
}

void BluezProfileAdaptor::RequestDisconnection(const QDBusObjectPath &device)
{
    m_profile->onRequestDisconnection(device);
}
