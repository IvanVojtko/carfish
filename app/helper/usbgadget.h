#pragma once

#include <QByteArray>
#include <QDBusAbstractAdaptor>
#include <QElapsedTimer>
#include <QTimer>
#include <QObject>
#include <QPointer>
#include <QList>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>

// Root-side wired Android Auto bring-up for a stock Sailfish OS.
//
// Reverse-engineered from mrcyjanek/cargadgetd. On these kernels the car (the
// USB host) switches the phone into Android Open Accessory mode, but the
// f_accessory function answers the AOA control requests WITHOUT re-enumerating
// as 18d1:2d00 — so the head unit never sees an accessory interface and never
// sends AAP bytes. This helper:
//
//   1. stops usb-moded (it would rewrite the gadget back to charging/MTP),
//   2. links the accessory function so the car's GET_PROTOCOL is answered,
//   3. waits for AOA START (poll /dev/usb_accessory ioctls) for Android Auto,
//   4. rebuilds the configfs gadget as 18d1:2d00 with a FunctionFS accessory
//      interface (one vendor interface FF/FF/00, bulk IN ep1 + bulk OUT ep2),
//      binds the UDC, and hands the app the FunctionFS mount dir, and
//   5. restores the gadget and restarts usb-moded on stop / cable unplug.
//
// The unprivileged app then drives AAP over ep1/ep2 (UsbFfsTransport).

class UsbGadgetWorker : public QThread
{
    Q_OBJECT
public:
    explicit UsbGadgetWorker(QObject *parent = nullptr);

    void requestStop() { m_stop.store(true); }
    bool stopping() const { return m_stop.load(); }

signals:
    void log(const QString &line);
    void startRequested(const QString &model, const QString &serial);
    void ready(const QString &ffsDir);
    void failed(const QString &reason);
    void stopped();

protected:
    void run() override;

private:
    // Discovery
    QString findGadget();
    QString firstUdc();
    QString firstConfig(const QString &gadget);
    QString accessoryFunction(const QString &gadget); // e.g. "accessory.gs2", created if absent

    // Recover from a previous accessory session that was interrupted (e.g. the
    // helper was restarted by an upgrade) and left the gadget as 18d1:2d00 — the
    // car would then never re-do the AOA handshake. Tear it down and let usb-moded
    // rebuild a normal gadget so the car enumerates fresh.
    bool gadgetIsAccessoryLeftover(const QString &gadget);
    void resetLeftoverAccessory(const QString &gadget);

    // Ask phase: make the car's GET_PROTOCOL be answered.
    void linkAskAccessory(const QString &gadget, const QString &config, const QString &udc);

    // AOA START edge detection via /dev/usb_accessory ioctls.
    bool waitForStart(QString *model, QString *serial);

    // Build the 18d1:2d00 FunctionFS accessory gadget and bind it.
    bool buildAccessoryGadget(const QString &gadget, const QString &config, const QString &udc,
                              QString *ffsDir, QString *err);
    bool writeFfsDescriptors(const QString &ffsDir, QString *err);
    void exposeEndpoints(const QString &ffsDir);

    // Restore usb-moded's gadget.
    void restore();

    // Write the gadget's UDC file with a timeout (see writeUdc in the .cpp).
    bool setUdc(const QString &gadget, const QString &udc, const char *why);
    bool m_udcStuck = false;   // a UDC write of ours never returned

    std::atomic_bool m_stop{false};

    // Gadget bits we need to clean up.
    QString m_gadget;
    QString m_config;
    QString m_udc;
    QString m_ffsDir;
    int m_ep0 = -1; // held open for the FunctionFS bind lifetime
    QByteArray m_savedVendor;
    QByteArray m_savedProduct;
    bool m_usbModedStopped = false;
    bool m_built = false;
    int m_uid = 0;
    int m_gid = 0;
};

// Owns the worker and re-emits its signals; exposed on D-Bus by UsbAdaptor.
class UsbGadget : public QObject
{
    Q_OBJECT
public:
    explicit UsbGadget(QObject *parent = nullptr);
    ~UsbGadget() override;

    void start();
    void stop();

signals:
    void log(const QString &line);
    void startRequested(const QString &model, const QString &serial);
    void ready(const QString &ffsDir);
    void failed(const QString &reason);
    // A USB host (a car or a computer, not a plain charger) was just plugged
    // in while no wired session was running.
    void cablePlugged();

private:
    // Cable watch: kernel uevents (power supply / USB) wake a short poll of the
    // UDC state; "not attached" -> "configured" outside our own gadget work is a
    // plug. Also touches kPlugTrigger when the app isn't running, which the
    // user unit carfish-usb-autostart.path watches to launch it.
    void setupCableWatch();
    void onUevent();
    void checkCable();
    bool busy() const;
    static bool appRunning();
    static void touchPlugTrigger();
    int m_uevent = -1;
    QTimer *m_cablePoll = nullptr;
    int m_pollTicks = 0;
    QString m_udcState;
    QElapsedTimer m_lastGadgetWork; // our own unbind/rebind look like replugs

    UsbGadgetWorker *m_worker = nullptr;
    // Workers that did not stop in time (blocked in the kernel); they delete
    // themselves when they finish. New attempts refuse to start meanwhile.
    QList<QPointer<UsbGadgetWorker>> m_abandoned;
};

// D-Bus face of UsbGadget (interface org.carfish.Usb1).
class UsbAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.carfish.Usb1")
public:
    explicit UsbAdaptor(UsbGadget *usb);

public slots:
    void Start();
    void Stop();

signals:
    void CablePlugged();
    void Ready(const QString &ffsDir);
    void StartRequested(const QString &model, const QString &serial);
    void Failed(const QString &reason);
    void Log(const QString &line);

private:
    UsbGadget *m_usb;
};
