use chrono::{DateTime, Utc};
use matrix_sdk::{
    AuthSession, SessionMeta, SessionTokens,
    authentication::matrix::MatrixSession,
    ruma::{OwnedDeviceId, OwnedUserId},
};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum TimeWindowUnit {
    None,
    Day,
    Week,
    Month,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum FailedJobRetentionUnit {
    None,
    Minute,
    Hour,
    Day,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum MediaCategory {
    Images,
    Videos,
    Audio,
    Documents,
    Archives,
    Programs,
    Other,
}

impl MediaCategory {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Images => "images",
            Self::Videos => "videos",
            Self::Audio => "audio",
            Self::Documents => "documents",
            Self::Archives => "archives",
            Self::Programs => "programs",
            Self::Other => "other",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "images" => Self::Images,
            "videos" => Self::Videos,
            "audio" => Self::Audio,
            "documents" => Self::Documents,
            "archives" => Self::Archives,
            "programs" => Self::Programs,
            _ => Self::Other,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum MediaSourceKind {
    Matrix,
    Ipfs,
    LocalFile,
}

impl MediaSourceKind {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Matrix => "matrix",
            Self::Ipfs => "ipfs",
            Self::LocalFile => "localFile",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "ipfs" => Self::Ipfs,
            "localFile" => Self::LocalFile,
            _ => Self::Matrix,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum LocalAssetSourceKind {
    Library,
    Downloads,
    Archive,
}

impl LocalAssetSourceKind {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Library => "library",
            Self::Downloads => "downloads",
            Self::Archive => "archive",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "archive" => Self::Archive,
            "downloads" => Self::Downloads,
            _ => Self::Library,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum DownloadJobState {
    Queued,
    Downloading,
    CoolingDown,
    Completed,
    DuplicateCompleted,
    FailedPermanent,
    UndecryptablePending,
}

impl DownloadJobState {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Queued => "queued",
            Self::Downloading => "downloading",
            Self::CoolingDown => "coolingDown",
            Self::Completed => "completed",
            Self::DuplicateCompleted => "duplicateCompleted",
            Self::FailedPermanent => "failedPermanent",
            Self::UndecryptablePending => "undecryptablePending",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "downloading" => Self::Downloading,
            "coolingDown" => Self::CoolingDown,
            "completed" => Self::Completed,
            "duplicateCompleted" => Self::DuplicateCompleted,
            "failedPermanent" => Self::FailedPermanent,
            "undecryptablePending" => Self::UndecryptablePending,
            _ => Self::Queued,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RoomHistoryMode {
    Idle,
    InitialBackfill,
    ReconnectCatchUp,
    Complete,
}

impl RoomHistoryMode {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::InitialBackfill => "initialBackfill",
            Self::ReconnectCatchUp => "reconnectCatchUp",
            Self::Complete => "complete",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "initialBackfill" => Self::InitialBackfill,
            "reconnectCatchUp" => Self::ReconnectCatchUp,
            "complete" => Self::Complete,
            _ => Self::Idle,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ConnectionState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum IpfsRuntimeState {
    Stopped,
    Starting,
    Running,
    Error,
    Unavailable,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum AppLogLevel {
    Debug,
    Info,
    Warning,
    Error,
}

impl AppLogLevel {
    pub fn as_storage_key(self) -> &'static str {
        match self {
            Self::Debug => "debug",
            Self::Info => "info",
            Self::Warning => "warning",
            Self::Error => "error",
        }
    }

    pub fn from_storage_key(value: &str) -> Self {
        match value {
            "debug" => Self::Debug,
            "warning" => Self::Warning,
            "error" => Self::Error,
            _ => Self::Info,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum VerificationStatus {
    Unknown,
    Verified,
    Unverified,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ViewerState {
    Idle,
    Downloading,
    Ready,
    Error,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    pub homeserver_url: String,
    pub username: String,
    pub owner_user_id: String,
    pub destination_root_path: String,
    pub library_root_path: String,
    pub flat_folder_layout: bool,
    pub archive_root_path: String,
    pub archive_scan_enabled: bool,
    pub archive_scan_high_priority: bool,
    pub manual_download_root_path: String,
    pub message_limit: i32,
    pub time_window_value: i32,
    pub time_window_unit: TimeWindowUnit,
    pub retry_cooldown_minutes: i32,
    pub retry_limit: i32,
    pub download_worker_count: i32,
    pub failed_job_retention_value: i32,
    pub failed_job_retention_unit: FailedJobRetentionUnit,
    pub primary_gateway_url: String,
    pub preferred_gateway_urls: Vec<String>,
    pub autostart_enabled: bool,
    pub minimize_to_tray: bool,
    pub start_hidden: bool,
    pub bandwidth_limit_kib_per_sec: i32,
    pub preview_worker_count: i32,
    pub auto_join_space_rooms: bool,
    pub auto_download_new_media: bool,
    pub desired_power_state: bool,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StoredSession {
    pub access_token: String,
    pub refresh_token: Option<String>,
    pub user_id: String,
    pub device_id: String,
    pub homeserver_url: String,
    pub sliding_sync_version: Option<String>,
}

impl StoredSession {
    pub fn from_auth_session(session: AuthSession, homeserver_url: String) -> Self {
        match session {
            AuthSession::Matrix(matrix_session) => Self {
                access_token: matrix_session.tokens.access_token,
                refresh_token: matrix_session.tokens.refresh_token,
                user_id: matrix_session.meta.user_id.to_string(),
                device_id: matrix_session.meta.device_id.to_string(),
                homeserver_url,
                sliding_sync_version: None,
            },
            AuthSession::OAuth(oauth_session) => Self {
                access_token: oauth_session.user.tokens.access_token,
                refresh_token: oauth_session.user.tokens.refresh_token,
                user_id: oauth_session.user.meta.user_id.to_string(),
                device_id: oauth_session.user.meta.device_id.to_string(),
                homeserver_url,
                sliding_sync_version: None,
            },
            _ => Self {
                access_token: String::new(),
                refresh_token: None,
                user_id: String::new(),
                device_id: String::new(),
                homeserver_url,
                sliding_sync_version: None,
            },
        }
    }

    pub fn try_into_matrix_session(self) -> anyhow::Result<MatrixSession> {
        Ok(MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from(self.user_id.as_str())?,
                device_id: OwnedDeviceId::try_from(self.device_id.as_str())?,
            },
            tokens: SessionTokens {
                access_token: self.access_token,
                refresh_token: self.refresh_token,
            },
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomRecord {
    pub room_id: String,
    pub current_display_name: Option<String>,
    pub current_canonical_alias: Option<String>,
    pub active_folder_label: String,
    pub is_space: bool,
    pub membership: String,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomCheckpoint {
    pub room_id: String,
    pub last_processed_event_id: Option<String>,
    pub last_processed_timestamp: Option<DateTime<Utc>>,
    pub oldest_backfilled_event_id: Option<String>,
    pub oldest_backfilled_timestamp: Option<DateTime<Utc>>,
    pub historical_message_count: i32,
    pub initial_backfill_complete: bool,
    pub last_history_mode: RoomHistoryMode,
    pub last_history_run_at: Option<DateTime<Utc>>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AttachmentDiscovery {
    pub room_id: String,
    pub event_id: String,
    pub origin_server_timestamp: DateTime<Utc>,
    pub source_kind: MediaSourceKind,
    pub direct_url: Option<String>,
    pub mxc_url: String,
    pub thumbnail_source_url: Option<String>,
    pub thumbnail_cached_path: Option<String>,
    pub original_filename: Option<String>,
    pub mime_type: Option<String>,
    pub category: MediaCategory,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaCatalogItem {
    pub id: i64,
    pub room_id: Option<String>,
    pub event_id: Option<String>,
    pub source_kind: MediaSourceKind,
    pub source_url: String,
    pub gateway_page_url: Option<String>,
    pub ipfs_cid: Option<String>,
    pub title: Option<String>,
    pub mime_type: Option<String>,
    pub category: MediaCategory,
    pub thumbnail_source_url: Option<String>,
    pub local_relative_path: Option<String>,
    pub is_saved: bool,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ArchiveFileRecord {
    pub sha256: String,
    pub file_path: String,
    pub file_size: i64,
    pub modified_at: Option<DateTime<Utc>>,
    pub last_seen_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TrackedUploadRecord {
    pub sha256: String,
    pub source_kind: LocalAssetSourceKind,
    pub source_path: String,
    pub library_path: Option<String>,
    pub archive_path: Option<String>,
    pub room_id: String,
    pub category: MediaCategory,
    pub original_filename: Option<String>,
    pub mime_type: Option<String>,
    pub file_size: i64,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpaceChildDescriptor {
    pub room_id: String,
    pub via_servers: Vec<String>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomHierarchySnapshot {
    pub room_id: String,
    pub is_space: bool,
    pub display_name: Option<String>,
    pub canonical_alias: Option<String>,
    pub children: Vec<SpaceChildDescriptor>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpaceAutoJoinRecord {
    pub space_room_id: String,
    pub child_room_id: String,
    pub auto_joined_by_bot: bool,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DownloadJobRecord {
    pub id: i64,
    pub media_item_id: Option<i64>,
    pub room_id: String,
    pub event_id: String,
    pub mxc_url: String,
    pub source_kind: MediaSourceKind,
    pub direct_url: Option<String>,
    pub original_filename: Option<String>,
    pub mime_type: Option<String>,
    pub category: MediaCategory,
    pub state: DownloadJobState,
    pub retry_count: i32,
    pub next_eligible_at: Option<DateTime<Utc>>,
    pub last_failure_at: Option<DateTime<Utc>>,
    pub last_error: Option<String>,
    pub sha256: Option<String>,
    pub saved_relative_path: Option<String>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ActivityLogEntry {
    pub id: i64,
    pub created_at: DateTime<Utc>,
    pub level: AppLogLevel,
    pub subsystem: String,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoomWorkerSnapshot {
    pub room_id: String,
    pub live_watcher_active: bool,
    pub history_mode: RoomHistoryMode,
    pub history_detail: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveDownloadSnapshot {
    pub worker_id: i32,
    pub job_id: i64,
    pub room_id: String,
    pub event_id: String,
    pub filename: String,
    pub received_bytes: i64,
    pub total_bytes: Option<i64>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerificationEmoji {
    pub symbol: String,
    pub description: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VerificationSnapshot {
    pub state: VerificationStatus,
    pub device_id: Option<String>,
    pub message: String,
    pub request_flow_id: Option<String>,
    pub request_state: Option<String>,
    pub has_active_request: bool,
    pub request_ready: bool,
    pub request_can_accept: bool,
    pub has_active_sas: bool,
    pub sas_can_accept: bool,
    pub can_bootstrap_cross_signing: bool,
    pub other_device_count: u32,
    pub emojis: Vec<VerificationEmoji>,
    pub decimals: Vec<u16>,
}

impl Default for VerificationSnapshot {
    fn default() -> Self {
        Self {
            state: VerificationStatus::Unknown,
            device_id: None,
            message: String::new(),
            request_flow_id: None,
            request_state: None,
            has_active_request: false,
            request_ready: false,
            request_can_accept: false,
            has_active_sas: false,
            sas_can_accept: false,
            can_bootstrap_cross_signing: false,
            other_device_count: 0,
            emojis: Vec::new(),
            decimals: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ViewerSnapshot {
    pub session_id: u64,
    pub state: ViewerState,
    pub room_id: Option<String>,
    pub event_id: Option<String>,
    pub file_name: Option<String>,
    pub mime_type: Option<String>,
    pub category: Option<MediaCategory>,
    pub local_path: Option<String>,
    pub received_bytes: i64,
    pub total_bytes: Option<i64>,
    pub error: Option<String>,
}

impl Default for ViewerSnapshot {
    fn default() -> Self {
        Self {
            session_id: 0,
            state: ViewerState::Idle,
            room_id: None,
            event_id: None,
            file_name: None,
            mime_type: None,
            category: None,
            local_path: None,
            received_bytes: 0,
            total_bytes: None,
            error: None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BotRuntimeSnapshot {
    pub connection_state: ConnectionState,
    pub current_user_id: Option<String>,
    pub device_id: Option<String>,
    pub account_mode: Option<String>,
    pub upload_size_limit_bytes: Option<i64>,
    pub upload_size_limit_detected_at: Option<DateTime<Utc>>,
    pub ipfs: IpfsStatusSnapshot,
    pub viewer: ViewerSnapshot,
    pub verification: VerificationSnapshot,
    pub worker_states: Vec<RoomWorkerSnapshot>,
    pub active_downloads: Vec<ActiveDownloadSnapshot>,
}

impl Default for BotRuntimeSnapshot {
    fn default() -> Self {
        Self {
            connection_state: ConnectionState::Stopped,
            current_user_id: None,
            device_id: None,
            account_mode: None,
            upload_size_limit_bytes: None,
            upload_size_limit_detected_at: None,
            ipfs: IpfsStatusSnapshot::default(),
            viewer: ViewerSnapshot::default(),
            verification: VerificationSnapshot::default(),
            worker_states: Vec::new(),
            active_downloads: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GatewayStatusSnapshot {
    pub gateway_url: String,
    pub region_label: String,
    pub supports_html: bool,
    pub supports_subdomain: bool,
    pub raw_file_ok: bool,
    pub enabled_by_default: bool,
    pub last_success_at: Option<DateTime<Utc>>,
    pub recent_success_rate: f64,
    pub p50_ttfb_ms: Option<i64>,
    pub selected_as_primary: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IpfsStatusSnapshot {
    pub state: IpfsRuntimeState,
    pub kubo_binary_path: Option<String>,
    pub api_url: Option<String>,
    pub peer_id: Option<String>,
    pub primary_gateway_url: Option<String>,
    pub gateway_statuses: Vec<GatewayStatusSnapshot>,
    pub last_error: Option<String>,
}

impl Default for IpfsStatusSnapshot {
    fn default() -> Self {
        Self {
            state: IpfsRuntimeState::Stopped,
            kubo_binary_path: None,
            api_url: None,
            peer_id: None,
            primary_gateway_url: None,
            gateway_statuses: Vec::new(),
            last_error: None,
        }
    }
}
