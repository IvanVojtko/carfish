#include "wirelesssetup.h"

#include "bluezprofile.h"
#include "wifijoiner.h"
#include "wireless.pb.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QtEndian>

Q_LOGGING_CATEGORY(lcWpp, "aap.wpp")

// The phone drops RFCOMM messages larger than this.
static constexpr int kMaxMessage = 1024;

// WPP version the phone reports in WifiVersionResponse. A genuine phone reports
// 6.1 with status 0 and nothing else; strict head units (Toyota/openauto) accept
// exactly this (F-Auto officialPhoneOrder, matching logcat "phone using version 6.1").
static constexpr int kPhoneWppMajor = 6;
static constexpr int kPhoneWppMinor = 1;

// Minimum spacing between outgoing WPP frames; some head units read one frame
// per socket read, so back-to-back frames lose all but the first.
static constexpr qint64 kFrameGapMs = 120;

// Identity sent in WifiVersionResponse. Strict OEM head units (Toyota) withhold
// the WifiStartRequest until the response carries a device serial + instance id;
// Google's receiver library also drops every phone message except pings/version
// responses until it has the instance id ("mobile device ID"). It also lets the
// head unit recognise the same phone across USB and wireless.
static const QString kDeviceName = QStringLiteral("Jolla Phone (2026)");
static const QString kDeviceSerial = QStringLiteral("carfish");
static const QString kInstanceId = QStringLiteral("5a11f00d-0000-4a7b-8c9d-0e1f2a3b4c5d");

static QString wppName(quint16 id)
{
    switch (id) {
    case wpp::WIFI_START_REQUEST: return QStringLiteral("WifiStartRequest");
    case wpp::WIFI_INFO_REQUEST: return QStringLiteral("WifiInfoRequest");
    case wpp::WIFI_INFO_RESPONSE: return QStringLiteral("WifiInfoResponse");
    case wpp::WIFI_VERSION_REQUEST: return QStringLiteral("WifiVersionRequest");
    case wpp::WIFI_VERSION_RESPONSE: return QStringLiteral("WifiVersionResponse");
    case wpp::WIFI_CONNECT_STATUS: return QStringLiteral("WifiConnectStatus");
    case wpp::WIFI_START_RESPONSE: return QStringLiteral("WifiStartResponse");
    case wpp::WIFI_PING_REQUEST: return QStringLiteral("WifiPingRequest");
    case wpp::WIFI_PING_RESPONSE: return QStringLiteral("WifiPingResponse");
    case wpp::WIFI_CONNECTION_REJECTION: return QStringLiteral("WifiConnectionRejection");
    case wpp::WIFI_SETUP_INFO: return QStringLiteral("WifiSetupInfo");
    default: return QStringLiteral("id %1").arg(id);
    }
}

