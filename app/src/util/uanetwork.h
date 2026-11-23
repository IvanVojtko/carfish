#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>
#include <QStandardPaths>
#include <QThread>
#include <atomic>

// OpenStreetMap's tile usage policy requires a valid, identifying User-Agent;
// requests without one are served a "418 / access blocked" placeholder tile
// (seen as a blank/garbage map). QtQuick's Image element gives no way to set
// request headers, so we install this factory on the QML engine to stamp a
// User-Agent on every QML network request (tiles, album art, geocoding, ...).
//
// It also gives each manager a disk cache, and map tiles are served from it
// before the network (PreferCache): the map is drawn at once from the last
// drive instead of re-downloading every tile on each start. A cached tile is
// refetched once it is more than kTileMaxAgeDays past its expiry. Live layers
// (traffic) and everything else follow normal HTTP caching.
class UaNetworkAccessManager : public QNetworkAccessManager
{
public:
    static constexpr qint64 kCacheBytes = 300LL * 1024 * 1024;
    static constexpr int kTileMaxAgeDays = 30;

    explicit UaNetworkAccessManager(const QString &cacheDir, QObject *parent = nullptr)
        : QNetworkAccessManager(parent)
    {
        auto *cache = new QNetworkDiskCache(this);
        cache->setCacheDirectory(cacheDir);
        cache->setMaximumCacheSize(kCacheBytes);
        setCache(cache);
    }

    // Base-map tiles: ".../z/x/y.png" from a tile server, not the traffic overlay.
    static bool isMapTile(const QUrl &url)
    {
        const QString host = url.host();
        return url.path().endsWith(QLatin1String(".png"))
            && host.contains(QLatin1String("tile"))
            && !host.contains(QLatin1String("tomtom"));
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override
    {
        QNetworkRequest req(request);
        if (req.header(QNetworkRequest::UserAgentHeader).toString().isEmpty())
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("Carfish/0.3 (SailfishOS Android Auto; personal use)"));
        if (op == GetOperation && isMapTile(req.url())) {
            req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, true);
            const QNetworkCacheMetaData meta = cache()->metaData(req.url());
            const bool stale = meta.isValid() && meta.expirationDate().isValid()
                && meta.expirationDate().addDays(kTileMaxAgeDays) < QDateTime::currentDateTimeUtc();
            req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             stale ? QNetworkRequest::PreferNetwork : QNetworkRequest::PreferCache);
        }
        return QNetworkAccessManager::createRequest(op, req, outgoingData);
    }
};

class UaNetworkAccessManagerFactory : public QQmlNetworkAccessManagerFactory
{
public:
    // Called once per thread that loads network resources (the GUI thread for
    // XMLHttpRequest, QtQuick's image reader thread for tiles). A disk cache
    // directory must not be shared between managers, so each gets its own.
    QNetworkAccessManager *create(QObject *parent) override
    {
        QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/.cache/carfish");
        const bool gui = QThread::currentThread() == QCoreApplication::instance()->thread();
        const int n = (gui ? m_gui : m_workers).fetch_add(1);
        QString sub = gui ? QStringLiteral("net-main") : QStringLiteral("net-images");
        if (n > 0)
            sub += QLatin1Char('-') + QString::number(n);
        return new UaNetworkAccessManager(base + QLatin1Char('/') + sub, parent);
    }

private:
    std::atomic_int m_gui{0};
    std::atomic_int m_workers{0};
};
