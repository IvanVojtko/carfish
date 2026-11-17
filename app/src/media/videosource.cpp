#include "videosource.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMetaObject>

// droidvenc's private color-format query (gst-droid gstdroidquery.c).
#define DROID_COLOR_FORMAT_QUERY "GetDroidVideoColorFormatQuery"
// HAL_PIXEL_FORMAT_YV12; droidvenc's pool hands us YV12 gralloc buffers.
#define DEFAULT_DROID_COLOR_FORMAT 0x32315659

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>

Q_LOGGING_CATEGORY(lcVideo, "aap.video")

// Encoder candidates in order of preference. We feed system-memory frames from
// the offscreen QML renderer, so we need an encoder that accepts plain
// video/x-raw. droidvenc (the gstreamer1.0-droid HW encoder) only accepts
// Android HAL buffers (video/x-raw(memory:DroidVideoMetaData)) produced by
// droidcamsrc/droidvdec, so it cannot encode our CPU frames -- keep it last as
// a nonfunctional fallback. v4l2h264enc is a kernel M2M HW encoder (system
// memory) where present; the rest are software.
static const char *const kEncoders[][2] = {
    { "v4l2h264enc", "v4l2h264enc name=enc" },
    { "x264enc", "x264enc name=enc tune=zerolatency speed-preset=ultrafast byte-stream=true" },
    { "openh264enc", "openh264enc name=enc complexity=low rate-control=bitrate" },
    { "avenc_h264", "avenc_h264 name=enc" },
    { "droidvenc", "droidvenc name=enc" },
};

VideoSource::VideoSource(QObject *parent)
    : QObject(parent)
{
}

VideoSource::~VideoSource()
{
    stop();
}

// Override the color format we report to droidvenc, from a file, so candidates
// can be tried without rebuilding:  echo 0x32315659 > ~/carfish-colorfmt
static int droidColorFormat()
{
    QFile f(QDir::homePath() + QStringLiteral("/carfish-colorfmt"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const QString s = QString::fromLatin1(f.readLine()).trimmed();
        const int v = s.toInt(&ok, 0); // base 0 accepts 0x.. and decimal
        if (ok)
            return v;
    }
    return DEFAULT_DROID_COLOR_FORMAT;
}

// Answers droidvenc's upstream color-format query on the appsrc src pad, the
// role droidcamsrc plays in a normal pipeline.
static GstPadProbeReturn colorFormatQueryProbe(GstPad *, GstPadProbeInfo *info, gpointer user)
{
    GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
    if (!q || GST_QUERY_TYPE(q) != GST_QUERY_CUSTOM)
        return GST_PAD_PROBE_OK;
    GstStructure *s = gst_query_writable_structure(q);
    if (!s || g_strcmp0(gst_structure_get_name(s), DROID_COLOR_FORMAT_QUERY) != 0)
        return GST_PAD_PROBE_OK;
    gst_structure_set(s, "color-format", G_TYPE_INT, GPOINTER_TO_INT(user), nullptr);
    return GST_PAD_PROBE_HANDLED;
}

// Initialise the bundled openh264 encoder for Constrained Baseline, real-time.
bool VideoSource::startOpenH264(int bitrateKbps, QString *error)
{
    ISVCEncoder *svc = nullptr;
    if (WelsCreateSVCEncoder(&svc) != 0 || !svc) {
        *error = QStringLiteral("WelsCreateSVCEncoder failed");
        return false;
    }

    const int w = m_size.width(), h = m_size.height();
    const int bps = bitrateKbps * 1000;

    SEncParamExt p;
    memset(&p, 0, sizeof(p));
    svc->GetDefaultParams(&p); // sensible defaults, then override what matters
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = w;
    p.iPicHeight = h;
    p.iTargetBitrate = bps;
    p.iMaxBitrate = bps;
    p.iRCMode = RC_BITRATE_MODE;
    p.fMaxFrameRate = float(m_fps);
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.uiIntraPeriod = guint(m_fps * 2); // IDR every ~2s (plus on-demand)
    p.iComplexityMode = LOW_COMPLEXITY;
    p.iEntropyCodingModeFlag = 0;       // CAVLC: required for Baseline
    p.bEnableFrameSkip = false;         // keep a steady cadence for the HU
    p.bEnableDenoise = false;
    p.bPrefixNalAddingCtrl = false;
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.bEnableFrameCroppingFlag = true;
    p.iMultipleThreadIdc = 1;           // single thread + single slice: 1 NAL/frame
    p.bEnableSceneChangeDetect = true;

    SSpatialLayerConfig &l = p.sSpatialLayers[0];
    l.iVideoWidth = w;
    l.iVideoHeight = h;
    l.fFrameRate = float(m_fps);
    l.iSpatialBitrate = bps;
    l.iMaxSpatialBitrate = bps;
    l.uiProfileIdc = PRO_BASELINE;
    l.uiLevelIdc = LEVEL_3_1;
    l.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    if (svc->InitializeExt(&p) != 0) {
        WelsDestroySVCEncoder(svc);
        *error = QStringLiteral("openh264 InitializeExt rejected %1x%2").arg(w).arg(h);
        return false;
    }
    // Emit I420 input (the default, but be explicit).
    int fmt = videoFormatI420;
    svc->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);

    m_svc = svc;
    m_openh264 = true;
    m_i420.resize(w * h * 3 / 2);
    emit pipelineInfo(QStringLiteral("encoder openh264 (constrained-baseline) @ %1 kbps").arg(bitrateKbps));
    qCInfo(lcVideo) << "openh264" << m_size << m_fps << "fps" << bitrateKbps << "kbps";
    return true;
}

