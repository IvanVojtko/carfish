#include "usbffstransport.h"

#include <QLoggingCategory>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
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

void UsbFfsTransport::stop()
{
    m_dead.store(true);
    if (m_reader)
        m_reader->running = false;
    if (m_pump)
        m_pump->running = false;
    // Closing the bulk fds unblocks the reader; closing ep0 unblocks the pump.
    if (m_ep2 >= 0) {
        ::close(m_ep2);
        m_ep2 = -1;
    }
    if (m_ep1 >= 0) {
        ::close(m_ep1);
        m_ep1 = -1;
    }
    if (m_ep0 >= 0) {
        ::close(m_ep0);
        m_ep0 = -1;
    }
    if (m_reader) {
        m_reader->wait(1000);
        delete m_reader;
        m_reader = nullptr;
    }
    if (m_pump) {
        m_pump->wait(1000);
        delete m_pump;
        m_pump = nullptr;
    }
}

void UsbFfsTransport::write(const QByteArray &data)
{
    // ep1 is O_NONBLOCK. Once any byte of this frame is on the wire we must
    // finish it (no interleaving mid-record), but if the host is not reading IN
    // yet we retry with a short sleep rather than block the writer forever. A
    // long stall fails the session so it reconnects instead of hanging.
    QMutexLocker lock(&m_writeMutex);
    if (m_ep1 < 0 || m_dead.load())
        return;

    const char *p = data.constData();
    qint64 left = data.size();
    m_bytes += left;
    int stallMs = 0;
    int frameWaitMs = 0;
    constexpr int kStallLimitMs = 10000;
    while (left > 0 && m_ep1 >= 0) {
        // Cap each transfer so an oversized frame cannot trip the UDC's
        // max-transfer limit (EOPNOTSUPP on mtu3).
        const size_t want = size_t(qMin<qint64>(left, kWriteChunk));
        const ssize_t n = ::write(m_ep1, p, want);
        if (n > 0) {
            p += n;
            left -= n;
            stallMs = 0;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts { 0, 2 * 1000 * 1000 }; // 2 ms
                ::nanosleep(&ts, nullptr);
                if (stallMs == 0)
                    ++m_waits;
                stallMs += 2;
                frameWaitMs += 2;
                m_maxWaitMs = qMax(m_maxWaitMs, frameWaitMs);
                if (stallMs >= kStallLimitMs) {
                    qCWarning(lcFfs) << "ep1 write stalled >" << kStallLimitMs << "ms — failing session";
                    fail(QStringLiteral("ep1 write stalled"));
                    return;
                }
                continue;
            }
            qCWarning(lcFfs) << "ep1 write failed:" << strerror(errno);
            fail(QString::fromLocal8Bit(strerror(errno)));
            return;
        }
    }
}

QString UsbFfsTransport::takeStats()
{
    QMutexLocker lock(&m_writeMutex);
    QString s = QStringLiteral("usb out %1 KB").arg(m_bytes / 1024);
    if (m_waits)
        s += QStringLiteral(", car not reading %1x (longest %2 ms)").arg(m_waits).arg(m_maxWaitMs);
    m_waits = 0;
    m_maxWaitMs = 0;
    m_bytes = 0;
    return s;
}

QString UsbFfsTransport::describe() const
{
    return QStringLiteral("USB accessory (FunctionFS %1)").arg(m_ffsDir);
}
