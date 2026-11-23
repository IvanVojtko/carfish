#include "ledcontroller.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QDateTime>

static const char kBluez[] = "org.bluez";
static const char kFfe1[] = "0000ffe1-0000-1000-8000-00805f9b34fb";
static const int kScanTicks = 8;      // x 2 s = how long we look for a kit
static const int kWriteGapMs = 50;    // at most one frame per 50 ms (doc §3.3)

LedController::LedController(QObject *parent)
    : QObject(parent)
    , m_settings(QStringLiteral("carfish"), QStringLiteral("carfish"))
{
    m_on = m_settings.value(QStringLiteral("leds/on"), true).toBool();
    m_color = QColor(m_settings.value(QStringLiteral("leds/color"), QStringLiteral("#4c8dff")).toString());
    if (!m_color.isValid())
        m_color = QColor(0x4c, 0x8d, 0xff);
    m_brightness = qBound(1, m_settings.value(QStringLiteral("leds/brightness"), 80).toInt(), 100);

    m_scanTimer.setInterval(2000);
    connect(&m_scanTimer, &QTimer::timeout, this, &LedController::scanTick);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &LedController::connectDevice);
    m_pump.setSingleShot(true);
    m_pump.setInterval(kWriteGapMs);
    connect(&m_pump, &QTimer::timeout, this, &LedController::pump);
}

LedController::~LedController()
{
    stop();
}

// ------------------------------------------------------------------ frames

LedController::Kind LedController::kindFor(const QString &name)
{
    if (name.startsWith(QLatin1String("LEDCAR-00-")))
        return Kind::Ble7E;
    if (name.startsWith(QLatin1String("LEDCAR-01-")))
        return Kind::Car01;
    if (name.startsWith(QLatin1String("LEDCAR-02-")))
        return Kind::Car02;
    if (name.startsWith(QLatin1String("LEDBLE-02-")))
        return Kind::Ble02;
    if (name.startsWith(QLatin1String("LEDBLE-03-")))
        return Kind::Ble03;
    if (name.startsWith(QLatin1String("LEDBLE-")))
        return Kind::Ble7E; // the app treats every other LEDBLE as LEDBLE-01
    return Kind::None;
}

// HH b1..b7 TT, trailer TT = ((HH & 0x0F) << 4) | 0x0F (doc §0).
QByteArray LedController::frame(quint8 header, std::initializer_list<int> payload7)
{
    QByteArray f(9, char(0xFF));
    f[0] = char(header);
    int i = 1;
    for (int v : payload7)
        if (i <= 7)
            f[i++] = char(v & 0xFF);
    f[8] = char(((header & 0x0F) << 4) | 0x0F);
    return f;
}

// 2A 02 <password A1 23 45 67> <(weekday<<5)|hour> <minute> AF (doc §4).
QByteArray LedController::handshake()
{
    const QDateTime now = QDateTime::currentDateTime();
    const int wh = (now.date().dayOfWeek() << 5) | now.time().hour();
    return frame(0x2A, {0x02, 0xA1, 0x23, 0x45, 0x67, wh, now.time().minute()});
}

QList<QByteArray> LedController::powerFrames(Kind k, bool on)
{
    const int o = on ? 1 : 0;
    switch (k) {
    case Kind::Ble02: return {frame(0x7E, {0xFF, 0x04, o, 0xFF, 0xFF, 0xFF, 0x00})};
    case Kind::Ble03: return {frame(0x7E, {0x04, o, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})};
    // CAR-01 has an RGB output (7E family) and a pixel output ("LED" = both in
    // sync, 7B family); send both so either wiring follows (doc §5.2).
    case Kind::Car01: return {frame(0x7E, {0xFF, 0x04, o, 0xFF, 0xFF, 0xFF, 0xFF}),
                              frame(0x7B, {0xFF, 0x04, on ? 0x07 : 0x06, 0xFF, 0xFF, 0xFF, 0xFF})};
    case Kind::Car02: return {frame(0x7B, {0x04, o, 0xFF, 0xFF, 0xFF, 0xFF, 0x00})};
    case Kind::Ble7E: return {frame(0x7E, {0xFF, 0x04, o, 0xFF, 0xFF, 0xFF, 0xFF})};
    case Kind::None: break;
    }
    return {};
}