// Convert one RGBA frame to I420 (BT.601 limited range) and encode it; the
// resulting Annex-B access unit is routed through handleSample() exactly like
// the GStreamer path, so SPS/PPS splitting and signalling are shared.
void VideoSource::encodeOpenH264(const QImage &img)
{
    auto *svc = static_cast<ISVCEncoder *>(m_svc);
    const int w = img.width(), h = img.height();
    if (w != m_size.width() || h != m_size.height())
        return;

    quint8 *yP = reinterpret_cast<quint8 *>(m_i420.data());
    quint8 *uP = yP + w * h;
    quint8 *vP = uP + (w / 2) * (h / 2);
    const int cs = w / 2; // chroma stride

    for (int y = 0; y < h; ++y) {
        const quint8 *row = reinterpret_cast<const quint8 *>(img.constScanLine(y));
        quint8 *yd = yP + y * w;
        for (int x = 0; x < w; ++x) {
            const int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
            yd[x] = quint8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < h / 2; ++y) {
        quint8 *ud = uP + y * cs;
        quint8 *vd = vP + y * cs;
        const quint8 *r0 = reinterpret_cast<const quint8 *>(img.constScanLine(2 * y));
        const quint8 *r1 = reinterpret_cast<const quint8 *>(img.constScanLine(qMin(2 * y + 1, h - 1)));
        for (int x = 0; x < w / 2; ++x) {
            const int i0 = 2 * x * 4, i1 = qMin(2 * x + 1, w - 1) * 4;
            const int r = (r0[i0] + r0[i1] + r1[i0] + r1[i1]) >> 2;
            const int g = (r0[i0 + 1] + r0[i1 + 1] + r1[i0 + 1] + r1[i1 + 1]) >> 2;
            const int b = (r0[i0 + 2] + r0[i1 + 2] + r1[i0 + 2] + r1[i1 + 2]) >> 2;
            ud[x] = quint8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vd[x] = quint8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }

    SSourcePicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = w;
    pic.iPicHeight = h;
    pic.iStride[0] = w;
    pic.iStride[1] = cs;
    pic.iStride[2] = cs;
    pic.pData[0] = yP;
    pic.pData[1] = uP;
    pic.pData[2] = vP;
    pic.uiTimeStamp = qint64(m_frameIndex) * 1000 / m_fps; // ms

    SFrameBSInfo info;
    memset(&info, 0, sizeof(info));
    const int ret = svc->EncodeFrame(&pic, &info);
    if (ret != cmResultSuccess) {
        emit pipelineError(QStringLiteral("openh264 EncodeFrame failed (%1)").arg(ret));
        return;
    }
    if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid)
        return;

    // Concatenate every NAL of every layer: the buffers are already Annex-B.
    m_au.clear();
    for (int li = 0; li < info.iLayerNum; ++li) {
        const SLayerBSInfo &layer = info.sLayerInfo[li];
        int bytes = 0;
        for (int n = 0; n < layer.iNalCount; ++n)
            bytes += layer.pNalLengthInByte[n];
        m_au.append(reinterpret_cast<const char *>(layer.pBsBuf), bytes);
    }

    const bool key = info.eFrameType == videoFrameTypeIDR || info.eFrameType == videoFrameTypeI;
    const quint64 ptsUs = gst_util_uint64_scale(m_frameIndex, G_USEC_PER_SEC, guint64(m_fps));
    ++m_frameIndex;
    handleSample(m_au, ptsUs, key);
}

bool VideoSource::start(const QSize &size, int fps, int bitrateKbps, QString *error)
{
    stop();
    m_size = size;
    m_fps = fps;
    m_frameIndex = 0;
    m_configSent = false;

    // Preferred: the bundled openh264 software encoder. Self-contained (no
    // GStreamer H.264 element needed on the device) and always Constrained
    // Baseline, which is what head units require. Fall back to GStreamer
    // encoders only if it somehow fails to initialise.
    QString ohErr;
    if (startOpenH264(bitrateKbps, &ohErr))
        return true;
    emit pipelineInfo(QStringLiteral("openh264 unavailable (%1); trying GStreamer encoders").arg(ohErr));

    const char *encoder = nullptr;
    const char *encoderName = nullptr;
    for (const auto &e : kEncoders) {
        if (GstElementFactory *f = gst_element_factory_find(e[0])) {
            gst_object_unref(f);
            encoderName = e[0];
            encoder = e[1];
            break;
        }
    }
    if (!encoder) {
        *error = QStringLiteral("no H.264 encoder element available");
        return false;
    }
    m_droid = qstrcmp(encoderName, "droidvenc") == 0;

    // Baseline profile, no B-frames: head units are only required to decode
    // H.264 Constrained Baseline.
    //
    // Two pipelines. Software/V4L2 encoders take system-memory frames: let
    // videoconvert pick the raw format they accept. droidvenc (Android HW) only
    // accepts gralloc buffers it allocates itself (YV12,
    // memory:DroidVideoMetaData), so appsrc produces those caps and we fill
    // buffers from droidvenc's own pool (see acquireDroidPool / fillDroidBuffer);
    // no videoconvert, the RGBA->YV12 convert happens as we fill each buffer.
    QString desc;
    if (m_droid) {
        // droidvenc only emits stream-format=avc; force that on its src pad and
        // let h264parse convert avc -> byte-stream (Annex-B) for the head unit.
        desc = QStringLiteral(
            "appsrc name=src is-live=true format=time do-timestamp=false "
            "caps=\"video/x-raw(memory:DroidVideoMetaData),format=YV12,width=%1,height=%2,framerate=%3/1\" "
            "! %4 "
            "! video/x-h264,stream-format=avc,alignment=au "
            "! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=sink emit-signals=false sync=false max-buffers=4 drop=false")
                .arg(size.width()).arg(size.height()).arg(fps).arg(QString::fromLatin1(encoder));
    } else {
        // Force I420 4:2:0 so the encoder can't negotiate a 4:4:4/high profile;
        // head units require H.264 Constrained Baseline.
        desc = QStringLiteral(
            "appsrc name=src is-live=true format=time do-timestamp=false "
            "caps=video/x-raw,format=RGBA,width=%1,height=%2,framerate=%3/1 "
            "! queue ! videoconvert ! video/x-raw,format=I420 "
            "! %4 "
            "! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au,profile=constrained-baseline "
            "! appsink name=sink emit-signals=false sync=false max-buffers=4 drop=false")
                .arg(size.width()).arg(size.height()).arg(fps).arg(QString::fromLatin1(encoder));
    }

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline) {
        *error = QString::fromUtf8(err ? err->message : "gst_parse_launch failed");
        g_clear_error(&err);
        return false;
    }

    m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
    m_encoder = gst_bin_get_by_name(GST_BIN(m_pipeline), "enc");

    if (m_droid) {
        // droidvenc queries its upstream for the HAL color format; appsrc can't
        // answer, so we do (the role droidcamsrc plays). Value is overridable.
        m_droidColorFormat = droidColorFormat();
        GstPad *sp = gst_element_get_static_pad(m_appsrc, "src");
        gst_pad_add_probe(sp, GstPadProbeType(GST_PAD_PROBE_TYPE_QUERY_UPSTREAM
                                              | GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
                          colorFormatQueryProbe, GINT_TO_POINTER(m_droidColorFormat), nullptr);
        gst_object_unref(sp);
        emit pipelineInfo(QStringLiteral("droid color-format answer: 0x%1")
                              .arg(uint(m_droidColorFormat), 0, 16));
    }

    // Property names differ per encoder; set what exists.
    auto setIfExists = [this](const char *prop, int value) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_encoder), prop))
            g_object_set(m_encoder, prop, value, nullptr);
    };
    // Bitrate units differ: x264enc/v4l2h264enc take kbps, the others bps.
    const bool bitsPerSecond = qstrcmp(encoderName, "openh264enc") == 0
            || qstrcmp(encoderName, "avenc_h264") == 0
            || qstrcmp(encoderName, "droidvenc") == 0;
    setIfExists("bitrate", bitsPerSecond ? bitrateKbps * 1000 : bitrateKbps);
    setIfExists("target-bitrate", bitrateKbps * 1000); // droidvenc's property name
    setIfExists("key-int-max", fps * 2);
    setIfExists("gop-size", fps * 2);
    setIfExists("i-frame-interval", 2);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    GstAppSinkCallbacks cb = {};
    cb.new_sample = [](GstAppSink *s, gpointer self) -> GstFlowReturn {
        return GstFlowReturn(VideoSource::onNewSample(s, self));
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
    gst_object_unref(sink);

    // Watch the bus so an asynchronous failure (e.g. a HW encoder refusing the
    // raw format after PLAYING) is reported instead of silently stalling.
    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatchId = gst_bus_add_watch(bus, &VideoSource::onBusMessage, this);
    gst_object_unref(bus);

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("cannot start video pipeline");
        stop();
        return false;
    }
    qCInfo(lcVideo) << "encoding" << size << fps << "fps with" << encoderName << bitrateKbps << "kbps";
    emit pipelineInfo(QStringLiteral("encoder %1 @ %2 kbps").arg(QString::fromLatin1(encoderName)).arg(bitrateKbps));

    if (m_droid && !acquireDroidPool(error)) {
        stop();
        return false;
    }
    return true;
}

