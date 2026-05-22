#include "computermanager.h"
#include "boxartmanager.h"
#include "nvhttp.h"
#include "nvpairingmanager.h"
#include "scbrokerclient.h"
#include "streaming/session.h"

#include <Limelight.h>
#include <QtEndian>

#include <QThread>
#include <QThreadPool>
#include <QCoreApplication>

#include <random>

#define SER_HOSTS "hosts"
#define SER_HOSTS_BACKUP "hostsbackup"

// SmartClassroom T14 — phase-1 ("getservercert") bound for headless
// auto-pairing. Generous: the Broker may retry the Sunshine /api/pin relay
// (T08 exponential backoff), so allow well past the ScBrokerClient transfer
// timeout before treating a never-arriving PIN as a failure.
#define SC_HEADLESS_GETSERVERCERT_TIMEOUT_MS 30000

// SmartClassroom T14 — overall auto-connect guard. Bounds headless pairing +
// the post-pairing app-list poll + margin; if the flow hasn't produced a
// Session by then it fails and the user falls back to the manual PcView UI.
#define SC_AUTOCONNECT_TIMEOUT_MS 90000

class PcMonitorThread : public QThread
{
    Q_OBJECT

#define TRIES_BEFORE_OFFLINING 2
#define POLLS_PER_APPLIST_FETCH 10

public:
    PcMonitorThread(NvComputer* computer)
        : m_Computer(computer)
    {
        setObjectName("Polling thread for " + computer->name);
    }

private:
    bool tryPollComputer(NvAddress address, bool& changed)
    {
        NvHTTP http(address, 0, m_Computer->serverCert);

        QString serverInfo;
        try {
            serverInfo = http.getServerInfo(NvHTTP::NvLogLevel::NVLL_NONE, true);
        } catch (...) {
            return false;
        }

        NvComputer newState(http, serverInfo);

        // Ensure the machine that responded is the one we intended to contact
        if (m_Computer->uuid != newState.uuid) {
            qInfo() << "Found unexpected PC" << newState.name << "looking for" << m_Computer->name;
            return false;
        }

        changed = m_Computer->update(newState);
        return true;
    }

    bool updateAppList(bool& changed)
    {
        NvHTTP http(m_Computer);

        QVector<NvApp> appList;

        try {
            appList = http.getAppList();
            if (appList.isEmpty()) {
                return false;
            }
        } catch (...) {
            return false;
        }

        QWriteLocker lock(&m_Computer->lock);
        changed = m_Computer->updateAppList(appList);
        return true;
    }

    void run() override
    {
        // Always fetch the applist the first time
        int pollsSinceLastAppListFetch = POLLS_PER_APPLIST_FETCH;
        while (!isInterruptionRequested()) {
            bool stateChanged = false;
            bool online = false;
            bool wasOnline = m_Computer->state == NvComputer::CS_ONLINE;
            for (int i = 0; i < (wasOnline ? TRIES_BEFORE_OFFLINING : 1) && !online; i++) {
                for (auto& address : m_Computer->uniqueAddresses()) {
                    if (isInterruptionRequested()) {
                        return;
                    }

                    if (tryPollComputer(address, stateChanged)) {
                        if (!wasOnline) {
                            qInfo() << m_Computer->name << "is now online at" << m_Computer->activeAddress.toString();
                        }
                        online = true;
                        break;
                    }
                }
            }

            // Check if we failed after all retry attempts
            // Note: we don't need to acquire the read lock here,
            // because we're on the writing thread.
            if (!online && m_Computer->state != NvComputer::CS_OFFLINE) {
                qInfo() << m_Computer->name << "is now offline";
                m_Computer->state = NvComputer::CS_OFFLINE;
                stateChanged = true;
            }

            // Grab the applist if it's empty or it's been long enough that we need to refresh
            pollsSinceLastAppListFetch++;
            if (m_Computer->state == NvComputer::CS_ONLINE &&
                    m_Computer->pairState == NvComputer::PS_PAIRED &&
                    (m_Computer->appList.isEmpty() || pollsSinceLastAppListFetch >= POLLS_PER_APPLIST_FETCH)) {
                // Notify prior to the app list poll since it may take a while, and we don't
                // want to delay onlining of a machine, especially if we already have a cached list.
                if (stateChanged) {
                    emit computerStateChanged(m_Computer);
                    stateChanged = false;
                }

                if (updateAppList(stateChanged)) {
                    pollsSinceLastAppListFetch = 0;
                }
            }

            if (stateChanged) {
                // Tell anyone listening that we've changed state
                emit computerStateChanged(m_Computer);
            }

            // Wait a bit to poll again, but do it in 100 ms chunks
            // so we can be interrupted reasonably quickly.
            // FIXME: QWaitCondition would be better.
            for (int i = 0; i < 30 && !isInterruptionRequested(); i++) {
                QThread::msleep(100);
            }
        }
    }

signals:
   void computerStateChanged(NvComputer* computer);

private:
    NvComputer* m_Computer;
};

// SmartClassroom T13 — static pending-connect bookkeeping.
QMutex ComputerManager::s_StashedConnectsLock;
QList<ComputerManager::PendingConnect> ComputerManager::s_StashedConnects;
QPointer<ComputerManager> ComputerManager::s_ActiveInstance;

