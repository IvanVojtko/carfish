#include "wifihelper.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QFile>
#include <QProcess>
#include <QStringList>

static const QString kConnman = QStringLiteral("net.connman");
static const QString kServiceIface = QStringLiteral("net.connman.Service");

namespace {
struct ConnmanObject {
    QDBusObjectPath path;
    QVariantMap props;
};
typedef QList<ConnmanObject> ConnmanObjectList;
} // namespace

Q_DECLARE_METATYPE(ConnmanObject)
Q_DECLARE_METATYPE(ConnmanObjectList)

static const QDBusArgument &operator>>(const QDBusArgument &arg, ConnmanObject &o)
{
    arg.beginStructure();
    arg >> o.path >> o.props;
    arg.endStructure();
    return arg;
}
static QDBusArgument &operator<<(QDBusArgument &arg, const ConnmanObject &o)
{
    arg.beginStructure();
    arg << o.path << o.props;
    arg.endStructure();
    return arg;
}

static constexpr int kMaxAttempts = 12;

WifiHelper::WifiHelper(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<ConnmanObject>();
    qDBusRegisterMetaType<ConnmanObjectList>();
    m_retry.setSingleShot(true);
    m_retry.setInterval(2000);
    connect(&m_retry, &QTimer::timeout, this, &WifiHelper::attempt);
    m_routeRetry.setSingleShot(true);
    m_routeRetry.setInterval(1200);
    connect(&m_routeRetry, &QTimer::timeout, this, &WifiHelper::applyRoutingFromService);
    // While we're on the car WiFi, keep re-asserting the cellular default route.
    // Cellular can be briefly not-"ready" right as the car WiFi associates (so a
    // one-shot pin misses it, leaving the gatewayless car WiFi as default = no
    // internet). Re-checking every few seconds recovers connectivity on its own
    // the moment cellular is back.
    m_enforce.setInterval(4000);
    connect(&m_enforce, &QTimer::timeout, this, &WifiHelper::enforceCellularDefault);
}

// Re-read the joined service's IPv4 and (re)apply the routing change. Used by the
// retry timer while connman is still settling the car WiFi address after "ready".
void WifiHelper::applyRoutingFromService()
{
    if (m_servicePath.isEmpty())
        return;
    QDBusInterface svc(kConnman, m_servicePath, kServiceIface, QDBusConnection::systemBus());
    QDBusReply<QVariantMap> props = svc.call(QStringLiteral("GetProperties"));
    applyRouting(qdbus_cast<QVariantMap>(props.value().value(QStringLiteral("IPv4"))));
}

void WifiHelper::join(const QString &ssid, const QString &passphrase, const QString &bssid)
{
    // Starting a fresh join: undo any previous routing change first.
    revertRouting();
    stopWatching();
    m_ssid = ssid;
    m_passphrase = passphrase;
    m_bssid = bssid.toLower();
    m_attempts = 0;
    m_routeAttempts = 0;
    m_routeRetry.stop();
    m_ready = false;
    m_servicePath.clear();
    emit log(QStringLiteral("helper: joining \"%1\"").arg(ssid));

    QDBusInterface wifi(kConnman, QStringLiteral("/net/connman/technology/wifi"),
                        QStringLiteral("net.connman.Technology"), QDBusConnection::systemBus());
    wifi.call(QStringLiteral("SetProperty"), QStringLiteral("Powered"),
              QVariant::fromValue(QDBusVariant(true)));
    attempt();
}

void WifiHelper::release()
{
    emit log(QStringLiteral("helper: release"));
    m_retry.stop();
    m_routeRetry.stop();
    m_enforce.stop();
    m_cellularPinned = false;
    revertRouting();
    stopWatching();
    m_servicePath.clear();
    m_ready = false;
}

