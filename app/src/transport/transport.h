#pragma once

#include <QByteArray>
#include <QObject>

// Byte pipe between the phone and the head unit. The AAP framing is the same
// over USB (AOA bulk endpoints) and TCP (wireless / head unit server mode).
class Transport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~Transport() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void write(const QByteArray &data) = 0;
    virtual QString describe() const = 0;

signals:
    void connected();
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason);
};
