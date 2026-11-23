#include "audiosource.h"

#include <QHash>
#include <QList>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QProcess>
#include <QVariant>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

Q_LOGGING_CATEGORY(lcAudio, "aap.audio")

// Unique PulseAudio client name for our capture, used to find and re-route the
// stream afterwards (droid parks every record stream on the microphone).
static const char kClientName[] = "sfauto-media";

// Config uses the token "@DEFAULT_MONITOR@" to mean "the monitor source of the
// default sink" — i.e. whatever the phone is *playing* (Spotify etc.), so it can
// be streamed to the car as media audio. It must NOT be left unresolved: an empty
// or bogus pulsesrc device falls back to the default *source*, which is the
// microphone, and the car then hears the phone's mic instead of the music.
// On Sailfish the playback monitor is "sink.primary_output.monitor"; we ask
// PulseAudio for the real default sink at runtime and append ".monitor", falling
// back to that known name if pactl is unavailable.
static QStringList pactl(const QStringList &args)
{
    QProcess p;
    p.start(QStringLiteral("pactl"), args);
    if (!p.waitForFinished(1500) || p.exitStatus() != QProcess::NormalExit)
        return {};
    return QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
}

// Index of a source by name, from `pactl list short sources` (-1 if not found).
// Used to tell whether our capture is already on the wanted monitor, so a
// routine re-assert does not needlessly re-route the stream.
static int sourceIndexByName(const QString &name)
{
    if (name.isEmpty())
        return -1;
    for (const QString &l : pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")})) {
        const QStringList c = l.split(QLatin1Char('\t'));
        if (c.size() >= 2 && c.at(1).trimmed() == name)
            return c.at(0).trimmed().toInt();
    }
    return -1;
}

// Our dedicated capture sink. Media ("player") streams are redirected into it
// and we record its monitor; the phone speaker stays silent and the capture is
// deterministic (it never drifts onto the mic the way chasing the live sink
// did). A distinctive name lets a stale one (left by a crash) be found/reused.
static const char kCaptureSink[] = "carfish_capture";

// A real hardware sink to move streams back onto when we stop, so the phone
// plays through its speaker again. Prefer the Android media sink, then the
// primary output, then anything that is not a null/monitor sink.
static QString hardwarePlaybackSink()
{
    QStringList names;
    for (const QString &l : pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")})) {
        const QStringList c = l.split(QLatin1Char('\t'));
        if (c.size() >= 2)
            names << c.at(1).trimmed();
    }
    for (const QString &pref : {QStringLiteral("sink.deep_buffer"), QStringLiteral("sink.primary_output")})
        if (names.contains(pref))
            return pref;
    for (const QString &n : names)
        if (!n.isEmpty() && !n.contains(QLatin1String("null")) && !n.contains(QLatin1String(kCaptureSink))
            && !n.endsWith(QLatin1String(".monitor")))
            return n;
    return QStringLiteral("sink.primary_output");
}

AudioSource::AudioSource(QObject *parent)
    : QObject(parent)
{
    // Maintain the capture routing for the whole session: the user frequently
    // starts the music well AFTER the car session is up, and droid keeps trying
    // to park the record stream on the microphone, so we re-assert periodically.
    m_watch.setInterval(2000);
    connect(&m_watch, &QTimer::timeout, this, &AudioSource::followActiveSink);
    // PulseAudio events (stream moved/started/paused) trigger an immediate
    // re-check; the 2 s watch stays as a fallback.
    m_recheck.setSingleShot(true);
    m_recheck.setInterval(60);
    connect(&m_recheck, &QTimer::timeout, this, &AudioSource::followActiveSink);
}

AudioSource::~AudioSource()
{
    stop();
}

bool AudioSource::start(const QString &device, int sampleRate, int channels, QString *error)
{
    stop();
    m_sampleRate = sampleRate;
    m_channels = channels;
    // Loopback mode: create a dedicated capture sink, redirect media into it,
    // and record its monitor. Otherwise capture the explicit device as given.
    m_loopback = (device == QLatin1String("@DEFAULT_MONITOR@") || device.isEmpty());
    QString src = device;
    if (m_loopback) {
        if (!ensureCaptureSink(&src, error))
            return false;
        adoptPlayers(); // move whatever is already playing into the capture sink
    }
    if (!buildPipeline(src, error)) {
        if (m_loopback)
            releasePlayers();
        return false;
    }
    if (m_loopback) {
        m_watch.start();
        startPulseEvents();
    }
    return true;
}

