#pragma once

#include <QByteArray>
#include <QString>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

namespace aap {

// TLS over the AAP control channel. Handshake records travel inside
// SSL_HANDSHAKE control messages; afterwards every encrypted frame carries
// the TLS records of exactly one fragment. Memory BIOs keep OpenSSL away
// from the transport.
class Cryptor
{
public:
    enum class Role { Client, Server };

    Cryptor() = default;
    ~Cryptor();
    Cryptor(const Cryptor &) = delete;
    Cryptor &operator=(const Cryptor &) = delete;

    // certPath/keyPath: PEM files. The private key is never bundled with the
    // application; the user provides it (see README).
    bool init(Role role, const QString &certPath, const QString &keyPath, QString *error);
    void reset();
    // Server: ask the peer for its certificate in the handshake (and accept
    // whatever it sends, or none), as Gearhead does. Call before handshake().
    void requestPeerCertificate();

    // Feed handshake bytes from the peer (may be empty to start a client
    // handshake). Returns bytes to send back in an SSL_HANDSHAKE message.
    bool handshake(const QByteArray &in, QByteArray &out, QString *error);
    bool isEstablished() const;
    QString peerSummary() const;

    bool encrypt(const QByteArray &plain, QByteArray &cipher);
    bool decrypt(const QByteArray &cipher, QByteArray &plain);

private:
    QByteArray drainWrite();

    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    BIO *m_rbio = nullptr; // network -> ssl
    BIO *m_wbio = nullptr; // ssl -> network
};

} // namespace aap
