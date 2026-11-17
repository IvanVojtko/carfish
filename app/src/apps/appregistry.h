#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

// Enumerates installed applications from their XDG .desktop entries (both native
// Sailfish apps and Android apps exposed through App Support / apkd) and can
// launch one. Used so the user can choose which app backs navigation and which
// backs media; the chosen apps are started when projection begins.
class AppRegistry : public QObject
{
    Q_OBJECT
    // Exposed to the projected car UI (as context property "appList") so the
    // Apps screen can list and launch installed apps.
    Q_PROPERTY(QVariantList apps READ toVariantList NOTIFY changed)
public:
    struct Entry {
        QString id;         // .desktop basename without extension
        QString name;       // display name
        QString exec;       // Exec line (with field codes)
        QString icon;
        QString categories; // XDG Categories string
        bool android = false;
    };

    explicit AppRegistry(QObject *parent = nullptr);

    void refresh();
    const QVector<Entry> &entries() const { return m_entries; }
    const Entry *find(const QString &id) const;
    QString name(const QString &id) const;

    // {id, name, android, categories} per app, sorted by name, for the QML picker.
    QVariantList toVariantList() const;

    // Launch the app detached; returns false if unknown or it failed to spawn.
    Q_INVOKABLE bool launch(const QString &id) const;

signals:
    void changed();

private:
    QVector<Entry> m_entries;
};
