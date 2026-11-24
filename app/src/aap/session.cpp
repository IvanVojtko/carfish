#include "session.h"

#include "aap.pb.h"
#include "media/audiosource.h"
#include "media/carscreen.h"
#include "media/micreceiver.h"
#include "media/navbridge.h"
#include "media/videosource.h"
#include "transport/transport.h"

#include <QDateTime>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
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

// The app icon as a size x size PNG, for the service discovery request.
static QByteArray iconPng(int size)
{
    static const char *const kPaths[] = {
        "/usr/share/icons/hicolor/172x172/apps/carfish.png",
        "/usr/share/icons/hicolor/128x128/apps/carfish.png",
        "/usr/share/icons/hicolor/108x108/apps/carfish.png",
        "/usr/share/icons/hicolor/86x86/apps/carfish.png",
    };
    QImage img;
    for (const char *path : kPaths) {
        if (img.load(QString::fromLatin1(path)))
            break;
    }
    if (img.isNull()) {
        // Not installed (host build): a plain blue disc.
        img = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x1e, 0x88, 0xe5));
        p.drawEllipse(QRectF(0, 0, size, size));
    }
    img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

// "Manufacturer Model" like Gearhead's Build.MANUFACTURER + MODEL, from
// /etc/hw-release (NAME="Jolla Jolla Phone" -> "Jolla Phone").
static QString phoneModel()
{
    QFile f(QStringLiteral("/etc/hw-release"));
    if (f.open(QIODevice::ReadOnly)) {
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (!line.startsWith(QLatin1String("NAME=")))
                continue;
            QString name = line.mid(5);
            name.remove(QLatin1Char('"'));
            QStringList words = name.split(QLatin1Char(' '), QString::SkipEmptyParts);
            if (words.size() > 1 && words[0].compare(words[1], Qt::CaseInsensitive) == 0)
                words.removeFirst();
            if (!words.isEmpty())
                return words.join(QLatin1Char(' '));
        }
    }
    return QStringLiteral("Sailfish OS");
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
    m_navFlush.setSingleShot(true);
    connect(&m_navFlush, &QTimer::timeout, this, &Session::flushNavGuidance);

    connect(m_transport, &Transport::note, this, &Session::log);
    m_stats.setInterval(10000);
    connect(&m_stats, &QTimer::timeout, this, &Session::logStats);
}

void Session::logStats()
{
    if (m_phase != Phase::Running) {
        m_stats.stop();
        return;
    }
    auto media = [this](int channel, const QString &name) -> QString {
        if (channel < 0 || !m_channels.contains(quint8(channel)))
            return QString();
        ChannelInfo &ch = m_channels[quint8(channel)];
        QString s = QStringLiteral(", %1 %2 sent/%3 acked").arg(name).arg(ch.txFrames).arg(ch.acks);
        if (ch.drops)
            s += QStringLiteral("/%1 dropped").arg(ch.drops);
        s += QStringLiteral(" (%1/%2 waiting%3)").arg(ch.unacked).arg(ch.maxUnacked)
                 .arg(ch.started ? QString() : QStringLiteral(", stopped"));
        ch.txFrames = ch.acks = ch.drops = 0;
        return s;
    };
    QString line = QStringLiteral("stats: rx %1 msgs (last %2 s ago)")
                       .arg(m_rxMsgs).arg(m_lastRxMs ? (qint64(monotonicUs() / 1000) - m_lastRxMs) / 1000.0 : -1.0, 0, 'f', 1);
    line += media(m_videoChannel, QStringLiteral("video"));
    line += media(m_mediaAudioChannel, QStringLiteral("audio"));
    const QString t = m_transport->takeStats();
    if (!t.isEmpty())
        line += QStringLiteral(", ") + t;
    m_rxMsgs = 0;
    log(line);
}