ComputerManager::ComputerManager(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_PollingRef(0),
      m_MdnsBrowser(nullptr),
      m_CompatFetcher(nullptr),
      m_NeedsDelayedFlush(false)
{
    QSettings settings;

    // If there's a hosts backup copy, we must have failed to commit
    // a previous update before exiting. Restore the backup now.
    int hosts = settings.beginReadArray(SER_HOSTS_BACKUP);
    if (hosts == 0) {
        // If there's no host backup, read from the primary location.
        settings.endArray();
        hosts = settings.beginReadArray(SER_HOSTS);
    }

    // Inflate our hosts from QSettings
    for (int i = 0; i < hosts; i++) {
        settings.setArrayIndex(i);
        NvComputer* computer = new NvComputer(settings);
        m_KnownHosts[computer->uuid] = computer;
        m_LastSerializedHosts[computer->uuid] = *computer;
    }
    settings.endArray();

    // Fetch latest compatibility data asynchronously
    m_CompatFetcher.start();

    // Start the delayed flush thread to handle saveHosts() calls
    m_DelayedFlushThread = new DelayedFlushThread(this);
    m_DelayedFlushThread->start();

    // To quit in a timely manner, we must block additional requests
    // after we receive the aboutToQuit() signal. This is necessary
    // because NvHTTP uses aboutToQuit() to abort requests in progress
    // while quitting, however this is a one time signal - additional
    // requests would not be aborted and block termination.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ComputerManager::handleAboutToQuit);

    // SmartClassroom T14 — auto-connect / headless-pairing wiring. The broker
    // client, the timeout guard and every completion leg run on the main
    // thread, so the auto-connect state needs no extra locking.
    m_ScBrokerClient = new ScBrokerClient(this);
    connect(m_ScBrokerClient, &ScBrokerClient::pairingRelayed,
            this, &ComputerManager::scHandleBrokerPairingRelayed);
    connect(this, &ComputerManager::autoPairingCompleted,
            this, &ComputerManager::scHandleAutoPairingCompleted);
    m_AutoConnectTimer = new QTimer(this);
    m_AutoConnectTimer->setSingleShot(true);
    connect(m_AutoConnectTimer, &QTimer::timeout,
            this, &ComputerManager::scHandleAutoConnectTimeout);

    // SmartClassroom T13 — claim the active-instance slot and drain any
    // connect requests stashed by main.cpp before this instance existed.
    s_ActiveInstance = this;
    flushStashedConnects();
}

ComputerManager::~ComputerManager()
{
    // SmartClassroom T13 — release the active-instance slot so any
    // subsequent stashPendingConnect calls fall back to the static queue.
    if (s_ActiveInstance.data() == this) {
        s_ActiveInstance.clear();
    }

    // Stop the delayed flush thread before acquiring the lock in write mode
    // to avoid deadlocking with a flush that needs the lock in read mode.
    {
        // Wake the delayed flush thread
        m_DelayedFlushThread->requestInterruption();
        m_DelayedFlushCondition.wakeOne();

        // Wait for it to terminate (and finish any pending flush)
        m_DelayedFlushThread->wait();
        delete m_DelayedFlushThread;

        // Delayed flushes should have completed by now
        Q_ASSERT(!m_NeedsDelayedFlush);
    }

    QWriteLocker lock(&m_Lock);

    // Delete machines that haven't been resolved yet
    while (!m_PendingResolution.isEmpty()) {
        MdnsPendingComputer* computer = m_PendingResolution.first();
        delete computer;
        m_PendingResolution.removeFirst();
    }

    // Delete the browser to stop discovery
    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;

    // Interrupt polling
    for (ComputerPollingEntry* entry : m_PollEntries) {
        entry->interrupt();
    }

    // Delete all polling entries (and associated threads)
    for (ComputerPollingEntry* entry : m_PollEntries) {
        delete entry;
    }

    // Destroy all NvComputer objects now that polling is halted
    for (NvComputer* computer : m_KnownHosts) {
        delete computer;
    }
}

void DelayedFlushThread::run() {
    for (;;) {
        // Wait for a delayed flush request or an interruption
        {
            QMutexLocker locker(&m_ComputerManager->m_DelayedFlushMutex);

            while (!QThread::currentThread()->isInterruptionRequested() && !m_ComputerManager->m_NeedsDelayedFlush) {
                m_ComputerManager->m_DelayedFlushCondition.wait(&m_ComputerManager->m_DelayedFlushMutex);
            }

            // Bail without flushing if we woke up for an interruption alone.
            // If we have both an interruption and a flush request, do the flush.
            if (!m_ComputerManager->m_NeedsDelayedFlush) {
                Q_ASSERT(QThread::currentThread()->isInterruptionRequested());
                break;
            }

            // Reset the delayed flush flag to ensure any racing saveHosts() call will set it again
            m_ComputerManager->m_NeedsDelayedFlush = false;

            // Update the last serialized hosts map under the delayed flush mutex
            m_ComputerManager->m_LastSerializedHosts.clear();
            for (const NvComputer* computer : m_ComputerManager->m_KnownHosts) {
                // Copy the current state of the NvComputer to allow us to check later if we need
                // to serialize it again when attribute updates occur.
                QReadLocker computerLock(&computer->lock);
                m_ComputerManager->m_LastSerializedHosts[computer->uuid] = *computer;
            }
        }

        // Perform the flush
        {
            QSettings settings;

            // First, write to the backup location
            settings.beginWriteArray(SER_HOSTS_BACKUP);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : m_ComputerManager->m_KnownHosts) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, false);
                }
            }
            settings.endArray();

            // Next, write to the primary location
            settings.remove(SER_HOSTS);
            settings.beginWriteArray(SER_HOSTS);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : m_ComputerManager->m_KnownHosts) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, true);
                }
            }
            settings.endArray();

            // Finally, delete the backup copy
            settings.remove(SER_HOSTS_BACKUP);
        }
    }
}

void ComputerManager::saveHosts()
{
    Q_ASSERT(m_DelayedFlushThread != nullptr && m_DelayedFlushThread->isRunning());

    // Punt to a worker thread because QSettings on macOS can take ages (> 500 ms)
    // to persist our host list to disk (especially when a host has a bunch of apps).
    QMutexLocker locker(&m_DelayedFlushMutex);
    m_NeedsDelayedFlush = true;
    m_DelayedFlushCondition.wakeOne();
}

