#pragma once

#include <QObject>
#include <QString>

// Client for the privileged carfish-helper service. On a stock Sailfish OS
// the app runs as the unprivileged device user, which connman won't let manage
// WiFi, so the actual connman connect + routing happens in the root helper over
// the system bus (org.carfish.Helper1). This keeps the same signals the
// rest of the app already uses.
class WifiJoiner : public QObject
{
    Q_OBJECT
public:
    explicit WifiJoiner(QObject *parent = nullptr);

    void join(const QString &ssid, const QString &passphrase, const QString &bssid);
    void cancel();

signals:
    void joined(const QString &localAddress);
    void failed(const QString &reason);
    void log(const QString &line);

private slots:
    void onJoined(const QString &localAddress);
    void onFailed(const QString &reason);
    void onLog(const QString &line);
};
