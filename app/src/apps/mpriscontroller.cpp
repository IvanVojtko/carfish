#include "mpriscontroller.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcMpris, "aap.mpris")

static const QString kPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
static const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
static const QString kPlayerIface = QStringLiteral("org.mpris.MediaPlayer2.Player");
static const QString kPropsIface = QStringLiteral("org.freedesktop.DBus.Properties");

MprisController::MprisController(QObject *parent)
    : QObject(parent)
{
    // Watch *every* service appearing/leaving and filter by the MPRIS prefix.
    // (QDBusServiceWatcher's wildcard matching is unreliable for arbitrary new
    // player names, so subscribe to the raw NameOwnerChanged signal instead.)
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
        this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    // MPRIS Position is not delivered via PropertiesChanged, so poll it while
    // something is playing to drive the now-playing progress bar.
    m_posTimer.setInterval(1000);
    connect(&m_posTimer, &QTimer::timeout, this, &MprisController::pollPosition);
    refresh();
}

void MprisController::pollPosition()
{
    if (m_service.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, kPath, kPropsIface, QStringLiteral("Get"));
    msg << kPlayerIface << QStringLiteral("Position");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        m_positionUs = reply.arguments().first().value<QDBusVariant>().variant().toLongLong();
        emit positionChanged();
    }
}

QStringList MprisController::players() const
{
    QStringList result;
    QDBusReply<QStringList> names = QDBusConnection::sessionBus().interface()->registeredServiceNames();
    if (names.isValid()) {
        for (const QString &n : names.value())
            if (n.startsWith(kPrefix))
                result << n;
    }
    return result;
}

// Demarshal a metadata value that may be wrapped in a QDBusArgument.
static QString asString(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>())
        return QString();
    return v.toString();
}

static QStringList asStringList(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>()) {
        QStringList list;
        QDBusArgument arg = v.value<QDBusArgument>();
        arg >> list;
        return list;
    }
    return v.toStringList();
}

void MprisController::readState()
{
    if (m_service.isEmpty())
        return;
    auto get = [this](const QString &prop) -> QVariant {
        QDBusMessage msg = QDBusMessage::createMethodCall(m_service, kPath, kPropsIface, QStringLiteral("Get"));
        msg << kPlayerIface << prop;
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return QVariant();
        return reply.arguments().first().value<QDBusVariant>().variant();
    };

    const QString status = get(QStringLiteral("PlaybackStatus")).toString();
    m_playing = status == QLatin1String("Playing");

    QVariantMap meta;
    const QVariant mv = get(QStringLiteral("Metadata"));
    if (mv.canConvert<QDBusArgument>()) {
        QDBusArgument arg = mv.value<QDBusArgument>();
        arg >> meta;
    } else {
        meta = mv.toMap();
    }
    m_title = asString(meta.value(QStringLiteral("xesam:title")));
    m_artist = asStringList(meta.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", "));
    m_album = asString(meta.value(QStringLiteral("xesam:album")));
    m_artUrl = asString(meta.value(QStringLiteral("mpris:artUrl")));
    m_lengthUs = meta.value(QStringLiteral("mpris:length")).toLongLong();

    // Refresh the position immediately and keep it ticking only while playing.
    pollPosition();
    if (m_playing && !m_posTimer.isActive())
        m_posTimer.start();
    else if (!m_playing && m_posTimer.isActive())
        m_posTimer.stop();
    emit changed();
}

void MprisController::setPlayerConnected(const QString &service, bool on)
{
    if (service.isEmpty())
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (on) {
        bus.connect(service, kPath, kPropsIface, QStringLiteral("PropertiesChanged"),
                    this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    } else {
        bus.disconnect(service, kPath, kPropsIface, QStringLiteral("PropertiesChanged"),
                       this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }
}

void MprisController::selectPlayer(const QString &service)
{
    if (service == m_service)
        return;
    if (!m_service.isEmpty())
        setPlayerConnected(m_service, false);
    m_service = service;
    if (!m_service.isEmpty()) {
        setPlayerConnected(m_service, true);
        qCInfo(lcMpris) << "active player:" << m_service;
        readState();
    } else {
        m_playing = false;
        m_title = m_artist = m_album = m_artUrl = QString();
        m_positionUs = m_lengthUs = 0;
        m_posTimer.stop();
        emit changed();
    }
}

void MprisController::refresh()
{
    const QStringList list = players();
    if (list.isEmpty()) {
        selectPlayer(QString());
        return;
    }
    // Prefer a player that is currently Playing; else keep the current one if
    // still present; else the first available.
    QString playingSvc;
    for (const QString &svc : list) {
        QDBusMessage msg = QDBusMessage::createMethodCall(svc, kPath, kPropsIface, QStringLiteral("Get"));
        msg << kPlayerIface << QStringLiteral("PlaybackStatus");
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
                && reply.arguments().first().value<QDBusVariant>().variant().toString()
                   == QLatin1String("Playing")) {
            playingSvc = svc;
            break;
        }
    }
    if (!playingSvc.isEmpty())
        selectPlayer(playingSvc);
    else if (list.contains(m_service))
        readState();
    else
        selectPlayer(list.first());
}

void MprisController::onPropertiesChanged(const QString &iface, const QVariantMap &, const QStringList &)
{
    if (iface == kPlayerIface)
        readState();
}

void MprisController::onNameOwnerChanged(const QString &name, const QString &, const QString &newOwner)
{
    if (!name.startsWith(kPrefix))
        return;
    if (newOwner.isEmpty() && name == m_service)
        selectPlayer(QString()); // current player went away
    refresh();
}

void MprisController::call(const QString &method)
{
    if (m_service.isEmpty()) {
        qCInfo(lcMpris) << "no player for" << method;
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, kPath, kPlayerIface, method);
    QDBusConnection::sessionBus().asyncCall(msg);
    qCInfo(lcMpris) << method << "->" << m_service;
}

// The Sailfish App Support MPRIS bridge honours transport commands (verified on
// device: PlayPause toggles PlaybackStatus, Next advances the track id), so
// Android players (e.g. Spotify) are driven over MPRIS exactly like native ones.
// No aliendalvik-control keyevent detour — that package isn't even installed.
void MprisController::playPause() { call(QStringLiteral("PlayPause")); }
void MprisController::play() { call(QStringLiteral("Play")); }
void MprisController::pause() { call(QStringLiteral("Pause")); }
void MprisController::next() { call(QStringLiteral("Next")); }
void MprisController::previous() { call(QStringLiteral("Previous")); }
void MprisController::stop() { call(QStringLiteral("Stop")); }

void MprisController::handleMediaKey(const QString &action)
{
    if (action.startsWith(QLatin1String("play-pause")))
        playPause();
    else if (action == QLatin1String("play"))
        play();
    else if (action == QLatin1String("pause"))
        pause();
    else if (action == QLatin1String("next"))
        next();
    else if (action == QLatin1String("prev"))
        previous();
    else if (action == QLatin1String("stop"))
        stop();
    // ff / rew: no standard MPRIS verb; could Seek, left for later.
}