void Session::trace(bool tx, quint8 channel, quint16 id, int size)
{
    // Skip the media stream itself (frames, acks) and pings.
    if (channel != 0 && (id == MEDIA_DATA || id == MEDIA_ACK))
        return;
    if (channel == 0 && (id == MSG_PING_REQUEST || id == MSG_PING_RESPONSE))
        return;
    if (m_trace.size() >= 12)
        m_trace.removeFirst();
    m_trace.append({ qint64(monotonicUs() / 1000), channel, id, size, tx });
}

void Session::noteUnhandled(const char *what, quint8 channel, quint16 id, int size)
{
    const quint32 key = (quint32(channel) << 16) | id;
    if (m_unhandledSeen.contains(key))
        return;
    m_unhandledSeen.insert(key);
    log(tr("unhandled %1 message 0x%2 on channel %3 (%4 B)")
            .arg(QString::fromLatin1(what)).arg(id, 0, 16).arg(channel).arg(size));
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
    if (m_phase == Phase::Running && !m_trace.isEmpty()) {
        const qint64 now = qint64(monotonicUs() / 1000);
        QStringList parts;
        for (const Trace &t : m_trace)
            parts << QStringLiteral("%1 ch%2 0x%3 %4B -%5s").arg(t.tx ? QStringLiteral("tx") : QStringLiteral("rx"))
                         .arg(t.channel).arg(t.id, 0, 16).arg(t.size).arg((now - t.ms) / 1000.0, 0, 'f', 1);
        log(QStringLiteral("last messages: ") + parts.join(QStringLiteral(", ")));
    }
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
    trace(true, channel, id, body.size());
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
    ++m_rxMsgs;
    m_lastRxMs = qint64(monotonicUs() / 1000);
    trace(false, msg.channel, msg.id(), msg.body().size());
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
        if (m_phase != Phase::WaitVersion) {
            // The head unit gave up on the handshake (e.g. it didn't like our
            // service discovery request) and started over in plain text. Start
            // over with it: answering on the old encrypted link only earns an
            // "unexpected message" from the car.
            if (m_phase == Phase::Running || ++m_restarts > 2) {
                fail(tr("head unit restarted the handshake"));
                return;
            }
            log(tr("head unit restarted the handshake (it got no usable reply); starting over"));
            QString error;
            if (!m_cryptor.init(Cryptor::Role::Server, m_config.certPath, m_config.keyPath, &error)) {
                fail(error);
                return;
            }
            m_encrypt = false;
            m_phase = Phase::WaitVersion;
        }
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
        if (legacyHeadUnit()) {
            // Head units before 1.6 get Gearhead's exact handshake (see the
            // service discovery request below); 1.6+ (e.g. Ford SYNC 4) keeps
            // the one verified in the field.
            m_cryptor.requestPeerCertificate();
            log(tr("older head unit (protocol < 1.6): using the stock phone's exact handshake"));
        }
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
        QByteArray small, medium, large;
        if (legacyHeadUnit()) {
            // Exactly what Gearhead sends (as F-Auto does too): 32/64/128 px PNG
            // icons, the label "Android" and "Manufacturer Model". The Hyundai
            // i30N (protocol 1.0) never answered our bare request.
            small = iconPng(32);
            medium = iconPng(64);
            large = iconPng(128);
            req.set_small_icon(small.constData(), size_t(small.size()));
            req.set_medium_icon(medium.constData(), size_t(medium.size()));
            req.set_large_icon(large.constData(), size_t(large.size()));
            req.set_label("Android");
            req.set_device_name(phoneModel().toStdString());
        } else {
            req.set_label(m_config.label.toStdString());
            req.set_device_name(m_config.deviceName.toStdString());
        }
        const std::string sdr = req.SerializeAsString();
        log(tr("ctrl tx service discovery request (%1 B: \"%2\" / \"%3\", icons %4/%5/%6 B)")
                .arg(sdr.size()).arg(QString::fromStdString(req.label()))
                .arg(QString::fromStdString(req.device_name()))
                .arg(small.size()).arg(medium.size()).arg(large.size()));
        send(0, MSG_SERVICE_DISCOVERY_REQUEST, sdr);
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
        noteUnhandled("control", 0, msg.id(), body.size());
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
    m_carSeen = true;
    log(tr("Car: %1 %2, head unit %3 %4")
            .arg(QString::fromStdString(resp.make()), QString::fromStdString(resp.model()),
                 QString::fromStdString(resp.head_unit_make()), QString::fromStdString(resp.head_unit_model())));

    // One line listing everything the head unit offers (what we use and what
    // we don't), so a log from a user's car answers "does it support X?".
    {
        QStringList names;
        bool wifi = false;
        for (const Service &svc : resp.services()) {
            if (svc.has_media_sink_service())
                names << (svc.media_sink_service().video_configs_size() > 0 ? tr("video") : tr("audio"));
            if (svc.has_sensor_source_service()) names << tr("sensors");
            if (svc.has_input_source_service()) names << tr("input");
            if (svc.has_media_source_service()) names << tr("mic");
            if (svc.has_navigation_status_service()) names << tr("cluster nav");
            if (svc.has_media_playback_service()) names << tr("now playing");
            if (svc.has_phone_status_service()) names << tr("phone status");
            if (svc.has_bluetooth_service()) names << tr("bluetooth");
            if (svc.has_radio_service()) names << tr("radio");
            if (svc.has_media_browser_service()) names << tr("media browser");
            if (svc.has_vendor_extension_service()) names << tr("vendor extension");
            if (svc.has_notification_service()) names << tr("notifications");
            if (svc.has_wifi_projection_service()) { names << tr("wifi projection"); wifi = true; }
        }
        log(tr("head unit services: %1").arg(names.join(QStringLiteral(", "))));
        log(wifi ? tr("head unit supports wireless Android Auto (wifi projection service)")
                 : tr("head unit does not advertise wireless Android Auto (no wifi projection service): wired only"));
    }

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
            // Battery-electric only when ELECTRIC (10) is listed and no burnable
            // fuel is: a plug-in hybrid (e.g. Peugeot 508 Hybrid lists petrol +
            // electric) would otherwise get "arriving with 0%" warnings once its
            // short electric range runs out, though it still has fuel.
            bool electric = false, combustion = false;
            QStringList fuels;
            for (int i = 0; i < ss.supported_fuel_types_size(); ++i) {
                const int f = ss.supported_fuel_types(i);
                fuels << QString::number(f);
                if (f == 10) // FUEL_TYPE_ELECTRIC
                    electric = true;
                else if ((f >= 1 && f <= 9) || f == 11) // petrol, diesel, LPG/CNG/LNG, hydrogen
                    combustion = true;
            }
            m_evCar = electric && !combustion;
            log(tr("head unit sensors: %1%2%3")
                    .arg(types.isEmpty() ? tr("(none)") : types.join(QLatin1Char(' ')),
                         fuels.isEmpty() ? QString() : tr(", fuel types %1").arg(fuels.join(QLatin1Char(' '))),
                         m_evCar ? tr(" [electric car]")
                                 : (electric ? tr(" [plug-in hybrid: not treated as electric]") : QString())));
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
            m_navWantsImages = nav.has_type() && nav.type() == 1; // IMAGE
            if (nav.has_image_options()) {
                m_navImgW = nav.image_options().width();
                m_navImgH = nav.image_options().height();
            }
            log(tr("instrument cluster navigation available (min interval %1 ms, %2%3)")
                    .arg(m_navMinIntervalMs)
                    .arg(m_navWantsImages ? tr("turn images") : tr("turn icons"))
                    .arg(m_navWantsImages ? QStringLiteral(" %1x%2").arg(m_navImgW).arg(m_navImgH)
                                          : QString()));
        } else if (svc.has_phone_status_service() && m_phoneStatusChannel < 0) {
            // The HU can show the in-AA call screen. Open the channel; we then
            // push call state (PhoneStatus) onto it and receive answer/reject.
            m_channels.insert(ch, ChannelInfo{Kind::PhoneStatus});
            log(tr("head unit can show calls in Android Auto (phone-status service)"));
        } else if (svc.has_media_playback_service() && m_mediaStatusChannel < 0) {
            // The car shows now playing on its own screens (cluster/native UI).
            m_channels.insert(ch, ChannelInfo{Kind::MediaStatus});
            log(tr("head unit shows now playing (media playback status service)"));
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
    m_stats.start();
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
            ch.acks += int(qMax(1u, ack.ack()));
        } else if (id == MEDIA_VIDEO_FOCUS_NOTIFICATION && ch.kind == Kind::Video) {
            onVideoFocus(ch, channel, body);
        } else {
            noteUnhandled("media", channel, id, body.size());
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
        } else {
            noteUnhandled("input", channel, id, body.size());
        }
        break;
    case Kind::Sensor:
        if (id == SENSOR_BATCH)
            onSensorBatch(body);
        else if (id == SENSOR_ERROR)
            log(tr("sensor error from head unit"));
        else
            noteUnhandled("sensor", channel, id, body.size());
        break;
    case Kind::Mic:
        if (id == MEDIA_MICROPHONE_RESPONSE)
            onMicResponse(ch, body);
        else if (id == MEDIA_DATA)
            onMicData(ch, channel, msg);
        else if (id == MEDIA_CONFIG)
            onMediaConfig(ch, channel, body);
        else
            noteUnhandled("mic", channel, id, body.size());
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
            noteUnhandled("cluster", channel, id, body.size());
        }
        break;
    case Kind::PhoneStatus:
        if (id == PHONE_STATUS_INPUT)
            onPhoneStatusInput(body);
        else
            noteUnhandled("phone-status", channel, id, body.size());
        break;
    case Kind::MediaStatus:
        if (id == MEDIA_PLAYBACK_INPUT)
            onMediaPlaybackInput(body);
        else
            noteUnhandled("now-playing", channel, id, body.size());
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

