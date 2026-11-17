#pragma once

#include <QString>

namespace util {

// Inspects the TLS credentials the user placed in phone storage. The private
// key is never bundled with the app; this only reads whatever PEM files the
// certPath/keyPath point at and reports whether they can be used.
struct CredentialInfo {
    bool certFound = false;
    bool keyFound = false;
    bool certValid = false;  // parsed as X.509
    bool keyValid = false;   // parsed as a private key
    bool match = false;      // key matches the certificate
    bool expired = false;
    bool notYetValid = false;
    bool googleRooted = false; // issuer is Google's automotive CA
    QString subject;
    QString issuer;
    QString notAfter;

    bool usable() const { return certValid && keyValid && match && !expired && !notYetValid; }

    // One line for the UI/log. Starts with "Ready" when usable, else "Problem:".
    QString summary(const QString &certPath, const QString &keyPath) const;
};

CredentialInfo inspectCredentials(const QString &certPath, const QString &keyPath);

} // namespace util
