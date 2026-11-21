#include "phonemanager.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QLocale>
#include <QLoggingCategory>
#include <algorithm>

#ifndef CARFISH_DESKTOP
#include <QContact>
#include <QContactDisplayLabel>
#include <QContactFetchHint>
#include <QContactFilter>
#include <QContactManager>
#include <QContactName>
#include <QContactPhoneNumber>
#include <QContactSortOrder>

#include <CommHistory/CallModel>
#include <CommHistory/Event>
#include <CommHistory/Recipient>

#include <QAbstractItemModel>

QTCONTACTS_USE_NAMESPACE
#endif

Q_LOGGING_CATEGORY(lcPhone, "aap.phone")

namespace {
const QString kService = QStringLiteral("org.nemomobile.voicecall");
const QString kMgrPath = QStringLiteral("/");
const QString kMgrIface = QStringLiteral("org.nemomobile.voicecall.VoiceCallManager");
const QString kCallIface = QStringLiteral("org.nemomobile.voicecall.VoiceCall");

// Reduce a dialled string to comparable digits (drop spaces/punctuation and the
// country/trunk prefix) so "+358 40 1234567" and "0401234567" match the same
// contact. Keep the last 9 digits.
QString normalise(const QString &number)
{
    QString d;
    for (QChar c : number)
        if (c.isDigit())
            d += c;
    if (d.size() > 9)
        d = d.right(9);
    return d;
}

QString initialsOf(const QString &name)
{
    QString out;
    const QStringList parts = name.split(QLatin1Char(' '), QString::SkipEmptyParts);
    for (const QString &p : parts) {
        out += p.at(0).toUpper();
        if (out.size() >= 2)
            break;
    }
    return out.isEmpty() ? QStringLiteral("#") : out;
}

QString whenText(const QDateTime &when)
{
    if (!when.isValid())
        return QString();
    const QDateTime now = QDateTime::currentDateTime();
    if (when.date() == now.date())
        return when.toString(QStringLiteral("HH:mm"));
    if (when.date().year() == now.date().year())
        return when.toString(QStringLiteral("d MMM"));
    return when.toString(QStringLiteral("d MMM yyyy"));
}
} // namespace

PhoneManager::PhoneManager(QObject *parent)
    : QObject(parent)
{
    connectManager();
    refresh();
}

void PhoneManager::connectManager()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_mgr = new QDBusInterface(kService, kMgrPath, kMgrIface, bus, this);
    if (!m_mgr->isValid()) {
        qCInfo(lcPhone) << "voicecall-manager not present:" << m_mgr->lastError().message();
        return;
    }
    connect(m_mgr, SIGNAL(voiceCallsChanged()), this, SLOT(onVoiceCallsChanged()));
    connect(m_mgr, SIGNAL(activeVoiceCallChanged()), this, SLOT(onVoiceCallsChanged()));
    connect(m_mgr, SIGNAL(microphoneMutedChanged()), this, SLOT(onCallStatusChanged()));
    trackActiveCall();
}

QString PhoneManager::callStatus() const
{
    switch (m_callStatus) {
    case 1: return tr("In call");
    case 2: return tr("On hold");
    case 3: return tr("Dialling");
    case 4: return tr("Calling");       // STATUS_ALERTING (ringing at the other end)
    case 5: return tr("Incoming call");
    case 6: return tr("Call waiting");
    default: return QString();
    }
}

QString PhoneManager::nameForNumber(const QString &number) const
{
    return m_nameByNumber.value(normalise(number));
}

void PhoneManager::refresh()
{
    loadContacts();
    loadRecents();
    rebuildContactView();
    emit recentsChanged();
}

void PhoneManager::setQuery(const QString &q)
{
    if (m_query == q)
        return;
    m_query = q;
    rebuildContactView();
}

void PhoneManager::rebuildContactView()
{
    const QString q = m_query.trimmed();
    const QString qDigits = normalise(q);
    m_contacts.clear();
    for (const Entry &e : m_all) {
        if (!q.isEmpty()) {
            const bool nameHit = e.name.contains(q, Qt::CaseInsensitive);
            const bool numHit = !qDigits.isEmpty() && normalise(e.number).contains(qDigits);
            if (!nameHit && !numHit)
                continue;
        }
        QVariantMap m;
        m[QStringLiteral("name")] = e.name;
        m[QStringLiteral("number")] = e.number;
        m[QStringLiteral("label")] = e.label;
        m[QStringLiteral("initials")] = initialsOf(e.name);
        m_contacts.append(m);
    }
    emit contactsChanged();
}

