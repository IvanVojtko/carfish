#pragma once

#include "transport.h"

#include <QHostAddress>
#include <QPointer>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

// Wireless AA: the phone connects to the head unit (address/port received in
// WifiStartRequest over RFCOMM, port is normally 5288).
// Server mode: the phone listens (like Android's "head unit server" on 5277)
// and a head unit / Desktop Head Unit connects to it - handy for testing.
class TcpTransport : public Transport
{
    Q_OBJECT
public:
    static TcpTransport *client(const QString &host, quint16 port, QObject *parent = nullptr);
    static TcpTransport *server(quint16 port, QObject *parent = nullptr);

    void start() override;
    void stop() override;
    void write(const QByteArray &data) override;
    QString describe() const override;

private:
    explicit TcpTransport(QObject *parent);
    void adopt(QTcpSocket *socket);
    void tryConnect();                 // client: one connect attempt (with retry)
    void retryConnect(const QString &why);

    bool m_serverMode = false;
    QString m_host;
    quint16 m_port = 0;
    QPointer<QTcpServer> m_server;
    QPointer<QTcpSocket> m_socket;
    bool m_live = false;               // the connect succeeded; errors now end the session
    int m_connectAttempts = 0;
    QTimer m_connectTimer;             // per-attempt connect timeout
    QTimer m_retryTimer;               // backoff between connect attempts
};