QList<QByteArray> LedController::colorFrames(Kind k, const QColor &c)
{
    const int r = c.red(), g = c.green(), b = c.blue();
    switch (k) {
    case Kind::Ble02: return {frame(0x7E, {0xFF, 0x05, 0x03, r, g, b, 0x00})};
    case Kind::Ble03: return {frame(0x7E, {0x05, r, g, b, 0x00, 0xFF, 0xFF})};
    case Kind::Car01: return {frame(0x7E, {0xFF, 0x05, 0x03, r, g, b, 0xFF}),
                              frame(0x7B, {0x01, 0x07, r, g, b, 0x00, 0xFF})};
    case Kind::Car02: return {frame(0x7B, {0x07, r, g, b, 0x00, 0xFF, 0x00})};
    case Kind::Ble7E: return {frame(0x7E, {0xFF, 0x05, 0x03, r, g, b, 0xFF})};
    case Kind::None: break;
    }
    return {};
}

QList<QByteArray> LedController::brightnessFrames(Kind k, int percent)
{
    const int l = qBound(1, percent, 100); // the kit's app never sends 0
    switch (k) {
    case Kind::Ble02: return {frame(0x7E, {0xFF, 0x01, l, 0x00, 0xFF, 0xFF, 0x00})};
    case Kind::Ble03: return {frame(0x7E, {0x01, l, 0x00, 0xFF, 0xFF, 0xFF, 0xFF})};
    case Kind::Car01: return {frame(0x7E, {0xFF, 0x01, l, 0x00, 0xFF, 0xFF, 0xFF}),
                              frame(0x7B, {0xFF, 0x01, l * 32 / 100, l, 0x02, 0xFF, 0xFF})};
    case Kind::Car02: return {frame(0x7B, {0x01, l, 0x00, 0xFF, 0xFF, 0xFF, 0x00})};
    case Kind::Ble7E: return {frame(0x7E, {0xFF, 0x01, l, 0x00, 0xFF, 0xFF, 0xFF})};
    case Kind::None: break;
    }
    return {};
}

// ------------------------------------------------------------- user actions

void LedController::save()
{
    m_settings.setValue(QStringLiteral("leds/on"), m_on);
    m_settings.setValue(QStringLiteral("leds/color"), m_color.name());
    m_settings.setValue(QStringLiteral("leds/brightness"), m_brightness);
}

void LedController::setOn(bool on)
{
    m_on = on;
    save();
    emit lightChanged();
    queue(QStringLiteral("power"), powerFrames(m_kind, on));
}

void LedController::setColor(const QColor &color)
{
    if (!color.isValid())
        return;
    m_color = color;
    if (!m_on) {
        m_on = true;
        queue(QStringLiteral("power"), powerFrames(m_kind, true));
    }
    save();
    emit lightChanged();
    queue(QStringLiteral("color"), colorFrames(m_kind, color));
}

void LedController::setBrightness(int percent)
{
    m_brightness = qBound(1, percent, 100);
    save();
    emit lightChanged();
    queue(QStringLiteral("brightness"), brightnessFrames(m_kind, m_brightness));
}

// ------------------------------------------------------------ write queue
// Latest value per key wins (a dragged slider sends its final value, not a
// backlog); one write in flight, >= 50 ms apart. Frames queued before the
// characteristic is ready go out once connected.

void LedController::queue(const QString &key, const QList<QByteArray> &frames)
{
    if (frames.isEmpty())
        return;
    if (!m_pending.contains(key))
        m_order << key;
    m_pending[key] = frames;
    if (!m_writing && !m_pump.isActive() && !m_char.isEmpty())
        pump();
}

