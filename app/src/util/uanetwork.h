#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>

// OpenStreetMap's tile usage policy requires a valid, identifying User-Agent;
// requests without one are served a "418 / access blocked" placeholder tile
// (seen as a blank/garbage map). QtQuick's Image element gives no way to set
// request headers, so we install this factory on the QML engine to stamp a
// User-Agent on every QML network request (tiles, album art, geocoding, ...).
class UaNetworkAccessManager : public QNetworkAccessManager
{
public:
    explicit UaNetworkAccessManager(QObject *parent = nullptr)
        : QNetworkAccessManager(parent) {}

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override
    {
        QNetworkRequest req(request);
        if (req.header(QNetworkRequest::UserAgentHeader).toString().isEmpty())
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("Carfish/0.1 (SailfishOS Android Auto; personal use)"));
        return QNetworkAccessManager::createRequest(op, req, outgoingData);
    }
};

class UaNetworkAccessManagerFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        return new UaNetworkAccessManager(parent);
    }
};
