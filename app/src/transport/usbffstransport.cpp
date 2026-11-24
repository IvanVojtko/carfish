#include "usbffstransport.h"

#include <QLoggingCategory>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcFfs, "aap.usb.ffs")

// One ep2 read() = one queued USB request. A multiple of the 512-byte high
// speed bulk max packet size; FunctionFS returns EOVERFLOW if a packet exceeds
// the queued request, so keep this comfortably above any AA frame.
static constexpr int kReadBuffer = 127 * 512; // ~64 KiB

// Max bytes per ep1 write() syscall. The MediaTek mtu3 UDC rejects any single
// bulk transfer longer than GPD_BUF_SIZE (65532) with EOPNOTSUPP ("Operation
// not supported"), which killed the session as soon as a large video keyframe
// was written. The host reassembles the AAP byte stream regardless of how the
// writes are split, so cap each write well under that limit.
static constexpr int kWriteChunk = 16 * 1024;

// The car must take our data within this long, or the session is failed (and
// reconnected) instead of hanging. Media flow control keeps the queue small
// while the car is healthy; the byte cap catches a car that reads but stalls.
static constexpr qint64 kWriteStallMs = 8000;
static constexpr qint64 kMaxQueuedBytes = 8 * 1024 * 1024;

// Signal used only to knock the writer thread out of a blocked write() (it
// returns EINTR). Installed without SA_RESTART and with an empty handler.
static int interruptSignal() { return SIGRTMIN + 7; }
static void onInterruptSignal(int) {}

static void installInterruptHandler()
{
    static bool installed = false;
    if (installed)
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onInterruptSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: a blocked read/write must return EINTR
    sigaction(interruptSignal(), &sa, nullptr);
    installed = true;
}

// Clears a published thread id when its run() returns, so nobody signals a
// thread that is gone.
struct TidGuard {
    std::atomic<unsigned long> &tid;
    ~TidGuard() { tid = 0; }
};

static qint64 monoMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// FunctionFS ep0 event types (uapi linux/usb/functionfs.h, enum order).
enum FfsEvent : unsigned char {
    FfsBind = 0,
    FfsUnbind = 1,
    FfsEnable = 2,
    FfsDisable = 3,
    FfsSetup = 4,
    FfsSuspend = 5,
    FfsResume = 6,
};

// AOA GET_PROTOCOL: bmRequestType 0xC0 (dir in | vendor | device), bRequest 51.
// The accessory driver reports AOAP version 2; we answer the same from here so
// a head unit that re-checks on the freshly enumerated 18d1:2d00 gadget gets a
// reply and then writes its version request on ep2.
static constexpr unsigned char kUsbDirIn = 0x80;
static constexpr unsigned char kAccGetProtocol = 51;
static constexpr unsigned char kAoapVersion = 2;

UsbFfsTransport::UsbFfsTransport(const QString &ffsDir, QObject *parent)
    : Transport(parent)
    , m_ffsDir(ffsDir)
{
}

UsbFfsTransport::~UsbFfsTransport()
{
    stop();
}

static int openEp(const QString &dir, const char *name)
{
    const QByteArray path = (dir + QLatin1Char('/') + QLatin1String(name)).toLocal8Bit();
    return ::open(path.constData(), O_RDWR | O_CLOEXEC);
}

