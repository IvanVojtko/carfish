#pragma once

#include <QObject>
#include <QString>

// App-side proxy for the privileged USB gadget switch in carfish-helper
// (system bus, org.carfish.Usb1). The app runs as the unprivileged device
// user and cannot touch configfs / usb-moded; it asks the helper to wait for
// the car's AOA START and rebuild the gadget as a FunctionFS accessory, then
// receives the FunctionFS mount dir to open with UsbFfsTransport.
class UsbHelperClient : public QObject
{
    Q_OBJECT
public:
    explicit UsbHelperClient(QObject *parent = nullptr);
    ~UsbHelperClient() override;

    void start(); // begin the AOA wait + gadget switch
    void stop();  // tear down and restore usb-moded

signals:
    void ready(const QString &ffsDir);          // gadget bound; ep0/ep1/ep2 ready
    void startRequested(const QString &model);  // AOA START seen (diagnostic)
    void failed(const QString &reason);
    void log(const QString &line);

private:
    bool m_started = false;
};
