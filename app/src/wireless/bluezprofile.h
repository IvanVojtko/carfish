#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QVariantMap>

class QSocketNotifier;

// Registers the Android Auto wireless RFCOMM profile with BlueZ
// (org.bluez.ProfileManager1). BlueZ performs SDP and the RFCOMM connect and
// hands us a connected socket in NewConnection().
class BluezProfile : public QObject
{
    Q_OBJECT
public:
    static const QString kAndroidAutoUuid;
    static const QString kAndroidAutoUuidReversed;

    explicit BluezProfile(QObject *parent = nullptr);
    ~BluezProfile() override;

    bool registerProfile(QString *error);
    void unregisterProfile();

    // Asks BlueZ to connect the AA profile on an already paired head unit
    // (object path like /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF).
    void connectDevice(const QString &devicePath);

    // The real Gearhead app is the RFCOMM *client*: it opens the connection to
    // the head unit's AA service, and the head unit (server) then drives the
    // handshake. Enumerate paired BlueZ devices that advertise the AA UUID and
    // initiate the profile connection to each.
    void connectKnownHeadUnits();

    void write(const QByteArray &data);
    void closeConnection();

signals:
    void connectionReady(const QString &devicePath);
    void dataReceived(const QByteArray &data);
    void connectionClosed();
    void error(const QString &message);
    void log(const QString &line);

private:
    friend class BluezProfileAdaptor;
    void onNewConnection(const QDBusObjectPath &device, const QDBusUnixFileDescriptor &fd, const QVariantMap &props);
    void onRequestDisconnection(const QDBusObjectPath &device);

    bool m_registered = false;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QString m_device;
};

class BluezProfileAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Profile1")
public:
    explicit BluezProfileAdaptor(BluezProfile *parent);

public slots:
    void Release();
    void NewConnection(const QDBusObjectPath &device, const QDBusUnixFileDescriptor &fd, const QVariantMap &props);
    void RequestDisconnection(const QDBusObjectPath &device);

private:
    BluezProfile *m_profile;
};
