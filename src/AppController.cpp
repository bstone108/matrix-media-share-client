#include "AppController.h"

#include "AppUpdater.h"
#include "MatrixClientBackend.h"
#include "ProcessMatrixClientBackend.h"
#include "UpdateUtilities.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {
QString defaultDestinationRootPath()
{
    QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty()) {
        downloads = QDir::homePath() + QStringLiteral("/Downloads");
    }
    return downloads + QStringLiteral("/Matrix Media Share Client");
}

QString releaseSummaryText(const UpdateCheckState &state)
{
    if (state.latestVersion.trimmed().isEmpty()) {
        return QStringLiteral("No published GitHub release yet.");
    }

    QString label = state.latestVersion.trimmed();
    const QString releaseName = state.latestReleaseName.trimmed();
    if (!releaseName.isEmpty() && releaseName != label) {
        label = QStringLiteral("%1 (%2)").arg(releaseName, label);
    }

    if (state.latestPublishedAt.isValid()) {
        return QStringLiteral("%1, published %2")
            .arg(label, QLocale().toString(state.latestPublishedAt.toLocalTime(), QLocale::ShortFormat));
    }
    return label;
}

QString replyMessage(const QByteArray &payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return {};
    }
    return document.object().value(QStringLiteral("message")).toString().trimmed();
}
}

AppController::AppController(QObject *parent)
    : QObject(parent)
    , database_(paths_.databasePath())
    , secretStore_(paths_)
    , backend_(std::make_unique<ProcessMatrixClientBackend>(paths_, this))
    , refreshTimer_(new QTimer(this))
    , updateNetworkManager_(new QNetworkAccessManager(this))
    , updater_(new AppUpdater(paths_, this))
{
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, &AppController::refresh);

    backend_->setRuntimeChangedCallback([this](const BotRuntimeSnapshot &runtime) {
        runtime_ = runtime;
        updateRefreshTimer();
        scheduleRefresh();
    });
    backend_->setBackendErrorCallback([this](const QString &message) {
        if (message.isEmpty()) {
            return;
        }
        lastErrorMessage_ = message;
        logError(QStringLiteral("backend"), message);
        updateRefreshTimer();
        scheduleRefresh();
    });
    updater_->setLogCallbacks(
        [this](const QString &message) {
            logInfo(QStringLiteral("updates"), message);
        },
        [this](const QString &message) {
            logWarning(QStringLiteral("updates"), message);
        },
        [this](const QString &message) {
            logError(QStringLiteral("updates"), message);
        });
    connect(updater_, &AppUpdater::stateChanged, this, [this]() {
        emit stateChanged();
    });
    connect(updater_, &AppUpdater::stagedUpdateReady, this, [this](const QString &version, const bool forcePrompt) {
        updateCheckState_.pendingInstallVersion = version;
        updateCheckState_.pendingInstallPath = updater_->stagedPayloadPath();
        persistUpdateCheckState();
        if (forcePrompt || UpdateUtilities::shouldNagForVersion(version, currentVersion(), updateCheckState_.lastNotifiedVersion)) {
            emit stagedUpdateReady(version);
        } else {
            updater_->scheduleInstallOnExit();
        }
        emit stateChanged();
    });
    connect(updater_, &AppUpdater::downloadLinkNotice, this, [this](const QString &version, const QUrl &pageUrl, const QString &reason, const bool forcePrompt) {
        if (forcePrompt || UpdateUtilities::shouldNagForVersion(version, currentVersion(), updateCheckState_.lastNotifiedVersion)) {
            emit updateDownloadLinkNotice(version, pageUrl.toString(), reason);
        }
        emit stateChanged();
    });
    connect(updater_, &AppUpdater::updateFailed, this, [this](const QString &message) {
        lastErrorMessage_ = message;
        emit stateChanged();
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
        shutdownBackendForExit();
        if (updater_ != nullptr && updater_->hasStagedUpdate()) {
            updater_->startPendingInstallHelper(false);
        }
    });
}

AppController::~AppController()
{
    shutdownBackendForExit();
}