void WifiHelper::attempt()
{
    if (++m_attempts > kMaxAttempts) {
        emit failed(QStringLiteral("access point \"%1\" not found").arg(m_ssid));
        return;
    }
    QDBusInterface mgr(kConnman, QStringLiteral("/"), QStringLiteral("net.connman.Manager"),
                       QDBusConnection::systemBus());
    QDBusReply<ConnmanObjectList> services = mgr.call(QStringLiteral("GetServices"));
    if (services.isValid()) {
        for (const ConnmanObject &s : services.value()) {
            if (s.props.value(QStringLiteral("Type")).toString() != QLatin1String("wifi"))
                continue;
            const bool bssidMatch = !m_bssid.isEmpty()
                    && s.props.value(QStringLiteral("BSSID")).toString().toLower() == m_bssid;
            if (!bssidMatch && s.props.value(QStringLiteral("Name")).toString() != m_ssid)
                continue;
            watchService(s.path.path());
            return;
        }
    }
    QDBusInterface wifi(kConnman, QStringLiteral("/net/connman/technology/wifi"),
                        QStringLiteral("net.connman.Technology"), QDBusConnection::systemBus());
    wifi.asyncCall(QStringLiteral("Scan"));
    m_retry.start();
}

void WifiHelper::watchService(const QString &path)
{
    m_servicePath = path;
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kConnman, path, kServiceIface, QStringLiteral("PropertyChanged"), this,
                SLOT(onServicePropertyChanged(QString, QDBusVariant)));

    QDBusInterface svc(kConnman, path, kServiceIface, bus);
    QDBusReply<QVariantMap> props = svc.call(QStringLiteral("GetProperties"));
    const QString state = props.value().value(QStringLiteral("State")).toString();
    if (state == QLatin1String("ready") || state == QLatin1String("online")) {
        onServicePropertyChanged(QStringLiteral("State"), QDBusVariant(state));
        return;
    }
    if (!m_passphrase.isEmpty())
        svc.call(QStringLiteral("SetProperty"), QStringLiteral("Passphrase"),
                 QVariant::fromValue(QDBusVariant(m_passphrase)));

    // Self-heal before connecting. The internet-on-cellular feature demotes this
    // service to a manual IPv4 config (no gateway). The car's projection SSID is
    // stable per phone, so connman *saves* the service — and it can retain stale
    // address/netmask keys from that manual config even after the method is
    // flipped back to dhcp, which then makes the NEXT association fail with
    // "No carrier" / "Input/output error". So unconditionally push a clean dhcp
    // IPv4 config here (method alone, no stored address) to wipe any remnants
    // before we Connect. applyRouting re-applies the no-gateway override once we
    // are actually on the AP again.
    const QVariantMap ipCfg = qdbus_cast<QVariantMap>(props.value().value(QStringLiteral("IPv4.Configuration")));
    const QString priorMethod = ipCfg.value(QStringLiteral("Method")).toString();
    QVariantMap dhcp;
    dhcp.insert(QStringLiteral("Method"), QStringLiteral("dhcp"));
    svc.call(QStringLiteral("SetProperty"), QStringLiteral("IPv4.Configuration"),
             QVariant::fromValue(QDBusVariant(QVariant(dhcp))));
    if (priorMethod == QLatin1String("manual"))
        emit log(QStringLiteral("helper: reset a stale manual IPv4 config to dhcp before connecting"));
    m_rerouted = false;

    QDBusMessage msg = QDBusMessage::createMethodCall(kConnman, path, kServiceIface, QStringLiteral("Connect"));
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 60000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> r = *w;
        if (r.isError()) {
            // "AlreadyConnected" and "InProgress" are not failures — connman is
            // already associating this service (often because flipping the IPv4
            // config above kicked off a connect). The PropertyChanged watcher
            // reports "ready" (join) or "failure" (retry), so just wait for it;
            // emitting failed here tore down a connection that was coming up.
            const QString name = r.error().name();
            if (name == QLatin1String("net.connman.Error.AlreadyConnected")
                    || name == QLatin1String("net.connman.Error.InProgress")) {
                emit log(QStringLiteral("helper: connman already associating (%1), waiting for state")
                             .arg(r.error().message()));
            } else {
                emit failed(QStringLiteral("connman Connect: ") + r.error().message());
            }
        }
        w->deleteLater();
    });
}

void WifiHelper::stopWatching()
{
    if (m_servicePath.isEmpty())
        return;
    QDBusConnection::systemBus().disconnect(kConnman, m_servicePath, kServiceIface,
                                            QStringLiteral("PropertyChanged"), this,
                                            SLOT(onServicePropertyChanged(QString, QDBusVariant)));
}