// Ask droidvenc for the gralloc buffer pool it proposes, and activate it, so
// pushFrame can hand it HW buffers filled with our frame. Heavy logging to the
// UI because this negotiation is the fragile part of the HW path.
bool VideoSource::acquireDroidPool(QString *error)
{
    GstPad *srcpad = gst_element_get_static_pad(m_appsrc, "src");
    GstCaps *caps = gst_caps_from_string(
        QStringLiteral("video/x-raw(memory:DroidVideoMetaData),format=YV12,width=%1,height=%2,framerate=%3/1")
            .arg(m_size.width()).arg(m_size.height()).arg(m_fps).toUtf8().constData());

    GstQuery *q = gst_query_new_allocation(caps, TRUE);
    const bool answered = gst_pad_peer_query(srcpad, q);
    guint nPools = answered ? gst_query_get_n_allocation_pools(q) : 0;
    emit pipelineInfo(QStringLiteral("droidvenc allocation query %1, %2 pool(s)")
                          .arg(answered ? "answered" : "unanswered").arg(nPools));

    guint size = 0, minBuf = 0, maxBuf = 0;
    if (nPools > 0)
        gst_query_parse_nth_allocation_pool(q, 0, &m_pool, &size, &minBuf, &maxBuf);

    bool ok = false;
    if (m_pool) {
        GstStructure *config = gst_buffer_pool_get_config(m_pool);
        gst_buffer_pool_config_set_params(config, caps, size, minBuf, maxBuf);
        if (!gst_buffer_pool_set_config(m_pool, config)) {
            emit pipelineInfo(QStringLiteral("droid pool rejected our config"));
        } else if (!gst_buffer_pool_set_active(m_pool, TRUE)) {
            emit pipelineInfo(QStringLiteral("droid pool would not activate"));
        } else {
            emit pipelineInfo(QStringLiteral("droid pool active (size %1, %2-%3 buffers)")
                                  .arg(size).arg(minBuf).arg(maxBuf));
            ok = true;
        }
    }

    gst_query_unref(q);
    gst_caps_unref(caps);
    gst_object_unref(srcpad);

    if (!ok) {
        if (m_pool) {
            gst_object_unref(m_pool);
            m_pool = nullptr;
        }
        *error = QStringLiteral("droidvenc did not provide a usable buffer pool "
                                "(the HW encoder will not accept app-fed frames)");
        return false;
    }
    return true;
}