void UsbFfsTransport::start()
{
    // ep0 first (control + events), then the bulk pair. The helper has already
    // written the FunctionFS descriptors and bound the UDC, so these exist.
    m_ep0 = openEp(m_ffsDir, "ep0");
    if (m_ep0 < 0) {
        emit disconnected(QStringLiteral("open %1/ep0: %2").arg(m_ffsDir, QString::fromLocal8Bit(strerror(errno))));
        return;
    }
    m_ep1 = openEp(m_ffsDir, "ep1");
    if (m_ep1 < 0) {
        emit disconnected(QStringLiteral("open %1/ep1: %2").arg(m_ffsDir, QString::fromLocal8Bit(strerror(errno))));
        stop();
        return;
    }
    // Non-blocking IN writes so a head unit that is not yet reading cannot pin
    // the session writer (and starve pings) inside one write() syscall.
    ::fcntl(m_ep1, F_SETFL, ::fcntl(m_ep1, F_GETFL, 0) | O_NONBLOCK);
    m_ep2 = openEp(m_ffsDir, "ep2");
    if (m_ep2 < 0) {
        emit disconnected(QStringLiteral("open %1/ep2: %2").arg(m_ffsDir, QString::fromLocal8Bit(strerror(errno))));
        stop();
        return;
    }

    m_pump = new UsbFfsEp0Pump(m_ep0, &m_active);
    connect(m_pump, &UsbFfsEp0Pump::fatal, this, &UsbFfsTransport::onEp0Fatal, Qt::QueuedConnection);
    connect(m_pump, &UsbFfsEp0Pump::note, this, &Transport::note, Qt::QueuedConnection);
    m_pump->start();

    installInterruptHandler();
    m_writer = new UsbFfsWriter(m_ep1);
    connect(m_writer, &UsbFfsWriter::failed, this, [this](const QString &e) { fail(e); },
            Qt::QueuedConnection);
    m_writer->start();
    m_watchdog.setInterval(1000);
    connect(&m_watchdog, &QTimer::timeout, this, &UsbFfsTransport::checkWriter, Qt::UniqueConnection);
    m_watchdog.start();

    m_reader = new UsbFfsReader(m_ep2);
    connect(m_reader, &UsbFfsReader::chunk, this, &UsbFfsTransport::onReadChunk, Qt::QueuedConnection);
    connect(m_reader, &UsbFfsReader::failed, this, &UsbFfsTransport::onReadFailed, Qt::QueuedConnection);
    m_reader->start();

    qCInfo(lcFfs) << "FunctionFS endpoints open at" << m_ffsDir << "— waiting for head unit";
    // The head unit speaks first (version request on ep2); the session just
    // needs to be live and reading.
    emit connected();
}

void UsbFfsTransport::onReadChunk(const QByteArray &data)
{
    m_active.store(true);
    emit dataReceived(data);
}

void UsbFfsTransport::onReadFailed(const QString &error)
{
    fail(QStringLiteral("ep2 read: %1").arg(error));
}

void UsbFfsTransport::onEp0Fatal(const QString &error)
{
    fail(error);
}

void UsbFfsTransport::fail(const QString &why)
{
    if (m_dead.exchange(true))
        return;
    emit disconnected(why);
}

void UsbFfsReader::run()
{
    tid = static_cast<unsigned long>(pthread_self());
    TidGuard guard{ tid };
    QByteArray buf(kReadBuffer, Qt::Uninitialized);
    while (running) {
        const ssize_t n = ::read(m_fd, buf.data(), size_t(buf.size()));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (running)
                emit failed(QString::fromLocal8Bit(strerror(errno)));
            return;
        }
        if (n == 0)
            continue; // zero-length packet
        emit chunk(QByteArray(buf.constData(), int(n)));
    }
}

void UsbFfsEp0Pump::run()
{
    // Each FunctionFS event is a 12-byte record: an 8-byte setup union + a
    // 1-byte type + 3 pad bytes.
    constexpr int kEvSize = 12;
    unsigned char buf[kEvSize * 16];
    bool enabled = false;
    tid = static_cast<unsigned long>(pthread_self());
    TidGuard guard{ tid };
    while (running) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            // Unmount/unbind wakes ep0; report it so the session reconnects
            // instead of hanging forever waiting for the head unit.
            if (running)
                emit fatal(QStringLiteral("ep0 closed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        for (ssize_t off = 0; off + kEvSize <= n; off += kEvSize) {
            const unsigned char type = buf[off + 8];
            switch (type) {
            case FfsEnable:
                if (m_active->load())
                    emit note(QStringLiteral("usb: endpoints re-enabled by the car"));
                enabled = true;
                qCInfo(lcFfs) << "ep0 ENABLE";
                break;
            case FfsResume:
                if (m_active->load())
                    emit note(QStringLiteral("usb: bus resumed by the car"));
                break;
            case FfsSetup:
                handleSetup(buf + off);
                break;
            case FfsDisable:
            case FfsSuspend:
            case FfsUnbind:
                if (m_active->load()) {
                    emit note(QStringLiteral("usb: car sent %1 during the session")
                                  .arg(type == FfsDisable ? QStringLiteral("DISABLE")
                                       : type == FfsSuspend ? QStringLiteral("SUSPEND (bus idle/reset)")
                                                            : QStringLiteral("UNBIND")));
                    // FIFO state is gone; any in-flight AA/TLS frame is already
                    // truncated on the host. Fail so we reconnect cleanly.
                    qCWarning(lcFfs) << "ep0 reset event" << type << "after HU traffic — failing session";
                    emit fatal(QStringLiteral("FunctionFS endpoint reset (event %1)").arg(type));
                    return;
                }
                qCInfo(lcFfs) << "ep0 reset event" << type << "before HU traffic — ignoring (enumeration churn)";
                break;
            default:
                break;
            }
        }
        (void)enabled;
    }
}

