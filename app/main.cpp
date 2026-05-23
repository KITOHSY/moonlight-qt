#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QQuickStyle>
#include <QMutex>
#include <QtDebug>
#include <QNetworkProxyFactory>
#include <QPalette>
#include <QFont>
#include <QCursor>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <QRegularExpression>
// SmartClassroom T13 — moonlight:// URL handler + single-instance forwarding
#include <QUrl>
#include <QUrlQuery>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDataStream>

// Don't let SDL hook our main function, since Qt is already
// doing the same thing. This needs to be before any headers
// that might include SDL.h themselves.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef HAVE_FFMPEG
#include "streaming/video/ffmpeg.h"
#endif

#if defined(Q_OS_WIN32)
#include "antihookingprotection.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif defined(Q_OS_LINUX)
#include <openssl/ssl.h>
#endif

#include "cli/listapps.h"
#include "cli/quitstream.h"
#include "cli/startstream.h"
#include "cli/pair.h"
#include "cli/commandlineparser.h"
#include "path.h"
#include "utils.h"
#include "gui/computermodel.h"
#include "gui/appmodel.h"
#include "backend/autoupdatechecker.h"
#include "backend/computermanager.h"
#include "backend/systemproperties.h"
#include "streaming/session.h"
#include "settings/streamingpreferences.h"
#include "gui/sdlgamepadkeynavigation.h"

#if defined(Q_OS_WIN32)
#define IS_UNSPECIFIED_HANDLE(x) ((x) == INVALID_HANDLE_VALUE || (x) == NULL)

// Log to file or console dynamically for Windows builds
#define LOG_TO_FILE
#elif !defined(QT_DEBUG) && defined(Q_OS_DARWIN)
// Log to file for release Mac builds
#define LOG_TO_FILE
#else
// Log to console for debug Mac builds
#endif

static QElapsedTimer s_LoggerTime;
static QTextStream s_LoggerStream(stderr);
static QMutex s_LoggerLock;
static bool s_SuppressVerboseOutput;
static QRegularExpression k_RikeyRegex("&rikey=\\w+");
static QRegularExpression k_RikeyIdRegex("&rikeyid=[\\d-]+");
#ifdef LOG_TO_FILE
// Max log file size of 10 MB
#define MAX_LOG_SIZE_BYTES (10 * 1024 * 1024)
static int s_LogBytesWritten = 0;
static bool s_LogLimitReached = false;
static QFile* s_LoggerFile;
#endif

void logToLoggerStream(QString& message)
{
    QMutexLocker lock(&s_LoggerLock);

#if defined(QT_DEBUG) && defined(Q_OS_WIN32)
    // Output log messages to a debugger if attached
    if (IsDebuggerPresent()) {
        static QString lineBuffer;
        lineBuffer += message;
        if (message.endsWith('\n')) {
            OutputDebugStringW(lineBuffer.toStdWString().c_str());
            lineBuffer.clear();
        }
    }
#endif

    // Strip session encryption keys and IVs from the logs
    message.replace(k_RikeyRegex, "&rikey=REDACTED");
    message.replace(k_RikeyIdRegex, "&rikeyid=REDACTED");

#ifdef LOG_TO_FILE
    if (s_LogLimitReached) {
        return;
    }
    else if (s_LogBytesWritten >= MAX_LOG_SIZE_BYTES) {
        s_LoggerStream << "Log size limit reached!";
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        s_LoggerStream << Qt::endl;
#else
        s_LoggerStream << endl;
#endif
        s_LogLimitReached = true;
        return;
    }
    else {
        s_LogBytesWritten += message.size();
    }
#endif

    s_LoggerStream << message;
    s_LoggerStream.flush();
}

void sdlLogToDiskHandler(void*, int category, SDL_LogPriority priority, const char* message)
{
    QString priorityTxt;

    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Verbose";
        break;
    case SDL_LOG_PRIORITY_DEBUG:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Debug";
        break;
    case SDL_LOG_PRIORITY_INFO:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Info";
        break;
    case SDL_LOG_PRIORITY_WARN:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Warn";
        break;
    case SDL_LOG_PRIORITY_ERROR:
        priorityTxt = "Error";
        break;
    case SDL_LOG_PRIORITY_CRITICAL:
        priorityTxt = "Critical";
        break;
    default:
        priorityTxt = "Unknown";
        break;
    }

    QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
    QString txt = QString("%1 - SDL %2 (%3): %4\n").arg(logTime.toString()).arg(priorityTxt).arg(category).arg(message);

    logToLoggerStream(txt);
}

void qtLogToDiskHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString typeTxt;

    switch (type) {
    case QtDebugMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Debug";
        break;
    case QtInfoMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Info";
        break;
    case QtWarningMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Warning";
        break;
    case QtCriticalMsg:
        typeTxt = "Critical";
        break;
    case QtFatalMsg:
        typeTxt = "Fatal";
        break;
    }

    QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
    QString txt = QString("%1 - Qt %2: %3\n").arg(logTime.toString()).arg(typeTxt).arg(msg);

    logToLoggerStream(txt);
}

#ifdef HAVE_FFMPEG

void ffmpegLogToDiskHandler(void* ptr, int level, const char* fmt, va_list vl)
{
    char lineBuffer[1024];
    static int printPrefix = 1;

    if ((level & 0xFF) > av_log_get_level()) {
        return;
    }
    else if ((level & 0xFF) > AV_LOG_WARNING && s_SuppressVerboseOutput) {
        return;
    }

    // We need to use the *previous* printPrefix value to determine whether to
    // print the prefix this time. av_log_format_line() will set the printPrefix
    // value to indicate whether the prefix should be printed *next time*.
    bool shouldPrefixThisMessage = printPrefix != 0;

    av_log_format_line(ptr, level, fmt, vl, lineBuffer, sizeof(lineBuffer), &printPrefix);

    if (shouldPrefixThisMessage) {
        QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
        QString txt = QString("%1 - FFmpeg: %2").arg(logTime.toString()).arg(lineBuffer);
        logToLoggerStream(txt);
    }
    else {
        QString txt = QString(lineBuffer);
        logToLoggerStream(txt);
    }
}

#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

static UINT s_HitUnhandledException = 0;

LONG WINAPI UnhandledExceptionHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    // Only write a dump for the first unhandled exception
    if (InterlockedCompareExchange(&s_HitUnhandledException, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    WCHAR dmpFileName[MAX_PATH];
    swprintf_s(dmpFileName, L"%ls\\Moonlight-%I64u.dmp",
               (PWCHAR)QDir::toNativeSeparators(Path::getLogDir()).utf16(), QDateTime::currentSecsSinceEpoch());
    QString qDmpFileName = QString::fromUtf16((const char16_t*)dmpFileName);
    HANDLE dumpHandle = CreateFileW(dmpFileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpHandle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info;

        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ExceptionInfo;
        info.ClientPointers = FALSE;

        DWORD typeFlags = MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpIgnoreInaccessibleMemory |
                MiniDumpWithUnloadedModules |
                MiniDumpWithThreadInfo;

        if (MiniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpHandle,
                               (MINIDUMP_TYPE)typeFlags,
                               &info,
                               nullptr,
                               nullptr)) {
            qCritical() << "Unhandled exception! Minidump written to:" << qDmpFileName;
        }
        else {
            qCritical() << "Unhandled exception! Failed to write dump:" << GetLastError();
        }

        CloseHandle(dumpHandle);
    }
    else {
        qCritical() << "Unhandled exception! Failed to open dump file:" << qDmpFileName << "with error" << GetLastError();
    }

    // Let the program crash and WER collect a dump
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

// SmartClassroom T13 — moonlight:// URL handler helpers.
//
// The URL `moonlight://connect?token=...&host-id=...&host=...&port=...` is
// generated by the SmartClassroom frontend (`frontend/src/lib/moonlight.ts`)
// and OS-registered to launch this binary. We expand any such URL inside argv
// into the equivalent shape that the `connect` CLI subcommand (see
// app/cli/commandlineparser.cpp) consumes, so URL launches and direct CLI
// invocations share a single dispatch path.
//
// Windows and Linux deliver the URL as a regular argv entry to a freshly-
// spawned process — which is what we handle here. macOS delivers URLs to an
// already-running .app via QFileOpenEvent (no argv), and intercepting that
// path requires QDesktopServices::setUrlHandler. Qt 6's setUrlHandler only
// accepts (scheme, QObject*, slot) — no lambda overload — so wiring it up
// needs a small Q_OBJECT receiver class. That work is deferred to v1.1.
#define SC_T13_SINGLE_INSTANCE_KEY "Moonlight-SingleInstance-v1"

static QStringList scT13ExpandMoonlightConnectUrl(const QStringList& args)
{
    QStringList expanded;
    for (const QString& arg : args) {
        if (!arg.startsWith("moonlight://", Qt::CaseInsensitive)) {
            expanded.append(arg);
            continue;
        }
        QUrl url(arg);
        if (!url.isValid() || url.host().compare("connect", Qt::CaseInsensitive) != 0) {
            qWarning() << "Ignoring unrecognized moonlight:// URL:" << arg;
            continue;
        }
        QUrlQuery q(url);
        const QString token  = q.queryItemValue("token");
        const QString hostId = q.queryItemValue("host-id");
        const QString host   = q.queryItemValue("host");
        const QString port   = q.queryItemValue("port");
        const QString broker = q.queryItemValue("broker", QUrl::FullyDecoded);
        if (host.isEmpty()) {
            qWarning() << "moonlight:// URL missing host parameter:" << arg;
            continue;
        }
        expanded.append("connect");
        expanded.append(host);
        if (!port.isEmpty()) {
            expanded.append("--port");
            expanded.append(port);
        }
        if (!token.isEmpty()) {
            expanded.append("--connect-token");
            expanded.append(token);
        }
        if (!hostId.isEmpty()) {
            expanded.append("--host-id");
            expanded.append(hostId);
        }
        if (!broker.isEmpty()) {
            expanded.append("--broker");
            expanded.append(broker);
        }
    }
    return expanded;
}

// Single-instance forwarder. Returns true if this process is the second
// instance and successfully handed args off to a running first instance;
// the caller should exit immediately.
static bool scT13TryForwardToRunningInstance(const QStringList& args)
{
    QLocalSocket socket;
    socket.connectToServer(SC_T13_SINGLE_INSTANCE_KEY);
    if (!socket.waitForConnected(500)) {
        return false;
    }
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_10);
    out << args;
    socket.write(payload);
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(500);
    }
    return true;
}

