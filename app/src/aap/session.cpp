#include "session.h"

#include "aap.pb.h"
#include "media/audiosource.h"
#include "media/carscreen.h"
#include "media/micreceiver.h"
#include "media/navbridge.h"
#include "media/videosource.h"
#include "transport/transport.h"

#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QStringList>

#include <QLoggingCategory>
#include <QtEndian>

#include <time.h>

Q_LOGGING_CATEGORY(lcAap, "aap.session")

namespace aap {

// The phone answers any head unit with protocol 1.7.
static constexpr quint16 kVersionMajor = 1;
static constexpr quint16 kVersionMinor = 7;

static quint64 monotonicUs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return quint64(ts.tv_sec) * 1000000u + quint64(ts.tv_nsec) / 1000u;
}

static QSize resolutionSize(int res)
{
    switch (res) {
    case RES_800x480: return QSize(800, 480);
    case RES_1280x720: return QSize(1280, 720);
    case RES_1920x1080: return QSize(1920, 1080);
    case RES_2560x1440: return QSize(2560, 1440);
    case RES_3840x2160: return QSize(3840, 2160);
    case RES_720x1280: return QSize(720, 1280);
    case RES_1080x1920: return QSize(1080, 1920);
    case RES_1440x2560: return QSize(1440, 2560);
    case RES_2160x3840: return QSize(2160, 3840);
    default: return QSize();
    }
}

template <typename T>
static bool parse(T &msg, const QByteArray &body)
{
    return msg.ParseFromArray(body.constData(), body.size());
}

