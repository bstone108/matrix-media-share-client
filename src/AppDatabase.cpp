#include "AppDatabase.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

namespace {
QString connectionName()
{
    static const QString value = QStringLiteral("matrix-media-share-client-qt-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return value;
}

QString timeWindowUnitKey(const TimeWindowUnit unit)
{
    switch (unit) {
    case TimeWindowUnit::None:
        return QStringLiteral("none");
    case TimeWindowUnit::Day:
        return QStringLiteral("day");
    case TimeWindowUnit::Week:
        return QStringLiteral("week");
    case TimeWindowUnit::Month:
        return QStringLiteral("month");
    }
    return QStringLiteral("none");
}

TimeWindowUnit parseTimeWindowUnit(const QString &value)
{
    if (value == QStringLiteral("day")) {
        return TimeWindowUnit::Day;
    }
    if (value == QStringLiteral("week")) {
        return TimeWindowUnit::Week;
    }
    if (value == QStringLiteral("month")) {
        return TimeWindowUnit::Month;
    }
    return TimeWindowUnit::None;
}

QString failedRetentionUnitKey(const FailedJobRetentionUnit unit)
{
    switch (unit) {
    case FailedJobRetentionUnit::None:
        return QStringLiteral("none");
    case FailedJobRetentionUnit::Minute:
        return QStringLiteral("minute");
    case FailedJobRetentionUnit::Hour:
        return QStringLiteral("hour");
    case FailedJobRetentionUnit::Day:
        return QStringLiteral("day");
    }
    return QStringLiteral("none");
}

FailedJobRetentionUnit parseFailedRetentionUnit(const QString &value)
{
    if (value == QStringLiteral("minute")) {
        return FailedJobRetentionUnit::Minute;
    }
    if (value == QStringLiteral("hour")) {
        return FailedJobRetentionUnit::Hour;
    }
    if (value == QStringLiteral("day")) {
        return FailedJobRetentionUnit::Day;
    }
    return FailedJobRetentionUnit::None;
}

MediaCategory parseMediaCategory(const QString &value)
{
    if (value == QStringLiteral("images")) {
        return MediaCategory::Images;
    }
    if (value == QStringLiteral("videos")) {
        return MediaCategory::Videos;
    }
    if (value == QStringLiteral("audio")) {
        return MediaCategory::Audio;
    }
    if (value == QStringLiteral("documents")) {
        return MediaCategory::Documents;
    }
    if (value == QStringLiteral("archives")) {
        return MediaCategory::Archives;
    }
    if (value == QStringLiteral("programs")) {
        return MediaCategory::Programs;
    }
    return MediaCategory::Other;
}

MediaSourceKind parseMediaSourceKind(const QString &value)
{
    if (value == QStringLiteral("ipfs")) {
        return MediaSourceKind::Ipfs;
    }
    if (value == QStringLiteral("localFile")) {
        return MediaSourceKind::LocalFile;
    }
    return MediaSourceKind::Matrix;
}

DownloadJobState parseDownloadJobState(const QString &value)
{
    if (value == QStringLiteral("downloading")) {
        return DownloadJobState::Downloading;
    }
    if (value == QStringLiteral("coolingDown")) {
        return DownloadJobState::CoolingDown;
    }
    if (value == QStringLiteral("completed")) {
        return DownloadJobState::Completed;
    }
    if (value == QStringLiteral("duplicateCompleted")) {
        return DownloadJobState::DuplicateCompleted;
    }
    if (value == QStringLiteral("failedPermanent")) {
        return DownloadJobState::FailedPermanent;
    }
    if (value == QStringLiteral("undecryptablePending")) {
        return DownloadJobState::UndecryptablePending;
    }
    return DownloadJobState::Queued;
}

AppLogLevel parseLogLevel(const QString &value)
{
    if (value == QStringLiteral("debug")) {
        return AppLogLevel::Debug;
    }
    if (value == QStringLiteral("warning")) {
        return AppLogLevel::Warning;
    }
    if (value == QStringLiteral("error")) {
        return AppLogLevel::Error;
    }
    return AppLogLevel::Info;
}

bool columnExists(const QSqlDatabase &database, const QString &tableName, const QString &columnName)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(tableName))) {
        return false;
    }

    while (query.next()) {
        if (query.value(1).toString() == columnName) {
            return true;
        }
    }
    return false;
}

QString normalizedText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

AppSettings normalizedSettings(AppSettings settings)
{
    const QString destinationRootPath = normalizedText(settings.destinationRootPath).trimmed().isEmpty()
        ? normalizedText(settings.manualDownloadRootPath)
        : normalizedText(settings.destinationRootPath);
    settings.destinationRootPath = destinationRootPath;
    settings.manualDownloadRootPath = settings.destinationRootPath;
    return settings;
}

