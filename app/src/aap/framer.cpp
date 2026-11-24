#include "framer.h"

#include <QtEndian>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcFramer, "aap.framer")

namespace aap {

quint16 Message::id() const
{
    if (payload.size() < 2)
        return 0xffff;
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(payload.constData()));
}

QByteArray makePayload(quint16 msgId, const QByteArray &body)
{
    QByteArray out(2, Qt::Uninitialized);
    qToBigEndian<quint16>(msgId, reinterpret_cast<uchar *>(out.data()));
    out.append(body);
    return out;
}

static void putU16(QByteArray &b, quint16 v)
{
    char tmp[2];
    qToBigEndian<quint16>(v, reinterpret_cast<uchar *>(tmp));
    b.append(tmp, 2);
}

static void putU32(QByteArray &b, quint32 v)
{
    char tmp[4];
    qToBigEndian<quint32>(v, reinterpret_cast<uchar *>(tmp));
    b.append(tmp, 4);
}

QVector<QByteArray> Framer::encodeFrames(const Message &msg) const
{
    QVector<QByteArray> frames;
    const int total = msg.payload.size();
    int offset = 0;
    do {
        int chunk = qMin(kMaxFramePayload, total - offset);
        const bool first = offset == 0;
        // Keep the frame length off a multiple of the USB max packet size, so
        // the transfer ends with a short packet. Taking one byte off a chunk
        // moves it to the next fragment (a single-frame message becomes two).
        // The encrypted size assumes AES-GCM (what head units negotiate); the
        // USB writer adds a zero-length packet if a frame still lands on one.
        auto frameSize = [&](int c) {
            const bool multiFirst = first && offset + c < total;
            return 4 + (multiFirst ? 4 : 0) + c + (msg.encrypted ? kTlsOverhead : 0);
        };
        if (chunk > 1 && frameSize(chunk) % kUsbMaxPacket == 0)
            --chunk;
        const bool last = offset + chunk == total;

        QByteArray data = msg.payload.mid(offset, chunk);
        if (msg.encrypted) {
            QByteArray enc;
            if (!m_encrypt || !m_encrypt(data, enc)) {
                qCWarning(lcFramer) << "encryption failed on channel" << msg.channel;
                return {};
            }
            data = enc;
        }

        quint8 flags = 0;
        if (first)
            flags |= FlagFirst;
        if (last)
            flags |= FlagLast;
        if (msg.control)
            flags |= FlagControl;
        if (msg.encrypted)
            flags |= FlagEncrypted;

        QByteArray out;
        out.reserve(8 + data.size());
        out.append(char(msg.channel));
        out.append(char(flags));
        putU16(out, quint16(data.size()));
        if (first && !last)
            putU32(out, quint32(total));
        out.append(data);
        frames.append(out);
        offset += chunk;
    } while (offset < total);
    return frames;
}

QByteArray Framer::encode(const Message &msg) const
{
    QByteArray out;
    for (const QByteArray &f : encodeFrames(msg))
        out.append(f);
    return out;
}

bool Framer::feed(const QByteArray &data, const std::function<void(const Message &)> &onMessage)
{
    m_rx.append(data);

    for (;;) {
        if (m_rx.size() < kHeaderSize + 2)
            return true;
        const auto *p = reinterpret_cast<const uchar *>(m_rx.constData());
        const quint8 channel = p[0];
        const quint8 flags = p[1];
        const quint16 frameLen = qFromBigEndian<quint16>(p + 2);
        const bool first = flags & FlagFirst;
        const bool last = flags & FlagLast;
        const int headerLen = kHeaderSize + 2 + ((first && !last) ? 4 : 0);
        if (m_rx.size() < headerLen + frameLen)
            return true;

        QByteArray frame = m_rx.mid(headerLen, frameLen);
        m_rx.remove(0, headerLen + frameLen);

        if (flags & FlagEncrypted) {
            QByteArray plain;
            if (!m_decrypt || !m_decrypt(frame, plain)) {
                qCWarning(lcFramer) << "decryption failed on channel" << channel;
                return false;
            }
            frame = plain;
        }

        if (first && last) {
            Message m;
            m.channel = channel;
            m.control = flags & FlagControl;
            m.encrypted = flags & FlagEncrypted;
            m.payload = frame;
            onMessage(m);
            continue;
        }

        Partial &part = m_partial[channel];
        if (first) {
            part.control = flags & FlagControl;
            part.encrypted = flags & FlagEncrypted;
            part.data = frame;
        } else {
            part.data.append(frame);
        }
        if (last) {
            Message m;
            m.channel = channel;
            m.control = part.control;
            m.encrypted = part.encrypted;
            m.payload = part.data;
            m_partial.remove(channel);
            onMessage(m);
        }
    }
}

} // namespace aap