WirelessSetup::WirelessSetup(QObject *parent)
    : QObject(parent)
    , m_profile(new BluezProfile(this))
    , m_wifi(new WifiJoiner(this))
{
    connect(m_profile, &BluezProfile::connectionReady, this, [this](const QString &dev) {
        m_rx.clear();
        m_gotMessage = false;
        m_haveEndpoint = false;
        m_joined = false;
        m_infoReceived = false;
        m_infoRequested = false;
        m_infoRetried = false;
        m_ready = false;
        m_host.clear();
        m_port = 0;
        m_localAddr.clear();
        m_sendQueue.clear();
        m_lastSendMs = 0;
        emit status(tr("Bluetooth connected to %1, waiting for head unit").arg(dev));
        emit log(tr("RFCOMM up to %1 (behaving as a genuine Android Auto phone)").arg(dev));
        m_negotiateTimeout.start();
    });
    connect(m_profile, &BluezProfile::dataReceived, this, &WirelessSetup::onRfcommData);
    connect(m_profile, &BluezProfile::connectionClosed, this, [this] {
        m_infoRetry.stop();
        m_negotiateTimeout.stop();
        m_keepalive.stop();
        m_sendTimer.stop();
        emit log(tr("RFCOMM link closed%1").arg(m_gotMessage ? QString() : tr(" (head unit sent nothing)")));
        emit status(tr("Bluetooth link closed"));
        if (!m_ready)
            scheduleReconnect(tr("Bluetooth link closed before projection"));
    });
    connect(m_profile, &BluezProfile::error, this, [this](const QString &e) {
        emit failed(e);
        scheduleReconnect(e);
    });
    connect(m_profile, &BluezProfile::log, this, &WirelessSetup::log);
    connect(m_wifi, &WifiJoiner::joined, this, &WirelessSetup::onWifiJoined);
    connect(m_wifi, &WifiJoiner::log, this, &WirelessSetup::log);
    connect(m_wifi, &WifiJoiner::failed, this, [this](const QString &reason) {
        wpp::WifiConnectStatus st;
        st.set_status(wpp::STATUS_WIFI_NETWORK_UNAVAILABLE);
        st.set_error_message(reason.toStdString());
        send(wpp::WIFI_CONNECT_STATUS, st.SerializeAsString());
        emit failed(reason);
        scheduleReconnect(reason);
    });

    // Pace outgoing frames at least kFrameGapMs apart.
    m_sendTimer.setSingleShot(true);
    connect(&m_sendTimer, &QTimer::timeout, this, &WirelessSetup::drainSendQueue);

    // Keepalive: once the version exchange is done, ping the head unit every 1 s.
    // Receiver-library head units go silent / close RFCOMM without this (a real
    // phone pings at 1 Hz); a strict OEM unit simply answers or ignores them.
    m_keepalive.setInterval(1000);
    connect(&m_keepalive, &QTimer::timeout, this, [this] {
        wpp::WifiPingRequest ping;
        ping.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
        send(wpp::WIFI_PING_REQUEST, ping.SerializeAsString());
    });

    // Resend WifiInfoRequest once if the credentials are slow to arrive.
    m_infoRetry.setSingleShot(true);
    m_infoRetry.setInterval(5000);
    connect(&m_infoRetry, &QTimer::timeout, this, [this] {
        if (m_infoReceived || m_infoRetried)
            return;
        m_infoRetried = true;
        emit log(tr("no WiFi credentials yet, asking again"));
        send(wpp::WIFI_INFO_REQUEST, wpp::WifiInfoRequest().SerializeAsString());
    });

    // Fail cleanly if the head unit falls silent before the bootstrap completes.
    m_negotiateTimeout.setSingleShot(true);
    m_negotiateTimeout.setInterval(15000);
    connect(&m_negotiateTimeout, &QTimer::timeout, this, [this] {
        if (!m_ready) {
            emit failed(tr("head unit went silent during WiFi bootstrap"));
            scheduleReconnect(tr("head unit went silent"));
        }
    });

    // Auto-retry the whole bring-up after a failure, so a flaky RFCOMM/WiFi/AAP
    // bootstrap recovers on its own instead of needing a manual Stop/Start.
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &WirelessSetup::doReconnect);
}

bool WirelessSetup::start(QString *error)
{
    if (!m_profile->registerProfile(error))
        return false;
    m_stopped = false;
    m_retryCount = 0;
    m_reconnectPending = false;
    // We are the RFCOMM client (like the real Gearhead app): once the profile
    // is registered, reach out to the head unit instead of waiting for it.
    QTimer::singleShot(500, this, [this] {
        emit log(tr("looking for a paired Android Auto head unit to connect to"));
        m_profile->connectKnownHeadUnits();
    });
    return true;
}

void WirelessSetup::stop()
{
    m_stopped = true;
    m_reconnect.stop();
    m_reconnectPending = false;
    m_infoRetry.stop();
    m_negotiateTimeout.stop();
    m_keepalive.stop();
    m_sendTimer.stop();
    m_sendQueue.clear();
    m_wifi->cancel();
    m_profile->unregisterProfile();
}