AttachmentDiscovery readDiscoveryRecord(const QSqlQuery &query)
{
    AttachmentDiscovery discovery;
    discovery.roomId = query.value(0).toString();
    discovery.eventId = query.value(1).toString();
    discovery.originServerTimestamp = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs);
    discovery.sourceKind = parseMediaSourceKind(query.value(3).toString());
    discovery.directUrl = query.value(4).toString();
    discovery.mxcUrl = query.value(5).toString();
    discovery.thumbnailSourceUrl = query.value(6).toString();
    discovery.thumbnailCachedPath = query.value(7).toString();
    discovery.originalFilename = query.value(8).toString();
    discovery.mimeType = query.value(9).toString();
    discovery.category = parseMediaCategory(query.value(10).toString());
    return discovery;
}

ActivityLogEntry readActivityLogEntry(const QSqlQuery &query)
{
    ActivityLogEntry entry;
    entry.id = query.value(0).toLongLong();
    entry.createdAt = QDateTime::fromString(query.value(1).toString(), Qt::ISODateWithMs);
    entry.level = parseLogLevel(query.value(2).toString());
    entry.subsystem = query.value(3).toString();
    entry.message = query.value(4).toString();
    return entry;
}

SharedItemRecord readSharedItemRecord(const QSqlQuery &query)
{
    SharedItemRecord item;
    item.sha256 = query.value(0).toString();
    item.sourceKind = query.value(1).toString() == QStringLiteral("archive")
        ? MediaSourceKind::LocalFile
        : query.value(1).toString() == QStringLiteral("downloads")
            ? MediaSourceKind::LocalFile
            : MediaSourceKind::LocalFile;
    item.sourcePath = query.value(2).toString();
    item.bundlePath = query.value(3).toString();
    item.libraryPath = query.value(4).toString();
    item.archivePath = query.value(5).toString();
    item.fileCid = query.value(6).toString();
    item.thumbnailCid = query.value(7).toString();
    item.pageCid = query.value(8).toString();
    item.landingPageUrl = query.value(9).toString();
    item.roomId = query.value(10).toString();
    item.category = parseMediaCategory(query.value(11).toString());
    item.originalFilename = query.value(12).toString();
    item.mimeType = query.value(13).toString();
    item.fileSize = query.value(14).toLongLong();
    item.updatedAt = QDateTime::fromString(query.value(15).toString(), Qt::ISODateWithMs);
    return item;
}
}

AppDatabase::AppDatabase(const QString &databasePath)
{
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName());
    database_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    database_.setDatabaseName(databasePath);
    if (!database_.open()) {
        lastErrorText_ = database_.lastError().text();
        return;
    }
    execute(QStringLiteral("PRAGMA journal_mode = WAL"));
    execute(QStringLiteral("PRAGMA synchronous = NORMAL"));
    execute(QStringLiteral("PRAGMA foreign_keys = ON"));
    initializeSchema();
}

AppDatabase::~AppDatabase()
{
    if (database_.isOpen()) {
        database_.close();
    }
}

AppSettings AppDatabase::loadSettings(const QString &defaultDestinationRootPath)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT * FROM app_settings ORDER BY id DESC LIMIT 1"));
    if (!query.exec()) {
        lastErrorText_ = query.lastError().text();
    } else if (query.next()) {
        AppSettings settings;
        settings.homeserverUrl = query.value(QStringLiteral("homeserver_url")).toString();
        settings.username = query.value(QStringLiteral("username")).toString();
        settings.ownerUserId.clear();
        settings.destinationRootPath = query.value(QStringLiteral("destination_root_path")).toString();
        settings.libraryRootPath = query.value(QStringLiteral("library_root_path")).toString();
        settings.flatFolderLayout = query.value(QStringLiteral("flat_folder_layout")).toBool();
        settings.archiveRootPath = query.value(QStringLiteral("archive_root_path")).toString();
        settings.archiveScanEnabled = query.value(QStringLiteral("archive_scan_enabled")).toBool();
        settings.archiveScanHighPriority = query.value(QStringLiteral("archive_scan_high_priority")).toBool();
        settings.manualDownloadRootPath = query.value(QStringLiteral("manual_download_root_path")).toString();
        settings.messageLimit = query.value(QStringLiteral("message_limit")).toInt();
        settings.timeWindowValue = query.value(QStringLiteral("time_window_value")).toInt();
        settings.timeWindowUnit = parseTimeWindowUnit(query.value(QStringLiteral("time_window_unit")).toString());
        settings.retryCooldownMinutes = query.value(QStringLiteral("retry_cooldown_minutes")).toInt();
        settings.retryLimit = query.value(QStringLiteral("retry_limit")).toInt();
        settings.downloadWorkerCount = query.value(QStringLiteral("download_worker_count")).toInt();
        settings.failedJobRetentionValue = query.value(QStringLiteral("failed_job_retention_value")).toInt();
        settings.failedJobRetentionUnit = parseFailedRetentionUnit(query.value(QStringLiteral("failed_job_retention_unit")).toString());
        settings.primaryGatewayUrl = query.value(QStringLiteral("primary_gateway_url")).toString();
        for (const QJsonValue &value : QJsonDocument::fromJson(query.value(QStringLiteral("preferred_gateway_urls")).toByteArray()).array()) {
            settings.preferredGatewayUrls.append(value.toString());
        }
        if (settings.preferredGatewayUrls.isEmpty()) {
            settings.preferredGatewayUrls = AppSettings::defaults(defaultDestinationRootPath).preferredGatewayUrls;
        }
        settings.autostartEnabled = query.value(QStringLiteral("autostart_enabled")).toBool();
        settings.minimizeToTray = query.value(QStringLiteral("minimize_to_tray")).toBool();
        settings.startHidden = query.value(QStringLiteral("start_hidden")).toBool();
        settings.bandwidthLimitKiBPerSec = query.value(QStringLiteral("bandwidth_limit_kib_per_sec")).toInt();
        settings.previewWorkerCount = query.value(QStringLiteral("preview_worker_count")).toInt();
        settings.autoJoinSpaceRooms = query.value(QStringLiteral("auto_join_space_rooms")).toBool();
        settings.autoDownloadNewMedia = query.value(QStringLiteral("auto_download_new_media")).toBool();
        settings.selfHealEnabled = query.value(QStringLiteral("self_heal_enabled")).toBool();
        settings.desiredPowerState = query.value(QStringLiteral("desired_power_state")).toBool();
        lastErrorText_.clear();
        return normalizedSettings(settings);
    }

    const AppSettings defaults = AppSettings::defaults(defaultDestinationRootPath);
    if (saveSettings(defaults)) {
        lastErrorText_.clear();
    }
    return normalizedSettings(defaults);
}

