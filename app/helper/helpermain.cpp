#include "contactshelper.h"
#include "usbgadget.h"
#include "wifihelper.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

// carfish-helper: a small privileged system service. It runs as root (via
// systemd) so it can drive connman, the USB gadget and read the (privileged)
// contacts DB on a stock Sailfish OS, and exposes org.carfish.Helper1
// (WiFi/routing), org.carfish.Usb1 (wired AA gadget switch) and
// org.carfish.Contacts1 on the system bus for the unprivileged app.
static const char *kServiceName = "org.carfish.Helper";
static const char *kObjectPath = "/org/carfish/Helper";
static const char *kUsbObjectPath = "/org/carfish/Usb";
static const char *kContactsObjectPath = "/org/carfish/Contacts";

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    WifiHelper helper;
    new HelperAdaptor(&helper); // parented to helper; exports org.carfish.Helper1

    UsbGadget usb;
    new UsbAdaptor(&usb); // parented to usb; exports org.carfish.Usb1

    ContactsHelper contacts;
    new ContactsAdaptor(&contacts); // parented to contacts; exports org.carfish.Contacts1

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(QString::fromLatin1(kObjectPath), &helper)) {
        qWarning() << "carfish-helper: cannot export object:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerObject(QString::fromLatin1(kUsbObjectPath), &usb)) {
        qWarning() << "carfish-helper: cannot export USB object:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerObject(QString::fromLatin1(kContactsObjectPath), &contacts)) {
        qWarning() << "carfish-helper: cannot export Contacts object:" << bus.lastError().message();
        return 1;
    }
    if (!bus.registerService(QString::fromLatin1(kServiceName))) {
        qWarning() << "carfish-helper: cannot own" << kServiceName << ":" << bus.lastError().message();
        return 1;
    }
    qInfo() << "carfish-helper: ready on" << kServiceName;
    return app.exec();
}