QHostAddress ComputerManager::getBestGlobalAddressV6(QVector<QHostAddress> &addresses)
{
    for (const QHostAddress& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            if (address.isInSubnet(QHostAddress("fe80::"), 10)) {
                // Link-local
                continue;
            }

            if (address.isInSubnet(QHostAddress("fec0::"), 10)) {
                qInfo() << "Ignoring site-local address:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("fc00::"), 7)) {
                qInfo() << "Ignoring ULA:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("2002::"), 16)) {
                qInfo() << "Ignoring 6to4 address:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("2001::"), 32)) {
                qInfo() << "Ignoring Teredo address:" << address;
                continue;
            }

            return address;
        }
    }

    return QHostAddress();
}

void ComputerManager::startPolling()
{
    QWriteLocker lock(&m_Lock);

    if (++m_PollingRef > 1) {
        return;
    }

    if (m_Prefs->enableMdns) {
        // Start an MDNS query for GameStream hosts
        m_MdnsServer.reset(new QMdnsEngine::Server());
        m_MdnsBrowser = new QMdnsEngine::Browser(m_MdnsServer.data(), "_nvstream._tcp.local.");
        connect(m_MdnsBrowser, &QMdnsEngine::Browser::serviceAdded,
                this, [this](const QMdnsEngine::Service& service) {
            qInfo() << "Discovered mDNS host:" << service.hostname();

            MdnsPendingComputer* pendingComputer = new MdnsPendingComputer(m_MdnsServer, service);
            connect(pendingComputer, &MdnsPendingComputer::resolvedHost,
                    this, &ComputerManager::handleMdnsServiceResolved);
            m_PendingResolution.append(pendingComputer);
        });
    }
    else {
        qWarning() << "mDNS is disabled by user preference";
    }

    // Start polling threads for each known host
    QMapIterator<QString, NvComputer*> i(m_KnownHosts);
    while (i.hasNext()) {
        i.next();
        startPollingComputer(i.value());
    }
}

// Must hold m_Lock for write
void ComputerManager::startPollingComputer(NvComputer* computer)
{
    if (m_PollingRef == 0) {
        return;
    }

    ComputerPollingEntry* pollingEntry;

    if (!m_PollEntries.contains(computer->uuid)) {
        pollingEntry = m_PollEntries[computer->uuid] = new ComputerPollingEntry();
    }
    else {
        pollingEntry = m_PollEntries[computer->uuid];
    }

    if (!pollingEntry->isActive()) {
        PcMonitorThread* thread = new PcMonitorThread(computer);
        connect(thread, &PcMonitorThread::computerStateChanged,
                this, &ComputerManager::handleComputerStateChanged);
        pollingEntry->setActiveThread(thread);
        thread->start();
    }
}

void ComputerManager::handleMdnsServiceResolved(MdnsPendingComputer* computer,
                                                QVector<QHostAddress>& addresses)
{
    QHostAddress v6Global = getBestGlobalAddressV6(addresses);
    bool added = false;

    // Add the host using the IPv4 address
    for (const QHostAddress& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            // NB: We don't just call addNewHost() here with v6Global because the IPv6
            // address may not be reachable (if the user hasn't installed the IPv6 helper yet
            // or if this host lacks outbound IPv6 capability). We want to add IPv6 even if
            // it's not currently reachable.
            addNewHost(NvAddress(address, computer->port()), true, NvAddress(v6Global, computer->port()));
            added = true;
            break;
        }
    }

    if (!added) {
        // If we get here, there wasn't an IPv4 address so we'll do it v6-only
        for (const QHostAddress& address : addresses) {
            if (address.protocol() == QAbstractSocket::IPv6Protocol) {
                // Use a link-local or site-local address for the "local address"
                if (address.isInSubnet(QHostAddress("fe80::"), 10) ||
                        address.isInSubnet(QHostAddress("fec0::"), 10) ||
                        address.isInSubnet(QHostAddress("fc00::"), 7)) {
                    addNewHost(NvAddress(address, computer->port()), true, NvAddress(v6Global, computer->port()));
                    break;
                }
            }
        }
    }

    m_PendingResolution.removeOne(computer);
    computer->deleteLater();
}

void ComputerManager::saveHost(NvComputer *computer)
{
    // If no serializable properties changed, don't bother saving hosts
    QMutexLocker lock(&m_DelayedFlushMutex);
    QReadLocker computerLock(&computer->lock);
    if (!m_LastSerializedHosts.value(computer->uuid).isEqualSerialized(*computer)) {
        // Queue a request for a delayed flush to QSettings outside of the lock
        computerLock.unlock();
        lock.unlock();
        saveHosts();
    }
}

void ComputerManager::handleComputerStateChanged(NvComputer* computer)
{
    emit computerStateChanged(computer);

    if (computer->pendingQuit && computer->currentGameId == 0) {
        computer->pendingQuit = false;
        emit quitAppCompleted(QVariant());
    }

    // SmartClassroom T13 — if we auto-added this host for a moonlight://
    // connect URL, stamp the broker token/host-id on it now that polling
    // has resolved its addresses.
    onPendingConnectStateChanged(computer);

    // SmartClassroom T14 — drive the auto-connect flow on every host state
    // change (resolution, online/offline, pairing, app-list refresh).
    scEvaluateAutoConnect(computer);

    // Save updates to this host
    saveHost(computer);
}

QString ComputerManager::pendingConnectKey(const QString& address, int port)
{
    return QStringLiteral("%1:%2").arg(address).arg(port);
}

void ComputerManager::stashPendingConnect(QString address, int port,
                                          QString connectToken, int hostId,
                                          QString brokerUrl)
{
    QMutexLocker lock(&s_StashedConnectsLock);
    if (!s_ActiveInstance.isNull()) {
        ComputerManager* inst = s_ActiveInstance.data();
        lock.unlock();
        // Active instance lives on the main thread (QML singleton lambda is
        // invoked there), and T13 callers (main.cpp ConnectRequested arm and
        // the QDesktopServices URL handler) are also on the main thread, so
        // a direct call is safe.
        inst->requestConnect(address, port, connectToken, hostId, brokerUrl);
        return;
    }
    s_StashedConnects.append(PendingConnect{address, port, connectToken, hostId, brokerUrl});
}