bool AppDatabase::saveSettings(const AppSettings &settings)
{
    AppSettings sanitizedSettings = normalizedSettings(settings);
    sanitizedSettings.homeserverUrl = normalizedText(sanitizedSettings.homeserverUrl);
    sanitizedSettings.username = normalizedText(sanitizedSettings.username);
    sanitizedSettings.ownerUserId = normalizedText(sanitizedSettings.ownerUserId);
    sanitizedSettings.libraryRootPath = normalizedText(sanitizedSettings.libraryRootPath);
    sanitizedSettings.archiveRootPath = normalizedText(sanitizedSettings.archiveRootPath);
    sanitizedSettings.primaryGatewayUrl = normalizedText(sanitizedSettings.primaryGatewayUrl);

    QSqlQuery deleteQuery(database_);
    if (!deleteQuery.exec(QStringLiteral("DELETE FROM app_settings"))) {
        lastErrorText_ = deleteQuery.lastError().text();
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO app_settings ("
        "homeserver_url, username, owner_user_id, destination_root_path, library_root_path, flat_folder_layout, archive_root_path, archive_scan_enabled, archive_scan_high_priority, manual_download_root_path, "
        "message_limit, time_window_value, time_window_unit, "
        "retry_cooldown_minutes, retry_limit, download_worker_count, "
        "failed_job_retention_value, failed_job_retention_unit, primary_gateway_url, preferred_gateway_urls, "
        "autostart_enabled, minimize_to_tray, start_hidden, bandwidth_limit_kib_per_sec, preview_worker_count, "
        "auto_join_space_rooms, auto_download_new_media, self_heal_enabled, desired_power_state, updated_at"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(sanitizedSettings.homeserverUrl);
    query.addBindValue(sanitizedSettings.username);
    query.addBindValue(sanitizedSettings.ownerUserId);
    query.addBindValue(sanitizedSettings.destinationRootPath);
    query.addBindValue(sanitizedSettings.libraryRootPath);
    query.addBindValue(sanitizedSettings.flatFolderLayout ? 1 : 0);
    query.addBindValue(sanitizedSettings.archiveRootPath);
    query.addBindValue(sanitizedSettings.archiveScanEnabled ? 1 : 0);
    query.addBindValue(sanitizedSettings.archiveScanHighPriority ? 1 : 0);
    query.addBindValue(sanitizedSettings.manualDownloadRootPath);
    query.addBindValue(sanitizedSettings.messageLimit);
    query.addBindValue(sanitizedSettings.timeWindowValue);
    query.addBindValue(timeWindowUnitKey(sanitizedSettings.timeWindowUnit));
    query.addBindValue(sanitizedSettings.retryCooldownMinutes);
    query.addBindValue(sanitizedSettings.retryLimit);
    query.addBindValue(qBound(1, sanitizedSettings.downloadWorkerCount, 6));
    query.addBindValue(sanitizedSettings.failedJobRetentionValue);
    query.addBindValue(failedRetentionUnitKey(sanitizedSettings.failedJobRetentionUnit));
    query.addBindValue(sanitizedSettings.primaryGatewayUrl);
    query.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(sanitizedSettings.preferredGatewayUrls)).toJson(QJsonDocument::Compact)));
    query.addBindValue(sanitizedSettings.autostartEnabled ? 1 : 0);
    query.addBindValue(sanitizedSettings.minimizeToTray ? 1 : 0);
    query.addBindValue(sanitizedSettings.startHidden ? 1 : 0);
    query.addBindValue(sanitizedSettings.bandwidthLimitKiBPerSec);
    query.addBindValue(sanitizedSettings.previewWorkerCount);
    query.addBindValue(sanitizedSettings.autoJoinSpaceRooms ? 1 : 0);
    query.addBindValue(sanitizedSettings.autoDownloadNewMedia ? 1 : 0);
    query.addBindValue(sanitizedSettings.selfHealEnabled ? 1 : 0);
    query.addBindValue(sanitizedSettings.desiredPowerState ? 1 : 0);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        lastErrorText_ = query.lastError().text();
        return false;
    }

    lastErrorText_.clear();
    return true;
}

