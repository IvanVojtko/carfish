#pragma once

#include "transport.h"

#include <QThread>
#include <atomic>

// Wired AA. The car is the USB host and switches the phone into Android Open
// Accessory (AOA) mode. On the phone side the kernel gadget function
// "accessory" (f_accessory, CONFIG_USB_CONFIGFS_F_ACC) answers the AOA
// control requests and exposes the two bulk endpoints as /dev/usb_accessory.
// usb-moded must be configured with an "android_auto" mode that enables that
// function (see config/usb-moded/).
class UsbAccessoryTransport : public Transport
{
    Q_OBJECT
public:
    explicit UsbAccessoryTransport(const QString &device = QStringLiteral("/dev/usb_accessory"),
                                   QObject *parent = nullptr);
    ~UsbAccessoryTransport() override;

    void start() override;
    void stop() override;
    void write(const QByteArray &data) override;
    QString describe() const override;

private:
    QString m_device;
    int m_fd = -1;
    class UsbAccessoryReader *m_reader = nullptr;
};

// Blocking reader thread (Qt 5.6 has no QThread::create).
class UsbAccessoryReader : public QThread
{
    Q_OBJECT
public:
    explicit UsbAccessoryReader(int fd) : m_fd(fd) {}
    std::atomic_bool running{true};

signals:
    void chunk(const QByteArray &data);
    void failed(const QString &error);

protected:
    void run() override;

private:
    int m_fd;
};