// First-instance receiver. Forwarded moonlight:// URLs are routed through
// QDesktopServices::openUrl so they hit the same handler that the local URL
// handler invokes. Non-URL forwarded args (e.g. `moonlight connect ...`
// invocations) are not dispatched in T13 v1 — the second instance returns 0
// and the user gets a no-op; the KPI path is the URL form, and CLI re-entry
// forwarding is a v1.1 follow-up.
//
// Forward declaration so the receiver lambda below can call into
// scT13DispatchMoonlightUrl, which is defined further down.
static void scT13DispatchMoonlightUrl(const QUrl& url);

static QLocalServer* scT13StartSingleInstanceServer(QObject* parent)
{
    // Drop any stale socket from a previous unclean shutdown.
    QLocalServer::removeServer(SC_T13_SINGLE_INSTANCE_KEY);
    auto* server = new QLocalServer(parent);
    if (!server->listen(SC_T13_SINGLE_INSTANCE_KEY)) {
        qWarning() << "T13: failed to start single-instance server:" << server->errorString();
        delete server;
        return nullptr;
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server]() {
        while (QLocalSocket* socket = server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() {
                const QByteArray data = socket->readAll();
                QDataStream in(data);
                in.setVersion(QDataStream::Qt_5_10);
                QStringList forwardedArgs;
                in >> forwardedArgs;
                for (const QString& arg : forwardedArgs) {
                    if (arg.startsWith("moonlight://", Qt::CaseInsensitive)) {
                        // Dispatch directly into the same helper the URL-on-
                        // argv path uses. We don't bounce through
                        // QDesktopServices::openUrl because we'd then need a
                        // setUrlHandler registration, and Qt 6's setUrlHandler
                        // signature requires a QObject* + slot pair (no
                        // lambda overload) — that path is deferred to v1.1
                        // along with the macOS QFileOpenEvent receiver.
                        scT13DispatchMoonlightUrl(QUrl(arg));
                    }
                }
            });
        }
    });
    return server;
}

// Hand a parsed moonlight://connect URL off to ComputerManager. Shared by
// (a) the single-instance receiver routing forwarded URLs from a second
// instance, and (b) the ConnectRequested switch arm below (via the expanded
// CLI args). The macOS QFileOpenEvent channel (which would also need to land
// here) is deferred to v1.1 — see scT13RegisterMoonlightUrlHandler note below.
static void scT13DispatchMoonlightUrl(const QUrl& url)
{
    if (!url.isValid() || url.host().compare("connect", Qt::CaseInsensitive) != 0) {
        qWarning() << "T13: ignoring unrecognized moonlight:// URL:" << url.toString();
        return;
    }
    QUrlQuery q(url);
    const QString token  = q.queryItemValue("token");
    const QString host   = q.queryItemValue("host");
    const QString hostId = q.queryItemValue("host-id");
    const QString port   = q.queryItemValue("port");
    const QString broker = q.queryItemValue("broker", QUrl::FullyDecoded);
    if (host.isEmpty()) {
        qWarning() << "T13: moonlight:// URL missing host parameter:" << url.toString();
        return;
    }
    bool portOk = !port.isEmpty();
    int portInt = portOk ? port.toInt(&portOk) : 0;
    bool hostIdOk = !hostId.isEmpty();
    int hostIdInt = hostIdOk ? hostId.toInt(&hostIdOk) : 0;
    qInfo() << "T13: dispatching moonlight://connect host=" << host
            << "port=" << (portOk ? portInt : 0)
            << "host-id=" << (hostIdOk ? hostIdInt : 0)
            << "token-prefix=" << token.left(8)
            << "broker=" << broker;
    ComputerManager::stashPendingConnect(host, portOk ? portInt : 0,
                                         token, hostIdOk ? hostIdInt : 0, broker);
}