void UsbFfsEp0Pump::handleSetup(const unsigned char *setup)
{
    // usb_ctrlrequest, little-endian: bRequestType, bRequest, wValue(2),
    // wIndex(2), wLength(2).
    const unsigned char bRequestType = setup[0];
    const unsigned char bRequest = setup[1];
    const bool in = (bRequestType & kUsbDirIn) != 0;

    if (in && bRequest == kAccGetProtocol) {
        const unsigned char resp[2] = { kAoapVersion, 0 };
        if (::write(m_fd, resp, sizeof(resp)) < 0)
            qCWarning(lcFfs) << "ep0 GET_PROTOCOL reply:" << strerror(errno);
        else
            qCInfo(lcFfs) << "ep0 answered GET_PROTOCOL = AOAP" << int(kAoapVersion);
        return;
    }

    // Stall everything else: Linux f_fs.c halts an IN request on a read of ep0
    // and an OUT request on a write. The resulting EL2HLT (51) is the stall
    // succeeding, not a failure.
    char c = 0;
    if (in)
        (void)!::read(m_fd, &c, 0);
    else
        (void)!::write(m_fd, &c, 0);
}

// Stop one of our endpoint threads. A read/write blocked in FunctionFS is not
// woken by close() from another thread, only by a signal (EINTR) or by the
// endpoint going away, so poke it until it leaves. Deleting a QThread that is
// still running aborts the whole app, so if it really won't stop, leak it.
static void joinThread(QThread *&t, std::atomic<unsigned long> &tid)
{
    if (!t)
        return;
    for (int i = 0; i < 20 && !t->wait(50); ++i) {
        const unsigned long id = tid.load();
        if (id)
            pthread_kill(pthread_t(id), interruptSignal());
    }
    if (t->wait(500))
        delete t;
    else
        qCWarning(lcFfs) << "USB thread did not stop; leaking it";
    t = nullptr;
}

void UsbFfsTransport::stop()
{
    m_dead.store(true);
    m_watchdog.stop();
    if (m_writer) {
        m_writer->shutdown();
        QThread *t = m_writer;
        joinThread(t, m_writer->m_tid);
        m_writer = nullptr;
    }
    if (m_reader) {
        m_reader->running = false;
        QThread *t = m_reader;
        joinThread(t, m_reader->tid);
        m_reader = nullptr;
    }
    if (m_pump) {
        m_pump->running = false;
        QThread *t = m_pump;
        joinThread(t, m_pump->tid);
        m_pump = nullptr;
    }
    // Close only after the threads are gone, so none of them can touch a
    // descriptor number that has been reused meanwhile.
    for (int *fd : { &m_ep2, &m_ep1, &m_ep0 }) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

void UsbFfsTransport::write(const QByteArray &data)
{
    // Never blocks: the writer thread puts it on the wire in order. Errors come
    // back queued, so the session never fails (and tears down) from inside its
    // own send().
    if (m_dead.load() || !m_writer)
        return;
    if (!m_writer->enqueue(data)) {
        QMetaObject::invokeMethod(this, "onEp0Fatal", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("the car stopped reading USB (send queue full)")));
    }
}

