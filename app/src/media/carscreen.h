#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;
class QQmlComponent;
class QQmlEngine;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;

// Renders a dedicated QML scene (the "car UI") offscreen at the head unit's
// video resolution. Sailfish's compositor does not allow third party screen
// capture, so the projected UI is a separate scene rather than a mirror.
class CarScreen : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool nightMode READ nightMode NOTIFY nightModeChanged)
public:
    explicit CarScreen(QQmlEngine *engine, QObject *parent = nullptr);
    ~CarScreen() override;

    // contentRect: the part of the frame the head unit actually shows.
    bool start(const QUrl &qml, const QSize &size, int fps, QString *error, const QRect &contentRect = QRect());
    void stop();

    bool nightMode() const { return m_nightMode; }
    void setNightMode(bool night);

    // Touch input from the head unit, already in video pixel coordinates.
    enum TouchAction { Press, Release, Move };
    void touch(TouchAction action, const QPointF &pos);

    // Set a property on the car QML root (e.g. "playing"); no-op if not running.
    void setCarProperty(const QString &name, const QVariant &value);

    // Let the car QML write a line to the device's durable log (carScreen.log),
    // so diagnostics from the map/UI survive next to the session log.
    Q_INVOKABLE void log(const QString &line) { emit logMessage(line); }

    // Object exposed to the car QML as the "media" context property (MPRIS).
    void setMediaSource(QObject *media) { m_media = media; }
    void setAppRegistry(QObject *apps) { m_apps = apps; }
    void setPhone(QObject *phone) { m_phone = phone; }
    QObject *navBridge() const; // NavBridge*; defined in the .cpp (complete type)

signals:
    void frameReady(const QImage &rgba);
    void nightModeChanged();
    void logMessage(const QString &line);

private:
    void render();

    QQmlEngine *m_engine;
    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QQuickRenderControl *m_renderControl = nullptr;
    QQuickWindow *m_window = nullptr;
    QOpenGLFramebufferObject *m_fbo = nullptr;
    QQmlComponent *m_component = nullptr;
    QQuickItem *m_root = nullptr;
    QTimer m_frameTimer;
    bool m_nightMode = false;
    QObject *m_media = nullptr;
    QObject *m_apps = nullptr;
    QObject *m_phone = nullptr;
    class NavBridge *m_navBridge = nullptr;
};