Session::Session(Transport *transport, CarScreen *screen, const Config &config, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
    , m_screen(screen)
    , m_config(config)
    , m_video(new VideoSource(this))
    , m_audio(new AudioSource(this))
    , m_mic(new MicReceiver(this))
{
    m_framer.setEncrypt([this](const QByteArray &in, QByteArray &out) { return m_cryptor.encrypt(in, out); });
    m_framer.setDecrypt([this](const QByteArray &in, QByteArray &out) { return m_cryptor.decrypt(in, out); });

    connect(m_transport, &Transport::dataReceived, this, &Session::onData);
    connect(m_transport, &Transport::connected, this, [this] {
        log(tr("transport connected, waiting for head unit's first AAP message"));
        emit stateChanged(tr("Connected, waiting for head unit"));
    });
    connect(m_transport, &Transport::disconnected, this, [this](const QString &reason) {
        fail(tr("transport closed: %1").arg(reason));
    });

    connect(m_screen, &CarScreen::frameReady, m_video, &VideoSource::pushFrame);
    connect(m_video, &VideoSource::codecConfig, this, &Session::sendVideoConfig);
    connect(m_video, &VideoSource::frameEncoded, this, &Session::sendVideoFrame);
    connect(m_video, &VideoSource::pipelineError, this, [this](const QString &e) {
        log(tr("video pipeline error: %1").arg(e));
    });
    connect(m_video, &VideoSource::pipelineInfo, this, [this](const QString &i) {
        log(tr("video: %1").arg(i));
    });
    connect(m_audio, &AudioSource::pcm, this, &Session::sendAudio);
    connect(m_audio, &AudioSource::captureChanged, this,
            [this](const QString &d) { log(tr("media audio: now capturing %1").arg(d)); });

    // Turn-by-turn from the projected map UI -> the car's instrument cluster.
    if (m_screen) {
        m_nav = qobject_cast<NavBridge *>(m_screen->navBridge());
        if (m_nav)
            connect(m_nav, &NavBridge::updated, this, &Session::onNavUpdated);
    }
}

Session::~Session()
{
    stopVideo();
    m_audio->stop();
    m_mic->end();
}

void Session::start()
{
    QString error;
    if (!m_cryptor.init(Cryptor::Role::Server, m_config.certPath, m_config.keyPath, &error)) {
        fail(error);
        return;
    }
    m_transport->start();
}

void Session::shutdown()
{
    if (m_phase == Phase::Running) {
        ByeByeRequest bye;
        bye.set_reason(1); // USER_SELECTION
        send(0, MSG_BYEBYE_REQUEST, bye.SerializeAsString());
    }
    stopVideo();
    m_audio->stop();
    m_mic->end();
    m_phase = Phase::Closed;
}

void Session::log(const QString &line)
{
    qCInfo(lcAap).noquote() << line;
    emit logMessage(line);
}

void Session::fail(const QString &reason)
{
    if (m_phase == Phase::Closed)
        return;
    log(QStringLiteral("session failed: ") + reason);
    stopVideo();
    m_audio->stop();
    m_mic->end();
    m_phase = Phase::Closed;
    emit finished(reason);
}

// ------------------------------------------------------------------ sending

void Session::send(quint8 channel, quint16 id, const QByteArray &body, bool control)
{
    Message m;
    m.channel = channel;
    m.control = control;
    m.encrypted = m_encrypt;
    m.payload = makePayload(id, body);
    const QByteArray wire = m_framer.encode(m);
    if (wire.isEmpty()) {
        fail(QStringLiteral("cannot encode message %1 on channel %2").arg(id).arg(channel));
        return;
    }
    m_transport->write(wire);
}

void Session::send(quint8 channel, quint16 id, const std::string &body, bool control)
{
    send(channel, id, QByteArray(body.data(), int(body.size())), control);
}

void Session::sendMedia(quint8 channel, quint16 id, quint64 timestampUs, const QByteArray &payload, bool withTimestamp)
{
    QByteArray body;
    body.reserve(payload.size() + 8);
    if (withTimestamp) {
        char ts[8];
        qToBigEndian<quint64>(timestampUs, reinterpret_cast<uchar *>(ts));
        body.append(ts, 8);
    }
    body.append(payload);
    send(channel, id, body);
}

// ---------------------------------------------------------------- receiving

void Session::onData(const QByteArray &data)
{
    // Durable raw-transport trace, but only until the session is running: once
    // projecting this floods the log with every encrypted frame. During the
    // handshake it distinguishes "head unit silent" from "unparseable bytes".
    if (m_phase != Phase::Running)
        log(tr("transport rx %1 B: %2").arg(data.size())
                .arg(QString::fromLatin1(data.left(16).toHex())));
    if (!m_framer.feed(data, [this](const Message &m) { onMessage(m); }))
        fail(QStringLiteral("framing error"));
}

void Session::onMessage(const Message &msg)
{
    if (m_phase == Phase::Closed)
        return;
    if (msg.channel == 0) {
        onControl(msg);
        return;
    }
    auto it = m_channels.find(msg.channel);
    if (it == m_channels.end()) {
        log(tr("rx id %1 on unopened channel %2 (%3 B)").arg(msg.id()).arg(msg.channel).arg(msg.body().size()));
        return;
    }
    onChannel(it.value(), msg.channel, msg);
}

void Session::onControl(const Message &msg)
{
    const QByteArray body = msg.body();
    // Durable trace of the control handshake so a real head unit's AAP sequence
    // is visible on-device (qCInfo is suppressed). Helps confirm message order
    // and direction against the real car.
    log(tr("ctrl rx id %1 (%2 B)").arg(msg.id()).arg(body.size()));
    switch (msg.id()) {
    case MSG_VERSION_REQUEST: {
        if (body.size() >= 4) {
            const auto *p = reinterpret_cast<const uchar *>(body.constData());
            m_huMajor = qFromBigEndian<quint16>(p);
            m_huMinor = qFromBigEndian<quint16>(p + 2);
        }
        log(tr("Head unit protocol %1.%2").arg(m_huMajor).arg(m_huMinor));
        // Answer like a genuine phone: 1.7 for head units up to 1.7, else the
        // newer 6.1 numbering (F-Auto's clamp). Status success.
        quint16 replyMajor = kVersionMajor, replyMinor = kVersionMinor;
        if (!(m_huMajor < 1 || (m_huMajor == 1 && m_huMinor <= 7))) {
            replyMajor = 6;
            replyMinor = 1;
        }
        QByteArray resp(6, Qt::Uninitialized);
        auto *p = reinterpret_cast<uchar *>(resp.data());
        qToBigEndian<quint16>(replyMajor, p);
        qToBigEndian<quint16>(replyMinor, p + 2);
        qToBigEndian<quint16>(quint16(STATUS_SUCCESS), p + 4);
        send(0, MSG_VERSION_RESPONSE, resp);
        m_phase = Phase::Handshake;
        emit stateChanged(tr("TLS handshake"));
        break;
    }
    case MSG_ENCAPSULATED_SSL: {
        QByteArray out;
        QString error;
        if (!m_cryptor.handshake(body, out, &error)) {
            fail(error);
            return;
        }
        if (!out.isEmpty())
            send(0, MSG_ENCAPSULATED_SSL, out);
        if (m_cryptor.isEstablished() && m_phase == Phase::Handshake) {
            log(tr("TLS established: %1").arg(m_cryptor.peerSummary()));
            m_phase = Phase::WaitAuth;
        }
        break;
    }
    case MSG_AUTH_COMPLETE: {
        AuthComplete auth;
        parse(auth, body);
        if (auth.status() != STATUS_SUCCESS) {
            fail(tr("head unit rejected authentication (status %1)").arg(auth.status()));
            return;
        }
        m_encrypt = true;
        m_phase = Phase::Discovery;
        emit stateChanged(tr("Service discovery"));
        ServiceDiscoveryRequest req;
        req.set_label(m_config.label.toStdString());
        req.set_device_name(m_config.deviceName.toStdString());
        send(0, MSG_SERVICE_DISCOVERY_REQUEST, req.SerializeAsString());
        break;
    }
    case MSG_SERVICE_DISCOVERY_RESPONSE:
        onServiceDiscovery(body);
        break;
    case MSG_PING_REQUEST: {
        PingRequest ping;
        parse(ping, body);
        PingResponse pong;
        pong.set_timestamp(ping.timestamp());
        send(0, MSG_PING_RESPONSE, pong.SerializeAsString());
        break;
    }
    case MSG_PING_RESPONSE:
        break;
    case MSG_NAV_FOCUS_NOTIFICATION:
        break;
    case MSG_AUDIO_FOCUS_NOTIFICATION:
        onAudioFocus(body);
        break;
    case MSG_BYEBYE_REQUEST: {
        ByeByeRequest bye;
        parse(bye, body);
        send(0, MSG_BYEBYE_RESPONSE, ByeByeResponse().SerializeAsString());
        fail(tr("head unit said bye (reason %1)").arg(bye.reason()));
        break;
    }
    case MSG_BYEBYE_RESPONSE:
        fail(tr("disconnected"));
        break;
    case MSG_CHANNEL_CLOSE_NOTIFICATION:
        log(tr("head unit closed a channel"));
        break;
    case MSG_FRAMING_ERROR:
        fail(tr("head unit reported a framing error"));
        break;
    default:
        qCInfo(lcAap) << "unhandled control message" << msg.id() << body.size() << "bytes";
        break;
    }
}

void Session::onServiceDiscovery(const QByteArray &body)
{
    ServiceDiscoveryResponse resp;
    if (!parse(resp, body)) {
        fail(tr("malformed service discovery response"));
        return;
    }
    log(tr("Car: %1 %2, head unit %3 %4")
            .arg(QString::fromStdString(resp.make()), QString::fromStdString(resp.model()),
                 QString::fromStdString(resp.head_unit_make()), QString::fromStdString(resp.head_unit_model())));

    for (const Service &svc : resp.services()) {
        const quint8 ch = quint8(svc.id());
        if (svc.has_media_sink_service()) {
            const MediaSinkService &sink = svc.media_sink_service();
            if (sink.video_configs_size() > 0 && m_videoChannel < 0
                    && sink.display_type() == 0 /* main display */) {
                ChannelInfo info{Kind::Video};
                for (const VideoConfig &vc : sink.video_configs()) {
                    VideoMode mode;
                    mode.size = resolutionSize(vc.resolution());
                    mode.fps = vc.frame_rate() == FPS_30 ? 30 : 60;
                    mode.density = int(vc.density());
                    // Margins are split between both sides (Android Rect(w/2, (h+1)/2, ...)).
                    const int wm = int(vc.width_margin());
                    const int hm = int(vc.height_margin());
                    mode.contentRect = QRect(wm / 2, (hm + 1) / 2,
                                             mode.size.width() - wm, mode.size.height() - hm);
                    log(tr("video config %1: %2x%3@%4 margins %5x%6 dpi %7")
                            .arg(info.videoModes.size()).arg(mode.size.width()).arg(mode.size.height())
                            .arg(mode.fps).arg(wm).arg(hm).arg(mode.density));
                    info.videoModes.append(mode);
                }
                m_channels.insert(ch, info);
                m_videoChannel = ch;
            } else if (sink.audio_type() == AUDIO_MEDIA && m_config.mediaAudio && m_mediaAudioChannel < 0) {
                ChannelInfo info{Kind::MediaAudio};
                if (sink.audio_configs_size() > 0) {
                    info.sampleRate = int(sink.audio_configs(0).sample_rate());
                    info.channels = int(sink.audio_configs(0).channels());
                }
                m_channels.insert(ch, info);
                m_mediaAudioChannel = ch;
            }
        } else if (svc.has_input_source_service()) {
            ChannelInfo info{Kind::Input};
            const InputSourceService &in = svc.input_source_service();
            if (in.touchscreen_size() > 0)
                info.touchSize = QSize(in.touchscreen(0).width(), in.touchscreen(0).height());
            // The head unit advertises which hardware/steering-wheel keycodes it
            // can send. We must bind exactly these for it to forward them (some
            // cars, e.g. Ford SYNC, use non-obvious codes), so remember them.
            QStringList codes;
            for (int i = 0; i < in.keycodes_supported_size(); ++i) {
                info.huKeycodes.append(in.keycodes_supported(i));
                codes << QString::number(in.keycodes_supported(i));
            }
            if (!info.huKeycodes.isEmpty())
                log(tr("head unit input keycodes: %1").arg(codes.join(QLatin1Char(' '))));
            m_channels.insert(ch, info);
        } else if (svc.has_sensor_source_service()) {
            m_channels.insert(ch, ChannelInfo{Kind::Sensor});
            // Remember which sensors the car offers, so we can ask for the useful
            // ones (e.g. FUEL/energy) and know what's even possible on this car.
            const SensorSourceService &ss = svc.sensor_source_service();
            QStringList types;
            for (int i = 0; i < ss.sensors_size(); ++i) {
                m_sensorsAvailable.insert(ss.sensors(i).sensor_type());
                types << QString::number(ss.sensors(i).sensor_type());
            }
            for (int i = 0; i < ss.supported_fuel_types_size(); ++i)
                if (ss.supported_fuel_types(i) == 10) // FUEL_TYPE_ELECTRIC
                    m_evCar = true;
            log(tr("head unit sensors: %1%2")
                    .arg(types.isEmpty() ? tr("(none)") : types.join(QLatin1Char(' ')),
                         m_evCar ? tr(" [electric car]") : QString()));
        } else if (svc.has_media_source_service() && m_micChannel < 0) {
            // The car's microphone is advertised as a MediaSource. We open it on
            // demand (e.g. a steering-wheel voice button), not at startup.
            ChannelInfo info{Kind::Mic};
            const MediaSourceService &src = svc.media_source_service();
            if (src.has_audio_config()) {
                info.sampleRate = int(src.audio_config().sample_rate());
                info.channels = int(src.audio_config().channels());
            } else {
                info.sampleRate = 16000;
                info.channels = 1;
            }
            m_channels.insert(ch, info);
            m_micChannel = ch;
            log(tr("microphone available: %1 Hz x%2").arg(info.sampleRate).arg(info.channels));
        } else if (svc.has_navigation_status_service() && m_navChannel < 0) {
            // Instrument-cluster turn-by-turn. Opened like any channel; we then
            // push guidance from the map UI onto it.
            m_channels.insert(ch, ChannelInfo{Kind::Nav});
            m_navChannel = ch;
            const NavigationStatusService &nav = svc.navigation_status_service();
            if (nav.has_minimum_interval_ms() && nav.minimum_interval_ms() > 0)
                m_navMinIntervalMs = nav.minimum_interval_ms();
            log(tr("instrument cluster navigation available (min interval %1 ms)").arg(m_navMinIntervalMs));
        } else if (svc.has_phone_status_service() && m_phoneStatusChannel < 0) {
            // The HU can show the in-AA call screen. Open the channel; we then
            // push call state (PhoneStatus) onto it and receive answer/reject.
            m_channels.insert(ch, ChannelInfo{Kind::PhoneStatus});
            log(tr("head unit can show calls in Android Auto (phone-status service)"));
        }
    }

    if (m_videoChannel < 0) {
        fail(tr("head unit offers no video sink"));
        return;
    }

    // Channel open requests carry the CONTROL flag on the service channel.
    for (auto it = m_channels.cbegin(); it != m_channels.cend(); ++it) {
        ChannelOpenRequest req;
        req.set_priority(0);
        req.set_service_id(it.key());
        send(it.key(), MSG_CHANNEL_OPEN_REQUEST, req.SerializeAsString(), true);
    }
    m_phase = Phase::Running;
    emit stateChanged(tr("Opening channels"));
}

void Session::onChannel(ChannelInfo &ch, quint8 channel, const Message &msg)
{
    const QByteArray body = msg.body();
    const quint16 id = msg.id();

    if (id == MSG_CHANNEL_OPEN_RESPONSE) {
        ChannelOpenResponse resp;
        parse(resp, body);
        if (resp.status() != STATUS_SUCCESS) {
            log(tr("channel %1 refused (status %2)").arg(channel).arg(resp.status()));
            return;
        }
        ch.open = true;
        onChannelOpened(ch, channel);
        return;
    }

    switch (ch.kind) {
    case Kind::Video:
    case Kind::MediaAudio:
        if (id == MEDIA_CONFIG) {
            onMediaConfig(ch, channel, body);
        } else if (id == MEDIA_ACK) {
            MediaAck ack;
            parse(ack, body);
            ch.unacked = qMax(0, ch.unacked - int(qMax(1u, ack.ack())));
        } else if (id == MEDIA_VIDEO_FOCUS_NOTIFICATION && ch.kind == Kind::Video) {
            onVideoFocus(ch, channel, body);
        } else {
            qCInfo(lcAap) << "unhandled media message" << hex << id << "on channel" << channel;
        }
        break;
    case Kind::Input:
        if (id == INPUT_REPORT)
            onInput(ch, body);
        else if (id == INPUT_KEY_BINDING_RESPONSE) {
            KeyBindingResponse resp;
            parse(resp, body);
            // 0 = OK; -18 = KEYCODE_NOT_BOUND, -20 = INVALID_INPUT. A non-zero
            // status means the HU rejected (some of) our keycodes and may forward
            // nothing — worth seeing in the log rather than assuming success.
            if (resp.has_status() && resp.status() != 0)
                log(tr("key binding REJECTED by head unit (status %1)").arg(resp.status()));
            else
                log(tr("key binding acknowledged"));
        }
        break;
    case Kind::Sensor:
        if (id == SENSOR_BATCH)
            onSensorBatch(body);
        else if (id == SENSOR_ERROR)
            log(tr("sensor error from head unit"));
        break;
    case Kind::Mic:
        if (id == MEDIA_MICROPHONE_RESPONSE)
            onMicResponse(ch, body);
        else if (id == MEDIA_DATA)
            onMicData(ch, channel, msg);
        else if (id == MEDIA_CONFIG)
            onMediaConfig(ch, channel, body);
        else
            qCInfo(lcAap) << "unhandled mic message" << hex << id;
        break;
    case Kind::Nav:
        // The cluster tells us when it wants guidance (focus on/off).
        if (id == NAV_STATUS_START) {
            m_navClusterReady = true;
            log(tr("instrument cluster requested navigation"));
            if (m_nav && m_nav->active())
                onNavUpdated();
        } else if (id == NAV_STATUS_STOP) {
            m_navClusterReady = false;
            log(tr("instrument cluster released navigation"));
        } else {
            qCInfo(lcAap) << "unhandled nav message" << hex << id;
        }
        break;
    case Kind::PhoneStatus:
        if (id == PHONE_STATUS_INPUT)
            onPhoneStatusInput(body);
        else
            qCInfo(lcAap) << "unhandled phone-status message" << hex << id;
        break;
    }
}

// The head unit's in-AA call UI sends an answer/reject tap here. CALL(7) answers
// an incoming call (or ends the active one); BACK(6)/ENTER on an incoming call is
// treated as reject. Call audio stays on Bluetooth HFP; we only drive telephony.
void Session::onPhoneStatusInput(const QByteArray &body)
{
    PhoneStatusInput in;
    if (!parse(in, body))
        return;
    const int action = in.has_input() ? in.input().action() : 0;
    log(tr("phone-status input: action %1").arg(action));
    switch (action) {
    case 7: // CALL -> answer (or toggle)
        emit phoneAnswer();
        break;
    case 6: // BACK -> reject / hang up
        emit phoneReject();
        break;
    default:
        break;
    }
}

void Session::updateCall(int state, const QString &number, const QString &name)
{
    if (m_phoneStatusChannel < 0)
        return; // the HU did not advertise/open a phone-status channel
    PhoneStatus status;
    // State 3 == INACTIVE: report an empty call list to clear the HU's call UI.
    if (state != PhoneStatus::INACTIVE && state != PhoneStatus::UNKNOWN) {
        PhoneStatus::Call *c = status.add_calls();
        c->set_phone_state(state);
        if (!number.isEmpty())
            c->set_caller_number(number.toStdString());
        if (!name.isEmpty())
            c->set_caller_id(name.toStdString());
    }
    send(quint8(m_phoneStatusChannel), PHONE_STATUS, status.SerializeAsString());
    log(tr("phone status -> HU: state %1 %2").arg(state).arg(name.isEmpty() ? number : name));
}

void Session::onChannelOpened(ChannelInfo &ch, quint8 channel)
{
    switch (ch.kind) {
    case Kind::Video: {
        MediaSetup setup;
        setup.set_type(CODEC_H264_BP);
        send(channel, MEDIA_SETUP, setup.SerializeAsString());
        break;
    }
    case Kind::MediaAudio: {
        MediaSetup setup;
        setup.set_type(CODEC_PCM);
        send(channel, MEDIA_SETUP, setup.SerializeAsString());
        break;
    }
    case Kind::Input: {
        // Bind the hardware/steering-wheel buttons so the HU forwards them. The HU
        // only forwards keys we bind. The head unit advertises EXACTLY the codes it
        // can send (keycodes_supported), so — like F-Auto, which has real-car
        // compatibility reports — we bind exactly that advertised set. Binding more
        // is pointless (the HU won't send unadvertised codes) and some units reject
        // the whole request if it contains a code they don't support, forwarding
        // nothing. Only if the HU advertised nothing do we fall back to a known set.
        static const int kKnownKeys[] = {
            1, 2, 3, 4, 5, 6,            // mic / menu / home / back / call / endcall
            19, 20, 21, 22, 23,         // DPAD up/down/left/right/center
            24, 25, 91, 164,            // volume up/down/mute, (vol) mute
            79, 85, 86, 87, 88, 89, 90, // headset-hook, play-pause, stop, next, prev, rewind, ff
            126, 127,                   // play / pause
            82, 84, 219, 231,           // menu / search / assist / voice-assist
            260, 261, 262, 263,         // navigate prev/next/in/out
            65536, 65537, 65538, 65539, 65540, // rotary + media/navigation/radio/tel knobs
        };
        KeyBindingRequest req;
        QStringList bound;
        if (!ch.huKeycodes.isEmpty()) {
            for (int k : ch.huKeycodes)
                if (k > 0) { req.add_keycodes(k); bound << QString::number(k); }
        } else {
            for (int k : kKnownKeys)
                if (k > 0) { req.add_keycodes(k); bound << QString::number(k); }
        }
        log(tr("binding %1 input keycode(s): %2").arg(bound.size()).arg(bound.join(QLatin1Char(' '))));
        send(channel, INPUT_KEY_BINDING_REQUEST, req.SerializeAsString());
        break;
    }
    case Kind::Mic:
        // Nothing until a voice button opens it; just note it's ready.
        log(tr("microphone channel ready"));
        break;
    case Kind::Sensor:
        {
            QVector<int> want{ int(SENSOR_DRIVING_STATUS), int(SENSOR_NIGHT_MODE) };
            // Ask for the fuel/energy sensor too when advertised — this is the EV
            // battery level + remaining range we use for battery-at-arrival.
            if (m_sensorsAvailable.contains(int(SENSOR_FUEL))) {
                want << int(SENSOR_FUEL);
                log(tr("requesting the car's fuel/energy sensor"));
            }
            for (int type : want) {
                SensorRequest req;
                req.set_type(type);
                req.set_min_update_period(0);
                send(channel, SENSOR_REQUEST, req.SerializeAsString());
            }
        }
        break;
    case Kind::Nav:
        // Cluster ready. Some head units never send NAV_STATUS_START, so assume
        // we may push guidance once a route is active; START/STOP refine it.
        m_navClusterReady = true;
        log(tr("instrument cluster channel ready"));
        if (m_nav && m_nav->active())
            onNavUpdated();
        break;
    case Kind::PhoneStatus:
        m_phoneStatusChannel = channel;
        log(tr("phone-status channel ready (calls will show in Android Auto)"));
        emit phoneChannelReady();
        break;
    }
}

void Session::onMediaConfig(ChannelInfo &ch, quint8 channel, const QByteArray &body)
{
    MediaConfig cfg;
    parse(cfg, body);
    // Head units at protocol >= 6.0 run "ackless": never block on ACKs.
    ch.maxUnacked = m_huMajor >= 6 ? 1 << 20 : int(qMax(1u, cfg.max_unacked()));
    ch.configIndex = cfg.configuration_indices_size() > 0 ? int(cfg.configuration_indices(0)) : 0;

    if (ch.kind == Kind::MediaAudio) {
        log(tr("media audio channel ready: %1 Hz x%2").arg(ch.sampleRate).arg(ch.channels));
        // Ask for audio focus; streaming starts once the HU grants it.
        AudioFocusRequest req;
        req.set_request(AUDIO_FOCUS_GAIN);
        send(0, MSG_AUDIO_FOCUS_REQUEST, req.SerializeAsString());
        return;
    }

    // Video: the HU lists configuration indices in preference order. Use the
    // first one we can encode (H.264 is limited to 1080p).
    for (uint32_t idx : cfg.configuration_indices()) {
        if (int(idx) < ch.videoModes.size() && ch.videoModes[int(idx)].size.width() <= 1920
                && ch.videoModes[int(idx)].size.height() <= 1920) {
            ch.configIndex = int(idx);
            break;
        }
    }
    if (ch.configIndex < ch.videoModes.size()) {
        const VideoMode &mode = ch.videoModes[ch.configIndex];
        ch.videoSize = mode.size;
        ch.contentRect = mode.contentRect;
        // 60 fps readback + software conversion is too slow on phones; the
        // HU accepts fewer frames than announced.
        ch.fps = qMin(mode.fps, 30);
    }
    log(tr("video config status %1, max unacked %2, preferred index %3")
            .arg(cfg.status()).arg(cfg.max_unacked()).arg(ch.configIndex));
    VideoFocusRequest focus;
    focus.set_mode(FOCUS_PROJECTED);
    send(channel, MEDIA_VIDEO_FOCUS_REQUEST, focus.SerializeAsString());
}

void Session::onVideoFocus(ChannelInfo &ch, quint8 channel, const QByteArray &body)
{
    VideoFocusNotification n;
    parse(n, body);
    log(tr("video focus mode %1").arg(n.mode()));
    if (n.mode() == FOCUS_PROJECTED || n.mode() == FOCUS_PROJECTED_NO_INPUT)
        startVideo(ch, channel);
    else
        stopVideo();
}

void Session::startVideo(ChannelInfo &ch, quint8 channel)
{
    if (ch.started)
        return;
    if (!ch.videoSize.isValid()) {
        fail(tr("no usable video configuration"));
        return;
    }
    ch.sessionId = m_nextSessionId++ & 0xff;
    ch.unacked = 0;
    MediaStart start;
    start.set_session_id(ch.sessionId);
    start.set_configuration_index(uint32_t(ch.configIndex));
    send(channel, MEDIA_START, start.SerializeAsString());
    ch.started = true;

    QString error;
    // Bitrate heuristic from the phone app: 2.75 bit per visible pixel + 1.5 Mbit/s.
    const int pixels = ch.contentRect.width() * ch.contentRect.height();
    const int kbps = qMax(2000, int(pixels * 2.75 / 1000) + 1500) * (ch.fps >= 60 ? 2 : 1);
    if (!m_screen->start(m_config.carQml, ch.videoSize, ch.fps, &error, ch.contentRect)
            || !m_video->start(ch.videoSize, ch.fps, kbps, &error)) {
        fail(error);
        return;
    }
    emit stateChanged(tr("Projecting %1x%2@%3")
                          .arg(ch.videoSize.width()).arg(ch.videoSize.height()).arg(ch.fps));
}

void Session::stopVideo()
{
    m_video->stop();
    m_screen->stop();
    if (m_videoChannel >= 0) {
        ChannelInfo &ch = m_channels[quint8(m_videoChannel)];
        if (ch.started && m_phase == Phase::Running)
            send(quint8(m_videoChannel), MEDIA_STOP, MediaStop().SerializeAsString());
        ch.started = false;
    }
}

void Session::sendVideoConfig(const QByteArray &spsPps)
{
    if (m_videoChannel < 0)
        return;
    sendMedia(quint8(m_videoChannel), MEDIA_CODEC_CONFIG, 0, spsPps, false);
}

void Session::sendVideoFrame(const QByteArray &au, quint64, bool keyFrame)
{
    if (m_videoChannel < 0)
        return;
    ChannelInfo &ch = m_channels[quint8(m_videoChannel)];
    if (!ch.started)
        return;
    if (m_keyFramePending && !keyFrame)
        return; // after a drop, wait for the next IDR
    if (ch.unacked >= ch.maxUnacked) {
        // The phone app waits up to 300 ms, then drops. With a live encoder
        // a drop breaks the reference chain, so ask for a fresh IDR.
        ++m_droppedFrames;
        m_keyFramePending = true;
        m_video->requestKeyFrame();
        return;
    }
    m_keyFramePending = false;
    ++ch.unacked;
    sendMedia(quint8(m_videoChannel), MEDIA_DATA, monotonicUs(), au, true);
}

void Session::onAudioFocus(const QByteArray &body)
{
    AudioFocusNotification n;
    parse(n, body);
    log(tr("audio focus state %1").arg(n.state()));
    if (m_mediaAudioChannel < 0)
        return;
    ChannelInfo &ch = m_channels[quint8(m_mediaAudioChannel)];
    const bool gained = n.state() == AUDIO_FOCUS_STATE_GAIN || n.state() == AUDIO_FOCUS_STATE_GAIN_MEDIA_ONLY;
    if (gained && !ch.started) {
        ch.sessionId = m_nextSessionId++ & 0xff;
        ch.unacked = 0;
        MediaStart start;
        start.set_session_id(ch.sessionId);
        start.set_configuration_index(uint32_t(ch.configIndex));
        send(quint8(m_mediaAudioChannel), MEDIA_START, start.SerializeAsString());
        ch.started = true;
        QString error;
        if (!m_audio->start(m_config.audioDevice, ch.sampleRate, ch.channels, &error))
            log(tr("audio capture failed: %1").arg(error));
        else
            log(tr("media audio: capturing %1").arg(m_audio->resolvedDevice()));
    } else if (!gained && ch.started) {
        m_audio->stop();
        send(quint8(m_mediaAudioChannel), MEDIA_STOP, MediaStop().SerializeAsString());
        ch.started = false;
    }
}

void Session::sendAudio(const QByteArray &pcm, quint64)
{
    if (m_mediaAudioChannel < 0)
        return;
    ChannelInfo &ch = m_channels[quint8(m_mediaAudioChannel)];
    if (!ch.started)
        return;
    // Audio is ackless from protocol 5.0 on.
    if (m_huMajor < 5 && ch.unacked >= ch.maxUnacked)
        return;
    ++ch.unacked;
    sendMedia(quint8(m_mediaAudioChannel), MEDIA_DATA, monotonicUs(), pcm, true);
}

// --- instrument cluster turn-by-turn -----------------------------------------
// Map QtLocation RouteManeuver.Direction (0..11) onto the Android Auto
// NavigationType (Car App Library Maneuver.TYPE_*). 0/1 == no turn / straight.
static int navTypeFromDir(int dir)
{
    switch (dir) {
    case 2: case 3: return 6;   // light/bear right -> TURN_SLIGHT_RIGHT
    case 4:         return 8;   // right            -> TURN_NORMAL_RIGHT
    case 5:         return 10;  // hard right       -> TURN_SHARP_RIGHT
    case 6:         return 12;  // u-turn right     -> U_TURN_RIGHT
    case 7:         return 11;  // u-turn left      -> U_TURN_LEFT
    case 8:         return 9;   // hard left        -> TURN_SHARP_LEFT
    case 9:         return 7;   // left             -> TURN_NORMAL_LEFT
    case 10: case 11: return 5; // light/bear left  -> TURN_SLIGHT_LEFT
    default:        return 36;  // straight / none  -> STRAIGHT
    }
}

// Legacy NextTurnEnum for older head units.
static int navLegacyEvent(int dir)
{
    switch (dir) {
    case 2: case 3: case 10: case 11: return 3; // SLIGHT_TURN
    case 4: case 9:                   return 4; // TURN
    case 5: case 8:                   return 5; // SHARP_TURN
    case 6: case 7:                   return 6; // U_TURN
    default:                          return 14; // STRAIGHT
    }
}

// turn_side: LEFT=1 RIGHT=2 UNSPECIFIED=3.
static int navTurnSide(int dir)
{
    if (dir == 6 || (dir >= 2 && dir <= 5)) return 2; // right family
    if (dir == 7 || (dir >= 8 && dir <= 11)) return 1; // left family
    return 3;
}

// Fill a NavigationDistance with meters + a human display value/unit.
static void navFillDistance(NavigationDistance *d, int meters)
{
    d->set_meters(meters);
    if (meters >= 1000) {
        d->set_display_units(3); // KILOMETERS_P1 (one decimal)
        d->set_display_value(QString::number(meters / 1000.0, 'f', 1).toStdString());
    } else {
        const int rounded = (meters / 10) * 10; // nearest 10 m
        d->set_display_units(1); // METERS
        d->set_display_value(QString::number(rounded).toStdString());
    }
}

void Session::onNavUpdated()
{
    if (!m_nav || m_navChannel < 0)
        return;

    if (!m_nav->active()) {
        if (m_navActive) {
            sendNavStatus(2); // INACTIVE
            m_navActive = false;
        }
        return;
    }

    // Transition into guidance: announce ACTIVE immediately.
    if (!m_navActive) {
        sendNavStatus(1); // ACTIVE
        m_navActive = true;
        m_lastNavSendMs = 0; // force a guidance push right away
    }

    // Respect the cluster's advertised rate floor for guidance pushes.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastNavSendMs != 0 && now - m_lastNavSendMs < m_navMinIntervalMs)
        return;
    m_lastNavSendMs = now;
    sendNavGuidance();
}

void Session::sendNavStatus(int status)
{
    if (m_navChannel < 0)
        return;
    NavigationStatus st;
    st.set_status(status);
    send(quint8(m_navChannel), NAV_STATUS, st.SerializeAsString());
}

void Session::sendNavGuidance()
{
    if (!m_nav || m_navChannel < 0)
        return;

    const int dir = m_nav->dir();
    const std::string road = m_nav->road().toStdString();
    const std::string instr = m_nav->instruction().toStdString();
    const int stepM = qMax(0, m_nav->stepMeters());
    const int remM = qMax(0, m_nav->remainingMeters());

    // NavigationState: the upcoming step (+ destination), modern cluster UI.
    NavigationState state;
    NavigationStep *step = state.add_steps();
    step->mutable_maneuver()->set_type(navTypeFromDir(dir));
    if (!road.empty())
        step->mutable_road()->set_name(road);
    if (!instr.empty())
        step->mutable_cue()->add_alternate_text(instr);
    if (!m_nav->destLabel().isEmpty())
        state.add_destinations()->set_address(m_nav->destLabel().toStdString());
    send(quint8(m_navChannel), NAV_STATE, state.SerializeAsString());

    // NavigationCurrentPosition: distance to next step + to destination.
    NavigationCurrentPosition pos;
    navFillDistance(pos.mutable_step_distance()->mutable_distance(), stepM);
    if (remM > 0) {
        NavigationDestinationDistance *dd = pos.add_destination_distances();
        navFillDistance(dd->mutable_distance(), remM);
        if (m_nav->remainingSeconds() > 0)
            dd->set_time_to_arrival_seconds(m_nav->remainingSeconds());
        if (!m_nav->eta().isEmpty())
            dd->set_estimated_time_at_arrival(m_nav->eta().toStdString());
    }
    if (!road.empty())
        pos.mutable_current_road()->set_name(road);
    send(quint8(m_navChannel), NAV_CURRENT_POSITION, pos.SerializeAsString());

    // Legacy pair for older head units that ignore STATE/CURRENT_POSITION.
    NavigationNextTurnEvent turn;
    turn.set_road(instr.empty() ? road : instr);
    turn.set_turn_side(navTurnSide(dir));
    turn.set_event(navLegacyEvent(dir));
    send(quint8(m_navChannel), NAV_TURN_EVENT, turn.SerializeAsString());

    NavigationNextTurnDistanceEvent dist;
    dist.set_distance_meters(stepM);
    dist.set_time_to_turn_seconds(0);
    if (stepM >= 1000) {
        dist.set_display_distance_unit(3); // KILOMETERS_P1
        dist.set_display_distance_e3(int(qRound(stepM / 1000.0 * 10) * 100)); // 0.1 km * 1000
    } else {
        const int rounded = (stepM / 10) * 10;
        dist.set_display_distance_unit(1); // METERS
        dist.set_display_distance_e3(rounded * 1000);
    }
    send(quint8(m_navChannel), NAV_DISTANCE_EVENT, dist.SerializeAsString());
}

// Two rectangles share an aspect ratio within ~1% (F-Auto TouchMapper heuristic).
static bool aspectClose(int aw, int ah, int bw, int bh)
{
    if (ah <= 0 || bh <= 0)
        return false;
    const qreal a = qreal(aw) / ah;
    const qreal b = qreal(bw) / bh;
    return qAbs(a - b) <= 0.01 * b;
}

void Session::onInput(const ChannelInfo &ch, const QByteArray &body)
{
    InputReport report;
    if (!parse(report, body))
        return;

    if (report.has_key_event()) {
        for (const Key &k : report.key_event().keys())
            dispatchKey(k.keycode(), k.down(), k.longpress());
    }

    // Rotary/knob/scroll-wheel steps. Many head units (and some steering wheels)
    // deliver track-skip and tuning here instead of as discrete key presses, so
    // this field being unparsed previously made those buttons do nothing at all.
    if (report.has_relative_event()) {
        for (const Rel &r : report.relative_event().data())
            dispatchRelative(r.keycode(), r.delta());
    }
    if (report.has_absolute_event()) {
        for (const Abs &a : report.absolute_event().data())
            log(tr("HU absolute event: keycode=%1 value=%2").arg(a.keycode()).arg(a.value()));
    }

    if (!report.has_touch_event())
        return;
    const TouchEvent &t = report.touch_event();
    if (t.pointers_size() == 0)
        return;
    // Single touch: follow the pointer the action refers to.
    const int idx = qBound(0, int(t.action_index()), t.pointers_size() - 1);
    const Pointer &p = t.pointers(idx);

    if (m_videoChannel < 0)
        return;
    const ChannelInfo &video = m_channels[quint8(m_videoChannel)];
    // Port of F-Auto's TouchMapper. The head unit reports touches in some source
    // resolution covering the *visible* area (the video frame minus its margins,
    // == contentRect). Work out that source extent per axis:
    //   * no declared touch panel        -> the visible area;
    //   * a declared panel larger than the visible area on BOTH axes AND the same
    //     aspect ratio (e.g. Ford SYNC 4) -> the HU actually reports visible-area
    //     pixels, so map 1:1 + margin. Scaling by the larger declared panel here
    //     lands every tap SHORT of the far edge — this was the "touch way off" bug;
    //   * otherwise the declared panel, scaled into the visible area.
    // A runtime guard latches to the visible area if a coordinate ever exceeds the
    // declared panel (proving that value is too small). The result is clamped to
    // the visible area; CarMain sits at contentRect, so a frame-space point lands
    // on the matching CarMain pixel.
    const QRect content = video.contentRect.isValid() ? video.contentRect
                                                      : QRect(QPoint(), video.videoSize);
    const int visW = qMax(1, content.width());
    const int visH = qMax(1, content.height());
    const int declX = ch.touchSize.width();
    const int declY = ch.touchSize.height();
    const bool visiblePixels = declX > visW && declY > visH
            && aspectClose(declX, declY, visW, visH);
    if (m_touchSrcX <= 0) {
        m_touchSrcX = (declX > 0 && !visiblePixels) ? declX : visW;
        m_touchSrcY = (declY > 0 && !visiblePixels) ? declY : visH;
    }
    if (declX > 0 && m_touchSrcX == declX && p.x() > declX) m_touchSrcX = visW;
    if (declY > 0 && m_touchSrcY == declY && p.y() > declY) m_touchSrcY = visH;
    const QPointF pos(
        qBound<qreal>(content.x(), p.x() * qreal(visW) / m_touchSrcX + content.x(), content.x() + visW),
        qBound<qreal>(content.y(), p.y() * qreal(visH) / m_touchSrcY + content.y(), content.y() + visH));

    switch (t.action()) {
    case TOUCH_DOWN:
        log(tr("touch down raw(%1,%2) panel=%3x%4 src=%5x%6 content=%7,%8 %9x%10 -> (%11,%12)")
                .arg(p.x()).arg(p.y()).arg(declX).arg(declY)
                .arg(m_touchSrcX).arg(m_touchSrcY)
                .arg(content.x()).arg(content.y())
                .arg(content.width()).arg(content.height())
                .arg(int(pos.x())).arg(int(pos.y())));
        m_screen->touch(CarScreen::Press, pos);
        break;
    case TOUCH_UP:
        m_screen->touch(CarScreen::Release, pos);
        break;
    case TOUCH_MOVE:
        m_screen->touch(CarScreen::Move, pos);
        break;
    default:
        break; // multi touch not mapped yet
    }
}

// Map an Android keycode from a steering-wheel / hardware button to a semantic
// action and route it. Media/nav actions are emitted for the controller to hand
// to the chosen apps; the voice button toggles the microphone.
void Session::dispatchKey(quint32 keycode, bool down, bool longpress)
{
    // Rotary controller arrives as a relative value; report direction on the
    // metastate-less event. We only surface it as nav scroll for now.
    if (keycode == 65536) {
        emit navKey(QStringLiteral("rotary"));
        return;
    }
    if (!down) // act on press; ignore the matching release (except where noted)
        return;

    QString media, nav;
    switch (keycode) {
    case 231: // VOICE_ASSIST -> push-to-talk: toggle the microphone
    case 84:  // SEARCH (some wheels map voice here)
        requestMicrophone(!m_micOn);
        emit voiceRequested(m_micOn);
        log(tr("voice button -> microphone %1").arg(m_micOn ? "on" : "off"));
        return;
    case 85: case 79: media = QStringLiteral("play-pause"); break;
    case 126: media = QStringLiteral("play"); break;
    case 127: media = QStringLiteral("pause"); break;
    case 86:  media = QStringLiteral("stop"); break;
    case 87:  media = QStringLiteral("next"); break;
    case 88:  media = QStringLiteral("prev"); break;
    case 90:  media = QStringLiteral("ff"); break;
    case 89:  media = QStringLiteral("rew"); break;
    case 4:   nav = QStringLiteral("back"); break;
    case 3:   nav = QStringLiteral("home"); break;
    case 82:  nav = QStringLiteral("menu"); break;
    case 19:  nav = QStringLiteral("up"); break;
    case 20:  nav = QStringLiteral("down"); break;
    case 21:  nav = QStringLiteral("left"); break;
    case 22:  nav = QStringLiteral("right"); break;
    case 23:  nav = QStringLiteral("select"); break;
    case 5:   log(tr("CALL button -> answer")); emit phoneAnswer(); return;   // KEYCODE_CALL
    case 6:   log(tr("ENDCALL button -> reject/hang up")); emit phoneReject(); return; // KEYCODE_ENDCALL
    default:
        log(tr("unmapped key %1").arg(keycode));
        return;
    }

    if (!media.isEmpty()) {
        if (longpress)
            media += QStringLiteral("-long");
        log(tr("media key: %1").arg(media));
        emit mediaKey(media);
        // Reflect transport state in the (mock) car UI where it makes sense.
        if (media == QLatin1String("play-pause") || media == QLatin1String("play")
                || media == QLatin1String("pause"))
            m_screen->setCarProperty(QStringLiteral("playing"), media != QLatin1String("pause"));
    } else {
        log(tr("nav key: %1").arg(nav));
        emit navKey(nav);
    }
}

// A rotary/knob/scroll step from the head unit (InputReport.relative_event). We
// log every one so a car test reveals exactly what a given control emits, then
// map it to a track skip — the user's wheel "change song" control. One skip per
// event (direction from the sign); magnitude is ignored so a multi-detent knob
// can't skip a whole album at once.
void Session::dispatchRelative(quint32 keycode, int delta)
{
    log(tr("HU relative event: keycode=%1 delta=%2").arg(keycode).arg(delta));
    if (delta == 0)
        return;
    emit mediaKey(delta > 0 ? QStringLiteral("next") : QStringLiteral("prev"));
}

// Open or close the car microphone. On open we send MICROPHONE_REQUEST and arm
// the receiver; PCM then arrives as DATA on the mic channel.
void Session::requestMicrophone(bool on)
{
    if (m_micChannel < 0) {
        log(tr("no microphone offered by this head unit"));
        return;
    }
    if (on == m_micOn)
        return;
    MicrophoneRequest req;
    req.set_open(on);
    req.set_anc_enabled(false);
    req.set_ec_enabled(false);
    req.set_max_unacked(2);
    send(quint8(m_micChannel), MEDIA_MICROPHONE_REQUEST, req.SerializeAsString());
    m_micOn = on;
    if (!on)
        m_mic->end();
}

void Session::onMicResponse(ChannelInfo &ch, const QByteArray &body)
{
    MicrophoneResponse resp;
    parse(resp, body);
    if (resp.status() != STATUS_SUCCESS && resp.status() != 0) {
        log(tr("microphone request refused (status %1)").arg(resp.status()));
        m_micOn = false;
        return;
    }
    ch.sessionId = resp.session_id();
    ch.unacked = 0;
    if (m_micOn) {
        QString err;
        const QString wav = QDir::homePath() + QStringLiteral("/carfish-mic.wav");
        if (!m_mic->begin(ch.sampleRate, ch.channels, wav, &err))
            log(tr("microphone capture error: %1").arg(err));
        else
            log(tr("microphone open (%1 Hz x%2), recording to %3").arg(ch.sampleRate).arg(ch.channels).arg(wav));
    } else {
        m_mic->end();
        log(tr("microphone closed"));
    }
}

void Session::onMicData(ChannelInfo &ch, quint8 channel, const Message &msg)
{
    QByteArray payload = msg.body();
    // DATA is [u64 BE timestamp us][PCM]; drop the timestamp prefix if present.
    if (payload.size() >= 8)
        payload = payload.mid(8);
    m_mic->feed(payload);

    // Acknowledge so the HU keeps streaming (ackless HUs ignore it, but cheap).
    ++ch.unacked;
    MediaAck ack;
    ack.set_session_id(ch.sessionId);
    ack.set_ack(ch.unacked);
    send(channel, MEDIA_ACK, ack.SerializeAsString());
    ch.unacked = 0;
}

void Session::onSensorBatch(const QByteArray &body)
{
    SensorBatch batch;
    parse(batch, body);
    if (batch.night_mode_size() > 0)
        m_screen->setNightMode(batch.night_mode(batch.night_mode_size() - 1).night_mode());
    if (batch.driving_status_size() > 0)
        log(tr("driving status %1").arg(batch.driving_status(batch.driving_status_size() - 1).status()));
    if (batch.fuel_size() > 0) {
        const FuelData &f = batch.fuel(batch.fuel_size() - 1);
        const int pct = f.has_fuel_level() ? f.fuel_level() : -1;
        const int rangeKm = f.has_range() ? f.range() : -1;
        log(tr("car %1: %2%% , range %3 km%4")
                .arg(m_evCar ? tr("battery") : tr("fuel"))
                .arg(pct).arg(rangeKm)
                .arg(f.low_fuel_warning() ? tr(" (LOW)") : QString()));
        emit energyStatus(pct, rangeKm, m_evCar, f.low_fuel_warning());
    }
}

} // namespace aap
