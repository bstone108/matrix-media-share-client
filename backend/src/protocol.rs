use serde::{Deserialize, Serialize};

use crate::domain::{AppSettings, BotRuntimeSnapshot};

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CommandEnvelope {
    pub id: u64,
    pub command: Command,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
pub enum Command {
    Start {
        settings: AppSettings,
        password: String,
    },
    Stop,
    SaveSettings {
        settings: AppSettings,
        password: String,
    },
    ShareLocalFile {
        room_id: String,
        file_path: String,
    },
    ShareLocalFiles {
        room_id: String,
        file_paths: Vec<String>,
    },
    ImportIpfsLink {
        link: String,
    },
    OpenDiscovery {
        room_id: String,
        event_id: String,
    },
    OpenMedia {
        media_item_id: i64,
    },
    SaveViewerItem {
        media_item_id: i64,
    },
    QueueDownload {
        media_item_id: i64,
    },
    CancelTransfer {
        transfer_id: i64,
    },
    RetryTransfer {
        transfer_id: i64,
    },
    RefreshCatalog,
    JoinRoom {
        room_id_or_alias: String,
    },
    LeaveRoom {
        room_id: String,
    },
    RequestVerification,
    StartSasVerification,
    ApproveVerification,
    DeclineVerification,
    ResetHistoryScans,
    Shutdown,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
pub enum ServerEvent {
    Response {
        id: u64,
        ok: bool,
        error: Option<String>,
    },
    Runtime {
        snapshot: BotRuntimeSnapshot,
    },
}
