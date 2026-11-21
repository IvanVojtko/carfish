#include "usbhelperclient.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>

static const QString kSvc = QStringLiteral("org.carfish.Helper");
static const QString kPath = QStringLiteral("/org/carfish/Usb");
static const QString kIface = QStringLiteral("org.carfish.Usb1");

UsbHelperClient::UsbHelperClient(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Ready"), this, SIGNAL(ready(QString)));
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Failed"), this, SIGNAL(failed(QString)));
    bus.connect(kSvc, kPath, kIface, QStringLiteral("Log"), this, SIGNAL(log(QString)));
    // StartRequested carries (model, serial) on the bus; we forward only the
    // model. Qt D-Bus matches on the signal name and drops the trailing arg.
    bus.connect(kSvc, kPath, kIface, QStringLiteral("StartRequested"),
                this, SIGNAL(startRequested(QString)));
}

UsbHelperClient::~UsbHelperClient()
{
    stop();
}

void UsbHelperClient::start()
{
    if (m_started)
        return;
    m_started = true;
    QDBusInterface usb(kSvc, kPath, kIface, QDBusConnection::systemBus());
    usb.asyncCall(QStringLiteral("Start"));
}

void UsbHelperClient::stop()
{
    if (!m_started)
        return;
    m_started = false;
    QDBusInterface usb(kSvc, kPath, kIface, QDBusConnection::systemBus());
    usb.asyncCall(QStringLiteral("Stop"));
}
