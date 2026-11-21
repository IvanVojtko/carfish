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
    // Live traffic overlay (TomTom). The key is user-supplied (phone settings);
    // the toggle is flipped from a button on the car map and persisted by the
    // Controller. Both are read by the map QML as carScreen.trafficKey / .trafficEnabled.
    Q_PROPERTY(QString trafficKey READ trafficKey WRITE setTrafficKey NOTIFY trafficKeyChanged)
    Q_PROPERTY(bool trafficEnabled READ trafficEnabled WRITE setTrafficEnabled NOTIFY trafficEnabledChanged)
    // Car fuel/energy level from the AAP FUEL sensor (−1 until the car reports it,
    // or when it doesn't expose the sensor). For an EV evElectric is true and
    // evLevel/evRange are the battery state-of-charge % and remaining range (km).
    // The map QML uses these to show a battery-at-arrival estimate.
    Q_PROPERTY(int evLevel READ evLevel NOTIFY energyChanged)
    Q_PROPERTY(int evRange READ evRange NOTIFY energyChanged)
    Q_PROPERTY(bool evElectric READ evElectric NOTIFY energyChanged)
    Q_PROPERTY(bool evLow READ evLow NOTIFY energyChanged)
public:
    explicit CarScreen(QQmlEngine *engine, QObject *parent = nullptr);
    ~CarScreen() override;

    // contentRect: the part of the frame the head unit actually shows.
    bool start(const QUrl &qml, const QSize &size, int fps, QString *error, const QRect &contentRect = QRect());
    void stop();

    bool nightMode() const { return m_nightMode; }
    void setNightMode(bool night);

    QString trafficKey() const { return m_trafficKey; }
    void setTrafficKey(const QString &key);
    bool trafficEnabled() const { return m_trafficEnabled; }
    void setTrafficEnabled(bool on);

    int evLevel() const { return m_evLevel; }
    int evRange() const { return m_evRange; }
    bool evElectric() const { return m_evElectric; }
    bool evLow() const { return m_evLow; }
    Q_INVOKABLE void setEnergy(int levelPct, int rangeKm, bool electric, bool low);

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
    void trafficKeyChanged();
    void trafficEnabledChanged();
    void energyChanged();
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
    QString m_trafficKey;
    bool m_trafficEnabled = true;
    int m_evLevel = -1;
    int m_evRange = -1;
    bool m_evElectric = false;
    bool m_evLow = false;
    QObject *m_media = nullptr;
    QObject *m_apps = nullptr;
    QObject *m_phone = nullptr;
    class NavBridge *m_navBridge = nullptr;
};
