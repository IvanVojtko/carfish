#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

class QDBusInterface;
#ifndef CARFISH_DESKTOP
namespace CommHistory { class CallModel; }
#endif

// Phone/dialer backend for the car UI. Provides the address book (contacts),
// the recent-call log, and live call control, and exposes them to CarMain as
// the context property "phone".
//
// Sources on the device:
//   - contacts : QtContacts (org.nemomobile.contacts.sqlite)
//   - recents  : libcommhistory CallModel
//   - calls    : the session-bus voicecall-manager (org.nemomobile.voicecall),
//                the same daemon Sailfish's own dialer drives. Placing a call
//                there routes its audio to the car over Bluetooth hands-free,
//                so we only initiate/track calls — we don't carry voice.
// On the desktop host build these are replaced by a small mock so the UI can
// be exercised in the simulator.
class PhoneManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QVariantList recents READ recents NOTIFY recentsChanged)
    Q_PROPERTY(QString query READ query NOTIFY contactsChanged)
    // Active call (empty / false when idle).
    Q_PROPERTY(bool hasCall READ hasCall NOTIFY callChanged)
    Q_PROPERTY(QString callNumber READ callNumber NOTIFY callChanged)
    Q_PROPERTY(QString callName READ callName NOTIFY callChanged)
    Q_PROPERTY(QString callStatus READ callStatus NOTIFY callChanged)
    Q_PROPERTY(bool incoming READ incoming NOTIFY callChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY callChanged)
    Q_PROPERTY(int callDuration READ callDuration NOTIFY callChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callChanged)
public:
    explicit PhoneManager(QObject *parent = nullptr);

    QVariantList contacts() const { return m_contacts; }
    QVariantList recents() const { return m_recents; }
    QString query() const { return m_query; }

    bool hasCall() const { return !m_callPath.isEmpty(); }
    QString callNumber() const { return m_callNumber; }
    QString callName() const { return m_callName; }
    QString callStatus() const;              // human text: "Dialling", "Ringing"…
    bool incoming() const { return m_callStatus == 5; }   // STATUS_INCOMING
    bool connected() const { return m_callStatus == 1; }  // STATUS_ACTIVE
    int callDuration() const { return m_callDuration; }
    bool muted() const { return m_muted; }

    // Reload contacts + recents (e.g. when the phone pane is opened).
    Q_INVOKABLE void refresh();
    // Filter the contact list (case-insensitive, name or number).
    Q_INVOKABLE void setQuery(const QString &q);

    Q_INVOKABLE void dial(const QString &number);
    Q_INVOKABLE void answer();
    Q_INVOKABLE void hangup();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void sendDtmf(const QString &tone);   // dial-pad while in a call

    // Resolve a phone number to a contact name ("" if unknown). Also used by QML.
    Q_INVOKABLE QString nameForNumber(const QString &number) const;

signals:
    void contactsChanged();
    void recentsChanged();
    void callChanged();
    void log(const QString &line);   // routed to the durable device log

private slots:
    void onVoiceCallsChanged();
    void onCallStatusChanged();
    void onCallDurationChanged();
#ifndef CARFISH_DESKTOP
    // Rebuild m_recents from the live CallModel; also fires when the contact
    // resolver fills in names asynchronously, so recents gain names on their own.
    void rebuildRecents();
#endif

private:
    struct Entry {
        QString name;
        QString number;
        QString label;   // "mobile" / "home" / "work" / ""
    };

    void loadContacts();          // fill m_all + m_nameByNumber
    void loadContactsFromHelper(); // root-helper fallback when QtContacts is blocked
    void loadRecents();           // fill m_recents
    void rebuildContactView();    // apply m_query -> m_contacts

    void connectManager();
    void trackActiveCall();       // pick + bind the current handler
    void clearCall();
    void refreshMuted();

    QList<Entry> m_all;                 // every contact number, name-sorted
    QHash<QString, QString> m_nameByNumber; // normalised digits -> name
    QString m_query;
    QVariantList m_contacts;            // filtered view handed to QML
    QVariantList m_recents;

#ifndef CARFISH_DESKTOP
    CommHistory::CallModel *m_callModel = nullptr; // live recent-call log (kept alive)
#endif
    QDBusInterface *m_mgr = nullptr;    // org.nemomobile.voicecall "/"
    QDBusInterface *m_call = nullptr;   // the active handler, if any
    QString m_callPath;
    QString m_callNumber;
    QString m_callName;
    int m_callStatus = 0;               // VoiceCallStatus enum
    int m_callDuration = 0;
    bool m_muted = false;
};
