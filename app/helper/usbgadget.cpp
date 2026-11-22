#include "usbgadget.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QProcess>

#include <QDebug>
#include <QPointer>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- f_accessory ioctls (linux drivers/usb/gadget/function/f_accessory.h) ----
// _IOW('M', n, char[256]) for the strings, _IO('M', 7) for the start flag.
static constexpr unsigned long ACCESSORY_GET_STRING_MANUFACTURER = 0x41004d01UL;
static constexpr unsigned long ACCESSORY_GET_STRING_MODEL = 0x41004d02UL;
static constexpr unsigned long ACCESSORY_GET_STRING_DESCRIPTION = 0x41004d03UL;
static constexpr unsigned long ACCESSORY_GET_STRING_VERSION = 0x41004d04UL;
static constexpr unsigned long ACCESSORY_GET_STRING_URI = 0x41004d05UL;
static constexpr unsigned long ACCESSORY_GET_STRING_SERIAL = 0x41004d06UL;
static constexpr unsigned long ACCESSORY_IS_START_REQUESTED = 0x00004d07UL;

static const char *kAccessoryDev = "/dev/usb_accessory";
static const char *kGadgetRoot = "/sys/kernel/config/usb_gadget";
static const char *kUdcClass = "/sys/class/udc";
static const char *kDefaultFfsDir = "/dev/usb-ffs/aa";

// ---------------------------------------------------------------------------
// small sysfs/file helpers
// ---------------------------------------------------------------------------
static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    return ok;
}

static bool writeStr(const QString &path, const QString &val)
{
    return writeFile(path, (val + QLatin1Char('\n')).toLatin1());
}

static QByteArray readFileRaw(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

static QString readTrim(const QString &path)
{
    return QString::fromLatin1(readFileRaw(path)).trimmed();
}

static void runCmd(const QString &program, const QStringList &args, int timeoutMs = 10000)
{
    QProcess p;
    p.start(program, args);
    p.waitForFinished(timeoutMs);
}

// Writing a gadget's UDC file (bind/unbind) can block inside the kernel; seen
// in the field right after a car's AOA START on reconnect, which froze the
// worker for good (no log, no restore, usb-moded left stopped). Do the write
// on a throwaway thread and stop waiting after timeoutMs so we can report and
// clean up instead. A write that never returns leaks its thread; while one is
// outstanding g_udcStuck stays set and new attempts refuse to start (any
// further UDC write would just queue behind the same kernel lock).
static std::atomic_bool g_udcStuck{false};

enum class UdcResult { Ok, Failed, TimedOut };

static UdcResult writeUdc(const QString &gadget, const QString &value, int timeoutMs, qint64 *tookMs)
{
    QElapsedTimer t;
    t.start();
    if (g_udcStuck.load()) {
        *tookMs = 0;
        return UdcResult::TimedOut;
    }
    auto result = std::make_shared<std::promise<bool>>();
    std::future<bool> done = result->get_future();
    const QString path = gadget + QStringLiteral("/UDC");
    g_udcStuck.store(true);
    std::thread([result, path, value] {
        const bool ok = writeStr(path, value);
        g_udcStuck.store(false);
        result->set_value(ok);
    }).detach();
    const bool finished = done.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
    *tookMs = t.elapsed();
    if (!finished)
        return UdcResult::TimedOut; // g_udcStuck stays set until the write returns
    return done.get() ? UdcResult::Ok : UdcResult::Failed;
}

static bool serviceActive(const QString &unit)
{
    QProcess p;
    p.start(QStringLiteral("systemctl"), { QStringLiteral("is-active"), QStringLiteral("--quiet"), unit });
    p.waitForFinished(5000);
    return p.exitCode() == 0;
}

static bool serviceEnabled(const QString &unit)
{
    QProcess p;
    p.start(QStringLiteral("systemctl"), { QStringLiteral("is-enabled"), QStringLiteral("--quiet"), unit });
    p.waitForFinished(5000);
    return p.exitCode() == 0;   // enabled / static / alias / indirect
}

// ---------------------------------------------------------------------------
// UsbGadgetWorker
// ---------------------------------------------------------------------------
// Bind (udc = controller name) or unbind (udc empty) the gadget with a 5 s
// limit, logging slow or stuck writes. False when it failed or got stuck.
bool UsbGadgetWorker::setUdc(const QString &gadget, const QString &udc, const char *why)
{
    const QString what = udc.isEmpty() ? QStringLiteral("unbind") : QStringLiteral("bind");
    qint64 took = 0;
    const UdcResult r = writeUdc(gadget, udc, 5000, &took);
    if (r == UdcResult::TimedOut) {
        m_udcStuck = true;
        emit log(QStringLiteral("usb: UDC %1 (%2) is stuck in the kernel (no return after %3 ms); "
                                "unplug the USB cable, or reboot the phone if that does not help")
                     .arg(what, QLatin1String(why)).arg(took));
        return false;
    }
    if (took > 1000)
        emit log(QStringLiteral("usb: UDC %1 (%2) took %3 ms").arg(what, QLatin1String(why)).arg(took));
    if (r == UdcResult::Failed) {
        emit log(QStringLiteral("usb: UDC %1 (%2) failed (%3)")
                     .arg(what, QLatin1String(why), udc.isEmpty() ? QStringLiteral("not bound?") : udc));
        return false;
    }
    return true;
}

UsbGadgetWorker::UsbGadgetWorker(QObject *parent)
    : QThread(parent)
{
    // The unprivileged app runs as the device user; resolve it so we can hand
    // the FunctionFS endpoint nodes over after we build the gadget as root.
    if (struct passwd *pw = getpwnam("defaultuser")) {
        m_uid = int(pw->pw_uid);
        m_gid = int(pw->pw_gid);
    } else {
        m_uid = 100000; // Sailfish default device user
        m_gid = 100000;
    }
}

QString UsbGadgetWorker::findGadget()
{
    QDir root(QString::fromLatin1(kGadgetRoot));
    if (!root.exists())
        return QString();
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.contains(QStringLiteral("g1")))
        return root.filePath(QStringLiteral("g1"));
    return entries.isEmpty() ? QString() : root.filePath(entries.first());
}