// Cluster buttons while the car shows our now playing: same action values as
// InstrumentClusterInput. LEFT/RIGHT skip tracks, ENTER toggles play/pause.
void Session::onMediaPlaybackInput(const QByteArray &body)
{
    MediaPlaybackInput in;
    if (!parse(in, body))
        return;
    log(tr("media playback input from car: action %1").arg(in.action()));
    switch (in.action()) {
    case 3: emit mediaKey(QStringLiteral("prev")); break;
    case 4: emit mediaKey(QStringLiteral("next")); break;
    case 5: emit mediaKey(QStringLiteral("play-pause")); break;
    default: break;
    }
}

void Session::updateMediaStatus(int state, const QString &source, int positionSec)
{
    if (state == m_mpState && source == m_mpSource && positionSec == m_mpPosition)
        return;
    const bool stateChanged = state != m_mpState;
    m_mpState = state;
    m_mpSource = source;
    m_mpPosition = qMax(0, positionSec);
    sendMediaStatus();
    if (stateChanged && m_mediaStatusChannel >= 0)
        log(tr("now playing -> car: state %1 (%2)").arg(state).arg(source));
}

void Session::updateMediaMetadata(const QString &title, const QString &artist, const QString &album,
                                  const QByteArray &art, int durationSec)
{
    if (title == m_mpTitle && artist == m_mpArtist && album == m_mpAlbum && art == m_mpArt
        && durationSec == m_mpDuration)
        return;
    m_mpTitle = title;
    m_mpArtist = artist;
    m_mpAlbum = album;
    m_mpArt = art;
    m_mpDuration = qMax(0, durationSec);
    m_mpMetaLogged = false;
    sendMediaMetadata();
}

