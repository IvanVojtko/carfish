#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>

typedef struct _GstElement GstElement;
typedef struct _GstAppSink GstAppSink;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GstBufferPool GstBufferPool;

// Encodes RGBA frames from CarScreen into H.264 Annex-B access units.
// The first access unit is split: SPS/PPS go out as the AAP "codec config"
// media message, everything else as timestamped media data.
class VideoSource : public QObject
{
    Q_OBJECT
public:
    explicit VideoSource(QObject *parent = nullptr);
    ~VideoSource() override;

    bool start(const QSize &size, int fps, int bitrateKbps, QString *error);
    void stop();
    bool isRunning() const { return m_pipeline != nullptr || m_openh264; }

    void requestKeyFrame();

public slots:
    void pushFrame(const QImage &rgba);

signals:
    void codecConfig(const QByteArray &spsPps);
    void frameEncoded(const QByteArray &accessUnit, quint64 timestampUs, bool keyFrame);
    void pipelineError(const QString &message);
    void pipelineInfo(const QString &message);

private slots:
    void handleSample(const QByteArray &au, quint64 ptsUs, bool keyFrame);

private:
    static int onNewSample(GstAppSink *sink, void *self);
    static int onBusMessage(GstBus *bus, GstMessage *msg, void *self);

    bool fillDroidBuffer(const QImage &rgba); // returns false if no HW buffer available
    bool acquireDroidPool(QString *error);

    // Bundled openh264 software encoder (Constrained Baseline). Preferred path:
    // self-contained, needs no GStreamer H.264 encoder on the device.
    bool startOpenH264(int bitrateKbps, QString *error);
    void encodeOpenH264(const QImage &rgba);
    void *m_svc = nullptr;    // ISVCEncoder*, opaque here
    bool m_openh264 = false;
    QByteArray m_i420;        // reusable I420 conversion scratch
    QByteArray m_au;          // reusable access-unit assembly scratch

    GstElement *m_pipeline = nullptr;
    GstElement *m_appsrc = nullptr;
    GstElement *m_encoder = nullptr;
    GstBufferPool *m_pool = nullptr; // droidvenc's gralloc pool, HW path only
    bool m_droid = false;            // feeding droidvenc HW encoder
    int m_droidColorFormat = 0;      // answer to droidvenc's color-format query
    unsigned int m_busWatchId = 0;
    QSize m_size;
    int m_fps = 30;
    quint64 m_frameIndex = 0;
    bool m_configSent = false;
};
