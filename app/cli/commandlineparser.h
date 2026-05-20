#pragma once

#include "settings/streamingpreferences.h"

#include <QMap>
#include <QString>

class GlobalCommandLineParser
{
public:
    enum ParseResult {
        NormalStartRequested,
        StreamRequested,
        QuitRequested,
        PairRequested,
        ListRequested,
        ConnectRequested,
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);

};

class QuitCommandLineParser
{
public:
    QuitCommandLineParser();
    virtual ~QuitCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;

private:
    QString m_Host;
};

class PairCommandLineParser
{
public:
    PairCommandLineParser();
    virtual ~PairCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    QString getPredefinedPin() const;

private:
    QString m_Host;
    QString m_PredefinedPin;
};

class StreamCommandLineParser
{
public:
    StreamCommandLineParser();
    virtual ~StreamCommandLineParser();

    void parse(const QStringList &args, StreamingPreferences *preferences);

    QString getHost() const;
    QString getAppName() const;

private:
    QString m_Host;
    QString m_AppName;
    QMap<QString, StreamingPreferences::WindowMode> m_WindowModeMap;
    QMap<QString, StreamingPreferences::AudioConfig> m_AudioConfigMap;
    QMap<QString, StreamingPreferences::VideoCodecConfig> m_VideoCodecMap;
    QMap<QString, StreamingPreferences::VideoDecoderSelection> m_VideoDecoderMap;
    QMap<QString, StreamingPreferences::CaptureSysKeysMode> m_CaptureSysKeysModeMap;
};

class ListCommandLineParser
{
public:
    ListCommandLineParser();
    virtual ~ListCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    bool isPrintCSV() const;
    bool isVerbose() const;

private:
    QString m_Host;
    bool m_PrintCSV;
    bool m_Verbose;
};

// SmartClassroom T13 — moonlight:// URL handler entry point.
//
// Parses `moonlight connect <host> [--port N] --connect-token <raw> [--host-id N]`
// — the CLI shape that the moonlight:// URL handler (in main.cpp) translates
// `moonlight://connect?token=...&host-id=...&host=...&port=...` into.
//
// The host/port pair locate (or auto-add) the Sunshine host; the connect token
// is a one-shot opaque secret issued by the SmartClassroom Broker and bound to
// a specific reservation window. T13 only parses + carries the token; the
// follow-up T14 patch series wires it into the NvHTTP Bearer header so that
// pairing/streaming proceeds without a user-entered PIN.
class ConnectCommandLineParser
{
public:
    ConnectCommandLineParser();
    virtual ~ConnectCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    int getPort() const;
    QString getConnectToken() const;
    int getHostId() const;

private:
    QString m_Host;
    int m_Port;
    QString m_ConnectToken;
    int m_HostId;
};
