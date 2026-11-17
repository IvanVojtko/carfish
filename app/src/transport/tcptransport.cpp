#include "tcptransport.h"

#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>

Q_LOGGING_CATEGORY(lcTcp, "aap.tcp")

// The head unit often opens its AAP TCP listener a moment AFTER it hands us the
// WiFi credentials over RFCOMM, so the first connect can race and fail. Retry a
// few times with a short backoff and a per-attempt timeout before giving up.
static constexpr int kMaxConnectAttempts = 20;
static constexpr int kConnectTimeoutMs = 3000;
static constexpr int kConnectBackoffMs = 700;

TcpTransport::TcpTransport(QObject *parent)
    : Transport(parent)
{
    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(kConnectTimeoutMs);
    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        if (!m_live)
            retryConnect(QStringLiteral("connect timed out"));
    });
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &TcpTransport::tryConnect);
}

TcpTransport *TcpTransport::client(const QString &host, quint16 port, QObject *parent)
{
    auto *t = new TcpTransport(parent);
    t->m_host = host;
    t->m_port = port;
    return t;
}

TcpTransport *TcpTransport::server(quint16 port, QObject *parent)
{
    auto *t = new TcpTransport(parent);
    t->m_serverMode = true;
    t->m_port = port;
    return t;
}

void TcpTransport::start()
{
    if (m_serverMode) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *s = m_server->nextPendingConnection();
            if (m_socket) {
                qCWarning(lcTcp) << "rejecting second connection from" << s->peerAddress();
                s->close();
                s->deleteLater();
                return;
            }
            qCInfo(lcTcp) << "head unit connected from" << s->peerAddress().toString();
            m_live = true;
            adopt(s);
            emit connected();
        });
        if (!m_server->listen(QHostAddress::Any, m_port)) {
            emit disconnected(m_server->errorString());
            return;
        }
        qCInfo(lcTcp) << "listening on port" << m_port;
        return;
    }

    m_live = false;
    m_connectAttempts = 0;
    tryConnect();
}

void TcpTransport::tryConnect()
{
    ++m_connectAttempts;
    auto *s = new QTcpSocket(this);
    adopt(s); // wires readyRead/disconnected/error; while !m_live, errors retry
    connect(s, &QTcpSocket::connected, this, [this] {
        m_live = true;
        m_connectTimer.stop();
        qCInfo(lcTcp) << "connected to" << m_host << m_port << "on attempt" << m_connectAttempts;
        emit connected();
    });
    qCInfo(lcTcp) << "connecting to" << m_host << m_port << "(attempt" << m_connectAttempts << ")";
    s->connectToHost(m_host, m_port);
    m_connectTimer.start();
}

void TcpTransport::retryConnect(const QString &why)
{
    if (m_live || m_retryTimer.isActive())
        return;
    m_connectTimer.stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_connectAttempts >= kMaxConnectAttempts) {
        emit disconnected(QStringLiteral("could not reach head unit AAP server %1:%2 (%3)")
                              .arg(m_host).arg(m_port).arg(why));
        return;
    }
    qCInfo(lcTcp).noquote() << "connect attempt" << m_connectAttempts << "failed:" << why
                            << "— retrying";
    m_retryTimer.start(kConnectBackoffMs);
}

void TcpTransport::adopt(QTcpSocket *socket)
{
    m_socket = socket;
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(socket, &QTcpSocket::readyRead, this, [this] {
        emit dataReceived(m_socket->readAll());
    });
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        if (m_live)
            emit disconnected(QStringLiteral("socket closed"));
    });
    connect(socket, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        const QString err = m_socket ? m_socket->errorString() : QStringLiteral("socket error");
        // Before the session is live, a connect error just means the head unit's
        // AAP server is not up yet — retry instead of tearing the session down.
        if (m_live)
            emit disconnected(err);
        else
            retryConnect(err);
    });
}

void TcpTransport::stop()
{
    m_connectTimer.stop();
    m_retryTimer.stop();
    m_live = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->flush(); // e.g. a final BYEBYE_RESPONSE
        m_socket->abort();
        m_socket->deleteLater();
    }
    if (m_server)
        m_server->close();
}

void TcpTransport::write(const QByteArray &data)
{
    if (m_socket)
        m_socket->write(data);
}

QString TcpTransport::describe() const
{
    return m_serverMode ? QStringLiteral("TCP server :%1").arg(m_port)
                        : QStringLiteral("TCP %1:%2").arg(m_host).arg(m_port);
}
