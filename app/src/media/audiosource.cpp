#include "audiosource.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QProcess>

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

// The sink index feeding the first actively-playing (uncorked) stream, or -1.
static int activeSinkIndex()
{
    int sink = -1;
    for (const QString &raw : pactl({QStringLiteral("list"), QStringLiteral("sink-inputs")})) {
        const QString l = raw.trimmed();
        if (l.startsWith(QLatin1String("Sink Input #")))
            sink = -1;
        else if (l.startsWith(QLatin1String("Sink:")))
            sink = l.midRef(5).trimmed().toInt();
        else if (l.startsWith(QLatin1String("Corked:")) && !l.contains(QLatin1String("yes")) && sink >= 0)
            return sink; // first uncorked input wins
    }
    return -1;
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

// name/state lookups from `pactl list short sinks` (tab-separated:
// index, name, module, sample-spec, state).
static QString sinkField(int wantIndex, const QString &wantRunning)
{
    for (const QString &l : pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")})) {
        const QStringList c = l.split(QLatin1Char('\t'));
        if (c.size() < 2)
            continue;
        const QString name = c.at(1).trimmed();
        if (wantIndex >= 0 && c.at(0).trimmed().toInt() == wantIndex)
            return name;
        if (!wantRunning.isEmpty() && c.size() >= 5 && c.last().trimmed() == wantRunning
                && !name.contains(QLatin1String("null")))
            return name;
    }
    return QString();
}

// The monitor of whatever is *actively playing*, or empty if nothing is.
// On Sailfish, Android apps (e.g. Spotify via App Support) route to
// sink.deep_buffer and native apps to sink.primary_output; the "default" sink is
// often sink.null (silent), so we must NOT trust the default. Returns empty when
// no stream is playing so the caller can keep its current pick instead of
// snapping to a silent sink.
static QString resolveActiveMonitor()
{
    const int idx = activeSinkIndex();
    if (idx >= 0) {
        const QString name = sinkField(idx, QString());
        // Never follow the null sink: it is silent, and Android briefly parks
        // streams there, which would otherwise make us flap off the real audio.
        if (!name.isEmpty() && !name.contains(QLatin1String("null")))
            return name + QStringLiteral(".monitor");
    }
    const QString running = sinkField(-1, QStringLiteral("RUNNING"));
    if (!running.isEmpty())
        return running + QStringLiteral(".monitor");
    return QString();
}

// Best guess when nothing is playing yet. On this phone Android media (the
// common case: Spotify) plays to sink.deep_buffer, whose monitor stays valid
// across suspend/resume, so capturing it before playback starts simply yields
// silence until the music begins — then real audio, with no reconfigure needed.
static QString resolveDefaultMonitor()
{
    const QString active = resolveActiveMonitor();
    return active.isEmpty() ? QStringLiteral("sink.deep_buffer.monitor") : active;
}

AudioSource::AudioSource(QObject *parent)
    : QObject(parent)
{
    // Maintain the capture routing for the whole session: the user frequently
    // starts the music well AFTER the car session is up, and droid keeps trying
    // to park the record stream on the microphone, so we re-assert periodically.
    m_watch.setInterval(2000);
    connect(&m_watch, &QTimer::timeout, this, &AudioSource::followActiveSink);
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
    // Resolve the "capture what's playing" token to a concrete monitor source, so
    // we never fall through to pulsesrc's default (the microphone). When we are
    // following the default, keep polling so we can switch to the sink that
    // actually starts playing after the session is already up.
    m_followDefault = (device == QLatin1String("@DEFAULT_MONITOR@") || device.isEmpty());
    const QString src = m_followDefault ? resolveDefaultMonitor() : device;
    if (!buildPipeline(src, error))
        return false;
    if (m_followDefault)
        m_watch.start();
    return true;
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
    const QString desc = QStringLiteral(
        "pulsesrc client-name=%5 device=%1 do-timestamp=true buffer-time=40000 latency-time=20000 "
        "! audioconvert ! audioresample "
        "! audio/x-raw,format=S16LE,layout=interleaved,rate=%2,channels=%3 "
        "! audiobuffersplit output-buffer-size=%4 "
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
    if (!m_pipeline || m_device.isEmpty())
        return;
    const int target = sourceIndexByName(m_device);
    int idx = -1, curSource = -1, moved = 0;
    bool found = false;
    for (const QString &raw : pactl({QStringLiteral("list"), QStringLiteral("source-outputs")})) {
        const QString l = raw.trimmed();
        if (l.startsWith(QLatin1String("Source Output #"))) {
            idx = l.midRef(15).toInt();
            curSource = -1;
        } else if (idx >= 0 && l.startsWith(QLatin1String("Source:"))) {
            curSource = l.midRef(7).trimmed().toInt();
        } else if (idx >= 0 && l.startsWith(QLatin1String("application.name = "))
                   && l.contains(QLatin1String(kClientName))) {
            found = true;
            // Only move when it is not already on the target monitor: a routine
            // re-assert of an already-correct stream would needlessly re-route it
            // (and could glitch the audio). When the monitor's index is unknown,
            // move anyway to be safe.
            if (target < 0 || curSource != target) {
                QProcess::execute(QStringLiteral("pactl"),
                                  {QStringLiteral("move-source-output"), QString::number(idx), m_device});
                ++moved;
            }
        }
    }
    if (moved) {
        qCInfo(lcAudio) << "forced capture onto" << m_device;
        emit captureChanged(m_device);
    } else if (!found) {
        qCWarning(lcAudio) << "capture stream not found to move onto" << m_device;
    }
}

// Keep the capture correctly routed for the whole session. Runs on the watch
// timer and does two cheap things:
//   * if a *different* sink is actually playing (e.g. a native app on
//     primary_output rather than Android on deep_buffer), switch the pipeline
//     once to follow it; and
//   * otherwise re-assert the capture onto the current monitor.
// The re-assert is the important part: droid re-parks record streams on the
// microphone over time and across the sink's suspend/resume — notably when the
// user starts the music minutes after the session is already up — so without it
// the car gets silence (or the mic) even though the pipeline looks fine. The
// re-assert only moves the stream when it has actually drifted, so it never
// rebuilds the pipeline and never causes the choppiness a rebuild would.
void AudioSource::followActiveSink()
{
    if (!m_pipeline || !m_followDefault)
        return;
    const QString want = resolveActiveMonitor();
    if (!want.isEmpty() && want != m_device) {
        QString error;
        const QString was = m_device;
        if (buildPipeline(want, &error)) // reschedules the move onto the new monitor
            qCInfo(lcAudio) << "followed active sink" << was << "->" << m_device;
        else
            qCWarning(lcAudio) << "could not follow active sink to" << want << ":" << error;
        return;
    }
    // Same sink, or nothing new playing yet: hold the capture on the monitor.
    moveCaptureToMonitor();
}

void AudioSource::stop()
{
    m_watch.stop();
    m_followDefault = false;
    teardownPipeline();
}

void AudioSource::teardownPipeline()
{
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
    emit pcm(data, timestampUs);
}