void Session::sendMediaStatus()
{
    if (m_mediaStatusChannel < 0 || m_mpState == 0)
        return;
    MediaPlaybackStatus st;
    st.set_state(MediaPlaybackStatus::State(m_mpState));
    if (!m_mpSource.isEmpty())
        st.set_media_source(m_mpSource.toStdString());
    st.set_playback_seconds(uint32_t(m_mpPosition));
    st.set_shuffle(false);
    st.set_repeat(false);
    st.set_repeat_one(false);
    send(quint8(m_mediaStatusChannel), MEDIA_PLAYBACK_STATUS, st.SerializeAsString());
}

void Session::sendMediaMetadata()
{
    if (m_mediaStatusChannel < 0 || (m_mpTitle.isEmpty() && m_mpArtist.isEmpty()))
        return;
    MediaPlaybackMetadata md;
    md.set_song(m_mpTitle.toStdString());
    if (!m_mpArtist.isEmpty())
        md.set_artist(m_mpArtist.toStdString());
    if (!m_mpAlbum.isEmpty())
        md.set_album(m_mpAlbum.toStdString());
    if (!m_mpArt.isEmpty())
        md.set_album_art(m_mpArt.constData(), size_t(m_mpArt.size()));
    md.set_duration_seconds(uint32_t(m_mpDuration));
    md.set_rating(0);
    send(quint8(m_mediaStatusChannel), MEDIA_PLAYBACK_METADATA, md.SerializeAsString());
    if (!m_mpMetaLogged) {
        m_mpMetaLogged = true;
        log(tr("now playing -> car: %1 — %2 (%3 s%4)").arg(m_mpTitle, m_mpArtist).arg(m_mpDuration)
                .arg(m_mpArt.isEmpty() ? QString() : tr(", %1-byte cover").arg(m_mpArt.size())));
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
    case Kind::MediaStatus:
        m_mediaStatusChannel = channel;
        log(tr("media playback status channel ready (now playing on the car's screens)"));
        sendMediaMetadata();
        sendMediaStatus();
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
        ++ch.drops;
        m_keyFramePending = true;
        m_video->requestKeyFrame();
        return;
    }
    m_keyFramePending = false;
    ++ch.unacked;
    ++ch.txFrames;
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
    if (m_huMajor < 5 && ch.unacked >= ch.maxUnacked) {
        ++ch.drops;
        return;
    }
    ++ch.unacked;
    ++ch.txFrames;
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
        m_navFlush.stop();
        if (m_navActive) {
            sendNavClear();
            m_navActive = false;
            m_lastNavSendMs = 0;
        }
        m_navRerouting = false;
        return;
    }

    // Transition into guidance: announce ACTIVE immediately.
    if (!m_navActive) {
        sendNavStatus(1); // ACTIVE
        m_navActive = true;
        m_navRerouting = false;
        m_lastNavSendMs = 0; // force a guidance push right away
    }

    // Off route: tell the cluster a new route is coming (head units >= 1.6, as
    // Gearhead does). The next guidance update (new route) switches back.
    if (m_nav->rerouting()) {
        if (!m_navRerouting && navModernHu()) {
            sendNavStatus(3); // REROUTING
            m_navRerouting = true;
            log(tr("instrument cluster: rerouting"));
        }
        return;
    }
    if (m_navRerouting) {
        sendNavStatus(1); // ACTIVE again, with the new route
        m_navRerouting = false;
        m_lastNavSendMs = 0;
    }

    // Respect the cluster's advertised rate floor, but never drop the newest
    // update: if we are inside the window, send it when the window ends.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = m_lastNavSendMs == 0 ? 0 : m_lastNavSendMs + m_navMinIntervalMs - now;
    if (wait > 0) {
        if (!m_navFlush.isActive())
            m_navFlush.start(int(wait));
        return;
    }
    flushNavGuidance();
}

