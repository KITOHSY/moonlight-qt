#pragma once

#include "identitymanager.h"
#include "nvhttp.h"

#include <openssl/x509.h>
#include <openssl/evp.h>

class NvPairingManager
{
public:
    enum PairState
    {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    };

    explicit NvPairingManager(NvComputer* computer);

    ~NvPairingManager();

    // getServerCertTimeoutMs bounds the phase-1 "getservercert" request only.
    // That request blocks on the host until the pairing PIN is entered, so the
    // default 0 (infinite) is correct for interactive pairing where the user
    // may take their time. SmartClassroom T14 headless auto-pairing passes a
    // finite value so a never-arriving PIN (e.g. Broker down) fails instead of
    // hanging Moonlight forever.
    PairState
    pair(QString appVersion, QString pin, QSslCertificate& serverCert,
         int getServerCertTimeoutMs = 0);

private:
    QByteArray
    generateRandomBytes(int length);

    QByteArray
    saltPin(const QByteArray& salt, QString pin);

    QByteArray
    encrypt(const QByteArray& plaintext, const QByteArray& key);

    QByteArray
    decrypt(const QByteArray& ciphertext, const QByteArray& key);

    QByteArray
    getSignatureFromPemCert(const QByteArray& certificate);

    bool
    verifySignature(const QByteArray& data, const QByteArray& signature, const QByteArray& serverCertificate);

    QByteArray
    signMessage(const QByteArray& message);

    NvHTTP m_Http;
    X509* m_Cert;
    EVP_PKEY* m_PrivateKey;
};
