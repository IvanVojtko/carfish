#include "wifijoiner.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>

static const QString kSvc = QStringLiteral("org.carfish.Helper");
static const QString kPath = QStringLiteral("/org/carfish/Helper");
static const QString kIface = QStringLiteral("org.carfish.Helper1");

WifiJoiner::WifiJoiner(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Joined"), this, SLOT(onJoined(QString)));
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Failed"), this, SLOT(onFailed(QString)));
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Log"), this, SLOT(onLog(QString)));
}

void WifiJoiner::join(const QString &ssid, const QString &passphrase, const QString &bssid)
{
    QDBusInterface helper(kSvc, kPath, kIface, QDBusConnection::systemBus());
    // The helper's Join returns at once (it starts the connect and reports the
    // result via the Joined/Failed signals). If the helper is missing this call
    // errors and we say so.
    const QDBusMessage r = helper.call(QStringLiteral("Join"), ssid, passphrase, bssid);
    if (r.type() == QDBusMessage::ErrorMessage) {
        emit failed(QStringLiteral("WiFi helper unavailable: %1 — is carfish-helper running?")
                        .arg(r.errorMessage()));
    }
}

void WifiJoiner::cancel()
{
    QDBusInterface helper(kSvc, kPath, kIface, QDBusConnection::systemBus());
    helper.asyncCall(QStringLiteral("Release"));
}

void WifiJoiner::onJoined(const QString &localAddress) { emit joined(localAddress); }
void WifiJoiner::onFailed(const QString &reason) { emit failed(reason); }
void WifiJoiner::onLog(const QString &line) { emit log(line); }
