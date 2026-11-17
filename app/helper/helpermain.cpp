#include "wifihelper.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

// carfish-helper: a small privileged system service. It runs as root (via
// systemd) so it can drive connman on a stock Sailfish OS, and exposes
// org.carfish.Helper1 on the system bus for the unprivileged app to call.
static const char *kServiceName = "org.carfish.Helper";
static const char *kObjectPath = "/org/carfish/Helper";

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    WifiHelper helper;
    new HelperAdaptor(&helper); // parented to helper; exports org.carfish.Helper1

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(QString::fromLatin1(kObjectPath), &helper)) {
        qWarning() << "carfish-helper: cannot export object:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerService(QString::fromLatin1(kServiceName))) {
        qWarning() << "carfish-helper: cannot own" << kServiceName << ":" << bus.lastError().message();
        return 1;
    }
    qInfo() << "carfish-helper: ready on" << kServiceName;
    return app.exec();
}
