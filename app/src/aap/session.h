#pragma once

#include "cryptor.h"
#include "framer.h"

#include <QHash>
#include <QSet>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <string>

class AudioSource;
class CarScreen;
class MicReceiver;
class NavBridge;
class Transport;
class VideoSource;

namespace aap {

// One Android Auto session from the phone's point of view:
//   HU  VERSION_REQUEST        -> phone VERSION_RESPONSE (1.7)
//   HU  SSL records (client)   <-> phone SSL records (server)
//   HU  AUTH_COMPLETE          -> encryption on, phone SERVICE_DISCOVERY_REQUEST
//   HU  SERVICE_DISCOVERY_RESP -> phone opens the channels it uses
//   video: SETUP -> CONFIG -> VIDEO_FOCUS_REQUEST -> NOTIFICATION -> START,
//          CODEC_CONFIG, DATA... <- ACK
class Session : public QObject
{
    Q_OBJECT
public:
    struct Config {
        QString certPath;
        QString keyPath;
        QUrl carQml;
        QString deviceName = QStringLiteral("Sailfish OS");
        QString label = QStringLiteral("Carfish");
        // PulseAudio source to send to the car as media audio. Must be the
        // MONITOR of the playback sink (what's playing), NOT the default source
        // (the microphone). @DEFAULT_MONITOR@ is the monitor of the default sink.
        QString audioDevice = QStringLiteral("@DEFAULT_MONITOR@");
        bool mediaAudio = true;
    };

    Session(Transport *transport, CarScreen *screen, const Config &config, QObject *parent = nullptr);
    ~Session() override;

    void start();
    void shutdown();

    // Report the current call to the head unit so it shows the in-AA call UI.
    // state is a PhoneStatus::State value; number/name identify the caller; a
    // non-call state (INACTIVE) with empty strings clears it. No-op until the HU
    // has advertised + opened its phone-status channel.
    void updateCall(int state, const QString &number, const QString &name);

    // Now playing for the car's own screens (cluster / native media display),
    // when the HU advertises MediaPlaybackStatusService. state is a
    // MediaPlaybackStatus::State value; art is a PNG of at most 256x256 (or
    // empty). Values are cached and replayed once the channel opens.
    void updateMediaStatus(int state, const QString &source, int positionSec);
    void updateMediaMetadata(const QString &title, const QString &artist, const QString &album,
                             const QByteArray &art, int durationSec);

signals:
    void stateChanged(const QString &state);
    void logMessage(const QString &line);
    void finished(const QString &reason);
    // Semantic head-unit button presses, for the controller to route to apps.
    void voiceRequested(bool active);   // steering-wheel voice / push-to-talk
    void mediaKey(const QString &action);  // play, pause, next, prev, ff, rew
    void navKey(const QString &action);    // back, home, menu, up/down/left/right, rotary±, select
    // Answer/reject taps from the head unit's in-AA call UI (and CALL/ENDCALL
    // wheel buttons), for the controller to drive the phone with.
    void phoneAnswer();
    void phoneReject();
    void phoneChannelReady(); // the HU opened its phone-status channel; push current call
    // Car fuel/energy level (from the FUEL sensor). For an EV, levelPct is the
    // battery state-of-charge and rangeKm the remaining range; electric says so.
    void energyStatus(int levelPct, int rangeKm, bool electric, bool low);

private:
    enum class Phase { WaitVersion, Handshake, WaitAuth, Discovery, Running, Closed };

    enum class Kind { Video, MediaAudio, Input, Sensor, Mic, Nav, PhoneStatus, MediaStatus };
    struct VideoMode {
        QSize size;
        QRect contentRect; // visible part after the HU crops its margins
        int fps = 30;
        int density = 160;
    };
    struct ChannelInfo {
        Kind kind;
        bool open = false;
        // media sinks
        int maxUnacked = 1;
        int unacked = 0;
        int sessionId = 0;
        bool started = false;
        // video
        QVector<VideoMode> videoModes; // index == HU configuration index
        int configIndex = 0;
        QSize videoSize;
        QRect contentRect;
        int fps = 30;
        QSize touchSize;
        QVector<int> huKeycodes; // keycodes_supported the HU advertised for this input
        // audio
        int sampleRate = 48000;
        int channels = 2;
    };

    void onData(const QByteArray &data);
    void onMessage(const Message &msg);
    void onControl(const Message &msg);
    void onChannel(ChannelInfo &ch, quint8 channel, const Message &msg);
    void onServiceDiscovery(const QByteArray &body);
    void onChannelOpened(ChannelInfo &ch, quint8 channel);
    void onMediaConfig(ChannelInfo &ch, quint8 channel, const QByteArray &body);
    void onVideoFocus(ChannelInfo &ch, quint8 channel, const QByteArray &body);
    void onInput(const ChannelInfo &ch, const QByteArray &body);
    void onPhoneStatusInput(const QByteArray &body);
    void onMediaPlaybackInput(const QByteArray &body);
    void sendMediaStatus();
    void sendMediaMetadata();
    void onKeyEvent(const QByteArray &body);
    void dispatchKey(quint32 keycode, bool down, bool longpress);
    void dispatchRelative(quint32 keycode, int delta); // rotary/knob/scroll steps
    void onSensorBatch(const QByteArray &body);
    void onAudioFocus(const QByteArray &body);