// v1.1 follow-up: macOS QFileOpenEvent channel. On macOS, when the .app is
// already running and the user clicks a moonlight:// link a second time,
// LaunchServices delivers the URL via QFileOpenEvent instead of argv — and
// only QDesktopServices::setUrlHandler can intercept it. Qt 6's
// setUrlHandler requires (scheme, QObject* receiver, const char* slot), so
// activating that path needs a tiny Q_OBJECT receiver class (header + cpp +
// .pro update). Windows/Linux v1 don't need it because argv carries every
// URL invocation; deferring keeps the v1 surface small.

int main(int argc, char *argv[])
{
    SDL_SetMainReady();

    // Set the app version for the QCommandLineParser's showVersion() command
    QCoreApplication::setApplicationVersion(VERSION_STR);

    // Set these here to allow us to use the default QSettings constructor.
    // These also ensure that our cache directory is named correctly. As such,
    // it is critical that these be called before Path::initialize().
    QCoreApplication::setOrganizationName("Moonlight Game Streaming Project");
    QCoreApplication::setOrganizationDomain("moonlight-stream.com");
    QCoreApplication::setApplicationName("Moonlight");

    if (QFile(QDir::currentPath() + "/portable.dat").exists()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::currentPath());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::currentPath());

        // Initialize paths for portable mode
        Path::initialize(true);
    }
    else {
        // Initialize paths for standard installation
        Path::initialize(false);
    }

    // Override the default QML cache directory with the one we chose
    if (qEnvironmentVariableIsEmpty("QML_DISK_CACHE_PATH")) {
        qputenv("QML_DISK_CACHE_PATH", Path::getQmlCacheDir().toUtf8());
    }

#ifdef Q_OS_WIN32
    // Grab the original std handles before we potentially redirect them later
    HANDLE oldConOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldConErr = GetStdHandle(STD_ERROR_HANDLE);
#endif

#ifdef LOG_TO_FILE
    QDir tempDir(Path::getLogDir());

#ifdef Q_OS_WIN32
    // Only log to a file if the user didn't redirect stderr somewhere else
    if (IS_UNSPECIFIED_HANDLE(oldConErr))
#endif
    {
        s_LoggerFile = new QFile(tempDir.filePath(QString("Moonlight-%1.log").arg(QDateTime::currentSecsSinceEpoch())));
        if (s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(stderr) << "Redirecting log output to " << s_LoggerFile->fileName() << Qt::endl;
            s_LoggerStream.setDevice(s_LoggerFile);
        }
    }
#endif

    s_LoggerTime.start();
    qInstallMessageHandler(qtLogToDiskHandler);
    SDL_LogSetOutputFunction(sdlLogToDiskHandler, nullptr);

#ifdef HAVE_FFMPEG
    av_log_set_callback(ffmpegLogToDiskHandler);
#endif

#ifdef Q_OS_WIN32
    // Create a crash dump when we crash on Windows
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif

#ifdef LOG_TO_FILE
    // Prune the oldest existing logs if there are more than 10
    QStringList existingLogNames = tempDir.entryList(QStringList("Moonlight-*.log"), QDir::NoFilter, QDir::SortFlag::Time);
    for (int i = 10; i < existingLogNames.size(); i++) {
        qInfo() << "Removing old log file:" << existingLogNames.at(i);
        QFile(tempDir.filePath(existingLogNames.at(i))).remove();
    }
#endif

#if defined(Q_OS_WIN32)
    // Force AntiHooking.dll to be statically imported and loaded
    // by ntdll on Win32 platforms by calling a dummy function.
    AntiHookingDummyImport();
#elif defined(Q_OS_LINUX)
    // Force libssl.so to be directly linked to our binary, so
    // linuxdeployqt can find it and include it in our AppImage.
    // QtNetwork will pull it in via dlopen().
    SSL_free(nullptr);