void Session::flushNavGuidance()
{
    m_navFlush.stop();
    if (!m_nav || m_navChannel < 0 || !m_nav->active() || !m_navActive || m_navRerouting)
        return;
    m_lastNavSendMs = QDateTime::currentMSecsSinceEpoch();
    sendNavGuidance();
    sendEnergyForecast();
}

void Session::sendNavStatus(int status)
{
    if (m_navChannel < 0)
        return;
    NavigationStatus st;
    st.set_status(status);
    send(quint8(m_navChannel), NAV_STATUS, st.SerializeAsString());
}

// Guidance ended (arrived / cancelled). STATUS(INACTIVE) alone is not enough on
// some head units: the cluster keeps drawing the last turn it was sent. So first
// overwrite the guidance with empty messages -- a NavigationState with no steps
// and no destination, and a NavigationCurrentPosition with no distances -- and
// only then report INACTIVE.
void Session::sendNavClear()
{
    if (m_navChannel < 0)
        return;
    send(quint8(m_navChannel), NAV_STATE, NavigationState().SerializeAsString());
    send(quint8(m_navChannel), NAV_CURRENT_POSITION, NavigationCurrentPosition().SerializeAsString());
    sendNavStatus(2); // INACTIVE
    log(tr("navigation ended: cleared instrument cluster guidance"));
}