void AppController::initialize()
{
    settings_ = database_.loadSettings(defaultDestinationRootPath());
    password_ = secretStore_.loadPassword();
    updateCheckState_ = database_.loadUpdateCheckState();
    updater_->restoreFromState(updateCheckState_);
    logInfo(QStringLiteral("app"), QStringLiteral("Application initialized."));
    logInfo(
        QStringLiteral("settings"),
        QStringLiteral("Settings database: %1 | Secret store: %2")
            .arg(QDir::toNativeSeparators(paths_.databasePath()), QDir::toNativeSeparators(paths_.secretStorePath())));
    if (!database_.lastErrorText().trimmed().isEmpty()) {
        lastErrorMessage_ = QStringLiteral("Settings storage error: %1").arg(database_.lastErrorText().trimmed());
        logError(QStringLiteral("settings"), lastErrorMessage_);
    }
    refresh();
    updateRefreshTimer();

    if (settings_.desiredPowerState) {
        togglePower(true);
    }

    if (UpdateUtilities::isUpdateCheckDue(updateCheckState_.lastCheckedAt)) {
        QTimer::singleShot(0, this, [this]() {
            checkForUpdates(false);
        });
    }
}

void AppController::refresh()
{
    refreshQueued_ = false;
    rooms_ = database_.fetchRooms();
    sharedItems_ = database_.fetchSharedItems();
    jobs_ = database_.fetchJobs();
    waitingQueueCount_ = database_.fetchWaitingJobCount();
    emit stateChanged();
}

void AppController::shutdownBackendForExit()
{
    if (backend_ == nullptr) {
        return;
    }

    if (refreshTimer_ != nullptr) {
        refreshTimer_->stop();
    }

    backend_.reset();
    runtime_ = BotRuntimeSnapshot {};
    runtime_.connectionState = ConnectionState::Stopped;
}

const AppSettings &AppController::settings() const
{
    return settings_;
}

const QString &AppController::password() const
{
    return password_;
}

const BotRuntimeSnapshot &AppController::runtime() const
{
    return runtime_;
}

const QVector<RoomRecord> &AppController::rooms() const
{
    return rooms_;
}

QVector<AttachmentDiscovery> AppController::fetchDiscoveriesPage(const QString &roomId, const int offset, const int limit) const
{
    return database_.fetchDiscoveriesPage(roomId, offset, limit);
}

int AppController::discoveryCount(const QString &roomId) const
{
    return database_.fetchDiscoveryCount(roomId);
}

QVector<RoomRecord> AppController::joinedRooms() const
{
    QVector<RoomRecord> result;
    for (const RoomRecord &room : rooms_) {
        if (room.membership == QStringLiteral("joined") && !room.isSpace) {
            result.append(room);
        }
    }
    return result;
}

QVector<RoomRecord> AppController::joinedSpaces() const
{
    QVector<RoomRecord> result;
    for (const RoomRecord &room : rooms_) {
        if (room.membership == QStringLiteral("joined") && room.isSpace) {
            result.append(room);
        }
    }
    return result;
}

const QVector<SharedItemRecord> &AppController::sharedItems() const
{
    return sharedItems_;
}

const QVector<DownloadJobRecord> &AppController::jobs() const
{
    return jobs_;
}

QVector<ActivityLogEntry> AppController::fetchLogsPage(const int offset, const int limit, const bool problemsOnly) const
{
    return database_.fetchLogsPage(offset, limit, problemsOnly);
}

int AppController::logCount(const bool problemsOnly) const
{
    return database_.fetchLogCount(problemsOnly);
}

int AppController::waitingQueueCount() const
{
    return waitingQueueCount_;
}

QStringList AppController::aliasHistory(const QString &roomId) const
{
    if (roomId.isEmpty()) {
        return {};
    }
    return database_.aliasHistory(roomId);
}

QString AppController::connectionStatusText() const
{
    return connectionStateTitle(runtime_.connectionState);
}

QString AppController::lastErrorMessage() const
{
    return lastErrorMessage_;
}

QString AppController::currentVersion() const
{
    return QCoreApplication::applicationVersion();
}

const UpdateCheckState &AppController::updateCheckState() const
{
    return updateCheckState_;
}