// Re-arm the bring-up after a session dropped: the Controller calls this when
// projection ends so the car reconnects on its own (no manual Stop/Start).
void WirelessSetup::retrySoon()
{
    if (m_stopped)
        return;
    m_ready = false; // allow scheduleReconnect to run again after a live session
    // The head unit resets its own Bluetooth/AP when it ends a session, so an
    // immediate page often hits br-connection-page-timeout. Give it a few seconds.
    scheduleReconnect(tr("projection session ended"), 5000);
}

// Queue a fresh bring-up attempt after a backoff. De-duplicated so several
// near-simultaneous failures (timeout + link close) schedule only one retry.
void WirelessSetup::scheduleReconnect(const QString &why, int minDelayMs)
{
    if (m_stopped || m_reconnectPending || m_ready)
        return;
    m_reconnectPending = true;
    m_infoRetry.stop();
    m_negotiateTimeout.stop();
    m_keepalive.stop();
    m_sendTimer.stop();
    m_sendQueue.clear();
    m_wifi->cancel();
    // Drop the current RFCOMM link so the next attempt starts from a clean SDP
    // connect. The resulting connectionClosed is ignored (a reconnect is pending).
    m_profile->closeConnection();
    const int delay = qMax(minDelayMs, qMin(2000 + m_retryCount * 1500, 8000));
    ++m_retryCount;
    emit log(tr("wireless bring-up failed (%1); retrying in %2 s").arg(why).arg(delay / 1000));
    emit status(tr("Reconnecting to head unit (attempt %1)").arg(m_retryCount + 1));
    m_reconnect.start(delay);
}

void WirelessSetup::doReconnect()
{
    m_reconnectPending = false;
    if (m_stopped)
        return;
    emit log(tr("reconnecting to the head unit"));
    m_profile->connectKnownHeadUnits();
}

void WirelessSetup::connectHeadUnit(const QString &devicePath)
{
    m_profile->connectDevice(devicePath);
}

void WirelessSetup::send(quint16 id, const std::string &body)
{
    m_sendQueue.append({id, body});
    drainSendQueue();
}