QString UsbGadgetWorker::firstUdc()
{
    QDir d(QString::fromLatin1(kUdcClass));
    const QStringList e = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    return e.isEmpty() ? QString() : e.first();
}

QString UsbGadgetWorker::firstConfig(const QString &gadget)
{
    QDir d(gadget + QStringLiteral("/configs"));
    const QStringList e = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (e.contains(QStringLiteral("c.1")))
        return QStringLiteral("c.1");
    if (e.contains(QStringLiteral("b.1")))
        return QStringLiteral("b.1");
    return e.isEmpty() ? QString() : e.first();
}

QString UsbGadgetWorker::accessoryFunction(const QString &gadget)
{
    QDir d(gadget + QStringLiteral("/functions"));
    const QStringList fns = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &fn : fns) {
        if (fn.startsWith(QStringLiteral("accessory.")))
            return fn;
    }
    // Create the instance (the kernel registers it on mkdir when f_accessory
    // is built in).
    const QString name = QStringLiteral("accessory.gs2");
    if (QDir().mkpath(gadget + QStringLiteral("/functions/") + name))
        return name;
    return QString();
}

void UsbGadgetWorker::linkAskAccessory(const QString &gadget, const QString &config, const QString &udc)
{
    // Make the car's AOA GET_PROTOCOL be answered while usb-moded's ask gadget
    // (or whatever is bound) is enumerated, so the kernel sets the start flag
    // we poll. On vendor 0x18d1 kernels the device setup hook already answers
    // (cargadgetd's finding on the Jolla C2); elsewhere the accessory function
    // must be linked into the config (e.g. MediaTek 0e8d). Skip the disruptive
    // unbind/relink when we are already 0x18d1.
    const QString vendor = readTrim(gadget + QStringLiteral("/idVendor"));
    if (vendor.contains(QLatin1String("18d1"))) {
        emit log(QStringLiteral("usb: vendor 0x18d1 answers AOA GET_PROTOCOL directly; no relink"));
        return;
    }
    const QString fn = accessoryFunction(gadget);
    if (fn.isEmpty())
        return;
    const QString link = gadget + QStringLiteral("/configs/") + config + QLatin1Char('/') + fn;
    if (QFileInfo::exists(link))
        return; // already linked
    // Unbind, link, rebind so the host re-enumerates with the accessory iface.
    // configfs resolves the symlink target against the process CWD, so use an
    // ABSOLUTE target (a relative one fails with ENOENT since the helper's CWD
    // is not the gadget dir, and chdir() is unsafe with the D-Bus main thread).
    if (!setUdc(gadget, QString(), "relink accessory"))
        return;
    QThread::msleep(150);
    const QByteArray fnTarget = (gadget + QStringLiteral("/functions/") + fn).toLocal8Bit();
    if (::symlink(fnTarget.constData(), link.toLocal8Bit().constData()) != 0 && errno != EEXIST)
        emit log(QStringLiteral("usb: link accessory failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
    else
        emit log(QStringLiteral("usb: linked %1 so the car's GET_PROTOCOL is answered").arg(fn));
    setUdc(gadget, udc, "relink accessory");
}

static int ioctlStart(int fd)
{
    const long r = ::ioctl(fd, ACCESSORY_IS_START_REQUESTED);
    return r < 0 ? -1 : int(r);
}

static QString ioctlString(int fd, unsigned long req)
{
    char buf[256];
    ::memset(buf, 0, sizeof(buf));
    if (::ioctl(fd, req, buf) < 0)
        return QString();
    buf[sizeof(buf) - 1] = 0;
    return QString::fromLatin1(buf);
}

static bool isAndroidAuto(const QString &model)
{
    return model == QLatin1String("Android Open Automotive Protocol")
        || model == QLatin1String("Android Auto");
}

bool UsbGadgetWorker::waitForStart(QString *model, QString *serial)
{
    // Poll ACCESSORY_IS_START_REQUESTED for a 0->1 edge. GET_PROTOCOL clears
    // the flag and the host sends START right after, which is the edge we want.
    // A flag already latched when we open is stale (left over from unplug); we
    // prime on it and wait for the next exchange. Fast 2 ms sampling at first
    // (the edge is brief) then back off so a charger does not spin the CPU.
    int prev = 0;
    bool primed = false;
    bool staleAtPrime = false;
    QElapsedTimer sinceOpen;
    sinceOpen.start();
    QElapsedTimer total;
    total.start();
    const qint64 kMaxWaitMs = 90000;   // never loop forever; give up and report
    const qint64 kStaleGraceMs = 6000; // accept a latched start after this if Android Auto
    auto accept = [&](int f, const char *via) -> bool {
        const QString m = ioctlString(f, ACCESSORY_GET_STRING_MODEL);
        const QString s = ioctlString(f, ACCESSORY_GET_STRING_SERIAL);
        if (isAndroidAuto(m)) {
            *model = m;
            *serial = s;
            emit log(QStringLiteral("usb: AOA start (%1) model=\"%2\"").arg(QLatin1String(via), m));
            return true;
        }
        emit log(QStringLiteral("usb: AOA start for model \"%1\" is not Android Auto").arg(m));
        return false;
    };
    while (!m_stop.load()) {
        if (total.elapsed() > kMaxWaitMs) {
            emit log(QStringLiteral("usb: no Android Auto start within %1 s; giving up").arg(kMaxWaitMs / 1000));
            return false;
        }
        const int fd = ::open(kAccessoryDev, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            // Node not present yet (no cable / f_accessory not active). Wait.
            QThread::msleep(250);
            continue;
        }
        const int cur = ioctlStart(fd);
        if (cur < 0) {
            ::close(fd);
            QThread::msleep(250);
            continue;
        }
        if (!primed) {
            primed = true;
            prev = cur;
            staleAtPrime = (cur == 1);
            if (staleAtPrime)
                emit log(QStringLiteral("usb: AOA start already pending; confirming the current car"));
            ::close(fd);
        } else if (prev == 0 && cur == 1) {
            // Fresh 0->1 edge: the car just completed the AOA handshake.
            const bool ok = accept(fd, "edge");
            ::close(fd);
            prev = cur;
            if (ok)
                return true;
        } else if (staleAtPrime && cur == 1 && sinceOpen.elapsed() > kStaleGraceMs) {
            // The flag was latched when we started and no fresh edge has come (the
            // gadget was already an accessory, so the car did not re-handshake).
            // If the car wants Android Auto, honour the latched start rather than
            // wait forever.
            const bool ok = accept(fd, "latched");
            ::close(fd);
            if (ok)
                return true;
            staleAtPrime = false; // not Android Auto; fall back to edge detection
        } else {
            prev = cur;
            ::close(fd);
        }
        const bool fast = (cur == 1) || sinceOpen.elapsed() < 5000;
        QThread::msleep(fast ? 2 : 250);
    }
    return false;
}

// FunctionFS V2 descriptor + strings blobs (matches cargadgetd / ffs-test.c).
static QByteArray buildAccessoryDescriptors()
{
    auto iface = []() -> QByteArray {
        return QByteArray::fromRawData("\x09\x04\x00\x00\x02\xff\xff\x00\x01", 9);
    };
    auto ep = [](unsigned char addr, quint16 maxPkt) -> QByteArray {
        QByteArray b(7, '\0');
        b[0] = 7;              // bLength
        b[1] = 0x05;           // ENDPOINT
        b[2] = char(addr);     // bEndpointAddress
        b[3] = 0x02;           // bmAttributes = BULK
        b[4] = char(maxPkt & 0xff);
        b[5] = char((maxPkt >> 8) & 0xff);
        b[6] = 0;              // bInterval
        return b;
    };
    QByteArray fs = iface();
    fs.append(ep(0x81, 64));   // ep1 IN
    fs.append(ep(0x02, 64));   // ep2 OUT
    QByteArray hs = iface();
    hs.append(ep(0x81, 512));
    hs.append(ep(0x02, 512));

    auto le32 = [](quint32 v) -> QByteArray {
        QByteArray b(4, '\0');
        b[0] = char(v & 0xff);
        b[1] = char((v >> 8) & 0xff);
        b[2] = char((v >> 16) & 0xff);
        b[3] = char((v >> 24) & 0xff);
        return b;
    };
    QByteArray body;
    body.append(QByteArray(12, '\0')); // header filled below
    body.append(le32(3));              // fs descriptor count
    body.append(le32(3));              // hs descriptor count
    body.append(fs);
    body.append(hs);
    // header: magic (V2=3), length, flags (HAS_FS=1 | HAS_HS=2 = 3)
    QByteArray hdr = le32(3) + le32(quint32(body.size())) + le32(3);
    body.replace(0, 12, hdr);
    return body;
}

static QByteArray buildStrings()
{
    const QByteArray str = QByteArrayLiteral("Android Accessory Interface");
    auto le32 = [](quint32 v) -> QByteArray {
        QByteArray b(4, '\0');
        b[0] = char(v & 0xff);
        b[1] = char((v >> 8) & 0xff);
        b[2] = char((v >> 16) & 0xff);
        b[3] = char((v >> 24) & 0xff);
        return b;
    };
    QByteArray inner;
    inner.append(char(0x09)); // lang 0x0409 LE
    inner.append(char(0x04));
    inner.append(str);
    inner.append('\0');

    QByteArray body;
    body.append(le32(2));   // STRINGS_MAGIC
    body.append(le32(0));   // length (filled below)
    body.append(le32(1));   // str_count
    body.append(le32(1));   // lang_count
    body.append(inner);
    body.replace(4, 4, le32(quint32(body.size())));
    return body;
}

bool UsbGadgetWorker::writeFfsDescriptors(const QString &ffsDir, QString *err)
{
    const QString ep0 = ffsDir + QStringLiteral("/ep0");
    m_ep0 = ::open(ep0.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (m_ep0 < 0) {
        *err = QStringLiteral("open %1: %2").arg(ep0, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    const QByteArray desc = buildAccessoryDescriptors();
    if (::write(m_ep0, desc.constData(), desc.size()) != desc.size()) {
        *err = QStringLiteral("write descriptors: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(m_ep0);
        m_ep0 = -1;
        return false;
    }
    const QByteArray strs = buildStrings();
    if (::write(m_ep0, strs.constData(), strs.size()) != strs.size()) {
        *err = QStringLiteral("write strings: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(m_ep0);
        m_ep0 = -1;
        return false;
    }
    return true; // keep m_ep0 open for the bind lifetime
}

void UsbGadgetWorker::exposeEndpoints(const QString &ffsDir)
{
    // Open a path for the unprivileged app: o+rx on the dir chain, and
    // chown/chmod the ep nodes. ep1/ep2 appear slightly after the descriptors.
    for (QString p = ffsDir; p != QStringLiteral("/") && !p.isEmpty();) {
        ::chmod(p.toLocal8Bit().constData(), 0755);
        const int slash = p.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0)
            break;
        p = p.left(slash);
    }
    for (const char *name : { "ep0", "ep1", "ep2" }) {
        const QString p = ffsDir + QLatin1Char('/') + QLatin1String(name);
        ::chmod(p.toLocal8Bit().constData(), 0660);
        if (::chown(p.toLocal8Bit().constData(), uid_t(m_uid), gid_t(m_gid)) != 0)
            emit log(QStringLiteral("usb: chown %1: %2").arg(p, QString::fromLocal8Bit(strerror(errno))));
    }
}

bool UsbGadgetWorker::buildAccessoryGadget(const QString &gadget, const QString &config,
                                           const QString &udc, QString *ffsDir, QString *err)
{
    const QString cfgDir = gadget + QStringLiteral("/configs/") + config;

    // Snapshot identity for restore.
    m_savedVendor = readFileRaw(gadget + QStringLiteral("/idVendor"));
    m_savedProduct = readFileRaw(gadget + QStringLiteral("/idProduct"));

    // Drop the pullup immediately so the host sees us leave the bus inside the
    // AOA exchange, then re-shape the gadget. Each step is logged so a field
    // log shows exactly where a failed switch stopped.
    emit log(QStringLiteral("usb: switching gadget: unbinding from the car"));
    if (!setUdc(gadget, QString(), "leave bus for accessory switch")) {
        *err = QStringLiteral("unbind UDC stuck or failed");
        return false;
    }
    QThread::msleep(100);

    // Remove every function symlink from the config (we want ONLY our ffs iface;
    // a leftover accessory/rndis interface makes the car configure USB without
    // sending the version request on the FunctionFS pipe).
    QDir cfg(cfgDir);
    for (const QString &name : cfg.entryList(QDir::NoDotAndDotDot | QDir::System | QDir::Files | QDir::Dirs)) {
        const QString p = cfgDir + QLatin1Char('/') + name;
        if (QFileInfo(p).isSymLink())
            ::unlink(p.toLocal8Bit().constData());
    }

    // 18d1:2d00 — the "already an accessory" identity the head unit expects, so
    // DHU/cars skip the AOA switch and go straight to the version request.
    writeStr(gadget + QStringLiteral("/idVendor"), QStringLiteral("0x18d1"));
    writeStr(gadget + QStringLiteral("/idProduct"), QStringLiteral("0x2d00"));

    // FunctionFS function instance + mount.
    const QString dir = QString::fromLatin1(kDefaultFfsDir);
    if (!QDir().mkpath(gadget + QStringLiteral("/functions/ffs.aa"))) {
        *err = QStringLiteral("mkdir functions/ffs.aa failed");
        return false;
    }
    QDir().mkpath(dir);
    ::umount2(dir.toLocal8Bit().constData(), MNT_DETACH); // in case of a stale mount
    if (::mount("aa", dir.toLocal8Bit().constData(), "functionfs", 0, nullptr) != 0) {
        // Already mounted with an ep0? Tolerate that; otherwise fail.
        if (!QFileInfo::exists(dir + QStringLiteral("/ep0"))) {
            *err = QStringLiteral("mount functionfs: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
    }

    emit log(QStringLiteral("usb: switching gadget: 18d1:2d00, functionfs mounted at %1").arg(dir));

    if (!writeFfsDescriptors(dir, err))
        return false;
    emit log(QStringLiteral("usb: switching gadget: FunctionFS descriptors written"));

    // Link ffs.aa into the config. configfs resolves the symlink target against
    // the process CWD, so the target must be ABSOLUTE (a relative one fails with
    // ENOENT; chdir() is unsafe here because the D-Bus main thread shares the CWD).
    const QString link = cfgDir + QStringLiteral("/ffs.aa");
    ::unlink(link.toLocal8Bit().constData());
    const QByteArray ffsTarget = (gadget + QStringLiteral("/functions/ffs.aa")).toLocal8Bit();
    if (::symlink(ffsTarget.constData(), link.toLocal8Bit().constData()) != 0
        && errno != EEXIST) {
        *err = QStringLiteral("link ffs.aa into config: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    exposeEndpoints(dir);

    // Bind the UDC: clear then set, so the host sees a fresh enumeration.
    emit log(QStringLiteral("usb: switching gadget: binding to the car as an accessory"));
    if (!setUdc(gadget, QString(), "before accessory bind")) {
        *err = QStringLiteral("unbind UDC stuck or failed");
        return false;
    }
    QThread::msleep(100);
    if (!setUdc(gadget, udc, "accessory")) {
        *err = QStringLiteral("bind UDC %1 stuck or failed").arg(udc);
        return false;
    }

    // ep nodes can be (re)created by the bind; re-apply ownership.
    QThread::msleep(50);
    exposeEndpoints(dir);

    *ffsDir = dir;
    m_built = true;
    return true;
}

void UsbGadgetWorker::restore()
{
    if (m_gadget.isEmpty())
        return;
    emit log(QStringLiteral("usb: restoring gadget"));
    // Unbind and strip our FunctionFS additions. If the UDC is stuck in the
    // kernel, skip the unbind (it would only queue behind it) but still put
    // usb-moded back so normal USB returns once the kernel recovers.
    if (!m_udcStuck)
        setUdc(m_gadget, QString(), "restore");
    QThread::msleep(150);
    if (m_ep0 >= 0) {
        ::close(m_ep0); // releases FunctionFS so the mount can go
        m_ep0 = -1;
    }
    if (!m_config.isEmpty())
        ::unlink((m_gadget + QStringLiteral("/configs/") + m_config + QStringLiteral("/ffs.aa")).toLocal8Bit().constData());
    if (!m_ffsDir.isEmpty())
        ::umount2(m_ffsDir.toLocal8Bit().constData(), MNT_DETACH);
    ::rmdir((m_gadget + QStringLiteral("/functions/ffs.aa")).toLocal8Bit().constData());
    // Put identity back (belt and suspenders; usb-moded rewrites it on restart).
    if (!m_savedVendor.isEmpty())
        writeFile(m_gadget + QStringLiteral("/idVendor"), m_savedVendor);
    if (!m_savedProduct.isEmpty())
        writeFile(m_gadget + QStringLiteral("/idProduct"), m_savedProduct);
    // Hand the gadget back to usb-moded, which rebuilds its own charging/MTP mode.
    if (m_usbModedStopped) {
        runCmd(QStringLiteral("systemctl"), { QStringLiteral("restart"), QStringLiteral("usb-moded") });
        m_usbModedStopped = false;
        emit log(QStringLiteral("usb: restarted usb-moded"));
    }
    m_built = false;
}

// True when the gadget is currently our 18d1:2d00 FunctionFS accessory, usually
// a leftover from a session that was interrupted before restore() could run.
bool UsbGadgetWorker::gadgetIsAccessoryLeftover(const QString &gadget)
{
    if (readTrim(gadget + QStringLiteral("/idProduct")).contains(QLatin1String("2d00")))
        return true;
    return QFileInfo::exists(gadget + QStringLiteral("/functions/ffs.aa"));
}

// Strip the accessory interface and hand the gadget back to usb-moded so the car
// sees a normal device and performs a fresh AOA handshake (without this, a stale
// accessory gadget means the car never re-sends GET_PROTOCOL/START and we wait
// on a latched start flag forever).
void UsbGadgetWorker::resetLeftoverAccessory(const QString &gadget)
{
    emit log(QStringLiteral("usb: resetting a leftover accessory gadget from an interrupted session"));
    setUdc(gadget, QString(), "reset leftover");
    QThread::msleep(150);
    QDir cfgRoot(gadget + QStringLiteral("/configs"));
    for (const QString &c : cfgRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        ::unlink((gadget + QStringLiteral("/configs/") + c + QStringLiteral("/ffs.aa")).toLocal8Bit().constData());
    ::umount2(QString::fromLatin1(kDefaultFfsDir).toLocal8Bit().constData(), MNT_DETACH);
    ::rmdir((gadget + QStringLiteral("/functions/ffs.aa")).toLocal8Bit().constData());
    // usb-moded rebuilds its own charging/MTP gadget, which re-enumerates us to
    // the car as a normal Android device.
    runCmd(QStringLiteral("systemctl"), { QStringLiteral("restart"), QStringLiteral("usb-moded") });
    QThread::msleep(1500);
}

void UsbGadgetWorker::run()
{
    m_gadget = findGadget();
    if (m_gadget.isEmpty()) {
        emit failed(QStringLiteral("no configfs USB gadget (/sys/kernel/config/usb_gadget)"));
        return;
    }
    m_config = firstConfig(m_gadget);
    m_udc = firstUdc();
    if (m_config.isEmpty() || m_udc.isEmpty()) {
        emit failed(QStringLiteral("gadget has no config or no UDC"));
        return;
    }
    emit log(QStringLiteral("usb: gadget=%1 config=%2 udc=%3")
                 .arg(QFileInfo(m_gadget).fileName(), m_config, m_udc));

    // Recover from a leftover accessory gadget first, so the car re-handshakes.
    if (gadgetIsAccessoryLeftover(m_gadget)) {
        resetLeftoverAccessory(m_gadget);
        m_config = firstConfig(m_gadget); // usb-moded may rebuild with a new config
        m_udc = firstUdc();
    }

    // usb-moded would rewrite the gadget back to charging/MTP; stop it for the
    // whole session and hand the gadget back on teardown.
    runCmd(QStringLiteral("modprobe"), { QStringLiteral("libcomposite") });
    if (serviceActive(QStringLiteral("usb-moded"))) {
        runCmd(QStringLiteral("systemctl"), { QStringLiteral("stop"), QStringLiteral("usb-moded") });
        m_usbModedStopped = true;
        emit log(QStringLiteral("usb: stopped usb-moded"));
        QThread::msleep(300);
    }
    // Re-read the UDC after stopping usb-moded (it may have unbound the gadget).
    linkAskAccessory(m_gadget, m_config, m_udc);
    if (m_udcStuck) {
        emit failed(QStringLiteral("the phone's USB controller is stuck: unplug the cable and plug it back in"));
        restore();
        emit stopped();
        return;
    }

    emit log(QStringLiteral("usb: waiting for the car to start Android Auto (AOA)"));
    QString model, serial;
    if (!waitForStart(&model, &serial)) {
        if (!m_stop.load())
            emit failed(QStringLiteral("the car did not start Android Auto — try replugging the USB cable"));
        restore();
        emit stopped();
        return;
    }
    emit startRequested(model, serial);
    emit log(QStringLiteral("usb: AOA start model=\"%1\" serial=\"%2\"").arg(model, serial));

    QString ffsDir, err;
    if (!buildAccessoryGadget(m_gadget, m_config, m_udc, &ffsDir, &err)) {
        emit log(QStringLiteral("usb: accessory switch failed: %1").arg(err));
        emit failed(QStringLiteral("usb: build accessory gadget: %1").arg(err));
        restore();
        emit stopped();
        return;
    }
    m_ffsDir = ffsDir;
    emit log(QStringLiteral("usb: bound 18d1:2d00 FunctionFS gadget on %1").arg(m_udc));
    emit ready(ffsDir);

    // Hold the gadget alive (ep0 open) until stop is requested.
    while (!m_stop.load())
        QThread::msleep(200);

    restore();
    emit stopped();
}

// ---------------------------------------------------------------------------
// UsbGadget
// ---------------------------------------------------------------------------
UsbGadget::UsbGadget(QObject *parent)
    : QObject(parent)
{
    // Everything also goes to the journal (journalctl -u carfish-helper): the
    // D-Bus Log signal only reaches the app while it is listening, so lines
    // from a stop/teardown or a stuck attempt were lost from ~/carfish.log.
    connect(this, &UsbGadget::log, this, [](const QString &l) { qInfo().noquote() << l; });
    connect(this, &UsbGadget::failed, this, [](const QString &r) { qWarning().noquote() << "usb: failed:" << r; });
    // On helper startup, recover daily USB if a previous session was interrupted
    // (e.g. the helper was restarted by a package upgrade) and left the gadget as
    // our 18d1:2d00 accessory. Done after the event loop starts so it never
    // blocks D-Bus registration.
    QTimer::singleShot(0, this, [this] {
        const QString root = QStringLiteral("/sys/kernel/config/usb_gadget");
        const QStringList gs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (gs.isEmpty())
            return;
        const QString g = gs.contains(QStringLiteral("g1")) ? root + QStringLiteral("/g1")
                                                            : root + QLatin1Char('/') + gs.first();
        const bool is2d00 = readTrim(g + QStringLiteral("/idProduct")).contains(QLatin1String("2d00"));
        const bool hasFfs = QFileInfo::exists(g + QStringLiteral("/functions/ffs.aa"));
        if (!is2d00 && !hasFfs) {
            // A previous helper may have died mid-switch after stopping
            // usb-moded but before building anything: the phone then has no
            // normal USB (charging/MTP mode) until usb-moded runs again.
            const QString unit = QStringLiteral("usb-moded");
            if (!serviceActive(unit) && serviceEnabled(unit)) {
                emit log(QStringLiteral("usb: usb-moded was left stopped by an interrupted session; restarting it"));
                runCmd(QStringLiteral("systemctl"), { QStringLiteral("restart"), unit });
            }
            return;
        }
        emit log(QStringLiteral("usb: recovering normal USB from a leftover accessory gadget at startup"));
        qint64 took = 0;
        writeUdc(g, QString(), 5000, &took);
        QThread::msleep(150);
        for (const QString &c : QDir(g + QStringLiteral("/configs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            ::unlink((g + QStringLiteral("/configs/") + c + QStringLiteral("/ffs.aa")).toLocal8Bit().constData());
        ::umount2(QStringLiteral("/dev/usb-ffs/aa").toLocal8Bit().constData(), MNT_DETACH);
        ::rmdir((g + QStringLiteral("/functions/ffs.aa")).toLocal8Bit().constData());
        runCmd(QStringLiteral("systemctl"), { QStringLiteral("restart"), QStringLiteral("usb-moded") });
    });
}

UsbGadget::~UsbGadget()
{
    stop();
}

void UsbGadget::start()
{
    if (m_worker && m_worker->isRunning())
        return;
    // An earlier attempt is still blocked in the kernel: a new one would block
    // behind it, so say so instead of failing silently.
    bool oldStuck = g_udcStuck.load();
    for (const QPointer<UsbGadgetWorker> &w : m_abandoned)
        if (w && w->isRunning())
            oldStuck = true;
    if (oldStuck) {
        emit log(QStringLiteral("usb: a previous USB switch is still stuck in the kernel; not starting"));
        emit failed(QStringLiteral("the phone's USB is stuck from the previous attempt: unplug the cable "
                                   "and plug it back in (reboot the phone if that does not help)"));
        return;
    }
    if (m_worker) {
        m_worker->deleteLater();
        m_worker = nullptr;
    }
    m_worker = new UsbGadgetWorker(this);
    connect(m_worker, &UsbGadgetWorker::log, this, &UsbGadget::log);
    connect(m_worker, &UsbGadgetWorker::startRequested, this, &UsbGadget::startRequested);
    connect(m_worker, &UsbGadgetWorker::ready, this, &UsbGadget::ready);
    connect(m_worker, &UsbGadgetWorker::failed, this, &UsbGadget::failed);
    m_worker->start();
}

void UsbGadget::stop()
{
    if (!m_worker)
        return;
    m_worker->requestStop();
    if (!m_worker->wait(8000)) {
        // Deleting a running QThread aborts the whole helper (and a crash left
        // the gadget half-switched with usb-moded stopped). Let it finish on
        // its own instead: it restores the gadget once its blocked call
        // returns. Its late ready/failed must not be mistaken for a new
        // attempt's, so only its log stays connected.
        UsbGadgetWorker *w = m_worker;
        m_worker = nullptr;
        emit log(QStringLiteral("usb: the USB worker did not stop within 8 s (blocked in the kernel); "
                                "leaving it to finish and clean up on its own"));
        disconnect(w, nullptr, this, nullptr);
        connect(w, &UsbGadgetWorker::log, this, [this](const QString &l) {
            emit log(QStringLiteral("(previous attempt) ") + l);
        });
        connect(w, &QThread::finished, w, &QObject::deleteLater);
        w->setParent(nullptr);
        m_abandoned << w;
        return;
    }
    m_worker->deleteLater();
    m_worker = nullptr;
}

// ---------------------------------------------------------------------------
// UsbAdaptor (D-Bus)
// ---------------------------------------------------------------------------
UsbAdaptor::UsbAdaptor(UsbGadget *usb)
    : QDBusAbstractAdaptor(usb)
    , m_usb(usb)
{
    connect(m_usb, &UsbGadget::ready, this, &UsbAdaptor::Ready);
    connect(m_usb, &UsbGadget::startRequested, this, &UsbAdaptor::StartRequested);
    connect(m_usb, &UsbGadget::failed, this, &UsbAdaptor::Failed);
    connect(m_usb, &UsbGadget::log, this, &UsbAdaptor::Log);
}

void UsbAdaptor::Start()
{
    m_usb->start();
}

void UsbAdaptor::Stop()
{
    m_usb->stop();
}