// Legacy NextTurnEnum + turn side from a NavigationType (richer than the
// QtLocation direction: knows roundabouts, forks, ramps, merges, arrival).
static int navLegacyEventFromType(int t)
{
    switch (t) {
    case 1:                          return 1;  // DEPART
    case 2:                          return 2;  // NAME_CHANGE
    case 3: case 4: case 5: case 6:  return 3;  // keep / slight -> SLIGHT_TURN
    case 7: case 8:                  return 4;  // TURN
    case 9: case 10:                 return 5;  // SHARP_TURN
    case 11: case 12:                return 6;  // U_TURN
    case 25: case 26:                return 9;  // FORK
    case 27: case 28: case 29:       return 10; // MERGE
    case 30:                         return 11; // ROUNDABOUT_ENTER
    case 31:                         return 12; // ROUNDABOUT_EXIT
    case 32: case 33: case 34: case 35: return 13; // ROUNDABOUT_ENTER_AND_EXIT
    case 37:                         return 16; // FERRY_BOAT
    case 38:                         return 17; // FERRY_TRAIN
    case 39: case 40: case 41: case 42: return 19; // DESTINATION
    default:
        if (t >= 13 && t <= 20) return 7;  // ON_RAMP
        if (t >= 21 && t <= 24) return 8;  // OFF_RAMP
        return 14;                         // STRAIGHT
    }
}

static int navTurnSideFromType(int t)
{
    switch (t) {
    case 3: case 5: case 7: case 9: case 11: case 13: case 15: case 17: case 19:
    case 21: case 23: case 25: case 27: case 41: case 32: case 33:
        return 1; // LEFT (clockwise roundabouts drive on the left)
    case 4: case 6: case 8: case 10: case 12: case 14: case 16: case 18: case 20:
    case 22: case 24: case 26: case 28: case 42: case 34: case 35:
        return 2; // RIGHT
    default:
        return 3; // UNSPECIFIED
    }
}

// Bend of a NavigationType in degrees (0 = straight, + = right), for the arrow.
static int navTurnAngle(int t)
{
    switch (t) {
    case 5: case 13: case 21: case 3: case 25: case 27: return -45;
    case 6: case 14: case 22: case 4: case 26: case 28: return 45;
    case 7: case 15: case 23: return -90;
    case 8: case 16: case 24: return 90;
    case 9: case 17: return -135;
    case 10: case 18: return 135;
    default: return 0;
    }
}

