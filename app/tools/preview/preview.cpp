// Desktop preview of the projected car UI (carqml/CarMain.qml), so you can see
// and interact with it on a computer without a head unit. It provides the one
// thing CarMain expects from the app -- a `carScreen` object with a `nightMode`
// property -- then loads the real QML in a resizable, rescaling frame with
// on-screen controls for day/night and the common head-unit resolutions.
//
// Build: part of the DESKTOP_BUILD (-DDESKTOP_BUILD=ON); run `carfish-preview`.
#include "apps/phonemanager.h"
#include "util/uanetwork.h"

#include <QGuiApplication>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QUrl>

class PreviewScreen : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool nightMode READ nightMode WRITE setNightMode NOTIFY nightModeChanged)
    Q_PROPERTY(QString trafficKey READ trafficKey WRITE setTrafficKey NOTIFY trafficKeyChanged)
    Q_PROPERTY(bool trafficEnabled READ trafficEnabled WRITE setTrafficEnabled NOTIFY trafficEnabledChanged)
    Q_PROPERTY(int evLevel READ evLevel NOTIFY energyChanged)
    Q_PROPERTY(int evRange READ evRange NOTIFY energyChanged)
    Q_PROPERTY(bool evElectric READ evElectric NOTIFY energyChanged)
    Q_PROPERTY(bool evLow READ evLow NOTIFY energyChanged)
public:
    bool nightMode() const { return m_night; }
    void setNightMode(bool n)
    {
        if (n == m_night)
            return;
        m_night = n;
        emit nightModeChanged();
    }
    QString trafficKey() const { return m_trafficKey; }
    void setTrafficKey(const QString &k)
    {
        if (k == m_trafficKey)
            return;
        m_trafficKey = k;
        emit trafficKeyChanged();
    }
    bool trafficEnabled() const { return m_trafficEnabled; }
    void setTrafficEnabled(bool on)
    {
        if (on == m_trafficEnabled)
            return;
        m_trafficEnabled = on;
        emit trafficEnabledChanged();
    }
    Q_INVOKABLE void log(const QString &) {}
    int evLevel() const { return m_evLevel; }
    int evRange() const { return m_evRange; }
    bool evElectric() const { return m_evElectric; }
    bool evLow() const { return m_evLow; }
    Q_INVOKABLE void setEnergy(int levelPct, int rangeKm, bool electric, bool low)
    {
        m_evLevel = levelPct; m_evRange = rangeKm; m_evElectric = electric; m_evLow = low;
        emit energyChanged();
    }
signals:
    void nightModeChanged();
    void trafficKeyChanged();
    void trafficEnabledChanged();
    void energyChanged();

private:
    bool m_night = false;
    QString m_trafficKey;
    bool m_trafficEnabled = true;
    int m_evLevel = -1;
    int m_evRange = -1;
    bool m_evElectric = false;
    bool m_evLow = false;
};

// Mock MPRIS backend so the now-playing pane and the play-queue overlay render
// with sample data in the preview (the real one is MprisController on device).
class PreviewMedia : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasPlayer READ hasPlayer CONSTANT)
    Q_PROPERTY(bool playing READ playing CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString artist READ artist CONSTANT)
    Q_PROPERTY(QString album READ album CONSTANT)
    Q_PROPERTY(QString artUrl READ artUrl CONSTANT)
    Q_PROPERTY(double progress READ progress CONSTANT)
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
public:
    PreviewMedia()
    {
        auto t = [](const QString &id, const QString &ti, const QString &ar, bool cur) {
            QVariantMap m;
            m[QStringLiteral("id")] = id; m[QStringLiteral("title")] = ti;
            m[QStringLiteral("artist")] = ar; m[QStringLiteral("current")] = cur;
            return QVariant(m);
        };
        m_queue << t(QStringLiteral("/q0"), QStringLiteral("Golden Hour"), QStringLiteral("Sailfish FM"), true)
                << t(QStringLiteral("/q1"), QStringLiteral("Night Drive"), QStringLiteral("The Commuters"), false)
                << t(QStringLiteral("/q2"), QStringLiteral("Coastline"), QStringLiteral("Aurora Lights"), false)
                << t(QStringLiteral("/q3"), QStringLiteral("Afterglow"), QStringLiteral("Mica"), false)
                << t(QStringLiteral("/q4"), QStringLiteral("Low Beams"), QStringLiteral("Radio Static"), false);
    }
    bool hasPlayer() const { return true; }
    bool playing() const { return true; }
    QString title() const { return QStringLiteral("Golden Hour"); }
    QString artist() const { return QStringLiteral("Sailfish FM"); }
    QString album() const { return QStringLiteral("Drive playlist"); }
    QString artUrl() const { return QString(); }
    double progress() const { return 0.4; }
    QVariantList queue() const { return m_queue; }
    Q_INVOKABLE void playPause() {}
    Q_INVOKABLE void next() {}
    Q_INVOKABLE void previous() {}
    Q_INVOKABLE void refreshQueue() {}
    Q_INVOKABLE void goToTrack(const QString &) {}