QString AppDatabase::lastErrorText() const
{
    return lastErrorText_;
}

UpdateCheckState AppDatabase::loadUpdateCheckState() const
{
    UpdateCheckState state;

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT last_checked_at, latest_version, latest_release_url, latest_release_name, latest_published_at, last_error "
        "FROM update_check_state WHERE id = 1"));
    if (!query.exec() || !query.next()) {
        return state;
    }

    state.lastCheckedAt = QDateTime::fromString(query.value(0).toString(), Qt::ISODateWithMs);
    state.latestVersion = query.value(1).toString();
    state.latestReleaseUrl = query.value(2).toString();
    state.latestReleaseName = query.value(3).toString();
    state.latestPublishedAt = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
    state.lastError = query.value(5).toString();
    return state;
}

bool AppDatabase::saveUpdateCheckState(const UpdateCheckState &state)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO update_check_state ("
        "id, last_checked_at, latest_version, latest_release_url, latest_release_name, latest_published_at, last_error"
        ") VALUES (1, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "last_checked_at = excluded.last_checked_at, "
        "latest_version = excluded.latest_version, "
        "latest_release_url = excluded.latest_release_url, "
        "latest_release_name = excluded.latest_release_name, "
        "latest_published_at = excluded.latest_published_at, "
        "last_error = excluded.last_error"));
    query.addBindValue(state.lastCheckedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(state.latestVersion);
    query.addBindValue(state.latestReleaseUrl);
    query.addBindValue(state.latestReleaseName);
    query.addBindValue(state.latestPublishedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(state.lastError);
    return query.exec();
}

QVector<RoomRecord> AppDatabase::fetchRooms() const
{
    QVector<RoomRecord> rooms;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT room_id, display_name, canonical_alias, active_folder_label, is_space, membership, updated_at, "
        "COALESCE((SELECT COUNT(*) FROM discovered_attachments WHERE room_id = rooms.room_id), 0) "
        "FROM rooms "
        "ORDER BY COALESCE(display_name, canonical_alias, room_id) COLLATE NOCASE ASC"));

    if (!query.exec()) {
        return rooms;
    }

    while (query.next()) {
        RoomRecord room;
        room.roomId = query.value(0).toString();
        room.currentDisplayName = query.value(1).toString();
        room.currentCanonicalAlias = query.value(2).toString();
        room.activeFolderLabel = query.value(3).toString();
        room.isSpace = query.value(4).toBool();
        room.membership = query.value(5).toString();
        room.updatedAt = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs);
        room.discoveredMediaCount = query.value(7).toInt();
        rooms.append(room);
    }

    return rooms;
}

QVector<AttachmentDiscovery> AppDatabase::fetchDiscoveries() const
{
    return fetchDiscoveriesPage(QString(), 0, 500);
}

QVector<AttachmentDiscovery> AppDatabase::fetchDiscoveriesPage(const QString &roomId, const int offset, const int limit) const
{
    QVector<AttachmentDiscovery> discoveries;
    const int safeOffset = qMax(0, offset);
    const int safeLimit = qBound(1, limit, 500);

    QSqlQuery query(database_);
    if (roomId.trimmed().isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT room_id, event_id, origin_ts, source_kind, direct_url, mxc_url, thumbnail_source_url, thumbnail_cached_path, original_filename, mime_type, category "
            "FROM discovered_attachments "
            "ORDER BY origin_ts DESC, id DESC "
            "LIMIT ? OFFSET ?"));
        query.addBindValue(safeLimit);
        query.addBindValue(safeOffset);
    } else {
        query.prepare(QStringLiteral(
            "SELECT room_id, event_id, origin_ts, source_kind, direct_url, mxc_url, thumbnail_source_url, thumbnail_cached_path, original_filename, mime_type, category "
            "FROM discovered_attachments "
            "WHERE room_id = ? "
            "ORDER BY origin_ts DESC, id DESC "
            "LIMIT ? OFFSET ?"));
        query.addBindValue(roomId);
        query.addBindValue(safeLimit);
        query.addBindValue(safeOffset);
    }

    if (!query.exec()) {
        return discoveries;
    }

    while (query.next()) {
        discoveries.append(readDiscoveryRecord(query));
    }

    return discoveries;
}

