#include "carscreen.h"

#include "navbridge.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QtMath>

Q_LOGGING_CATEGORY(lcScreen, "aap.screen")

CarScreen::CarScreen(QQmlEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_navBridge(new NavBridge(this))
{
    connect(&m_frameTimer, &QTimer::timeout, this, &CarScreen::render);
}

CarScreen::~CarScreen()
{
    stop();
}

bool CarScreen::start(const QUrl &qml, const QSize &size, int fps, QString *error, const QRect &contentRect)
{
    stop();

    QSurfaceFormat format;
    if (QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES)
        format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setDepthBufferSize(16);
    format.setStencilBufferSize(8);

    m_context = new QOpenGLContext;
    m_context->setFormat(format);
    if (!m_context->create()) {
        *error = QStringLiteral("cannot create GL context");
        stop();
        return false;
    }
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(m_context->format());
    m_surface->create();

    m_renderControl = new QQuickRenderControl;
    m_window = new QQuickWindow(m_renderControl);
    m_window->setGeometry(0, 0, size.width(), size.height());
    m_window->setColor(Qt::black);

    if (!m_context->makeCurrent(m_surface)) {
        *error = QStringLiteral("cannot make GL context current");
        stop();
        return false;
    }
    m_fbo = new QOpenGLFramebufferObject(size, QOpenGLFramebufferObject::CombinedDepthStencil);
    m_window->setRenderTarget(m_fbo);
    m_renderControl->initialize(m_context);

    m_engine->rootContext()->setContextProperty(QStringLiteral("carScreen"), this);
    m_engine->rootContext()->setContextProperty(QStringLiteral("media"), m_media);
    m_engine->rootContext()->setContextProperty(QStringLiteral("appList"), m_apps);
    m_engine->rootContext()->setContextProperty(QStringLiteral("navBridge"), m_navBridge);
    m_engine->rootContext()->setContextProperty(QStringLiteral("phone"), m_phone);
    m_engine->rootContext()->setContextProperty(QStringLiteral("leds"), m_leds);
    m_component = new QQmlComponent(m_engine, qml, QQmlComponent::PreferSynchronous);
    if (m_component->isError()) {
        *error = m_component->errorString();
        stop();
        return false;
    }
    m_root = qobject_cast<QQuickItem *>(m_component->create());
    if (!m_root) {
        *error = QStringLiteral("car QML root must be an Item");
        stop();
        return false;
    }
    m_root->setParentItem(m_window->contentItem());
    const QRect visible = contentRect.isValid() ? contentRect : QRect(QPoint(), size);
    m_root->setPosition(visible.topLeft());
    m_root->setSize(visible.size());

    m_frameTimer.start(1000 / qMax(1, fps));
    qCInfo(lcScreen) << "rendering" << qml << "at" << size << fps << "fps";
    return true;
}

void CarScreen::stop()
{
    m_frameTimer.stop();
    if (m_context && m_surface)
        m_context->makeCurrent(m_surface);
    delete m_root;
    m_root = nullptr;
    delete m_component;
    m_component = nullptr;
    delete m_renderControl; // must go before the window
    m_renderControl = nullptr;
    delete m_window;
    m_window = nullptr;
    delete m_fbo;
    m_fbo = nullptr;
    if (m_context)
        m_context->doneCurrent();
    delete m_surface;
    m_surface = nullptr;
    delete m_context;
    m_context = nullptr;
}

QObject *CarScreen::navBridge() const
{
    return m_navBridge;
}

bool CarScreen::nightMode() const
{
    if (m_themeMode == QLatin1String("day"))
        return false;
    if (m_themeMode == QLatin1String("night"))
        return true;
    return m_nightMode;
}

void CarScreen::setNightMode(bool night)
{
    if (m_nightMode == night)
        return;
    const bool before = nightMode();
    m_nightMode = night;
    if (nightMode() != before)
        emit nightModeChanged();
}

void CarScreen::setThemeMode(const QString &mode)
{
    const QString m = (mode == QLatin1String("day") || mode == QLatin1String("night"))
        ? mode : QStringLiteral("auto");
    if (m_themeMode == m)
        return;
    const bool before = nightMode();
    m_themeMode = m;
    emit themeModeChanged();
    if (nightMode() != before)
        emit nightModeChanged();
}

void CarScreen::setRightHandDrive(bool on)
{
    if (m_rightHandDrive == on)
        return;
    m_rightHandDrive = on;
    emit rightHandDriveChanged();
}

void CarScreen::setTrafficKey(const QString &key)
{
    if (m_trafficKey == key)
        return;
    m_trafficKey = key;
    emit trafficKeyChanged();
}

void CarScreen::setTrafficEnabled(bool on)
{
    if (m_trafficEnabled == on)
        return;
    m_trafficEnabled = on;
    emit trafficEnabledChanged();
}

static QVariantMap placeEntry(const QString &label, double lat, double lon)
{
    QVariantMap m;
    m[QStringLiteral("label")] = label;
    m[QStringLiteral("lat")] = lat;
    m[QStringLiteral("lon")] = lon;
    return m;
}

void CarScreen::loadPlaces(const QVariantMap &places, const QVariantList &recents)
{
    m_places = places;
    m_recentPlaces = recents;
}

void CarScreen::setPlace(const QString &kind, const QString &label, double lat, double lon)
{
    if (kind.isEmpty())
        return;
    m_places[kind] = placeEntry(label, lat, lon);
    emit placesChanged();
}

void CarScreen::clearPlace(const QString &kind)
{
    if (m_places.remove(kind))
        emit placesChanged();
}

// Newest first, no duplicates (same label, or within ~50 m), at most 8.
void CarScreen::addRecentPlace(const QString &label, double lat, double lon)
{
    QVariantList out;
    out << placeEntry(label, lat, lon);
    for (const QVariant &v : m_recentPlaces) {
        const QVariantMap m = v.toMap();
        const double dLat = (m.value(QStringLiteral("lat")).toDouble() - lat) * 111320.0;
        const double dLon = (m.value(QStringLiteral("lon")).toDouble() - lon) * 111320.0
                            * qCos(qDegreesToRadians(lat));
        const bool same = m.value(QStringLiteral("label")).toString() == label
                          || (dLat * dLat + dLon * dLon) < 50.0 * 50.0;
        if (!same && out.size() < 8)
            out << m;
    }
    m_recentPlaces = out;
    emit placesChanged();
}

void CarScreen::removeRecentPlace(int index)
{
    if (index < 0 || index >= m_recentPlaces.size())
        return;
    m_recentPlaces.removeAt(index);
    emit placesChanged();
}

void CarScreen::setMapHeadingUp(bool on)
{
    if (m_mapHeadingUp == on)
        return;
    m_mapHeadingUp = on;
    emit mapHeadingUpChanged();
}

void CarScreen::setEnergy(int levelPct, int rangeKm, bool electric, bool low)
{
    if (m_evLevel == levelPct && m_evRange == rangeKm
            && m_evElectric == electric && m_evLow == low)
        return;
    m_evLevel = levelPct;
    m_evRange = rangeKm;
    m_evElectric = electric;
    m_evLow = low;
    emit energyChanged();
}

void CarScreen::setCarProperty(const QString &name, const QVariant &value)
{
    if (m_root)
        m_root->setProperty(name.toUtf8().constData(), value);
}

void CarScreen::render()
{
    if (!m_context->makeCurrent(m_surface))
        return;
    m_renderControl->polishItems();
    m_renderControl->sync();
    m_renderControl->render();
    m_context->functions()->glFlush();
    // toImage() returns ARGB32 premultiplied on GLES; the encoder wants RGBA.
    emit frameReady(m_fbo->toImage().convertToFormat(QImage::Format_RGBA8888));
}

void CarScreen::touch(TouchAction action, const QPointF &pos)
{
    if (!m_window)
        return;
    QEvent::Type type = QEvent::MouseMove;
    Qt::MouseButtons buttons = Qt::LeftButton;
    if (action == Press)
        type = QEvent::MouseButtonPress;
    else if (action == Release) {
        type = QEvent::MouseButtonRelease;
        buttons = Qt::NoButton;
    }
    QMouseEvent ev(type, pos, pos, pos, Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &ev);
}