// Watch PulseAudio for stream changes so a capture re-parked onto the mic is
// silenced and moved back within milliseconds, not at the next 2 s tick.
void AudioSource::startPulseEvents()
{
    stopPulseEvents();
    m_events = new QProcess(this);
    connect(m_events, &QProcess::readyReadStandardOutput, this, &AudioSource::onPulseEvents);
    m_events->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
}

void AudioSource::stopPulseEvents()
{
    if (!m_events)
        return;
    m_events->disconnect(this);
    m_events->kill();
    m_events->waitForFinished(500);
    delete m_events;
    m_events = nullptr;
    m_eventBuf.clear();
}

void AudioSource::onPulseEvents()
{
    if (!m_events)
        return;
    m_eventBuf += m_events->readAllStandardOutput();
    bool relevant = false, urgent = false;
    int nl;
    while ((nl = m_eventBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_eventBuf.left(nl);
        m_eventBuf.remove(0, nl + 1);
        // e.g. "Event 'change' on source-output #42", "Event 'new' on sink-input #7"
        if (line.contains("source-output")) {
            relevant = true;
            // Our own record stream changed: droid may just have moved it onto
            // the mic, so check now rather than after the debounce. It is NOT
            // muted on the event itself any more: PulseAudio reports routine
            // changes on the stream too, and muting on each one (until a
            // ~150 ms re-check verified it) cut the music in short, frequent
            // gaps. The check mutes it only if it really left the monitor.
            if (m_captureIdx >= 0 && line.endsWith("#" + QByteArray::number(m_captureIdx)))
                urgent = true;
        } else if (line.contains("sink-input")) {
            relevant = true;
        }
    }
    if (urgent) {
        m_recheck.stop();
        followActiveSink();
    } else if (relevant && !m_recheck.isActive()) {
        m_recheck.start();
    }
}

// Pick (or, as a last resort, create) a null capture sink and return its
// monitor source name. droid PulseAudio already ships a "sink.null", so we
// reuse that rather than loading our own module (which can fail on the device);
// we only create one if no null sink exists, and we unload only what we made.
bool AudioSource::ensureCaptureSink(QString *monitor, QString *error)
{
    auto listSinks = [] {
        QStringList names;
        for (const QString &l : pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")})) {
            const QStringList c = l.split(QLatin1Char('\t'));
            if (c.size() >= 2)
                names << c.at(1).trimmed();
        }
        return names;
    };
    QStringList names = listSinks();

    // Prefer an existing null sink: the stock "sink.null", a leftover of ours,
    // then any other "*null*" sink. These are reused as-is (not unloaded on stop).
    QString found;
    if (names.contains(QLatin1String(kCaptureSink)))
        found = QLatin1String(kCaptureSink);
    else if (names.contains(QStringLiteral("sink.null")))
        found = QStringLiteral("sink.null");
    else {
        for (const QString &n : names)
            if (n.contains(QLatin1String("null"))) { found = n; break; }
    }
    if (!found.isEmpty()) {
        m_captureSink = found;
        m_captureModule.clear(); // not ours — leave it loaded on stop
        *monitor = found + QStringLiteral(".monitor");
        qCInfo(lcAudio) << "using existing capture sink" << found;
        return true;
    }

    // No null sink anywhere: create one (tracked so we unload it on stop).
    QProcess p;
    p.start(QStringLiteral("pactl"), {QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
                                      QStringLiteral("sink_name=%1").arg(QLatin1String(kCaptureSink))});
    if (p.waitForFinished(2000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        m_captureModule = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    if (!listSinks().contains(QLatin1String(kCaptureSink))) {
        if (error)
            *error = QStringLiteral("no null sink available and could not create one");
        return false;
    }
    m_captureSink = QLatin1String(kCaptureSink);
    *monitor = m_captureSink + QStringLiteral(".monitor");
    qCInfo(lcAudio) << "created capture sink" << m_captureSink << "module" << m_captureModule;
    return true;
}

// Redirect media ("player") streams into the capture sink so their audio is
// what we record — and so new tracks the user starts AFTER the session is up
// are caught too (this runs on every watch tick). Keyed on PulseAudio's
// policy.group=player (Sailfish/droid tag for music/video apps); as a fallback
// we also adopt uncorked streams sitting on the known media sinks.
void AudioSource::adoptPlayers()
{
    if (m_captureSink.isEmpty())
        return;
    m_sinkNames = parseSinkNames(pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")}));
    adoptFrom(pactl({QStringLiteral("list"), QStringLiteral("sink-inputs")}), false);
}

QHash<int, QString> AudioSource::parseSinkNames(const QStringList &shortSinks)
{
    QHash<int, QString> names;
    for (const QString &l : shortSinks) {
        const QStringList c = l.split(QLatin1Char('\t'));
        if (c.size() >= 2)
            names.insert(c.at(0).trimmed().toInt(), c.at(1).trimmed());
    }
    return names;
}

// Parse `pactl list sink-inputs`, move media streams into the capture sink and
// count the uncorked ones already there (the forward/mute gate).
void AudioSource::adoptFrom(const QStringList &lines, bool async)
{
    struct In { int idx = -1; QString sink; bool corked = false; bool player = false; QString app; };
    QList<In> inputs;
    In cur;
    bool have = false;
    auto flush = [&] { if (have && cur.idx >= 0) inputs << cur; cur = In(); have = false; };
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith(QLatin1String("Sink Input #"))) {
            flush();
            cur.idx = l.midRef(12).toInt();
            have = true;
        } else if (!have) {
            continue;
        } else if (l.startsWith(QLatin1String("Sink:"))) {
            cur.sink = l.mid(5).trimmed();
        } else if (l.startsWith(QLatin1String("Corked:"))) {
            cur.corked = l.contains(QLatin1String("yes"));
        } else if (l.startsWith(QLatin1String("application.name = "))) {
            cur.app = l.mid(19);
        } else if (l.contains(QLatin1String("policy.group =")) && l.contains(QLatin1String("player"))) {
            cur.player = true;
        } else if (l.startsWith(QLatin1String("media.role = ")) &&
                   (l.contains(QLatin1String("music")) || l.contains(QLatin1String("video")) || l.contains(QLatin1String("game")))) {
            cur.player = true;
        }
    }
    flush();

    int moved = 0, playing = 0;
    for (const In &in : inputs) {
        const QString name = m_sinkNames.value(in.sink.toInt(), in.sink);
        if (name == m_captureSink) {
            if (!in.corked)
                ++playing;
            continue; // already captured
        }
        const bool onMediaSink = name.contains(QLatin1String("deep_buffer"))
            || name.contains(QLatin1String("primary_output"));
        if (!in.player && !(onMediaSink && !in.corked))
            continue; // leave ringtones/alarms/system sounds where they are
        const QStringList args = {QStringLiteral("move-sink-input"), QString::number(in.idx), m_captureSink};
        if (async) {
            pactlAsync(args, [](bool, const QStringList &) {});
        } else if (QProcess::execute(QStringLiteral("pactl"), args) != 0) {
            continue;
        }
        ++moved;
        if (!in.corked)
            ++playing;
        qCInfo(lcAudio) << "adopted media stream" << in.idx << in.app.trimmed() << "->" << m_captureSink;
    }
    m_playingInputs = playing;
    if (moved)
        emit captureChanged(m_device);
}

// Run pactl WITHOUT blocking: this thread also sends the audio and video to the
// car, and the routine 2 s re-check used to block it for ~100 ms (4-5 pactl
// runs of 15-35 ms each on the phone), which starved the car's audio buffer.
// Callbacks from a previous start() are dropped via the generation counter.
void AudioSource::pactlAsync(const QStringList &args, std::function<void(bool, const QStringList &)> done)
{
    auto *p = new QProcess(this);
    const int gen = m_gen;
    auto finish = [this, p, done, gen](bool ok) {
        if (p->property("done").toBool())
            return;
        p->setProperty("done", true);
        const QStringList lines = ok ? QString::fromUtf8(p->readAllStandardOutput()).split(QLatin1Char('\n'))
                                     : QStringList();
        p->deleteLater();
        if (gen == m_gen)
            done(ok, lines);
    };
    connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus st) { finish(st == QProcess::NormalExit && code == 0); });
    connect(p, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false); });
    QTimer::singleShot(3000, p, [p] { p->kill(); });
    p->start(QStringLiteral("pactl"), args);
}