void WirelessSetup::drainSendQueue()
{
    if (m_sendQueue.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 since = now - m_lastSendMs;
    if (m_lastSendMs != 0 && since < kFrameGapMs) {
        if (!m_sendTimer.isActive())
            m_sendTimer.start(int(kFrameGapMs - since));
        return;
    }
    const Outgoing item = m_sendQueue.takeFirst();
    writeFrame(item.id, item.body);
    m_lastSendMs = now;
    if (!m_sendQueue.isEmpty() && !m_sendTimer.isActive())
        m_sendTimer.start(int(kFrameGapMs));
}

void WirelessSetup::writeFrame(quint16 id, const std::string &body)
{
    QByteArray msg(4, Qt::Uninitialized);
    qToBigEndian<quint16>(quint16(body.size()), reinterpret_cast<uchar *>(msg.data()));
    qToBigEndian<quint16>(id, reinterpret_cast<uchar *>(msg.data() + 2));
    msg.append(body.data(), int(body.size()));
    // Don't spam the durable log with the 1 Hz keepalive ping traffic.
    if (id != wpp::WIFI_PING_REQUEST)
        emit log(tr("WPP tx %1 (%2 B)").arg(wppName(id)).arg(int(body.size())));
    m_profile->write(msg);
}

void WirelessSetup::onRfcommData(const QByteArray &data)
{
    emit log(tr("RFCOMM rx %1 B").arg(data.size()));
    m_rx.append(data);
    while (m_rx.size() >= 4) {
        const auto *p = reinterpret_cast<const uchar *>(m_rx.constData());
        const quint16 len = qFromBigEndian<quint16>(p);
        const quint16 id = qFromBigEndian<quint16>(p + 2);
        if (len > kMaxMessage) {
            qCWarning(lcWpp) << "invalid message length" << len;
            m_rx.clear();
            return;
        }
        if (m_rx.size() < 4 + len)
            return;
        const QByteArray body = m_rx.mid(4, len);
        m_rx.remove(0, 4 + len);
        handleMessage(id, body);
    }
}

void WirelessSetup::handleMessage(quint16 id, const QByteArray &body)
{
    m_gotMessage = true;
    // Each message that signals real progress extends the deadline, but NOT the
    // ping responses to our own 1 Hz keepalive — otherwise a head unit that
    // answers pings yet never delivers credentials would keep us waiting forever.
    if (!m_ready && id != wpp::WIFI_PING_RESPONSE && id != wpp::WIFI_PING_REQUEST)
        m_negotiateTimeout.start();
    if (id != wpp::WIFI_PING_RESPONSE) // don't spam the log with 1 Hz ping replies
        emit log(tr("WPP rx %1 (%2 B)").arg(wppName(id)).arg(body.size()));
    switch (id) {
    case wpp::WIFI_VERSION_REQUEST: {
        wpp::WifiVersionRequest req;
        if (!req.ParseFromArray(body.constData(), body.size())) {
            qCWarning(lcWpp) << "malformed WPP message" << id;
            return;
        }
        m_huMajor = req.major();
        m_huMinor = req.minor();
        emit log(tr("HU WPP version %1.%2, channel type %3").arg(m_huMajor).arg(m_huMinor).arg(int(req.channel_type())));
        // Strict OEM head units (this car withholds the WifiStartRequest for a
        // bare SUCCESS) need the Gearhead-style "satisfactory" response: echo the
        // head unit's own version, a device serial, the selected WiFi channel
        // type, and the phone's stable instance id. The instance id is also the
        // "mobile device ID" Google's receiver requires before it processes any
        // phone message, and it correlates this Bluetooth session with the AAP
        // TCP connection that follows.
        wpp::WifiVersionResponse resp;
        resp.set_major(m_huMajor > 0 ? m_huMajor : 1);
        resp.set_minor(m_huMinor);
        resp.set_status(wpp::STATUS_SUCCESS);
        resp.set_device_serial(kDeviceSerial.toStdString());
        resp.set_channel_type(req.channel_type());
        wpp::DeviceInfo *di = resp.mutable_device_info();
        di->set_device_uuid(kInstanceId.toStdString());
        di->set_bluetooth_name(kDeviceName.toStdString());
        send(wpp::WIFI_VERSION_RESPONSE, resp.SerializeAsString());
        // Start 1 Hz keepalive pings now. Without them a receiver-library head
        // unit stops talking after the version exchange (our "HU went silent"),
        // which also means it was dropping the fallback prompt below.
        if (!m_keepalive.isActive())
            emit log(tr("starting 1 Hz keepalive pings to the head unit"));
        m_keepalive.start();
        // A genuine phone never sends a WifiStartRequest; it waits for the head
        // unit's. But if this head unit stays silent after the version exchange,
        // fall back to prompting it (the log shows who sent WifiStartRequest, so
        // we learn whether this unit needs the genuine-phone order or a prompt).
        QTimer::singleShot(5000, this, [this] {
            if (m_ready || m_haveEndpoint || m_infoReceived)
                return;
            emit log(tr("head unit sent no WifiStartRequest in 5 s; prompting (fallback)"));
            send(wpp::WIFI_START_REQUEST, wpp::WifiStartRequest().SerializeAsString());
            if (!m_infoRequested) {
                m_infoRequested = true;
                send(wpp::WIFI_INFO_REQUEST, wpp::WifiInfoRequest().SerializeAsString());
                m_infoRetry.start();
            }
        });
        break;
    }
    case wpp::WIFI_START_REQUEST: {
        // The head unit announces its AAP TCP endpoint. Ack at once with a
        // status-only WifiStartResponse, then ask for the credentials.
        wpp::WifiStartRequest req;
        if (!req.ParseFromArray(body.constData(), body.size())) {
            qCWarning(lcWpp) << "malformed WPP message" << id;
            return;
        }
        m_host = QString::fromStdString(req.ip_address());
        m_port = quint16(req.port());
        m_haveEndpoint = true;
        emit log(tr("head unit AAP server %1:%2").arg(m_host).arg(m_port));
        emit status(tr("Head unit at %1:%2").arg(m_host).arg(m_port));

        wpp::WifiStartResponse resp;
        resp.set_status(wpp::STATUS_SUCCESS);
        send(wpp::WIFI_START_RESPONSE, resp.SerializeAsString());

        if (!m_infoReceived && !m_infoRequested) {
            m_infoRequested = true;
            send(wpp::WIFI_INFO_REQUEST, wpp::WifiInfoRequest().SerializeAsString());
            m_infoRetry.start();
        }
        maybeReady();
        break;
    }
    case wpp::WIFI_INFO_RESPONSE: {
        wpp::WifiInfoResponse info;
        if (!info.ParseFromArray(body.constData(), body.size())) {
            qCWarning(lcWpp) << "malformed WPP message" << id;
            return;
        }
        m_infoReceived = true;
        m_infoRetry.stop();
        if (info.security_mode() != wpp::WPA2_PERSONAL && info.security_mode() != wpp::WPA2_WPA3_PERSONAL
                && info.security_mode() != wpp::WPA3_PERSONAL) {
            qCWarning(lcWpp) << "unexpected security mode" << info.security_mode();
        }
        const QString ssid = QString::fromStdString(info.ssid());
        emit status(tr("Joining head unit WiFi %1").arg(ssid));
        m_wifi->join(ssid, QString::fromStdString(info.key()), QString::fromStdString(info.bssid()));
        break;
    }
    case wpp::WIFI_START_RESPONSE:
        // We don't initiate a start, so this is unexpected; record an endpoint
        // if one is present, for robustness.
        {
            wpp::WifiStartResponse resp;
            if (resp.ParseFromArray(body.constData(), body.size()) && !resp.ip_address().empty()) {
                m_host = QString::fromStdString(resp.ip_address());
                m_port = quint16(resp.port());
                m_haveEndpoint = true;
                maybeReady();
            }
        }
        break;
    case wpp::WIFI_PING_REQUEST: {
        // Always answer the head unit's pings (echo the timestamp). We do not
        // originate pings ourselves (officialPhoneOrder).
        wpp::WifiPingRequest req;
        if (!req.ParseFromArray(body.constData(), body.size())) {
            qCWarning(lcWpp) << "malformed WPP message" << id;
            return;
        }
        wpp::WifiPingResponse resp;
        resp.set_timestamp_ms(req.timestamp_ms());
        send(wpp::WIFI_PING_RESPONSE, resp.SerializeAsString());
        break;
    }
    case wpp::WIFI_PING_RESPONSE:
        break;
    case wpp::WIFI_CONNECTION_REJECTION:
        emit failed(tr("head unit rejected the connection"));
        break;
    default:
        emit log(tr("ignoring WPP %1").arg(wppName(id)));
        break;
    }
}

void WirelessSetup::onWifiJoined(const QString &localAddress)
{
    m_joined = true;
    m_localAddr = localAddress;
    emit log(tr("joined head unit WiFi, local address %1").arg(localAddress));
    // Report success to the head unit, then (officialPhoneOrder) send nothing
    // else over RFCOMM but answers to its pings; the link is held open.
    wpp::WifiConnectStatus st;
    st.set_status(wpp::STATUS_SUCCESS);
    send(wpp::WIFI_CONNECT_STATUS, st.SerializeAsString());
    maybeReady();
}

void WirelessSetup::maybeReady()
{
    if (m_ready || !m_joined || !m_haveEndpoint)
        return;
    if (m_host.isEmpty() || m_port == 0) {
        emit failed(tr("no projection endpoint received"));
        scheduleReconnect(tr("no projection endpoint received"));
        return;
    }
    m_ready = true;
    m_retryCount = 0; // a clean bring-up resets the backoff
    m_negotiateTimeout.stop();
    // Keep the RFCOMM link open for the whole session (we still answer pings):
    // Google's receiver tears down projection if the phone drops the link.
    emit log(tr("WiFi bootstrap complete; holding the Bluetooth link open"));
    emit readyForProjection(m_host, m_port);
}
