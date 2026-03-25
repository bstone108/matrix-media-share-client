#include "AppController.h"

#include "MatrixClientBackend.h"
#include "ProcessMatrixClientBackend.h"

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
constexpr auto kLatestReleaseApiUrl = "https://api.github.com/repos/bstone108/matrix-media-share-client/releases/latest";
constexpr auto kReleasesPageUrl = "https://github.com/bstone108/matrix-media-share-client/releases";
constexpr qint64 kWeeklyUpdateCheckIntervalSeconds = 7LL * 24LL * 60LL * 60LL;

QString defaultDestinationRootPath()
{
    QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty()) {
        downloads = QDir::homePath() + QStringLiteral("/Downloads");
    }
    return downloads + QStringLiteral("/Matrix Media Share Client");
}

QString normalizeReleaseVersion(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    return version;
}

QVector<int> parseVersionParts(const QString &version)
{
    QVector<int> parts;
    for (const QString &segment : normalizeReleaseVersion(version).split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int value = segment.toInt(&ok);
        if (!ok) {
            return {};
        }
        parts.append(value);
    }
    return parts;
}

int compareVersionStrings(const QString &lhs, const QString &rhs)
{
    const QVector<int> lhsParts = parseVersionParts(lhs);
    const QVector<int> rhsParts = parseVersionParts(rhs);
    if (lhsParts.isEmpty() || rhsParts.isEmpty()) {
        return QString::compare(normalizeReleaseVersion(lhs), normalizeReleaseVersion(rhs), Qt::CaseInsensitive);
    }

    const int segmentCount = qMax(lhsParts.size(), rhsParts.size());
    for (int index = 0; index < segmentCount; ++index) {
        const int lhsValue = index < lhsParts.size() ? lhsParts.at(index) : 0;
        const int rhsValue = index < rhsParts.size() ? rhsParts.at(index) : 0;
        if (lhsValue < rhsValue) {
            return -1;
        }
        if (lhsValue > rhsValue) {
            return 1;
        }
    }
    return 0;
}

bool isUpdateCheckDue(const UpdateCheckState &state)
{
    if (!state.lastCheckedAt.isValid()) {
        return true;
    }
    return state.lastCheckedAt.secsTo(QDateTime::currentDateTimeUtc()) >= kWeeklyUpdateCheckIntervalSeconds;
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
}

void AppController::initialize()
{
    settings_ = database_.loadSettings(defaultDestinationRootPath());
    password_ = secretStore_.loadPassword();
    updateCheckState_ = database_.loadUpdateCheckState();
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

    if (isUpdateCheckDue(updateCheckState_)) {
        QTimer::singleShot(0, this, [this]() {
            checkForUpdates(false);
        });
    }
}

void AppController::refresh()
{
    refreshQueued_ = false;
    rooms_ = database_.fetchRooms();
    jobs_ = database_.fetchJobs();
    waitingQueueCount_ = database_.fetchWaitingJobCount();
    emit stateChanged();
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
    return updateCheckInProgress_;
}

bool AppController::updateAvailable() const
{
    return compareVersionStrings(updateCheckState_.latestVersion, currentVersion()) > 0;
}

QString AppController::updateStatusText() const
{
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

    const int comparison = compareVersionStrings(updateCheckState_.latestVersion, currentVersion());
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
    return QString::fromLatin1(kReleasesPageUrl);
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
    settings_.desiredPowerState = enabled;
    if (!database_.saveSettings(settings_)) {
        lastErrorMessage_ = QStringLiteral("Failed to save power state: %1").arg(database_.lastErrorText().trimmed());
        logError(QStringLiteral("settings"), lastErrorMessage_);
    }

    if (!enabled) {
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

    QString errorMessage;
    BotRuntimeSnapshot runtime;
    runtime.connectionState = ConnectionState::Starting;
    if (backend_->start(settings_, password_, runtime, errorMessage)) {
        runtime_ = runtime;
        logInfo(QStringLiteral("matrix"), QStringLiteral("Power on requested."));
    } else {
        runtime_ = runtime;
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("matrix"), errorMessage);
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

    QString errorMessage;
    if (!backend_->shareLocalFiles(roomId, cleanedPaths, errorMessage)) {
        lastErrorMessage_ = errorMessage;
        logError(QStringLiteral("share"), errorMessage);
    } else {
        logInfo(
            QStringLiteral("share"),
            QStringLiteral("Queued %1 file(s) for sharing into %2.").arg(cleanedPaths.size()).arg(roomId));
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
    if (updateCheckInProgress_) {
        return;
    }
    if (!force && !isUpdateCheckDue(updateCheckState_)) {
        return;
    }

    updateCheckInProgress_ = true;
    emit stateChanged();

    QNetworkRequest request(QUrl(QString::fromLatin1(kLatestReleaseApiUrl)));
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MatrixMediaShareClientQt/%1").arg(currentVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");

    QNetworkReply *reply = updateNetworkManager_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray payload = reply->readAll();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        UpdateCheckState nextState = updateCheckState_;
        nextState.lastCheckedAt = QDateTime::currentDateTimeUtc();
        nextState.lastError.clear();

        if (reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300) {
            const QJsonDocument document = QJsonDocument::fromJson(payload);
            const QJsonObject object = document.object();
            nextState.latestVersion = normalizeReleaseVersion(object.value(QStringLiteral("tag_name")).toString());
            nextState.latestReleaseUrl = object.value(QStringLiteral("html_url")).toString().trimmed();
            nextState.latestReleaseName = object.value(QStringLiteral("name")).toString().trimmed();
            nextState.latestPublishedAt = QDateTime::fromString(
                object.value(QStringLiteral("published_at")).toString(),
                Qt::ISODate);

            if (!nextState.latestVersion.isEmpty() && compareVersionStrings(nextState.latestVersion, currentVersion()) > 0) {
                logInfo(
                    QStringLiteral("updates"),
                    QStringLiteral("Update available: %1 (current: %2).").arg(nextState.latestVersion, currentVersion()));
            } else if (!nextState.latestVersion.isEmpty()) {
                logInfo(
                    QStringLiteral("updates"),
                    QStringLiteral("Update check complete. Latest published release is %1.").arg(nextState.latestVersion));
            } else {
                logInfo(QStringLiteral("updates"), QStringLiteral("Update check complete. No published release found."));
            }
        } else if (httpStatus == 404 || replyMessage(payload) == QStringLiteral("Not Found")) {
            nextState.latestVersion.clear();
            nextState.latestReleaseUrl.clear();
            nextState.latestReleaseName.clear();
            nextState.latestPublishedAt = {};
            logInfo(QStringLiteral("updates"), QStringLiteral("No published GitHub release found yet."));
        } else {
            const QString message = replyMessage(payload);
            nextState.lastError = !message.isEmpty() ? message : reply->errorString().trimmed();
            logWarning(
                QStringLiteral("updates"),
                QStringLiteral("Update check failed: %1").arg(nextState.lastError));
        }

        database_.saveUpdateCheckState(nextState);
        updateCheckState_ = nextState;
        updateCheckInProgress_ = false;
        reply->deleteLater();
        emit stateChanged();
    });
}

void AppController::openLatestReleasePage()
{
    QDesktopServices::openUrl(QUrl(latestReleasePageUrl()));
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