#endif

    // We keep this at function scope to ensure it stays around while we're running,
    // becaue the Qt QPA will need to read it. Since the temporary file is only
    // created when open() is called, this doesn't do any harm for other platforms.
    QTemporaryFile eglfsConfigFile("eglfs_override_XXXXXX.conf");

    // Avoid using High DPI on EGLFS. It breaks font rendering.
    // https://bugreports.qt.io/browse/QTBUG-64377
    //
    // NB: We can't use QGuiApplication::platformName() here because it is only
    // set once the QGuiApplication is created, which is too late to enable High DPI :(
    if (WMUtils::isRunningWindowManager()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        // Enable High DPI support on Qt 5.x. It is always enabled on Qt 6.0
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        // Enable fractional High DPI scaling on Qt 5.14 and later
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    }
    else {
#ifndef STEAM_LINK
        if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
            qInfo() << "Unable to detect Wayland or X11, so EGLFS will be used by default. Set QT_QPA_PLATFORM to override this.";
            qputenv("QT_QPA_PLATFORM", "eglfs");

            if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_ALWAYS_SET_MODE")) {
                qInfo() << "Setting display mode by default. Set QT_QPA_EGLFS_ALWAYS_SET_MODE=0 to override this.";

                // The UI doesn't appear on RetroPie without this option.
                qputenv("QT_QPA_EGLFS_ALWAYS_SET_MODE", "1");
            }

            if (!QFile("/dev/dri").exists()) {
                qWarning() << "Unable to find a KMSDRM display device!";
                qWarning() << "On the Raspberry Pi, you must enable the 'fake KMS' driver in raspi-config to use Moonlight outside of the GUI environment.";
            }
            else if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_KMS_CONFIG")) {
                // HACK: Remove this when Qt is fixed to properly check for display support before picking a card
                QString cardOverride = WMUtils::getDrmCardOverride();
                if (!cardOverride.isEmpty()) {
                    if (eglfsConfigFile.open()) {
                        qInfo() << "Overriding default Qt EGLFS card selection to" << cardOverride;
                        QTextStream(&eglfsConfigFile) << "{ \"device\": \"" << cardOverride << "\" }";
                        qputenv("QT_QPA_EGLFS_KMS_CONFIG", eglfsConfigFile.fileName().toUtf8());
                    }
                }
            }
        }

        // EGLFS uses OpenGLES 2.0, so we will too. Some embedded platforms may not
        // even have working OpenGL implementations, so GLES is the only option.
        // See https://github.com/moonlight-stream/moonlight-qt/issues/868
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
    }

#if !defined(Q_PROCESSOR_X86) && defined(SDL_HINT_VIDEO_X11_FORCE_EGL)
    // Some ARM and RISC-V embedded devices don't have working GLX which can cause
    // SDL to fail to find a working OpenGL implementation at all. Let's force EGL
    // on non-x86 platforms, since GLX is deprecated anyway.
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif

#ifdef Q_OS_MACOS
    // This avoids using the default keychain for SSL, which may cause
    // password prompts on macOS.
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#endif

#if defined(Q_OS_WIN32) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!qEnvironmentVariableIsSet("QT_OPENGL")) {
        // On Windows, use ANGLE so we don't have to load OpenGL
        // user-mode drivers into our app. OGL drivers (especially Intel)
        // seem to crash Moonlight far more often than DirectX.
        qputenv("QT_OPENGL", "angle");
    }
#endif

#if !defined(Q_OS_WIN32) || QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Moonlight requires the non-threaded renderer because we depend
    // on being able to control the render thread by blocking in the
    // main thread (and pumping events from the main thread when needed).
    // That doesn't work with the threaded renderer which causes all
    // sorts of odd behavior depending on the platform.
    //
    // NB: Windows defaults to the "windows" non-threaded render loop on
    // Qt 5 and the threaded render loop on Qt 6.
    qputenv("QSG_RENDER_LOOP", "basic");
#endif

#if defined(Q_OS_DARWIN) && defined(QT_DEBUG)
    // Enable Metal valiation for debug builds
    qputenv("MTL_DEBUG_LAYER", "1");
    qputenv("MTL_SHADER_VALIDATION", "1");
#endif

    // We don't want system proxies to apply to us
    QNetworkProxyFactory::setUseSystemConfiguration(false);

    // Clear any default application proxy
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    QNetworkProxy::setApplicationProxy(noProxy);

    // Register custom metatypes for use in signals
    qRegisterMetaType<NvApp>("NvApp");

    // Allow the display to sleep by default. We will manually use SDL_DisableScreenSaver()
    // and SDL_EnableScreenSaver() when appropriate. This hint must be set before
    // initializing the SDL video subsystem to have any effect.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    // We use MMAL to render on Raspberry Pi, so we do not require DRM master.
    SDL_SetHint("SDL_KMSDRM_REQUIRE_DRM_MASTER", "0");

    // Use Direct3D 9Ex to avoid a deadlock caused by the D3D device being reset when
    // the user triggers a UAC prompt. This option controls the software/SDL renderer.
    // The DXVA2 renderer uses Direct3D 9Ex itself directly.
    SDL_SetHint("SDL_WINDOWS_USE_D3D9EX", "1");

    if (SDL_InitSubSystem(SDL_INIT_TIMER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_TIMER) failed: %s",
                     SDL_GetError());
        return -1;
    }