void LedController::pump()
{
    if (m_char.isEmpty() || m_writing || m_order.isEmpty())
        return;
    const QString key = m_order.first();
    QList<QByteArray> &frames = m_pending[key];
    const QByteArray f = frames.takeFirst();
    if (frames.isEmpty()) {
        m_pending.remove(key);
        m_order.removeFirst();
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_char,
                                                      QStringLiteral("org.bluez.GattCharacteristic1"),
                                                      QStringLiteral("WriteValue"));
    QVariantMap opts;
    opts[QStringLiteral("type")] = m_writeCommand ? QStringLiteral("command") : QStringLiteral("request");
    msg << f << opts;
    m_writing = true;
    const int gen = m_gen;
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 5000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, gen, f] {
        w->deleteLater();
        if (gen != m_gen)
            return;
        m_writing = false;
        const QDBusMessage r = w->reply();
        if (r.type() == QDBusMessage::ErrorMessage) {
            emit log(QStringLiteral("lights: write %1 failed: %2")
                         .arg(QString::fromLatin1(f.toHex()), r.errorMessage()));
            dropConnection(QStringLiteral("write failed"));
            return;
        }
        if (!m_order.isEmpty())
            m_pump.start();
    });
}

// --------------------------------------------------------- BlueZ plumbing

void LedController::listObjects(std::function<void(const Objects &)> done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(kBluez), QStringLiteral("/"),
                                                      QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                      QStringLiteral("GetManagedObjects"));
    const int gen = m_gen;
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 5000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, gen, done] {
        w->deleteLater();
        if (gen != m_gen)
            return;
        const QDBusMessage r = w->reply();
        Objects objs;
        if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
            const QDBusArgument arg = r.arguments().first().value<QDBusArgument>();
            arg.beginMap(); // a{oa{sa{sv}}}
            while (!arg.atEnd()) {
                arg.beginMapEntry();
                QDBusObjectPath path;
                QMap<QString, QVariantMap> ifaces;
                arg >> path >> ifaces;
                arg.endMapEntry();
                objs.insert(path.path(), ifaces);
            }
            arg.endMap();
        }
        done(objs);
    });
}

void LedController::setState(const QString &s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

void LedController::start()
{
    if (m_running)
        return;
    m_running = true;
    m_scanTicks = 0;
    m_connectTries = 0;
    setState(QStringLiteral("searching"));
    // Find the adapter, then look among known devices and discover LE ones.
    listObjects([this](const Objects &objs) {
        for (auto it = objs.constBegin(); it != objs.constEnd(); ++it) {
            const QVariantMap a = it.value().value(QStringLiteral("org.bluez.Adapter1"));
            if (!a.isEmpty() && a.value(QStringLiteral("Powered")).toBool()) {
                m_adapter = it.key();
                break;
            }
        }
        if (m_adapter.isEmpty()) {
            emit log(QStringLiteral("lights: no powered Bluetooth adapter"));
            setState(QString());
            return;
        }
        QVariantMap filter;
        filter[QStringLiteral("Transport")] = QStringLiteral("le");
        QDBusMessage f = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_adapter,
                                                        QStringLiteral("org.bluez.Adapter1"),
                                                        QStringLiteral("SetDiscoveryFilter"));
        f << filter;
        QDBusConnection::systemBus().asyncCall(f);
        QDBusMessage d = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_adapter,
                                                        QStringLiteral("org.bluez.Adapter1"),
                                                        QStringLiteral("StartDiscovery"));
        QDBusConnection::systemBus().asyncCall(d);
        m_discovering = true;
        scanTick();
        m_scanTimer.start();
    });
}

