#include "usbaccessorytransport.h"

#include <QLoggingCategory>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcUsb, "aap.usb")

// f_accessory requires reads of at least the bulk max packet size; Android's
// UsbConnectionHelper uses 16 KiB buffers as well.
static constexpr int kReadBuffer = 16384;

UsbAccessoryTransport::UsbAccessoryTransport(const QString &device, QObject *parent)
    : Transport(parent)
    , m_device(device)
{
}

UsbAccessoryTransport::~UsbAccessoryTransport()
{
    stop();
}

void UsbAccessoryTransport::start()
{
    m_fd = ::open(m_device.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (m_fd < 0) {
        emit disconnected(QStringLiteral("cannot open %1: %2").arg(m_device, QString::fromLocal8Bit(strerror(errno))));
        return;
    }
    m_reader = new UsbAccessoryReader(m_fd);
    connect(m_reader, &UsbAccessoryReader::chunk, this, &Transport::dataReceived, Qt::QueuedConnection);
    connect(m_reader, &UsbAccessoryReader::failed, this, &Transport::disconnected, Qt::QueuedConnection);
    m_reader->start();
    emit connected();
}

void UsbAccessoryReader::run()
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
            continue; // zero length packet
        emit chunk(QByteArray(buf.constData(), int(n)));
    }
}

void UsbAccessoryTransport::stop()
{
    if (m_reader)
        m_reader->running = false;
    if (m_fd >= 0) {
        ::close(m_fd); // unblocks read() with an error
        m_fd = -1;
    }
    if (m_reader) {
        m_reader->wait(1000);
        delete m_reader;
        m_reader = nullptr;
    }
}

void UsbAccessoryTransport::write(const QByteArray &data)
{
    // Writes are small compared to USB bandwidth; a blocking write from the
    // main thread keeps ordering trivial. Move to a writer thread if video
    // frames start stalling the UI.
    const char *p = data.constData();
    qint64 left = data.size();
    while (left > 0 && m_fd >= 0) {
        const ssize_t n = ::write(m_fd, p, size_t(left));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            qCWarning(lcUsb) << "write failed:" << strerror(errno);
            emit disconnected(QString::fromLocal8Bit(strerror(errno)));
            return;
        }
        p += n;
        left -= n;
    }
}

QString UsbAccessoryTransport::describe() const
{
    return QStringLiteral("USB accessory %1").arg(m_device);
}