#ifdef STEAM_LINK
    // Steam Link requires that we initialize video before creating our
    // QGuiApplication in order to configure the framebuffer correctly.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return -1;
    }
#endif

    // Use atexit() to ensure SDL_Quit() is called. This avoids
    // racing with object destruction where SDL may be used.
    atexit(SDL_Quit);

    // Avoid the default behavior of changing the timer resolution to 1 ms.
    // We don't want this all the time that Moonlight is open. We will set
    // it manually when we start streaming.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");

    // Disable minimize on focus loss by default. Users seem to want this off by default.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // SDL 2.0.12 changes the default behavior to use the button label rather than the button
    // position as most other software does. Set this back to 0 to stay consistent with prior
    // releases of Moonlight.
    SDL_SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");

    // Disable relative mouse scaling to renderer size or logical DPI. We want to send
    // the mouse motion exactly how it was given to us.
    SDL_SetHint("SDL_MOUSE_RELATIVE_SCALING", "0");

    // Set our app name for SDL to use with PulseAudio and PipeWire. This matches what we
    // provide as our app name to libsoundio too. On SDL 2.0.18+, SDL_APP_NAME is also used
    // for screensaver inhibitor reporting.
    SDL_SetHint("SDL_AUDIO_DEVICE_APP_NAME", "Moonlight");
    SDL_SetHint("SDL_APP_NAME", "Moonlight");

    // We handle capturing the mouse ourselves when it leaves the window, so we don't need
    // SDL doing it for us behind our backs.
    SDL_SetHint("SDL_MOUSE_AUTO_CAPTURE", "0");

    // SDL will try to lock the mouse cursor on Wayland if it's not visible in order to
    // support applications that assume they can warp the cursor (which isn't possible
    // on Wayland). We don't want this behavior because it interferes with seamless mouse
    // mode when toggling between windowed and fullscreen modes by unexpectedly locking
    // the mouse cursor.
    SDL_SetHint("SDL_VIDEO_WAYLAND_EMULATE_MOUSE_WARP", "0");

#ifdef QT_DEBUG
    // Allow thread naming using exceptions on debug builds. SDL doesn't use SEH
    // when throwing the exceptions, so we don't enable it for release builds out
    // of caution.
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "0");
#endif

    QGuiApplication app(argc, argv);

#ifndef STEAM_LINK
    // Force use of the KMSDRM backend for SDL when using Qt platform plugins
    // that directly draw to the display without a windowing system.
    if (QGuiApplication::platformName() == "eglfs" || QGuiApplication::platformName() == "linuxfb") {
        qputenv("SDL_VIDEODRIVER", "kmsdrm");
    }
#endif

    // SmartClassroom T13 — moonlight:// URL plumbing.
    //
    // 1) If a moonlight:// URL is in argv and another instance is already
    //    running, forward the args and exit so the user only ever sees one
    //    Moonlight window for an OS-level URL click. Other CLI invocations
    //    fall through to per-instance handling (forwarding CLI re-entry is a
    //    v1.1 follow-up).
    // 2) Start a local server so a future second instance can find us.
    //    (macOS QFileOpenEvent registration is deferred to v1.1 — see the
    //    note next to scT13DispatchMoonlightUrl below.)
    {
        const QStringList earlyArgs = app.arguments();
        bool hasMoonlightUrl = false;
        for (const QString& a : earlyArgs) {
            if (a.startsWith("moonlight://", Qt::CaseInsensitive)) {
                hasMoonlightUrl = true;
                break;
            }
        }
        if (hasMoonlightUrl && scT13TryForwardToRunningInstance(earlyArgs)) {
            return 0;
        }
    }
    QLocalServer* scT13SingleInstanceServer = scT13StartSingleInstanceServer(&app);
    Q_UNUSED(scT13SingleInstanceServer);

    // Expand any moonlight:// URL in argv into the equivalent `connect`
    // subcommand shape before the standard CLI parser runs.
    const QStringList scT13ProcessedArgs = scT13ExpandMoonlightConnectUrl(app.arguments());

    GlobalCommandLineParser parser;
    GlobalCommandLineParser::ParseResult commandLineParserResult = parser.parse(scT13ProcessedArgs);
    switch (commandLineParserResult) {
    case GlobalCommandLineParser::ListRequested:
        // Don't log to the console since it will jumble the command output
        s_SuppressVerboseOutput = true;
#ifdef Q_OS_WIN32
        // If we don't have stdout or stderr handles (which will normally be the case
        // since we're a /SUBSYSTEM:WINDOWS app), attach to our parent console and use
        // that for stdout and stderr.
        //
        // If we do have stdout or stderr handles, that means the user has used standard
        // handle redirection. In that case, we don't want to override those handles.
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            // If we didn't have an old stdout/stderr handle, use the new CONOUT$ handle
            if (IS_UNSPECIFIED_HANDLE(oldConOut)) {
                freopen("CONOUT$", "w", stdout);
                setvbuf(stdout, NULL, _IONBF, 0);
            }
            if (IS_UNSPECIFIED_HANDLE(oldConErr)) {
                freopen("CONOUT$", "w", stderr);
                setvbuf(stderr, NULL, _IONBF, 0);
            }
        }