signals:
    void queueChanged();
private:
    QVariantList m_queue;
};

#include "preview.moc"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationDisplayName(QStringLiteral("Carfish — car UI preview"));

    PreviewScreen screen;
    PreviewMedia mediaMock; // mock now-playing + queue
    PhoneManager phone;   // mock contacts/recents on the desktop build

    QQuickView view;
    view.setTitle(QStringLiteral("Carfish — car UI preview"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    // Stamp a User-Agent on QML network requests so OSM serves real tiles in the
    // preview too (same as the app's Controller does on the device).
    view.engine()->setNetworkAccessManagerFactory(new UaNetworkAccessManagerFactory);
    // PREVIEW_TRAFFIC_KEY makes the traffic toggle button appear (a real key also
    // fetches live TomTom tiles).
    screen.setTrafficKey(QString::fromLocal8Bit(qgetenv("PREVIEW_TRAFFIC_KEY")));
    view.rootContext()->setContextProperty(QStringLiteral("carScreen"), &screen);
    view.rootContext()->setContextProperty(QStringLiteral("media"), &mediaMock);
    view.rootContext()->setContextProperty(QStringLiteral("phone"), &phone);
    // PREVIEW_QUEUE=1 opens the play-queue (track list) overlay for a still.
    view.rootContext()->setContextProperty(QStringLiteral("previewQueue"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_QUEUE")));
    // PREVIEW_HEADINGUP=1 forces heading-up map rotation (no GPS off-car).
    view.rootContext()->setContextProperty(QStringLiteral("previewHeadingUp"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_HEADINGUP")));
    // PREVIEW_PANE=phone|media|apps|maps opens that pane for a still/grab.
    view.rootContext()->setContextProperty(QStringLiteral("previewPane"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_PANE")));
    // PREVIEW_EXPANDED=1 opens the full-screen map state (music as a status bar).
    view.rootContext()->setContextProperty(QStringLiteral("previewExpanded"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_EXPANDED")));
    // PREVIEW_SEARCH=1 opens the destination search overlay (shows the keyboard);
    // PREVIEW_SEARCH_TEXT overrides the query it runs.
    view.rootContext()->setContextProperty(QStringLiteral("previewSearch"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_SEARCH")));
    view.rootContext()->setContextProperty(QStringLiteral("previewSearchText"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_SEARCH_TEXT")));
    // PREVIEW_NAV=1 runs the no-input nav self-test (shows the turn + arrival bars).
    view.rootContext()->setContextProperty(QStringLiteral("previewNav"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_NAV")));
    view.rootContext()->setContextProperty(QStringLiteral("carMainUrl"),
                                           QUrl::fromLocalFile(QStringLiteral(CARMAIN_QML)));
    // PREVIEW_SIZE=WxH renders CarMain ALONE at an exact head-unit resolution
    // (no desktop frame/controls), for a faithful still at e.g. 800x480.
    const QByteArray rawSize = qgetenv("PREVIEW_SIZE");
    int rawW = 0, rawH = 0;
    if (!rawSize.isEmpty()) {
        const QList<QByteArray> wh = rawSize.split('x');
        if (wh.size() == 2) { rawW = wh[0].trimmed().toInt(); rawH = wh[1].trimmed().toInt(); }
    }
    if (rawW > 0 && rawH > 0)
        view.setSource(QUrl::fromLocalFile(QStringLiteral(CARMAIN_QML)));
    else
        view.setSource(QUrl::fromLocalFile(QStringLiteral(PREVIEW_QML)));
    if (view.status() == QQuickView::Error)
        return 1;
    if (!qgetenv("PREVIEW_NIGHT").isEmpty())
        screen.setNightMode(true);

    if (rawW > 0 && rawH > 0)
        view.resize(rawW, rawH);
    else
        view.resize(980, 620);
    view.show();

    // Optional: render once to a PNG and quit (PREVIEW_GRAB=path), for stills/CI.
    const QByteArray grab = qgetenv("PREVIEW_GRAB");
    if (!grab.isEmpty()) {
        bool ok = false;
        const int ms = qEnvironmentVariableIntValue("PREVIEW_GRAB_MS", &ok);
        QTimer::singleShot(ok ? ms : 400, &app, [&]() {
            view.grabWindow().save(QString::fromLocal8Bit(grab));
            app.quit();
        });
    }
    return app.exec();
}