void ComputerManager::flushStashedConnects()
{
    QList<PendingConnect> drained;
    {
        QMutexLocker lock(&s_StashedConnectsLock);
        drained = s_StashedConnects;
        s_StashedConnects.clear();
    }
    for (const PendingConnect& pc : drained) {
        requestConnect(pc.address, pc.port, pc.connectToken, pc.hostId, pc.brokerUrl);
    }
}

void ComputerManager::requestConnect(QString address, int port,
                                     QString connectToken, int hostId,
                                     QString brokerUrl)
{
    if (address.isEmpty()) {
        qWarning() << "T13: requestConnect called with empty address — ignoring";
        return;
    }
    if (port <= 0) {
        port = DEFAULT_HTTP_PORT;
    }

    // First pass: see if we already know this host by any of its addresses.
    NvComputer* matched = nullptr;
    {
        QReadLocker lock(&m_Lock);
        for (NvComputer* c : m_KnownHosts) {
            QReadLocker cLock(&c->lock);
            auto matchesAddr = [&](const NvAddress& a) {
                return !a.isNull() && a.address() == address;
            };
            if (matchesAddr(c->manualAddress) || matchesAddr(c->localAddress) ||
                matchesAddr(c->remoteAddress) || matchesAddr(c->ipv6Address) ||
                matchesAddr(c->activeAddress)) {
                matched = c;
                break;
            }
        }
    }
    if (matched != nullptr) {
        {
            QWriteLocker cLock(&matched->lock);
            matched->pendingConnectToken = connectToken;
            matched->pendingHostId = hostId;
            matched->pendingBrokerUrl = brokerUrl;
            qInfo() << "T13: stamped pending connect on known host" << matched->name;
        }
        // T14 — a known host may already be online; computerStateChanged won't
        // fire just from stamping, so kick the auto-connect evaluator now.
        scEvaluateAutoConnect(matched);
        return;
    }

    // Otherwise queue the token for the address and trigger an addNewHost.
    {
        QMutexLocker lock(&m_PendingConnectLock);
        PendingConnect pc{address, port, connectToken, hostId, brokerUrl};
        m_PendingTokensByAddress[pendingConnectKey(address, port)] = pc;
    }
    qInfo() << "T13: requesting addNewHost for" << address << "port" << port
            << "host-id=" << hostId;
    addNewHost(NvAddress(address, static_cast<uint16_t>(port)), false);
}

void ComputerManager::onPendingConnectStateChanged(NvComputer* computer)
{
    if (computer == nullptr) {
        return;
    }
    QStringList candidateKeys;
    {
        QReadLocker cLock(&computer->lock);
        auto appendKey = [&](const NvAddress& a) {
            if (!a.isNull()) {
                candidateKeys.append(pendingConnectKey(a.address(), a.port()));
            }
        };
        appendKey(computer->manualAddress);
        appendKey(computer->localAddress);
        appendKey(computer->remoteAddress);
        appendKey(computer->ipv6Address);
        appendKey(computer->activeAddress);
    }
    PendingConnect found;
    bool foundOne = false;
    {
        QMutexLocker lock(&m_PendingConnectLock);
        for (const QString& key : candidateKeys) {
            auto it = m_PendingTokensByAddress.find(key);
            if (it != m_PendingTokensByAddress.end()) {
                found = it.value();
                foundOne = true;
                m_PendingTokensByAddress.erase(it);
                break;
            }
        }
    }
    if (foundOne) {
        QWriteLocker cLock(&computer->lock);
        computer->pendingConnectToken = found.connectToken;
        computer->pendingHostId = found.hostId;
        computer->pendingBrokerUrl = found.brokerUrl;
        qInfo() << "T13: stamped pending connect on host" << computer->name
                << "after polling resolved it";
    }
}

QVector<NvComputer*> ComputerManager::getComputers()
{
    QReadLocker lock(&m_Lock);

    // Return a sorted host list
    auto hosts = QVector<NvComputer*>::fromList(m_KnownHosts.values());
    std::stable_sort(hosts.begin(), hosts.end(), [](const NvComputer* host1, const NvComputer* host2) {
        return host1->name.toLower() < host2->name.toLower();
    });
    return hosts;
}

class DeferredHostDeletionTask : public QRunnable
{
public:
    DeferredHostDeletionTask(ComputerManager* cm, NvComputer* computer)
        : m_Computer(computer),
          m_ComputerManager(cm) {}

    void run()
    {
        ComputerPollingEntry* pollingEntry;

        // Only do the minimum amount of work while holding the writer lock.
        // We must release it before calling saveHosts().
        {
            QWriteLocker lock(&m_ComputerManager->m_Lock);

            pollingEntry = m_ComputerManager->m_PollEntries.take(m_Computer->uuid);

            m_ComputerManager->m_KnownHosts.remove(m_Computer->uuid);
        }

        // Persist the new host list with this computer deleted
        m_ComputerManager->saveHosts();

        // Delete the polling entry first. This will stop all polling threads too.
        delete pollingEntry;

        // Delete cached box art
        BoxArtManager::deleteBoxArt(m_Computer);

        // Finally, delete the computer itself. This must be done
        // last because the polling thread might be using it.
        delete m_Computer;
    }

private:
    NvComputer* m_Computer;
    ComputerManager* m_ComputerManager;
};

void ComputerManager::deleteHost(NvComputer* computer)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for the polling thread to die
    QThreadPool::globalInstance()->start(new DeferredHostDeletionTask(this, computer));
}

