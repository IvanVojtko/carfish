#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QVector>
#include <functional>

namespace aap {

// Frame header byte 1 (flags)
enum FrameFlag : quint8 {
    FlagFirst     = 0x01, // first fragment of a message
    FlagLast      = 0x02, // last fragment of a message
    FlagControl   = 0x04, // "control" message type (channel open etc. on a service channel)
    FlagEncrypted = 0x08, // payload is a TLS record stream
};

constexpr int kHeaderSize = 2;          // channel, flags
// The official phone never sends a whole frame (header + payload) over 16128
// bytes, and Hyundai/Kia (Mobis) head units size their receive buffer to it.
constexpr int kMaxFrameSize = 16128;
// A TLS 1.2 AES-GCM record wraps each encrypted chunk in 5 header + 8
// explicit nonce + 16 tag bytes. Taken off the plaintext chunk so an encrypted
// first fragment still fits kMaxFrameSize (it came out at 16157 before).
constexpr int kTlsOverhead = 5 + 8 + 16;
constexpr int kMaxFramePayload = kMaxFrameSize - 8 - kTlsOverhead;
// USB high-speed bulk max packet size. A transfer whose length is an exact
// multiple of it has no short packet to end it, so the head unit's read runs on
// into the next frame; frames are sized to avoid that (see encodeFrames).
constexpr int kUsbMaxPacket = 512;

struct Message {
    quint8 channel = 0;
    bool control = false;
    bool encrypted = false;
    QByteArray payload; // 2-byte BE message id followed by body (plaintext)

    quint16 id() const;
    QByteArray body() const { return payload.mid(2); }
};

QByteArray makePayload(quint16 msgId, const QByteArray &body);

// Splits outgoing messages into frames and reassembles incoming frames.
// Encryption is applied per fragment through the callbacks, exactly like
// the phone does it: each fragment payload is encrypted separately and the
// "total size" field of a multi-fragment message carries the plaintext size.
class Framer
{
public:
    using CryptFn = std::function<bool(const QByteArray &in, QByteArray &out)>;

    void setEncrypt(CryptFn fn) { m_encrypt = std::move(fn); }
    void setDecrypt(CryptFn fn) { m_decrypt = std::move(fn); }

    // One entry per frame. Over USB each frame must go out as its own bulk
    // transfer: Mobis head units parse exactly one frame per transfer and
    // reset USB when a frame is split across transfers or two share one.
    QVector<QByteArray> encodeFrames(const Message &msg) const;
    QByteArray encode(const Message &msg) const; // all frames joined

    // Feed raw bytes from the transport. Calls onMessage for every complete
    // message. Returns false on a fatal protocol error.
    bool feed(const QByteArray &data, const std::function<void(const Message &)> &onMessage);

private:
    struct Partial {
        bool control = false;
        bool encrypted = false;
        QByteArray data;
    };

    CryptFn m_encrypt;
    CryptFn m_decrypt;
    QByteArray m_rx;
    QHash<quint8, Partial> m_partial;
};

} // namespace aap