// Look for a kit: a device whose name has a known prefix and that is in range
// now (RSSI from this discovery) or already connected. A cached entry without
// either may be miles away, so it is not tried.
void LedController::scanTick()
{
    if (!m_running || found())
        return;
    listObjects([this](const Objects &objs) {
        if (!m_running || found())
            return;
        QString best, bestName;
        int bestRssi = -1000;
        for (auto it = objs.constBegin(); it != objs.constEnd(); ++it) {
            const QVariantMap dev = it.value().value(QStringLiteral("org.bluez.Device1"));
            if (dev.isEmpty())
                continue;
            const QString name = dev.value(QStringLiteral("Name"), dev.value(QStringLiteral("Alias"))).toString();
            if (kindFor(name) == Kind::None)
                continue;
            const bool connected = dev.value(QStringLiteral("Connected")).toBool();
            if (!connected && !dev.contains(QStringLiteral("RSSI")))
                continue;
            const int rssi = connected ? 0 : dev.value(QStringLiteral("RSSI")).toInt();
            if (rssi > bestRssi) {
                best = it.key();
                bestName = name;
                bestRssi = rssi;
            }
        }
        if (!best.isEmpty()) {
            m_device = best;
            m_name = bestName;
            m_kind = kindFor(bestName);
            emit log(QStringLiteral("lights: found %1 (%2)").arg(m_name, m_device));
            emit deviceChanged();
            m_scanTimer.stop();
            stopDiscovery(); // discovery while connecting slows the link down
            QDBusConnection::systemBus().connect(QLatin1String(kBluez), m_device,
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                this, SLOT(onDeviceProps(QString, QVariantMap, QStringList)));
            connectDevice();
            return;
        }
        if (++m_scanTicks >= kScanTicks) {
            m_scanTimer.stop();
            stopDiscovery();
            setState(QString());
            emit log(QStringLiteral("lights: no LED kit (LEDCAR-/LEDBLE-) nearby"));
        }
    });
}

void LedController::stopDiscovery()
{
    if (!m_discovering || m_adapter.isEmpty())
        return;
    m_discovering = false;
    QDBusMessage d = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_adapter,
                                                    QStringLiteral("org.bluez.Adapter1"),
                                                    QStringLiteral("StopDiscovery"));
    QDBusConnection::systemBus().asyncCall(d);
}

void LedController::connectDevice()
{
    if (!m_running || m_device.isEmpty())
        return;
    setState(QStringLiteral("connecting"));
    ++m_connectTries;
    QDBusMessage c = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_device,
                                                    QStringLiteral("org.bluez.Device1"),
                                                    QStringLiteral("Connect"));
    const int gen = m_gen;
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(c, 20000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, gen] {
        w->deleteLater();
        if (gen != m_gen)
            return;
        const QDBusMessage r = w->reply();
        if (r.type() == QDBusMessage::ErrorMessage
                && !r.errorName().endsWith(QLatin1String("AlreadyConnected"))) {
            emit log(QStringLiteral("lights: connect to %1 failed: %2").arg(m_name, r.errorMessage()));
            setState(QStringLiteral("disconnected"));
            if (m_connectTries < 5)
                m_reconnect.start(5000 * m_connectTries);
            return;
        }
        findCharacteristic(0);
    });
}