void ComputerManager::renameHost(NvComputer* computer, QString name)
{
    {
        QWriteLocker lock(&computer->lock);

        computer->name = name;
        computer->hasCustomName = true;
    }

    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::clientSideAttributeUpdated(NvComputer* computer)
{
    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::handleAboutToQuit()
{
    QReadLocker lock(&m_Lock);

    // Interrupt polling threads immediately, so they
    // avoid making additional requests while quitting
    for (ComputerPollingEntry* entry : m_PollEntries) {
        entry->interrupt();
    }
}

class PendingPairingTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingPairingTask(ComputerManager* computerManager, NvComputer* computer, QString pin,
                       int getServerCertTimeoutMs, bool headless)
        : m_ComputerManager(computerManager),
          m_Computer(computer),
          m_Pin(pin),
          m_GetServerCertTimeoutMs(getServerCertTimeoutMs),
          m_Headless(headless)
    {
        // SmartClassroom T14 — headless (auto-connect) pairing reports through a
        // private slot so the public pairingCompleted signal stays exclusively
        // the interactive-pairing channel. ComputerModel / CLI pair listen to
        // it and would otherwise raise a duplicate error dialog.
        if (m_Headless) {
            connect(this, &PendingPairingTask::pairingCompleted,
                    computerManager, &ComputerManager::scHandleHeadlessPairingResult);
        }
        else {
            connect(this, &PendingPairingTask::pairingCompleted,
                    computerManager, &ComputerManager::pairingCompleted);
        }
    }

signals:
    void pairingCompleted(NvComputer* computer, QString error);

private:
    void run()
    {
        NvPairingManager pairingManager(m_Computer);

        try {
           NvPairingManager::PairState result = pairingManager.pair(m_Computer->appVersion, m_Pin, m_Computer->serverCert,
                                                                     m_GetServerCertTimeoutMs);
           switch (result)
           {
           case NvPairingManager::PairState::PIN_WRONG:
               emit pairingCompleted(m_Computer, tr("The PIN from the PC didn't match. Please try again."));
               break;
           case NvPairingManager::PairState::FAILED:
               if (m_Computer->currentGameId != 0) {
                   emit pairingCompleted(m_Computer, tr("You cannot pair while a previous session is still running on the host PC. Quit any running games or reboot the host PC, then try pairing again."));
               }
               else {
                   emit pairingCompleted(m_Computer, tr("Pairing failed. Please try again."));
               }
               break;
           case NvPairingManager::PairState::ALREADY_IN_PROGRESS:
               emit pairingCompleted(m_Computer, tr("Another pairing attempt is already in progress."));
               break;
           case NvPairingManager::PairState::PAIRED:
               // Persist the newly pinned server certificate for this host
               m_ComputerManager->saveHost(m_Computer);

               emit pairingCompleted(m_Computer, nullptr);
               break;
           }
        } catch (const GfeHttpResponseException& e) {
            emit pairingCompleted(m_Computer, tr("GeForce Experience returned error: %1").arg(e.toQString()));
        } catch (const QtNetworkReplyException& e) {
            emit pairingCompleted(m_Computer, e.toQString());
        }
    }

    ComputerManager* m_ComputerManager;
    NvComputer* m_Computer;
    QString m_Pin;
    int m_GetServerCertTimeoutMs;
    bool m_Headless;
};

void ComputerManager::pairHost(NvComputer* computer, QString pin)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for pairing to complete
    PendingPairingTask* pairing = new PendingPairingTask(this, computer, pin, 0, false);
    QThreadPool::globalInstance()->start(pairing);
}

// === SmartClassroom T14 — moonlight:// automatic connect (pair + stream) =====
//
// A moonlight:// URL stamps a connect token + broker URL on an NvComputer
// (T13). scEvaluateAutoConnect() is then driven idempotently by host state
// changes: once the host is online it pairs headlessly via the Broker if
// needed, finds the "Desktop" app and hands a ready Session to QML to stream.
// The user types nothing. Any failure ends the flow and leaves the user on
// the standard PcView UI (the manual fallback).

// Headless pairing: runs the local pairing handshake and the Broker PIN relay
// concurrently. Whichever leg fails first ends the attempt via
// autoPairingCompleted(). A successful Broker relay only means the PIN reached
// the host — the local handshake's completion is authoritative for success.
void ComputerManager::beginHeadlessPairing(NvComputer* computer)
{
    if (computer == nullptr) {
        return;
    }

    QString connectToken;
    QString brokerUrl;
    {
        QReadLocker cLock(&computer->lock);
        connectToken = computer->pendingConnectToken;
        brokerUrl = computer->pendingBrokerUrl;
    }

    if (connectToken.isEmpty() || brokerUrl.isEmpty()) {
        qWarning() << "T14: beginHeadlessPairing missing connect token or broker URL for"
                   << computer->name;
        emit autoPairingCompleted(computer, false, QStringLiteral("missing_connect_context"));
        return;
    }

    if (m_AutoPairingComputer != nullptr) {
        qWarning() << "T14: headless pairing already in progress for"
                   << m_AutoPairingComputer->name << "— ignoring request for" << computer->name;
        emit autoPairingCompleted(computer, false, QStringLiteral("pairing_in_progress"));
        return;
    }

    const QString pin = generatePinString();
    m_AutoPairingComputer = computer;

    qInfo() << "T14: starting headless pairing for" << computer->name;

    // Start the local pairing handshake first so the host has an open pairing
    // session by the time the Broker relays the PIN to its /api/pin endpoint.
    // The bounded phase-1 timeout keeps a never-arriving PIN from hanging
    // Moonlight; the headless flag routes completion to scHandleHeadless-
    // PairingResult instead of the interactive pairingCompleted signal.
    PendingPairingTask* pairing = new PendingPairingTask(
        this, computer, pin, SC_HEADLESS_GETSERVERCERT_TIMEOUT_MS, true);
    QThreadPool::globalInstance()->start(pairing);

    // Then ask the Broker to relay this PIN to the host's Sunshine instance.
    m_ScBrokerClient->requestPairing(brokerUrl, connectToken, pin);
}

void ComputerManager::scHandleHeadlessPairingResult(NvComputer* computer, QString error)
{
    // Only headless completions reach this slot. Ignore an orphaned handshake
    // whose attempt was already abandoned (Broker relay failed first, or the
    // overall auto-connect timed out) — m_AutoPairingComputer is cleared then.
    if (computer != m_AutoPairingComputer) {
        return;
    }

    m_AutoPairingComputer = nullptr;

    if (error.isEmpty()) {
        qInfo() << "T14: headless pairing succeeded for" << computer->name;
        emit autoPairingCompleted(computer, true, QString());
    }
    else {
        qWarning() << "T14: headless pairing failed for" << computer->name << "—" << error;
        emit autoPairingCompleted(computer, false, error);
    }
}