void UsbFfsTransport::checkWriter()
{
    if (!m_writer || m_dead.load())
        return;
    const qint64 blocked = m_writer->blockedMs();
    if (blocked >= kWriteStallMs) {
        qCWarning(lcFfs) << "ep1 write blocked for" << blocked << "ms — failing session";
        fail(QStringLiteral("the car stopped reading USB for %1 s").arg(blocked / 1000));
    }
}

QString UsbFfsTransport::takeStats()
{
    return m_writer ? m_writer->takeStats() : QString();
}

bool UsbFfsWriter::enqueue(const QByteArray &frame)
{
    QMutexLocker lock(&m_mutex);
    if (m_queued + frame.size() > kMaxQueuedBytes)
        return false;
    m_queue.enqueue(frame);
    m_queued += frame.size();
    m_cond.wakeOne();
    return true;
}

qint64 UsbFfsWriter::blockedMs() const
{
    const qint64 since = m_busySinceMs.load();
    return since ? monoMs() - since : 0;
}

qint64 UsbFfsWriter::queuedBytes()
{
    QMutexLocker lock(&m_mutex);
    return m_queued;
}

QString UsbFfsWriter::takeStats()
{
    QMutexLocker lock(&m_mutex);
    QString s = QStringLiteral("usb out %1 KB").arg(m_bytes / 1024);
    if (m_slow)
        s += QStringLiteral(", car slow to read %1x (longest %2 ms)").arg(m_slow).arg(m_maxWaitMs);
    if (m_queued > 64 * 1024)
        s += QStringLiteral(", %1 KB queued").arg(m_queued / 1024);
    m_bytes = 0;
    m_slow = 0;
    m_maxWaitMs = 0;
    return s;
}

void UsbFfsWriter::shutdown()
{
    m_running = false;
    QMutexLocker lock(&m_mutex);
    m_cond.wakeAll();
}

void UsbFfsWriter::run()
{
    m_tid = static_cast<unsigned long>(pthread_self());
    TidGuard guard{ m_tid };
    while (m_running) {
        QByteArray frame;
        {
            QMutexLocker lock(&m_mutex);
            while (m_running && m_queue.isEmpty())
                m_cond.wait(&m_mutex);
            if (!m_running)
                break;
            frame = m_queue.dequeue();
            m_queued -= frame.size();
        }
        if (!writeFrame(frame))
            break;
    }
}

bool UsbFfsWriter::writeFrame(const QByteArray &frame)
{
    // Once any byte of a frame is on the wire it must be finished (no
    // interleaving mid-record). ep1 is O_NONBLOCK, which only matters while
    // the endpoint is not enabled (EAGAIN): retry briefly then.
    const char *p = frame.constData();
    qint64 left = frame.size();
    const qint64 started = monoMs();
    while (left > 0) {
        if (!m_running)
            return false;
        // Cap each transfer so an oversized frame cannot trip the UDC's
        // max-transfer limit (EOPNOTSUPP on mtu3).
        const size_t want = size_t(qMin<qint64>(left, kWriteChunk));
        m_busySinceMs = monoMs();
        const ssize_t n = ::write(m_fd, p, want);
        const int err = errno;
        m_busySinceMs = 0;
        if (n > 0) {
            p += n;
            left -= n;
            continue;
        }
        if (n < 0 && err == EINTR)
            continue;
        if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
            if (monoMs() - started >= kWriteStallMs) {
                emit failed(QStringLiteral("the car stopped reading USB"));
                return false;
            }
            m_busySinceMs = started;
            struct timespec ts { 0, 2 * 1000 * 1000 }; // 2 ms
            ::nanosleep(&ts, nullptr);
            m_busySinceMs = 0;
            continue;
        }
        if (m_running) {
            qCWarning(lcFfs) << "ep1 write failed:" << strerror(err);
            emit failed(QString::fromLocal8Bit(strerror(err)));
        }
        return false;
    }
    const qint64 took = monoMs() - started;
    QMutexLocker lock(&m_mutex);
    m_bytes += frame.size();
    if (took > 100)
        ++m_slow;
    m_maxWaitMs = qMax(m_maxWaitMs, took);
    return true;
}

QString UsbFfsTransport::describe() const
{
    return QStringLiteral("USB accessory (FunctionFS %1)").arg(m_ffsDir);
}