bool AppController::isUpdateCheckInProgress() const
{
    return updateCheckInProgress_ || (updater_ != nullptr && updater_->isBusy());
}

bool AppController::updateAvailable() const
{
    return UpdateUtilities::isNewerVersion(updateCheckState_.latestVersion, currentVersion());
}

bool AppController::hasStagedUpdate() const
{
    return updater_ != nullptr && updater_->hasStagedUpdate();
}

QString AppController::updateStatusText() const
{
    if (updater_ != nullptr) {
        const QString updaterText = updater_->statusText(currentVersion(), updateCheckState_);
        if (!updaterText.isEmpty()) {
            return updaterText;
        }
    }
    if (updateCheckInProgress_) {
        return QStringLiteral("Checking GitHub releases...");
    }
    if (!updateCheckState_.lastError.trimmed().isEmpty()) {
        return QStringLiteral("Last update check failed: %1").arg(updateCheckState_.lastError.trimmed());
    }
    if (updateCheckState_.latestVersion.trimmed().isEmpty()) {
        if (!updateCheckState_.lastCheckedAt.isValid()) {
            return QStringLiteral("Update check pending.");
        }
        return QStringLiteral("No published GitHub release yet.");
    }

    const int comparison = UpdateUtilities::compareVersionStrings(updateCheckState_.latestVersion, currentVersion());
    if (comparison > 0) {
        return QStringLiteral("Update available: %1").arg(updateCheckState_.latestVersion);
    }
    if (comparison < 0) {
        return QStringLiteral("This build is newer than the latest published release.");
    }
    return QStringLiteral("Up to date.");
}

QString AppController::latestReleaseSummaryText() const
{
    return releaseSummaryText(updateCheckState_);
}

QString AppController::latestReleasePageUrl() const
{
    if (!updateCheckState_.latestReleaseUrl.trimmed().isEmpty()) {
        return updateCheckState_.latestReleaseUrl.trimmed();
    }
    return QString::fromLatin1(UpdateUtilities::kGithubReleasesPageUrl);
}

QString AppController::settingsDatabasePath() const
{
    return paths_.databasePath();
}

QString AppController::secretStorePath() const
{
    return paths_.secretStorePath();
}

void AppController::recordInfo(const QString &subsystem, const QString &message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    logInfo(subsystem, message.trimmed());
    scheduleRefresh();
}

void AppController::recordWarning(const QString &subsystem, const QString &message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    logWarning(subsystem, message.trimmed());
    scheduleRefresh();
}

void AppController::recordError(const QString &subsystem, const QString &message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    logError(subsystem, message.trimmed());
    scheduleRefresh();
}

void AppController::togglePower(const bool enabled)
{
    if (!enabled) {
        settings_.desiredPowerState = false;
        if (!database_.saveSettings(settings_)) {
            lastErrorMessage_ = QStringLiteral("Failed to save power state: %1").arg(database_.lastErrorText().trimmed());
            logError(QStringLiteral("settings"), lastErrorMessage_);
        }
        QString errorMessage;
        if (backend_->stop(runtime_, errorMessage)) {
            logInfo(QStringLiteral("matrix"), QStringLiteral("Power off requested."));
        } else {
            lastErrorMessage_ = errorMessage;
            logError(QStringLiteral("matrix"), errorMessage);
        }
        updateRefreshTimer();
        refresh();
        return;
    }

    const QString validationError = startupValidationError(settings_, password_);
    if (!validationError.isEmpty()) {
        settings_.desiredPowerState = false;
        if (!database_.saveSettings(settings_)) {
            lastErrorMessage_ = QStringLiteral("Failed to save power state: %1").arg(database_.lastErrorText().trimmed());
            logError(QStringLiteral("settings"), lastErrorMessage_);
        }
        lastErrorMessage_ = validationError;
        logWarning(QStringLiteral("matrix"), validationError);
        emit userNoticeRequested(QStringLiteral("Connection Details Needed"), validationError);
        updateRefreshTimer();
        refresh();
        return;
    }

    settings_.desiredPowerState = true;
    if (!database_.saveSettings(settings_)) {
        lastErrorMessage_ = QStringLiteral("Failed to save power state: %1").arg(database_.lastErrorText().trimmed());
        logError(QStringLiteral("settings"), lastErrorMessage_);
    }

    QString errorMessage;
    BotRuntimeSnapshot runtime;
    runtime.connectionState = ConnectionState::Starting;
    if (backend_->start(settings_, password_, runtime, errorMessage)) {
        runtime_ = runtime;
        logInfo(QStringLiteral("matrix"), QStringLiteral("Power on requested."));
    } else {
        settings_.desiredPowerState = false;
        if (!database_.saveSettings(settings_)) {
            logError(
                QStringLiteral("settings"),
                QStringLiteral("Failed to save power state after startup failure: %1").arg(database_.lastErrorText().trimmed()));
        }
        runtime_ = runtime;
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("matrix"), errorMessage);
        const QString noticeMessage = errorMessage.trimmed().isEmpty()
            ? QStringLiteral("The client could not connect with the current account details.")
            : QStringLiteral("The client could not connect with the current account details.\n\n%1").arg(errorMessage.trimmed());
        emit userNoticeRequested(QStringLiteral("Connection Failed"), noticeMessage);
    }

    updateRefreshTimer();
    refresh();
}

