#pragma once

#include <QDBusAbstractAdaptor>
#include <QObject>
#include <QString>
#include <QStringList>

// Root-side contacts provider. The device user (defaultuser) is not in the
// Sailfish "privileged" group here, so the unprivileged app gets EACCES reading
// the contacts database. carfish-helper runs as root, so it can read it and hand
// the app a plain list of name/number pairs over the system bus
// (org.carfish.Contacts1). It reads a COPY of the DB so the user's real contacts
// database (and its WAL) is never modified.
class ContactsHelper : public QObject
{
    Q_OBJECT
public:
    explicit ContactsHelper(QObject *parent = nullptr);

    // Each entry is "name\tnumber" (tab-separated). The app splits them, builds
    // its number->name map and the contacts pane, and resolves recent calls.
    QStringList contacts();
};

// D-Bus face (interface org.carfish.Contacts1).
class ContactsAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.carfish.Contacts1")
public:
    explicit ContactsAdaptor(ContactsHelper *helper);

public slots:
    QStringList GetContacts();

private:
    ContactsHelper *m_helper;
};