// --------------------------------------------------------------------- calls
void PhoneManager::dial(const QString &number)
{
    if (number.trimmed().isEmpty())
        return;
#ifdef CARFISH_DESKTOP
    // Host stub so the call UI can be exercised in the simulator.
    m_callPath = QStringLiteral("/mock");
    m_callNumber = number;
    m_callName = nameForNumber(number);
    m_callStatus = 3; // dialling
    m_callDuration = 0;
    emit callChanged();
    return;
#else
    if (!m_mgr || !m_mgr->isValid()) {
        // voicecall-manager may not have been up when we started; try again.
        delete m_mgr;
        m_mgr = nullptr;
        connectManager();
    }
    if (!m_mgr || !m_mgr->isValid()) {
        emit log(QStringLiteral("dial: voicecall-manager unavailable"));
        return;
    }
    // Each "providers" entry is "<providerId>:<providerType>" (e.g.
    // "telepathy-ring/tel/ril_0:tel"), but dial() wants the bare providerId.
    // Passing the whole entry made every call fail. Pick the alphabetically first
    // id, as the stock dialer (VoiceCallProviderModel) does.
    QStringList ids;
    const QStringList providers = m_mgr->property("providers").toStringList();
    for (const QString &p : providers) {
        const int colon = p.lastIndexOf(QLatin1Char(':'));
        const QString id = colon > 0 ? p.left(colon) : p;
        if (!id.isEmpty())
            ids << id;
    }
    ids.sort();
    if (ids.isEmpty()) {
        emit log(QStringLiteral("dial: no voice call provider (modem offline?) providers=[%1]")
                     .arg(providers.join(QStringLiteral(", "))));
        return;
    }
    const QString provider = ids.first();
    emit log(QStringLiteral("dial %1 via %2").arg(number, provider));
    QDBusPendingCall pc = m_mgr->asyncCall(QStringLiteral("dial"), provider, number);
    auto *w = new QDBusPendingCallWatcher(pc, this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *cw) {
        const QDBusPendingReply<bool> r = *cw;
        emit log(r.isError() ? QStringLiteral("dial failed: %1").arg(r.error().message())
                             : QStringLiteral("dial accepted=%1").arg(r.value() ? 1 : 0));
        cw->deleteLater();
    });
#endif
}

void PhoneManager::answer()
{
#ifdef CARFISH_DESKTOP
    m_callStatus = 1;
    emit callChanged();
#else
    if (m_call && m_call->isValid())
        m_call->asyncCall(QStringLiteral("answer"));
#endif
}

void PhoneManager::hangup()
{
#ifdef CARFISH_DESKTOP
    clearCall();
#else
    if (m_call && m_call->isValid())
        m_call->asyncCall(QStringLiteral("hangup"));
#endif
}

void PhoneManager::toggleMute()
{
#ifdef CARFISH_DESKTOP
    m_muted = !m_muted;
    emit callChanged();
#else
    if (!m_mgr || !m_mgr->isValid())
        return;
    m_mgr->asyncCall(QStringLiteral("setMuteMicrophone"), !m_muted);
    m_muted = !m_muted; // optimistic; corrected by microphoneMutedChanged
    emit callChanged();
#endif
}

void PhoneManager::sendDtmf(const QString &tone)
{
#ifndef CARFISH_DESKTOP
    if (m_call && m_call->isValid())
        m_call->asyncCall(QStringLiteral("sendDtmf"), tone);
#else
    Q_UNUSED(tone);
#endif
}

void PhoneManager::onVoiceCallsChanged()
{
    trackActiveCall();
}

void PhoneManager::onCallStatusChanged()
{
#ifndef CARFISH_DESKTOP
    if (m_mgr && m_mgr->isValid())
        m_muted = m_mgr->property("isMicrophoneMuted").toBool();
    if (m_call && m_call->isValid()) {
        m_callStatus = m_call->property("status").toInt();
        const QString line = m_call->property("lineId").toString();
        if (!line.isEmpty() && line != m_callNumber) {
            m_callNumber = line;
            m_callName = nameForNumber(line);
        }
    }
#endif
    emit callChanged();
}

void PhoneManager::onCallDurationChanged()
{
#ifndef CARFISH_DESKTOP
    if (m_call && m_call->isValid())
        m_callDuration = m_call->property("duration").toInt();
#endif
    emit callChanged();
}

