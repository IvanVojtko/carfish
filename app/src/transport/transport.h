#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

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
    // Short transport health summary since the last call (e.g. USB write
    // stalls), for the session's periodic stats line. Empty if nothing to say.
    virtual QString takeStats() { return QString(); }

signals:
    void connected();
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason);
    // A transport event worth a line in the durable log (e.g. USB bus events).
    void note(const QString &line);
};