void VideoSource::stop()
{
    if (m_svc) {
        auto *svc = static_cast<ISVCEncoder *>(m_svc);
        svc->Uninitialize();
        WelsDestroySVCEncoder(svc);
        m_svc = nullptr;
    }
    m_openh264 = false;

    if (!m_pipeline)
        return;
    if (m_busWatchId) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_pool) {
        gst_buffer_pool_set_active(m_pool, FALSE);
        gst_object_unref(m_pool);
        m_pool = nullptr;
    }
    m_droid = false;
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_appsrc);
    gst_object_unref(m_encoder);
    gst_object_unref(m_pipeline);
    m_pipeline = m_appsrc = m_encoder = nullptr;
}

void VideoSource::requestKeyFrame()
{
    if (m_openh264) {
        if (m_svc)
            static_cast<ISVCEncoder *>(m_svc)->ForceIntraFrame(true);
        return;
    }
    if (!m_encoder)
        return;
    GstEvent *ev = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
    GstPad *pad = gst_element_get_static_pad(m_encoder, "src");
    gst_pad_send_event(pad, ev);
    gst_object_unref(pad);
}

void VideoSource::pushFrame(const QImage &rgba)
{
    if (!m_openh264 && !m_appsrc)
        return;
    const QImage img = rgba.format() == QImage::Format_RGBA8888 ? rgba
                                                                : rgba.convertToFormat(QImage::Format_RGBA8888);
    if (img.size() != m_size)
        return;

    if (m_openh264) {
        encodeOpenH264(img);
        return;
    }

    if (m_droid) {
        fillDroidBuffer(img);
        return;
    }

    const gsize bytes = gsize(img.width()) * 4 * gsize(img.height());
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    for (int y = 0; y < img.height(); ++y)
        memcpy(map.data + gsize(y) * img.width() * 4, img.constScanLine(y), size_t(img.width()) * 4);
    gst_buffer_unmap(buf, &map);

    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(m_frameIndex, GST_SECOND, guint64(m_fps));
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, guint64(m_fps));
    ++m_frameIndex;
    gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buf);
}

