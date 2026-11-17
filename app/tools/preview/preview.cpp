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
public:
    bool nightMode() const { return m_night; }
    void setNightMode(bool n)
    {
        if (n == m_night)
            return;
        m_night = n;
        emit nightModeChanged();
    }
signals:
    void nightModeChanged();

private:
    bool m_night = false;
};

#include "preview.moc"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationDisplayName(QStringLiteral("Carfish — car UI preview"));

    PreviewScreen screen;
    PhoneManager phone;   // mock contacts/recents on the desktop build

    QQuickView view;
    view.setTitle(QStringLiteral("Carfish — car UI preview"));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    // Stamp a User-Agent on QML network requests so OSM serves real tiles in the
    // preview too (same as the app's Controller does on the device).
    view.engine()->setNetworkAccessManagerFactory(new UaNetworkAccessManagerFactory);
    view.rootContext()->setContextProperty(QStringLiteral("carScreen"), &screen);
    view.rootContext()->setContextProperty(QStringLiteral("phone"), &phone);
    // PREVIEW_PANE=phone|media|apps|maps opens that pane for a still/grab.
    view.rootContext()->setContextProperty(QStringLiteral("previewPane"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_PANE")));
    // PREVIEW_EXPANDED=1 opens the full-screen map state (music as a status bar).
    view.rootContext()->setContextProperty(QStringLiteral("previewExpanded"),
                                           QString::fromLocal8Bit(qgetenv("PREVIEW_EXPANDED")));
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