// On stop: move any streams still on the capture sink back to a real speaker
// sink, then unload the null module we created so the phone audio is normal.
void AudioSource::releasePlayers()
{
    if (m_captureSink.isEmpty())
        return;
    const QString hw = hardwarePlaybackSink();
    QHash<int, QString> sinkName;
    for (const QString &l : pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")})) {
        const QStringList c = l.split(QLatin1Char('\t'));
        if (c.size() >= 2)
            sinkName.insert(c.at(0).trimmed().toInt(), c.at(1).trimmed());
    }
    int idx = -1;
    for (const QString &raw : pactl({QStringLiteral("list"), QStringLiteral("sink-inputs")})) {
        const QString l = raw.trimmed();
        if (l.startsWith(QLatin1String("Sink Input #"))) {
            idx = l.midRef(12).toInt();
        } else if (idx >= 0 && l.startsWith(QLatin1String("Sink:"))) {
            if (sinkName.value(l.midRef(5).trimmed().toInt()) == m_captureSink)
                QProcess::execute(QStringLiteral("pactl"), {QStringLiteral("move-sink-input"), QString::number(idx), hw});
            idx = -1;
        }
    }
    if (!m_captureModule.isEmpty()) {
        QProcess::execute(QStringLiteral("pactl"), {QStringLiteral("unload-module"), m_captureModule});
        qCInfo(lcAudio) << "unloaded capture sink module" << m_captureModule;
    }
    m_captureModule.clear();
    m_captureSink.clear();
}