// After Connect, BlueZ resolves the GATT services; find FFE1 under the device.
void LedController::findCharacteristic(int attempt)
{
    listObjects([this, attempt](const Objects &objs) {
        if (!m_running)
            return;
        for (auto it = objs.constBegin(); it != objs.constEnd(); ++it) {
            if (!it.key().startsWith(m_device + QLatin1Char('/')))
                continue;
            const QVariantMap ch = it.value().value(QStringLiteral("org.bluez.GattCharacteristic1"));
            if (ch.value(QStringLiteral("UUID")).toString().compare(QLatin1String(kFfe1), Qt::CaseInsensitive) != 0)
                continue;
            m_char = it.key();
            const QStringList flags = ch.value(QStringLiteral("Flags")).toStringList();
            m_writeCommand = flags.contains(QStringLiteral("write-without-response"));
            emit log(QStringLiteral("lights: connected to %1 (FFE1 %2)").arg(m_name, flags.join(QLatin1Char(','))));
            m_connectTries = 0;
            // Notifications only matter for the password reply (2A 05 xx).
            QDBusConnection::systemBus().connect(QLatin1String(kBluez), m_char,
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                this, SLOT(onCharProps(QString, QVariantMap, QStringList)));
            QDBusMessage n = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_char,
                                                            QStringLiteral("org.bluez.GattCharacteristic1"),
                                                            QStringLiteral("StartNotify"));
            QDBusConnection::systemBus().asyncCall(n);
            setState(QStringLiteral("connected"));
            // The kit's app sends password + clock 300 ms after the link is up,
            // ahead of anything else (doc §3.2).
            const int gen = m_gen;
            QTimer::singleShot(300, this, [this, gen] {
                if (gen != m_gen || m_char.isEmpty())
                    return;
                m_order.prepend(QStringLiteral("handshake"));
                m_pending[QStringLiteral("handshake")] = {handshake()};
                pump();
            });
            return;
        }
        if (attempt < 20) {
            const int gen = m_gen;
            QTimer::singleShot(500, this, [this, attempt, gen] {
                if (gen == m_gen)
                    findCharacteristic(attempt + 1);
            });
        } else {
            emit log(QStringLiteral("lights: %1 has no FFE1 characteristic; not a supported kit").arg(m_name));
            dropConnection(QStringLiteral("no FFE1"));
        }
    });
}

void LedController::dropConnection(const QString &why)
{
    if (m_char.isEmpty() && m_state != QLatin1String("connected") && m_state != QLatin1String("connecting"))
        return;
    m_char.clear();
    m_writing = false;
    setState(QStringLiteral("disconnected"));
    emit log(QStringLiteral("lights: %1 disconnected (%2)").arg(m_name, why));
    if (m_running && m_connectTries < 5)
        m_reconnect.start(5000);
}

void LedController::onDeviceProps(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (iface == QLatin1String("org.bluez.Device1") && changed.contains(QStringLiteral("Connected"))
            && !changed.value(QStringLiteral("Connected")).toBool())
        dropConnection(QStringLiteral("link lost"));
}

void LedController::onCharProps(const QString &iface, const QVariantMap &changed, const QStringList &)
{
    if (iface != QLatin1String("org.bluez.GattCharacteristic1") || !changed.contains(QStringLiteral("Value")))
        return;
    const QByteArray v = changed.value(QStringLiteral("Value")).toByteArray();
    if (v.size() >= 3 && quint8(v[0]) == 0x2A && quint8(v[1]) == 0x05) {
        if (quint8(v[2]) == 0x01)
            emit log(QStringLiteral("lights: %1 rejected the default password (A1234567)").arg(m_name));
        else
            emit log(QStringLiteral("lights: password reply %1").arg(QString::fromLatin1(v.toHex())));
    }
}

void LedController::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ++m_gen;
    m_scanTimer.stop();
    m_reconnect.stop();
    m_pump.stop();
    stopDiscovery();
    if (!m_device.isEmpty()) {
        QDBusConnection::systemBus().disconnect(QLatin1String(kBluez), m_device,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
            this, SLOT(onDeviceProps(QString, QVariantMap, QStringList)));
        if (!m_char.isEmpty())
            QDBusConnection::systemBus().disconnect(QLatin1String(kBluez), m_char,
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
                this, SLOT(onCharProps(QString, QVariantMap, QStringList)));
        QDBusMessage d = QDBusMessage::createMethodCall(QLatin1String(kBluez), m_device,
                                                        QStringLiteral("org.bluez.Device1"),
                                                        QStringLiteral("Disconnect"));
        QDBusConnection::systemBus().asyncCall(d);
    }
    m_device.clear();
    m_name.clear();
    m_kind = Kind::None;
    m_char.clear();
    m_writing = false;
    m_order.clear();
    m_pending.clear();
    emit deviceChanged();
    setState(QString());
}