void ComputerManager::scHandleBrokerPairingRelayed(bool success, QString reason)
{
    if (m_AutoPairingComputer == nullptr) {
        return;
    }

    if (success) {
        // The PIN reached the host. The local handshake is now authoritative —
        // wait for its completion via scHandleHeadlessPairingResult().
        qInfo() << "T14: Broker relayed PIN for" << m_AutoPairingComputer->name;
        return;
    }

    // The Broker relay failed, so no PIN will reach the host. End the attempt
    // now rather than waiting out the phase-1 timeout. The orphaned pairing
    // handshake will fail on its own (bounded by SC_HEADLESS_GETSERVERCERT_-
    // TIMEOUT_MS) and its later completion is ignored because
    // m_AutoPairingComputer is cleared here.
    NvComputer* computer = m_AutoPairingComputer;
    m_AutoPairingComputer = nullptr;
    qWarning() << "T14: Broker pairing relay failed for" << computer->name << "—" << reason;
    emit autoPairingCompleted(computer, false, QStringLiteral("broker_relay:") + reason);
}

void ComputerManager::scClearPendingConnect(NvComputer* computer)
{
    if (computer == nullptr) {
        return;
    }
    QWriteLocker cLock(&computer->lock);
    computer->pendingConnectToken.clear();
    computer->pendingHostId = 0;
    computer->pendingBrokerUrl.clear();
}

// Idempotent auto-connect driver. Called on every host state change and right
// after a connect token is stamped on a known host. Advances the flow when the
// host is ready; otherwise returns and waits for the next state change.
void ComputerManager::scEvaluateAutoConnect(NvComputer* computer)
{
    if (computer == nullptr) {
        return;
    }

    // Cheap first pass: is this even an auto-connect target we should act on?
    bool hasToken;
    bool online;
    QString hostName;
    {
        QReadLocker cLock(&computer->lock);
        hasToken = !computer->pendingConnectToken.isEmpty();
        online = computer->state == NvComputer::CS_ONLINE;
        hostName = computer->name;
    }
    if (!hasToken) {
        return;
    }
    if (m_AutoConnectComputer != nullptr && m_AutoConnectComputer != computer) {
        // A different auto-connect already owns the flow.
        return;
    }
    // Requiring CS_ONLINE also defers the very first evaluation safely past
    // ComputerManager construction: no host is online until polling (which
    // main.qml starts), so main.qml is always connected to our signals before
    // autoConnectStarted() can fire.
    if (!online) {
        return;
    }

    // It's a live target — snapshot pairing state and look for the app.
    bool paired;
    int appIndex = -1;
    NvApp chosenApp;
    {
        QReadLocker cLock(&computer->lock);
        paired = computer->pairState == NvComputer::PS_PAIRED;
        for (int i = 0; i < computer->appList.size(); i++) {
            if (computer->appList[i].name.toLower() == QLatin1String("desktop")) {
                appIndex = i;
                break;
            }
        }
        if (appIndex < 0 && !computer->appList.isEmpty()) {
            // Sunshine always exposes "Desktop"; this is a safety net for a
            // non-standard host config (T20 deployment keeps the default app).
            appIndex = 0;
        }
        if (appIndex >= 0) {
            chosenApp = computer->appList[appIndex];
        }
    }

    if (m_AutoConnectState == ScAcIdle) {
        m_AutoConnectComputer = computer;
        m_AutoConnectTimer->start(SC_AUTOCONNECT_TIMEOUT_MS);
        emit autoConnectStarted(hostName);
        qInfo() << "T14: auto-connect started for" << hostName
                << (paired ? "(already paired)" : "(needs pairing)");
        if (!paired) {
            m_AutoConnectState = ScAcPairing;
            emit autoConnectStageChanged(tr("Pairing with %1...").arg(hostName));
            beginHeadlessPairing(computer);
            return;
        }
        m_AutoConnectState = ScAcWaitingForApps;
        emit autoConnectStageChanged(tr("Loading app list..."));
        // Fall through to the ScAcWaitingForApps handling below.
    }

    if (m_AutoConnectState == ScAcWaitingForApps) {
        if (appIndex < 0) {
            // App list not fetched yet. The next poll populates it and
            // re-drives this evaluator; the timeout guard covers a stuck host.
            return;
        }
        scStartAutoStream(computer, chosenApp);
    }
}

void ComputerManager::scStartAutoStream(NvComputer* computer, NvApp app)
{
    m_AutoConnectTimer->stop();
    m_AutoConnectState = ScAcIdle;
    m_AutoConnectComputer = nullptr;

    // Consume the token so a subsequent poll's computerStateChanged doesn't
    // re-trigger the flow while we're already streaming.
    scClearPendingConnect(computer);

    qInfo() << "T14: auto-connect streaming" << app.name << "from" << computer->name;
    Session* session = new Session(computer, app, m_Prefs);
    emit autoStreamSessionReady(app.name, session);
}

void ComputerManager::scFailAutoConnect(QString message)
{
    m_AutoConnectTimer->stop();
    NvComputer* computer = m_AutoConnectComputer;
    m_AutoConnectState = ScAcIdle;
    m_AutoConnectComputer = nullptr;

    // Drop the token so the failed flow doesn't loop on every future poll.
    scClearPendingConnect(computer);

    qWarning() << "T14: auto-connect failed —" << message;
    emit autoConnectFailed(message);
}

void ComputerManager::scHandleAutoPairingCompleted(NvComputer* computer, bool success, QString reason)
{
    if (m_AutoConnectState != ScAcPairing || computer != m_AutoConnectComputer) {
        return;
    }
    if (!success) {
        scFailAutoConnect(tr("Pairing failed (%1)").arg(reason));
        return;
    }
    qInfo() << "T14: auto-connect paired" << computer->name << "— waiting for app list";
    m_AutoConnectState = ScAcWaitingForApps;
    emit autoConnectStageChanged(tr("Loading app list..."));
    // pairState/appList refresh on the next poll; re-drive now in case a poll
    // already landed.
    scEvaluateAutoConnect(computer);
}

