#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <string>

class BluezProfile;
class WifiJoiner;

// Phone side of the Wireless Projection Protocol:
//   1. BlueZ connects RFCOMM to the head unit's AA UUID (phone = client)
//   2. HU -> WifiVersionRequest, phone -> WifiVersionResponse
//   3. HU -> WifiStartRequest{ip, port}, phone -> WifiInfoRequest + WifiStartResponse
//   4. HU -> WifiInfoResponse{ssid, key, bssid}, phone joins the AP
//   5. phone -> WifiConnectStatus, then opens TCP to ip:port (AAP starts)
class WirelessSetup : public QObject
{
    Q_OBJECT
public:
    explicit WirelessSetup(QObject *parent = nullptr);

    bool start(QString *error);
    void stop();
    // Optional: kick the AA profile on a paired head unit, e.g. from the UI.
    void connectHeadUnit(const QString &devicePath);
    // Re-arm the whole bring-up after a session dropped (called by the Controller
    // when projection ends) so a lost car reconnects without a manual Stop/Start.
    void retrySoon();

signals:
    void status(const QString &text);
    // Verbose protocol trace routed to the app's durable on-device log (device
    // builds suppress qCInfo), so a failed real-car handshake can be diagnosed.
    void log(const QString &line);
    void readyForProjection(const QString &host, quint16 port);
    void failed(const QString &reason);

private:
    void onRfcommData(const QByteArray &data);
    void handleMessage(quint16 id, const QByteArray &body);
    void send(quint16 id, const std::string &body);   // enqueue, paced
    void writeFrame(quint16 id, const std::string &body);
    void drainSendQueue();
    void onWifiJoined(const QString &localAddress);
    void maybeReady();         // emit readyForProjection once joined + endpoint known
    void scheduleReconnect(const QString &why, int minDelayMs = 0); // auto-retry the bring-up on failure
    void doReconnect();        // re-initiate the RFCOMM connect after a backoff

    BluezProfile *m_profile;
    WifiJoiner *m_wifi;
    QByteArray m_rx;
    QString m_host;
    quint16 m_port = 0;
    int m_huMajor = 0;
    int m_huMinor = 0;
    bool m_gotMessage = false;
    bool m_haveEndpoint = false;  // WifiStartRequest received
    bool m_joined = false;        // on the head unit's WiFi
    bool m_infoReceived = false;  // WifiInfoResponse received
    bool m_infoRequested = false; // WifiInfoRequest already sent
    bool m_infoRetried = false;
    bool m_ready = false;         // readyForProjection already emitted
    QString m_localAddr;
    QTimer m_infoRetry;           // resend WifiInfoRequest once if credentials are slow
    QTimer m_negotiateTimeout;    // give up if the head unit goes silent mid-bootstrap
    QTimer m_reconnect;           // backoff before re-initiating a failed bring-up
    bool m_reconnectPending = false; // a reconnect is already queued (dedupe)
    bool m_stopped = false;       // user pressed Stop; do not auto-retry
    int m_retryCount = 0;         // consecutive bring-up failures (for backoff)

    // Outgoing frames are paced at least kFrameGapMs apart: some head units
    // (openauto-based) read only one WPP frame per socket read, so two frames
    // in one packet leave the second stuck. Matches F-Auto's officialPhoneOrder.
    struct Outgoing { quint16 id; std::string body; };
    QVector<Outgoing> m_sendQueue;
    QTimer m_sendTimer;
    qint64 m_lastSendMs = 0;

    // Phone-originated WifiPingRequest keepalives. Google-receiver head units
    // drop (or stop talking on) the RFCOMM link if ~10 s pass with no phone ping
    // after the version exchange; a real Android Auto phone pings every 1 s. We
    // start this once we've answered the HU's WifiVersionRequest.
    QTimer m_keepalive;
};
