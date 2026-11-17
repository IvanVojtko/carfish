#include "cryptor.h"

#include <QLoggingCategory>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

Q_LOGGING_CATEGORY(lcTls, "aap.tls")

namespace aap {

static QString sslErrors()
{
    QString s;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!s.isEmpty())
            s += QLatin1String("; ");
        s += QString::fromLatin1(buf);
    }
    return s;
}

Cryptor::~Cryptor()
{
    reset();
}

void Cryptor::reset()
{
    if (m_ssl)
        SSL_free(m_ssl); // also frees the BIOs
    if (m_ctx)
        SSL_CTX_free(m_ctx);
    m_ssl = nullptr;
    m_ctx = nullptr;
    m_rbio = m_wbio = nullptr;
}

bool Cryptor::init(Role role, const QString &certPath, const QString &keyPath, QString *error)
{
    reset();
    m_ctx = SSL_CTX_new(role == Role::Server ? TLS_server_method() : TLS_client_method());
    if (!m_ctx) {
        *error = sslErrors();
        return false;
    }
    // Head units in the field speak TLS 1.2 only.
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
    // Head unit certificates are issued by Google's automotive CA, which is
    // not in the system trust store. Verification is left to a later step.
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);

    if (SSL_CTX_use_certificate_chain_file(m_ctx, certPath.toLocal8Bit().constData()) != 1
            || SSL_CTX_use_PrivateKey_file(m_ctx, keyPath.toLocal8Bit().constData(), SSL_FILETYPE_PEM) != 1
            || SSL_CTX_check_private_key(m_ctx) != 1) {
        *error = QStringLiteral("cannot load certificate/key: ") + sslErrors();
        return false;
    }

    m_ssl = SSL_new(m_ctx);
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(m_ssl, m_rbio, m_wbio);
    if (role == Role::Server)
        SSL_set_accept_state(m_ssl);
    else
        SSL_set_connect_state(m_ssl);
    return true;
}

QByteArray Cryptor::drainWrite()
{
    QByteArray out;
    char buf[4096];
    int n;
    while ((n = BIO_read(m_wbio, buf, sizeof(buf))) > 0)
        out.append(buf, n);
    return out;
}

bool Cryptor::handshake(const QByteArray &in, QByteArray &out, QString *error)
{
    if (!in.isEmpty())
        BIO_write(m_rbio, in.constData(), in.size());
    const int r = SSL_do_handshake(m_ssl);
    if (r != 1) {
        const int e = SSL_get_error(m_ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            *error = QStringLiteral("TLS handshake failed: ") + sslErrors();
            return false;
        }
    }
    out = drainWrite();
    if (r == 1)
        qCInfo(lcTls) << "handshake complete:" << peerSummary();
    return true;
}

bool Cryptor::isEstablished() const
{
    return m_ssl && SSL_is_init_finished(m_ssl);
}

QString Cryptor::peerSummary() const
{
    if (!m_ssl)
        return QString();
    QString s = QString::fromLatin1(SSL_get_version(m_ssl)) + QLatin1Char(' ')
            + QString::fromLatin1(SSL_get_cipher_name(m_ssl));
    if (X509 *peer = SSL_get1_peer_certificate(m_ssl)) {
        char name[256];
        X509_NAME_oneline(X509_get_subject_name(peer), name, sizeof(name));
        s += QLatin1String(" peer=") + QString::fromUtf8(name);
        X509_free(peer);
    }
    return s;
}

bool Cryptor::encrypt(const QByteArray &plain, QByteArray &cipher)
{
    if (!isEstablished())
        return false;
    if (SSL_write(m_ssl, plain.constData(), plain.size()) != plain.size()) {
        qCWarning(lcTls) << "SSL_write:" << sslErrors();
        return false;
    }
    cipher = drainWrite();
    return true;
}

bool Cryptor::decrypt(const QByteArray &cipher, QByteArray &plain)
{
    if (!isEstablished())
        return false;
    BIO_write(m_rbio, cipher.constData(), cipher.size());
    plain.clear();
    char buf[16384];
    for (;;) {
        const int n = SSL_read(m_ssl, buf, sizeof(buf));
        if (n > 0) {
            plain.append(buf, n);
            continue;
        }
        const int e = SSL_get_error(m_ssl, n);
        if (e == SSL_ERROR_WANT_READ)
            return true;
        qCWarning(lcTls) << "SSL_read:" << sslErrors();
        return false;
    }
}

} // namespace aap