bool AppController::saveSettings(const AppSettings &settings, const QString &password)
{
    settings_ = settings;
    password_ = password;
    if (!database_.saveSettings(settings_)) {
        lastErrorMessage_ = QStringLiteral("Failed to save settings: %1").arg(database_.lastErrorText().trimmed());
        logError(QStringLiteral("settings"), lastErrorMessage_);
        updateRefreshTimer();
        refresh();
        return false;
    }

    secretStore_.savePassword(password_);

    QString errorMessage;
    if ((runtime_.connectionState != ConnectionState::Stopped || settings_.desiredPowerState)
        && !backend_->saveSettings(settings_, password_, errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("settings"), errorMessage);
    } else {
        logInfo(QStringLiteral("settings"), QStringLiteral("Settings saved."));
    }

    updateRefreshTimer();
    refresh();
    return true;
}

QString AppController::startupValidationError(const AppSettings &settings, const QString &password) const
{
    if (settings.homeserverUrl.trimmed().isEmpty()) {
        return QStringLiteral("Enter your homeserver in Settings before starting the client.");
    }
    if (settings.username.trimmed().isEmpty()) {
        return QStringLiteral("Enter your Matrix username in Settings before starting the client.");
    }

    const StoredSession storedSession = secretStore_.loadSession();
    if (password.trimmed().isEmpty() && storedSession.accessToken.trimmed().isEmpty()) {
        return QStringLiteral("Enter your password in Settings before starting the client.");
    }

    return QString();
}

void AppController::resetHistoryScans()
{
    const bool backendActive = runtime_.connectionState != ConnectionState::Stopped || settings_.desiredPowerState;
    bool ok = false;
    if (backendActive) {
        QString errorMessage;
        ok = backend_->resetHistoryScans(errorMessage);
        if (!ok) {
            lastErrorMessage_ = errorMessage;
            logError(QStringLiteral("history"), errorMessage);
        }
    } else {
        ok = database_.resetHistoryScansForFullRescan();
    }

    if (ok) {
        logInfo(
            QStringLiteral("history"),
            QStringLiteral("Reset room history, discoveries, and queued jobs for a full rescan."));
    } else {
        logError(QStringLiteral("history"), QStringLiteral("Failed to reset room history state."));
    }
    refresh();
}

void AppController::retryFailedJob(const qint64 jobId)
{
    if (database_.retryFailedJob(jobId)) {
        logInfo(QStringLiteral("queue"), QStringLiteral("Retried permanently failed job %1.").arg(jobId));
    }
    refresh();
}

void AppController::retryAllFailedJobs()
{
    const int count = database_.retryAllFailedJobs();
    if (count > 0) {
        logInfo(QStringLiteral("queue"), QStringLiteral("Retried %1 permanently failed jobs.").arg(count));
    }
    refresh();
}