// Acquire a gralloc buffer from droidvenc's pool, convert the RGBA frame into it
// as YV12 (BT.601 limited range) honouring the hardware plane strides, and push.
bool VideoSource::fillDroidBuffer(const QImage &img)
{
    if (!m_pool)
        return false;
    GstBuffer *buf = nullptr;
    if (gst_buffer_pool_acquire_buffer(m_pool, &buf, nullptr) != GST_FLOW_OK || !buf) {
        emit pipelineError(QStringLiteral("droid pool: could not acquire a buffer"));
        return false;
    }

    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_YV12, img.width(), img.height());
    GstVideoFrame frame;
    // With a GstVideoMeta on the pool buffer, this maps using the HW strides.
    if (!gst_video_frame_map(&frame, &info, buf, GstMapFlags(GST_MAP_WRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF))) {
        emit pipelineError(QStringLiteral("droid buffer: video_frame_map failed"));
        gst_buffer_unref(buf);
        return false;
    }

    const int w = img.width(), h = img.height();
    quint8 *yP = static_cast<quint8 *>(GST_VIDEO_FRAME_COMP_DATA(&frame, 0));
    quint8 *uP = static_cast<quint8 *>(GST_VIDEO_FRAME_COMP_DATA(&frame, 1));
    quint8 *vP = static_cast<quint8 *>(GST_VIDEO_FRAME_COMP_DATA(&frame, 2));
    const int yS = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 0);
    const int uS = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 1);
    const int vS = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 2);

    for (int y = 0; y < h; ++y) {
        const quint8 *row = img.constScanLine(y);
        quint8 *yd = yP + y * yS;
        for (int x = 0; x < w; ++x) {
            const int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
            yd[x] = quint8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    // 4:2:0 chroma: average each 2x2 block.
    for (int y = 0; y < h / 2; ++y) {
        quint8 *ud = uP + y * uS;
        quint8 *vd = vP + y * vS;
        const quint8 *r0 = img.constScanLine(2 * y);
        const quint8 *r1 = img.constScanLine(qMin(2 * y + 1, h - 1));
        for (int x = 0; x < w / 2; ++x) {
            const int i0 = 2 * x * 4, i1 = qMin(2 * x + 1, w - 1) * 4;
            const int r = (r0[i0] + r0[i1] + r1[i0] + r1[i1]) >> 2;
            const int g = (r0[i0 + 1] + r0[i1 + 1] + r1[i0 + 1] + r1[i1 + 1]) >> 2;
            const int b = (r0[i0 + 2] + r0[i1 + 2] + r1[i0 + 2] + r1[i1 + 2]) >> 2;
            ud[x] = quint8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vd[x] = quint8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
    gst_video_frame_unmap(&frame);

    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(m_frameIndex, GST_SECOND, guint64(m_fps));
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, guint64(m_fps));
    ++m_frameIndex;
    gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buf);
    return true;
}

int VideoSource::onBusMessage(GstBus *, GstMessage *msg, void *self)
{
    auto *vs = static_cast<VideoSource *>(self);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        const QString where = QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)));
        const QString what = QString::fromUtf8(err ? err->message : "unknown");
        const QString detail = QString::fromUtf8(dbg ? dbg : "");
        qCWarning(lcVideo) << "pipeline error from" << where << ":" << what << detail;
        emit vs->pipelineError(where + QLatin1String(": ") + what
                               + (detail.isEmpty() ? QString() : QLatin1String(" | ") + detail));
        g_clear_error(&err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        qCWarning(lcVideo) << "pipeline warning:" << (err ? err->message : "") << (dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        break;
    }
    default:
        break;
    }
    return TRUE; // keep the watch
}