#endif
        break;
    default:
        break;
    }

    SDL_version compileVersion;
    SDL_VERSION(&compileVersion);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Compiled with SDL %d.%d.%d",
                compileVersion.major, compileVersion.minor, compileVersion.patch);

    SDL_version runtimeVersion;
    SDL_GetVersion(&runtimeVersion);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Running with SDL %d.%d.%d",
                runtimeVersion.major, runtimeVersion.minor, runtimeVersion.patch);

    // Apply the initial translation based on user preference
    StreamingPreferences::get()->retranslate();

    // Trickily declare the translation for dialog buttons
    QCoreApplication::translate("QPlatformTheme", "&Yes");
    QCoreApplication::translate("QPlatformTheme", "&No");
    QCoreApplication::translate("QPlatformTheme", "OK");
    QCoreApplication::translate("QPlatformTheme", "Help");
    QCoreApplication::translate("QPlatformTheme", "Cancel");

    // After the QGuiApplication is created, the platform stuff will be initialized
    // and we can set the SDL video driver to match Qt.
    if (WMUtils::isRunningWayland() && QGuiApplication::platformName() == "xcb") {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected XWayland. This will probably break hardware decoding! Try running with QT_QPA_PLATFORM=wayland or switch to X11.");
        qputenv("SDL_VIDEODRIVER", "x11");
    }
    else if (QGuiApplication::platformName().startsWith("wayland")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected Wayland");
        qputenv("SDL_VIDEODRIVER", "wayland");
    }

#ifdef STEAM_LINK
    // Qt 5.9 from the Steam Link SDK is not able to load any fonts
    // since the Steam Link doesn't include any of the ones it looks
    // for. We know it has NotoSans so we will explicitly ask for that.
    if (app.font().family().isEmpty()) {
        qWarning() << "SL HACK: No default font - using NotoSans";

        QFont fon("NotoSans");
        app.setFont(fon);
    }

    // Move the mouse to the bottom right so it's invisible when using
    // gamepad-only navigation.
    QCursor().setPos(0xFFFF, 0xFFFF);
#elif !SDL_VERSION_ATLEAST(2, 0, 11) && defined(Q_OS_LINUX) && (defined(__arm__) || defined(__aarch64__))
    if (qgetenv("SDL_VIDEO_GL_DRIVER").isEmpty() && QGuiApplication::platformName() == "eglfs") {
        // Look for Raspberry Pi GLES libraries. SDL 2.0.10 and earlier needs some help finding
        // the correct libraries for the KMSDRM backend if not compiled with the RPI backend enabled.
        if (SDL_LoadObject("libbrcmGLESv2.so") != nullptr) {
            qputenv("SDL_VIDEO_GL_DRIVER", "libbrcmGLESv2.so");
        }
        else if (SDL_LoadObject("/opt/vc/lib/libbrcmGLESv2.so") != nullptr) {
            qputenv("SDL_VIDEO_GL_DRIVER", "/opt/vc/lib/libbrcmGLESv2.so");
        }
    }
#endif

#ifndef Q_OS_DARWIN
    // Set the window icon except on macOS where we want to keep the
    // modified macOS 11 style rounded corner icon.
    app.setWindowIcon(QIcon(":/res/moonlight.svg"));
