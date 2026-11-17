#pragma once

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>

// Receives microphone PCM streamed from the head unit (the car's mic, S16LE,
// typically 16 kHz mono) and makes it available on the phone.
//
// The protocol delivery (open/receive/ack) lives in aap::Session; this class is
// the consumer seam. For now it records the stream to a WAV file so the path can
// be verified end to end, and re-emits each chunk via pcm() for a future voice
// assistant / telephony uplink to attach to. Routing to a live PulseAudio source
// will hook in here without touching the protocol code.
class MicReceiver : public QObject
{
    Q_OBJECT
public:
    explicit MicReceiver(QObject *parent = nullptr);
    ~MicReceiver() override;

    // Begin a capture. wavPath may be empty to skip recording (signal only).
    bool begin(int sampleRate, int channels, const QString &wavPath, QString *error);
    void feed(const QByteArray &pcm);
    void end();
    bool active() const { return m_active; }

signals:
    void pcm(const QByteArray &data);

private:
    void writeWavHeader(quint32 dataBytes);

    QFile m_file;
    bool m_active = false;
    int m_sampleRate = 16000;
    int m_channels = 1;
    quint32 m_dataBytes = 0;
};
