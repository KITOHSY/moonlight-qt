#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// SmartClassroom T14 — Broker auto-pairing client.
//
// Posts the broker-issued connect token + the locally-generated 4-digit pairing
// PIN to the SmartClassroom Broker (POST <brokerUrl>/api/v1/pairing). The Broker
// (task T08) relays the PIN to the Sunshine host's /api/pin so the host-side
// pairing session unblocks — the user never types a PIN.
//
// Asynchronous: requestPairing() returns immediately; pairingRelayed() is
// emitted on the owning thread's event loop once the Broker responds. The
// pairing handshake (NvPairingManager::pair) runs concurrently on a worker
// thread, so this client stays on the main thread.
class ScBrokerClient : public QObject
{
    Q_OBJECT

public:
    explicit ScBrokerClient(QObject* parent = nullptr);

    // brokerUrl: Broker base URL from the moonlight:// `broker` param
    //            (e.g. "https://broker.example"); the API path is appended here.
    void requestPairing(const QString& brokerUrl,
                         const QString& connectToken,
                         const QString& pin);

signals:
    // success=true  → Broker accepted (HTTP 200, status "paired_pending").
    // success=false → reason carries a short machine code for logging
    //                 (bad_broker_url / broker_unreachable / http_<code> /
    //                 the broker's own `error` code).
    void pairingRelayed(bool success, QString reason);

private:
    QNetworkAccessManager m_Nam;
};
