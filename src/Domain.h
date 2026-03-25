#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

enum class AppSection {
    Browser,
    Rooms,
    Library,
    Transfers,
    Logs,
    Settings,
    Verification,
};

inline QList<AppSection> allSections()
{
    return {
        AppSection::Browser,
        AppSection::Rooms,
        AppSection::Library,
        AppSection::Transfers,
        AppSection::Logs,
        AppSection::Settings,
        AppSection::Verification,
    };
}

inline QString sectionTitle(const AppSection section)
{
    switch (section) {
    case AppSection::Browser:
        return QStringLiteral("Browser");
    case AppSection::Rooms:
        return QStringLiteral("Rooms");
    case AppSection::Library:
        return QStringLiteral("Shared Files");
    case AppSection::Transfers:
        return QStringLiteral("Transfers");
    case AppSection::Logs:
        return QStringLiteral("Logs");
    case AppSection::Settings:
        return QStringLiteral("Settings");
    case AppSection::Verification:
        return QStringLiteral("Verification");
    }
    return QStringLiteral("Unknown");
}

inline int sectionIndex(const AppSection targetSection)
{
    const QList<AppSection> sections = allSections();
    for (int index = 0; index < sections.size(); ++index) {
        if (sections.at(index) == targetSection) {
            return index;
        }
    }
    return -1;
}

enum class AccountMode {
    DedicatedBot,
    SharedOwnerAccount,
};

inline QString accountModeTitle(const AccountMode mode)
{
    switch (mode) {
    case AccountMode::DedicatedBot:
        return QStringLiteral("dedicatedBot");
    case AccountMode::SharedOwnerAccount:
        return QStringLiteral("sharedOwnerAccount");
    }
    return QStringLiteral("unknown");
}

enum class TimeWindowUnit {
    None,
    Day,
    Week,
    Month,
};

inline QList<TimeWindowUnit> allTimeWindowUnits()
{
    return {TimeWindowUnit::None, TimeWindowUnit::Day, TimeWindowUnit::Week, TimeWindowUnit::Month};
}

inline QString timeWindowUnitTitle(const TimeWindowUnit unit)
{
    switch (unit) {
    case TimeWindowUnit::None:
        return QStringLiteral("Disabled");
    case TimeWindowUnit::Day:
        return QStringLiteral("Days");
    case TimeWindowUnit::Week:
        return QStringLiteral("Weeks");
    case TimeWindowUnit::Month:
        return QStringLiteral("Months");
    }
    return QStringLiteral("Unknown");
}

enum class FailedJobRetentionUnit {
    None,
    Minute,
    Hour,
    Day,
};

inline QList<FailedJobRetentionUnit> allFailedJobRetentionUnits()
{
    return {
        FailedJobRetentionUnit::None,
        FailedJobRetentionUnit::Minute,
        FailedJobRetentionUnit::Hour,
        FailedJobRetentionUnit::Day,
    };
}

inline QString failedJobRetentionUnitTitle(const FailedJobRetentionUnit unit)
{
    switch (unit) {
    case FailedJobRetentionUnit::None:
        return QStringLiteral("Disabled");
    case FailedJobRetentionUnit::Minute:
        return QStringLiteral("Minutes");
    case FailedJobRetentionUnit::Hour:
        return QStringLiteral("Hours");
    case FailedJobRetentionUnit::Day:
        return QStringLiteral("Days");
    }
    return QStringLiteral("Unknown");
}

enum class MediaCategory {
    Images,
    Videos,
    Audio,
    Documents,
    Archives,
    Programs,
    Other,
};