int AppDatabase::fetchDiscoveryCount(const QString &roomId) const
{
    QSqlQuery query(database_);
    if (roomId.trimmed().isEmpty()) {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM discovered_attachments"));
    } else {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM discovered_attachments WHERE room_id = ?"));
        query.addBindValue(roomId);
    }

    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QVector<DownloadJobRecord> AppDatabase::fetchJobs() const
{
    QVector<DownloadJobRecord> jobs;
    QSqlQuery query(database_);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.prepare(QStringLiteral(
        "SELECT id, media_item_id, room_id, event_id, mxc_url, source_kind, direct_url, original_filename, mime_type, category, state, retry_count, "
        "next_eligible_at, last_failure_at, received_bytes, total_bytes, last_error, sha256, saved_relative_path, created_at, updated_at "
        "FROM download_jobs "
        "ORDER BY "
        "CASE state "
        "    WHEN 'queued' THEN 0 "
        "    WHEN 'coolingDown' THEN CASE WHEN next_eligible_at IS NULL OR next_eligible_at <= ? THEN 0 ELSE 1 END "
        "    WHEN 'undecryptablePending' THEN CASE WHEN next_eligible_at IS NULL OR next_eligible_at <= ? THEN 0 ELSE 1 END "
        "    WHEN 'failedPermanent' THEN 2 "
        "    ELSE 3 "
        "END, "
        "COALESCE(last_failure_at, created_at) ASC, id ASC"));
    query.addBindValue(now);
    query.addBindValue(now);

    if (!query.exec()) {
        return jobs;
    }

    while (query.next()) {
        DownloadJobRecord job;
        job.id = query.value(0).toLongLong();
        job.mediaItemId = query.value(1).toLongLong();
        job.roomId = query.value(2).toString();
        job.eventId = query.value(3).toString();
        job.mxcUrl = query.value(4).toString();
        job.sourceKind = parseMediaSourceKind(query.value(5).toString());
        job.directUrl = query.value(6).toString();
        job.originalFilename = query.value(7).toString();
        job.mimeType = query.value(8).toString();
        job.category = parseMediaCategory(query.value(9).toString());
        job.state = parseDownloadJobState(query.value(10).toString());
        job.retryCount = query.value(11).toInt();
        job.nextEligibleAt = QDateTime::fromString(query.value(12).toString(), Qt::ISODateWithMs);
        job.lastFailureAt = QDateTime::fromString(query.value(13).toString(), Qt::ISODateWithMs);
        job.receivedBytes = query.value(14).toLongLong();
        job.totalBytes = query.value(15).isNull() ? -1 : query.value(15).toLongLong();
        job.lastError = query.value(16).toString();
        job.sha256 = query.value(17).toString();
        job.savedRelativePath = query.value(18).toString();
        job.createdAt = QDateTime::fromString(query.value(19).toString(), Qt::ISODateWithMs);
        job.updatedAt = QDateTime::fromString(query.value(20).toString(), Qt::ISODateWithMs);
        jobs.append(job);
    }

    return jobs;
}

QVector<SharedItemRecord> AppDatabase::fetchSharedItems() const
{
    QVector<SharedItemRecord> items;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT sha256, source_kind, source_path, bundle_path, library_path, archive_path, "
        "file_cid, thumbnail_cid, page_cid, landing_page_url, room_id, category, original_filename, mime_type, file_size, updated_at "
        "FROM tracked_uploads "
        "ORDER BY updated_at DESC, sha256 ASC"));
    if (!query.exec()) {
        return items;
    }
    while (query.next()) {
        items.append(readSharedItemRecord(query));
    }
    return items;
}

QVector<ActivityLogEntry> AppDatabase::fetchRecentLogs(const int limit) const
{
    return fetchLogsPage(0, limit, false);
}

QVector<ActivityLogEntry> AppDatabase::fetchLogsPage(const int offset, const int limit, const bool problemsOnly) const
{
    QVector<ActivityLogEntry> logs;
    const int safeOffset = qMax(0, offset);
    const int safeLimit = qBound(1, limit, 500);

    QSqlQuery query(database_);
    if (problemsOnly) {
        query.prepare(QStringLiteral(
            "SELECT id, created_at, level, subsystem, message "
            "FROM activity_log "
            "WHERE level IN ('warning', 'error') "
            "ORDER BY id DESC "
            "LIMIT ? OFFSET ?"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT id, created_at, level, subsystem, message "
            "FROM activity_log "
            "ORDER BY id DESC "
            "LIMIT ? OFFSET ?"));
    }
    query.addBindValue(safeLimit);
    query.addBindValue(safeOffset);
    if (!query.exec()) {
        return logs;
    }

    while (query.next()) {
        logs.append(readActivityLogEntry(query));
    }

    return logs;
}