    // Microphone (car mic -> phone). Opened on demand (e.g. a voice key).
    void requestMicrophone(bool on);
    void onMicResponse(ChannelInfo &ch, const QByteArray &body);
    void onMicData(ChannelInfo &ch, quint8 channel, const Message &msg);

    // Instrument-cluster turn-by-turn (from the map UI via NavBridge).
    void onNavUpdated();
    void sendNavStatus(int status);   // NavigationStatus (ACTIVE/INACTIVE/REROUTING)
    void sendNavClear();              // wipe the cluster turn card when guidance ends
    void sendNavGuidance();           // STATE + CURRENT_POSITION, or the legacy pair
    void sendEnergyForecast();        // EV battery forecast (0x8008), when supported
    void flushNavGuidance();          // rate-limited push of the newest guidance
    // Gearhead's head-unit gates (CarInfo major/minor = the HU's protocol version):
    bool navModernHu() const { return m_huMajor > 1 || (m_huMajor == 1 && m_huMinor >= 6); }
    bool energyForecastHu() const { return m_huMajor > 5 || (m_huMajor == 5 && m_huMinor > 0); }

    void startVideo(ChannelInfo &ch, quint8 channel);
    void stopVideo();
    void sendVideoConfig(const QByteArray &spsPps);
    void sendVideoFrame(const QByteArray &au, quint64 timestampUs, bool keyFrame);
    void sendAudio(const QByteArray &pcm, quint64 timestampUs);

    void send(quint8 channel, quint16 id, const QByteArray &body, bool control = false);
    void send(quint8 channel, quint16 id, const std::string &body, bool control = false);
    void sendMedia(quint8 channel, quint16 id, quint64 timestampUs, const QByteArray &payload, bool withTimestamp);
    void fail(const QString &reason);
    void log(const QString &line);

    Transport *m_transport;
    CarScreen *m_screen;
    Config m_config;
    Framer m_framer;
    Cryptor m_cryptor;
    Phase m_phase = Phase::WaitVersion;
    int m_restarts = 0;          // head-unit-initiated handshake restarts
    bool m_encrypt = false;
    int m_huMajor = 0;
    int m_huMinor = 0;
    QHash<quint8, ChannelInfo> m_channels;
    VideoSource *m_video;
    AudioSource *m_audio;
    MicReceiver *m_mic;
    int m_videoChannel = -1;
    int m_mediaAudioChannel = -1;
    int m_micChannel = -1;
    int m_navChannel = -1;
    int m_phoneStatusChannel = -1;
    // Media playback status (now playing on the car's own screens).
    int m_mediaStatusChannel = -1;
    int m_mpState = 0;
    QString m_mpSource;
    int m_mpPosition = 0;
    QString m_mpTitle, m_mpArtist, m_mpAlbum;
    QByteArray m_mpArt;
    int m_mpDuration = 0;
    bool m_mpMetaLogged = false;
    QSet<int> m_sensorsAvailable; // sensor types the head unit advertised
    bool m_evCar = false;          // car advertised FUEL_TYPE_ELECTRIC
    int m_navMinIntervalMs = 1000; // rate floor the head unit advertises
    bool m_navActive = false;      // STATUS(ACTIVE) already sent
    bool m_navClusterReady = false;
    // Touch source resolution per axis, latched once per session (F-Auto
    // TouchMapper): the declared panel, or the visible area when the panel is a
    // larger same-aspect rectangle (e.g. Ford SYNC 4 reports visible-area pixels).
    int m_touchSrcX = 0;
    int m_touchSrcY = 0;// cluster channel open / focus granted
    qint64 m_lastNavSendMs = 0;
    QTimer m_navFlush;             // trailing send when an update hits the rate floor
    bool m_navRerouting = false;   // STATUS(REROUTING) sent, waiting for the new route
    bool m_navWantsImages = false; // cluster type IMAGE: legacy turn events carry a PNG
    int m_navImgW = 0, m_navImgH = 0;
    int m_fuelPct = -1;            // last FUEL sensor reading (EV: state of charge %)
    int m_fuelRangeKm = -1;        //   and remaining range
    bool m_forecastLogged = false;     // "why no forecast" logged once
    bool m_forecastSentLogged = false; // "sending forecast" logged once
    NavBridge *m_nav = nullptr;    // source of turn-by-turn from the map UI
    bool m_micOn = false;
    int m_nextSessionId = 1;
    int m_droppedFrames = 0;
    bool m_keyFramePending = false;
};

} // namespace aap