void AppController::clearFailedJob(const qint64 jobId)
{
    if (database_.clearFailedJob(jobId)) {
        logInfo(QStringLiteral("queue"), QStringLiteral("Cleared permanently failed job %1.").arg(jobId));
    }
    refresh();
}

void AppController::clearAllFailedJobs()
{
    const int count = database_.clearAllFailedJobs();
    if (count > 0) {
        logInfo(QStringLiteral("queue"), QStringLiteral("Cleared %1 permanently failed jobs.").arg(count));
    }
    refresh();
}

void AppController::queueDiscoveryDownload(const QString &roomId, const QString &eventId)
{
    if (database_.queueDiscoveryDownload(roomId, eventId)) {
        logInfo(QStringLiteral("transfers"), QStringLiteral("Queued media from %1 in %2.").arg(eventId, roomId));
    } else {
        logWarning(
            QStringLiteral("transfers"),
            QStringLiteral("Media from %1 was already queued or could not be added to the download queue.").arg(eventId));
    }
    refresh();
}

void AppController::openDiscovery(const QString &roomId, const QString &eventId)
{
    if (roomId.trimmed().isEmpty() || eventId.trimmed().isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!backend_->openDiscovery(roomId, eventId, errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("browser"), errorMessage);
    } else {
        logInfo(QStringLiteral("browser"), QStringLiteral("Opening %1 from %2.").arg(eventId, roomId));
    }
    refresh();
}

void AppController::focusRoom(const QString &roomId)
{
    const QString trimmedRoomId = roomId.trimmed();
    if (trimmedRoomId.isEmpty() || runtime_.connectionState != ConnectionState::Running) {
        return;
    }

    QString errorMessage;
    if (!backend_->focusRoom(trimmedRoomId, errorMessage) && !errorMessage.trimmed().isEmpty()) {
        lastErrorMessage_ = errorMessage;
        logWarning(QStringLiteral("rooms"), QStringLiteral("Unable to prioritize %1: %2").arg(trimmedRoomId, errorMessage));
        refresh();
    }
}

void AppController::shareLocalFile(const QString &roomId, const QString &filePath)
{
    if (roomId.trimmed().isEmpty() || filePath.trimmed().isEmpty()) {
        return;
    }
    QString errorMessage;
    if (!backend_->shareLocalFile(roomId, filePath, errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("share"), errorMessage);
    } else {
        logInfo(QStringLiteral("share"), QStringLiteral("Queued %1 for sharing into %2.").arg(filePath, roomId));
    }
    refresh();
}

void AppController::shareLocalFiles(const QString &roomId, const QStringList &filePaths)
{
    if (roomId.trimmed().isEmpty() || filePaths.isEmpty()) {
        return;
    }

    QStringList cleanedPaths;
    for (const QString &filePath : filePaths) {
        const QString trimmed = filePath.trimmed();
        if (!trimmed.isEmpty() && !cleanedPaths.contains(trimmed)) {
            cleanedPaths.append(trimmed);
        }
    }
    if (cleanedPaths.isEmpty()) {
        return;
    }

    int queuedCount = 0;
    QStringList failures;
    for (const QString &filePath : cleanedPaths) {
        QString errorMessage;
        if (backend_->shareLocalFile(roomId, filePath, errorMessage)) {
            queuedCount += 1;
        } else if (!errorMessage.trimmed().isEmpty()) {
            failures.append(errorMessage.trimmed());
            logError(QStringLiteral("share"), errorMessage);
        }
    }

    if (queuedCount > 0) {
        logInfo(
            QStringLiteral("share"),
            QStringLiteral("Queued %1 file(s) for sharing into %2.").arg(queuedCount).arg(roomId));
    }
    if (!failures.isEmpty()) {
        lastErrorMessage_ = failures.join(QStringLiteral("\n"));
    }
    refresh();
}

void AppController::importIpfsLink(const QString &link)
{
    if (link.trimmed().isEmpty()) {
        return;
    }
    QString errorMessage;
    if (!backend_->importIpfsLink(link, errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("ipfs"), errorMessage);
    } else {
        logInfo(QStringLiteral("ipfs"), QStringLiteral("Imported IPFS link."));
    }
    refresh();
}