int AppDatabase::fetchLogCount(const bool problemsOnly) const
{
    QSqlQuery query(database_);
    if (problemsOnly) {
        query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM activity_log WHERE level IN ('warning', 'error')"));
    } else {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM activity_log"));
    }

    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QStringList AppDatabase::aliasHistory(const QString &roomId) const
{
    QStringList aliases;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT alias FROM room_alias_history WHERE room_id = ? ORDER BY seen_at DESC"));
    query.addBindValue(roomId);
    if (!query.exec()) {
        return aliases;
    }

    while (query.next()) {
        aliases.append(query.value(0).toString());
    }

    return aliases;
}

int AppDatabase::fetchWaitingJobCount() const
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM download_jobs WHERE state IN (?, ?, ?)"));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::Queued));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::CoolingDown));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::UndecryptablePending));
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

bool AppDatabase::queueDiscoveryDownload(const QString &roomId, const QString &eventId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO download_jobs ("
        "room_id, event_id, mxc_url, source_kind, direct_url, original_filename, mime_type, category, "
        "state, retry_count, created_at, updated_at"
        ") "
        "SELECT room_id, event_id, mxc_url, source_kind, direct_url, original_filename, mime_type, category, "
        "'queued', 0, ?, ? "
        "FROM discovered_attachments WHERE room_id = ? AND event_id = ?"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(roomId);
    query.addBindValue(eventId);
    return query.exec() && query.numRowsAffected() > 0;
}

bool AppDatabase::retryFailedJob(const qint64 jobId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE download_jobs "
        "SET state = ?, retry_count = 0, next_eligible_at = NULL, last_failure_at = NULL, last_error = NULL, updated_at = ? "
        "WHERE id = ? AND state = ?"));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::Queued));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(jobId);
    query.addBindValue(downloadJobStateTitle(DownloadJobState::FailedPermanent));
    return query.exec() && query.numRowsAffected() > 0;
}

int AppDatabase::retryAllFailedJobs()
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE download_jobs "
        "SET state = ?, retry_count = 0, next_eligible_at = NULL, last_failure_at = NULL, last_error = NULL, updated_at = ? "
        "WHERE state = ?"));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::Queued));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::FailedPermanent));
    if (!query.exec()) {
        return 0;
    }
    return query.numRowsAffected();
}

bool AppDatabase::clearFailedJob(const qint64 jobId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM download_jobs WHERE id = ? AND state = ?"));
    query.addBindValue(jobId);
    query.addBindValue(downloadJobStateTitle(DownloadJobState::FailedPermanent));
    return query.exec() && query.numRowsAffected() > 0;
}

int AppDatabase::clearAllFailedJobs()
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM download_jobs WHERE state = ?"));
    query.addBindValue(downloadJobStateTitle(DownloadJobState::FailedPermanent));
    if (!query.exec()) {
        return 0;
    }
    return query.numRowsAffected();
}

bool AppDatabase::resetHistoryScansForFullRescan()
{
    const bool scanReset = execute(QStringLiteral(
        "UPDATE room_scan_state SET "
        "last_processed_event_id = NULL, "
        "last_processed_ts = NULL, "
        "oldest_backfilled_event_id = NULL, "
        "oldest_backfilled_ts = NULL, "
        "historical_message_count = 0, "
        "initial_backfill_complete = 0, "
        "last_history_mode = 'idle', "
        "last_history_run_at = NULL"));
    const bool discoveriesCleared = execute(QStringLiteral("DELETE FROM discovered_attachments"));
    const bool jobsCleared = execute(QStringLiteral("DELETE FROM download_jobs"));
    return scanReset && discoveriesCleared && jobsCleared;
}

bool AppDatabase::insertLog(const AppLogLevel level, const QString &subsystem, const QString &message)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO activity_log (created_at, level, subsystem, message) VALUES (?, ?, ?, ?)"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(appLogLevelTitle(level));
    query.addBindValue(subsystem);
    query.addBindValue(message);
    const bool inserted = query.exec();
    execute(QStringLiteral(
        "DELETE FROM activity_log "
        "WHERE created_at < datetime('now', '-30 day')"));
    execute(QStringLiteral(
        "DELETE FROM activity_log "
        "WHERE id NOT IN (SELECT id FROM activity_log ORDER BY id DESC LIMIT 5000)"));
    return inserted;
}

