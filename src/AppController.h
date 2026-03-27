#pragma once

#include "AppDatabase.h"
#include "AppPaths.h"
#include "Domain.h"
#include "MatrixClientBackend.h"
#include "SecretStore.h"

#include <QObject>
#include <QStringList>
#include <memory>

class QTimer;
class QNetworkAccessManager;

class AppController : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    void initialize();
    void refresh();

    const AppSettings &settings() const;
    const QString &password() const;
    const BotRuntimeSnapshot &runtime() const;
    const QVector<RoomRecord> &rooms() const;
    QVector<AttachmentDiscovery> fetchDiscoveriesPage(const QString &roomId, int offset, int limit) const;
    int discoveryCount(const QString &roomId = QString()) const;
    QVector<RoomRecord> joinedRooms() const;
    QVector<RoomRecord> joinedSpaces() const;
    const QVector<SharedItemRecord> &sharedItems() const;
    const QVector<DownloadJobRecord> &jobs() const;
    QVector<ActivityLogEntry> fetchLogsPage(int offset, int limit, bool problemsOnly) const;
    int logCount(bool problemsOnly) const;
    int waitingQueueCount() const;
    QStringList aliasHistory(const QString &roomId) const;

    QString connectionStatusText() const;
    QString lastErrorMessage() const;
    QString currentVersion() const;
    const UpdateCheckState &updateCheckState() const;
    bool isUpdateCheckInProgress() const;
    bool updateAvailable() const;
    QString updateStatusText() const;
    QString latestReleaseSummaryText() const;
    QString latestReleasePageUrl() const;
    QString settingsDatabasePath() const;
    QString secretStorePath() const;
    void recordInfo(const QString &subsystem, const QString &message);
    void recordWarning(const QString &subsystem, const QString &message);
    void recordError(const QString &subsystem, const QString &message);

public slots:
    void togglePower(bool enabled);
    bool saveSettings(const AppSettings &settings, const QString &password);
    void resetHistoryScans();
    void retryFailedJob(qint64 jobId);
    void retryAllFailedJobs();
    void clearFailedJob(qint64 jobId);
    void clearAllFailedJobs();
    void queueDiscoveryDownload(const QString &roomId, const QString &eventId);
    void openDiscovery(const QString &roomId, const QString &eventId);
    void focusRoom(const QString &roomId);
    void shareLocalFile(const QString &roomId, const QString &filePath);
    void shareLocalFiles(const QString &roomId, const QStringList &filePaths);
    void importIpfsLink(const QString &link);
    void deleteSharedItem(const QString &sha256);
    void refreshCatalog();
    void joinRoom(const QString &roomIdOrAlias);
    void leaveRoom(const QString &roomId);
    void requestVerification();
    void startSasVerification();
    void approveVerification();
    void declineVerification();
    void checkForUpdates(bool force = true);
    void openLatestReleasePage();
    void dismissError();

signals:
    void stateChanged();
    void userNoticeRequested(const QString &title, const QString &message);

private:
    void logInfo(const QString &subsystem, const QString &message);
    void logWarning(const QString &subsystem, const QString &message);
    void logError(const QString &subsystem, const QString &message);
    QString startupValidationError(const AppSettings &settings, const QString &password) const;
    void scheduleRefresh();
    void updateRefreshTimer();
    void shutdownBackendForExit();

    AppPaths paths_;
    AppDatabase database_;
    SecretStore secretStore_;
    std::unique_ptr<MatrixClientBackend> backend_;
    QTimer *refreshTimer_ = nullptr;

    AppSettings settings_;
    QString password_;
    BotRuntimeSnapshot runtime_;
    QVector<RoomRecord> rooms_;
    QVector<SharedItemRecord> sharedItems_;
    QVector<DownloadJobRecord> jobs_;
    int waitingQueueCount_ = 0;
    QString lastErrorMessage_;
    bool refreshQueued_ = false;
    UpdateCheckState updateCheckState_;
    bool updateCheckInProgress_ = false;
    QNetworkAccessManager *updateNetworkManager_ = nullptr;
};
