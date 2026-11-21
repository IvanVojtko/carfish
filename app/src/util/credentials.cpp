#include "credentials.h"

#include <QFileInfo>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace util {

// const-correct for OpenSSL 4 (getters return const) while still building
// against the device's older OpenSSL, whose get_index_by_NID takes non-const.
static QString nameField(const X509_NAME *name, int nid)
{
    if (!name)
        return QString();
    const int i = X509_NAME_get_index_by_NID(const_cast<X509_NAME *>(name), nid, -1);
    if (i < 0)
        return QString();
    const X509_NAME_ENTRY *e = X509_NAME_get_entry(name, i);
    const ASN1_STRING *d = X509_NAME_ENTRY_get_data(e);
    unsigned char *utf8 = nullptr;
    const int len = ASN1_STRING_to_UTF8(&utf8, d);
    QString s = len > 0 ? QString::fromUtf8(reinterpret_cast<char *>(utf8), len) : QString();
    if (utf8)
        OPENSSL_free(utf8);
    return s;
}

static QString nameOneline(const X509_NAME *name)
{
    const QString cn = nameField(name, NID_commonName);
    const QString o = nameField(name, NID_organizationName);
    if (!cn.isEmpty() && !o.isEmpty())
        return cn + QStringLiteral(" (") + o + QLatin1Char(')');
    return cn.isEmpty() ? o : cn;
}

static QString asn1TimeString(const ASN1_TIME *t)
{
    if (!t)
        return QString();
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
        return QString();
    QString out;
    if (ASN1_TIME_print(bio, t)) {
        char *data = nullptr;
        const long n = BIO_get_mem_data(bio, &data);
        if (n > 0)
            out = QString::fromLatin1(data, int(n));
    }
    BIO_free(bio);
    return out;
}

CredentialInfo inspectCredentials(const QString &certPath, const QString &keyPath)
{
    CredentialInfo info;

    info.certFound = QFileInfo::exists(certPath);
    info.keyFound = QFileInfo::exists(keyPath);

    X509 *cert = nullptr;
    EVP_PKEY *key = nullptr;

    if (info.certFound) {
        if (BIO *bio = BIO_new_file(certPath.toLocal8Bit().constData(), "r")) {
            cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
        }
        if (cert) {
            info.certValid = true;
            info.subject = nameOneline(X509_get_subject_name(cert));
            info.issuer = nameOneline(X509_get_issuer_name(cert));
            info.googleRooted =
                nameField(X509_get_issuer_name(cert), NID_organizationName)
                    .contains(QStringLiteral("Google Automotive Link"), Qt::CaseInsensitive);

            const ASN1_TIME *na = X509_get0_notAfter(cert);
            const ASN1_TIME *nb = X509_get0_notBefore(cert);
            info.notAfter = asn1TimeString(na);
            info.expired = na && X509_cmp_current_time(na) < 0;
            info.notYetValid = nb && X509_cmp_current_time(nb) > 0;
        }
    }

    if (info.keyFound) {
        if (BIO *bio = BIO_new_file(keyPath.toLocal8Bit().constData(), "r")) {
            key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
        }
        info.keyValid = key != nullptr;
    }

    if (cert && key)
        info.match = X509_check_private_key(cert, key) == 1;

    if (cert)
        X509_free(cert);
    if (key)
        EVP_PKEY_free(key);

    return info;
}

QString CredentialInfo::summary(const QString &certPath, const QString &keyPath) const
{
    if (!certFound)
        return QStringLiteral("Problem: no certificate at %1 — place your PEM there.").arg(certPath);
    if (!certValid)
        return QStringLiteral("Problem: %1 is not a valid PEM certificate.").arg(certPath);
    if (!keyFound)
        return QStringLiteral("Problem: no private key at %1 — place your PEM key there.").arg(keyPath);
    if (!keyValid)
        return QStringLiteral("Problem: %1 is not a valid PEM private key.").arg(keyPath);
    if (!match)
        return QStringLiteral("Problem: the private key does not match the certificate.");
    if (notYetValid)
        return QStringLiteral("Problem: the certificate is not valid yet.");
    if (expired)
        return QStringLiteral("Problem: the certificate expired (%1).").arg(notAfter);

    const QString kind = googleRooted
            ? QStringLiteral("Google-rooted")
            : QStringLiteral("self-signed/other (OK for open head units)");
    return QStringLiteral("Ready — %1 cert, subject %2, expires %3.")
            .arg(kind, subject.isEmpty() ? QStringLiteral("?") : subject,
                 notAfter.isEmpty() ? QStringLiteral("?") : notAfter);
}

} // namespace util