void ComputerManager::scHandleAutoConnectTimeout()
{
    if (m_AutoConnectState == ScAcIdle) {
        return;
    }
    scFailAutoConnect(tr("Timed out connecting to the PC"));
}

void ComputerManager::cancelAutoConnect()
{
    if (m_AutoConnectState == ScAcIdle) {
        return;
    }
    m_AutoConnectTimer->stop();
    NvComputer* computer = m_AutoConnectComputer;
    m_AutoConnectState = ScAcIdle;
    m_AutoConnectComputer = nullptr;
    scClearPendingConnect(computer);
    qInfo() << "T14: auto-connect canceled by user";
    // No autoConnectFailed — the QML popup's Cancel handler closes itself.
}

class PendingQuitTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingQuitTask(ComputerManager* computerManager, NvComputer* computer)
        : m_Computer(computer)
    {
        connect(this, &PendingQuitTask::quitAppFailed,
                computerManager, &ComputerManager::quitAppCompleted);
    }

signals:
    void quitAppFailed(QString error);

private:
    void run()
    {
        NvHTTP http(m_Computer);

        try {
            if (m_Computer->currentGameId != 0) {
                http.quitApp();
            }
        } catch (const GfeHttpResponseException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            if (e.getStatusCode() == 599) {
                // 599 is a special code we make a custom message for
                emit quitAppFailed(tr("The running game wasn't started by this PC. "
                                      "You must quit the game on the host PC manually or use the device that originally started the game."));
            }
            else {
                emit quitAppFailed(e.toQString());
            }
        } catch (const QtNetworkReplyException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            emit quitAppFailed(e.toQString());
        }
    }

    NvComputer* m_Computer;
};

void ComputerManager::quitRunningApp(NvComputer* computer)
{
    QWriteLocker lock(&computer->lock);
    computer->pendingQuit = true;

    PendingQuitTask* quit = new PendingQuitTask(this, computer);
    QThreadPool::globalInstance()->start(quit);
}

void ComputerManager::stopPollingAsync()
{
    QWriteLocker lock(&m_Lock);

    Q_ASSERT(m_PollingRef > 0);
    if (--m_PollingRef > 0) {
        return;
    }

    // Delete machines that haven't been resolved yet
    while (!m_PendingResolution.isEmpty()) {
        MdnsPendingComputer* computer = m_PendingResolution.first();
        computer->deleteLater();
        m_PendingResolution.removeFirst();
    }

    // Delete the browser and server to stop discovery and refresh polling
    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;
    m_MdnsServer.reset();

    // Interrupt all threads, but don't wait for them to terminate
    for (ComputerPollingEntry* entry : m_PollEntries) {
        entry->interrupt();
    }
}

void ComputerManager::addNewHostManually(QString address)
{
    QUrl url = QUrl::fromUserInput("moonlight://" + address);
    if (url.isValid() && !url.host().isEmpty() && url.scheme() == "moonlight") {
        // If there wasn't a port specified, use the default
        addNewHost(NvAddress(url.host(), url.port(DEFAULT_HTTP_PORT)), false);
    }
    else {
        emit computerAddCompleted(false, false);
    }
}

class PendingAddTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingAddTask(ComputerManager* computerManager, NvAddress address, NvAddress mdnsIpv6Address, bool mdns)
        : m_ComputerManager(computerManager),
          m_Address(address),
          m_MdnsIpv6Address(mdnsIpv6Address),
          m_Mdns(mdns),
          m_AboutToQuit(false)
    {
        connect(this, &PendingAddTask::computerAddCompleted,
                computerManager, &ComputerManager::computerAddCompleted);
        connect(this, &PendingAddTask::computerStateChanged,
                computerManager, &ComputerManager::handleComputerStateChanged);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &PendingAddTask::handleAboutToQuit);
    }

signals:
    void computerAddCompleted(QVariant success, QVariant detectedPortBlocking);

    void computerStateChanged(NvComputer* computer);

