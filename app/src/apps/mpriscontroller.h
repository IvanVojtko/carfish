#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

// Watches the session bus for MPRIS media players (org.mpris.MediaPlayer2.*),
// tracks the currently active one's metadata and playback state, and forwards
// transport commands to it. This is the media backend for the car UI: the
// now-playing pane binds to these properties and steering-wheel / on-screen
// controls call the invokable methods. Works with any MPRIS player (native
// Sailfish apps, or Android apps bridged onto the bus).
class MprisController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasPlayer READ hasPlayer NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(QString album READ album NOTIFY changed)
    Q_PROPERTY(QString artUrl READ artUrl NOTIFY changed)
    // Playback position for the now-playing progress bar. position/duration are
    // in seconds; progress is 0..1 (0 when the player reports no length).
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)
    // The current play queue / track list, when the player exposes the optional
    // MPRIS TrackList interface (the Sailfish Android bridge does, so Spotify's
    // queue is listable). Each entry: { id, title, artist, album, current }.
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(bool hasQueue READ hasQueue NOTIFY queueChanged)
public:
    explicit MprisController(QObject *parent = nullptr);

    bool hasPlayer() const { return !m_service.isEmpty(); }
    bool playing() const { return m_playing; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    QString artUrl() const { return m_artUrl; }
    // Short name of the active player ("spotify" from org.mpris.MediaPlayer2.spotify).
    QString playerName() const;
    double position() const { return double(m_positionUs) / 1e6; }
    double duration() const { return double(m_lengthUs) / 1e6; }
    double progress() const { return m_lengthUs > 0 ? double(m_positionUs) / double(m_lengthUs) : 0.0; }
    QVariantList queue() const { return m_queue; }
    bool hasQueue() const { return !m_queue.isEmpty(); }

    // Fetch the current track list from the player's TrackList interface (keeps
    // it refreshed afterwards while the list view is open).
    Q_INVOKABLE void refreshQueue();
    // Jump to a track by its TrackList id (the object path from refreshQueue).
    Q_INVOKABLE void goToTrack(const QString &id);

    Q_INVOKABLE void playPause();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void stop();

    // Route a semantic steering-wheel media action ("play-pause", "next", ...).
    void handleMediaKey(const QString &action);

    // Re-scan the bus and (re)choose the active player.
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void positionChanged();
    void queueChanged();

private slots:
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);

private:
    QStringList players() const;             // current org.mpris.MediaPlayer2.* names
    void selectPlayer(const QString &service);
    void call(const QString &method);
    void readState();                        // pull Metadata + PlaybackStatus + length
    void pollPosition();                     // poll the player's Position (not in PropertiesChanged)
    void checkState();                       // async: re-read if status/track changed unannounced
    void confirmSoon();                      // re-check a few times after a transport command
    void setPlayerConnected(const QString &service, bool on);

    QString m_service;        // the chosen player's bus name, empty if none
    bool m_playing = false;
    QString m_title, m_artist, m_album, m_artUrl;
    qint64 m_positionUs = 0;  // current position, microseconds
    qint64 m_lengthUs = 0;    // track length (mpris:length), microseconds
    QTimer m_posTimer;        // ticks Position while playing
    QTimer m_stateTimer;      // safety net for players that skip PropertiesChanged
    QString m_trackId;        // mpris:trackid of the current track (queue highlight)
    QVariantList m_queue;     // current TrackList entries
    bool m_wantQueue = false; // keep the queue refreshed after the user opened it
};
