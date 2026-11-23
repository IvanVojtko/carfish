#pragma once

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <initializer_list>

// Bluetooth LE ambient-light kits from the "LED LAMP" app family (advertised
// names LEDCAR-00/01/02-..., LEDBLE-...; an HM-10 style UART on GATT service
// FFE0, every command a 9-byte write to characteristic FFE1). The protocol is
// documented in ~/MyWork/led/LED_PROTOCOL.md (reverse engineered, untested on
// hardware).
//
// While a car session runs, this looks for such a kit, connects, and lets the
// car's Settings screen set a static colour, brightness and on/off. The kit
// reports nothing back, so the light state shown is our own, persisted model.
// Talks to BlueZ over D-Bus (Qt 5.6's QtBluetooth LE predates BlueZ's GATT API).
class LedController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool found READ found NOTIFY deviceChanged)
    Q_PROPERTY(QString name READ name NOTIFY deviceChanged)
    // "" (idle), "searching", "connecting", "connected", "disconnected"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool on READ on NOTIFY lightChanged)
    Q_PROPERTY(QColor color READ color NOTIFY lightChanged)
    Q_PROPERTY(int brightness READ brightness NOTIFY lightChanged)
public:
    explicit LedController(QObject *parent = nullptr);
    ~LedController() override;

    void start(); // car session is up: find a kit and connect
    void stop();  // session over: disconnect

    bool found() const { return !m_device.isEmpty(); }
    QString name() const { return m_name; }
    QString state() const { return m_state; }
    bool on() const { return m_on; }
    QColor color() const { return m_color; }
    int brightness() const { return m_brightness; }

    Q_INVOKABLE void setOn(bool on);
    Q_INVOKABLE void setColor(const QColor &color); // also switches the lights on
    Q_INVOKABLE void setBrightness(int percent);    // 1..100

    // Frame building, public for tests.
    enum class Kind { None, Ble7E, Ble02, Ble03, Car01, Car02 };
    static Kind kindFor(const QString &name);
    static QByteArray frame(quint8 header, std::initializer_list<int> payload7);
    static QList<QByteArray> powerFrames(Kind k, bool on);
    static QList<QByteArray> colorFrames(Kind k, const QColor &c);
    static QList<QByteArray> brightnessFrames(Kind k, int percent);
    static QByteArray handshake();

signals:
    void deviceChanged();
    void stateChanged();
    void lightChanged();
    void log(const QString &line);

private slots:
    void onDeviceProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onCharProps(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    using Objects = QMap<QString, QMap<QString, QVariantMap>>; // path -> iface -> props
    void listObjects(std::function<void(const Objects &)> done);
    void scanTick();
    void stopDiscovery();
    void connectDevice();
    void findCharacteristic(int attempt);
    void dropConnection(const QString &why);
    void setState(const QString &s);
    void queue(const QString &key, const QList<QByteArray> &frames);
    void pump();
    void save();

    QSettings m_settings;
    bool m_running = false;
    QString m_adapter;          // /org/bluez/hciN
    QString m_device;           // /org/bluez/hciN/dev_XX_..
    QString m_name;
    Kind m_kind = Kind::None;
    QString m_char;             // FFE1 characteristic path, once resolved
    bool m_writeCommand = true; // write-without-response if offered
    QString m_state;
    bool m_discovering = false;
    int m_scanTicks = 0;
    int m_connectTries = 0;
    QTimer m_scanTimer;
    QTimer m_reconnect;
    QTimer m_pump;
    int m_gen = 0;              // bumps on stop(); stale async replies are dropped

    bool m_on = true;
    QColor m_color;
    int m_brightness = 80;

    QStringList m_order;                       // keys in the order to send
    QMap<QString, QList<QByteArray>> m_pending; // key -> frames (latest value wins)
    bool m_writing = false;
};