bool AudioSource::buildPipeline(const QString &src, QString *error)
{
    teardownPipeline();
    m_device = src;
    const int sampleRate = m_sampleRate;
    const int channels = m_channels;
    // Same frame sizes as the phone app: 2048 samples at 44.1/48 kHz,
    // 1024 samples at 16 kHz.
    const int chunkBytes = (sampleRate >= 44100 ? 2048 : 1024) * channels * 2;
    // client-name lets us find this exact capture stream in `pactl` afterwards so
    // we can force it onto the monitor (see moveCaptureToMonitor).
    //
    // Buffering: the old 40 ms capture ring (2 x 20 ms) overran whenever the
    // capture thread was late by more than ~40 ms (software H.264 encoding and
    // the UI share the CPU), dropping samples = short, frequent dropouts. The
    // 20 ms fragments also made the null sink, and so the Android player
    // feeding it, run in tiny blocks that underrun easily. 400 ms of ring in
    // 40 ms fragments costs a little latency, which music does not mind.
    // do-timestamp is gone (arrival-time stamps are jittery and made
    // audiobuffersplit see discontinuities); gapless keeps it from resyncing.
    const QString desc = QStringLiteral(
        "pulsesrc client-name=%5 device=%1 buffer-time=400000 latency-time=40000 "
        "! audioconvert ! audioresample "
        "! audio/x-raw,format=S16LE,layout=interleaved,rate=%2,channels=%3 "
        "! audiobuffersplit output-buffer-size=%4 gapless=true "
        "! appsink name=sink sync=false max-buffers=50 drop=true")
            .arg(src).arg(sampleRate).arg(channels).arg(chunkBytes).arg(kClientName);

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline) {
        *error = QString::fromUtf8(err ? err->message : "gst_parse_launch failed");
        g_clear_error(&err);
        return false;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    GstAppSinkCallbacks cb = {};
    cb.new_sample = [](GstAppSink *s, gpointer self) -> GstFlowReturn {
        return GstFlowReturn(AudioSource::onNewSample(s, self));
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
    gst_object_unref(sink);

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("cannot start audio capture");
        teardownPipeline();
        return false;
    }
    qCInfo(lcAudio) << "capturing" << src << sampleRate << "Hz" << channels << "ch";
    // On this droid device PulseAudio ignores the requested monitor and binds the
    // capture to the hardware microphone (source.primary_input) — which is why the
    // car heard the mic. Force the stream back onto the monitor once PulseAudio has
    // created the source-output (a moment after PLAYING).
    QTimer::singleShot(350, this, [this] { moveCaptureToMonitor(); });
    return true;
}

// Move our capture stream onto the intended monitor source. `pactl` identifies it
// by our unique client-name; droid otherwise parks every record stream on the mic.
void AudioSource::moveCaptureToMonitor()
{
    if (!m_pipeline || m_device.isEmpty()) {
        m_onMonitor = false;
        return;
    }
    m_monitorIdx = sourceIndexByName(m_device);
    pinFrom(pactl({QStringLiteral("list"), QStringLiteral("source-outputs")}), false);
}

