#include "scbrokerclient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

// Broker may briefly retry the Sunshine /api/pin relay (T08 exponential
// backoff), so allow a generous transfer timeout before giving up.
#define SC_BROKER_TIMEOUT_MS 15000

ScBrokerClient::ScBrokerClient(QObject* parent)
    : QObject(parent)
{
}

void ScBrokerClient::requestPairing(const QString& brokerUrl,
                                    const QString& connectToken,
                                    const QString& pin)
{
    QUrl url(brokerUrl);
    if (!url.isValid() || url.host().isEmpty()) {
        qWarning() << "T14: invalid broker URL:" << brokerUrl;
        emit pairingRelayed(false, QLatin1String("bad_broker_url"));
        return;
    }
    url.setPath(QLatin1String("/api/v1/pairing"));

    QJsonObject body;
    body[QLatin1String("token")] = connectToken;
    body[QLatin1String("pin")] = pin;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("application/json"));
    request.setTransferTimeout(SC_BROKER_TIMEOUT_MS);

    QNetworkReply* reply = m_Nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // Pull the broker's machine-readable fields if present — T08 responds
        // with {"status":"paired_pending"} (200) or {"error":"...",
        // "fallback":"manual_pin"} (4xx/5xx).
        QString brokerError;
        QString brokerStatus;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            brokerError = obj.value(QLatin1String("error")).toString();
            brokerStatus = obj.value(QLatin1String("status")).toString();
        }

        if (httpStatus == 0) {
            // Transport-level failure (broker unreachable, TLS error, timeout).
            qWarning() << "T14: broker pairing request failed:" << reply->errorString();
            emit pairingRelayed(false, QLatin1String("broker_unreachable"));
            return;
        }

        if (httpStatus == 200 && brokerStatus == QLatin1String("paired_pending")) {
            qInfo() << "T14: broker accepted pairing relay";
            emit pairingRelayed(true, QString());
            return;
        }

        const QString reason =
            brokerError.isEmpty() ? QStringLiteral("http_%1").arg(httpStatus) : brokerError;
        qWarning() << "T14: broker rejected pairing relay — status" << httpStatus
                   << "reason" << reason;
        emit pairingRelayed(false, reason);
    });
}
