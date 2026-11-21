#include "contactshelper.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <pwd.h>

ContactsHelper::ContactsHelper(QObject *parent)
    : QObject(parent)
{
}

static QString contactsDbPath()
{
    QString home = QStringLiteral("/home/defaultuser");
    if (struct passwd *pw = getpwnam("defaultuser"))
        home = QString::fromLocal8Bit(pw->pw_dir);
    return home + QStringLiteral("/.local/share/system/privileged/Contacts/qtcontacts-sqlite/contacts.db");
}

QStringList ContactsHelper::contacts()
{
    QStringList out;
    const QString src = contactsDbPath();
    if (!QFile::exists(src)) {
        qWarning() << "carfish-helper: contacts DB not found at" << src;
        return out;
    }

    // Work on a copy so we never write to the user's real DB (its WAL). Root can
    // read the privileged directory; the copy lands in /tmp (world-readable dir).
    const QString dir = QStringLiteral("/tmp/carfish-contacts");
    QDir().mkpath(dir);
    const QString copy = dir + QStringLiteral("/contacts.db");
    for (const QString &suf : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
        QFile::remove(copy + suf);
    if (!QFile::copy(src, copy)) {
        qWarning() << "carfish-helper: cannot copy contacts DB";
        return out;
    }
    // Copy the WAL too (if any) so recent, un-checkpointed edits are included.
    if (QFile::exists(src + QStringLiteral("-wal")))
        QFile::copy(src + QStringLiteral("-wal"), copy + QStringLiteral("-wal"));

    int count = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("carfish_contacts"));
        db.setDatabaseName(copy);
        if (!db.open()) {
            qWarning() << "carfish-helper: open contacts DB:" << db.lastError().text();
        } else {
            // This qtcontacts-sqlite schema keeps names in SEPARATE tables (the
            // Contacts table has no name columns). Discover tables/columns at
            // runtime and build the query to match, so it adapts to any version.
            auto colsOf = [&db](const QString &table) {
                QStringList cols;
                QSqlQuery q(QStringLiteral("PRAGMA table_info(") + table + QLatin1Char(')'), db);
                while (q.next())
                    cols << q.value(1).toString();
                return cols;
            };
            QStringList tables;
            { QSqlQuery q(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"), db);
              while (q.next()) tables << q.value(0).toString(); }

            // Full schema dump (readable) for diagnosing any future change.
            QFile sf(dir + QStringLiteral("/schema.txt"));
            if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                for (const QString &t : tables)
                    sf.write((t + QStringLiteral(": ") + colsOf(t).join(QLatin1Char(',')) + QLatin1Char('\n')).toUtf8());
                sf.close();
            }

            // Build a coalesced name expression from whichever name tables exist.
            QStringList joins, nameParts;
            if (tables.contains(QStringLiteral("DisplayLabels"))
                    && colsOf(QStringLiteral("DisplayLabels")).contains(QStringLiteral("displayLabel"))) {
                joins << QStringLiteral("LEFT JOIN DisplayLabels dl ON dl.contactId = c.contactId");
                nameParts << QStringLiteral("nullif(dl.displayLabel,'')");
            }
            const QStringList nameCols = colsOf(QStringLiteral("Names"));
            if (tables.contains(QStringLiteral("Names")) && nameCols.contains(QStringLiteral("firstName"))) {
                joins << QStringLiteral("LEFT JOIN Names nm ON nm.contactId = c.contactId");
                const QString ln = nameCols.contains(QStringLiteral("lastName")) ? QStringLiteral("coalesce(nm.lastName,'')") : QStringLiteral("''");
                nameParts << (QStringLiteral("nullif(trim(coalesce(nm.firstName,'')||' '||") + ln + QStringLiteral("),'')"));
            }
            nameParts << QStringLiteral("p.phoneNumber"); // final fallback
            const QString nameExpr = QStringLiteral("coalesce(") + nameParts.join(QStringLiteral(", ")) + QLatin1Char(')');

            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT %1 AS nm, p.phoneNumber AS num "
                                       "FROM PhoneNumbers p JOIN Contacts c ON c.contactId = p.contactId %2 "
                                       "WHERE p.phoneNumber IS NOT NULL AND p.phoneNumber <> ''")
                            .arg(nameExpr, joins.join(QLatin1Char(' '))))) {
                qWarning() << "carfish-helper: contacts query:" << q.lastError().text();
            } else {
                while (q.next()) {
                    QString nm = q.value(0).toString().trimmed();
                    const QString num = q.value(1).toString().trimmed();
                    if (num.isEmpty())
                        continue;
                    if (nm.isEmpty())
                        nm = num;
                    out << nm + QLatin1Char('\t') + num;
                    ++count;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("carfish_contacts"));
    for (const QString &suf : { QString(), QStringLiteral("-wal"), QStringLiteral("-shm") })
        QFile::remove(copy + suf);

    qInfo() << "carfish-helper: provided" << count << "contact numbers";
    return out;
}

ContactsAdaptor::ContactsAdaptor(ContactsHelper *helper)
    : QDBusAbstractAdaptor(helper)
    , m_helper(helper)
{
}

QStringList ContactsAdaptor::GetContacts()
{
    return m_helper->contacts();
}