// Parse `pactl list source-outputs`: find our record stream, move it back onto
// the monitor if droid parked it elsewhere (the mic), and set the gate.
void AudioSource::pinFrom(const QStringList &lines, bool async)
{
    if (!m_pipeline || m_device.isEmpty()) {
        m_onMonitor = false;
        return;
    }
    const int target = m_monitorIdx;
    int idx = -1, curSource = -1, moved = 0;
    bool found = false, verified = false;
    for (const QString &raw : lines) {
        const QString l = raw.trimmed();
        if (l.startsWith(QLatin1String("Source Output #"))) {
            idx = l.midRef(15).toInt();
            curSource = -1;
        } else if (idx >= 0 && l.startsWith(QLatin1String("Source:"))) {
            curSource = l.midRef(7).trimmed().toInt();
        } else if (idx >= 0 && l.startsWith(QLatin1String("application.name = "))
                   && l.contains(QLatin1String(kClientName))) {
            found = true;
            m_captureIdx = idx;
            // Only move when it is not already on the target monitor: a routine
            // re-assert of an already-correct stream would needlessly re-route it
            // (and could glitch the audio). When the monitor's index is unknown,
            // move anyway to be safe.
            if (target < 0 || curSource != target) {
                ++moved;
                const QStringList args = {QStringLiteral("move-source-output"), QString::number(idx), m_device};
                if (async) {
                    // Off the monitor (on the mic): mute until the move lands.
                    m_onMonitor = false;
                    pactlAsync(args, [this](bool ok, const QStringList &) {
                        m_onMonitor = ok && m_pipeline;
                        if (ok)
                            emit captureChanged(m_device);
                    });
                    verified = false;
                } else {
                    verified = QProcess::execute(QStringLiteral("pactl"), args) == 0;
                }
            } else {
                verified = true;
            }
        }
    }
    if (!async || !moved)
        m_onMonitor = verified;
    if (moved && !async) {
        qCInfo(lcAudio) << "forced capture onto" << m_device;
        emit captureChanged(m_device);
    } else if (moved) {
        qCInfo(lcAudio) << "capture was off the monitor; moving it back onto" << m_device;
    } else if (!found) {
        qCWarning(lcAudio) << "capture stream not found to move onto" << m_device;
    }
}

// Keep audio flowing to the car for the whole session. Runs on the watch timer
// and does two cheap things, neither of which rebuilds the pipeline (so no
// choppiness):
//   * adopt any media stream that has appeared since — the user often starts
//     the music minutes after the session is up, and a new track can land on a
//     media sink; and
//   * re-pin our own record stream onto the capture sink's monitor, because
//     droid keeps re-parking record streams on the microphone over time and
//     across the sink's suspend/resume.
void AudioSource::followActiveSink()
{
    if (!m_pipeline || !m_loopback)
        return;
    if (m_checkBusy) {
        m_checkAgain = true;
        return;
    }
    m_checkBusy = true;
    // Our own record stream first (the mic guard), then the players. A failed
    // or timed-out pactl leaves the previous state alone instead of reading as
    // "nothing playing" (which muted the car until the next tick).
    pactlAsync({QStringLiteral("list"), QStringLiteral("source-outputs")}, [this](bool ok, const QStringList &l) {
        if (ok)
            pinFrom(l, true);
        pactlAsync({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")},
                   [this](bool ok, const QStringList &l) {
            if (ok)
                m_sinkNames = parseSinkNames(l);
            pactlAsync({QStringLiteral("list"), QStringLiteral("sink-inputs")}, [this](bool ok, const QStringList &l) {
                if (ok && !m_captureSink.isEmpty())
                    adoptFrom(l, true);
                m_checkBusy = false;
                if (m_checkAgain) {
                    m_checkAgain = false;
                    m_recheck.start();
                }
            });
        });
    });
}

void AudioSource::stop()
{
    ++m_gen; // drop callbacks of pactl runs still in flight
    m_checkBusy = m_checkAgain = false;
    m_watch.stop();
    m_recheck.stop();
    stopPulseEvents();
    teardownPipeline();
    if (m_loopback)
        releasePlayers();
    m_loopback = false;
}

void AudioSource::teardownPipeline()
{
    m_onMonitor = false;
    m_captureIdx = -1;
    if (!m_pipeline)
        return;
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

int AudioSource::onNewSample(GstAppSink *sink, void *self)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_EOS;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        const QByteArray data(reinterpret_cast<const char *>(map.data), int(map.size));
        const quint64 ts = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) / 1000 : 0;
        gst_buffer_unmap(buf, &map);
        QMetaObject::invokeMethod(static_cast<AudioSource *>(self), "handleChunk", Qt::QueuedConnection,
                                  Q_ARG(QByteArray, data), Q_ARG(quint64, ts));
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void AudioSource::handleChunk(const QByteArray &data, quint64 timestampUs)
{
    // Loopback mode: only real media may reach the car. Until our capture is
    // verified on the monitor (droid starts it on the mic and can move it back
    // there later), or when nothing is playing into the capture sink, send
    // silence instead -- same size, so the stream timing is unchanged.
    const bool pass = !m_loopback || (m_onMonitor && m_playingInputs > 0);
    if (pass != !m_gated) {
        m_gated = !pass;
        qCInfo(lcAudio) << (pass ? "media audio: forwarding" : "media audio: muted")
                        << "(on monitor:" << m_onMonitor << ", playing streams:" << m_playingInputs << ")";
    }
    emit pcm(pass ? data : QByteArray(data.size(), '\0'), timestampUs);
}
