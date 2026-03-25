#pragma once

#include "Domain.h"

#include <QSqlDatabase>
#include <QStringList>
#include <QVector>

class AppDatabase
{
public:
    explicit AppDatabase(const QString &databasePath);
    ~AppDatabase();

    AppSettings loadSettings(const QString &defaultDestinationRootPath);
    bool saveSettings(const AppSettings &settings);
    QString lastErrorText() const;
    UpdateCheckState loadUpdateCheckState() const;
    bool saveUpdateCheckState(const UpdateCheckState &state);

    QVector<RoomRecord> fetchRooms() const;
    QVector<AttachmentDiscovery> fetchDiscoveries() const;
    QVector<AttachmentDiscovery> fetchDiscoveriesPage(const QString &roomId, int offset, int limit) const;
    int fetchDiscoveryCount(const QString &roomId = QString()) const;
    QVector<SharedItemRecord> fetchSharedItems() const;
    QVector<DownloadJobRecord> fetchJobs() const;
    QVector<ActivityLogEntry> fetchRecentLogs(int limit = 500) const;
    QVector<ActivityLogEntry> fetchLogsPage(int offset, int limit, bool problemsOnly) const;
    int fetchLogCount(bool problemsOnly) const;
    QStringList aliasHistory(const QString &roomId) const;
    int fetchWaitingJobCount() const;
    bool queueDiscoveryDownload(const QString &roomId, const QString &eventId);

    bool retryFailedJob(qint64 jobId);
    int retryAllFailedJobs();
    bool clearFailedJob(qint64 jobId);
    int clearAllFailedJobs();
    bool resetHistoryScansForFullRescan();

    bool insertLog(AppLogLevel level, const QString &subsystem, const QString &message);

private:
    void initializeSchema();
    bool execute(const QString &sql) const;

    QSqlDatabase database_;
    QString lastErrorText_;
};