// White-on-transparent turn arrow PNG for head units whose cluster wants
// images (NavigationStatusService type IMAGE) in the legacy turn event.
static QByteArray navTurnImage(int type, int exitNo, int w, int h)
{
    if (w <= 0 || h <= 0) { w = 128; h = 128; }
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = qMin(w, h);
    QPen pen(Qt::white, s * 0.12, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.translate(w / 2.0, h / 2.0);
    auto head = [&](QPointF tip, qreal deg) {        // filled arrowhead at tip
        p.save();
        p.translate(tip);
        p.rotate(deg);
        QPainterPath a;
        a.moveTo(0, -s * 0.16);
        a.lineTo(s * 0.16, s * 0.04);
        a.lineTo(-s * 0.16, s * 0.04);
        a.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawPath(a);
        p.restore();
    };
    const bool dest = type >= 39 && type <= 42;
    const bool round = type >= 30 && type <= 35;
    const bool uturn = type == 11 || type == 12 || type == 19 || type == 20;
    if (dest) {                                      // destination: ring + dot
        p.drawEllipse(QPointF(0, -s * 0.05), s * 0.22, s * 0.22);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(0, -s * 0.05), s * 0.06, s * 0.06);
    } else if (round) {                              // roundabout: ring, stem, exit #
        p.drawEllipse(QPointF(0, -s * 0.08), s * 0.18, s * 0.18);
        p.drawLine(QPointF(0, s * 0.42), QPointF(0, s * 0.10));
        if (exitNo > 0) {
            QFont f = p.font();
            f.setPixelSize(int(s * 0.2));
            f.setBold(true);
            p.setFont(f);
            p.drawText(QRectF(-s * 0.18, -s * 0.26, s * 0.36, s * 0.36), Qt::AlignCenter,
                       QString::number(exitNo));
        }
    } else if (uturn) {                              // U-turn hook
        const qreal dx = (type == 11 || type == 19) ? -1 : 1;
        QPainterPath u;
        u.moveTo(s * 0.15 * -dx, s * 0.40);
        u.lineTo(s * 0.15 * -dx, -s * 0.10);
        u.arcTo(QRectF(-s * 0.15, -s * 0.25, s * 0.30, s * 0.30), dx < 0 ? 0 : 180, dx < 0 ? 180 : -180);
        u.lineTo(s * 0.15 * dx, s * 0.10);
        p.drawPath(u);
        head(QPointF(s * 0.15 * dx, s * 0.22), 180);
    } else {                                         // stem up, then bend by angle
        const qreal deg = navTurnAngle(type);
        // Sharp turns bend back down: raise the knee and shift the stem away
        // from the turn so the arrowhead has room.
        const bool sharp = qAbs(deg) > 90;
        const QPointF knee(sharp ? (deg > 0 ? -s * 0.14 : s * 0.14) : 0, sharp ? -s * 0.18 : s * 0.02);
        p.drawLine(QPointF(knee.x(), s * 0.42), knee);
        const qreal rad = qDegreesToRadians(deg);
        const qreal len = s * 0.28;
        const QPointF tip(knee.x() + len * qSin(rad), knee.y() - len * qCos(rad));
        p.drawLine(knee, tip);
        head(tip, deg);
    }
    p.end();
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
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
    const QVariantList upcoming = m_nav->upcoming();
    const QVariantMap first = upcoming.value(0).toMap();
    const int firstType = first.isEmpty() ? navTypeFromDir(dir) : first.value(QStringLiteral("type")).toInt();
    const int firstExit = first.value(QStringLiteral("exit")).toInt();

    // Head units on protocol >= 1.6 get the modern STATE + CURRENT_POSITION,
    // older ones only the legacy TURN/DISTANCE pair -- exactly what Gearhead
    // does. (Sending both confuses some clusters, e.g. stale turns.)
    if (navModernHu()) {
        // NavigationState: the next up-to-3 maneuvers (+ lanes) and destination.
        NavigationState state;
        if (upcoming.isEmpty()) {
            NavigationStep *step = state.add_steps();
            step->mutable_maneuver()->set_type(navTypeFromDir(dir));
            if (!instr.empty())
                step->mutable_cue()->add_alternate_text(instr);
        }
        for (int i = 0; i < upcoming.size() && i < 3; ++i) {
            const QVariantMap u = upcoming.at(i).toMap();
            NavigationStep *step = state.add_steps();
            NavigationManeuver *m = step->mutable_maneuver();
            m->set_type(u.value(QStringLiteral("type")).toInt());
            const int exitNo = u.value(QStringLiteral("exit")).toInt();
            if (exitNo > 0)
                m->set_roundabout_exit_number(exitNo);
            const QString stepRoad = u.value(QStringLiteral("road")).toString();
            if (!stepRoad.isEmpty())
                step->mutable_road()->set_name(stepRoad.toStdString());
            const QString text = u.value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                step->mutable_cue()->add_alternate_text(text.toStdString());
            const QVariantList lanes = u.value(QStringLiteral("lanes")).toList();
            for (const QVariant &lv : lanes) {
                NavigationLane *lane = step->add_lanes();
                for (const QVariant &dv : lv.toList()) {
                    const QVariantMap d = dv.toMap();
                    NavigationLaneDirection *ld = lane->add_lane_directions();
                    ld->set_shape(d.value(QStringLiteral("shape")).toInt());
                    ld->set_is_highlighted(d.value(QStringLiteral("hl")).toBool());
                }
            }
        }
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
        return;
    }

    // Legacy pair for head units older than 1.6.
    NavigationNextTurnEvent turn;
    turn.set_road(instr.empty() ? road : instr);
    turn.set_turn_side(first.isEmpty() ? navTurnSide(dir) : navTurnSideFromType(firstType));
    turn.set_event(first.isEmpty() ? navLegacyEvent(dir) : navLegacyEventFromType(firstType));
    if (firstExit > 0)
        turn.set_turn_number(firstExit);
    if (m_navWantsImages) {
        const QByteArray png = navTurnImage(firstType, firstExit, m_navImgW, m_navImgH);
        turn.set_image(png.constData(), size_t(png.size()));
    }
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

// EV battery forecast to the cluster (what Google Maps pushes as
// VehicleEnergyForecast). AAP has no battery capacity, and the forecast is in
// Wh, so it needs the capacity the user entered in the phone app. The arrival
// energy comes from the car's own range (which embeds its consumption model):
//   arrival = capacity * SoC% * (range - distanceToGo) / range
void Session::sendEnergyForecast()
{
    if (!m_nav || m_navChannel < 0 || !m_evCar)
        return;
    const double capKwh = m_screen ? m_screen->evCapacityKwh() : 0.0;
    QString why;
    if (!energyForecastHu())
        why = tr("head unit protocol %1.%2 is too old (Google needs 5.1+)").arg(m_huMajor).arg(m_huMinor);
    else if (capKwh <= 0)
        why = tr("no battery capacity set in the phone app");
    else if (m_fuelPct < 0 || m_fuelRangeKm <= 0)
        why = tr("the car hasn't reported charge level and range yet");
    if (!why.isEmpty()) {
        if (!m_forecastLogged) {
            m_forecastLogged = true;
            log(tr("instrument cluster: no battery forecast — %1").arg(why));
        }
        return;
    }
    const int remM = qMax(0, m_nav->remainingMeters());
    const double rangeM = m_fuelRangeKm * 1000.0;
    const double nowWh = capKwh * 1000.0 * m_fuelPct / 100.0;
    const double arrivalWh = qMax(0.0, nowWh * (rangeM - remM) / rangeM);

    VehicleEnergyForecast fc;
    EnergyAtDistance *next = fc.mutable_energy_at_next_stop();
    next->set_distance_meters(remM);
    next->set_arrival_battery_energy_wh(int(qRound(arrivalWh)));
    if (m_nav->remainingSeconds() > 0)
        next->set_time_to_arrival_seconds(m_nav->remainingSeconds());
    EnergyAtDistance *empty = fc.mutable_distance_to_empty();
    empty->set_distance_meters(int(rangeM));
    empty->set_arrival_battery_energy_wh(0);

    NavigationVehicleEnergyForecast msg;
    const std::string body = fc.SerializeAsString();
    msg.set_forecast(body);
    send(quint8(m_navChannel), NAV_ENERGY_FORECAST, msg.SerializeAsString());
    if (!m_forecastSentLogged) {
        m_forecastSentLogged = true;
        log(tr("instrument cluster: sending battery forecast (%1 Wh at arrival, %2 kWh pack)")
                .arg(qRound(arrivalWh)).arg(capKwh));
    }
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
        // Reflect explicit play/pause in the (mock) car UI. Not "play-pause":
        // that forced the icon to "playing" on every press; the real state
        // comes from the player (MPRIS) a moment later.
        if (media == QLatin1String("play") || media == QLatin1String("pause"))
            m_screen->setCarProperty(QStringLiteral("playing"), media == QLatin1String("play"));
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
        m_fuelPct = pct;
        m_fuelRangeKm = rangeKm;
        emit energyStatus(pct, rangeKm, m_evCar, f.low_fuel_warning());
    }
}

} // namespace aap
