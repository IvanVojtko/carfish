#pragma once

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class QImage;

class AppRegistry;
class CarScreen;
class MprisController;
class PhoneManager;
class QQmlEngine;
class QQmlNetworkAccessManagerFactory;
class Transport;
class WirelessSetup;
namespace aap { class Session; }

// Glue between the phone UI and the projection pipeline.
class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(QString certPath READ certPath WRITE setCertPath NOTIFY certPathChanged)
    Q_PROPERTY(QString keyPath READ keyPath WRITE setKeyPath NOTIFY keyPathChanged)
    Q_PROPERTY(QString certStatus READ certStatus NOTIFY certStatusChanged)
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QString navApp READ navApp WRITE setNavApp NOTIFY navAppChanged)
    Q_PROPERTY(QString mediaApp READ mediaApp WRITE setMediaApp NOTIFY mediaAppChanged)
    Q_PROPERTY(bool autoStart READ autoStart WRITE setAutoStart NOTIFY autoStartChanged)
public:
    enum Mode { Wireless = 0, Usb = 1, TcpServer = 2 };
    Q_ENUM(Mode)

    explicit Controller(QQmlEngine *engine, QObject *parent = nullptr);
    ~Controller() override;

    int mode() const { return m_mode; }
    bool running() const { return m_running; }
    QString state() const { return m_state; }
    QString log() const { return m_log.join(QLatin1Char('\n')); }
    QString certPath() const;
    void setCertPath(const QString &path);
    QString keyPath() const;
    void setKeyPath(const QString &path);
    QString certStatus() const { return m_certStatus; }

    QVariantList apps() const;
    QString navApp() const;
    void setNavApp(const QString &id);
    QString mediaApp() const;
    void setMediaApp(const QString &id);
    bool autoStart() const;
    void setAutoStart(bool on);

    void setCarQml(const QUrl &url) { m_carQml = url; }

    Q_INVOKABLE void start(int mode);
    Q_INVOKABLE void stop();
    // Render the car UI off-car and dump frames to ~/carfish-preview.png,
    // so the projected screen (map etc.) can be inspected on the desk without a
    // head unit. No transport, no session, no credentials needed.
    Q_INVOKABLE void startPreview(const QSize &size = QSize(1920, 1080), int fps = 5);
    Q_INVOKABLE void refreshCertStatus();
    Q_INVOKABLE void refreshApps();
    Q_INVOKABLE QString appName(const QString &id) const;
    Q_INVOKABLE bool launchApp(const QString &id);

signals:
    void modeChanged();
    void runningChanged();
    void stateChanged();
    void logChanged();
    void certPathChanged();
    void keyPathChanged();
    void certStatusChanged();
    void appsChanged();
    void navAppChanged();
    void mediaAppChanged();
    void autoStartChanged();

private:
    void setState(const QString &state);
    void appendLog(const QString &line);
    void startSession(Transport *transport);
    void endSession(const QString &reason);
    void launchBackendApps();
    void savePreviewFrame(const QImage &rgba); // write the preview PNG (throttled)
    void checkAutoStart();                 // poll for a connected car
    bool usbCarConnected() const;          // AOA gadget actively configured by a head unit
    bool aaHeadUnitConnected() const;      // a paired BlueZ device advertising AA is connected

    QQmlEngine *m_engine;
    QSettings m_settings;
    AppRegistry *m_apps;
    MprisController *m_mpris;
    PhoneManager *m_phone;
    int m_mode = Wireless;
    bool m_running = false;
    QString m_state;
    QString m_certStatus;
    QStringList m_log;
    QUrl m_carQml = QUrl::fromLocalFile(QStringLiteral("/usr/share/carfish/carqml/CarMain.qml"));
    CarScreen *m_screen;
    QQmlNetworkAccessManagerFactory *m_namFactory = nullptr; // sets the OSM User-Agent
    WirelessSetup *m_wireless = nullptr;
    QPointer<Transport> m_transport;
    QPointer<aap::Session> m_session;

    QTimer m_autoDetect;       // polls for a car when auto-start is enabled
    bool m_carPresent = false; // edge-trigger: only auto-start on connect
    int m_previewFrames = 0;   // frames seen in preview mode (for throttling)
};