int VideoSource::onNewSample(GstAppSink *sink, void *self)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_EOS;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
        const QByteArray au(reinterpret_cast<const char *>(map.data), int(map.size));
        const quint64 ptsUs = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) / 1000 : 0;
        const bool key = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
        gst_buffer_unmap(buf, &map);
        // Streaming thread -> main thread.
        QMetaObject::invokeMethod(static_cast<VideoSource *>(self), "handleSample", Qt::QueuedConnection,
                                  Q_ARG(QByteArray, au), Q_ARG(quint64, ptsUs), Q_ARG(bool, key));
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Splits an Annex-B access unit into NAL units: returns [start, end) pairs
// including the start code.
static QVector<QPair<int, int>> splitNals(const QByteArray &au)
{
    QVector<QPair<int, int>> nals;
    const auto *d = reinterpret_cast<const uchar *>(au.constData());
    const int n = au.size();
    int start = -1;
    for (int i = 0; i + 3 <= n; ++i) {
        const bool sc3 = d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
        if (!sc3)
            continue;
        const int scStart = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
        if (start >= 0)
            nals.append(qMakePair(start, scStart));
        start = scStart;
        i += 2;
    }
    if (start >= 0)
        nals.append(qMakePair(start, n));
    return nals;
}

static int nalType(const QByteArray &au, const QPair<int, int> &nal)
{
    int i = nal.first;
    while (i < nal.second && au.at(i) == 0)
        ++i;
    return (i + 1 < nal.second) ? (uchar(au.at(i + 1)) & 0x1f) : -1; // skip the 0x01
}

void VideoSource::handleSample(const QByteArray &au, quint64 ptsUs, bool keyFrame)
{
    if (!m_configSent) {
        if (!keyFrame)
            return; // wait for the first IDR
        QByteArray config, rest;
        for (const auto &nal : splitNals(au)) {
            const int t = nalType(au, nal);
            (t == 7 || t == 8 ? config : rest).append(au.mid(nal.first, nal.second - nal.first));
        }
        if (config.isEmpty()) {
            qCWarning(lcVideo) << "first key frame carries no SPS/PPS";
            return;
        }
        m_configSent = true;
        emit codecConfig(config);
        emit frameEncoded(rest, ptsUs, true);
        return;
    }
    emit frameEncoded(au, ptsUs, keyFrame);
}
