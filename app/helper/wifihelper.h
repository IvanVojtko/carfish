#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusVariant>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

// Root-side worker for wireless Android Auto on a stock Sailfish OS, where the
// app runs as the unprivileged device user and connman refuses network changes
// from non-privileged callers.
//
// The app asks this service (over the system bus, org.carfish.Helper1) to:
//   * Join the head unit's WiFi access point (connman connect), and
//   * keep the internet on cellular while the car's AP (no uplink) carries only
//     the projection traffic (drop the car WiFi's default route),
// and the service reverts the routing automatically when the car's WiFi drops
// or the app calls Release. Nothing here is car-specific.
class WifiHelper : public QObject
{
    Q_OBJECT
public:
    explicit WifiHelper(QObject *parent = nullptr);

    void join(const QString &ssid, const QString &passphrase, const QString &bssid);
    void release();

signals:
    void joined(const QString &localAddress);
    void failed(const QString &reason);
    void log(const QString &line);

private slots:
    void onServicePropertyChanged(const QString &name, const QDBusVariant &value);

private:
    void attempt();
    void watchService(const QString &path);
    void stopWatching();
    void forgetService(const QString &path); // disconnect + remove a stale service
    void applyRouting(const QVariantMap &ipv4);
    void applyRoutingFromService(); // re-read the service's IPv4 and apply
    void enforceCellularDefault();  // pin the kernel default route to cellular
    bool cellularRoute(QString &iface, QString &gw); // active cellular iface + gw
    void logDiagnostics();          // log resolv.conf + default route after the change
    void revertRouting();

    QString m_ssid;
    QString m_passphrase;
    QString m_bssid;
    QString m_servicePath;   // the car WiFi service we joined
    QString m_wifiIface;     // the car WiFi interface (e.g. wlan0)
    bool m_rerouted = false; // we changed IPv4 config and must revert it
    bool m_ready = false;     // joined signalled
    int m_attempts = 0;
    int m_routeAttempts = 0;  // retries waiting for the car WiFi IPv4 to settle
    bool m_cellularPinned = false; // last enforce result (for quiet logging)
    QTimer m_retry;
    QTimer m_routeRetry;
    QTimer m_enforce;         // re-assert the cellular default route while on car WiFi
};

// D-Bus face of WifiHelper (interface org.carfish.Helper1).
class HelperAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.carfish.Helper1")
public:
    explicit HelperAdaptor(WifiHelper *helper);

public slots:
    void Join(const QString &ssid, const QString &passphrase, const QString &bssid);
    void Release();

signals:
    void Joined(const QString &localAddress);
    void Failed(const QString &reason);
    void Log(const QString &line);

private:
    WifiHelper *m_helper;
};