void PhoneManager::trackActiveCall()
{
#ifndef CARFISH_DESKTOP
    if (!m_mgr || !m_mgr->isValid())
        return;
    const QStringList calls = m_mgr->property("voiceCalls").toStringList();
    if (calls.isEmpty()) {
        clearCall();
        return;
    }
    QString path = m_mgr->property("activeVoiceCall").toString();
    if (path.isEmpty() || !calls.contains(path))
        path = calls.first();
    if (path == m_callPath && m_call)
        return; // already bound to this handler

    delete m_call;
    m_callPath = path;
    m_call = new QDBusInterface(kService, path, kCallIface, QDBusConnection::sessionBus(), this);
    connect(m_call, SIGNAL(statusChanged(int, QString)), this, SLOT(onCallStatusChanged()));
    connect(m_call, SIGNAL(durationChanged(int)), this, SLOT(onCallDurationChanged()));
    connect(m_call, SIGNAL(lineIdChanged(QString)), this, SLOT(onCallStatusChanged()));

    m_callNumber = m_call->property("lineId").toString();
    m_callStatus = m_call->property("status").toInt();
    m_callDuration = m_call->property("duration").toInt();
    m_callName = nameForNumber(m_callNumber);
    m_muted = m_mgr->property("isMicrophoneMuted").toBool();
    emit callChanged();
#endif
}

void PhoneManager::clearCall()
{
    if (m_callPath.isEmpty())
        return;
    delete m_call;
    m_call = nullptr;
    m_callPath.clear();
    m_callNumber.clear();
    m_callName.clear();
    m_callStatus = 0;
    m_callDuration = 0;
    emit callChanged();
}

// ---------------------------------------------------------- contacts & recents
#ifndef CARFISH_DESKTOP
void PhoneManager::loadContacts()
{
    QContactManager mgr(QStringLiteral("org.nemomobile.contacts.sqlite"));

    QContactFetchHint hint;
    hint.setDetailTypesHint(QList<QContactDetail::DetailType>()
                            << QContactDetail::TypeDisplayLabel
                            << QContactDetail::TypeName
                            << QContactDetail::TypePhoneNumber);
    hint.setOptimizationHints(QContactFetchHint::NoRelationships
                              | QContactFetchHint::NoActionPreferences
                              | QContactFetchHint::NoBinaryBlobs);

    m_all.clear();
    m_nameByNumber.clear();

    const QList<QContact> list = mgr.contacts(QContactFilter(), QList<QContactSortOrder>(), hint);
    for (const QContact &c : list) {
        const QList<QContactPhoneNumber> phones = c.details<QContactPhoneNumber>();
        if (phones.isEmpty())
            continue;

        QString name = c.detail<QContactDisplayLabel>().label();
        if (name.isEmpty()) {
            const QContactName n = c.detail<QContactName>();
            name = (n.firstName() + QLatin1Char(' ') + n.lastName()).trimmed();
        }
        if (name.isEmpty())
            name = phones.first().number();

        for (const QContactPhoneNumber &p : phones) {
            if (p.number().isEmpty())
                continue;
            QString label;
            const QList<int> subs = p.subTypes();
            if (subs.contains(QContactPhoneNumber::SubTypeMobile))
                label = tr("mobile");
            else if (p.contexts().contains(QContactDetail::ContextWork))
                label = tr("work");
            else if (p.contexts().contains(QContactDetail::ContextHome))
                label = tr("home");
            m_all.append(Entry{name, p.number(), label});
            m_nameByNumber.insert(normalise(p.number()), name);
        }
    }

    // On a device where defaultuser isn't in the 'privileged' group, QtContacts
    // can't read the contacts DB (EACCES) and returns nothing. Fall back to the
    // root carfish-helper, which reads it on our behalf.
    if (m_all.isEmpty())
        loadContactsFromHelper();

    std::sort(m_all.begin(), m_all.end(), [](const Entry &a, const Entry &b) {
        const int c = QString::localeAwareCompare(a.name, b.name);
        return c != 0 ? c < 0 : a.number < b.number;
    });
    qCInfo(lcPhone) << "loaded" << m_all.size() << "contact numbers";
}

// Ask carfish-helper (root) for the contacts it read from the privileged DB.
// Each entry is "name\tnumber".
void PhoneManager::loadContactsFromHelper()
{
    QDBusInterface iface(QStringLiteral("org.carfish.Helper"),
                         QStringLiteral("/org/carfish/Contacts"),
                         QStringLiteral("org.carfish.Contacts1"),
                         QDBusConnection::systemBus());
    QDBusReply<QStringList> reply = iface.call(QStringLiteral("GetContacts"));
    if (!reply.isValid()) {
        qCWarning(lcPhone) << "contacts helper unavailable:" << reply.error().message();
        return;
    }
    const QStringList entries = reply.value();
    for (const QString &e : entries) {
        const int tab = e.indexOf(QLatin1Char('\t'));
        if (tab <= 0)
            continue;
        const QString name = e.left(tab);
        const QString number = e.mid(tab + 1);
        if (number.isEmpty())
            continue;
        m_all.append(Entry{name, number, QString()});
        m_nameByNumber.insert(normalise(number), name);
    }
    qCInfo(lcPhone) << "contacts via helper:" << entries.size();
}

