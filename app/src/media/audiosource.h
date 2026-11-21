#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

typedef struct _GstElement GstElement;
typedef struct _GstAppSink GstAppSink;

// Captures PCM from a PulseAudio monitor source and hands out fixed size
// chunks for one AAP audio channel (media: 48 kHz stereo, guidance/system:
// 16 kHz mono, all S16LE).
class AudioSource : public QObject
{
    Q_OBJECT
public:
    explicit AudioSource(QObject *parent = nullptr);
    ~AudioSource() override;

    // device: PulseAudio source name, e.g. "sink.primary.monitor". The token
    // "@DEFAULT_MONITOR@" (or an empty string) selects "loopback" mode
    // (reverse-engineered from mrcyjanek/cargadgetd): a dedicated capture sink
    // is created, every media ("player") stream is redirected into it, and we
    // capture its monitor. This is deterministic (the capture never drifts onto
    // the mic the way chasing the active sink did), keeps the phone speaker
    // silent, and — crucially — catches music the user starts AFTER the car
    // session is already up, because new streams are re-adopted on every tick.
    bool start(const QString &device, int sampleRate, int channels, QString *error);
    void stop();
    bool isRunning() const { return m_pipeline != nullptr; }
    // The source actually captured (after resolving "@DEFAULT_MONITOR@").
    QString resolvedDevice() const { return m_device; }

signals:
    void pcm(const QByteArray &data, quint64 timestampUs);
    // Emitted when the followed capture moves to a different sink monitor.
    void captureChanged(const QString &device);

private slots:
    void handleChunk(const QByteArray &data, quint64 timestampUs);
    // While following the default monitor, re-check which sink is actually
    // playing and switch the capture to it (e.g. Spotify starts on deep_buffer
    // after the session is already up).
    void followActiveSink();

private:
    static int onNewSample(GstAppSink *sink, void *self);
    bool buildPipeline(const QString &src, QString *error);
    void moveCaptureToMonitor();   // force our capture off the mic onto m_device
    void teardownPipeline();

    // Loopback mode (reverse-engineered from cargadgetd):
    bool ensureCaptureSink(QString *monitor, QString *error); // create/find the capture sink
    void adoptPlayers();   // redirect media streams into the capture sink
    void releasePlayers(); // move them back to real hardware + unload the sink

    GstElement *m_pipeline = nullptr;
    QString m_device;        // source currently being captured
    bool m_loopback = false; // manage a dedicated capture sink + stream routing
    QString m_captureSink;   // our dedicated capture sink name (loopback mode)
    QString m_captureModule; // module-null-sink id to unload on stop (if we made it)
    int m_sampleRate = 0;
    int m_channels = 0;
    QTimer m_watch;          // re-adopts new media streams + re-pins the capture
};
