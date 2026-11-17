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

    // device: PulseAudio source name, e.g. "sink.primary.monitor" or a
    // dedicated null sink's monitor so the phone speaker stays silent. The
    // token "@DEFAULT_MONITOR@" (or an empty string) means "capture whatever is
    // playing": resolved now AND followed while running, since the user often
    // starts the music seconds after the car session comes up.
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
    void moveCaptureToMonitor();   // force the stream off the mic onto m_device
    void teardownPipeline();

    GstElement *m_pipeline = nullptr;
    QString m_device;        // source currently being captured
    bool m_followDefault = false; // track the actively-playing sink
    int m_sampleRate = 0;
    int m_channels = 0;
    QTimer m_watch;          // polls for a change of the active sink
};
