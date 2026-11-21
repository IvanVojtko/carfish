#include "mpriscontroller.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcMpris, "aap.mpris")

static const QString kPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
static const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
static const QString kPlayerIface = QStringLiteral("org.mpris.MediaPlayer2.Player");
static const QString kTrackListIface = QStringLiteral("org.mpris.MediaPlayer2.TrackList");
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
    const QString prevTrackId = m_trackId;
    m_title = asString(meta.value(QStringLiteral("xesam:title")));
    m_artist = asStringList(meta.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", "));
    m_album = asString(meta.value(QStringLiteral("xesam:album")));
    m_artUrl = asString(meta.value(QStringLiteral("mpris:artUrl")));
    m_lengthUs = meta.value(QStringLiteral("mpris:length")).toLongLong();
    const QVariant tid = meta.value(QStringLiteral("mpris:trackid"));
    m_trackId = tid.canConvert<QDBusObjectPath>() ? tid.value<QDBusObjectPath>().path() : tid.toString();

    // Refresh the position immediately and keep it ticking only while playing.
    pollPosition();
    if (m_playing && !m_posTimer.isActive())
        m_posTimer.start();
    else if (!m_playing && m_posTimer.isActive())
        m_posTimer.stop();
    emit changed();

    // When the list view is open, re-fetch the queue on a track change: this
    // bridge slides the current song forward, so the rows (and which one is
    // "current") need rebuilding as playback advances.
    if (m_wantQueue && m_trackId != prevTrackId)
        refreshQueue();
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
        m_title = m_artist = m_album = m_artUrl = m_trackId = QString();
        m_positionUs = m_lengthUs = 0;
        m_posTimer.stop();
        emit changed();
        if (!m_queue.isEmpty()) {
            m_queue.clear();
            emit queueChanged();
        }
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

// Read the optional MPRIS TrackList interface: the Tracks property is an array
// of object paths; GetTracksMetadata returns their metadata (a{sv} per track).
// The Sailfish Android MPRIS bridge (mprisam) implements this, so an Android
// player's play queue — e.g. Spotify's — can be listed and jumped into.
void MprisController::refreshQueue()
{
    m_wantQueue = true;
    QDBusConnection bus = QDBusConnection::sessionBus();
    auto clearQueue = [this] {
        if (!m_queue.isEmpty()) {
            m_queue.clear();
            emit queueChanged();
        }
    };
    if (m_service.isEmpty()) {
        clearQueue();
        return;
    }

    // 1) TrackList.Tracks (ao). Absent interface -> error reply -> empty queue.
    QDBusMessage gm = QDBusMessage::createMethodCall(m_service, kPath, kPropsIface, QStringLiteral("Get"));
    gm << kTrackListIface << QStringLiteral("Tracks");
    const QDBusMessage gr = bus.call(gm);
    if (gr.type() != QDBusMessage::ReplyMessage || gr.arguments().isEmpty()) {
        qCInfo(lcMpris) << "player has no TrackList interface";
        clearQueue();
        return;
    }
    QList<QDBusObjectPath> paths;
    const QVariant tv = gr.arguments().first().value<QDBusVariant>().variant();
    if (tv.canConvert<QDBusArgument>()) {
        QDBusArgument arg = tv.value<QDBusArgument>();
        arg >> paths;
    }
    if (paths.isEmpty()) {
        clearQueue();
        return;
    }

    // 2) TrackList.GetTracksMetadata(ao) -> aa{sv}.
    QDBusMessage mm = QDBusMessage::createMethodCall(m_service, kPath, kTrackListIface,
                                                     QStringLiteral("GetTracksMetadata"));
    mm << QVariant::fromValue(paths);
    const QDBusMessage mr = bus.call(mm);
    if (mr.type() != QDBusMessage::ReplyMessage || mr.arguments().isEmpty())
        return;

    QVariantList out;
    const QVariant mv = mr.arguments().first();
    if (mv.canConvert<QDBusArgument>()) {
        QDBusArgument arg = mv.value<QDBusArgument>();
        arg.beginArray();
        int i = 0;
        while (!arg.atEnd()) {
            QVariantMap meta;
            arg >> meta; // a{sv}
            const QString id = (i < paths.size()) ? paths.at(i).path() : QString();
            QVariantMap item;
            item[QStringLiteral("id")] = id;
            item[QStringLiteral("title")] = asString(meta.value(QStringLiteral("xesam:title")));
            item[QStringLiteral("artist")] = asStringList(meta.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", "));
            item[QStringLiteral("album")] = asString(meta.value(QStringLiteral("xesam:album")));
            // Highlight the row that is playing now. The bridge's per-track
            // mpris:trackid may differ from the Tracks path, so match on title+
            // artist as well as id.
            const QString mid = [&] {
                const QVariant t = meta.value(QStringLiteral("mpris:trackid"));
                return t.canConvert<QDBusObjectPath>() ? t.value<QDBusObjectPath>().path() : t.toString();
            }();
            const bool current = (!m_trackId.isEmpty() && (mid == m_trackId || id == m_trackId))
                || (item[QStringLiteral("title")].toString() == m_title && !m_title.isEmpty()
                    && item[QStringLiteral("artist")].toString() == m_artist);
            item[QStringLiteral("current")] = current;
            out << item;
            ++i;
        }
        arg.endArray();
    }
    m_queue = out;
    qCInfo(lcMpris) << "queue:" << m_queue.size() << "tracks";
    emit queueChanged();
}

void MprisController::goToTrack(const QString &id)
{
    if (m_service.isEmpty() || id.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, kPath, kTrackListIface, QStringLiteral("GoTo"));
    msg << QVariant::fromValue(QDBusObjectPath(id));
    QDBusConnection::sessionBus().asyncCall(msg);
    qCInfo(lcMpris) << "GoTo" << id << "->" << m_service;
    // The bridge updates the current track + slides the queue shortly after.
    QTimer::singleShot(400, this, [this] {
        readState();
        if (m_wantQueue)
            refreshQueue();
    });
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

QString MprisController::playerName() const
{
    QString n = m_service;
    n.remove(QStringLiteral("org.mpris.MediaPlayer2."));
    const int inst = n.indexOf(QLatin1String(".instance"));
    if (inst > 0)
        n.truncate(inst);
    return n;
}

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
