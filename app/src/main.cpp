#include "controller.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QScopedPointer>
#include <QTimer>

#ifndef CARFISH_DESKTOP
#include <sailfishapp.h>
#endif

#include <gst/gst.h>

#include <QDir>

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

// A crash used to leave no trace in carfish.log (the system journal needs
// root to read). On a fatal signal, append the signal and a raw backtrace to
// the log with async-signal-safe calls, then die the normal way.
static int g_crashFd = -1;

static void crashHandler(int sig)
{
    if (g_crashFd >= 0) {
        char line[96];
        const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : sig == SIGBUS ? "SIGBUS"
                         : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "signal";
        size_t n = 0;
        const char *pre = "CRASH: Carfish died on ";
        for (const char *p = pre; *p && n < sizeof(line) - 2; ++p) line[n++] = *p;
        for (const char *p = name; *p && n < sizeof(line) - 2; ++p) line[n++] = *p;
        line[n++] = '\n';
        (void)!::write(g_crashFd, line, n);
#if defined(__GLIBC__)
        void *frames[48];
        const int depth = backtrace(frames, 48);
        backtrace_symbols_fd(frames, depth, g_crashFd);
#endif
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

static void installCrashHandler()
{
    const QByteArray path = (QDir::homePath() + QStringLiteral("/carfish.log")).toLocal8Bit();
    g_crashFd = ::open(path.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
#if defined(__GLIBC__)
    // backtrace() loads libgcc on first use, which is not safe inside a signal
    // handler; warm it up now.
    void *warm[1];
    backtrace(warm, 1);
#endif
    for (int sig : { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL })
        ::signal(sig, crashHandler);
}

// Headless mode runs a session without the phone UI, e.g. over SSH on the
// phone or on a desktop PC against a fake head unit:
//   carfish --headless --mode tcp-server --cert c.pem --key k.pem
static int runHeadless(QGuiApplication &app)
{
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({ QStringLiteral("headless"), QStringLiteral("Run without UI.") });
    p.addOption({ QStringLiteral("mode"), QStringLiteral("wireless | usb | tcp-server"), QStringLiteral("mode"),
                  QStringLiteral("tcp-server") });
    p.addOption({ QStringLiteral("cert"), QStringLiteral("PEM certificate chain."), QStringLiteral("file") });
    p.addOption({ QStringLiteral("key"), QStringLiteral("PEM private key."), QStringLiteral("file") });
    p.addOption({ QStringLiteral("car-qml"), QStringLiteral("QML scene projected to the car."), QStringLiteral("file") });
    p.addOption({ QStringLiteral("audio-device"),
                  QStringLiteral("PulseAudio source for car media audio, or \"none\" (default: loopback capture)."),
                  QStringLiteral("source") });
    p.addOption({ QStringLiteral("preview"),
                  QStringLiteral("Render the car UI to ~/carfish-preview.png (no car).") });
    p.addOption({ QStringLiteral("preview-size"),
                  QStringLiteral("Preview resolution, e.g. 800x480 (default 1920x1080)."),
                  QStringLiteral("WxH") });
    p.process(app);

    QQmlEngine engine;
    Controller controller(&engine);
    if (p.isSet(QStringLiteral("cert")))
        controller.setCertPath(p.value(QStringLiteral("cert")));
    if (p.isSet(QStringLiteral("key")))
        controller.setKeyPath(p.value(QStringLiteral("key")));
    if (p.isSet(QStringLiteral("audio-device")))
        controller.setAudioDevice(p.value(QStringLiteral("audio-device")));
    if (p.isSet(QStringLiteral("car-qml")))
        controller.setCarQml(QUrl::fromLocalFile(p.value(QStringLiteral("car-qml"))));

    // Desk preview: render the projected UI and dump PNGs, no transport/session.
    if (p.isSet(QStringLiteral("preview"))) {
        QSize size(1920, 1080);
        if (p.isSet(QStringLiteral("preview-size"))) {
            const QStringList wh = p.value(QStringLiteral("preview-size")).split(QLatin1Char('x'));
            if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0)
                size = QSize(wh[0].toInt(), wh[1].toInt());
        }
        controller.startPreview(size);
        return app.exec();
    }

    const QString mode = p.value(QStringLiteral("mode"));
    controller.start(mode == QLatin1String("wireless") ? Controller::Wireless
                     : mode == QLatin1String("usb")    ? Controller::Usb
                                                       : Controller::TcpServer);
    // start() bails synchronously (without ever running) when the credentials
    // in phone storage are missing or invalid; don't hang the event loop.
    if (!controller.running()) {
        fprintf(stderr, "carfish: %s\n", qPrintable(controller.certStatus()));
        return 1;
    }
    QObject::connect(&controller, &Controller::runningChanged, &app, [&] {
        if (!controller.running())
            app.exit(1);
    });
    return app.exec();
}

int main(int argc, char *argv[])
{
    installCrashHandler();
    gst_init(&argc, &argv);

#ifdef CARFISH_DESKTOP
    QGuiApplication app(argc, argv);
    return runHeadless(app);
#else
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    if (app->arguments().contains(QStringLiteral("--headless")))
        return runHeadless(*app);

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    Controller controller(view->engine());
    controller.appendLog(QStringLiteral("app started (pid %1)").arg(QCoreApplication::applicationPid()));
    QObject::connect(app.data(), &QCoreApplication::aboutToQuit, &controller,
                     [&controller] { controller.appendLog(QStringLiteral("app closed")); });
    controller.setCarQml(QUrl::fromLocalFile(SailfishApp::pathTo(QStringLiteral("carqml/CarMain.qml")).toLocalFile()));
    view->rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    view->setSource(SailfishApp::pathToMainQml());
    view->show();
    // Launched by the carfish-usb-autostart user unit: a USB cable was plugged in.
    if (app->arguments().contains(QStringLiteral("--usb-autostart")))
        QTimer::singleShot(0, &controller, [&controller] { controller.startUsbAuto(true); });
    return app->exec();
#endif
}