// Make connman forget a service (disconnect + remove its saved profile). Remove
// only succeeds on a Favorite service; the error on a non-saved one is harmless.
void WifiHelper::forgetService(const QString &path)
{
    if (path.isEmpty())
        return;
    QDBusInterface svc(kConnman, path, kServiceIface, QDBusConnection::systemBus());
    svc.call(QStringLiteral("Disconnect"));
    svc.call(QStringLiteral("Remove"));
    emit log(QStringLiteral("helper: forgot the car WiFi service so the retry starts clean"));
}

void WifiHelper::onServicePropertyChanged(const QString &name, const QDBusVariant &value)
{
    if (name != QLatin1String("State"))
        return;
    const QString state = value.variant().toString();

    if (!m_ready) {
        if (state == QLatin1String("ready") || state == QLatin1String("online")) {
            QDBusInterface svc(kConnman, m_servicePath, kServiceIface, QDBusConnection::systemBus());
            QDBusReply<QVariantMap> props = svc.call(QStringLiteral("GetProperties"));
            const QVariantMap ipv4 = qdbus_cast<QVariantMap>(props.value().value(QStringLiteral("IPv4")));
            const QVariantMap eth = qdbus_cast<QVariantMap>(props.value().value(QStringLiteral("Ethernet")));
            m_wifiIface = eth.value(QStringLiteral("Interface")).toString();
            const QString addr = ipv4.value(QStringLiteral("Address")).toString();
            m_ready = true;
            m_retry.stop();
            applyRouting(ipv4);
            emit joined(addr);
        } else if (state == QLatin1String("failure")) {
            emit failed(QStringLiteral("association with \"%1\" failed").arg(m_ssid));
            // Forget the service: connman blacklists a saved service after
            // repeated association failures and keeps failing it, so drop its
            // stored profile. The app's retry re-scans and associates afresh,
            // re-supplying the passphrase from the next WifiInfoResponse.
            forgetService(m_servicePath);
            stopWatching();
            m_servicePath.clear();
        }
        return;
    }

    // Already joined: watch only for a real disconnect (not the transient
    // association/configuration flap that applying the routing change causes),
    // and revert the routing so the user's normal internet comes back.
    if (state == QLatin1String("idle") || state == QLatin1String("disconnect")) {
        emit log(QStringLiteral("helper: car WiFi dropped, reverting routing"));
        revertRouting();
        stopWatching();
        m_servicePath.clear();
        m_ready = false;
    }
}

void WifiHelper::applyRouting(const QVariantMap &ipv4)
{
    const QString addr = ipv4.value(QStringLiteral("Address")).toString();
    const QString mask = ipv4.value(QStringLiteral("Netmask")).toString();
    const QString gw = ipv4.value(QStringLiteral("Gateway")).toString();
    // We only need the address + netmask: connman assigns those as soon as the
    // service reaches "ready", but the DHCP gateway often lags behind by a moment.
    // The old guard also required the gateway and so bailed out ("nothing to drop")
    // on exactly the cars that need the fix — leaving the car WiFi as connman's
    // default service, owning the (dead) default route *and* the DNS servers in
    // resolv.conf, which is why name lookups failed even with live cellular data.
    if (addr.isEmpty() || mask.isEmpty()) {
        if (++m_routeAttempts <= 5) {
            emit log(QStringLiteral("helper: car WiFi IP not settled yet, retrying routing"));
            m_routeRetry.start();
        } else {
            emit log(QStringLiteral("helper: car WiFi never got an IPv4 address; routing unchanged"));
        }
        return;
    }
    m_routeAttempts = 0;
    // Manual IPv4 with the same address/netmask but NO gateway: connman keeps the
    // on-link subnet route (so the car's AAP endpoint stays reachable) but has no
    // default route via WiFi, so it can no longer be the default service. Mobile
    // data then serves both the default route and DNS (resolv.conf).
    QVariantMap cfg;
    cfg.insert(QStringLiteral("Method"), QStringLiteral("manual"));
    cfg.insert(QStringLiteral("Address"), addr);
    cfg.insert(QStringLiteral("Netmask"), mask);
    QDBusInterface svc(kConnman, m_servicePath, kServiceIface, QDBusConnection::systemBus());
    QDBusReply<void> r = svc.call(QStringLiteral("SetProperty"), QStringLiteral("IPv4.Configuration"),
                                  QVariant::fromValue(QDBusVariant(QVariant(cfg))));
    if (r.isValid()) {
        m_rerouted = true;
        emit log(QStringLiteral("helper: internet kept on cellular (car WiFi %1/%2, no default route)")
                     .arg(addr, mask));
    } else {
        emit log(QStringLiteral("helper: could not drop the WiFi default route: %1").arg(r.error().message()));
    }
    // connman still tends to make the (now gatewayless) car WiFi its default
    // service and install `default dev wlan0 scope link`, leaving cellular — which
    // is only "ready", not "online" — with NO default route, so nothing routes.
    // Pin the kernel default route to cellular ourselves, and keep re-asserting it
    // (m_enforce) so it recovers if cellular was briefly not ready at join time.
    m_cellularPinned = false;
    enforceCellularDefault();
    m_enforce.start();
    QTimer::singleShot(1500, this, &WifiHelper::logDiagnostics);
    QTimer::singleShot(5500, this, &WifiHelper::logDiagnostics);
}