void AppController::deleteSharedItem(const QString &sha256)
{
    if (sha256.trimmed().isEmpty()) {
        return;
    }
    QString errorMessage;
    if (!backend_->deleteSharedItem(sha256.trimmed(), errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("shared-files"), errorMessage);
    } else {
        logInfo(QStringLiteral("shared-files"), QStringLiteral("Queued cleanup for shared item %1.").arg(sha256.trimmed()));
    }
    refresh();
}

void AppController::refreshCatalog()
{
    QString errorMessage;
    if (!backend_->refreshCatalog(errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("browser"), errorMessage);
    }
    refresh();
}

void AppController::joinRoom(const QString &roomIdOrAlias)
{
    const QString trimmed = roomIdOrAlias.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (backend_->joinRoom(trimmed, errorMessage)) {
        logInfo(QStringLiteral("rooms"), QStringLiteral("Joined room %1.").arg(trimmed));
    } else {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("rooms"), QStringLiteral("Join failed for %1: %2").arg(trimmed, errorMessage));
    }
    refresh();
}

void AppController::leaveRoom(const QString &roomId)
{
    QString errorMessage;
    if (backend_->leaveRoom(roomId, errorMessage)) {
        logInfo(QStringLiteral("rooms"), QStringLiteral("Left room %1.").arg(roomId));
    } else {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("rooms"), QStringLiteral("Leave failed for %1: %2").arg(roomId, errorMessage));
    }
    refresh();
}