#endif

    // This is necessary to show our icon correctly on Wayland
    app.setDesktopFileName("com.moonlight_stream.Moonlight.desktop");
    qputenv("SDL_VIDEO_WAYLAND_WMCLASS", "com.moonlight_stream.Moonlight");
    qputenv("SDL_VIDEO_X11_WMCLASS", "com.moonlight_stream.Moonlight");

    // Register our C++ types for QML
    qmlRegisterType<ComputerModel>("ComputerModel", 1, 0, "ComputerModel");
    qmlRegisterType<AppModel>("AppModel", 1, 0, "AppModel");
    qmlRegisterUncreatableType<Session>("Session", 1, 0, "Session", "Session cannot be created from QML");
    qmlRegisterSingletonType<ComputerManager>("ComputerManager", 1, 0,
                                              "ComputerManager",
                                              [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                  return new ComputerManager(StreamingPreferences::get(qmlEngine));
                                              });
    qmlRegisterSingletonType<AutoUpdateChecker>("AutoUpdateChecker", 1, 0,
                                                "AutoUpdateChecker",
                                                [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                    return new AutoUpdateChecker();
                                                });
    qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0,
                                               "SystemProperties",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   return new SystemProperties();
                                               });
    qmlRegisterSingletonType<SdlGamepadKeyNavigation>("SdlGamepadKeyNavigation", 1, 0,
                                                      "SdlGamepadKeyNavigation",
                                                      [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                          return new SdlGamepadKeyNavigation(StreamingPreferences::get(qmlEngine));
                                                      });
    qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
                                                   "StreamingPreferences",
                                                   [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                       return StreamingPreferences::get(qmlEngine);
                                                   });

    // Create the identity manager on the main thread
    IdentityManager::get();

    // We require the Material theme
    QQuickStyle::setStyle("Material");

    // Our icons are styled for a dark theme, so we do not allow the user to override this
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

    // These are defaults that we allow the user to override
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_ACCENT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Purple");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    }

    QQmlApplicationEngine engine;
    QString initialView;
    bool hasGUI = true;

    switch (commandLineParserResult) {
    case GlobalCommandLineParser::NormalStartRequested:
        initialView = "qrc:/gui/PcView.qml";
        break;
    case GlobalCommandLineParser::StreamRequested:
        {
            initialView = "qrc:/gui/CliStartStreamSegue.qml";
            StreamingPreferences* preferences = StreamingPreferences::get();
            StreamCommandLineParser streamParser;
            streamParser.parse(app.arguments(), preferences);
            QString host    = streamParser.getHost();
            QString appName = streamParser.getAppName();
            auto launcher   = new CliStartStream::Launcher(host, appName, preferences, &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::QuitRequested:
        {
            initialView = "qrc:/gui/CliQuitStreamSegue.qml";
            QuitCommandLineParser quitParser;
            quitParser.parse(app.arguments());
            auto launcher = new CliQuitStream::Launcher(quitParser.getHost(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::PairRequested:
        {
            initialView = "qrc:/gui/CliPair.qml";
            PairCommandLineParser pairParser;
            pairParser.parse(app.arguments());
            auto launcher = new CliPair::Launcher(pairParser.getHost(), pairParser.getPredefinedPin(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::ListRequested:
        {
            ListCommandLineParser listParser;
            listParser.parse(app.arguments());
            auto launcher = new CliListApps::Launcher(listParser.getHost(), listParser, &app);
            launcher->execute(new ComputerManager(StreamingPreferences::get()));
            hasGUI = false;
            break;
        }
    case GlobalCommandLineParser::ConnectRequested:
        {
            // SmartClassroom T13 — moonlight:// URL or `connect` CLI entry.
            // Lands on PcView while the host/token/broker are handed off to
            // ComputerManager. ComputerManager auto-adds the host if it's
            // not already known and stamps the broker connect context on the
            // resulting NvComputer (T14 uses it to auto-pair via the Broker
            // and auto-stream without a user-entered PIN).
            initialView = "qrc:/gui/PcView.qml";
            ConnectCommandLineParser connectParser;
            connectParser.parse(scT13ProcessedArgs);
            qInfo() << "T13 connect requested host=" << connectParser.getHost()
                    << "port=" << connectParser.getPort()
                    << "host-id=" << connectParser.getHostId()
                    << "token-prefix=" << connectParser.getConnectToken().left(8)
                    << "broker=" << connectParser.getBrokerUrl();
            ComputerManager::stashPendingConnect(connectParser.getHost(),
                                                 connectParser.getPort(),
                                                 connectParser.getConnectToken(),
                                                 connectParser.getHostId(),
                                                 connectParser.getBrokerUrl());
            break;
        }
    }

    if (hasGUI) {
        engine.rootContext()->setContextProperty("initialView", initialView);

        // Load the main.qml file
        engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
        if (engine.rootObjects().isEmpty())
            return -1;
    }

    int err = app.exec();

    // Give worker tasks time to properly exit. Fixes PendingQuitTask
    // sometimes freezing and blocking process exit.
    QThreadPool::globalInstance()->waitForDone(30000);

#ifdef Q_OS_WIN32
    // Without an explicit flush, console redirection for the list command
    // doesn't work reliably (sometimes the target file contains no text).
    fflush(stderr);
    fflush(stdout);
#endif

    return err;
}