// Find the active cellular service's interface and gateway from connman.
bool WifiHelper::cellularRoute(QString &iface, QString &gw)
{
    QDBusInterface mgr(kConnman, QStringLiteral("/"), QStringLiteral("net.connman.Manager"),
                       QDBusConnection::systemBus());
    QDBusReply<ConnmanObjectList> services = mgr.call(QStringLiteral("GetServices"));
    if (!services.isValid())
        return false;
    for (const ConnmanObject &s : services.value()) {
        if (s.props.value(QStringLiteral("Type")).toString() != QLatin1String("cellular"))
            continue;
        const QString state = s.props.value(QStringLiteral("State")).toString();
        if (state != QLatin1String("ready") && state != QLatin1String("online"))
            continue;
        const QVariantMap ipv4 = qdbus_cast<QVariantMap>(s.props.value(QStringLiteral("IPv4")));
        const QVariantMap eth = qdbus_cast<QVariantMap>(s.props.value(QStringLiteral("Ethernet")));
        iface = eth.value(QStringLiteral("Interface")).toString();
        gw = ipv4.value(QStringLiteral("Gateway")).toString();
        return !iface.isEmpty();
    }
    return false;
}

// Make cellular the kernel default route while we're on the car WiFi: delete any
// default route connman left on the WiFi interface, then install cellular's.
void WifiHelper::enforceCellularDefault()
{
    if (!m_rerouted) {
        m_enforce.stop();
        return; // only while the car WiFi routing override is active
    }
    QString cellIface, cellGw;
    if (!cellularRoute(cellIface, cellGw)) {
        // Log only on transition so the 4 s re-check isn't spammy.
        if (m_cellularPinned)
            emit log(QStringLiteral("helper: cellular not ready; internet paused until it returns"));
        m_cellularPinned = false;
        return;
    }
    const QString wifiIface = m_wifiIface.isEmpty() ? QStringLiteral("wlan0") : m_wifiIface;
    // Absolute path: the helper's systemd PATH may lack /usr/sbin, and a bare
    // "ip" that fails to launch would make this route pinning a silent no-op.
    auto ip = [](const QStringList &args) {
        QProcess p;
        p.start(QStringLiteral("/usr/sbin/ip"), args);
        p.waitForFinished(2000);
    };
    // Drop connman's gatewayless default via the car WiFi (repeat to clear dupes).
    ip({QStringLiteral("route"), QStringLiteral("del"), QStringLiteral("default"), QStringLiteral("dev"), wifiIface});
    ip({QStringLiteral("route"), QStringLiteral("del"), QStringLiteral("default"), QStringLiteral("dev"), wifiIface});
    // Install cellular as the default. Cellular links are point-to-point; add a
    // scope-link default if connman didn't expose a gateway.
    QStringList add = {QStringLiteral("route"), QStringLiteral("replace"), QStringLiteral("default")};
    if (!cellGw.isEmpty())
        add << QStringLiteral("via") << cellGw;
    add << QStringLiteral("dev") << cellIface;
    ip(add);
    if (!m_cellularPinned) // log only on transition (the 4 s re-check is quiet)
        emit log(QStringLiteral("helper: pinned default route to cellular (%1%2)")
                     .arg(cellIface, cellGw.isEmpty() ? QString() : QStringLiteral(" via ") + cellGw));
    m_cellularPinned = true;
}