void AppController::requestVerification()
{
    QString errorMessage;
    if (!backend_->requestVerification(errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logWarning(QStringLiteral("verification"), errorMessage);
        emit stateChanged();
    }
}

void AppController::startSasVerification()
{
    QString errorMessage;
    if (!backend_->startSasVerification(errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logWarning(QStringLiteral("verification"), errorMessage);
        emit stateChanged();
    }
}

void AppController::approveVerification()
{
    QString errorMessage;
    if (!backend_->approveVerification(errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logWarning(QStringLiteral("verification"), errorMessage);
        emit stateChanged();
    }
}

void AppController::declineVerification()
{
    QString errorMessage;
    if (!backend_->declineVerification(errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logWarning(QStringLiteral("verification"), errorMessage);
        emit stateChanged();
    }
}

void AppController::checkForUpdates(const bool force)
{
    if (updateCheckInProgress_ || (updater_ != nullptr && updater_->isBusy())) {
        return;
    }
    if (!force && !UpdateUtilities::isUpdateCheckDue(updateCheckState_.lastCheckedAt)) {
        if (updater_ != nullptr && UpdateUtilities::isNewerVersion(updateCheckState_.latestVersion, currentVersion())) {
            updater_->handleAvailableRelease(updateCheckState_, false);
        }
        return;
    }

    updateCheckInProgress_ = true;
    emit stateChanged();

    QNetworkRequest request(QUrl(QString::fromLatin1(UpdateUtilities::kGithubReleasesApiUrl)));
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MatrixMediaShareClientQt/%1").arg(currentVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = updateNetworkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, force]() {
        const QByteArray payload = reply->readAll();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        UpdateCheckState nextState = updateCheckState_;
        nextState.lastCheckedAt = QDateTime::currentDateTimeUtc();
        nextState.lastError.clear();

        bool shouldInstall = false;
        if (reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300) {
            GithubReleaseInfo release;
            if (!UpdateUtilities::parseGithubRelease(QJsonDocument::fromJson(payload).object(), UpdateUtilities::currentPackageKind(), &release)
                || release.version.isEmpty()) {
                nextState.lastError = QStringLiteral("GitHub release response was missing a version tag.");
                logWarning(QStringLiteral("updates"), nextState.lastError);
            } else {
                nextState.latestVersion = release.version;
                nextState.latestReleaseUrl = release.htmlUrl;
                nextState.latestReleaseName = release.name;
                nextState.latestPublishedAt = release.publishedAt;
                nextState.latestAssetName = release.assetName;
                nextState.latestAssetUrl = release.assetUrl;
                nextState.latestAssetSize = release.assetSize;
                shouldInstall = UpdateUtilities::isNewerVersion(nextState.latestVersion, currentVersion());

                if (shouldInstall) {
                    logInfo(
                        QStringLiteral("updates"),
                        QStringLiteral("Update available: %1 (current: %2).").arg(nextState.latestVersion, currentVersion()));
                } else {
                    logInfo(
                        QStringLiteral("updates"),
                        QStringLiteral("Update check complete. Latest published release is %1.").arg(nextState.latestVersion));
                }
            }
        } else if (httpStatus == 404 || replyMessage(payload) == QStringLiteral("Not Found")) {
            nextState.latestVersion.clear();
            nextState.latestReleaseUrl.clear();
            nextState.latestReleaseName.clear();
            nextState.latestPublishedAt = {};
            nextState.latestAssetName.clear();
            nextState.latestAssetUrl.clear();
            nextState.latestAssetSize = -1;
            logInfo(QStringLiteral("updates"), QStringLiteral("No published GitHub release found yet."));
        } else {
            const QString message = replyMessage(payload);
            nextState.lastError = !message.isEmpty() ? message : reply->errorString().trimmed();
            logWarning(
                QStringLiteral("updates"),
                QStringLiteral("Update check failed: %1").arg(nextState.lastError));
        }

        updateCheckState_ = nextState;
        persistUpdateCheckState();
        updateCheckInProgress_ = false;
        reply->deleteLater();
        emit stateChanged();

        if (shouldInstall && updater_ != nullptr) {
            updater_->handleAvailableRelease(updateCheckState_, force);
        }
    });
}

void AppController::openLatestReleasePage()
{
    QDesktopServices::openUrl(QUrl(latestReleasePageUrl()));
}

void AppController::applyStagedUpdate(const bool relaunchNow)
{
    if (updater_ == nullptr) {
        return;
    }
    markUpdateNotified(updater_->stagedVersion());
    updater_->applyStagedUpdate(relaunchNow);
}

void AppController::deferStagedUpdate()
{
    if (updater_ == nullptr) {
        return;
    }
    markUpdateNotified(updater_->stagedVersion());
    updater_->scheduleInstallOnExit();
}

void AppController::markUpdateNotified(const QString &version)
{
    const QString normalized = UpdateUtilities::normalizeReleaseVersion(version);
    if (normalized.isEmpty() || updateCheckState_.lastNotifiedVersion == normalized) {
        return;
    }
    updateCheckState_.lastNotifiedVersion = normalized;
    persistUpdateCheckState();
}

void AppController::persistUpdateCheckState()
{
    if (updater_ != nullptr && updater_->hasStagedUpdate()) {
        updateCheckState_.pendingInstallVersion = updater_->stagedVersion();
        updateCheckState_.pendingInstallPath = updater_->stagedPayloadPath();
    }
    database_.saveUpdateCheckState(updateCheckState_);
}

void AppController::dismissError()
{
    lastErrorMessage_.clear();
}

void AppController::logInfo(const QString &subsystem, const QString &message)
{
    database_.insertLog(AppLogLevel::Info, subsystem, message);
}

void AppController::logWarning(const QString &subsystem, const QString &message)
{
    database_.insertLog(AppLogLevel::Warning, subsystem, message);
}

void AppController::logError(const QString &subsystem, const QString &message)
{
    database_.insertLog(AppLogLevel::Error, subsystem, message);
}

void AppController::scheduleRefresh()
{
    if (refreshQueued_) {
        return;
    }

    refreshQueued_ = true;
    QTimer::singleShot(0, this, &AppController::refresh);
}

void AppController::updateRefreshTimer()
{
    const bool shouldPoll = settings_.desiredPowerState
        || runtime_.connectionState == ConnectionState::Starting
        || runtime_.connectionState == ConnectionState::Running
        || runtime_.connectionState == ConnectionState::Stopping
        || runtime_.connectionState == ConnectionState::Error;

    if (shouldPoll) {
        refreshTimer_->start();
    } else {
        refreshTimer_->stop();
    }
}
