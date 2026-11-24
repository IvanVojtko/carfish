#pragma once

#include "transport.h"

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <atomic>

// Wired Android Auto over a FunctionFS accessory gadget.
//
// On these Sailfish kernels the car switches the phone into Android Open
// Accessory mode, but the kernel's f_accessory function answers the AOA
// control requests WITHOUT re-enumerating as 18d1:2d00 — so the head unit
// never sees an accessory interface and never sends AAP bytes. The fix
// (reverse-engineered from mrcyjanek/cargadgetd) is for userspace to rebuild
// the configfs gadget as 18d1:2d00 with a FunctionFS accessory interface
// (one vendor interface FF/FF/00, bulk IN ep1 + bulk OUT ep2). The privileged
// carfish-helper does that switch and hands us the FunctionFS mount dir; this
// transport then drives AAP over the bulk pair.
//
//   ep1 = device -> host  (we WRITE here)
//   ep2 = host -> device  (we READ here)
//   ep0 = control + FunctionFS event stream (BIND/ENABLE/SETUP/DISABLE...)
//
// We open our own ep0 handle to pump events and answer GET_PROTOCOL (the HU
// re-checks the AOA protocol version on the freshly enumerated gadget); the
// helper keeps a separate ep0 handle open only to hold the gadget alive.
class UsbFfsReader;
class UsbFfsWriter;
class UsbFfsEp0Pump;

class UsbFfsTransport : public Transport
{
    Q_OBJECT
public:
    explicit UsbFfsTransport(const QString &ffsDir, QObject *parent = nullptr);
    ~UsbFfsTransport() override;

    void start() override;
    void stop() override;
    void write(const QByteArray &data) override;
    QString describe() const override;
    QString takeStats() override;

private slots:
    void onReadChunk(const QByteArray &data);
    void onReadFailed(const QString &error);
    void onEp0Fatal(const QString &error);
    void checkWriter();

private:
    void fail(const QString &why);

    QString m_ffsDir;
    int m_ep0 = -1;
    int m_ep1 = -1; // IN  (write to HU)
    int m_ep2 = -1; // OUT (read from HU)
    UsbFfsReader *m_reader = nullptr;
    UsbFfsWriter *m_writer = nullptr;
    UsbFfsEp0Pump *m_pump = nullptr;
    QTimer m_watchdog; // fails the session if the car stops reading
    std::atomic_bool m_active{false}; // HU has actually sent bytes
    std::atomic_bool m_dead{false};
};

// ep1 (device->host) writer thread. A FunctionFS write blocks in the kernel
// until the car has read the data (O_NONBLOCK only covers a disabled
// endpoint), so writing from the GUI thread froze the whole app whenever the
// car stopped reading. Frames are queued here in send order instead.
class UsbFfsWriter : public QThread
{
    Q_OBJECT
public:
    explicit UsbFfsWriter(int fd) : m_fd(fd) {}
    bool enqueue(const QByteArray &frame); // false: queue over its limit
    void shutdown();                       // stop, interrupting a blocked write
    qint64 blockedMs() const;              // how long the current write has been stuck
    qint64 queuedBytes();
    QString takeStats();

signals:
    void failed(const QString &error);

protected:
    void run() override;

private:
    bool writeFrame(const QByteArray &frame);

    int m_fd;
    bool m_zlp = true; // end 512-multiple frames with a zero-length packet
    std::atomic_bool m_running{true};
    std::atomic<qint64> m_busySinceMs{0};
public:
    std::atomic<unsigned long> m_tid{0};
private:
    QMutex m_mutex;
    QWaitCondition m_cond;
    QQueue<QByteArray> m_queue;
    qint64 m_queued = 0;
    // since the last takeStats()
    qint64 m_bytes = 0;
    int m_slow = 0;      // frames that waited > 100 ms for the car
    qint64 m_maxWaitMs = 0;
};

// Blocking ep2 (host->device) reader thread.
class UsbFfsReader : public QThread
{
    Q_OBJECT
public:
    explicit UsbFfsReader(int fd) : m_fd(fd) {}
    std::atomic_bool running{true};
    std::atomic<unsigned long> tid{0};

signals:
    void chunk(const QByteArray &data);
    void failed(const QString &error);

protected:
    void run() override;

private:
    int m_fd;
};

// ep0 FunctionFS event pump: answers GET_PROTOCOL, stalls other control
// requests, and reports session-fatal endpoint resets (DISABLE/SUSPEND/
// UNBIND after the HU has started talking).
class UsbFfsEp0Pump : public QThread
{
    Q_OBJECT
public:
    UsbFfsEp0Pump(int fd, std::atomic_bool *active) : m_fd(fd), m_active(active) {}
    std::atomic_bool running{true};
    std::atomic<unsigned long> tid{0};

signals:
    void fatal(const QString &error);
    void note(const QString &line);

protected:
    void run() override;

private:
    void handleSetup(const unsigned char *setup);
    int m_fd;
    std::atomic_bool *m_active;
};