void WifiHelper::logDiagnostics()
{
    QString dns;
    QFile resolv(QStringLiteral("/etc/resolv.conf"));
    if (resolv.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QList<QByteArray> lines = resolv.readAll().split('\n');
        for (const QByteArray &l : lines) {
            if (l.startsWith("nameserver"))
                dns += QString::fromUtf8(l.trimmed()) + QLatin1Char(' ');
        }
    }
    emit log(QStringLiteral("helper: resolv.conf -> %1").arg(dns.isEmpty() ? QStringLiteral("(no nameservers!)") : dns.trimmed()));

    QProcess ipr;
    ipr.start(QStringLiteral("/usr/sbin/ip"), {QStringLiteral("-4"), QStringLiteral("route"), QStringLiteral("show"),
                                               QStringLiteral("default")});
    if (ipr.waitForFinished(2000)) {
        const QString out = QString::fromUtf8(ipr.readAllStandardOutput()).trimmed().replace('\n', QLatin1Char(';'));
        emit log(QStringLiteral("helper: default route -> %1").arg(out.isEmpty() ? QStringLiteral("(none)") : out));
    }
}

void WifiHelper::revertRouting()
{
    m_enforce.stop();
    m_cellularPinned = false;
    if (!m_rerouted || m_servicePath.isEmpty())
        return;

    // The car WiFi has no internet. If we just revert it to DHCP, connman
    // reinstalls its gatewayless default route and blackholes ALL traffic — the
    // "no phone data" symptom, which then persists across the reconnect storm.
    // So, in order, with no window where data is dark:
    //   1. pin cellular as the default route NOW (data works immediately);
    //   2. disconnect the car WiFi service so its dead default route is removed
    //      (cellular then stays the only default); connman keeps the saved config;
    //   3. restore the service's DHCP config for the next clean join.
    QString cellIface, cellGw;
    if (cellularRoute(cellIface, cellGw)) {
        auto ip = [](const QStringList &args) {
            QProcess p;
            p.start(QStringLiteral("/usr/sbin/ip"), args);
            p.waitForFinished(2000);
        };
        QStringList add = {QStringLiteral("route"), QStringLiteral("replace"), QStringLiteral("default")};
        if (!cellGw.isEmpty())
            add << QStringLiteral("via") << cellGw;
        add << QStringLiteral("dev") << cellIface;
        ip(add);
    }
    QDBusInterface svc(kConnman, m_servicePath, kServiceIface, QDBusConnection::systemBus());
    svc.call(QStringLiteral("Disconnect"));
    QVariantMap cfg;
    cfg.insert(QStringLiteral("Method"), QStringLiteral("dhcp"));
    svc.call(QStringLiteral("SetProperty"), QStringLiteral("IPv4.Configuration"),
             QVariant::fromValue(QDBusVariant(QVariant(cfg))));
    m_rerouted = false;
    emit log(QStringLiteral("helper: released car WiFi; internet stays on cellular"));
}

// ---------------------------------------------------------------- adaptor
HelperAdaptor::HelperAdaptor(WifiHelper *helper)
    : QDBusAbstractAdaptor(helper)
    , m_helper(helper)
{
    connect(m_helper, &WifiHelper::joined, this, &HelperAdaptor::Joined);
    connect(m_helper, &WifiHelper::failed, this, &HelperAdaptor::Failed);
    connect(m_helper, &WifiHelper::log, this, &HelperAdaptor::Log);
}

void HelperAdaptor::Join(const QString &ssid, const QString &passphrase, const QString &bssid)
{
    m_helper->join(ssid, passphrase, bssid);
}

void HelperAdaptor::Release()
{
    m_helper->release();
}