void PhoneManager::loadRecents()
{
    // Keep the CallModel alive and let CommHistory resolve the caller names via
    // the shared contact cache (SeasideCache) — the same resolver the stock
    // dialer uses, so a name matches however the number is formatted. That
    // resolution is asynchronous (and ResolveImmediately is incompatible with
    // SyncQuery), so we rebuild the recents list whenever the model changes;
    // names then appear on their own as the resolver fills them in.
    if (!m_callModel) {
        m_callModel = new CommHistory::CallModel(this);
        m_callModel->setResolveContacts(CommHistory::EventModel::ResolveImmediately);
        m_callModel->setSorting(CommHistory::CallModel::SortByTime);
        connect(m_callModel, &QAbstractItemModel::modelReset, this, &PhoneManager::rebuildRecents);
        connect(m_callModel, &QAbstractItemModel::dataChanged, this, &PhoneManager::rebuildRecents);
        connect(m_callModel, &QAbstractItemModel::rowsInserted, this, &PhoneManager::rebuildRecents);
        connect(m_callModel, &QAbstractItemModel::rowsRemoved, this, &PhoneManager::rebuildRecents);
    }
    if (!m_callModel->getEvents())
        qCWarning(lcPhone) << "CallModel::getEvents() failed";
}

void PhoneManager::rebuildRecents()
{
    if (!m_callModel)
        return;
    m_recents.clear();
    int named = 0;
    const int n = qMin(m_callModel->rowCount(), 50);
    for (int i = 0; i < n; ++i) {
        const CommHistory::Event ev = m_callModel->event(i);
        const CommHistory::Recipient r = ev.recipients().value(0);
        QString number = r.remoteUid();
        if (number.isEmpty())
            number = ev.recipients().remoteUids().value(0);
        if (number.isEmpty())
            continue;
        // Prefer the resolved contact name (event, then recipient), then our own
        // contact map built from QtContacts, then fall back to the number.
        QString name = ev.contactName();
        if (name.isEmpty())
            name = r.contactName();
        if (name.isEmpty())
            name = nameForNumber(number);
        if (!name.isEmpty())
            ++named;

        QVariantMap m;
        m[QStringLiteral("number")] = number;
        m[QStringLiteral("name")] = name.isEmpty() ? number : name;
        m[QStringLiteral("known")] = !name.isEmpty();
        m[QStringLiteral("incoming")] = ev.direction() == CommHistory::Event::Inbound;
        m[QStringLiteral("missed")] = ev.isMissedCall();
        m[QStringLiteral("initials")] = initialsOf(name.isEmpty() ? number : name);
        m[QStringLiteral("whenText")] = whenText(ev.startTime());
        m_recents.append(m);
    }
    qCInfo(lcPhone) << "recents:" << m_recents.size() << "calls," << named << "named";
    emit recentsChanged();
}
#else  // CARFISH_DESKTOP — mock data so the car UI renders in the simulator
void PhoneManager::loadContacts()
{
    m_all = {
        {QStringLiteral("Alex Driver"), QStringLiteral("+358401112222"), tr("mobile")},
        {QStringLiteral("Home"), QStringLiteral("+358911234567"), tr("home")},
        {QStringLiteral("Workshop"), QStringLiteral("+358201234567"), tr("work")},
        {QStringLiteral("Sam Passenger"), QStringLiteral("+358407654321"), tr("mobile")},
    };
    m_nameByNumber.clear();
    for (const Entry &e : m_all)
        m_nameByNumber.insert(normalise(e.number), e.name);
}

void PhoneManager::loadRecents()
{
    const QDateTime now = QDateTime::currentDateTime();
    auto rec = [&](const QString &num, bool in, bool missed, int minsAgo) {
        const QString name = nameForNumber(num);
        QVariantMap m;
        m[QStringLiteral("number")] = num;
        m[QStringLiteral("name")] = name.isEmpty() ? num : name;
        m[QStringLiteral("known")] = !name.isEmpty();
        m[QStringLiteral("incoming")] = in;
        m[QStringLiteral("missed")] = missed;
        m[QStringLiteral("initials")] = initialsOf(name.isEmpty() ? num : name);
        m[QStringLiteral("whenText")] = whenText(now.addSecs(-60 * minsAgo));
        return m;
    };
    m_recents.clear();
    m_recents << rec(QStringLiteral("+358401112222"), false, false, 8);
    m_recents << rec(QStringLiteral("+358407654321"), true, true, 95);
    m_recents << rec(QStringLiteral("+358911234567"), true, false, 300);
    m_recents << rec(QStringLiteral("+358509998877"), false, false, 1500);
}
#endif
