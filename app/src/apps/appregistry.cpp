#include "appregistry.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcApps, "aap.apps")

AppRegistry::AppRegistry(QObject *parent)
    : QObject(parent)
{
    refresh();
}

void AppRegistry::refresh()
{
    m_entries.clear();
    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    dirs << QStringLiteral("/usr/share/applications")
         << QDir::homePath() + QStringLiteral("/.local/share/applications");
    dirs.removeDuplicates();

    QSet<QString> seen;
    for (const QString &dir : dirs) {
        const QDir d(dir);
        const auto files = d.entryInfoList({ QStringLiteral("*.desktop") }, QDir::Files);
        for (const QFileInfo &fi : files) {
            const QString id = fi.completeBaseName();
            if (seen.contains(id))
                continue;

            QSettings de(fi.absoluteFilePath(), QSettings::IniFormat);
            de.beginGroup(QStringLiteral("Desktop Entry"));
            if (de.value(QStringLiteral("Type")).toString() != QLatin1String("Application"))
                continue;
            if (de.value(QStringLiteral("NoDisplay")).toBool()
                    || de.value(QStringLiteral("Hidden")).toBool())
                continue;

            Entry e;
            e.id = id;
            e.name = de.value(QStringLiteral("Name")).toString();
            e.exec = de.value(QStringLiteral("Exec")).toString();
            e.icon = de.value(QStringLiteral("Icon")).toString();
            e.categories = de.value(QStringLiteral("Categories")).toString();
            // Android apps run through apkd; detect by the launcher or an apkd key.
            e.android = e.exec.contains(QStringLiteral("apkd-launcher"))
                    || id.startsWith(QStringLiteral("apkd_"))
                    || de.contains(QStringLiteral("X-apkd-packageName"));
            if (e.name.isEmpty() || e.exec.isEmpty())
                continue;
            seen.insert(id);
            m_entries.append(e);
        }
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const Entry &a, const Entry &b) { return a.name.localeAwareCompare(b.name) < 0; });
    qCInfo(lcApps) << "found" << m_entries.size() << "applications";
    emit changed();
}

const AppRegistry::Entry *AppRegistry::find(const QString &id) const
{
    for (const Entry &e : m_entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

QString AppRegistry::name(const QString &id) const
{
    const Entry *e = find(id);
    return e ? e->name : QString();
}

QVariantList AppRegistry::toVariantList() const
{
    QVariantList list;
    for (const Entry &e : m_entries) {
        QVariantMap m;
        m[QStringLiteral("id")] = e.id;
        m[QStringLiteral("name")] = e.name;
        m[QStringLiteral("android")] = e.android;
        m[QStringLiteral("categories")] = e.categories;
        list.append(m);
    }
    return list;
}

// Split an Exec line into argv, honouring simple quoting and dropping the XDG
// field codes (%u %U %f %F %i %c %k, and %% -> %).
static QStringList parseExec(const QString &exec)
{
    QStringList args;
    QString cur;
    bool inQuote = false;
    bool have = false;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (c == QLatin1Char('"')) {
            inQuote = !inQuote;
            have = true;
        } else if (c == QLatin1Char('%') && i + 1 < exec.size()) {
            const QChar n = exec[++i];
            if (n == QLatin1Char('%')) {
                cur += QLatin1Char('%');
                have = true;
            }
            // other field codes are dropped
        } else if (c.isSpace() && !inQuote) {
            if (have) {
                args << cur;
                cur.clear();
                have = false;
            }
        } else {
            cur += c;
            have = true;
        }
    }
    if (have)
        args << cur;
    return args;
}

bool AppRegistry::launch(const QString &id) const
{
    const Entry *e = find(id);
    if (!e) {
        qCWarning(lcApps) << "launch: unknown app" << id;
        return false;
    }
    QStringList argv = parseExec(e->exec);
    if (argv.isEmpty())
        return false;
    const QString program = argv.takeFirst();
    const bool ok = QProcess::startDetached(program, argv);
    qCInfo(lcApps) << "launch" << id << program << argv << "->" << ok;
    return ok;
}
