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
    // Map orientation (false = north-up, true = heading-up), toggled by the compass
    // button on the car map and persisted by the Controller.
    Q_PROPERTY(bool mapHeadingUp READ mapHeadingUp WRITE setMapHeadingUp NOTIFY mapHeadingUpChanged)
    // Saved places for the destination search, persisted by the Controller:
    // places = {home: {label, lat, lon}, work: {...}} (a key is absent when
    // unset); recentPlaces = newest-first [{label, lat, lon}], at most 8.
    Q_PROPERTY(QVariantMap places READ places NOTIFY placesChanged)
    Q_PROPERTY(QVariantList recentPlaces READ recentPlaces NOTIFY placesChanged)
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
    bool mapHeadingUp() const { return m_mapHeadingUp; }
    void setMapHeadingUp(bool on);

    QVariantMap places() const { return m_places; }
    QVariantList recentPlaces() const { return m_recentPlaces; }
    void loadPlaces(const QVariantMap &places, const QVariantList &recents); // from settings, no signal
    Q_INVOKABLE void setPlace(const QString &kind, const QString &label, double lat, double lon);
    Q_INVOKABLE void clearPlace(const QString &kind);
    Q_INVOKABLE void addRecentPlace(const QString &label, double lat, double lon);
    Q_INVOKABLE void removeRecentPlace(int index);

    int evLevel() const { return m_evLevel; }
    int evRange() const { return m_evRange; }
    bool evElectric() const { return m_evElectric; }
    bool evLow() const { return m_evLow; }
    Q_INVOKABLE void setEnergy(int levelPct, int rangeKm, bool electric, bool low);
    // Usable battery capacity (kWh) the user entered on the phone; AAP never
    // reports it. 0 = unknown. Used for the cluster battery forecast (Wh).
    double evCapacityKwh() const { return m_evCapacityKwh; }
    void setEvCapacityKwh(double kwh) { m_evCapacityKwh = kwh; }

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
    void mapHeadingUpChanged();
    void placesChanged();
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
    bool m_mapHeadingUp = false;
    double m_evCapacityKwh = 0;
    QVariantMap m_places;
    QVariantList m_recentPlaces;
    int m_evLevel = -1;
    int m_evRange = -1;
    bool m_evElectric = false;
    bool m_evLow = false;
    QObject *m_media = nullptr;
    QObject *m_apps = nullptr;
    QObject *m_phone = nullptr;
    class NavBridge *m_navBridge = nullptr;
};