void AppDatabase::initializeSchema()
{
    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS app_settings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "homeserver_url TEXT NOT NULL,"
        "username TEXT NOT NULL,"
        "owner_user_id TEXT NOT NULL,"
        "destination_root_path TEXT NOT NULL,"
        "library_root_path TEXT NOT NULL DEFAULT '',"
        "flat_folder_layout INTEGER NOT NULL DEFAULT 0,"
        "archive_root_path TEXT NOT NULL DEFAULT '',"
        "archive_scan_enabled INTEGER NOT NULL DEFAULT 0,"
        "archive_scan_high_priority INTEGER NOT NULL DEFAULT 0,"
        "manual_download_root_path TEXT NOT NULL DEFAULT '',"
        "message_limit INTEGER NOT NULL,"
        "time_window_value INTEGER NOT NULL,"
        "time_window_unit TEXT NOT NULL,"
        "retry_cooldown_minutes INTEGER NOT NULL,"
        "retry_limit INTEGER NOT NULL,"
        "download_worker_count INTEGER NOT NULL DEFAULT 1,"
        "failed_job_retention_value INTEGER NOT NULL DEFAULT 0,"
        "failed_job_retention_unit TEXT NOT NULL DEFAULT 'none',"
        "primary_gateway_url TEXT NOT NULL DEFAULT 'https://dweb.link',"
        "preferred_gateway_urls TEXT NOT NULL DEFAULT '[]',"
        "autostart_enabled INTEGER NOT NULL DEFAULT 0,"
        "minimize_to_tray INTEGER NOT NULL DEFAULT 1,"
        "start_hidden INTEGER NOT NULL DEFAULT 0,"
        "bandwidth_limit_kib_per_sec INTEGER NOT NULL DEFAULT 0,"
        "preview_worker_count INTEGER NOT NULL DEFAULT 1,"
        "auto_join_space_rooms INTEGER NOT NULL DEFAULT 0,"
        "auto_download_new_media INTEGER NOT NULL DEFAULT 0,"
        "self_heal_enabled INTEGER NOT NULL DEFAULT 0,"
        "desired_power_state INTEGER NOT NULL,"
        "updated_at TEXT NOT NULL"
        ")"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS rooms ("
        "room_id TEXT PRIMARY KEY,"
        "display_name TEXT,"
        "canonical_alias TEXT,"
        "active_folder_label TEXT NOT NULL,"
        "is_space INTEGER NOT NULL DEFAULT 0,"
        "membership TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ")"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS room_alias_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "room_id TEXT NOT NULL,"
        "alias TEXT NOT NULL,"
        "seen_at TEXT NOT NULL,"
        "UNIQUE(room_id, alias)"
        ")"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS room_scan_state ("
        "room_id TEXT PRIMARY KEY,"
        "last_processed_event_id TEXT,"
        "last_processed_ts TEXT,"
        "oldest_backfilled_event_id TEXT,"
        "oldest_backfilled_ts TEXT,"
        "historical_message_count INTEGER NOT NULL DEFAULT 0,"
        "initial_backfill_complete INTEGER NOT NULL DEFAULT 0,"
        "last_history_mode TEXT NOT NULL DEFAULT 'idle',"
        "last_history_run_at TEXT"
        ")"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS discovered_attachments ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "room_id TEXT NOT NULL,"
        "event_id TEXT NOT NULL,"
        "origin_ts TEXT NOT NULL,"
        "source_kind TEXT NOT NULL DEFAULT 'matrix',"
        "direct_url TEXT,"
        "mxc_url TEXT NOT NULL,"
        "thumbnail_source_url TEXT,"
        "thumbnail_cached_path TEXT,"
        "original_filename TEXT,"
        "mime_type TEXT,"
        "category TEXT NOT NULL,"
        "UNIQUE(room_id, event_id)"
        ")"));
    if (!columnExists(database_, QStringLiteral("discovered_attachments"), QStringLiteral("source_kind"))) {
        execute(QStringLiteral(
            "ALTER TABLE discovered_attachments ADD COLUMN source_kind TEXT NOT NULL DEFAULT 'matrix'"));
    }
    if (!columnExists(database_, QStringLiteral("discovered_attachments"), QStringLiteral("direct_url"))) {
        execute(QStringLiteral(
            "ALTER TABLE discovered_attachments ADD COLUMN direct_url TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("discovered_attachments"), QStringLiteral("thumbnail_source_url"))) {
        execute(QStringLiteral(
            "ALTER TABLE discovered_attachments ADD COLUMN thumbnail_source_url TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("discovered_attachments"), QStringLiteral("thumbnail_cached_path"))) {
        execute(QStringLiteral(
            "ALTER TABLE discovered_attachments ADD COLUMN thumbnail_cached_path TEXT"));
    }
    execute(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_discovered_attachments_room_origin "
        "ON discovered_attachments(room_id, origin_ts DESC, id DESC)"));
    execute(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_discovered_attachments_origin "
        "ON discovered_attachments(origin_ts DESC, id DESC)"));
    if (!columnExists(database_, QStringLiteral("app_settings"), QStringLiteral("flat_folder_layout"))) {
        execute(QStringLiteral(
            "ALTER TABLE app_settings ADD COLUMN flat_folder_layout INTEGER NOT NULL DEFAULT 0"));
    }
    if (!columnExists(database_, QStringLiteral("app_settings"), QStringLiteral("self_heal_enabled"))) {
        execute(QStringLiteral(
            "ALTER TABLE app_settings ADD COLUMN self_heal_enabled INTEGER NOT NULL DEFAULT 0"));
    }

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS download_jobs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "media_item_id INTEGER,"
        "room_id TEXT NOT NULL,"
        "event_id TEXT NOT NULL,"
        "mxc_url TEXT NOT NULL,"
        "source_kind TEXT NOT NULL DEFAULT 'matrix',"
        "direct_url TEXT,"
        "original_filename TEXT,"
        "mime_type TEXT,"
        "category TEXT NOT NULL,"
        "state TEXT NOT NULL,"
        "retry_count INTEGER NOT NULL DEFAULT 0,"
        "next_eligible_at TEXT,"
        "last_failure_at TEXT,"
        "received_bytes INTEGER NOT NULL DEFAULT 0,"
        "total_bytes INTEGER,"
        "last_error TEXT,"
        "sha256 TEXT,"
        "saved_relative_path TEXT,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,"
        "UNIQUE(room_id, event_id)"
        ")"));
    if (!columnExists(database_, QStringLiteral("download_jobs"), QStringLiteral("received_bytes"))) {
        execute(QStringLiteral(
            "ALTER TABLE download_jobs ADD COLUMN received_bytes INTEGER NOT NULL DEFAULT 0"));
    }
    if (!columnExists(database_, QStringLiteral("download_jobs"), QStringLiteral("total_bytes"))) {
        execute(QStringLiteral(
            "ALTER TABLE download_jobs ADD COLUMN total_bytes INTEGER"));
    }

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS activity_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_at TEXT NOT NULL,"
        "level TEXT NOT NULL,"
        "subsystem TEXT NOT NULL,"
        "message TEXT NOT NULL"
        ")"));
    execute(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_activity_log_level_id "
        "ON activity_log(level, id DESC)"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS update_check_state ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "last_checked_at TEXT,"
        "latest_version TEXT NOT NULL DEFAULT '',"
        "latest_release_url TEXT NOT NULL DEFAULT '',"
        "latest_release_name TEXT NOT NULL DEFAULT '',"
        "latest_published_at TEXT,"
        "last_error TEXT NOT NULL DEFAULT ''"
        ")"));

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tracked_uploads ("
        "sha256 TEXT PRIMARY KEY,"
        "source_kind TEXT NOT NULL DEFAULT 'library',"
        "source_path TEXT NOT NULL,"
        "bundle_path TEXT,"
        "library_path TEXT,"
        "archive_path TEXT,"
        "file_cid TEXT,"
        "thumbnail_cid TEXT,"
        "page_cid TEXT,"
        "landing_page_url TEXT,"
        "room_id TEXT NOT NULL,"
        "category TEXT NOT NULL,"
        "original_filename TEXT,"
        "mime_type TEXT,"
        "file_size INTEGER NOT NULL DEFAULT 0,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ")"));
    if (!columnExists(database_, QStringLiteral("tracked_uploads"), QStringLiteral("bundle_path"))) {
        execute(QStringLiteral("ALTER TABLE tracked_uploads ADD COLUMN bundle_path TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("tracked_uploads"), QStringLiteral("file_cid"))) {
        execute(QStringLiteral("ALTER TABLE tracked_uploads ADD COLUMN file_cid TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("tracked_uploads"), QStringLiteral("thumbnail_cid"))) {
        execute(QStringLiteral("ALTER TABLE tracked_uploads ADD COLUMN thumbnail_cid TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("tracked_uploads"), QStringLiteral("page_cid"))) {
        execute(QStringLiteral("ALTER TABLE tracked_uploads ADD COLUMN page_cid TEXT"));
    }
    if (!columnExists(database_, QStringLiteral("tracked_uploads"), QStringLiteral("landing_page_url"))) {
        execute(QStringLiteral("ALTER TABLE tracked_uploads ADD COLUMN landing_page_url TEXT"));
    }

    execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS space_auto_joins ("
        "space_room_id TEXT NOT NULL,"
        "child_room_id TEXT NOT NULL,"
        "auto_joined_by_bot INTEGER NOT NULL DEFAULT 0,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,"
        "PRIMARY KEY(space_room_id, child_room_id)"
        ")"));
}

bool AppDatabase::execute(const QString &sql) const
{
    QSqlQuery query(database_);
    return query.exec(sql);
}