inline QString mediaCategoryTitle(const MediaCategory category)
{
    switch (category) {
    case MediaCategory::Images:
        return QStringLiteral("images");
    case MediaCategory::Videos:
        return QStringLiteral("videos");
    case MediaCategory::Audio:
        return QStringLiteral("audio");
    case MediaCategory::Documents:
        return QStringLiteral("documents");
    case MediaCategory::Archives:
        return QStringLiteral("archives");
    case MediaCategory::Programs:
        return QStringLiteral("programs");
    case MediaCategory::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

enum class MediaSourceKind {
    Matrix,
    Ipfs,
    LocalFile,
};

inline QString mediaSourceKindTitle(const MediaSourceKind sourceKind)
{
    switch (sourceKind) {
    case MediaSourceKind::Matrix:
        return QStringLiteral("matrix");
    case MediaSourceKind::Ipfs:
        return QStringLiteral("ipfs");
    case MediaSourceKind::LocalFile:
        return QStringLiteral("localFile");
    }
    return QStringLiteral("matrix");
}

enum class DownloadJobState {
    Queued,
    Downloading,
    CoolingDown,
    Completed,
    DuplicateCompleted,
    FailedPermanent,
    UndecryptablePending,
};

inline QString downloadJobStateTitle(const DownloadJobState state)
{
    switch (state) {
    case DownloadJobState::Queued:
        return QStringLiteral("queued");
    case DownloadJobState::Downloading:
        return QStringLiteral("downloading");
    case DownloadJobState::CoolingDown:
        return QStringLiteral("coolingDown");
    case DownloadJobState::Completed:
        return QStringLiteral("completed");
    case DownloadJobState::DuplicateCompleted:
        return QStringLiteral("duplicateCompleted");
    case DownloadJobState::FailedPermanent:
        return QStringLiteral("failedPermanent");
    case DownloadJobState::UndecryptablePending:
        return QStringLiteral("undecryptablePending");
    }
    return QStringLiteral("queued");
}

enum class RoomHistoryMode {
    Idle,
    InitialBackfill,
    ReconnectCatchUp,
    Complete,
};

inline QString roomHistoryModeTitle(const RoomHistoryMode mode)
{
    switch (mode) {
    case RoomHistoryMode::Idle:
        return QStringLiteral("idle");
    case RoomHistoryMode::InitialBackfill:
        return QStringLiteral("initialBackfill");
    case RoomHistoryMode::ReconnectCatchUp:
        return QStringLiteral("reconnectCatchUp");
    case RoomHistoryMode::Complete:
        return QStringLiteral("complete");
    }
    return QStringLiteral("idle");
}

enum class ConnectionState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error,
};

inline QString connectionStateTitle(const ConnectionState state)
{
    switch (state) {
    case ConnectionState::Stopped:
        return QStringLiteral("Disconnected");
    case ConnectionState::Starting:
        return QStringLiteral("Starting");
    case ConnectionState::Running:
        return QStringLiteral("Connected");
    case ConnectionState::Stopping:
        return QStringLiteral("Stopping");
    case ConnectionState::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Disconnected");
}

enum class IpfsRuntimeState {
    Stopped,
    Starting,
    Running,
    Error,
    Unavailable,
};

inline QString ipfsRuntimeStateTitle(const IpfsRuntimeState state)
{
    switch (state) {
    case IpfsRuntimeState::Stopped:
        return QStringLiteral("Stopped");
    case IpfsRuntimeState::Starting:
        return QStringLiteral("Starting");
    case IpfsRuntimeState::Running:
        return QStringLiteral("Running");
    case IpfsRuntimeState::Error:
        return QStringLiteral("Error");
    case IpfsRuntimeState::Unavailable:
        return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Stopped");
}

enum class AppLogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

inline QString appLogLevelTitle(const AppLogLevel level)
{
    switch (level) {
    case AppLogLevel::Debug:
        return QStringLiteral("debug");
    case AppLogLevel::Info:
        return QStringLiteral("info");
    case AppLogLevel::Warning:
        return QStringLiteral("warning");
    case AppLogLevel::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

enum class VerificationStatus {
    Unknown,
    Verified,
    Unverified,
};

inline QString verificationStatusTitle(const VerificationStatus status)
{
    switch (status) {
    case VerificationStatus::Unknown:
        return QStringLiteral("unknown");
    case VerificationStatus::Verified:
        return QStringLiteral("verified");
    case VerificationStatus::Unverified:
        return QStringLiteral("unverified");
    }
    return QStringLiteral("unknown");
}

enum class ViewerState {
    Idle,
    Downloading,
    Ready,
    Error,
};

inline QString viewerStateTitle(const ViewerState state)
{
    switch (state) {
    case ViewerState::Idle:
        return QStringLiteral("idle");
    case ViewerState::Downloading:
        return QStringLiteral("downloading");
    case ViewerState::Ready:
        return QStringLiteral("ready");
    case ViewerState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

struct AppSettings {
    QString homeserverUrl;
    QString username;
    QString ownerUserId;
    QString destinationRootPath;
    QString libraryRootPath;
    bool flatFolderLayout = false;
    QString archiveRootPath;
    bool archiveScanEnabled = false;
    bool archiveScanHighPriority = false;
    QString manualDownloadRootPath;
    int messageLimit = 5000;
    int timeWindowValue = 0;
    TimeWindowUnit timeWindowUnit = TimeWindowUnit::None;
    int retryCooldownMinutes = 5;
    int retryLimit = 10;
    int downloadWorkerCount = 1;
    int failedJobRetentionValue = 0;
    FailedJobRetentionUnit failedJobRetentionUnit = FailedJobRetentionUnit::None;
    QString primaryGatewayUrl = QStringLiteral("https://dweb.link");
    QStringList preferredGatewayUrls {
        QStringLiteral("https://dweb.link"),
        QStringLiteral("https://ipfs.io"),
        QStringLiteral("https://eu.orbitor.dev"),
        QStringLiteral("https://ipfs.ecolatam.com"),
        QStringLiteral("https://apac.orbitor.dev"),
        QStringLiteral("https://4everland.io"),
    };
    bool autostartEnabled = false;
    bool minimizeToTray = true;
    bool startHidden = false;
    int bandwidthLimitKiBPerSec = 0;
    int previewWorkerCount = 1;
    bool autoJoinSpaceRooms = false;
    bool autoDownloadNewMedia = false;
    bool desiredPowerState = false;

    static AppSettings defaults(const QString &destinationRootPath)
    {
        AppSettings settings;
        settings.homeserverUrl = QStringLiteral("https://matrix.org");
        settings.username = QStringLiteral("");
        settings.ownerUserId = QStringLiteral("");
        settings.destinationRootPath = destinationRootPath;
        settings.libraryRootPath = destinationRootPath + QStringLiteral("/Shared Files");
        settings.flatFolderLayout = false;
        settings.archiveRootPath = QStringLiteral("");
        settings.manualDownloadRootPath = destinationRootPath + QStringLiteral("/Downloads");
        return settings;
    }
};

struct StoredSession {
    QString accessToken;
    QString refreshToken;
    QString userId;
    QString deviceId;
    QString homeserverUrl;
    QString oidcData;
    QString slidingSyncVersion;
};

struct RoomRecord {
    QString roomId;
    QString currentDisplayName;
    QString currentCanonicalAlias;
    QString activeFolderLabel;
    bool isSpace = false;
    QString membership;
    int discoveredMediaCount = 0;
    QDateTime updatedAt;
};

struct RoomCheckpoint {
    QString roomId;
    QString lastProcessedEventId;
    QDateTime lastProcessedTimestamp;
    QString oldestBackfilledEventId;
    QDateTime oldestBackfilledTimestamp;
    int historicalMessageCount = 0;
    bool initialBackfillComplete = false;
    RoomHistoryMode lastHistoryMode = RoomHistoryMode::Idle;
    QDateTime lastHistoryRunAt;
};

struct AttachmentDiscovery {
    QString roomId;
    QString eventId;
    QDateTime originServerTimestamp;
    MediaSourceKind sourceKind = MediaSourceKind::Matrix;
    QString directUrl;
    QString mxcUrl;
    QString thumbnailSourceUrl;
    QString thumbnailCachedPath;
    QString originalFilename;
    QString mimeType;
    MediaCategory category = MediaCategory::Other;
};

struct MediaCatalogItem {
    qint64 id = 0;
    QString roomId;
    QString eventId;
    MediaSourceKind sourceKind = MediaSourceKind::Matrix;
    QString sourceUrl;
    QString gatewayPageUrl;
    QString ipfsCid;
    QString title;
    QString mimeType;
    MediaCategory category = MediaCategory::Other;
    QString thumbnailSourceUrl;
    QString localRelativePath;
    bool isSaved = false;
    QDateTime createdAt;
    QDateTime updatedAt;
};

struct SpaceChildDescriptor {
    QString roomId;
    QStringList viaServers;
};

struct RoomHierarchySnapshot {
    QString roomId;
    bool isSpace = false;
    QString displayName;
    QString canonicalAlias;
    QVector<SpaceChildDescriptor> children;
};

struct SpaceAutoJoinRecord {
    QString spaceRoomId;
    QString childRoomId;
    bool autoJoinedByBot = false;
    QDateTime createdAt;
    QDateTime updatedAt;
};

struct DownloadJobRecord {
    qint64 id = 0;
    qint64 mediaItemId = 0;
    QString roomId;
    QString eventId;
    QString mxcUrl;
    MediaSourceKind sourceKind = MediaSourceKind::Matrix;
    QString directUrl;
    QString originalFilename;
    QString mimeType;
    MediaCategory category = MediaCategory::Other;
    DownloadJobState state = DownloadJobState::Queued;
    int retryCount = 0;
    QDateTime nextEligibleAt;
    QDateTime lastFailureAt;
    QString lastError;
    QString sha256;
    QString savedRelativePath;
    QDateTime createdAt;
    QDateTime updatedAt;
};

struct ActivityLogEntry {
    qint64 id = 0;
    QDateTime createdAt;
    AppLogLevel level = AppLogLevel::Info;
    QString subsystem;
    QString message;
};

struct RoomWorkerSnapshot {
    QString roomId;
    bool liveWatcherActive = false;
    RoomHistoryMode historyMode = RoomHistoryMode::Idle;
    QString historyDetail = QStringLiteral("Idle");
};

struct ActiveDownloadSnapshot {
    int workerId = 0;
    qint64 jobId = 0;
    QString roomId;
    QString eventId;
    QString filename;
    qint64 receivedBytes = 0;
    qint64 totalBytes = -1;
};

struct VerificationEmoji {
    QString symbol;
    QString description;
};

struct VerificationSnapshot {
    VerificationStatus state = VerificationStatus::Unknown;
    QString deviceId;
    QString message;
    QString requestFlowId;
    QString requestState;
    bool hasActiveRequest = false;
    bool requestReady = false;
    bool requestCanAccept = false;
    bool hasActiveSas = false;
    bool sasCanAccept = false;
    bool canBootstrapCrossSigning = false;
    int otherDeviceCount = 0;
    QVector<VerificationEmoji> emojis;
    QVector<quint16> decimals;
};

struct ViewerSnapshot {
    quint64 sessionId = 0;
    ViewerState state = ViewerState::Idle;
    QString roomId;
    QString eventId;
    QString fileName;
    QString mimeType;
    MediaCategory category = MediaCategory::Other;
    QString localPath;
    qint64 receivedBytes = 0;
    qint64 totalBytes = -1;
    QString error;
};

struct BotRuntimeSnapshot {
    ConnectionState connectionState = ConnectionState::Stopped;
    QString currentUserId;
    QString deviceId;
    QString accountMode;
    qint64 uploadSizeLimitBytes = 0;
    QDateTime uploadSizeLimitDetectedAt;
    struct IpfsStatusSnapshot {
        IpfsRuntimeState state = IpfsRuntimeState::Stopped;
        QString kuboBinaryPath;
        QString apiUrl;
        QString peerId;
        QString primaryGatewayUrl;
        QString lastError;
    } ipfs;
    ViewerSnapshot viewer;
    VerificationSnapshot verification;
    QVector<RoomWorkerSnapshot> workerStates;
    QVector<ActiveDownloadSnapshot> activeDownloads;
};

struct GatewayStatusSnapshot {
    QString gatewayUrl;
    QString regionLabel;
    bool supportsHtml = false;
    bool supportsSubdomain = false;
    bool rawFileOk = false;
    bool enabledByDefault = false;
    QDateTime lastSuccessAt;
    double recentSuccessRate = 0.0;
    qint64 p50TtfbMs = 0;
    bool selectedAsPrimary = false;
};

struct UpdateCheckState {
    QDateTime lastCheckedAt;
    QString latestVersion;
    QString latestReleaseUrl;
    QString latestReleaseName;
    QDateTime latestPublishedAt;
    QString lastError;
};
