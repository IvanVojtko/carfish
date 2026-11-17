#include "micreceiver.h"

#include <QDataStream>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcMic, "aap.mic")

MicReceiver::MicReceiver(QObject *parent)
    : QObject(parent)
{
}

MicReceiver::~MicReceiver()
{
    end();
}

bool MicReceiver::begin(int sampleRate, int channels, const QString &wavPath, QString *error)
{
    end();
    m_sampleRate = sampleRate > 0 ? sampleRate : 16000;
    m_channels = channels > 0 ? channels : 1;
    m_dataBytes = 0;

    if (!wavPath.isEmpty()) {
        m_file.setFileName(wavPath);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("cannot open %1: %2").arg(wavPath, m_file.errorString());
            return false;
        }
        writeWavHeader(0); // placeholder sizes, patched in end()
    }
    m_active = true;
    qCInfo(lcMic) << "microphone capture started" << m_sampleRate << "Hz x" << m_channels;
    return true;
}

void MicReceiver::feed(const QByteArray &pcm)
{
    if (!m_active || pcm.isEmpty())
        return;
    if (m_file.isOpen()) {
        m_file.write(pcm);
        m_dataBytes += quint32(pcm.size());
    }
    emit this->pcm(pcm);
}

void MicReceiver::end()
{
    if (!m_active)
        return;
    m_active = false;
    if (m_file.isOpen()) {
        m_file.seek(0);
        writeWavHeader(m_dataBytes); // patch RIFF/data sizes now they're known
        m_file.close();
    }
}

// Minimal canonical 44-byte PCM WAV header.
void MicReceiver::writeWavHeader(quint32 dataBytes)
{
    const quint32 byteRate = quint32(m_sampleRate) * quint32(m_channels) * 2;
    const quint16 blockAlign = quint16(m_channels * 2);
    QDataStream s(&m_file);
    s.setByteOrder(QDataStream::LittleEndian);
    m_file.write("RIFF", 4);
    s << quint32(36 + dataBytes);
    m_file.write("WAVE", 4);
    m_file.write("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(m_channels)
      << quint32(m_sampleRate) << byteRate << blockAlign << quint16(16);
    m_file.write("data", 4);
    s << quint32(dataBytes);
}