private:
    void handleAboutToQuit()
    {
        m_AboutToQuit = true;
    }

    QString fetchServerInfo(NvHTTP& http)
    {
        QString serverInfo;

        // Do nothing if we're quitting
        if (m_AboutToQuit) {
            return QString();
        }

        try {
            // There's a race condition between GameStream servers reporting presence over
            // mDNS and the HTTPS server being ready to respond to our queries. To work
            // around this issue, we will issue the request again after a few seconds if
            // we see a ServiceUnavailableError error.
            try {
                serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
            } catch (const QtNetworkReplyException& e) {
                if (e.getError() == QNetworkReply::ServiceUnavailableError) {
                    qWarning() << "Retrying request in 5 seconds after ServiceUnavailableError";
                    QThread::sleep(5);
                    serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
                    qInfo() << "Retry successful";
                }
                else {
                    // Rethrow other errors
                    throw e;
                }
            }
            return serverInfo;
        } catch (...) {
            if (!m_Mdns) {
                unsigned int portTestResult;

                if (m_ComputerManager->m_Prefs->detectNetworkBlocking) {
                    // We failed to connect to the specified PC. Let's test to make sure this network
                    // isn't blocking Moonlight, so we can tell the user about it.
                    portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443,
                                                              ML_PORT_FLAG_TCP_47984 | ML_PORT_FLAG_TCP_47989);
                }
                else {
                    portTestResult = 0;
                }

                emit computerAddCompleted(false, portTestResult != 0 && portTestResult != ML_TEST_RESULT_INCONCLUSIVE);
            }
            return QString();
        }
    }

    void run()
    {
        NvHTTP http(m_Address, 0, QSslCertificate());

        qInfo() << "Processing new PC at" << m_Address.toString() << "from" << (m_Mdns ? "mDNS" : "user") << "with IPv6 address" << m_MdnsIpv6Address.toString();

        // Perform initial serverinfo fetch over HTTP since we don't know which cert to use
        QString serverInfo = fetchServerInfo(http);
        if (serverInfo.isEmpty() && !m_MdnsIpv6Address.isNull()) {
            // Retry using the global IPv6 address if the IPv4 or link-local IPv6 address fails
            http.setAddress(m_MdnsIpv6Address);
            serverInfo = fetchServerInfo(http);
        }
        if (serverInfo.isEmpty()) {
            return;
        }

        // Create initial newComputer using HTTP serverinfo with no pinned cert
        NvComputer* newComputer = new NvComputer(http, serverInfo);

        // Check if we have a record of this host UUID to pull the pinned cert
        NvComputer* existingComputer;
        {
            QReadLocker lock(&m_ComputerManager->m_Lock);
            existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            if (existingComputer != nullptr) {
                http.setServerCert(existingComputer->serverCert);
            }
        }

        // Fetch serverinfo again over HTTPS with the pinned cert
        if (existingComputer != nullptr) {
            Q_ASSERT(http.httpsPort() != 0);
            serverInfo = fetchServerInfo(http);
            if (serverInfo.isEmpty()) {
                return;
            }

            // Update the polled computer with the HTTPS information
            NvComputer httpsComputer(http, serverInfo);
            newComputer->update(httpsComputer);
        }

        // Update addresses depending on the context
        if (m_Mdns) {
            // Only update local address if we actually reached the PC via this address.
            // If we reached it via the IPv6 address after the local address failed,
            // don't store the non-working local address.
            if (http.address() == m_Address) {
                newComputer->localAddress = m_Address;
            }

            // Get the WAN IP address using STUN if we're on mDNS over IPv4
            if (QHostAddress(newComputer->localAddress.address()).protocol() == QAbstractSocket::IPv4Protocol) {
                quint32 addr;
                int err = LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &addr);
                if (err == 0) {
                    newComputer->setRemoteAddress(QHostAddress(qFromBigEndian(addr)));
                }
                else {
                    qWarning() << "STUN failed to get WAN address:" << err;
                }
            }

            if (!m_MdnsIpv6Address.isNull()) {
                Q_ASSERT(QHostAddress(m_MdnsIpv6Address.address()).protocol() == QAbstractSocket::IPv6Protocol);
                newComputer->ipv6Address = m_MdnsIpv6Address;
            }
        }
        else {
            newComputer->manualAddress = m_Address;
        }

        QHostAddress hostAddress(m_Address.address());
        bool addressIsSiteLocalV4 =
                hostAddress.isInSubnet(QHostAddress("10.0.0.0"), 8) ||
                hostAddress.isInSubnet(QHostAddress("172.16.0.0"), 12) ||
                hostAddress.isInSubnet(QHostAddress("192.168.0.0"), 16);

        {
            // Check if this PC already exists using opportunistic read lock
            m_ComputerManager->m_Lock.lockForRead();
            NvComputer* existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);

            // If it doesn't already exist, convert to a write lock in preparation for updating.
            //
            // NB: ComputerManager's lock protects the host map itself, not the elements inside.
            // Those are protected by their individual locks. Since we only mutate the map itself
            // when the PC doesn't exist, we need the lock in write-mode for that case only.
            if (existingComputer == nullptr) {
                m_ComputerManager->m_Lock.unlock();
                m_ComputerManager->m_Lock.lockForWrite();

                // Since we had to unlock to lock for write, someone could have raced and added
                // this PC before us. We have to check again whether it already exists.
                existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            }

            if (existingComputer != nullptr) {
                // Fold it into the existing PC
                bool changed = existingComputer->update(*newComputer);
                delete newComputer;

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // For non-mDNS clients, let them know it succeeded
                if (!m_Mdns) {
                    emit computerAddCompleted(true, false);
                }

                // Tell our client if something changed
                if (changed) {
                    qInfo() << existingComputer->name << "is now at" << existingComputer->activeAddress.toString();
                    emit computerStateChanged(existingComputer);
                }
            }
            else {
                // Store this in our active sets
                m_ComputerManager->m_KnownHosts[newComputer->uuid] = newComputer;

                // Start polling if enabled (write lock required)
                m_ComputerManager->startPollingComputer(newComputer);

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // If this wasn't added via mDNS but it is a RFC 1918 IPv4 address and not a VPN,
                // go ahead and do the STUN request now to populate an external address.
                if (!m_Mdns && addressIsSiteLocalV4 && newComputer->getActiveAddressReachability() != NvComputer::RI_VPN) {
                    quint32 addr;
                    int err = LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &addr);
                    if (err == 0) {
                        newComputer->setRemoteAddress(QHostAddress(qFromBigEndian(addr)));
                    }
                    else {
                        qWarning() << "STUN failed to get WAN address:" << err;
                    }
                }

                // For non-mDNS clients, let them know it succeeded
                if (!m_Mdns) {
                    emit computerAddCompleted(true, false);
                }

                // Tell our client about this new PC
                emit computerStateChanged(newComputer);
            }
        }
    }

    ComputerManager* m_ComputerManager;
    NvAddress m_Address;
    NvAddress m_MdnsIpv6Address;
    bool m_Mdns;
    bool m_AboutToQuit;
};

void ComputerManager::addNewHost(NvAddress address, bool mdns, NvAddress mdnsIpv6Address)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for serverinfo query to complete
    PendingAddTask* addTask = new PendingAddTask(this, address, mdnsIpv6Address, mdns);
    QThreadPool::globalInstance()->start(addTask);
}

// TODO: Use QRandomGenerator when we drop Qt 5.9 support
QString ComputerManager::generatePinString()
{
    std::uniform_int_distribution<int> dist(0, 9999);
    std::random_device rd;
    std::mt19937 engine(rd());

    return QString::asprintf("%04u", dist(engine));
}

#include "computermanager.moc"
