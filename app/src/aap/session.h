#pragma once

#include "cryptor.h"
#include "framer.h"

#include <QHash>
#include <QObject>
#include <QRect>
#include <QSize>
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

signals:
    void stateChanged(const QString &state);
    void logMessage(const QString &line);
    void finished(const QString &reason);
    // Semantic head-unit button presses, for the controller to route to apps.
    void voiceRequested(bool active);   // steering-wheel voice / push-to-talk
    void mediaKey(const QString &action);  // play, pause, next, prev, ff, rew
    void navKey(const QString &action);    // back, home, menu, up/down/left/right, rotary±, select

private:
    enum class Phase { WaitVersion, Handshake, WaitAuth, Discovery, Running, Closed };

    enum class Kind { Video, MediaAudio, Input, Sensor, Mic, Nav };
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
    void onKeyEvent(const QByteArray &body);
    void dispatchKey(quint32 keycode, bool down, bool longpress);
    void onSensorBatch(const QByteArray &body);
    void onAudioFocus(const QByteArray &body);

    // Microphone (car mic -> phone). Opened on demand (e.g. a voice key).
    void requestMicrophone(bool on);
    void onMicResponse(ChannelInfo &ch, const QByteArray &body);
    void onMicData(ChannelInfo &ch, quint8 channel, const Message &msg);

    // Instrument-cluster turn-by-turn (from the map UI via NavBridge).
    void onNavUpdated();
    void sendNavStatus(int status);   // NavigationStatus (ACTIVE/INACTIVE/REROUTING)
    void sendNavGuidance();           // STATE + CURRENT_POSITION + legacy events

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
    int m_navMinIntervalMs = 1000; // rate floor the head unit advertises
    bool m_navActive = false;      // STATUS(ACTIVE) already sent
    bool m_navClusterReady = false;
    // Touch source resolution per axis, latched once per session (F-Auto
    // TouchMapper): the declared panel, or the visible area when the panel is a
    // larger same-aspect rectangle (e.g. Ford SYNC 4 reports visible-area pixels).
    int m_touchSrcX = 0;
    int m_touchSrcY = 0;// cluster channel open / focus granted
    qint64 m_lastNavSendMs = 0;
    NavBridge *m_nav = nullptr;    // source of turn-by-turn from the map UI
    bool m_micOn = false;
    int m_nextSessionId = 1;
    int m_droppedFrames = 0;
    bool m_keyFramePending = false;
};

} // namespace aap
