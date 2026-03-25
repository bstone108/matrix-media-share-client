use std::{
    collections::{HashMap, HashSet},
    io::Cursor,
    path::{Path, PathBuf},
    process::Stdio,
    sync::Arc,
    time::Duration,
};

use anyhow::{Context, Result, anyhow};
use chrono::{DateTime, Utc};
use eyeball_im::{Vector, VectorDiff};
use filetime::{FileTime, set_file_mtime};
use futures_util::StreamExt;
use matrix_sdk::{
    attachment::AttachmentConfig,
    Client, Room,
    encryption::{
        VerificationState,
        verification::{SasVerification, VerificationRequest, VerificationRequestState},
    },
    media::{MediaFormat, MediaRequestParameters},
    ruma::{
        OwnedMxcUri, OwnedServerName, RoomAliasId, RoomOrAliasId, ServerName, UserId,
        api::client::uiaa,
        events::room::{
            MediaSource,
            message::{MessageType, RoomMessageEventContent, TextMessageEventContent},
        },
    },
};
use matrix_sdk_ui::{
    sync_service::{State as SyncState, SyncService},
    timeline::{RoomExt, TimelineItem},
};
use mime::Mime;
use reqwest::header::{ACCEPT, AUTHORIZATION, USER_AGENT};
use serde::Serialize;
use sha2::{Digest, Sha256};
use tokio::{
    fs::{self as tokio_fs, File as TokioFile},
    io::{AsyncReadExt, AsyncWriteExt},
    sync::{Mutex, RwLock, Semaphore, mpsc},
    task::JoinHandle,
    time::{sleep, timeout},
};
use uuid::Uuid;

use crate::{
    app_paths::AppPaths,
    database::AppDatabase,
    domain::{
        ActiveDownloadSnapshot, AppLogLevel, AppSettings, AttachmentDiscovery, BotRuntimeSnapshot,
        ConnectionState, DownloadJobRecord, FailedJobRetentionUnit, LocalAssetSourceKind,
        MediaCategory, MediaSourceKind, RoomCheckpoint, RoomHierarchySnapshot, RoomHistoryMode,
        RoomWorkerSnapshot, SpaceChildDescriptor, StoredSession, TrackedUploadRecord, ViewerSnapshot,
        ViewerState,
        VerificationEmoji, VerificationSnapshot, VerificationStatus,
    },
    gateway_registry::{BOOTSTRAP_PRIMARY_GATEWAY, page_url, raw_file_url},
    ipfs_service::IpfsService,
    media_classification,
    protocol::{Command, ServerEvent},
    room_catalog::RoomCatalog,
    secret_store::SecretStore,
};

const ROOM_REFRESH_INTERVAL: Duration = Duration::from_secs(15);
const DOWNLOAD_IDLE_DELAY: Duration = Duration::from_secs(1);
const DOWNLOAD_ERROR_DELAY: Duration = Duration::from_secs(2);
const ROOM_THUMBNAIL_WARM_BATCH_SIZE: i64 = 48;
const ROOM_THUMBNAIL_WARM_ITEM_DELAY: Duration = Duration::from_millis(120);
const ROOM_THUMBNAIL_WARM_BATCH_DELAY: Duration = Duration::from_millis(500);
const SHARE_QUEUE_DELAY: Duration = Duration::from_secs(10);
const MAX_DISCOVERY_CACHE_ENTRIES: usize = 12_000;
const MANUAL_IPFS_ROOM_ID: &str = "__manual_ipfs__";
const MANUAL_IPFS_FOLDER_LABEL: &str = "Imported IPFS";
const DOWNLOAD_PROGRESS_PERSIST_BYTES: i64 = 256 * 1024;
const HASH_ROOT_ARCHIVE: &str = "archive";
const HASH_ROOT_DOWNLOADS: &str = "downloads";
const HASH_ROOT_SHARED: &str = "shared";
const MANAGED_SHARE_SCAN_INTERVAL: Duration = Duration::from_secs(300);
const MANAGED_SHARE_ITEM_DELAY: Duration = Duration::from_millis(500);

pub enum CommandOutcome {
    Continue,
    Shutdown,
}

pub struct BackendService {
    paths: AppPaths,
    database: AppDatabase,
    secret_store: SecretStore,
    runtime: RuntimeStore,
    event_tx: mpsc::UnboundedSender<ServerEvent>,
    running: Option<RunningService>,
}

struct RunningService {
    context: Arc<RunningContext>,
    handles: Vec<JoinHandle<()>>,
}

struct RunningContext {
    paths: AppPaths,
    database: AppDatabase,
    secret_store: SecretStore,
    room_catalog: RoomCatalog,
    runtime: RuntimeStore,
    client: Client,
    sync_service: Arc<SyncService>,
    settings: Arc<RwLock<AppSettings>>,
    ipfs: Arc<IpfsService>,
    downloads: Arc<DownloadManager>,
    share_slots: Arc<Semaphore>,
    room_workers: Arc<Mutex<HashMap<String, RoomWorkerState>>>,
    focused_room_id: Arc<RwLock<Option<String>>>,
    handled_event_ids: Arc<Mutex<HashSet<String>>>,
    verification: Arc<Mutex<VerificationContext>>,
}

struct RoomWorkerState {
    live_task: Option<JoinHandle<()>>,
    history_task: Option<JoinHandle<()>>,
    thumbnail_task: Option<JoinHandle<()>>,
    live_watcher_active: bool,
    history_mode: RoomHistoryMode,
    history_detail: String,
}

impl RoomWorkerState {
    fn new() -> Self {
        Self {
            live_task: None,
            history_task: None,
            thumbnail_task: None,
            live_watcher_active: false,
            history_mode: RoomHistoryMode::Idle,
            history_detail: "Idle".to_owned(),
        }
    }

    fn snapshot(&self, room_id: &str) -> RoomWorkerSnapshot {
        RoomWorkerSnapshot {
            room_id: room_id.to_owned(),
            live_watcher_active: self.live_watcher_active,
            history_mode: self.history_mode,
            history_detail: self.history_detail.clone(),
        }
    }
}

#[derive(Default)]
struct VerificationContext {
    request: Option<VerificationRequest>,
    request_task: Option<JoinHandle<()>>,
    sas: Option<SasVerification>,
    sas_task: Option<JoinHandle<()>>,
}

#[derive(Clone)]
struct RuntimeStore {
    state: Arc<Mutex<BotRuntimeSnapshot>>,
    event_tx: mpsc::UnboundedSender<ServerEvent>,
}

impl RuntimeStore {
    fn new(event_tx: mpsc::UnboundedSender<ServerEvent>) -> Self {
        Self {
            state: Arc::new(Mutex::new(BotRuntimeSnapshot::default())),
            event_tx,
        }
    }

    async fn snapshot(&self) -> BotRuntimeSnapshot {
        self.state.lock().await.clone()
    }

    async fn replace(&self, next: BotRuntimeSnapshot) {
        *self.state.lock().await = next.clone();
        let _ = self.event_tx.send(ServerEvent::Runtime { snapshot: next });
    }

    async fn mutate<F>(&self, callback: F)
    where
        F: FnOnce(&mut BotRuntimeSnapshot),
    {
        let snapshot = {
            let mut state = self.state.lock().await;
            callback(&mut state);
            state.clone()
        };
        let _ = self.event_tx.send(ServerEvent::Runtime { snapshot });
    }
}

#[derive(Clone)]
struct DownloadManager {
    database: AppDatabase,
    room_catalog: RoomCatalog,
    paths: AppPaths,
    runtime: RuntimeStore,
    settings: Arc<RwLock<AppSettings>>,
    client: Client,
    workers: Arc<Mutex<HashMap<i32, JoinHandle<()>>>>,
}

#[derive(Clone, Serialize)]
struct EncodedMediaSource<'a> {
    #[serde(rename = "kind")]
    source_kind: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    plain: Option<&'a OwnedMxcUri>,
    #[serde(skip_serializing_if = "Option::is_none")]
    encrypted: Option<&'a matrix_sdk::ruma::events::room::EncryptedFile>,
}

impl BackendService {
    pub async fn new(
        paths: AppPaths,
        event_tx: mpsc::UnboundedSender<ServerEvent>,
    ) -> Result<Self> {
        paths.ensure_directories()?;
        let database = AppDatabase::open(&paths.database_path).await?;
        let secret_store = SecretStore::new(paths.secret_store_path.clone());
        let runtime = RuntimeStore::new(event_tx.clone());

        Ok(Self {
            paths,
            database,
            secret_store,
            runtime,
            event_tx,
            running: None,
        })
    }

    pub async fn handle_command(&mut self, command: Command) -> Result<CommandOutcome> {
        match command {
            Command::Start { settings, password } => {
                self.start(settings, password).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::Stop => {
                self.stop().await?;
                Ok(CommandOutcome::Continue)
            }
            Command::SaveSettings { settings, password } => {
                self.save_settings(settings, password).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::ShareLocalFile { room_id, file_path } => {
                let running = self.running_context()?;
                queue_share_requests(&running, &room_id, vec![file_path]).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::ShareLocalFiles { room_id, file_paths } => {
                let running = self.running_context()?;
                queue_share_requests(&running, &room_id, file_paths).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::ImportIpfsLink { link } => {
                let running = self.running_context()?;
                import_ipfs_link(&running, &link).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::DeleteSharedItem { sha256 } => {
                delete_shared_item(&self.database, &self.paths, self.running.as_ref(), &sha256).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::OpenDiscovery { room_id, event_id } => {
                let running = self.running_context()?;
                tokio::spawn(async move {
                    if let Err(error) = open_discovery(&running, &room_id, &event_id).await {
                        let _ = running
                            .runtime
                            .mutate(|runtime| {
                                runtime
                                    .active_downloads
                                    .retain(|download| download.worker_id != 0);
                                runtime.viewer.state = ViewerState::Error;
                                runtime.viewer.room_id = Some(room_id.clone());
                                runtime.viewer.event_id = Some(event_id.clone());
                                runtime.viewer.error = Some(error.to_string());
                            })
                            .await;
                        let _ = running
                            .database
                            .insert_log(
                                AppLogLevel::Error,
                                "viewer",
                                &format!("Failed to open browser item {event_id} from {room_id}: {error:#}"),
                            )
                            .await;
                    }
                });
                Ok(CommandOutcome::Continue)
            }
            Command::FocusRoom { room_id } => {
                let running = self.running_context()?;
                tokio::spawn(async move {
                    if let Err(error) = focus_room_now(&running, &room_id).await {
                        let _ = running
                            .database
                            .insert_log(
                                AppLogLevel::Warning,
                                "rooms",
                                &format!("Failed to prioritize room {room_id}: {error:#}"),
                            )
                            .await;
                    }
                });
                Ok(CommandOutcome::Continue)
            }
            Command::OpenMedia { media_item_id } => {
                let running = self.running_context()?;
                running
                    .database
                    .insert_log(AppLogLevel::Info, "browser", &format!("Opened media item {media_item_id}"))
                    .await?;
                Ok(CommandOutcome::Continue)
            }
            Command::SaveViewerItem { media_item_id } => {
                let running = self.running_context()?;
                running
                    .database
                    .insert_log(AppLogLevel::Info, "library", &format!("Requested save for media item {media_item_id}"))
                    .await?;
                Ok(CommandOutcome::Continue)
            }
            Command::QueueDownload { media_item_id } => {
                let running = self.running_context()?;
                running
                    .database
                    .insert_log(AppLogLevel::Info, "transfers", &format!("Queued media item {media_item_id}"))
                    .await?;
                Ok(CommandOutcome::Continue)
            }
            Command::CancelTransfer { transfer_id } => {
                let running = self.running_context()?;
                running
                    .database
                    .insert_log(AppLogLevel::Info, "transfers", &format!("Cancelled transfer {transfer_id}"))
                    .await?;
                Ok(CommandOutcome::Continue)
            }
            Command::RetryTransfer { transfer_id } => {
                let running = self.running_context()?;
                running
                    .database
                    .insert_log(AppLogLevel::Info, "transfers", &format!("Retried transfer {transfer_id}"))
                    .await?;
                Ok(CommandOutcome::Continue)
            }
            Command::RefreshCatalog => {
                let running = self.running_context()?;
                refresh_joined_rooms(running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::JoinRoom { room_id_or_alias } => {
                let running = self.running_context()?;
                join_room(&running, &room_id_or_alias, &[]).await?;
                refresh_joined_rooms(running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::LeaveRoom { room_id } => {
                let running = self.running_context()?;
                leave_room(&running, &room_id).await?;
                refresh_joined_rooms(running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::RequestVerification => {
                let running = self.running_context()?;
                request_verification(&running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::StartSasVerification => {
                let running = self.running_context()?;
                start_sas_verification(&running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::ApproveVerification => {
                let running = self.running_context()?;
                approve_verification(&running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::DeclineVerification => {
                let running = self.running_context()?;
                decline_verification(&running).await?;
                Ok(CommandOutcome::Continue)
            }
            Command::ResetHistoryScans => {
                self.reset_history_scans().await?;
                Ok(CommandOutcome::Continue)
            }
            Command::Shutdown => {
                self.stop().await?;
                Ok(CommandOutcome::Shutdown)
            }
        }
    }

    fn running_context(&self) -> Result<Arc<RunningContext>> {
        self.running
            .as_ref()
            .map(|running| running.context.clone())
            .ok_or_else(|| anyhow!("Matrix client is not connected."))
    }

    async fn start(&mut self, settings: AppSettings, password: String) -> Result<()> {
        if self.running.is_some() {
            self.stop().await?;
        }

        self.paths.ensure_directories()?;
        self.database.save_settings(&settings).await?;
        self.secret_store.save_password(&password)?;
        cleanup_temp_files(&self.paths.temp_downloads_path).await?;
        self.database.reset_interrupted_jobs().await?;

        self.runtime
            .replace(BotRuntimeSnapshot {
                connection_state: ConnectionState::Starting,
                ..BotRuntimeSnapshot::default()
            })
            .await;

        let client = connect_client(&self.paths, &self.secret_store, &settings, &password).await?;
        persist_current_session(&self.secret_store, &settings.homeserver_url, &client).await?;

        let current_user_id = runtime_user_id_from_settings(&settings)
            .ok_or_else(|| anyhow!("Connected Matrix client has no configured user id"))?;
        let device_id = client
            .device_id()
            .map(ToString::to_string)
            .ok_or_else(|| anyhow!("Connected Matrix client has no device id"))?;
        let account_mode = "client".to_owned();

        let sync_service = Arc::new(SyncService::builder(client.clone()).build().await?);
        let settings_store = Arc::new(RwLock::new(settings.clone()));
        let room_catalog = RoomCatalog::new(self.database.clone());
        let ipfs = Arc::new(IpfsService::new(self.paths.clone()));
        let ipfs_status = ipfs.start().await.unwrap_or_default();
        let upload_size_limit_bytes = detect_upload_limit(&client).await;
        let share_slots = Arc::new(Semaphore::new(1));
        let downloads = Arc::new(DownloadManager::new(
            self.database.clone(),
            room_catalog.clone(),
            self.paths.clone(),
            self.runtime.clone(),
            settings_store.clone(),
            client.clone(),
        ));
        let context = Arc::new(RunningContext {
            paths: self.paths.clone(),
            database: self.database.clone(),
            secret_store: self.secret_store.clone(),
            room_catalog,
            runtime: self.runtime.clone(),
            client: client.clone(),
            sync_service: sync_service.clone(),
            settings: settings_store,
            ipfs: ipfs.clone(),
            downloads: downloads.clone(),
            share_slots: share_slots.clone(),
            room_workers: Arc::new(Mutex::new(HashMap::new())),
            focused_room_id: Arc::new(RwLock::new(None)),
            handled_event_ids: Arc::new(Mutex::new(HashSet::new())),
            verification: Arc::new(Mutex::new(VerificationContext::default())),
        });

        self.runtime
            .mutate(|runtime| {
                runtime.connection_state = ConnectionState::Starting;
                runtime.current_user_id = Some(current_user_id.clone());
                runtime.device_id = Some(device_id.clone());
                runtime.account_mode = Some(account_mode.clone());
                runtime.upload_size_limit_bytes = upload_size_limit_bytes;
                runtime.upload_size_limit_detected_at = upload_size_limit_bytes.map(|_| Utc::now());
                runtime.ipfs = ipfs_status.clone();
            })
            .await;
        refresh_verification_snapshot(&context).await?;

        downloads.start().await;
        sync_service.start().await;
        publish_sync_state(&context, &sync_service.state().get()).await;
        refresh_joined_rooms(context.clone()).await?;

        let handles = vec![
            tokio::spawn(watch_sync_state(context.clone())),
            tokio::spawn(watch_session_changes(context.clone())),
            tokio::spawn(watch_verification_state(context.clone())),
            tokio::spawn(periodic_room_refresh(context.clone())),
            tokio::spawn(archive_scan_loop(context.clone())),
            tokio::spawn(managed_share_maintenance_loop(context.clone())),
        ];
        self.running = Some(RunningService { context, handles });

        Ok(())
    }

    async fn stop(&mut self) -> Result<()> {
        let Some(running) = self.running.take() else {
            self.runtime
                .replace(BotRuntimeSnapshot {
                    connection_state: ConnectionState::Stopped,
                    ..BotRuntimeSnapshot::default()
                })
                .await;
            return Ok(());
        };

        let context = running.context;
        for handle in running.handles {
            handle.abort();
        }

        {
            let mut verification = context.verification.lock().await;
            if let Some(task) = verification.request_task.take() {
                task.abort();
            }
            if let Some(task) = verification.sas_task.take() {
                task.abort();
            }
            verification.request = None;
            verification.sas = None;
        }

        stop_all_room_workers(&context).await;
        context.downloads.stop().await;
        context.sync_service.stop().await;
        context.ipfs.stop().await;

        self.runtime
            .replace(BotRuntimeSnapshot {
                connection_state: ConnectionState::Stopped,
                ..BotRuntimeSnapshot::default()
            })
            .await;
        Ok(())
    }

    async fn save_settings(&mut self, settings: AppSettings, password: String) -> Result<()> {
        let previous_settings = self
            .running
            .as_ref()
            .map(|running| running.context.settings.clone());
        let previous_settings = match previous_settings {
            Some(settings_store) => settings_store.read().await.clone(),
            None => self.database.load_settings("").await?,
        };
        let previous_password = self.secret_store.load_password().unwrap_or_default();

        self.database.save_settings(&settings).await?;
        self.secret_store.save_password(&password)?;

        if let Some(running) = &self.running {
            *running.context.settings.write().await = settings.clone();
        }

        let requires_restart = previous_settings.homeserver_url != settings.homeserver_url
            || previous_settings.username != settings.username
            || previous_settings.owner_user_id != settings.owner_user_id
            || previous_password != password;

        if requires_restart && settings.desired_power_state {
            self.start(settings, password).await?;
            return Ok(());
        }

        if !settings.desired_power_state {
            self.stop().await?;
            return Ok(());
        }

        if let Some(running) = &self.running {
            running.context.downloads.restart().await;
            refresh_joined_rooms(running.context.clone()).await?;
        }

        Ok(())
    }

    async fn reset_history_scans(&mut self) -> Result<()> {
        self.database
            .reset_all_history_scans_for_full_rescan()
            .await?;
        if let Some(running) = &self.running {
            stop_all_room_workers(&running.context).await;
            running.context.downloads.restart().await;
            refresh_joined_rooms(running.context.clone()).await?;
        }
        Ok(())
    }
}

impl DownloadManager {
    fn new(
        database: AppDatabase,
        room_catalog: RoomCatalog,
        paths: AppPaths,
        runtime: RuntimeStore,
        settings: Arc<RwLock<AppSettings>>,
        client: Client,
    ) -> Self {
        Self {
            database,
            room_catalog,
            paths,
            runtime,
            settings,
            client,
            workers: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    async fn start(&self) {
        self.stop().await;

        let worker_count = self.settings.read().await.download_worker_count.clamp(1, 6);
        let mut workers = self.workers.lock().await;
        for worker_id in 1..=worker_count {
            let manager = self.clone();
            workers.insert(
                worker_id,
                tokio::spawn(async move {
                    manager.worker_loop(worker_id).await;
                }),
            );
        }
    }

    async fn stop(&self) {
        let mut workers = self.workers.lock().await;
        let handles = workers
            .drain()
            .map(|(_, handle)| handle)
            .collect::<Vec<_>>();
        drop(workers);

        for handle in handles {
            handle.abort();
        }

        self.runtime
            .mutate(|runtime| {
                runtime.active_downloads.clear();
                runtime.viewer = ViewerSnapshot::default();
            })
            .await;
    }

    async fn restart(&self) {
        self.start().await;
    }

    async fn worker_loop(self, worker_id: i32) {
        loop {
            let connection_state = self.runtime.snapshot().await.connection_state;
            if connection_state != ConnectionState::Running {
                sleep(DOWNLOAD_IDLE_DELAY).await;
                continue;
            }

            match self.database.claim_next_eligible_job(Utc::now()).await {
                Ok(Some(job)) => {
                    if let Err(error) = self.process_job(worker_id, job).await {
                        let _ = self
                            .database
                            .insert_log(
                                AppLogLevel::Error,
                                "queue",
                                &format!("Queue worker {worker_id} failed: {error:#}"),
                            )
                            .await;
                        sleep(DOWNLOAD_ERROR_DELAY).await;
                    }
                }
                Ok(None) => sleep(DOWNLOAD_IDLE_DELAY).await,
                Err(error) => {
                    let _ = self
                        .database
                        .insert_log(
                            AppLogLevel::Error,
                            "queue",
                            &format!("Queue polling failed: {error:#}"),
                        )
                        .await;
                    sleep(DOWNLOAD_ERROR_DELAY).await;
                }
            }
        }
    }

    async fn process_job(&self, worker_id: i32, job: DownloadJobRecord) -> Result<()> {
        let file_name = job
            .original_filename
            .clone()
            .unwrap_or_else(|| job.event_id.clone());
        self.set_active_download(
            worker_id,
            Some(ActiveDownloadSnapshot {
                worker_id,
                job_id: job.id,
                room_id: job.room_id.clone(),
                event_id: job.event_id.clone(),
                filename: file_name.clone(),
                received_bytes: 0,
                total_bytes: None,
            }),
        )
        .await;

        let result = self.process_job_inner(worker_id, &job).await;
        self.set_active_download(worker_id, None).await;
        result
    }

    async fn process_job_inner(&self, worker_id: i32, job: &DownloadJobRecord) -> Result<()> {
        let settings = self.settings.read().await.clone();
        let temp_path = download_media_to_temp(
            &self.client,
            &self.database,
            &self.paths,
            &self.runtime,
            worker_id,
            job,
            settings.homeserver_url.clone(),
        )
        .await;

        let temp_path = match temp_path {
            Ok(temp_path) => temp_path,
            Err(error) => {
                handle_job_failure(&self.database, &self.settings, job, &error).await?;
                return Ok(());
            }
        };

        let result = async {
            validate_downloaded_media(&temp_path, job.category).await?;
            let final_size = tokio_fs::metadata(&temp_path).await?.len() as i64;
            let sha256 = sha256_file(&temp_path).await?;

            if let Some(existing) = self
                .database
                .find_completed_job(&job.room_id, job.category, &sha256)
                .await?
            {
                self.database
                    .mark_job_duplicate(
                        job.id,
                        &sha256,
                        existing.saved_relative_path.as_deref(),
                        final_size,
                    )
                    .await?;
                self.database
                    .insert_log(
                        AppLogLevel::Info,
                        "queue",
                        &format!(
                            "Skipped duplicate {}",
                            job.original_filename
                                .clone()
                                .unwrap_or_else(|| job.event_id.clone())
                        ),
                    )
                    .await?;
                return Ok(());
            }

            let ext = media_classification::preferred_extension(
                job.original_filename.as_deref(),
                job.mime_type.as_deref(),
            );
            let final_name = ext.map_or_else(|| sha256.clone(), |ext| format!("{sha256}.{ext}"));
            let destination_root = configured_destination_root(&settings, &self.paths);
            let destination_folder = download_job_destination_folder(
                &self.room_catalog,
                &self.paths,
                &settings,
                &job.room_id,
                job.category,
            )
            .await?;
            let final_path = destination_folder.join(final_name);
            let stored_path = relative_storage_path(&destination_root.to_string_lossy(), &final_path);

            if final_path.exists() {
                self.database
                    .mark_job_duplicate(job.id, &sha256, Some(&stored_path), final_size)
                    .await?;
                return Ok(());
            }

            move_file_cross_filesystem(&temp_path, &final_path).await?;
            if let Some(origin) = self
                .database
                .discovery_origin_timestamp(&job.room_id, &job.event_id)
                .await?
            {
                let _ = set_file_mtime(
                    &final_path,
                    FileTime::from_unix_time(origin.timestamp(), origin.timestamp_subsec_nanos()),
                );
            }

            self.database
                .mark_job_completed(job.id, &sha256, &stored_path, final_size)
                .await?;
            self.database
                .insert_log(
                    AppLogLevel::Info,
                    "queue",
                    &format!(
                        "Downloaded {} -> {}",
                        job.original_filename
                            .clone()
                            .unwrap_or_else(|| job.event_id.clone()),
                        stored_path
                    ),
                )
                .await?;
            Ok(())
        }
        .await;

        if result.is_err() {
            let _ = tokio_fs::remove_file(&temp_path).await;
        }
        result
    }

    async fn set_active_download(&self, worker_id: i32, next: Option<ActiveDownloadSnapshot>) {
        self.runtime
            .mutate(|runtime| {
                runtime
                    .active_downloads
                    .retain(|download| download.worker_id != worker_id);
                if let Some(next) = next {
                    runtime.active_downloads.push(next);
                    runtime
                        .active_downloads
                        .sort_by_key(|download| download.worker_id);
                }
            })
            .await;
    }
}

async fn build_client(paths: &AppPaths, homeserver_url: &str) -> Result<Client> {
    let builder = Client::builder()
        .server_name_or_homeserver_url(homeserver_url.to_owned())
        .sqlite_store_with_cache_path(&paths.matrix_data_path, &paths.matrix_cache_path, None)
        .user_agent("MatrixMediaShareClient/0.1")
        .sliding_sync_version_builder(matrix_sdk::sliding_sync::VersionBuilder::DiscoverNative);

    Ok(builder.build().await?)
}

async fn connect_client(
    paths: &AppPaths,
    secret_store: &SecretStore,
    settings: &AppSettings,
    password: &str,
) -> Result<Client> {
    if let Some(stored_session) = secret_store.load_session()? {
        if stored_session_matches_settings_login(&stored_session, settings) {
            let restore_homeserver_url = if stored_session.homeserver_url.trim().is_empty() {
                settings.homeserver_url.as_str()
            } else {
                stored_session.homeserver_url.as_str()
            };
            let client = build_client(paths, restore_homeserver_url).await?;
            let previous_device_id = stored_session.device_id.clone();
            if let Ok(matrix_session) = stored_session.try_into_matrix_session() {
                if client.restore_session(matrix_session).await.is_ok() {
                    return Ok(client);
                }
            }

            if !previous_device_id.trim().is_empty() {
                let login_result = client
                    .matrix_auth()
                    .login_username(&settings.username, password)
                    .device_id(&previous_device_id)
                    .initial_device_display_name("Matrix Media Share Client")
                    .await;
                if login_result.is_ok() {
                    return Ok(client);
                }
            }

            drop(client);
        }
        secret_store.clear_session()?;
    }

    reset_matrix_store(paths).await?;
    let client = build_client(paths, &settings.homeserver_url).await?;
    client
        .matrix_auth()
        .login_username(&settings.username, password)
        .initial_device_display_name("Matrix Media Share Client")
        .await?;
    Ok(client)
}

fn normalized_homeserver_url(homeserver_url: &str) -> String {
    homeserver_url.trim().trim_end_matches('/').to_owned()
}

async fn persist_current_session(secret_store: &SecretStore, _homeserver_url: &str, client: &Client) -> Result<()> {
    let Some(session) = client.session() else {
        return Ok(());
    };
    let stored = StoredSession::from_auth_session(
        session,
        normalized_homeserver_url(client.homeserver().as_str()),
    );
    secret_store.save_session(&stored)?;
    Ok(())
}

async fn detect_upload_limit(client: &Client) -> Option<i64> {
    let session = client.session()?;
    let access_token = match session {
        matrix_sdk::AuthSession::Matrix(session) => session.tokens.access_token,
        matrix_sdk::AuthSession::OAuth(session) => session.user.tokens.access_token,
        _ => return None,
    };

    let http_client = reqwest::Client::builder().build().ok()?;
    let homeserver = client.homeserver().to_string();
    let homeserver = homeserver.trim_end_matches('/').to_owned();
    let candidates = [
        format!("{homeserver}/_matrix/media/v3/config"),
        format!("{homeserver}/_matrix/client/v1/media/config"),
    ];

    for url in candidates {
        let Ok(response) = http_client.get(&url).bearer_auth(&access_token).send().await else {
            continue;
        };
        if !response.status().is_success() {
            continue;
        }
        let Ok(payload) = response.json::<serde_json::Value>().await else {
            continue;
        };
        if let Some(limit) = payload.get("m.upload.size").and_then(|value| value.as_i64()) {
            return Some(limit);
        }
    }

    None
}

fn stored_session_matches_settings_login(
    stored_session: &StoredSession,
    settings: &AppSettings,
) -> bool {
    let normalized_username = settings.username.trim();
    if normalized_username.is_empty() {
        return false;
    }

    if stored_session.user_id == normalized_username {
        return true;
    }

    let trimmed = normalized_username.trim_start_matches('@');
    let localpart = trimmed.split(':').next().unwrap_or(trimmed);
    UserId::parse(stored_session.user_id.as_str())
        .map(|user_id| user_id.localpart() == localpart)
        .unwrap_or(false)
}

fn runtime_user_id_from_settings(settings: &AppSettings) -> Option<String> {
    let normalized_username = settings.username.trim();
    if normalized_username.is_empty() {
        return None;
    }

    if normalized_username.starts_with('@') {
        return Some(normalized_username.to_owned());
    }

    let trimmed = normalized_username.trim_start_matches('@');
    if trimmed.contains(':') {
        return Some(format!("@{trimmed}"));
    }

    let homeserver_host = reqwest::Url::parse(&settings.homeserver_url)
        .ok()
        .and_then(|url| url.host_str().map(ToOwned::to_owned));

    match homeserver_host {
        Some(host) if !host.is_empty() => Some(format!("@{trimmed}:{host}")),
        _ => Some(trimmed.to_owned()),
    }
}

#[derive(Clone)]
struct PreparedShareSource {
    sha256: String,
    category: MediaCategory,
    file_name: String,
    content_type: Mime,
    file_size: i64,
    source_path: PathBuf,
    bundle_path: PathBuf,
}

async fn queue_share_requests(
    context: &Arc<RunningContext>,
    room_id: &str,
    file_paths: Vec<String>,
) -> Result<()> {
    let room_id = room_id.trim().to_owned();
    if room_id.is_empty() {
        return Err(anyhow!("Pick a room before sharing files."));
    }
    let selected_room = context
        .client
        .joined_rooms()
        .into_iter()
        .find(|room| room.room_id().as_str() == room_id)
        .ok_or_else(|| anyhow!("Pick a joined room before sharing files."))?;
    if selected_room.is_space() {
        return Err(anyhow!("Pick a room, not a space, before sharing files."));
    }

    let mut queued_count = 0;
    for file_path in file_paths {
        let trimmed = file_path.trim();
        if trimmed.is_empty() {
            continue;
        }

        queued_count += 1;
        let context = context.clone();
        let room_id = room_id.clone();
        let file_path = trimmed.to_owned();
        tokio::spawn(async move {
            let permit = context.share_slots.clone().acquire_owned().await;
            let _permit = match permit {
                Ok(permit) => permit,
                Err(_) => return,
            };

            if let Err(error) = share_local_file(&context, &room_id, Path::new(&file_path)).await {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "share",
                        &format!("Failed to share {} into {}: {error:#}", file_path, room_id),
                    )
                    .await;
            }

            sleep(SHARE_QUEUE_DELAY).await;
        });
    }

    if queued_count == 0 {
        return Err(anyhow!("No files were selected for sharing."));
    }

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "share",
            &format!("Queued {queued_count} file(s) for sharing into {room_id}."),
        )
        .await?;
    Ok(())
}

async fn archive_scan_loop(context: Arc<RunningContext>) {
    loop {
        let settings = context.settings.read().await.clone();
        if !settings.archive_scan_enabled || settings.archive_root_path.trim().is_empty() {
            sleep(Duration::from_secs(30)).await;
            continue;
        }

        let archive_root = PathBuf::from(settings.archive_root_path.trim());
        if !archive_root.exists() {
            let _ = context
                .database
                .insert_log(
                    AppLogLevel::Warning,
                    "archive",
                    &format!("Archive root is unavailable: {}", archive_root.display()),
                )
                .await;
            sleep(Duration::from_secs(60)).await;
            continue;
        }

        let archive_root_for_walk = archive_root.clone();
        let discovered_paths = tokio::task::spawn_blocking(move || collect_regular_files(&archive_root_for_walk)).await;
        let discovered_paths = match discovered_paths {
            Ok(Ok(paths)) => paths,
            Ok(Err(error)) => {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "archive",
                        &format!("Archive scan failed: {error:#}"),
                    )
                    .await;
                sleep(Duration::from_secs(60)).await;
                continue;
            }
            Err(error) => {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "archive",
                        &format!("Archive scan task failed: {error:#}"),
                    )
                    .await;
                sleep(Duration::from_secs(60)).await;
                continue;
            }
        };

        let per_file_delay = if settings.archive_scan_high_priority {
            Duration::from_millis(0)
        } else {
            Duration::from_millis(250)
        };
        let mut seen_archive_paths = HashSet::new();

        for archive_path in discovered_paths {
            let metadata = match tokio_fs::metadata(&archive_path).await {
                Ok(metadata) if metadata.is_file() => metadata,
                _ => continue,
            };

            let canonical_archive_path = tokio_fs::canonicalize(&archive_path)
                .await
                .unwrap_or_else(|_| archive_path.clone());
            seen_archive_paths.insert(canonical_archive_path.to_string_lossy().to_string());

            let sha256 = match sha256_with_cache(&context.database, &canonical_archive_path, HASH_ROOT_ARCHIVE).await {
                Ok(sha256) => sha256,
                Err(error) => {
                    let _ = context
                        .database
                        .insert_log(
                            AppLogLevel::Warning,
                            "archive",
                            &format!("Failed to hash {}: {error:#}", archive_path.display()),
                        )
                        .await;
                    continue;
                }
            };

            let modified_at = metadata
                .modified()
                .ok()
                .map(DateTime::<Utc>::from);
            let archive_path_string = canonical_archive_path.to_string_lossy().to_string();
            if context
                .database
                .upsert_archive_file(
                    &sha256,
                    &archive_path_string,
                    metadata.len() as i64,
                    modified_at,
                )
                .await
                .is_ok()
            {
                let _ = repoint_tracked_upload_to_archive(&context, &sha256, &canonical_archive_path).await;
            }

            if per_file_delay > Duration::ZERO {
                sleep(per_file_delay).await;
            }
        }

        let _ = context
            .database
            .prune_archive_files_to_paths(&seen_archive_paths)
            .await;
        let _ = context
            .database
            .prune_file_hash_cache_for_root_kind(HASH_ROOT_ARCHIVE, &seen_archive_paths)
            .await;

        sleep(Duration::from_secs(300)).await;
    }
}

async fn managed_share_maintenance_loop(context: Arc<RunningContext>) {
    loop {
        if let Err(error) = maintain_managed_share_store(&context).await {
            let _ = context
                .database
                .insert_log(
                    AppLogLevel::Warning,
                    "shared-files",
                    &format!("Managed shared-files maintenance failed: {error:#}"),
                )
                .await;
        }
        sleep(MANAGED_SHARE_SCAN_INTERVAL).await;
    }
}

async fn maintain_managed_share_store(context: &Arc<RunningContext>) -> Result<()> {
    let settings = context.settings.read().await.clone();
    let shared_root = configured_library_root(&settings, &context.paths);
    tokio_fs::create_dir_all(&shared_root).await?;
    let tracked_uploads = context.database.fetch_tracked_uploads().await?;
    let mut valid_categories = HashSet::new();
    let mut valid_bundles_by_category: HashMap<String, HashSet<String>> = HashMap::new();
    let mut valid_shared_media_paths = HashSet::new();

    for mut record in tracked_uploads {
        let category_key = record.category.as_storage_key().to_owned();
        valid_categories.insert(category_key.clone());
        valid_bundles_by_category
            .entry(category_key.clone())
            .or_default()
            .insert(record.sha256.clone());

        let bundle_path = managed_share_bundle_dir(&settings, &context.paths, record.category, &record.sha256);
        tokio_fs::create_dir_all(&bundle_path).await?;
        let mut allowed_names = HashSet::from([
            String::from("index.html"),
            String::from("thumbnail.jpg"),
        ]);
        let expected_media_path = managed_share_media_path(
            &bundle_path,
            &record.sha256,
            media_classification::preferred_extension(
                record.original_filename.as_deref(),
                record.mime_type.as_deref(),
            )
            .as_deref(),
        );
        let expected_media_name = expected_media_path
            .file_name()
            .and_then(|value| value.to_str())
            .unwrap_or_default()
            .to_owned();
        if !expected_media_name.is_empty()
            && (record.source_kind == LocalAssetSourceKind::Library || record.library_path.is_some())
        {
            allowed_names.insert(expected_media_name.clone());
        }

        let mut changed = false;
        let bundle_path_string = bundle_path.to_string_lossy().to_string();
        if record.bundle_path.as_deref() != Some(bundle_path_string.as_str()) {
            record.bundle_path = Some(bundle_path_string);
            changed = true;
        }

        if let Some(library_path) = record.library_path.clone() {
            let current_library_path = PathBuf::from(&library_path);
            if current_library_path != expected_media_path
                && tokio_fs::try_exists(&current_library_path).await.unwrap_or(false)
            {
                move_file_cross_filesystem(&current_library_path, &expected_media_path).await?;
                remember_file_hash(
                    &context.database,
                    &expected_media_path,
                    HASH_ROOT_SHARED,
                    &record.sha256,
                )
                .await?;
                record.library_path = Some(expected_media_path.to_string_lossy().to_string());
                if record.source_kind == LocalAssetSourceKind::Library {
                    record.source_path = expected_media_path.to_string_lossy().to_string();
                }
                if !expected_media_name.is_empty() {
                    allowed_names.insert(expected_media_name.clone());
                }
                changed = true;
            }
        }

        if record.source_kind == LocalAssetSourceKind::Library
            && tokio_fs::try_exists(&expected_media_path).await.unwrap_or(false)
        {
            valid_shared_media_paths.insert(expected_media_path.to_string_lossy().to_string());
            remember_file_hash(
                &context.database,
                &expected_media_path,
                HASH_ROOT_SHARED,
                &record.sha256,
            )
            .await?;
            if record.library_path.as_deref() != Some(expected_media_path.to_string_lossy().as_ref()) {
                record.library_path = Some(expected_media_path.to_string_lossy().to_string());
                record.source_path = expected_media_path.to_string_lossy().to_string();
                changed = true;
            }
        }

        let source_exists = match record.source_kind {
            LocalAssetSourceKind::Library => {
                tokio_fs::try_exists(&expected_media_path).await.unwrap_or(false)
            }
            LocalAssetSourceKind::Downloads | LocalAssetSourceKind::Archive => {
                tokio_fs::try_exists(&record.source_path).await.unwrap_or(false)
            }
        };

        if !source_exists && settings.self_heal_enabled {
            if let Some(file_cid) = record.file_cid.as_deref() {
                let recovered = fetch_ipfs_artifact_to_path(
                    context,
                    &settings,
                    file_cid,
                    None,
                    &expected_media_path,
                )
                .await;
                if recovered.is_ok() {
                    remember_file_hash(
                        &context.database,
                        &expected_media_path,
                        HASH_ROOT_SHARED,
                        &record.sha256,
                    )
                    .await?;
                    record.source_kind = LocalAssetSourceKind::Library;
                    record.source_path = expected_media_path.to_string_lossy().to_string();
                    record.library_path = Some(expected_media_path.to_string_lossy().to_string());
                    if !expected_media_name.is_empty() {
                        allowed_names.insert(expected_media_name.clone());
                    }
                    changed = true;
                    valid_shared_media_paths.insert(expected_media_path.to_string_lossy().to_string());
                    context
                        .database
                        .insert_log(
                            AppLogLevel::Info,
                            "shared-files",
                            &format!(
                                "Self-healed managed copy for {} into {}.",
                                record.sha256,
                                expected_media_path.display()
                            ),
                        )
                        .await?;
                }
            }
        }

        let thumbnail_path = managed_share_thumbnail_path(&bundle_path);
        if !tokio_fs::try_exists(&thumbnail_path).await.unwrap_or(false) {
            if let Some(thumbnail_cid) = record.thumbnail_cid.as_deref() {
                let _ = fetch_ipfs_artifact_to_path(
                    context,
                    &settings,
                    thumbnail_cid,
                    Some("image/jpeg"),
                    &thumbnail_path,
                )
                .await;
            }
        }

        let landing_page_path = managed_share_landing_page_path(&bundle_path);
        if !tokio_fs::try_exists(&landing_page_path).await.unwrap_or(false) {
            if let Some(page_cid) = record.page_cid.as_deref() {
                let _ = fetch_ipfs_landing_page_to_path(
                    context,
                    &settings,
                    page_cid,
                    record.landing_page_url.as_deref(),
                    &landing_page_path,
                )
                .await;
            }
        }

        purge_managed_bundle_contents(&bundle_path, &allowed_names).await?;

        if changed {
            record.updated_at = Utc::now();
            context.database.upsert_tracked_upload(&record).await?;
        }

        sleep(MANAGED_SHARE_ITEM_DELAY).await;
    }

    purge_managed_share_root(
        &shared_root,
        &valid_categories,
        &valid_bundles_by_category,
    )
    .await?;
    let _ = context
        .database
        .prune_file_hash_cache_for_root_kind(HASH_ROOT_SHARED, &valid_shared_media_paths)
        .await;

    Ok(())
}

async fn delete_shared_item(
    database: &AppDatabase,
    paths: &AppPaths,
    running: Option<&RunningService>,
    sha256: &str,
) -> Result<()> {
    let Some(record) = database.find_tracked_upload_by_sha256(sha256).await? else {
        return Ok(());
    };

    let settings = if let Some(running) = running {
        running.context.settings.read().await.clone()
    } else {
        database
            .load_settings(paths.manual_downloads_path.to_string_lossy().as_ref())
            .await?
    };
    let shared_root = configured_library_root(&settings, paths);
    let expected_bundle_path = managed_share_bundle_dir(&settings, paths, record.category, &record.sha256);
    let bundle_path = record
        .bundle_path
        .as_deref()
        .map(PathBuf::from)
        .unwrap_or(expected_bundle_path);

    if bundle_path.is_absolute() && bundle_path.starts_with(&shared_root) {
        remove_managed_path(&bundle_path).await;
        remove_empty_parent_dirs(&bundle_path, &shared_root).await;
    }

    for candidate in [
        record.library_path.as_deref(),
        Some(record.source_path.as_str()),
    ] {
        let Some(candidate) = candidate else {
            continue;
        };
        let candidate_path = PathBuf::from(candidate);
        if !candidate_path.is_absolute()
            || !candidate_path.starts_with(&shared_root)
            || candidate_path.starts_with(&bundle_path)
        {
            continue;
        }

        remove_managed_path(&candidate_path).await;
        remove_empty_parent_dirs(&candidate_path, &shared_root).await;
    }

    if let Some(library_path) = record.library_path.as_deref() {
        let library_path = PathBuf::from(library_path);
        if library_path.is_absolute() && library_path.starts_with(&shared_root) {
            let _ = database
                .remove_file_hash_cache_entry(library_path.to_string_lossy().as_ref())
                .await;
        }
    }

    if record.source_kind == LocalAssetSourceKind::Library {
        let source_path = PathBuf::from(&record.source_path);
        if source_path.is_absolute() && source_path.starts_with(&shared_root) {
            let _ = database
                .remove_file_hash_cache_entry(source_path.to_string_lossy().as_ref())
                .await;
        }
    }

    database.remove_tracked_upload(sha256).await?;
    database
        .insert_log(
            AppLogLevel::Info,
            "shared-files",
            &format!("Removed managed shared item {sha256} from Shared Files."),
        )
        .await?;
    Ok(())
}

async fn remove_managed_path(path: &Path) {
    match tokio_fs::symlink_metadata(path).await {
        Ok(metadata) if metadata.is_dir() => {
            let _ = tokio_fs::remove_dir_all(path).await;
        }
        Ok(_) => {
            let _ = tokio_fs::remove_file(path).await;
        }
        Err(_) => {}
    }
}

async fn remove_empty_parent_dirs(path: &Path, stop_root: &Path) {
    let mut current = path.parent().map(Path::to_path_buf);
    while let Some(dir) = current {
        if dir == stop_root {
            break;
        }

        match tokio_fs::read_dir(&dir).await {
            Ok(mut entries) => match entries.next_entry().await {
                Ok(None) => {
                    let _ = tokio_fs::remove_dir(&dir).await;
                    current = dir.parent().map(Path::to_path_buf);
                }
                Ok(Some(_)) | Err(_) => break,
            },
            Err(_) => break,
        }
    }
}

async fn purge_managed_bundle_contents(
    bundle_path: &Path,
    allowed_names: &HashSet<String>,
) -> Result<()> {
    let mut entries = match tokio_fs::read_dir(bundle_path).await {
        Ok(entries) => entries,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };

    while let Some(entry) = entries.next_entry().await? {
        let name = entry.file_name().to_string_lossy().to_string();
        let path = entry.path();
        if allowed_names.contains(&name) {
            continue;
        }
        let file_type = entry.file_type().await?;
        if file_type.is_dir() {
            let _ = tokio_fs::remove_dir_all(&path).await;
        } else {
            let _ = tokio_fs::remove_file(&path).await;
        }
    }
    Ok(())
}

async fn purge_managed_share_root(
    shared_root: &Path,
    valid_categories: &HashSet<String>,
    valid_bundles_by_category: &HashMap<String, HashSet<String>>,
) -> Result<()> {
    let mut category_entries = match tokio_fs::read_dir(shared_root).await {
        Ok(entries) => entries,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };

    while let Some(category_entry) = category_entries.next_entry().await? {
        let category_name = category_entry.file_name().to_string_lossy().to_string();
        let category_path = category_entry.path();
        let category_type = category_entry.file_type().await?;
        if !category_type.is_dir() || !valid_categories.contains(&category_name) {
            if category_type.is_dir() {
                let _ = tokio_fs::remove_dir_all(&category_path).await;
            } else {
                let _ = tokio_fs::remove_file(&category_path).await;
            }
            continue;
        }

        let valid_bundles = valid_bundles_by_category
            .get(&category_name)
            .cloned()
            .unwrap_or_default();
        let mut bundle_entries = tokio_fs::read_dir(&category_path).await?;
        while let Some(bundle_entry) = bundle_entries.next_entry().await? {
            let bundle_name = bundle_entry.file_name().to_string_lossy().to_string();
            let bundle_path = bundle_entry.path();
            let bundle_type = bundle_entry.file_type().await?;
            if !bundle_type.is_dir() || !valid_bundles.contains(&bundle_name) {
                if bundle_type.is_dir() {
                    let _ = tokio_fs::remove_dir_all(&bundle_path).await;
                } else {
                    let _ = tokio_fs::remove_file(&bundle_path).await;
                }
            }
        }
    }

    Ok(())
}

async fn fetch_ipfs_artifact_to_path(
    context: &Arc<RunningContext>,
    settings: &AppSettings,
    cid: &str,
    content_type_hint: Option<&str>,
    destination: &Path,
) -> Result<()> {
    let gateway = if settings.primary_gateway_url.trim().is_empty() {
        BOOTSTRAP_PRIMARY_GATEWAY
    } else {
        settings.primary_gateway_url.trim()
    };
    let url = raw_file_url(gateway, cid);
    let _ = content_type_hint;
    fetch_http_asset_to_path(context, &url, destination, true).await
}

async fn fetch_ipfs_landing_page_to_path(
    context: &Arc<RunningContext>,
    settings: &AppSettings,
    page_cid: &str,
    existing_url: Option<&str>,
    destination: &Path,
) -> Result<()> {
    let gateway = if settings.primary_gateway_url.trim().is_empty() {
        BOOTSTRAP_PRIMARY_GATEWAY
    } else {
        settings.primary_gateway_url.trim()
    };
    let url = existing_url
        .map(ToOwned::to_owned)
        .unwrap_or_else(|| page_url(gateway, page_cid));
    fetch_http_asset_to_path(context, &url, destination, true).await
}

async fn fetch_http_asset_to_path(
    context: &Arc<RunningContext>,
    url: &str,
    destination: &Path,
    ipfs_like: bool,
) -> Result<()> {
    let parsed = reqwest::Url::parse(url)?;
    let timeout_total = if ipfs_like {
        Duration::from_secs(20 * 60)
    } else {
        Duration::from_secs(5 * 60)
    };
    let timeout_stall = if ipfs_like {
        Duration::from_secs(90)
    } else {
        Duration::from_secs(30)
    };
    let client = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .timeout(timeout_total)
        .build()?;
    let response = client
        .get(parsed)
        .header(USER_AGENT, "MatrixMediaShareClient/0.1")
        .send()
        .await?
        .error_for_status()?;

    let temp_path = temp_download_path(&context.paths.temp_downloads_path, destination.extension().and_then(|value| value.to_str()));
    let transfer = async {
        if let Some(parent) = destination.parent() {
            tokio_fs::create_dir_all(parent).await?;
        }
        let mut file = tokio_fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp_path)
            .await?;
        let mut stream = response.bytes_stream();
        while let Some(chunk) = timeout(timeout_stall, stream.next())
            .await
            .map_err(|_| anyhow!("Timed out while waiting for background IPFS sidecar data."))?
        {
            file.write_all(&chunk?).await?;
        }
        file.flush().await?;
        move_file_cross_filesystem(&temp_path, destination).await?;
        Ok::<(), anyhow::Error>(())
    }
    .await;

    if let Err(error) = transfer {
        let _ = tokio_fs::remove_file(&temp_path).await;
        return Err(error);
    }

    Ok(())
}

fn collect_regular_files(root: &Path) -> Result<Vec<PathBuf>> {
    let mut pending = vec![root.to_path_buf()];
    let mut files = Vec::new();

    while let Some(path) = pending.pop() {
        for entry in std::fs::read_dir(&path)
            .with_context(|| format!("Failed to read {}", path.display()))?
        {
            let entry = entry?;
            let entry_path = entry.path();
            let file_type = entry.file_type()?;
            if file_type.is_dir() {
                pending.push(entry_path);
                continue;
            }
            if file_type.is_file() {
                files.push(entry_path);
            }
        }
    }

    Ok(files)
}

async fn repoint_tracked_upload_to_archive(
    context: &Arc<RunningContext>,
    sha256: &str,
    archive_path: &Path,
) -> Result<()> {
    let Some(existing) = context.database.find_tracked_upload_by_sha256(sha256).await? else {
        return Ok(());
    };
    if existing.source_kind == LocalAssetSourceKind::Archive
        && Path::new(&existing.source_path) == archive_path
    {
        return Ok(());
    }

    let mut next = existing.clone();
    next.source_kind = LocalAssetSourceKind::Archive;
    next.source_path = archive_path.to_string_lossy().to_string();
    next.archive_path = Some(next.source_path.clone());

    if let Some(library_path) = existing.library_path.as_deref() {
        if Path::new(library_path) != archive_path && tokio_fs::try_exists(library_path).await.unwrap_or(false) {
            match tokio_fs::remove_file(library_path).await {
                Ok(()) => {
                    next.library_path = None;
                    context
                        .database
                        .insert_log(
                            AppLogLevel::Info,
                            "archive",
                            &format!(
                                "Removed duplicate library copy after archive match: {}",
                                library_path
                            ),
                        )
                        .await?;
                }
                Err(error) => {
                    context
                        .database
                        .insert_log(
                            AppLogLevel::Warning,
                            "archive",
                            &format!(
                                "Failed to remove duplicate library copy {}: {error}",
                                library_path
                            ),
                        )
                        .await?;
                }
            }
        }
    }

    next.updated_at = Utc::now();
    context.database.upsert_tracked_upload(&next).await?;
    Ok(())
}

async fn share_source_from_archive_or_library(
    context: &Arc<RunningContext>,
    room: &Room,
    file_path: &Path,
) -> Result<PreparedShareSource> {
    let metadata = tokio_fs::metadata(file_path)
        .await
        .with_context(|| format!("Failed to inspect {}", file_path.display()))?;
    let file_name = file_path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("shared-file")
        .to_owned();
    let content_type = mime_guess::from_path(file_path).first_or_octet_stream();
    let category = media_classification::category(Some(&file_name), Some(content_type.essence_str()));
    let file_size = metadata.len() as i64;
    let settings = context.settings.read().await.clone();
    let sha256 = managed_file_sha256(context, file_path, &settings).await?;
    let bundle_path = managed_share_bundle_dir(&settings, &context.paths, category, &sha256);
    tokio_fs::create_dir_all(&bundle_path).await?;
    let now = Utc::now();

    if let Some(existing) = context.database.find_tracked_upload_by_sha256(&sha256).await? {
        let existing_source = PathBuf::from(&existing.source_path);
        if tokio_fs::try_exists(&existing_source).await.unwrap_or(false) {
            return Ok(PreparedShareSource {
                sha256,
                category,
                file_name,
                content_type,
                file_size,
                source_path: existing_source,
                bundle_path,
            });
        }
        if let Some(archive_path) = existing.archive_path.as_deref() {
            let archive_source = PathBuf::from(archive_path);
            if tokio_fs::try_exists(&archive_source).await.unwrap_or(false) {
                return Ok(PreparedShareSource {
                    sha256,
                    category,
                    file_name,
                    content_type,
                    file_size,
                    source_path: archive_source,
                    bundle_path,
                });
            }
        }
    }

    if let Some((source_kind, source_path)) =
        resolve_managed_share_source(context, &sha256, file_path, &settings).await?
    {
        let record = TrackedUploadRecord {
            sha256: sha256.clone(),
            source_kind,
            source_path: source_path.to_string_lossy().to_string(),
            bundle_path: Some(bundle_path.to_string_lossy().to_string()),
            library_path: (source_kind == LocalAssetSourceKind::Library)
                .then(|| source_path.to_string_lossy().to_string()),
            archive_path: (source_kind == LocalAssetSourceKind::Archive)
                .then(|| source_path.to_string_lossy().to_string()),
            file_cid: None,
            thumbnail_cid: None,
            page_cid: None,
            landing_page_url: None,
            room_id: room.room_id().to_string(),
            category,
            original_filename: Some(file_name.clone()),
            mime_type: Some(content_type.essence_str().to_owned()),
            file_size,
            created_at: now,
            updated_at: now,
        };
        context.database.upsert_tracked_upload(&record).await?;
        return Ok(PreparedShareSource {
            sha256,
            category,
            file_name,
            content_type,
            file_size,
            source_path,
            bundle_path,
        });
    }

    if let Some(archive_path) = resolve_archive_source_path(context, &sha256, file_path, &settings).await? {
        let record = TrackedUploadRecord {
            sha256: sha256.clone(),
            source_kind: LocalAssetSourceKind::Archive,
            source_path: archive_path.to_string_lossy().to_string(),
            bundle_path: Some(bundle_path.to_string_lossy().to_string()),
            library_path: None,
            archive_path: Some(archive_path.to_string_lossy().to_string()),
            file_cid: None,
            thumbnail_cid: None,
            page_cid: None,
            landing_page_url: None,
            room_id: room.room_id().to_string(),
            category,
            original_filename: Some(file_name.clone()),
            mime_type: Some(content_type.essence_str().to_owned()),
            file_size,
            created_at: now,
            updated_at: now,
        };
        context.database.upsert_tracked_upload(&record).await?;
        return Ok(PreparedShareSource {
            sha256,
            category,
            file_name,
            content_type,
            file_size,
            source_path: archive_path,
            bundle_path,
        });
    }

    let extension = media_classification::preferred_extension(
        Some(&file_name),
        Some(content_type.essence_str()),
    );
    let library_path = managed_share_media_path(&bundle_path, &sha256, extension.as_deref());
    if !tokio_fs::try_exists(&library_path).await.unwrap_or(false) {
        copy_file_cross_filesystem(file_path, &library_path).await?;
        remember_file_hash(&context.database, &library_path, HASH_ROOT_SHARED, &sha256).await?;
    }

    let record = TrackedUploadRecord {
        sha256,
        source_kind: LocalAssetSourceKind::Library,
        source_path: library_path.to_string_lossy().to_string(),
        bundle_path: Some(bundle_path.to_string_lossy().to_string()),
        library_path: Some(library_path.to_string_lossy().to_string()),
        archive_path: None,
        file_cid: None,
        thumbnail_cid: None,
        page_cid: None,
        landing_page_url: None,
        room_id: room.room_id().to_string(),
        category,
        original_filename: Some(file_name.clone()),
        mime_type: Some(content_type.essence_str().to_owned()),
        file_size,
        created_at: now,
        updated_at: now,
    };
    context.database.upsert_tracked_upload(&record).await?;

    Ok(PreparedShareSource {
        sha256: record.sha256.clone(),
        category: record.category,
        file_name,
        content_type,
        file_size,
        source_path: library_path,
        bundle_path,
    })
}

async fn resolve_managed_share_source(
    context: &Arc<RunningContext>,
    sha256: &str,
    file_path: &Path,
    settings: &AppSettings,
) -> Result<Option<(LocalAssetSourceKind, PathBuf)>> {
    if let Some(archive_path) = configured_archive_source(file_path, settings).await? {
        let metadata = tokio_fs::metadata(&archive_path).await?;
        let modified_at = metadata.modified().ok().map(DateTime::<Utc>::from);
        let archive_path_string = archive_path.to_string_lossy().to_string();
        context
            .database
            .upsert_archive_file(
                sha256,
                &archive_path_string,
                metadata.len() as i64,
                modified_at,
            )
            .await?;
        return Ok(Some((LocalAssetSourceKind::Archive, archive_path)));
    }

    if let Some(download_path) =
        configured_download_source(file_path, settings, &context.paths).await?
    {
        return Ok(Some((LocalAssetSourceKind::Downloads, download_path)));
    }

    if let Some(library_path) =
        configured_library_source(file_path, settings, &context.paths).await?
    {
        return Ok(Some((LocalAssetSourceKind::Library, library_path)));
    }

    Ok(None)
}

async fn resolve_archive_source_path(
    context: &Arc<RunningContext>,
    sha256: &str,
    file_path: &Path,
    settings: &AppSettings,
) -> Result<Option<PathBuf>> {
    if let Some(archive_path) = configured_archive_source(file_path, settings).await? {
        let metadata = tokio_fs::metadata(&archive_path).await?;
        let modified_at = metadata.modified().ok().map(DateTime::<Utc>::from);
        let archive_path_string = archive_path.to_string_lossy().to_string();
        context
            .database
            .upsert_archive_file(
                sha256,
                &archive_path_string,
                metadata.len() as i64,
                modified_at,
            )
            .await?;
        return Ok(Some(archive_path));
    }

    if let Some(existing) = context.database.find_archive_file_by_sha256(sha256).await? {
        let archive_path = PathBuf::from(existing.file_path);
        if tokio_fs::try_exists(&archive_path).await.unwrap_or(false) {
            return Ok(Some(archive_path));
        }
    }

    Ok(None)
}

async fn configured_archive_source(file_path: &Path, settings: &AppSettings) -> Result<Option<PathBuf>> {
    let archive_root = settings.archive_root_path.trim();
    if archive_root.is_empty() {
        return Ok(None);
    }

    let file_canonical = tokio_fs::canonicalize(file_path)
        .await
        .unwrap_or_else(|_| file_path.to_path_buf());
    let archive_root_canonical = tokio_fs::canonicalize(archive_root)
        .await
        .unwrap_or_else(|_| PathBuf::from(archive_root));
    if file_canonical.starts_with(&archive_root_canonical) {
        return Ok(Some(file_canonical));
    }
    Ok(None)
}

fn configured_library_root(settings: &AppSettings, paths: &AppPaths) -> PathBuf {
    let configured = settings.library_root_path.trim();
    if configured.is_empty() {
        paths.library_path.clone()
    } else {
        PathBuf::from(configured)
    }
}

fn managed_share_category_root(
    settings: &AppSettings,
    paths: &AppPaths,
    category: MediaCategory,
) -> PathBuf {
    configured_library_root(settings, paths).join(category.as_storage_key())
}

fn managed_share_bundle_dir(
    settings: &AppSettings,
    paths: &AppPaths,
    category: MediaCategory,
    sha256: &str,
) -> PathBuf {
    managed_share_category_root(settings, paths, category).join(sha256)
}

fn managed_share_media_path(
    bundle_path: &Path,
    sha256: &str,
    extension: Option<&str>,
) -> PathBuf {
    let file_name = extension
        .filter(|value| !value.is_empty())
        .map(|value| format!("{sha256}.{value}"))
        .unwrap_or_else(|| sha256.to_owned());
    bundle_path.join(file_name)
}

fn managed_share_thumbnail_path(bundle_path: &Path) -> PathBuf {
    bundle_path.join("thumbnail.jpg")
}

fn managed_share_landing_page_path(bundle_path: &Path) -> PathBuf {
    bundle_path.join("index.html")
}

fn configured_destination_root(settings: &AppSettings, paths: &AppPaths) -> PathBuf {
    let configured = settings.destination_root_path.trim();
    if configured.is_empty() {
        paths.manual_downloads_path.clone()
    } else {
        PathBuf::from(configured)
    }
}

async fn download_job_destination_folder(
    room_catalog: &RoomCatalog,
    paths: &AppPaths,
    settings: &AppSettings,
    room_id: &str,
    category: MediaCategory,
) -> Result<PathBuf> {
    if room_id == MANUAL_IPFS_ROOM_ID {
        let downloads_root = configured_destination_root(settings, paths);
        tokio_fs::create_dir_all(&downloads_root).await?;
        if settings.flat_folder_layout {
            return Ok(downloads_root);
        }

        let manual_root = downloads_root.join(MANUAL_IPFS_FOLDER_LABEL);
        tokio_fs::create_dir_all(&manual_root).await?;
        let category_folder = manual_root.join(category.as_storage_key());
        tokio_fs::create_dir_all(&category_folder).await?;
        return Ok(category_folder);
    }

    room_catalog.category_folder(room_id, category, settings).await
}

async fn configured_library_source(
    file_path: &Path,
    settings: &AppSettings,
    paths: &AppPaths,
) -> Result<Option<PathBuf>> {
    configured_root_source(file_path, &configured_library_root(settings, paths)).await
}

async fn configured_download_source(
    file_path: &Path,
    settings: &AppSettings,
    paths: &AppPaths,
) -> Result<Option<PathBuf>> {
    let root = configured_destination_root(settings, paths);
    if let Some(path) = configured_root_source(file_path, &root).await? {
        return Ok(Some(path));
    }

    Ok(None)
}

async fn configured_root_source(file_path: &Path, root_path: &Path) -> Result<Option<PathBuf>> {
    if root_path.as_os_str().is_empty() {
        return Ok(None);
    }

    let file_canonical = tokio_fs::canonicalize(file_path)
        .await
        .unwrap_or_else(|_| file_path.to_path_buf());
    let root_canonical = tokio_fs::canonicalize(root_path)
        .await
        .unwrap_or_else(|_| root_path.to_path_buf());
    if file_canonical.starts_with(&root_canonical) {
        return Ok(Some(file_canonical));
    }
    Ok(None)
}

async fn managed_file_sha256(
    context: &Arc<RunningContext>,
    file_path: &Path,
    settings: &AppSettings,
) -> Result<String> {
    if configured_archive_source(file_path, settings).await?.is_some() {
        return sha256_with_cache(&context.database, file_path, HASH_ROOT_ARCHIVE).await;
    }
    if configured_download_source(file_path, settings, &context.paths).await?.is_some() {
        return sha256_with_cache(&context.database, file_path, HASH_ROOT_DOWNLOADS).await;
    }
    if configured_library_source(file_path, settings, &context.paths).await?.is_some() {
        return sha256_with_cache(&context.database, file_path, HASH_ROOT_SHARED).await;
    }
    sha256_file(file_path).await
}

async fn sha256_with_cache(
    database: &AppDatabase,
    file_path: &Path,
    root_kind: &str,
) -> Result<String> {
    let metadata = tokio_fs::metadata(file_path)
        .await
        .with_context(|| format!("Failed to inspect {}", file_path.display()))?;
    let canonical_path = tokio_fs::canonicalize(file_path)
        .await
        .unwrap_or_else(|_| file_path.to_path_buf());
    let canonical_string = canonical_path.to_string_lossy().to_string();
    let modified_at = metadata.modified().ok().map(DateTime::<Utc>::from);

    if let Some(cached) = database.file_hash_cache_record(&canonical_string).await? {
        if cached.root_kind == root_kind
            && cached.file_size == metadata.len() as i64
            && cached.modified_at == modified_at
        {
            return Ok(cached.sha256);
        }
    }

    let sha256 = sha256_file(&canonical_path).await?;
    database
        .upsert_file_hash_cache(
            &canonical_string,
            root_kind,
            &sha256,
            metadata.len() as i64,
            modified_at,
        )
        .await?;
    Ok(sha256)
}

async fn remember_file_hash(
    database: &AppDatabase,
    file_path: &Path,
    root_kind: &str,
    sha256: &str,
) -> Result<()> {
    let metadata = tokio_fs::metadata(file_path)
        .await
        .with_context(|| format!("Failed to inspect {}", file_path.display()))?;
    let canonical_path = tokio_fs::canonicalize(file_path)
        .await
        .unwrap_or_else(|_| file_path.to_path_buf());
    database
        .upsert_file_hash_cache(
            &canonical_path.to_string_lossy(),
            root_kind,
            sha256,
            metadata.len() as i64,
            metadata.modified().ok().map(DateTime::<Utc>::from),
        )
        .await
}

async fn share_local_file(
    context: &Arc<RunningContext>,
    room_id: &str,
    file_path: &Path,
) -> Result<()> {
    let room = context
        .client
        .joined_rooms()
        .into_iter()
        .find(|room| room.room_id().as_str() == room_id)
        .ok_or_else(|| anyhow!("Room not found: {room_id}"))?;
    let prepared = share_source_from_archive_or_library(context, &room, file_path).await?;
    let thumbnail_path = prepare_managed_share_thumbnail(&prepared).await.ok();
    let landing_page_path = managed_share_landing_page_path(&prepared.bundle_path);
    let settings = context.settings.read().await.clone();
    let preferred_gateway = if settings.primary_gateway_url.trim().is_empty() {
        BOOTSTRAP_PRIMARY_GATEWAY
    } else {
        settings.primary_gateway_url.as_str()
    };
    let published = context
        .ipfs
        .publish_share(
            &prepared.file_name,
            &prepared.source_path,
            thumbnail_path.as_deref(),
            &landing_page_path,
            Some(preferred_gateway),
            &settings.preferred_gateway_urls,
        )
        .await?;
    update_tracked_share_publication(context, &prepared, &published).await?;
    let comment_message = format!(
        "Download link: {}\nIPFS media: ipfs://{}",
        published.landing_page_url, published.file_cid,
    );
    let attachment_comment = Some(TextMessageEventContent::plain(comment_message.clone()));

    let upload_limit = context.runtime.snapshot().await.upload_size_limit_bytes;
    let should_try_full_upload = upload_limit.is_none_or(|limit| prepared.file_size <= limit);

    if should_try_full_upload {
        let file_bytes = tokio_fs::read(&prepared.source_path)
            .await
            .with_context(|| format!("Failed to read {}", prepared.source_path.display()))?;
        match room
            .send_attachment(
                prepared.file_name.clone(),
                &prepared.content_type,
                file_bytes,
                AttachmentConfig::new().caption(attachment_comment.clone()),
            )
            .await
        {
            Ok(_) => {
                context
                    .database
                    .insert_log(
                        AppLogLevel::Info,
                        "share",
                        &format!("Shared {} to {room_id} via Matrix and IPFS.", prepared.file_name),
                    )
                    .await?;
                return Ok(());
            }
            Err(error) => {
                context
                    .database
                    .insert_log(
                        AppLogLevel::Warning,
                        "share",
                        &format!(
                            "Matrix upload failed for {}, falling back to IPFS link: {error:#}",
                            prepared.file_name
                        ),
                    )
                    .await?;
            }
        }
    }

    if let Some(thumbnail_path) = thumbnail_path.as_deref() {
        if let Ok(thumbnail_bytes) = tokio_fs::read(thumbnail_path).await {
            if room
                .send_attachment(
                    format!("{}.preview.jpg", prepared.file_name),
                    &mime::IMAGE_JPEG,
                    thumbnail_bytes,
                    AttachmentConfig::new().caption(attachment_comment.clone()),
                )
                .await
                .is_ok()
            {
                context
                    .database
                    .insert_log(
                        AppLogLevel::Info,
                        "share",
                        &format!(
                            "Shared {} to {room_id} via IPFS landing page {}.",
                            prepared.file_name, published.landing_page_url
                        ),
                    )
                    .await?;
                return Ok(());
            }
        }
    }

    room.send(RoomMessageEventContent::text_plain(format!(
        "{}\n{}",
        prepared.file_name, comment_message
    )))
        .await?;
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "share",
            &format!(
                "Shared {} to {room_id} via IPFS landing page {}.",
                prepared.file_name, published.landing_page_url
            ),
        )
        .await?;
    Ok(())
}

async fn update_tracked_share_publication(
    context: &Arc<RunningContext>,
    prepared: &PreparedShareSource,
    published: &crate::ipfs_service::PublishedShare,
) -> Result<()> {
    let Some(mut record) = context
        .database
        .find_tracked_upload_by_sha256(&prepared.sha256)
        .await?
    else {
        return Ok(());
    };

    record.bundle_path = Some(prepared.bundle_path.to_string_lossy().to_string());
    record.file_cid = Some(published.file_cid.clone());
    record.thumbnail_cid = published.thumbnail_cid.clone();
    record.page_cid = Some(published.page_cid.clone());
    record.landing_page_url = Some(published.landing_page_url.clone());
    record.updated_at = Utc::now();
    context.database.upsert_tracked_upload(&record).await
}

async fn import_ipfs_link(context: &Arc<RunningContext>, link: &str) -> Result<()> {
    let settings = context.settings.read().await.clone();
    let imported = parse_ipfs_import_target(link, &settings)?;
    let original_filename = imported.file_name.as_deref();
    let category = media_classification::category(original_filename, None);
    let event_id = format!("manual-ipfs-{}", Uuid::new_v4());
    let queued = context
        .database
        .enqueue_direct_download(
            MANUAL_IPFS_ROOM_ID,
            &event_id,
            crate::domain::MediaSourceKind::Ipfs,
            &imported.direct_url,
            imported.file_name.as_deref(),
            None,
            category,
        )
        .await?;
    let log_message = if queued {
        format!("Queued manual IPFS download: {}", imported.direct_url)
    } else {
        format!("Manual IPFS link was already queued: {}", imported.direct_url)
    };
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "ipfs",
            &log_message,
        )
        .await?;
    Ok(())
}

async fn open_discovery(
    context: &Arc<RunningContext>,
    room_id: &str,
    event_id: &str,
) -> Result<()> {
    let discovery = context
        .database
        .discovery_record(room_id, event_id)
        .await?
        .ok_or_else(|| anyhow!("Media item was not found in the browser catalog."))?;
    let settings = context.settings.read().await.clone();
    let job = DownloadJobRecord {
        id: 0,
        media_item_id: None,
        room_id: discovery.room_id.clone(),
        event_id: discovery.event_id.clone(),
        mxc_url: discovery.mxc_url.clone(),
        fallback_source_url: discovery.fallback_source_url.clone(),
        source_kind: discovery.source_kind,
        direct_url: discovery.direct_url.clone(),
        original_filename: discovery.original_filename.clone(),
        mime_type: discovery.mime_type.clone(),
        category: discovery.category,
        state: crate::domain::DownloadJobState::Downloading,
        retry_count: 0,
        next_eligible_at: None,
        last_failure_at: None,
        received_bytes: 0,
        total_bytes: None,
        last_error: None,
        sha256: None,
        saved_relative_path: None,
        created_at: Utc::now(),
        updated_at: Utc::now(),
    };
    let next_session_id = context.runtime.snapshot().await.viewer.session_id.saturating_add(1);
    let file_name = discovery
        .original_filename
        .clone()
        .unwrap_or_else(|| discovery.event_id.clone());

    context
        .runtime
        .mutate(|runtime| {
            runtime.viewer = ViewerSnapshot {
                session_id: next_session_id.max(1),
                state: ViewerState::Downloading,
                room_id: Some(discovery.room_id.clone()),
                event_id: Some(discovery.event_id.clone()),
                file_name: Some(file_name.clone()),
                mime_type: discovery.mime_type.clone(),
                category: Some(discovery.category),
                local_path: None,
                received_bytes: 0,
                total_bytes: None,
                error: None,
            };
            runtime
                .active_downloads
                .retain(|download| download.worker_id != 0);
            runtime.active_downloads.push(ActiveDownloadSnapshot {
                worker_id: 0,
                job_id: 0,
                room_id: discovery.room_id.clone(),
                event_id: discovery.event_id.clone(),
                filename: file_name.clone(),
                received_bytes: 0,
                total_bytes: None,
            });
            runtime.active_downloads.sort_by_key(|download| download.worker_id);
        })
        .await;

    let temp_path = download_media_to_temp(
        &context.client,
        &context.database,
        &context.paths,
        &context.runtime,
        0,
        &job,
        settings.homeserver_url.clone(),
    )
    .await;

    context
        .runtime
        .mutate(|runtime| {
            runtime
                .active_downloads
                .retain(|download| download.worker_id != 0);
        })
        .await;

    let temp_path = match temp_path {
        Ok(temp_path) => temp_path,
        Err(error) => {
            context
                .runtime
                .mutate(|runtime| {
                    runtime.viewer.state = ViewerState::Error;
                    runtime.viewer.local_path = None;
                    runtime.viewer.error = Some(error.to_string());
                })
                .await;
            return Err(error);
        }
    };
    let final_size = tokio_fs::metadata(&temp_path)
        .await
        .map(|metadata| metadata.len() as i64)
        .unwrap_or_default();
    context
        .runtime
        .mutate(|runtime| {
            runtime.viewer.state = ViewerState::Ready;
            runtime.viewer.local_path = Some(temp_path.to_string_lossy().to_string());
            runtime.viewer.received_bytes = final_size;
            runtime.viewer.total_bytes = Some(final_size);
            runtime.viewer.error = None;
        })
        .await;
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "viewer",
            &format!("Prepared browser item {} from room {} for the built-in viewer.", event_id, room_id),
        )
        .await?;
    Ok(())
}

#[derive(Clone, Debug)]
struct ImportedIpfsTarget {
    direct_url: String,
    file_name: Option<String>,
}

#[derive(Clone, Debug)]
struct IpfsMessageHints {
    media_target: Option<ImportedIpfsTarget>,
    landing_page_url: Option<String>,
    file_name_hint: Option<String>,
}

fn parse_ipfs_import_target(link: &str, settings: &AppSettings) -> Result<ImportedIpfsTarget> {
    let trimmed = link.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("Paste an IPFS link or CID first."));
    }

    if let Some(stripped) = trimmed.strip_prefix("ipfs://") {
        let without_slashes = stripped.trim_start_matches('/');
        let mut parts = without_slashes.splitn(2, '/');
        let cid = parts
            .next()
            .filter(|value| !value.is_empty())
            .ok_or_else(|| anyhow!("Invalid IPFS URI."))?
            .to_owned();
        let remainder = parts.next().map(ToOwned::to_owned);
        return Ok(ImportedIpfsTarget {
            direct_url: gateway_raw_url(settings, &cid, remainder.as_deref())?,
            file_name: remainder
                .and_then(|value| std::path::Path::new(&value).file_name()?.to_str().map(ToOwned::to_owned))
                .or_else(|| Some(cid)),
        });
    }

    if let Ok(url) = reqwest::Url::parse(trimmed) {
        if let Some((cid, remainder)) = cid_and_remainder_from_path(url.path()) {
            let direct_url = gateway_raw_url(settings, &cid, remainder.as_deref())?;
            return Ok(ImportedIpfsTarget {
                direct_url,
                file_name: remainder
                    .and_then(|value| std::path::Path::new(&value).file_name()?.to_str().map(ToOwned::to_owned))
                    .or_else(|| Some(cid)),
            });
        }

        let file_name = url
            .path_segments()
            .and_then(|segments| segments.last())
            .filter(|segment| !segment.is_empty())
            .map(ToOwned::to_owned);
        return Ok(ImportedIpfsTarget {
            direct_url: url.to_string(),
            file_name,
        });
    }

    if looks_like_ipfs_cid(trimmed) {
        return Ok(ImportedIpfsTarget {
            direct_url: gateway_raw_url(settings, trimmed, None)?,
            file_name: Some(trimmed.to_owned()),
        });
    }

    Err(anyhow!("Could not recognize that as an IPFS URL or CID."))
}

fn ipfs_discovery_from_body(
    room_id: &str,
    event_id: &str,
    timestamp: DateTime<Utc>,
    body: &str,
    settings: &AppSettings,
) -> Result<Option<AttachmentDiscovery>> {
    let hints = ipfs_message_hints(body, settings)?;
    let Some(media_target) = hints.media_target else {
        return Ok(None);
    };

    let file_name = hints.file_name_hint.clone().or(media_target.file_name);
    let category = media_classification::category(file_name.as_deref(), None);
    let direct_url = media_target.direct_url;

    Ok(Some(AttachmentDiscovery {
        room_id: room_id.to_owned(),
        event_id: event_id.to_owned(),
        origin_server_timestamp: timestamp,
        source_kind: MediaSourceKind::Ipfs,
        direct_url: Some(direct_url.clone()),
        mxc_url: hints.landing_page_url.unwrap_or_else(|| direct_url.clone()),
        fallback_source_url: None,
        thumbnail_source_url: file_name
            .as_deref()
            .and_then(|name| (media_classification::category(Some(name), None) == MediaCategory::Images).then(|| direct_url.clone())),
        thumbnail_cached_path: None,
        original_filename: file_name,
        mime_type: None,
        category,
    }))
}

fn preferred_message_discovery(
    matrix_discovery: Option<AttachmentDiscovery>,
    ipfs_discovery: Option<AttachmentDiscovery>,
) -> Option<AttachmentDiscovery> {
    match (matrix_discovery, ipfs_discovery) {
        (Some(matrix), Some(mut ipfs)) => {
            ipfs.fallback_source_url = Some(matrix.mxc_url.clone());
            if ipfs.original_filename.is_none() {
                ipfs.original_filename = matrix.original_filename;
            }
            if ipfs.mime_type.is_none() {
                ipfs.mime_type = matrix.mime_type;
            }
            if ipfs.category == MediaCategory::Other {
                ipfs.category = matrix.category;
            }
            if ipfs.thumbnail_source_url.is_none() {
                ipfs.thumbnail_source_url = matrix.thumbnail_source_url;
            }
            if ipfs.thumbnail_cached_path.is_none() {
                ipfs.thumbnail_cached_path = matrix.thumbnail_cached_path;
            }
            Some(ipfs)
        }
        (None, Some(ipfs)) => Some(ipfs),
        (Some(matrix), None) => Some(matrix),
        (None, None) => None,
    }
}

fn ipfs_message_hints(body: &str, settings: &AppSettings) -> Result<IpfsMessageHints> {
    let file_name_hint = ipfs_message_file_name_hint(body);
    let landing_page_url = ipfs_landing_page_hint(body);

    if let Some(media_target) = ipfs_media_hint(body, settings)? {
        return Ok(IpfsMessageHints {
            media_target: Some(media_target),
            landing_page_url,
            file_name_hint,
        });
    }

    if let Some(cid) = ipfs_cid_hint(body) {
        return Ok(IpfsMessageHints {
            media_target: Some(ImportedIpfsTarget {
                direct_url: gateway_raw_url(settings, &cid, None)?,
                file_name: Some(cid),
            }),
            landing_page_url,
            file_name_hint,
        });
    }

    for token in ipfs_body_candidates(body) {
        if token.to_ascii_lowercase().contains("/index.html") {
            continue;
        }
        let Ok(imported) = parse_ipfs_import_target(&token, settings) else {
            continue;
        };
        return Ok(IpfsMessageHints {
            media_target: Some(imported),
            landing_page_url,
            file_name_hint,
        });
    }

    Ok(IpfsMessageHints {
        media_target: None,
        landing_page_url,
        file_name_hint,
    })
}

fn ipfs_media_hint(body: &str, settings: &AppSettings) -> Result<Option<ImportedIpfsTarget>> {
    for line in body.lines() {
        let trimmed = line.trim();
        if !trimmed.to_ascii_lowercase().starts_with("ipfs media:") {
            continue;
        }
        let Some((_, value)) = trimmed.split_once(':') else {
            continue;
        };
        let candidate = value.trim();
        if candidate.is_empty() {
            continue;
        }
        return parse_ipfs_import_target(candidate, settings).map(Some);
    }
    Ok(None)
}

fn ipfs_message_file_name_hint(body: &str) -> Option<String> {
    body.lines()
        .map(str::trim)
        .find(|line| {
            let lower = line.to_ascii_lowercase();
            !line.is_empty()
                && !line.contains("://")
                && !lower.starts_with("cid:")
                && !lower.starts_with("media cid:")
                && !lower.starts_with("page cid:")
                && !lower.starts_with("landing page cid:")
                && !lower.starts_with("download link:")
                && !lower.starts_with("ipfs landing page:")
                && !lower.starts_with("ipfs media:")
        })
        .map(ToOwned::to_owned)
}

fn ipfs_landing_page_hint(body: &str) -> Option<String> {
    body.lines().find_map(|line| {
        let trimmed = line.trim();
        let lower = trimmed.to_ascii_lowercase();
        if !lower.starts_with("ipfs landing page:")
            && !lower.starts_with("download link:")
        {
            return None;
        }
        let (_, value) = trimmed.split_once(':')?;
        let url = value.trim();
        reqwest::Url::parse(url).ok().map(|parsed| parsed.to_string())
    })
}

fn ipfs_cid_hint(body: &str) -> Option<String> {
    for line in body.lines() {
        let trimmed = line.trim();
        let lower = trimmed.to_ascii_lowercase();
        if lower.starts_with("page cid:") || lower.starts_with("landing page cid:") {
            continue;
        }

        if lower.starts_with("cid:") || lower.starts_with("media cid:") {
            let rest = trimmed
                .split_once(':')
                .map(|(_, value)| value.trim())
                .unwrap_or_default();
            if looks_like_ipfs_cid(rest) {
                return Some(rest.to_owned());
            }
        }

        for token in trimmed.split_whitespace() {
            let cleaned = token.trim_matches(|character: char| {
                matches!(
                    character,
                    '"' | '\'' | '(' | ')' | '[' | ']' | '{' | '}' | '<' | '>' | ',' | '.' | ';'
                )
            });
            if looks_like_ipfs_cid(cleaned) {
                return Some(cleaned.to_owned());
            }
        }
    }
    None
}

fn ipfs_body_candidates(body: &str) -> Vec<String> {
    let mut candidates = Vec::new();
    for token in body.split_whitespace() {
        let cleaned = token.trim_matches(|character: char| {
            matches!(
                character,
                '"' | '\'' | '(' | ')' | '[' | ']' | '{' | '}' | '<' | '>' | ',' | ';'
            )
        });
        if cleaned.contains("/ipfs/") || cleaned.starts_with("ipfs://") || looks_like_ipfs_cid(cleaned) {
            candidates.push(cleaned.trim_end_matches('.').to_owned());
        }
    }
    candidates
}

async fn prepare_managed_share_thumbnail(prepared: &PreparedShareSource) -> Result<PathBuf> {
    let thumbnail_path = managed_share_thumbnail_path(&prepared.bundle_path);
    generate_preview_thumbnail_to_path(&prepared.source_path, &thumbnail_path).await?;
    Ok(thumbnail_path)
}

async fn generate_preview_thumbnail(paths: &AppPaths, file_path: &Path) -> Result<PathBuf> {
    let name = file_path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("preview");
    let destination = paths.temp_downloads_path.join(format!("{name}.preview.jpg"));
    generate_preview_thumbnail_to_path(file_path, &destination).await?;
    Ok(destination)
}

async fn generate_preview_thumbnail_to_path(file_path: &Path, destination: &Path) -> Result<()> {
    let extension = file_path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or_default()
        .to_ascii_lowercase();
    if !matches!(extension.as_str(), "jpg" | "jpeg" | "png" | "webp" | "bmp" | "gif") {
        return Err(anyhow!("Preview thumbnails are currently supported for image files only."));
    }

    let bytes = tokio_fs::read(file_path)
        .await
        .with_context(|| format!("Failed to read {}", file_path.display()))?;
    let image = load_oriented_image_from_bytes(&bytes).context("Failed to decode the image")?;
    let thumbnail = image.thumbnail(480, 480);
    if let Some(parent) = destination.parent() {
        tokio_fs::create_dir_all(parent).await?;
    }
    thumbnail
        .save_with_format(destination, image::ImageFormat::Jpeg)
        .with_context(|| format!("Failed to write {}", destination.display()))?;
    Ok(())
}

fn load_oriented_image_from_bytes(bytes: &[u8]) -> Result<image::DynamicImage> {
    use image::{DynamicImage, ImageDecoder, ImageReader, metadata::Orientation};

    let reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .context("Could not detect image format")?;
    let mut decoder = reader.into_decoder().context("Could not create image decoder")?;
    let orientation = decoder.orientation().unwrap_or(Orientation::NoTransforms);
    let mut image = DynamicImage::from_decoder(decoder).context("Could not decode image")?;
    image.apply_orientation(orientation);
    Ok(image)
}

async fn reset_matrix_store(paths: &AppPaths) -> Result<()> {
    remove_directory_if_exists(&paths.matrix_data_path).await?;
    remove_directory_if_exists(&paths.matrix_cache_path).await?;
    paths.ensure_directories()?;
    Ok(())
}

async fn remove_directory_if_exists(path: &Path) -> Result<()> {
    match tokio_fs::remove_dir_all(path).await {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => {
            Err(error).with_context(|| format!("Failed to remove directory {}", path.display()))
        }
    }
}

async fn watch_sync_state(context: Arc<RunningContext>) {
    let mut subscriber = context.sync_service.state();
    publish_sync_state(&context, &subscriber.get()).await;

    while let Some(state) = subscriber.next().await {
        publish_sync_state(&context, &state).await;
        if matches!(state, SyncState::Offline | SyncState::Error(_)) {
            let _ = context
                .database
                .insert_log(
                    AppLogLevel::Warning,
                    "matrix",
                    "Matrix sync entered an error/offline state.",
                )
                .await;
        }
    }
}

async fn publish_sync_state(context: &Arc<RunningContext>, state: &SyncState) {
    let connection_state = connection_state_from_sync_state(state);
    context
        .runtime
        .mutate(|runtime| runtime.connection_state = connection_state)
        .await;
}

fn connection_state_from_sync_state(state: &SyncState) -> ConnectionState {
    match state {
        SyncState::Idle => ConnectionState::Starting,
        SyncState::Running => ConnectionState::Running,
        SyncState::Terminated => ConnectionState::Stopped,
        SyncState::Offline | SyncState::Error(_) => ConnectionState::Error,
    }
}

async fn watch_session_changes(context: Arc<RunningContext>) {
    let mut receiver = context.client.subscribe_to_session_changes();
    loop {
        match receiver.recv().await {
            Ok(matrix_sdk::SessionChange::TokensRefreshed) => {
                let settings = context.settings.read().await.clone();
                let _ = persist_current_session(
                    &context.secret_store,
                    &settings.homeserver_url,
                    &context.client,
                )
                .await;
            }
            Ok(matrix_sdk::SessionChange::UnknownToken { soft_logout }) => {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "matrix",
                        &format!("Authentication error received. softLogout={soft_logout}"),
                    )
                    .await;
                context
                    .runtime
                    .mutate(|runtime| runtime.connection_state = ConnectionState::Error)
                    .await;
            }
            Err(_) => break,
        }
    }
}

async fn watch_verification_state(context: Arc<RunningContext>) {
    let mut subscriber = context.client.encryption().verification_state();
    while subscriber.next().await.is_some() {
        let _ = refresh_verification_snapshot(&context).await;
    }
}

async fn periodic_room_refresh(context: Arc<RunningContext>) {
    loop {
        let connection_state = context.runtime.snapshot().await.connection_state;
        if matches!(
            connection_state,
            ConnectionState::Running | ConnectionState::Starting
        ) {
            let _ = refresh_joined_rooms(context.clone()).await;
            let _ = prune_expired_failed_jobs(&context).await;
            let _ = refresh_verification_snapshot(&context).await;
        }
        sleep(ROOM_REFRESH_INTERVAL).await;
    }
}

fn schedule_room_refresh(context: Arc<RunningContext>) {
    tokio::spawn(async move {
        let _ = refresh_joined_rooms(context).await;
    });
}

async fn refresh_joined_rooms(context: Arc<RunningContext>) -> Result<()> {
    let settings = context.settings.read().await.clone();
    let current_user_id = runtime_user_id_from_settings(&settings).unwrap_or_default();

    if !settings.owner_user_id.trim().is_empty() && current_user_id != settings.owner_user_id {
        let invited_rooms = context.client.invited_rooms();
        for room in invited_rooms {
            let _ = accept_owner_invite_if_allowed(&context, &room, &settings.owner_user_id).await;
        }
    }

    let invited_ids = context
        .client
        .invited_rooms()
        .into_iter()
        .map(|room| room.room_id().to_string())
        .collect::<HashSet<_>>();
    let joined_rooms = context.client.joined_rooms();
    let joined_ids = joined_rooms
        .iter()
        .map(|room| room.room_id().to_string())
        .collect::<HashSet<_>>();

    for room in joined_rooms {
        let record = context.room_catalog.sync_sdk_room(&room, &settings).await?;
        ensure_room_worker(context.clone(), room, record.is_space).await?;
    }

    for room in context.database.fetch_rooms().await? {
        if joined_ids.contains(&room.room_id) {
            continue;
        }
        let membership = if invited_ids.contains(&room.room_id) {
            "invited"
        } else {
            "left"
        };
        context
            .database
            .upsert_room(
                &room.room_id,
                room.current_display_name.as_deref(),
                room.current_canonical_alias.as_deref(),
                &room.active_folder_label,
                room.is_space,
                membership,
            )
            .await?;
    }

    let stale_room_ids = {
        let room_workers = context.room_workers.lock().await;
        room_workers
            .keys()
            .filter(|room_id| !joined_ids.contains(*room_id))
            .cloned()
            .collect::<Vec<_>>()
    };

    for room_id in stale_room_ids {
        cleanup_tracked_space_if_needed(context.clone(), &room_id).await?;
        stop_room_worker(&context, &room_id).await;
    }

    publish_worker_states(&context).await;
    Ok(())
}

async fn focus_room_now(context: &Arc<RunningContext>, room_id: &str) -> Result<()> {
    let room_id = room_id.trim();
    if room_id.is_empty() {
        return Ok(());
    }
    *context.focused_room_id.write().await = Some(room_id.to_owned());

    let room = context
        .client
        .joined_rooms()
        .into_iter()
        .find(|room| room.room_id().as_str() == room_id)
        .ok_or_else(|| anyhow!("Room is not currently joined: {room_id}"))?;
    let settings = context.settings.read().await.clone();
    let room_record = context.room_catalog.sync_sdk_room(&room, &settings).await?;
    ensure_room_worker(context.clone(), room.clone(), room_record.is_space).await?;

    if room_record.is_space {
        return Ok(());
    }

    let timeline = room.timeline().await?;
    let (initial_items, mut stream) = timeline.subscribe().await;
    let initial_events = collect_events_from_vector(&initial_items, room_id)?;
    process_events(context, room_id, TimelineSource::Live, initial_events, None).await?;
    drain_timeline_stream(context, room_id, &mut stream, TimelineSource::Live, None).await?;

    let checkpoint = context.database.load_checkpoint(room_id).await?;
    if !checkpoint.initial_backfill_complete {
        let _ = timeline.paginate_backwards(50).await?;
        drain_timeline_stream(
            context,
            room_id,
            &mut stream,
            TimelineSource::InitialBackfill,
            checkpoint.oldest_backfilled_timestamp,
        )
        .await?;
    }

    ensure_room_thumbnail_warmer(context, room_id).await;

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "rooms",
            &format!("Prioritized foreground refresh for {room_id}."),
        )
        .await?;
    Ok(())
}

async fn ensure_room_thumbnail_warmer(context: &Arc<RunningContext>, room_id: &str) {
    let mut workers = context.room_workers.lock().await;
    let entry = workers
        .entry(room_id.to_owned())
        .or_insert_with(RoomWorkerState::new);
    if entry.thumbnail_task.is_some() {
        return;
    }

    let room_id_clone = room_id.to_owned();
    let context_clone = context.clone();
    entry.thumbnail_task = Some(tokio::spawn(async move {
        if let Err(error) = warm_room_thumbnail_cache(context_clone.clone(), &room_id_clone).await {
            let _ = context_clone
                .database
                .insert_log(
                    AppLogLevel::Warning,
                    "thumbnails",
                    &format!("Background thumbnail warmup failed for {room_id_clone}: {error:#}"),
                )
                .await;
        }

        let mut workers = context_clone.room_workers.lock().await;
        if let Some(worker) = workers.get_mut(&room_id_clone) {
            worker.thumbnail_task = None;
        }
    }));
}

async fn ensure_room_worker(
    context: Arc<RunningContext>,
    room: Room,
    is_space: bool,
) -> Result<()> {
    let room_id = room.room_id().to_string();
    let mut workers = context.room_workers.lock().await;
    let entry = workers
        .entry(room_id.clone())
        .or_insert_with(RoomWorkerState::new);

    if !is_space && entry.live_task.is_none() {
        let room_id_clone = room_id.clone();
        let room_clone = room.clone();
        let context_clone = context.clone();
        entry.live_watcher_active = true;
        entry.live_task = Some(tokio::spawn(async move {
            if let Err(error) = run_live_watcher(context_clone.clone(), room_clone).await {
                let _ = context_clone
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "rooms",
                        &format!("Live watcher failed for {room_id_clone}: {error:#}"),
                    )
                    .await;
            }
            let mut workers = context_clone.room_workers.lock().await;
            if let Some(worker) = workers.get_mut(&room_id_clone) {
                worker.live_watcher_active = false;
                worker.live_task = None;
            }
            drop(workers);
            publish_worker_states(&context_clone).await;
        }));
    }

    if entry.history_task.is_none() {
        let room_id_clone = room_id.clone();
        let room_clone = room.clone();
        let context_clone = context.clone();
        entry.history_task = Some(tokio::spawn(async move {
            if let Err(error) = run_history_task(context_clone.clone(), room_clone).await {
                let _ = context_clone
                    .database
                    .insert_log(
                        AppLogLevel::Error,
                        "history",
                        &format!("History worker failed for {room_id_clone}: {error:#}"),
                    )
                    .await;
                set_history_state(
                    &context_clone,
                    &room_id_clone,
                    RoomHistoryMode::Idle,
                    "Error",
                )
                .await;
            }

            let mut workers = context_clone.room_workers.lock().await;
            if let Some(worker) = workers.get_mut(&room_id_clone) {
                worker.history_task = None;
            }
            drop(workers);
            publish_worker_states(&context_clone).await;
        }));
    }

    drop(workers);
    publish_worker_states(&context).await;
    Ok(())
}

async fn stop_all_room_workers(context: &Arc<RunningContext>) {
    let room_ids = {
        let workers = context.room_workers.lock().await;
        workers.keys().cloned().collect::<Vec<_>>()
    };
    for room_id in room_ids {
        stop_room_worker(context, &room_id).await;
    }
    publish_worker_states(context).await;
}

async fn stop_room_worker(context: &Arc<RunningContext>, room_id: &str) {
    let worker = {
        let mut workers = context.room_workers.lock().await;
        workers.remove(room_id)
    };

    if let Some(mut worker) = worker {
        if let Some(handle) = worker.live_task.take() {
            handle.abort();
        }
        if let Some(handle) = worker.history_task.take() {
            handle.abort();
        }
        if let Some(handle) = worker.thumbnail_task.take() {
            handle.abort();
        }
    }
}

async fn publish_worker_states(context: &Arc<RunningContext>) {
    let snapshots = {
        let workers = context.room_workers.lock().await;
        let mut snapshots = workers
            .iter()
            .map(|(room_id, worker)| worker.snapshot(room_id))
            .collect::<Vec<_>>();
        snapshots.sort_by(|left, right| left.room_id.cmp(&right.room_id));
        snapshots
    };

    context
        .runtime
        .mutate(|runtime| runtime.worker_states = snapshots)
        .await;
}

async fn set_history_state(
    context: &Arc<RunningContext>,
    room_id: &str,
    mode: RoomHistoryMode,
    detail: &str,
) {
    {
        let mut workers = context.room_workers.lock().await;
        if let Some(worker) = workers.get_mut(room_id) {
            worker.history_mode = mode;
            worker.history_detail = detail.to_owned();
        }
    }
    publish_worker_states(context).await;
}

async fn run_live_watcher(context: Arc<RunningContext>, room: Room) -> Result<()> {
    let room_id = room.room_id().to_string();
    let timeline = room.timeline().await?;
    let (initial_items, mut stream) = timeline.subscribe().await;

    let initial_events = collect_events_from_vector(&initial_items, &room_id)?;
    process_events(
        &context,
        &room_id,
        TimelineSource::Live,
        initial_events,
        None,
    )
    .await?;

    while let Some(diffs) = stream.next().await {
        let events = collect_events_from_diffs(&diffs, &room_id)?;
        process_events(&context, &room_id, TimelineSource::Live, events, None).await?;
    }

    Ok(())
}

async fn run_history_task(context: Arc<RunningContext>, room: Room) -> Result<()> {
    let room_id = room.room_id().to_string();
    let checkpoint = context.database.load_checkpoint(&room_id).await?;
    let hierarchy_snapshot = fetch_room_hierarchy_snapshot(&context.client, &room_id)
        .await
        .ok();
    let is_space_room = hierarchy_snapshot
        .as_ref()
        .is_some_and(|snapshot| snapshot.is_space)
        || room.is_space()
        || context
            .database
            .room_record(&room_id)
            .await?
            .map(|record| record.is_space)
            .unwrap_or(false);

    if is_space_room {
        stop_live_watcher(&context, &room_id).await;
        set_history_state(
            &context,
            &room_id,
            RoomHistoryMode::InitialBackfill,
            "Scanning space rooms",
        )
        .await;
        let membership_changed =
            run_space_refresh(&context, &room_id, checkpoint, hierarchy_snapshot).await?;
        if membership_changed {
            schedule_room_refresh(context.clone());
        }
        return Ok(());
    }

    if checkpoint.initial_backfill_complete {
        set_history_state(
            &context,
            &room_id,
            RoomHistoryMode::ReconnectCatchUp,
            "Recovering missed messages",
        )
        .await;
        run_reconnect_catchup(&context, &room, checkpoint).await?;
    } else {
        set_history_state(
            &context,
            &room_id,
            RoomHistoryMode::InitialBackfill,
            "Scanning room history",
        )
        .await;
        run_initial_backfill(&context, &room).await?;
    }

    set_history_state(&context, &room_id, RoomHistoryMode::Complete, "Idle").await;
    Ok(())
}

async fn stop_live_watcher(context: &Arc<RunningContext>, room_id: &str) {
    let handle = {
        let mut workers = context.room_workers.lock().await;
        workers.get_mut(room_id).and_then(|worker| {
            worker.live_watcher_active = false;
            worker.live_task.take()
        })
    };

    if let Some(handle) = handle {
        handle.abort();
    }

    publish_worker_states(context).await;
}

async fn run_initial_backfill(context: &Arc<RunningContext>, room: &Room) -> Result<()> {
    let room_id = room.room_id().to_string();
    let settings = context.settings.read().await.clone();
    let timeline = room.timeline().await?;
    let (_initial_items, mut stream) = timeline.subscribe().await;
    let mut checkpoint;
    let mut resume_cutoff = context
        .database
        .load_checkpoint(&room_id)
        .await?
        .oldest_backfilled_timestamp;

    loop {
        drain_timeline_stream(
            context,
            &room_id,
            &mut stream,
            TimelineSource::InitialBackfill,
            resume_cutoff,
        )
        .await?;

        let reached_start = timeline.paginate_backwards(100).await?;
        drain_timeline_stream(
            context,
            &room_id,
            &mut stream,
            TimelineSource::InitialBackfill,
            resume_cutoff,
        )
        .await?;

        checkpoint = context.database.load_checkpoint(&room_id).await?;
        resume_cutoff = checkpoint.oldest_backfilled_timestamp;
        set_history_state(
            context,
            &room_id,
            RoomHistoryMode::InitialBackfill,
            &backfill_detail(&checkpoint, &settings),
        )
        .await;

        if reached_start || should_stop_initial_backfill(&checkpoint, &settings) {
            checkpoint.initial_backfill_complete = true;
            checkpoint.last_history_mode = RoomHistoryMode::Complete;
            checkpoint.last_history_run_at = Some(Utc::now());
            context.database.save_checkpoint(&checkpoint).await?;
            break;
        }
    }

    Ok(())
}

async fn run_reconnect_catchup(
    context: &Arc<RunningContext>,
    room: &Room,
    checkpoint: RoomCheckpoint,
) -> Result<()> {
    let room_id = room.room_id().to_string();
    let timeline = room.timeline().await?;
    let (initial_items, mut stream) = timeline.subscribe().await;
    let initial_events = collect_events_from_vector(&initial_items, &room_id)?;
    process_events(
        context,
        &room_id,
        TimelineSource::ReconnectCatchUp,
        initial_events,
        checkpoint.last_processed_timestamp,
    )
    .await?;

    for _ in 0..10 {
        drain_timeline_stream(
            context,
            &room_id,
            &mut stream,
            TimelineSource::ReconnectCatchUp,
            checkpoint.last_processed_timestamp,
        )
        .await?;
        let reached_start = timeline.paginate_backwards(100).await?;
        drain_timeline_stream(
            context,
            &room_id,
            &mut stream,
            TimelineSource::ReconnectCatchUp,
            checkpoint.last_processed_timestamp,
        )
        .await?;
        if reached_start {
            break;
        }
    }

    Ok(())
}

async fn drain_timeline_stream<S>(
    context: &Arc<RunningContext>,
    room_id: &str,
    stream: &mut S,
    source: TimelineSource,
    cutoff: Option<DateTime<Utc>>,
) -> Result<()>
where
    S: futures_util::Stream<Item = Vec<VectorDiff<Arc<TimelineItem>>>> + Unpin,
{
    while let Ok(Some(diffs)) = tokio::time::timeout(Duration::from_millis(50), stream.next()).await
    {
        let events = collect_events_from_diffs(&diffs, room_id)?;
        process_events(context, room_id, source, events, cutoff).await?;
    }
    Ok(())
}

async fn run_space_refresh(
    context: &Arc<RunningContext>,
    room_id: &str,
    mut checkpoint: RoomCheckpoint,
    hierarchy_snapshot: Option<RoomHierarchySnapshot>,
) -> Result<bool> {
    let settings = context.settings.read().await.clone();
    let snapshot = match hierarchy_snapshot {
        Some(snapshot) => snapshot,
        None => fetch_room_hierarchy_snapshot(&context.client, room_id).await?,
    };
    context
        .room_catalog
        .sync_hierarchy_metadata(
            room_id,
            snapshot.display_name.clone(),
            snapshot.canonical_alias.clone(),
            "joined",
            &settings,
        )
        .await?;
    let membership_changed = reconcile_space_children(context, room_id, &snapshot.children).await?;

    checkpoint.initial_backfill_complete = true;
    checkpoint.last_history_mode = RoomHistoryMode::Complete;
    checkpoint.last_history_run_at = Some(Utc::now());
    context.database.save_checkpoint(&checkpoint).await?;

    let detail = if snapshot.children.len() == 1 {
        "Tracking 1 space room".to_owned()
    } else {
        format!("Tracking {} space rooms", snapshot.children.len())
    };
    set_history_state(context, room_id, RoomHistoryMode::Complete, &detail).await;
    Ok(membership_changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn sample_settings(username: &str) -> AppSettings {
        AppSettings {
            homeserver_url: "https://fantasyhaven.me".to_owned(),
            username: username.to_owned(),
            owner_user_id: String::new(),
            destination_root_path: String::new(),
            library_root_path: String::new(),
            flat_folder_layout: false,
            archive_root_path: String::new(),
            archive_scan_enabled: false,
            archive_scan_high_priority: false,
            manual_download_root_path: String::new(),
            message_limit: 0,
            time_window_value: 0,
            time_window_unit: crate::domain::TimeWindowUnit::Day,
            retry_cooldown_minutes: 0,
            retry_limit: 0,
            download_worker_count: 1,
            failed_job_retention_value: 0,
            failed_job_retention_unit: crate::domain::FailedJobRetentionUnit::Day,
            primary_gateway_url: "https://dweb.link".to_owned(),
            preferred_gateway_urls: vec!["https://dweb.link".to_owned()],
            autostart_enabled: false,
            minimize_to_tray: true,
            start_hidden: false,
            bandwidth_limit_kib_per_sec: 0,
            preview_worker_count: 1,
            auto_join_space_rooms: false,
            auto_download_new_media: false,
            self_heal_enabled: false,
            desired_power_state: true,
        }
    }

    #[test]
    fn connection_state_mapping_matches_swift_port_behavior() {
        assert_eq!(
            connection_state_from_sync_state(&SyncState::Idle),
            ConnectionState::Starting
        );
        assert_eq!(
            connection_state_from_sync_state(&SyncState::Running),
            ConnectionState::Running
        );
        assert_eq!(
            connection_state_from_sync_state(&SyncState::Terminated),
            ConnectionState::Stopped
        );
    }

    #[test]
    fn stored_session_restore_only_matches_same_login_identity() {
        let stored_session = StoredSession {
            access_token: "token".to_owned(),
            refresh_token: None,
            user_id: "@meow:fantasyhaven.me".to_owned(),
            device_id: "DEVICE".to_owned(),
            homeserver_url: "https://fantasyhaven.me".to_owned(),
            sliding_sync_version: None,
        };

        let exact_settings = sample_settings("@meow:fantasyhaven.me");
        let localpart_settings = sample_settings("meow");
        let different_user_settings = sample_settings("someoneelse");

        assert!(stored_session_matches_settings_login(
            &stored_session,
            &exact_settings
        ));
        assert!(stored_session_matches_settings_login(
            &stored_session,
            &localpart_settings
        ));
        assert!(!stored_session_matches_settings_login(
            &stored_session,
            &different_user_settings
        ));
    }

    #[test]
    fn runtime_user_id_from_settings_preserves_full_matrix_id() {
        let settings = sample_settings("@meow:fantasyhaven.me");

        assert_eq!(
            runtime_user_id_from_settings(&settings).as_deref(),
            Some("@meow:fantasyhaven.me")
        );
    }

    #[test]
    fn runtime_user_id_from_settings_expands_localpart_with_homeserver() {
        let settings = sample_settings("meow");

        assert_eq!(
            runtime_user_id_from_settings(&settings).as_deref(),
            Some("@meow:fantasyhaven.me")
        );
    }

    #[test]
    fn hierarchy_response_tolerates_rooms_missing_room_id() {
        let response: SpaceHierarchyResponse = serde_json::from_value(json!({
            "rooms": [
                {
                    "room_id": "!space:example.org",
                    "room_type": "m.space",
                    "name": "Example Space",
                    "children_state": [
                        {
                            "type": "m.space.child",
                            "state_key": "!child:example.org",
                            "content": { "via": ["example.org"] }
                        }
                    ]
                },
                {
                    "name": "Malformed Child"
                }
            ]
        }))
        .expect("hierarchy response should deserialize");

        assert_eq!(response.rooms.len(), 2);
        assert_eq!(
            response.rooms[0].room_id.as_deref(),
            Some("!space:example.org")
        );
        assert_eq!(response.rooms[1].room_id, None);
    }

    #[test]
    fn ipfs_discovery_prefers_explicit_media_link_over_landing_page() {
        let settings = sample_settings("meow");
        let body = "\
Example.webm
Download link: https://dweb.link/ipfs/bafklandingpage
IPFS media: ipfs://bafybeicba7jmibru2lzxrm7wpy7ydkxgh2xmgko2cq5qwveytsz3p4wgnm";

        let discovery = ipfs_discovery_from_body(
            "!room:example.org",
            "$event",
            Utc::now(),
            body,
            &settings,
        )
        .expect("discovery parse should succeed")
        .expect("discovery should be detected");

        assert_eq!(
            discovery.direct_url.as_deref(),
            Some("https://dweb.link/ipfs/bafybeicba7jmibru2lzxrm7wpy7ydkxgh2xmgko2cq5qwveytsz3p4wgnm?download=1")
        );
        assert_eq!(discovery.mxc_url, "https://dweb.link/ipfs/bafklandingpage");
        assert_eq!(discovery.original_filename.as_deref(), Some("Example.webm"));
    }

    #[test]
    fn preferred_message_discovery_prefers_ipfs_media_over_matrix_copy() {
        let matrix = AttachmentDiscovery {
            room_id: "!room:example.org".to_owned(),
            event_id: "$event".to_owned(),
            origin_server_timestamp: Utc::now(),
            source_kind: MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: "mxc://example.org/media".to_owned(),
            fallback_source_url: None,
            thumbnail_source_url: Some("mxc://example.org/thumb".to_owned()),
            thumbnail_cached_path: None,
            original_filename: Some("MatrixName.webm".to_owned()),
            mime_type: Some("video/webm".to_owned()),
            category: MediaCategory::Videos,
        };
        let ipfs = AttachmentDiscovery {
            room_id: "!room:example.org".to_owned(),
            event_id: "$event".to_owned(),
            origin_server_timestamp: Utc::now(),
            source_kind: MediaSourceKind::Ipfs,
            direct_url: Some("https://dweb.link/ipfs/bafyraw?download=1".to_owned()),
            mxc_url: "https://dweb.link/ipfs/bafkpage".to_owned(),
            fallback_source_url: None,
            thumbnail_source_url: None,
            thumbnail_cached_path: None,
            original_filename: None,
            mime_type: None,
            category: MediaCategory::Other,
        };

        let merged = preferred_message_discovery(Some(matrix), Some(ipfs))
            .expect("discovery should be selected");

        assert_eq!(merged.source_kind, MediaSourceKind::Ipfs);
        assert_eq!(
            merged.direct_url.as_deref(),
            Some("https://dweb.link/ipfs/bafyraw?download=1")
        );
        assert_eq!(merged.original_filename.as_deref(), Some("MatrixName.webm"));
        assert_eq!(merged.mime_type.as_deref(), Some("video/webm"));
        assert_eq!(merged.category, MediaCategory::Videos);
        assert_eq!(
            merged.thumbnail_source_url.as_deref(),
            Some("mxc://example.org/thumb")
        );
        assert_eq!(
            merged.fallback_source_url.as_deref(),
            Some("mxc://example.org/media")
        );
    }
}

fn should_cache_discovery_thumbnail(discovery: &AttachmentDiscovery) -> bool {
    if discovery
        .thumbnail_cached_path
        .as_deref()
        .is_some_and(|path| Path::new(path).exists())
    {
        return false;
    }

    discovery
        .thumbnail_source_url
        .as_deref()
        .is_some_and(|value| !value.trim().is_empty())
}

fn should_fetch_ipfs_landing_page_thumbnail(discovery: &AttachmentDiscovery) -> bool {
    if discovery.source_kind != MediaSourceKind::Ipfs {
        return false;
    }
    if discovery
        .thumbnail_source_url
        .as_deref()
        .is_some_and(|value| !value.trim().is_empty())
    {
        return false;
    }
    let landing_page_url = discovery.mxc_url.trim();
    let Some(direct_url) = discovery.direct_url.as_deref() else {
        return false;
    };
    !landing_page_url.is_empty()
        && (landing_page_url.starts_with("http://") || landing_page_url.starts_with("https://"))
        && landing_page_url != direct_url
}

async fn cache_discovery_thumbnail(
    context: &Arc<RunningContext>,
    discovery: &AttachmentDiscovery,
) -> Result<Option<String>> {
    if let Some(existing_path) = discovery.thumbnail_cached_path.as_deref() {
        if tokio_fs::try_exists(existing_path).await.unwrap_or(false) {
            return Ok(Some(existing_path.to_owned()));
        }
    }

    let Some(source) = discovery
        .thumbnail_source_url
        .as_deref()
        .filter(|value| !value.trim().is_empty())
    else {
        return Ok(None);
    };

    let thumbnail_bytes = load_discovery_thumbnail_bytes(&context.client, source).await?;
    let image = load_oriented_image_from_bytes(&thumbnail_bytes)
        .with_context(|| format!("Thumbnail bytes were not a valid image for {}", discovery.event_id))?;
    let thumbnail = image.thumbnail(384, 384);

    tokio_fs::create_dir_all(&context.paths.thumbnail_cache_path).await?;
    let cache_path = thumbnail_cache_file_path(&context.paths, &discovery.room_id, &discovery.event_id);
    let cache_path_for_write = cache_path.clone();
    tokio::task::spawn_blocking(move || -> Result<()> {
        thumbnail
            .save_with_format(&cache_path_for_write, image::ImageFormat::Jpeg)
            .with_context(|| format!("Failed to save {}", cache_path_for_write.display()))
    })
    .await??;

    let cached_path = cache_path.to_string_lossy().to_string();
    context
        .database
        .set_discovery_thumbnail_cached_path(
            &discovery.room_id,
            &discovery.event_id,
            Some(&cached_path),
        )
        .await?;
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "thumbnails",
            &format!(
                "Cached thumbnail for {} in {} at {}",
                discovery.event_id, discovery.room_id, cached_path
            ),
        )
        .await?;
    Ok(Some(cached_path))
}

async fn enrich_ipfs_discovery_thumbnail_from_landing_page(
    context: &Arc<RunningContext>,
    discovery: &AttachmentDiscovery,
) -> Result<()> {
    if !should_fetch_ipfs_landing_page_thumbnail(discovery) {
        return Ok(());
    }

    let landing_page_url = discovery.mxc_url.clone();
    let html = fetch_http_text(&landing_page_url, true).await?;
    let Some(thumbnail_url) = extract_landing_page_thumbnail_url(&landing_page_url, &html)? else {
        return Ok(());
    };

    context
        .database
        .set_discovery_thumbnail_source_url(
            &discovery.room_id,
            &discovery.event_id,
            Some(&thumbnail_url),
        )
        .await?;

    let mut updated = discovery.clone();
    updated.thumbnail_source_url = Some(thumbnail_url.clone());
    let _ = cache_discovery_thumbnail(context, &updated).await;

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "thumbnails",
            &format!(
                "Resolved landing-page thumbnail for {} in {} from {}",
                discovery.event_id, discovery.room_id, landing_page_url
            ),
        )
        .await?;
    Ok(())
}

async fn warm_room_thumbnail_cache(
    context: Arc<RunningContext>,
    room_id: &str,
) -> Result<()> {
    loop {
        if context.focused_room_id.read().await.as_deref() != Some(room_id) {
            break;
        }

        let discoveries = context
            .database
            .fetch_room_discoveries_missing_thumbnails(room_id, ROOM_THUMBNAIL_WARM_BATCH_SIZE)
            .await?;
        if discoveries.is_empty() {
            break;
        }

        let mut made_progress = false;
        for discovery in discoveries {
            if context.focused_room_id.read().await.as_deref() != Some(room_id) {
                return Ok(());
            }

            let mut current = discovery;
            if should_fetch_ipfs_landing_page_thumbnail(&current) {
                match enrich_ipfs_discovery_thumbnail_from_landing_page(&context, &current).await {
                    Ok(()) => {
                        if let Some(updated) = context
                            .database
                            .discovery_record(&current.room_id, &current.event_id)
                            .await?
                        {
                            current = updated;
                        }
                        made_progress = true;
                    }
                    Err(error) => {
                        let _ = context
                            .database
                            .insert_log(
                                AppLogLevel::Warning,
                                "thumbnails",
                                &format!(
                                    "Failed background landing-page thumbnail lookup for {} in {}: {error:#}",
                                    current.event_id, current.room_id
                                ),
                            )
                            .await;
                    }
                }
            }

            if should_cache_discovery_thumbnail(&current) {
                match cache_discovery_thumbnail(&context, &current).await {
                    Ok(Some(_)) => {
                        made_progress = true;
                    }
                    Ok(None) => {}
                    Err(error) => {
                        let _ = context
                            .database
                            .insert_log(
                                AppLogLevel::Warning,
                                "thumbnails",
                                &format!(
                                    "Failed background thumbnail cache for {} in {}: {error:#}",
                                    current.event_id, current.room_id
                                ),
                            )
                            .await;
                    }
                }
            }

            sleep(ROOM_THUMBNAIL_WARM_ITEM_DELAY).await;
        }

        if !made_progress {
            break;
        }

        sleep(ROOM_THUMBNAIL_WARM_BATCH_DELAY).await;
    }

    Ok(())
}

async fn load_discovery_thumbnail_bytes(client: &Client, source: &str) -> Result<Vec<u8>> {
    if source.starts_with("http://") || source.starts_with("https://") {
        let response = reqwest::Client::builder()
            .timeout(Duration::from_secs(30))
            .build()?
            .get(source)
            .header(USER_AGENT, "MatrixMediaShareClient/0.1")
            .send()
            .await?;
        if !response.status().is_success() {
            return Err(anyhow!(
                "Thumbnail download failed with status {} from {}",
                response.status(),
                source
            ));
        }
        return Ok(response.bytes().await?.to_vec());
    }

    let request = MediaRequestParameters {
        source: decode_media_source(source)?,
        format: MediaFormat::File,
    };
    client.media().get_media_content(&request, true).await.map_err(Into::into)
}

async fn fetch_http_text(url: &str, ipfs_like: bool) -> Result<String> {
    let parsed = reqwest::Url::parse(url)?;
    let timeout_total = if ipfs_like {
        Duration::from_secs(20 * 60)
    } else {
        Duration::from_secs(5 * 60)
    };
    let client = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .timeout(timeout_total)
        .build()?;
    let response = client
        .get(parsed)
        .header(USER_AGENT, "MatrixMediaShareClient/0.1")
        .send()
        .await?
        .error_for_status()?;
    Ok(response.text().await?)
}

fn extract_landing_page_thumbnail_url(base_url: &str, html: &str) -> Result<Option<String>> {
    let base = reqwest::Url::parse(base_url)?;
    let lower_html = html.to_ascii_lowercase();
    let Some(img_index) = lower_html.find("<img") else {
        return Ok(None);
    };
    let html_after_img = &html[img_index..];
    let lower_after_img = &lower_html[img_index..];
    let Some(src_index) = lower_after_img.find("src=") else {
        return Ok(None);
    };
    let value = &html_after_img[src_index + 4..];
    let mut chars = value.chars();
    let Some(quote) = chars.next() else {
        return Ok(None);
    };
    if quote != '"' && quote != '\'' {
        return Ok(None);
    }
    let remainder = &value[quote.len_utf8()..];
    let Some(end_index) = remainder.find(quote) else {
        return Ok(None);
    };
    let src = remainder[..end_index].trim();
    if src.is_empty() {
        return Ok(None);
    }
    Ok(Some(base.join(src)?.to_string()))
}

fn thumbnail_cache_file_path(paths: &AppPaths, room_id: &str, event_id: &str) -> PathBuf {
    let digest = Sha256::digest(format!("{room_id}:{event_id}").as_bytes());
    paths.thumbnail_cache_path.join(format!("{digest:x}.jpg"))
}

async fn process_events(
    context: &Arc<RunningContext>,
    room_id: &str,
    source: TimelineSource,
    events: Vec<ObservedTimelineEvent>,
    cutoff: Option<DateTime<Utc>>,
) -> Result<()> {
    let mut event_count = 0;
    let mut new_discovery_count = 0usize;
    let mut thumbnail_warmups = Vec::new();
    let mut landing_page_thumbnail_warmups = Vec::new();
    let mut oldest: Option<(String, DateTime<Utc>)> = None;
    let mut newest: Option<(String, DateTime<Utc>)> = None;
    let is_space_room = context
        .database
        .room_record(room_id)
        .await?
        .map(|record| record.is_space)
        .unwrap_or(false);
    let settings = if is_space_room {
        None
    } else {
        Some(context.settings.read().await.clone())
    };
    let should_warm_thumbnails = context
        .focused_room_id
        .read()
        .await
        .as_deref()
        == Some(room_id);

    for event in events {
        let ObservedTimelineEvent {
            event_id,
            sender,
            timestamp,
            command_body,
            discovery,
        } = event;

        if let Some(cutoff) = cutoff {
            let should_skip = match source {
                TimelineSource::ReconnectCatchUp => timestamp <= cutoff,
                TimelineSource::InitialBackfill => timestamp >= cutoff,
                TimelineSource::Live => false,
            };
            if should_skip {
                continue;
            }
        }

        event_count += 1;
        if oldest.as_ref().is_none_or(|(_, ts)| timestamp < *ts) {
            oldest = Some((event_id.clone(), timestamp));
        }
        if newest.as_ref().is_none_or(|(_, ts)| timestamp >= *ts) {
            newest = Some((event_id.clone(), timestamp));
        }

        if !is_space_room {
            let ipfs_discovery = command_body.as_deref().and_then(|body| {
                settings
                    .as_ref()
                    .and_then(|settings| {
                        ipfs_discovery_from_body(
                            room_id,
                            &event_id,
                            timestamp,
                            body,
                            settings,
                        )
                        .ok()
                        .flatten()
                    })
            });
            let discovery = preferred_message_discovery(discovery, ipfs_discovery);

            if let Some(discovery) = discovery {
                let auto_download = settings
                    .as_ref()
                    .is_some_and(|settings| settings.auto_download_new_media);
                let inserted = context
                    .database
                    .enqueue_discovery(&discovery, auto_download)
                    .await?;
                if inserted {
                    new_discovery_count += 1;
                }
                if should_warm_thumbnails && should_cache_discovery_thumbnail(&discovery) {
                    thumbnail_warmups.push(discovery.clone());
                }
                if should_warm_thumbnails && should_fetch_ipfs_landing_page_thumbnail(&discovery) {
                    landing_page_thumbnail_warmups.push(discovery.clone());
                }
            }
        }

        if source != TimelineSource::InitialBackfill {
            if let Some(body) = command_body.as_deref() {
                handle_owner_command(context, &event_id, &sender, body, room_id).await?;
            }
        }
    }

    if event_count == 0 {
        return Ok(());
    }

    let mut checkpoint = context.database.load_checkpoint(room_id).await?;
    match source {
        TimelineSource::Live | TimelineSource::ReconnectCatchUp => {
            if let Some((event_id, timestamp)) = newest {
                if checkpoint
                    .last_processed_timestamp
                    .is_none_or(|current| timestamp >= current)
                {
                    checkpoint.last_processed_event_id = Some(event_id);
                    checkpoint.last_processed_timestamp = Some(timestamp);
                }
            }
        }
        TimelineSource::InitialBackfill => {
            checkpoint.historical_message_count += event_count;
            if let Some((event_id, timestamp)) = oldest {
                if checkpoint
                    .oldest_backfilled_timestamp
                    .is_none_or(|current| timestamp < current)
                {
                    checkpoint.oldest_backfilled_event_id = Some(event_id);
                    checkpoint.oldest_backfilled_timestamp = Some(timestamp);
                }
            }
            checkpoint.last_history_mode = RoomHistoryMode::InitialBackfill;
            checkpoint.last_history_run_at = Some(Utc::now());
        }
    }

    context.database.save_checkpoint(&checkpoint).await?;

    if new_discovery_count > 0 {
        let removed_thumbnail_paths = context
            .database
            .prune_discovery_cache(MAX_DISCOVERY_CACHE_ENTRIES)
            .await?;
        for removed_path in removed_thumbnail_paths {
            let _ = tokio_fs::remove_file(&removed_path).await;
        }
    }

    for discovery in thumbnail_warmups {
        let context = context.clone();
        tokio::spawn(async move {
            if let Err(error) = cache_discovery_thumbnail(&context, &discovery).await {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Warning,
                        "thumbnails",
                        &format!(
                            "Failed to cache thumbnail for {} in {}: {error:#}",
                            discovery.event_id, discovery.room_id
                        ),
                    )
                    .await;
            }
        });
    }
    for discovery in landing_page_thumbnail_warmups {
        let context = context.clone();
        tokio::spawn(async move {
            if let Err(error) = enrich_ipfs_discovery_thumbnail_from_landing_page(&context, &discovery).await {
                let _ = context
                    .database
                    .insert_log(
                        AppLogLevel::Warning,
                        "thumbnails",
                        &format!(
                            "Failed to resolve landing-page thumbnail for {} in {}: {error:#}",
                            discovery.event_id, discovery.room_id
                        ),
                    )
                    .await;
            }
        });
    }
    Ok(())
}

async fn handle_owner_command(
    context: &Arc<RunningContext>,
    event_id: &str,
    sender: &str,
    body: &str,
    room_id: &str,
) -> Result<()> {
    let settings = context.settings.read().await.clone();
    if sender != settings.owner_user_id {
        return Ok(());
    }

    {
        let mut handled = context.handled_event_ids.lock().await;
        if !handled.insert(event_id.to_owned()) {
            return Ok(());
        }
    }

    let trimmed = body.trim();
    if !trimmed.starts_with("!matrixdl ") {
        return Ok(());
    }

    let mut components = trimmed.split_whitespace();
    let _ = components.next();
    let Some(command) = components.next() else {
        return Ok(());
    };
    if !command.eq_ignore_ascii_case("join") {
        return Ok(());
    }

    let target = components.collect::<Vec<_>>().join(" ");
    if target.trim().is_empty() {
        return Ok(());
    }

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "commands",
            &format!("Command from {sender} in {room_id}: {body}"),
        )
        .await?;

    match join_room(context, target.trim(), &[]).await {
        Ok(()) => {
            schedule_room_refresh(context.clone());
            context
                .database
                .insert_log(
                    AppLogLevel::Info,
                    "commands",
                    &format!("Followed join command: {}", target.trim()),
                )
                .await?;
            if context.client.user_id().map(|user_id| user_id.as_str())
                != Some(settings.owner_user_id.as_str())
            {
                let _ = send_owner_reply(context, "Joined", target.trim()).await;
            }
        }
        Err(error) => {
            context
                .database
                .insert_log(
                    AppLogLevel::Error,
                    "commands",
                    &format!("Join command failed for {}: {error:#}", target.trim()),
                )
                .await?;
            if context.client.user_id().map(|user_id| user_id.as_str())
                != Some(settings.owner_user_id.as_str())
            {
                let _ = send_owner_reply(
                    context,
                    "Join failed for",
                    &format!("{}: {error:#}", target.trim()),
                )
                .await;
            }
        }
    }

    Ok(())
}

async fn send_owner_reply(context: &Arc<RunningContext>, prefix: &str, detail: &str) -> Result<()> {
    let settings = context.settings.read().await.clone();
    let owner_id = UserId::parse(settings.owner_user_id)?;
    let room = if let Some(room) = context.client.get_dm_room(&owner_id) {
        room
    } else {
        context.client.create_dm(&owner_id).await?
    };
    let timeline = room.timeline().await?;
    let content = RoomMessageEventContent::text_plain(format!("{prefix} {detail}"));
    timeline.send(content.into()).await?;
    Ok(())
}

fn verification_request_state_label(state: &VerificationRequestState) -> &'static str {
    match state {
        VerificationRequestState::Created { .. } => "created",
        VerificationRequestState::Requested { .. } => "requested",
        VerificationRequestState::Ready { .. } => "ready",
        VerificationRequestState::Transitioned { .. } => "inProgress",
        VerificationRequestState::Done => "done",
        VerificationRequestState::Cancelled(_) => "cancelled",
    }
}

async fn bootstrap_cross_signing_if_needed(context: &Arc<RunningContext>) -> Result<bool> {
    let Some(user_id) = context.client.user_id() else {
        return Ok(false);
    };

    if context
        .client
        .encryption()
        .get_user_identity(user_id)
        .await?
        .is_some()
    {
        return Ok(false);
    }

    let settings = context.settings.read().await.clone();
    let password = context.secret_store.load_password().unwrap_or_default();
    if password.trim().is_empty() {
        return Err(anyhow!("A saved password is required to set up Matrix verification for this device."));
    }

    match context.client.encryption().bootstrap_cross_signing_if_needed(None).await {
        Ok(()) => {}
        Err(error) => {
            if let Some(response) = error.as_uiaa_response() {
                let login_id =
                    runtime_user_id_from_settings(&settings).unwrap_or_else(|| settings.username.trim().to_owned());
                let mut auth = uiaa::Password::new(
                    uiaa::UserIdentifier::UserIdOrLocalpart(login_id),
                    password,
                );
                auth.session = response.session.clone();
                context
                    .client
                    .encryption()
                    .bootstrap_cross_signing(Some(uiaa::AuthData::Password(auth)))
                    .await?;
            } else {
                return Err(error.into());
            }
        }
    }

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "verification",
            "Set up cross-signing for this client account.",
        )
        .await?;
    Ok(true)
}

async fn request_verification(context: &Arc<RunningContext>) -> Result<()> {
    let _ = bootstrap_cross_signing_if_needed(context).await?;

    let own_user_id = context
        .client
        .user_id()
        .ok_or_else(|| anyhow!("Unable to determine the current Matrix user for verification"))?;
    let own_identity = context
        .client
        .encryption()
        .get_user_identity(own_user_id)
        .await?
        .ok_or_else(|| anyhow!("Cross-signing has not finished initializing yet. Give the client a moment and try again."))?;

    let current_device_id = context.client.device_id().map(ToString::to_string);
    let other_device_count = context
        .client
        .encryption()
        .get_user_devices(own_user_id)
        .await?
        .devices()
        .filter(|device| Some(device.device_id().as_str()) != current_device_id.as_deref())
        .count();

    if other_device_count == 0 {
        return Err(anyhow!(
            "No other logged-in devices were found for this account. Add another device if you want to run SAS verification."
        ));
    }

    let request = own_identity.request_verification().await?;
    let watcher_context = context.clone();
    let watcher_request = request.clone();
    let request_watcher = tokio::spawn(async move {
        let mut changes = watcher_request.changes();
        while changes.next().await.is_some() {
            let _ = refresh_verification_snapshot(&watcher_context).await;
        }
    });

    let mut verification = context.verification.lock().await;
    if let Some(task) = verification.request_task.take() {
        task.abort();
    }
    if let Some(task) = verification.sas_task.take() {
        task.abort();
    }
    verification.request = Some(request);
    verification.request_task = Some(request_watcher);
    verification.sas = None;
    drop(verification);

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "verification",
            "Requested verification from another signed-in device.",
        )
        .await?;
    refresh_verification_snapshot(context).await?;
    Ok(())
}

async fn start_sas_verification(context: &Arc<RunningContext>) -> Result<()> {
    let (request, sas) = {
        let verification = context.verification.lock().await;
        (verification.request.clone(), verification.sas.clone())
    };

    if let Some(sas) = sas {
        if !sas.can_be_presented() && !sas.is_done() && !sas.is_cancelled() {
            sas.accept().await?;
            context
                .database
                .insert_log(AppLogLevel::Info, "verification", "Accepted the SAS verification flow.")
                .await?;
            refresh_verification_snapshot(context).await?;
            return Ok(());
        }

        return Err(anyhow!("SAS verification is already active."));
    }

    let request = request.ok_or_else(|| anyhow!("No verification request is active."))?;
    match request.state() {
        VerificationRequestState::Requested { .. } => {
            request.accept().await?;
            context
                .database
                .insert_log(AppLogLevel::Info, "verification", "Accepted the verification request.")
                .await?;
            refresh_verification_snapshot(context).await?;
            Ok(())
        }
        VerificationRequestState::Transitioned { verification } => {
            let sas = verification
                .sas()
                .ok_or_else(|| anyhow!("The verification transitioned into an unsupported flow."))?;
            if !sas.can_be_presented() && !sas.is_done() && !sas.is_cancelled() {
                sas.accept().await?;
                context
                    .database
                    .insert_log(AppLogLevel::Info, "verification", "Accepted the SAS verification flow.")
                    .await?;
            }
            refresh_verification_snapshot(context).await?;
            Ok(())
        }
        _ => {
            let sas = request
                .start_sas()
                .await?
                .ok_or_else(|| anyhow!("Verification request is not ready for SAS yet."))?;
            attach_sas_tracking(context, sas).await;
            context
                .database
                .insert_log(AppLogLevel::Info, "verification", "Started SAS verification.")
                .await?;
            refresh_verification_snapshot(context).await?;
            Ok(())
        }
    }
}

async fn approve_verification(context: &Arc<RunningContext>) -> Result<()> {
    let (sas, request) = {
        let verification = context.verification.lock().await;
        (verification.sas.clone(), verification.request.clone())
    };
    let sas = if let Some(sas) = sas {
        sas
    } else if let Some(request) = request {
        match request.state() {
            VerificationRequestState::Transitioned { verification } => verification
                .sas()
                .ok_or_else(|| anyhow!("The verification transitioned into an unsupported flow."))?,
            _ => return Err(anyhow!("No SAS verification is active.")),
        }
    } else {
        return Err(anyhow!("No SAS verification is active."));
    };
    sas.confirm().await?;
    context
        .database
        .insert_log(AppLogLevel::Info, "verification", "Approved the SAS verification.")
        .await?;
    refresh_verification_snapshot(context).await?;
    Ok(())
}

async fn decline_verification(context: &Arc<RunningContext>) -> Result<()> {
    let (sas, request) = {
        let verification = context.verification.lock().await;
        (verification.sas.clone(), verification.request.clone())
    };

    if let Some(sas) = sas {
        sas.mismatch().await?;
    } else if let Some(request) = request {
        match request.state() {
            VerificationRequestState::Transitioned { verification } => {
                if let Some(sas) = verification.sas() {
                    sas.mismatch().await?;
                } else {
                    request.cancel().await?;
                }
            }
            _ => request.cancel().await?,
        }
    } else {
        return Err(anyhow!("No verification flow is active."));
    }

    context
        .database
        .insert_log(AppLogLevel::Info, "verification", "Rejected the active verification flow.")
        .await?;
    refresh_verification_snapshot(context).await?;
    Ok(())
}

async fn refresh_verification_snapshot(context: &Arc<RunningContext>) -> Result<()> {
    let encryption = context.client.encryption();
    let state = encryption.verification_state().get();
    let status = match state {
        VerificationState::Unknown => VerificationStatus::Unknown,
        VerificationState::Verified => VerificationStatus::Verified,
        VerificationState::Unverified => VerificationStatus::Unverified,
    };

    let device_id = context.client.device_id().map(ToString::to_string);
    let own_user_id = context.client.user_id().map(|user_id| user_id.to_owned());
    let can_bootstrap_cross_signing = if let Some(user_id) = own_user_id.as_deref() {
        encryption.get_user_identity(user_id).await?.is_none()
    } else {
        false
    };
    let other_device_count = if let Some(user_id) = own_user_id.as_deref() {
        encryption
            .get_user_devices(user_id)
            .await?
            .devices()
            .filter(|device| Some(device.device_id().as_str()) != device_id.as_deref())
            .count() as u32
    } else {
        0
    };
    let own_device_verified = encryption
        .get_own_device()
        .await?
        .map(|device| device.is_verified())
        .unwrap_or(false);

    let (request_flow_id, request_state, has_active_request, request_ready, request_can_accept, has_active_sas, sas_can_accept, emojis, decimals) = {
        let verification = context.verification.lock().await;
        let request_state_value = verification.request.as_ref().map(|request| request.state());
        let request_flow_id = verification.request.as_ref().map(|request| request.flow_id().to_owned());
        let request_state = request_state_value
            .as_ref()
            .map(|state| verification_request_state_label(state).to_owned());
        let has_active_request = verification
            .request
            .as_ref()
            .map(|request| !request.is_done() && !request.is_cancelled())
            .unwrap_or(false);
        let request_ready = verification
            .request
            .as_ref()
            .map(|request| request.is_ready())
            .unwrap_or(false);
        let request_can_accept = matches!(request_state_value, Some(VerificationRequestState::Requested { .. }));
        let derived_sas = request_state_value.and_then(|state| match state {
            VerificationRequestState::Transitioned { verification } => verification.sas(),
            _ => None,
        });
        let active_sas = verification.sas.clone().or(derived_sas);

        if let Some(sas) = active_sas {
            let sas_can_accept = matches!(sas.state(), matrix_sdk::encryption::verification::SasState::Started { .. });
            let emojis = sas
                .emoji()
                .map(|values| {
                    values
                        .iter()
                        .map(|emoji| VerificationEmoji {
                            symbol: emoji.symbol.to_owned(),
                            description: emoji.description.to_owned(),
                        })
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default();
            let decimals = sas
                .decimals()
                .map(|values| vec![values.0, values.1, values.2])
                .unwrap_or_default();
            (
                request_flow_id,
                request_state,
                has_active_request,
                request_ready,
                request_can_accept,
                !sas.is_done() && !sas.is_cancelled(),
                sas_can_accept,
                emojis,
                decimals,
            )
        } else {
            (
                request_flow_id,
                request_state,
                has_active_request,
                request_ready,
                request_can_accept,
                false,
                false,
                Vec::new(),
                Vec::new(),
            )
        }
    };

    let message = if has_active_sas && sas_can_accept {
        "Your other device started SAS. Accept it here to reveal the verification codes.".to_owned()
    } else if has_active_sas {
        "Compare the emoji or decimal codes with your other device, then approve only if they match exactly.".to_owned()
    } else if has_active_request && request_can_accept {
        "Another device requested verification. Accept the request here to continue.".to_owned()
    } else if has_active_request && request_ready {
        "The verification request is ready. Start SAS on this device to compare codes.".to_owned()
    } else if has_active_request {
        "Waiting for your other device to accept the verification request.".to_owned()
    } else if can_bootstrap_cross_signing {
        "Verification has not been set up for this account yet. Use Set Up Verification on this page.".to_owned()
    } else if matches!(status, VerificationStatus::Verified) {
        "This device is verified.".to_owned()
    } else if own_device_verified {
        "This device is trusted locally. Waiting for Matrix sync to confirm the account verification state.".to_owned()
    } else if other_device_count == 0 {
        "No other logged-in devices were found for this account. Add another device if you want to run SAS verification.".to_owned()
    } else {
        format!("This device is not verified yet. {other_device_count} other device(s) are available for verification.")
    };

    context
        .runtime
        .mutate(|runtime| {
            runtime.verification = VerificationSnapshot {
                state: status,
                device_id: device_id.clone(),
                message: message.clone(),
                request_flow_id: request_flow_id.clone(),
                request_state: request_state.clone(),
                has_active_request,
                request_ready,
                request_can_accept,
                has_active_sas,
                sas_can_accept,
                can_bootstrap_cross_signing,
                other_device_count,
                emojis: emojis.clone(),
                decimals: decimals.clone(),
            };
        })
        .await;
    Ok(())
}

async fn attach_sas_tracking(context: &Arc<RunningContext>, sas: SasVerification) {
    let watcher_context = context.clone();
    let watcher_sas = sas.clone();
    let watcher = tokio::spawn(async move {
        let mut changes = watcher_sas.changes();
        while changes.next().await.is_some() {
            let _ = refresh_verification_snapshot(&watcher_context).await;
        }
    });

    let mut verification = context.verification.lock().await;
    if let Some(task) = verification.sas_task.take() {
        task.abort();
    }
    verification.sas = Some(sas);
    verification.sas_task = Some(watcher);
}


async fn accept_owner_invite_if_allowed(
    context: &Arc<RunningContext>,
    room: &Room,
    owner_user_id: &str,
) -> Result<bool> {
    let invite = room.invite_details().await?;
    let Some(inviter) = invite.inviter else {
        return Ok(false);
    };
    if inviter.user_id() != owner_user_id {
        context
            .database
            .insert_log(
                AppLogLevel::Info,
                "invites",
                &format!(
                    "Ignoring invite to {} from non-owner {}",
                    room.room_id(),
                    inviter.user_id()
                ),
            )
            .await?;
        return Ok(false);
    }

    room.join().await?;
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "invites",
            &format!("Accepted invite to {} from owner", room.room_id()),
        )
        .await?;
    Ok(true)
}

async fn join_room(
    context: &Arc<RunningContext>,
    room_id_or_alias: &str,
    via_servers: &[String],
) -> Result<()> {
    if !room_id_or_alias.starts_with('!') && !room_id_or_alias.starts_with('#') {
        return Err(anyhow!(
            "Room joins require a Matrix room alias (#room:server) or room ID (!id:server)."
        ));
    }

    let room_id_or_alias = RoomOrAliasId::parse(room_id_or_alias)?;
    let via = if !via_servers.is_empty() {
        via_servers
            .iter()
            .filter_map(|server| ServerName::parse(server.as_str()).ok())
            .map(OwnedServerName::from)
            .collect::<Vec<_>>()
    } else if let Ok(alias) = RoomAliasId::parse(room_id_or_alias.as_str()) {
        context
            .client
            .resolve_room_alias(&alias)
            .await
            .map(|response| response.servers)
            .unwrap_or_default()
    } else {
        Vec::new()
    };

    context
        .client
        .join_room_by_id_or_alias(room_id_or_alias.as_ref(), &via)
        .await?;
    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "rooms",
            &format!("Joined room {}", room_id_or_alias.as_str()),
        )
        .await?;
    Ok(())
}

async fn leave_room(context: &Arc<RunningContext>, room_id: &str) -> Result<()> {
    let room = context
        .client
        .joined_rooms()
        .into_iter()
        .find(|room| room.room_id().as_str() == room_id)
        .ok_or_else(|| anyhow!("Room not found: {room_id}"))?;

    let tracked_links = context
        .database
        .fetch_space_auto_join_links_for_space(room_id)
        .await?;
    room.leave().await?;
    context
        .database
        .insert_log(AppLogLevel::Info, "rooms", &format!("Left room {room_id}"))
        .await?;

    if !tracked_links.is_empty() {
        let _ = cleanup_tracked_space(context.clone(), room_id, "user left the space").await?;
    }

    Ok(())
}

async fn cleanup_tracked_space_if_needed(
    context: Arc<RunningContext>,
    parent_space_id: &str,
) -> Result<()> {
    let _ = cleanup_tracked_space(context, parent_space_id, "space is no longer joined").await?;
    Ok(())
}

async fn cleanup_tracked_space(
    context: Arc<RunningContext>,
    parent_space_id: &str,
    reason: &str,
) -> Result<bool> {
    let child_links = context
        .database
        .fetch_space_auto_join_links_for_space(parent_space_id)
        .await?;
    if child_links.is_empty() {
        return Ok(false);
    }
    let child_link_count = child_links.len();

    let mut membership_changed = false;
    for link in child_links {
        context
            .database
            .delete_space_auto_join_link(&link.space_room_id, &link.child_room_id)
            .await?;
        if link.auto_joined_by_bot {
            membership_changed |= maybe_leave_orphaned_auto_joined_room(
                context.clone(),
                &link.child_room_id,
                parent_space_id,
            )
            .await?;
        }
    }

    context
        .database
        .insert_log(
            AppLogLevel::Info,
            "rooms",
            &format!(
                "Stopped tracking {} space rooms from {}: {}",
                child_link_count, parent_space_id, reason
            ),
        )
        .await?;

    Ok(membership_changed)
}

async fn maybe_leave_orphaned_auto_joined_room(
    context: Arc<RunningContext>,
    room_id: &str,
    parent_space_id: &str,
) -> Result<bool> {
    let remaining_links = context
        .database
        .fetch_space_auto_join_links_for_child(room_id)
        .await?;
    if !remaining_links.is_empty() {
        return Ok(false);
    }

    let joined_room = context
        .client
        .joined_rooms()
        .into_iter()
        .find(|room| room.room_id().as_str() == room_id);
    if let Some(room) = joined_room {
        room.leave().await?;
        context
            .database
            .insert_log(
                AppLogLevel::Info,
                "rooms",
                &format!(
                    "Left {} after it was removed from space {}",
                    room_id, parent_space_id
                ),
            )
            .await?;
        return Ok(true);
    }

    Ok(false)
}

async fn reconcile_space_children(
    context: &Arc<RunningContext>,
    parent_space_id: &str,
    children: &[SpaceChildDescriptor],
) -> Result<bool> {
    let unique_children = children
        .iter()
        .filter(|child| child.room_id != parent_space_id && child.room_id.starts_with('!'))
        .cloned()
        .fold(
            HashMap::<String, SpaceChildDescriptor>::new(),
            |mut map, child| {
                map.entry(child.room_id.clone()).or_insert(child);
                map
            },
        );
    let current_child_ids = unique_children.keys().cloned().collect::<HashSet<_>>();
    let existing_links = context
        .database
        .fetch_space_auto_join_links_for_space(parent_space_id)
        .await?;
    let mut joined_room_ids = context
        .client
        .joined_rooms()
        .into_iter()
        .map(|room| room.room_id().to_string())
        .collect::<HashSet<_>>();
    let mut membership_changed = false;

    for child in unique_children.values() {
        if joined_room_ids.contains(&child.room_id) {
            context
                .database
                .upsert_space_auto_join_link(parent_space_id, &child.room_id, false)
                .await?;
            continue;
        }

        match join_room(context, &child.room_id, &child.via_servers).await {
            Ok(()) => {
                membership_changed = true;
                joined_room_ids.insert(child.room_id.clone());
                context
                    .database
                    .upsert_space_auto_join_link(parent_space_id, &child.room_id, true)
                    .await?;
                context
                    .database
                    .insert_log(
                        AppLogLevel::Info,
                        "rooms",
                        &format!("Joined {} from space {}", child.room_id, parent_space_id),
                    )
                    .await?;
            }
            Err(error) => {
                context
                    .database
                    .insert_log(
                        AppLogLevel::Warning,
                        "rooms",
                        &format!(
                            "Failed joining {} from space {}: {error:#}",
                            child.room_id, parent_space_id
                        ),
                    )
                    .await?;
            }
        }
    }

    for link in existing_links {
        if current_child_ids.contains(&link.child_room_id) {
            continue;
        }

        context
            .database
            .delete_space_auto_join_link(&link.space_room_id, &link.child_room_id)
            .await?;
        if link.auto_joined_by_bot {
            membership_changed |= maybe_leave_orphaned_auto_joined_room(
                context.clone(),
                &link.child_room_id,
                parent_space_id,
            )
            .await?;
        }
    }

    Ok(membership_changed)
}

async fn fetch_room_hierarchy_snapshot(
    client: &Client,
    room_id: &str,
) -> Result<RoomHierarchySnapshot> {
    let access_token = client
        .access_token()
        .ok_or_else(|| anyhow!("Client is not authenticated"))?;
    let homeserver_url = client.homeserver();
    let encoded_room_id = urlencoding::encode(room_id);
    let endpoint_paths = [
        format!("/_matrix/client/v1/rooms/{encoded_room_id}/hierarchy"),
        format!("/_matrix/client/unstable/org.matrix.msc2946/rooms/{encoded_room_id}/hierarchy"),
    ];

    let http_client = reqwest::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()?;
    let mut from_token: Option<String> = None;
    let mut root_room_type: Option<String> = None;
    let mut root_display_name: Option<String> = None;
    let mut root_canonical_alias: Option<String> = None;
    let mut direct_children: HashMap<String, HashSet<String>> = HashMap::new();
    let mut fallback_children = HashSet::new();
    let mut attempts = Vec::new();

    loop {
        let mut payload = None;
        for endpoint in &endpoint_paths {
            let mut url = homeserver_url.join(endpoint)?;
            {
                let mut pairs = url.query_pairs_mut();
                pairs.append_pair("max_depth", "1");
                pairs.append_pair("limit", "200");
                pairs.append_pair("suggested_only", "false");
                if let Some(token) = &from_token {
                    pairs.append_pair("from", token);
                }
            }

            let response = http_client
                .get(url.clone())
                .header(AUTHORIZATION, format!("Bearer {access_token}"))
                .header(ACCEPT, "application/json")
                .header(USER_AGENT, "MatrixMediaArchiver/0.1")
                .send()
                .await;

            let response = match response {
                Ok(response) => response,
                Err(error) => {
                    attempts.push(format!("{} -> {}", url, error));
                    continue;
                }
            };

            if response.status().as_u16() == 404 || response.status().as_u16() == 405 {
                attempts.push(format!("{} -> HTTP {}", url, response.status()));
                continue;
            }

            if !response.status().is_success() {
                let status = response.status();
                let body = response.text().await.unwrap_or_default();
                return Err(anyhow!(
                    "Space hierarchy request failed: [{}] {}",
                    status,
                    body
                ));
            }

            payload = Some(response.json::<SpaceHierarchyResponse>().await?);
            break;
        }

        let Some(page) = payload else {
            return Err(anyhow!(
                "Space hierarchy request failed: {}",
                attempts.join(" | ")
            ));
        };

        for room in page.rooms {
            let Some(page_room_id) = room.room_id else {
                continue;
            };

            if page_room_id == room_id {
                root_room_type = room.room_type.or(root_room_type);
                root_display_name = room.name.or(root_display_name);
                root_canonical_alias = room.canonical_alias.or(root_canonical_alias);
                for child in room
                    .children_state
                    .into_iter()
                    .filter(|child| child.event_type.as_deref() == Some("m.space.child"))
                {
                    let Some(state_key) = child.state_key else {
                        continue;
                    };
                    if state_key != room_id {
                        direct_children
                            .entry(state_key)
                            .or_default()
                            .extend(child.content.via.unwrap_or_default());
                    }
                }
                continue;
            }

            fallback_children.insert(page_room_id);
        }

        from_token = page.next_batch;
        if from_token.is_none() {
            break;
        }
    }

    let is_space = root_room_type.as_deref() == Some("m.space") || !direct_children.is_empty();
    let children = if direct_children.is_empty() {
        if is_space {
            fallback_children
                .into_iter()
                .filter(|child_room_id| child_room_id != room_id)
                .map(|room_id| SpaceChildDescriptor {
                    room_id,
                    via_servers: Vec::new(),
                })
                .collect::<Vec<_>>()
        } else {
            Vec::new()
        }
    } else {
        direct_children
            .into_iter()
            .map(|(room_id, via_servers)| SpaceChildDescriptor {
                room_id,
                via_servers: via_servers.into_iter().collect(),
            })
            .collect::<Vec<_>>()
    };

    Ok(RoomHierarchySnapshot {
        room_id: room_id.to_owned(),
        is_space,
        display_name: root_display_name,
        canonical_alias: root_canonical_alias,
        children,
    })
}

async fn prune_expired_failed_jobs(context: &Arc<RunningContext>) -> Result<()> {
    let settings = context.settings.read().await.clone();
    let Some(cutoff) = failed_job_cutoff(&settings) else {
        return Ok(());
    };

    let cleared = context.database.prune_permanent_failed_jobs(cutoff).await?;
    if cleared > 0 {
        context
            .database
            .insert_log(
                AppLogLevel::Info,
                "queue",
                &format!("Auto-cleared {cleared} permanently failed jobs."),
            )
            .await?;
    }
    Ok(())
}

async fn download_media_to_temp(
    client: &Client,
    database: &AppDatabase,
    paths: &AppPaths,
    runtime: &RuntimeStore,
    worker_id: i32,
    job: &DownloadJobRecord,
    homeserver_url: String,
) -> Result<PathBuf> {
    if let Some(direct_url) = job.direct_url.as_deref().filter(|value| !value.trim().is_empty()) {
        match download_http_url_to_temp(
            database,
            runtime,
            &paths.temp_downloads_path,
            worker_id,
            job,
            direct_url,
        )
        .await
        {
            Ok(temp_path) => return Ok(temp_path),
            Err(error) if should_fallback_to_matrix_attachment(job, &error) => {
                let fallback_source_url = job
                    .fallback_source_url
                    .as_deref()
                    .filter(|value| !value.trim().is_empty())
                    .ok_or_else(|| anyhow!("Missing Matrix fallback source for IPFS media."))?;
                database
                    .insert_log(
                        AppLogLevel::Warning,
                        "downloads",
                        &format!(
                            "IPFS media returned 404 for {}. Falling back to the Matrix attachment.",
                            job.original_filename
                                .clone()
                                .unwrap_or_else(|| job.event_id.clone())
                        ),
                    )
                    .await?;
                return download_matrix_source_to_temp(
                    client,
                    database,
                    paths,
                    runtime,
                    worker_id,
                    job,
                    fallback_source_url,
                    homeserver_url,
                )
                .await;
            }
            Err(error) => return Err(error),
        }
    }

    download_matrix_source_to_temp(
        client,
        database,
        paths,
        runtime,
        worker_id,
        job,
        &job.mxc_url,
        homeserver_url,
    )
    .await
}

fn should_fallback_to_matrix_attachment(job: &DownloadJobRecord, error: &anyhow::Error) -> bool {
    job.source_kind == MediaSourceKind::Ipfs
        && job
            .fallback_source_url
            .as_deref()
            .is_some_and(|value| !value.trim().is_empty())
        && error_is_http_not_found(error)
}

fn error_is_http_not_found(error: &anyhow::Error) -> bool {
    let lowered = error.to_string().to_ascii_lowercase();
    lowered.contains("status 404") || lowered.contains("404 not found")
}

async fn download_matrix_source_to_temp(
    client: &Client,
    database: &AppDatabase,
    paths: &AppPaths,
    runtime: &RuntimeStore,
    worker_id: i32,
    job: &DownloadJobRecord,
    source_url: &str,
    homeserver_url: String,
) -> Result<PathBuf> {
    let source = decode_media_source(source_url)?;
    if let MediaSource::Plain(uri) = &source {
        if let Some(path) = direct_remote_media_download_to_temp(
            database,
            runtime,
            &paths.temp_downloads_path,
            worker_id,
            job,
            uri.as_str(),
            &homeserver_url,
            client.access_token(),
        )
        .await?
        {
            return Ok(path);
        }
    }

    let mime = job
        .mime_type
        .as_deref()
        .unwrap_or("application/octet-stream")
        .parse::<Mime>()
        .unwrap_or(mime::APPLICATION_OCTET_STREAM);
    let request = MediaRequestParameters {
        source,
        format: MediaFormat::File,
    };
    let handle = client
        .media()
        .get_media_file(
            &request,
            job.original_filename.clone(),
            &mime,
            false,
            Some(paths.temp_downloads_path.to_string_lossy().to_string()),
        )
        .await?;

    let extension = media_classification::preferred_extension(
        job.original_filename.as_deref(),
        job.mime_type.as_deref(),
    );
    let temp_path = temp_download_path(&paths.temp_downloads_path, extension.as_deref());
    let persisted = handle
        .persist(&temp_path)
        .map_err(|error| anyhow!(error.error))?;
    let size = persisted
        .metadata()
        .map(|value| value.len() as i64)
        .unwrap_or_default();
    runtime
        .mutate(|snapshot| {
            if let Some(active) = snapshot
                .active_downloads
                .iter_mut()
                .find(|download| download.worker_id == worker_id)
            {
                active.received_bytes = size;
                active.total_bytes = Some(size);
            }
            if worker_id == 0 && snapshot.viewer.state == ViewerState::Downloading {
                snapshot.viewer.received_bytes = size;
                snapshot.viewer.total_bytes = Some(size);
            }
        })
        .await;
    persist_download_progress(database, job.id, size, Some(size), true).await?;
    Ok(temp_path)
}

async fn download_http_url_to_temp(
    database: &AppDatabase,
    runtime: &RuntimeStore,
    temp_root: &Path,
    worker_id: i32,
    job: &DownloadJobRecord,
    url: &str,
) -> Result<PathBuf> {
    let parsed = reqwest::Url::parse(url)?;
    let policy = http_download_policy(job);
    let http_client = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .timeout(policy.total_timeout)
        .build()?;
    let response = http_client
        .get(parsed.clone())
        .header(USER_AGENT, "MatrixMediaShareClient/0.1")
        .send()
        .await?;
    if !response.status().is_success() {
        return Err(anyhow!("Download failed with status {}", response.status()));
    }

    let total = response.content_length().map(|value| value as i64);
    let extension = media_classification::preferred_extension(
        job.original_filename.as_deref(),
        job.mime_type.as_deref(),
    )
    .or_else(|| {
        parsed
            .path_segments()
            .and_then(|segments| segments.last())
            .and_then(|segment| std::path::Path::new(segment).extension()?.to_str().map(ToOwned::to_owned))
    });
    stream_http_response_to_temp(
        database,
        runtime,
        temp_root,
        worker_id,
        job,
        response,
        extension.as_deref(),
        total,
        policy.stall_timeout,
    )
    .await
}

async fn direct_remote_media_download_to_temp(
    database: &AppDatabase,
    runtime: &RuntimeStore,
    temp_root: &Path,
    worker_id: i32,
    job: &DownloadJobRecord,
    mxc_url: &str,
    homeserver_url: &str,
    access_token: Option<String>,
) -> Result<Option<PathBuf>> {
    let Some((server_name, media_id)) = parse_mxc_url(mxc_url) else {
        return Ok(None);
    };

    let base = reqwest::Url::parse(homeserver_url)?;
    let encoded_server = urlencoding::encode(&server_name);
    let encoded_media_id = urlencoding::encode(&media_id);
    let candidate_paths = [
        format!("/_matrix/client/v1/media/download/{encoded_server}/{encoded_media_id}"),
        format!("/_matrix/media/v3/download/{encoded_server}/{encoded_media_id}"),
        format!("/_matrix/media/r0/download/{encoded_server}/{encoded_media_id}"),
    ];

    let policy = http_download_policy(job);
    let http_client = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(15))
        .timeout(policy.total_timeout)
        .build()?;
    for candidate_path in candidate_paths {
        let url = base.join(&candidate_path)?;
        let mut request = http_client
            .get(url.clone())
            .header(USER_AGENT, "MatrixMediaShareClient/0.1");
        if access_token.is_some() && same_origin(&url, &base) {
            request = request.header(
                AUTHORIZATION,
                format!("Bearer {}", access_token.clone().unwrap()),
            );
        }

        let response = match request.send().await {
            Ok(response) => response,
            Err(_) => continue,
        };
        if !response.status().is_success() {
            continue;
        }

        let total = response.content_length().map(|value| value as i64);
        let extension = media_classification::preferred_extension(
            job.original_filename.as_deref(),
            job.mime_type.as_deref(),
        );
        match stream_http_response_to_temp(
            database,
            runtime,
            temp_root,
            worker_id,
            job,
            response,
            extension.as_deref(),
            total,
            policy.stall_timeout,
        )
        .await
        {
            Ok(temp_path) => return Ok(Some(temp_path)),
            Err(_) => continue,
        }
    }

    Ok(None)
}

struct HttpDownloadPolicy {
    total_timeout: Duration,
    stall_timeout: Duration,
}

fn http_download_policy(job: &DownloadJobRecord) -> HttpDownloadPolicy {
    if job.source_kind == MediaSourceKind::Ipfs {
        return HttpDownloadPolicy {
            total_timeout: Duration::from_secs(20 * 60),
            stall_timeout: Duration::from_secs(90),
        };
    }

    HttpDownloadPolicy {
        total_timeout: Duration::from_secs(5 * 60),
        stall_timeout: Duration::from_secs(30),
    }
}

async fn stream_http_response_to_temp(
    database: &AppDatabase,
    runtime: &RuntimeStore,
    temp_root: &Path,
    worker_id: i32,
    job: &DownloadJobRecord,
    response: reqwest::Response,
    extension: Option<&str>,
    total: Option<i64>,
    stall_timeout: Duration,
) -> Result<PathBuf> {
    let temp_path = temp_download_path(temp_root, extension);
    let transfer = async {
        let mut file = tokio_fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp_path)
            .await?;
        let mut received = 0i64;
        let mut stream = response.bytes_stream();
        let mut last_persisted_received = -1i64;
        let mut last_persisted_total = None;
        while let Some(chunk) = timeout(stall_timeout, stream.next())
            .await
            .map_err(|_| {
                if job.source_kind == MediaSourceKind::Ipfs {
                    anyhow!("IPFS download timed out while waiting for gateway data.")
                } else {
                    anyhow!("Download timed out while waiting for remote data.")
                }
            })?
        {
            let chunk = chunk?;
            file.write_all(&chunk).await?;
            received += chunk.len() as i64;
            runtime
                .mutate(|snapshot| {
                    if let Some(active) = snapshot
                        .active_downloads
                        .iter_mut()
                        .find(|download| download.worker_id == worker_id)
                    {
                        active.received_bytes = received;
                        active.total_bytes = total;
                    }
                    if worker_id == 0 && snapshot.viewer.state == ViewerState::Downloading {
                        snapshot.viewer.received_bytes = received;
                        snapshot.viewer.total_bytes = total;
                    }
                })
                .await;
            persist_download_progress_if_needed(
                database,
                job.id,
                received,
                total,
                &mut last_persisted_received,
                &mut last_persisted_total,
                false,
            )
            .await?;
        }
        file.flush().await?;
        persist_download_progress_if_needed(
            database,
            job.id,
            received,
            total.or(Some(received)),
            &mut last_persisted_received,
            &mut last_persisted_total,
            true,
        )
        .await?;
        Ok::<(), anyhow::Error>(())
    }
    .await;

    if let Err(error) = transfer {
        let _ = tokio_fs::remove_file(&temp_path).await;
        return Err(error);
    }

    Ok(temp_path)
}

async fn persist_download_progress_if_needed(
    database: &AppDatabase,
    job_id: i64,
    received_bytes: i64,
    total_bytes: Option<i64>,
    last_persisted_received: &mut i64,
    last_persisted_total: &mut Option<i64>,
    force: bool,
) -> Result<()> {
    let should_persist = force
        || *last_persisted_received < 0
        || received_bytes.saturating_sub(*last_persisted_received) >= DOWNLOAD_PROGRESS_PERSIST_BYTES
        || total_bytes != *last_persisted_total;
    if !should_persist {
        return Ok(());
    }

    persist_download_progress(database, job_id, received_bytes, total_bytes, force).await?;
    *last_persisted_received = received_bytes;
    *last_persisted_total = total_bytes;
    Ok(())
}

async fn persist_download_progress(
    database: &AppDatabase,
    job_id: i64,
    received_bytes: i64,
    total_bytes: Option<i64>,
    force: bool,
) -> Result<()> {
    if job_id <= 0 && !force {
        return Ok(());
    }
    if job_id <= 0 {
        return Ok(());
    }
    database
        .update_job_progress(job_id, received_bytes.max(0), total_bytes)
        .await
}

async fn handle_job_failure(
    database: &AppDatabase,
    settings: &Arc<RwLock<AppSettings>>,
    job: &DownloadJobRecord,
    error: &anyhow::Error,
) -> Result<()> {
    let description = error.to_string();
    let lowered = description.to_ascii_lowercase();
    let settings = settings.read().await.clone();
    let next_eligible_at =
        Utc::now() + chrono::Duration::minutes(i64::from(settings.retry_cooldown_minutes.max(1)));
    if lowered.contains("decrypt") || lowered.contains("utd") {
        database
            .mark_job_undecryptable(job.id, next_eligible_at, &description)
            .await?;
        database
            .insert_log(
                AppLogLevel::Warning,
                "queue",
                &format!(
                    "Marked {} as undecryptable pending keys until {}.",
                    job.original_filename
                        .clone()
                        .unwrap_or_else(|| job.event_id.clone()),
                    next_eligible_at.to_rfc3339_opts(chrono::SecondsFormat::Secs, true),
                ),
            )
            .await?;
        return Ok(());
    }

    let retry_count = job.retry_count + 1;
    let permanently_failed = retry_count >= settings.retry_limit;
    database
        .mark_job_cooling_down(
            job.id,
            retry_count,
            next_eligible_at,
            &description,
            permanently_failed,
        )
        .await?;

    let level = if permanently_failed {
        AppLogLevel::Error
    } else {
        AppLogLevel::Warning
    };
    let message = if permanently_failed {
        format!(
            "{} failed permanently: {}",
            job.original_filename
                .clone()
                .unwrap_or_else(|| job.event_id.clone()),
            description
        )
    } else {
        format!(
            "{} failed. Cooling down until {}: {}",
            job.original_filename
                .clone()
                .unwrap_or_else(|| job.event_id.clone()),
            next_eligible_at,
            description
        )
    };
    database.insert_log(level, "queue", &message).await?;
    Ok(())
}

fn collect_events_from_vector(
    items: &Vector<Arc<TimelineItem>>,
    room_id: &str,
) -> Result<Vec<ObservedTimelineEvent>> {
    items
        .iter()
        .filter_map(|item| ObservedTimelineEvent::from_timeline_item(item, room_id).transpose())
        .collect()
}

fn collect_events_from_diffs(
    diffs: &[VectorDiff<Arc<TimelineItem>>],
    room_id: &str,
) -> Result<Vec<ObservedTimelineEvent>> {
    let mut events = Vec::new();
    for diff in diffs {
        match diff {
            VectorDiff::Append { values } | VectorDiff::Reset { values } => {
                for item in values {
                    if let Some(event) = ObservedTimelineEvent::from_timeline_item(item, room_id)? {
                        events.push(event);
                    }
                }
            }
            VectorDiff::PushBack { value }
            | VectorDiff::PushFront { value }
            | VectorDiff::Insert { value, .. }
            | VectorDiff::Set { value, .. } => {
                if let Some(event) = ObservedTimelineEvent::from_timeline_item(value, room_id)? {
                    events.push(event);
                }
            }
            VectorDiff::Clear
            | VectorDiff::Remove { .. }
            | VectorDiff::PopBack
            | VectorDiff::PopFront
            | VectorDiff::Truncate { .. } => {}
        }
    }
    Ok(events)
}

struct ObservedTimelineEvent {
    event_id: String,
    sender: String,
    timestamp: DateTime<Utc>,
    command_body: Option<String>,
    discovery: Option<AttachmentDiscovery>,
}

impl ObservedTimelineEvent {
    fn from_timeline_item(item: &Arc<TimelineItem>, room_id: &str) -> Result<Option<Self>> {
        let Some(event) = item.as_event() else {
            return Ok(None);
        };
        if !event.is_remote_event() {
            return Ok(None);
        }

        let timestamp = DateTime::from_timestamp_millis(event.timestamp().get().into())
            .ok_or_else(|| anyhow!("Invalid Matrix event timestamp"))?;
        let event_id = event
            .event_id()
            .map(ToString::to_string)
            .unwrap_or_else(|| format!("remote-{}-{}", event.timestamp().get(), event.sender()));
        let sender = event.sender().to_string();
        let command_body = event
            .content()
            .as_message()
            .map(|message| message.body().to_owned());
        let discovery = timeline_discovery(event, room_id, &event_id, timestamp)?;

        Ok(Some(Self {
            event_id,
            sender,
            timestamp,
            command_body,
            discovery,
        }))
    }
}

fn timeline_discovery(
    event: &matrix_sdk_ui::timeline::EventTimelineItem,
    room_id: &str,
    event_id: &str,
    timestamp: DateTime<Utc>,
) -> Result<Option<AttachmentDiscovery>> {
    let Some(message) = event.content().as_message() else {
        if let Some(sticker) = event.content().as_sticker() {
            let content = sticker.content();
            let source: MediaSource = content.source.clone().into();
            let encoded_source = encode_media_source(&source)?;
            return Ok(Some(AttachmentDiscovery {
                room_id: room_id.to_owned(),
                event_id: event_id.to_owned(),
                origin_server_timestamp: timestamp,
                source_kind: MediaSourceKind::Matrix,
                direct_url: None,
                mxc_url: encoded_source.clone(),
                fallback_source_url: None,
                thumbnail_source_url: Some(encoded_source),
                thumbnail_cached_path: None,
                original_filename: Some(content.body.clone()),
                mime_type: content.info.mimetype.clone(),
                category: MediaCategory::Images,
            }));
        }
        return Ok(None);
    };

    let discovery = match message.msgtype() {
        MessageType::Image(content) => Some(AttachmentDiscovery {
            room_id: room_id.to_owned(),
            event_id: event_id.to_owned(),
            origin_server_timestamp: timestamp,
            source_kind: MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: encode_media_source(&content.source)?,
            fallback_source_url: None,
            thumbnail_source_url: Some(encode_media_source(&content.source)?),
            thumbnail_cached_path: None,
            original_filename: content
                .filename
                .clone()
                .or_else(|| Some(content.body.clone())),
            mime_type: content.info.as_ref().and_then(|info| info.mimetype.clone()),
            category: MediaCategory::Images,
        }),
        MessageType::Video(content) => Some(AttachmentDiscovery {
            room_id: room_id.to_owned(),
            event_id: event_id.to_owned(),
            origin_server_timestamp: timestamp,
            source_kind: MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: encode_media_source(&content.source)?,
            fallback_source_url: None,
            thumbnail_source_url: content
                .info
                .as_ref()
                .and_then(|info| info.thumbnail_source.as_ref())
                .map(encode_media_source)
                .transpose()?,
            thumbnail_cached_path: None,
            original_filename: content
                .filename
                .clone()
                .or_else(|| Some(content.body.clone())),
            mime_type: content.info.as_ref().and_then(|info| info.mimetype.clone()),
            category: MediaCategory::Videos,
        }),
        MessageType::Audio(content) => Some(AttachmentDiscovery {
            room_id: room_id.to_owned(),
            event_id: event_id.to_owned(),
            origin_server_timestamp: timestamp,
            source_kind: MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: encode_media_source(&content.source)?,
            fallback_source_url: None,
            thumbnail_source_url: None,
            thumbnail_cached_path: None,
            original_filename: content
                .filename
                .clone()
                .or_else(|| Some(content.body.clone())),
            mime_type: content.info.as_ref().and_then(|info| info.mimetype.clone()),
            category: MediaCategory::Audio,
        }),
        MessageType::File(content) => Some(AttachmentDiscovery {
            room_id: room_id.to_owned(),
            event_id: event_id.to_owned(),
            origin_server_timestamp: timestamp,
            source_kind: MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: encode_media_source(&content.source)?,
            fallback_source_url: None,
            thumbnail_source_url: content
                .info
                .as_ref()
                .and_then(|info| info.thumbnail_source.as_ref())
                .map(encode_media_source)
                .transpose()?,
            thumbnail_cached_path: None,
            original_filename: content
                .filename
                .clone()
                .or_else(|| Some(content.body.clone())),
            mime_type: content.info.as_ref().and_then(|info| info.mimetype.clone()),
            category: media_classification::category(
                content.filename.as_deref().or(Some(content.body.as_str())),
                content
                    .info
                    .as_ref()
                    .and_then(|info| info.mimetype.as_deref()),
            ),
        }),
        _ => None,
    };

    Ok(discovery)
}

fn encode_media_source(source: &MediaSource) -> Result<String> {
    match source {
        MediaSource::Plain(uri) => Ok(uri.to_string()),
        MediaSource::Encrypted(file) => Ok(format!("encrypted:{}", serde_json::to_string(file)?)),
    }
}

fn decode_media_source(value: &str) -> Result<MediaSource> {
    if let Some(rest) = value.strip_prefix("encrypted:") {
        let file = serde_json::from_str(rest)?;
        return Ok(MediaSource::Encrypted(file));
    }
    Ok(MediaSource::Plain(OwnedMxcUri::from(value.to_owned())))
}

fn gateway_raw_url(
    settings: &AppSettings,
    cid: &str,
    remainder: Option<&str>,
) -> Result<String> {
    let gateway = if settings.primary_gateway_url.trim().is_empty() {
        BOOTSTRAP_PRIMARY_GATEWAY
    } else {
        settings.primary_gateway_url.trim()
    };
    let mut url = reqwest::Url::parse(gateway)?;
    let mut path = format!("/ipfs/{cid}");
    if let Some(remainder) = remainder.filter(|value| !value.is_empty()) {
        path.push('/');
        path.push_str(remainder.trim_start_matches('/'));
    }
    url.set_path(&path);
    url.set_query(Some("download=1"));
    Ok(url.to_string())
}

fn cid_and_remainder_from_path(path: &str) -> Option<(String, Option<String>)> {
    let mut segments = path.split('/').filter(|segment| !segment.is_empty());
    let marker = segments.next()?;
    if marker != "ipfs" {
        return None;
    }

    let cid = segments.next()?.to_owned();
    let remainder = {
        let parts = segments.collect::<Vec<_>>();
        if parts.is_empty() {
            None
        } else {
            Some(parts.join("/"))
        }
    };
    Some((cid, remainder))
}

fn looks_like_ipfs_cid(value: &str) -> bool {
    let trimmed = value.trim();
    !trimmed.is_empty()
        && !trimmed.contains(char::is_whitespace)
        && (trimmed.starts_with("Qm")
            || trimmed.starts_with("bafy")
            || trimmed.starts_with("bafk")
            || trimmed.starts_with("k51"))
}

async fn open_path_in_default_app(path: &Path) -> Result<()> {
    #[cfg(target_os = "macos")]
    let mut command = {
        let mut command = tokio::process::Command::new("open");
        command.arg(path);
        command
    };
    #[cfg(target_os = "linux")]
    let mut command = {
        let mut command = tokio::process::Command::new("xdg-open");
        command.arg(path);
        command
    };
    #[cfg(target_os = "windows")]
    let mut command = {
        let mut command = tokio::process::Command::new("cmd");
        command.args(["/C", "start", ""]).arg(path);
        command
    };

    command.stdout(Stdio::null()).stderr(Stdio::null());
    let status = command.status().await?;
    if status.success() {
        Ok(())
    } else {
        Err(anyhow!("Failed to open {}", path.display()))
    }
}

async fn validate_downloaded_media(path: &Path, category: MediaCategory) -> Result<()> {
    let metadata = tokio_fs::metadata(path).await?;
    if metadata.len() == 0 {
        return Err(anyhow!("file is empty"));
    }

    if category == MediaCategory::Images {
        let bytes = tokio_fs::read(path).await?;
        image::load_from_memory(&bytes).context("Downloaded image could not be decoded")?;
        return Ok(());
    }

    if category == MediaCategory::Videos {
        if probe_with_external_tool("ffprobe", path).await?
            || probe_with_external_tool("ffmpeg", path).await?
        {
            return Ok(());
        }
    }

    Ok(())
}

async fn probe_with_external_tool(tool: &str, path: &Path) -> Result<bool> {
    let executable = which(tool).await?;
    let Some(executable) = executable else {
        return Ok(false);
    };

    let output = if tool == "ffprobe" {
        tokio::process::Command::new(executable)
            .args([
                "-v",
                "error",
                "-show_entries",
                "stream=codec_type,width,height",
                "-of",
                "json",
            ])
            .arg(path)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .output()
            .await?
    } else {
        tokio::process::Command::new(executable)
            .args(["-v", "error", "-i"])
            .arg(path)
            .args(["-frames:v", "1", "-f", "null", "-"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .output()
            .await?
    };

    Ok(output.status.success())
}

async fn which(name: &str) -> Result<Option<PathBuf>> {
    let Some(path_var) = std::env::var_os("PATH") else {
        return Ok(None);
    };
    for path in std::env::split_paths(&path_var) {
        if !path.is_absolute() {
            continue;
        }

        for executable in candidate_executable_paths(&path, name) {
            let metadata = match tokio_fs::metadata(&executable).await {
                Ok(metadata) => metadata,
                Err(_) => continue,
            };
            if !metadata.is_file() {
                continue;
            }
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                if metadata.permissions().mode() & 0o111 == 0 {
                    continue;
                }
            }

            let canonical = tokio_fs::canonicalize(&executable)
                .await
                .unwrap_or(executable);
            return Ok(Some(canonical));
        }
    }
    Ok(None)
}

fn candidate_executable_paths(path: &Path, name: &str) -> Vec<PathBuf> {
    #[cfg(windows)]
    {
        return vec![path.join(name), path.join(format!("{name}.exe"))];
    }
    #[cfg(not(windows))]
    {
        vec![path.join(name)]
    }
}

async fn sha256_file(path: &Path) -> Result<String> {
    let mut file = TokioFile::open(path).await?;
    let mut buffer = vec![0_u8; 1024 * 1024];
    let mut hasher = Sha256::new();
    loop {
        let read = file.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

async fn copy_file_cross_filesystem(from: &Path, to: &Path) -> Result<()> {
    if let Some(parent) = to.parent() {
        tokio_fs::create_dir_all(parent).await?;
    }

    if tokio_fs::symlink_metadata(to)
        .await
        .map(|metadata| metadata.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(anyhow!(
            "Refusing to copy media through symbolic link {}",
            to.display()
        ));
    }

    if tokio_fs::try_exists(to).await.unwrap_or(false) {
        return Ok(());
    }

    let mut source = TokioFile::open(from).await?;
    let mut destination = tokio_fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(to)
        .await?;
    tokio::io::copy(&mut source, &mut destination).await?;
    destination.flush().await?;
    Ok(())
}

async fn move_file_cross_filesystem(from: &Path, to: &Path) -> Result<()> {
    if let Some(parent) = to.parent() {
        tokio_fs::create_dir_all(parent).await?;
    }

    if tokio_fs::symlink_metadata(to)
        .await
        .map(|metadata| metadata.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(anyhow!(
            "Refusing to write downloaded media through symbolic link {}",
            to.display()
        ));
    }

    match tokio_fs::hard_link(from, to).await {
        Ok(()) => {
            tokio_fs::remove_file(from).await?;
            Ok(())
        }
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
            Err(anyhow!("Destination file already exists: {}", to.display()))
        }
        Err(_) => {
            let mut source = TokioFile::open(from).await?;
            let mut destination = tokio_fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(to)
                .await?;
            tokio::io::copy(&mut source, &mut destination).await?;
            destination.flush().await?;
            drop(destination);
            tokio_fs::remove_file(from).await?;
            Ok(())
        }
    }
}

fn relative_storage_path(root: &str, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/")
        .trim_start_matches('/')
        .to_owned()
}

fn temp_download_path(root: &Path, extension: Option<&str>) -> PathBuf {
    let name = match extension.filter(|extension| !extension.is_empty()) {
        Some(extension) => format!("{}.{}", Uuid::new_v4(), extension),
        None => Uuid::new_v4().to_string(),
    };
    root.join(name)
}

fn parse_mxc_url(value: &str) -> Option<(String, String)> {
    let trimmed = value.strip_prefix("mxc://")?;
    let (server_name, media_id) = trimmed.split_once('/')?;
    if server_name.is_empty() || media_id.is_empty() {
        return None;
    }
    Some((server_name.to_owned(), media_id.to_owned()))
}

fn is_remote_media(mxc_url: &str, homeserver_url: &str) -> bool {
    let Some((server_name, _)) = parse_mxc_url(mxc_url) else {
        return false;
    };
    let remote_host = server_name
        .split(':')
        .next()
        .unwrap_or_default()
        .to_ascii_lowercase();
    let homeserver_host = reqwest::Url::parse(homeserver_url)
        .ok()
        .and_then(|url| url.host_str().map(|host| host.to_ascii_lowercase()))
        .unwrap_or_default();
    !remote_host.is_empty() && remote_host != homeserver_host
}

fn same_origin(left: &reqwest::Url, right: &reqwest::Url) -> bool {
    left.scheme() == right.scheme()
        && left.host_str() == right.host_str()
        && left.port_or_known_default() == right.port_or_known_default()
}

fn should_stop_initial_backfill(checkpoint: &RoomCheckpoint, settings: &AppSettings) -> bool {
    if settings.message_limit > 0 && checkpoint.historical_message_count >= settings.message_limit {
        return true;
    }
    let Some(cutoff) = history_cutoff(settings) else {
        return false;
    };
    checkpoint
        .oldest_backfilled_timestamp
        .is_some_and(|timestamp| timestamp <= cutoff)
}

fn backfill_detail(checkpoint: &RoomCheckpoint, settings: &AppSettings) -> String {
    if settings.message_limit > 0 {
        format!(
            "Scanning {} / {} messages",
            checkpoint.historical_message_count, settings.message_limit
        )
    } else {
        format!("Scanning {} messages", checkpoint.historical_message_count)
    }
}

fn history_cutoff(settings: &AppSettings) -> Option<DateTime<Utc>> {
    if settings.time_window_value <= 0 {
        return None;
    }
    let now = Utc::now();
    match settings.time_window_unit {
        crate::domain::TimeWindowUnit::None => None,
        crate::domain::TimeWindowUnit::Day => {
            Some(now - chrono::Duration::days(i64::from(settings.time_window_value)))
        }
        crate::domain::TimeWindowUnit::Week => {
            Some(now - chrono::Duration::weeks(i64::from(settings.time_window_value)))
        }
        crate::domain::TimeWindowUnit::Month => {
            Some(now - chrono::Duration::days(i64::from(settings.time_window_value) * 30))
        }
    }
}

fn failed_job_cutoff(settings: &AppSettings) -> Option<DateTime<Utc>> {
    if settings.failed_job_retention_value <= 0 {
        return None;
    }
    let value = i64::from(settings.failed_job_retention_value);
    let now = Utc::now();
    match settings.failed_job_retention_unit {
        FailedJobRetentionUnit::None => None,
        FailedJobRetentionUnit::Minute => Some(now - chrono::Duration::minutes(value)),
        FailedJobRetentionUnit::Hour => Some(now - chrono::Duration::hours(value)),
        FailedJobRetentionUnit::Day => Some(now - chrono::Duration::days(value)),
    }
}

async fn cleanup_temp_files(path: &Path) -> Result<()> {
    if !path.exists() {
        return Ok(());
    }
    let mut entries = tokio_fs::read_dir(path).await?;
    while let Some(entry) = entries.next_entry().await? {
        let _ = tokio_fs::remove_file(entry.path()).await;
    }
    Ok(())
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum TimelineSource {
    Live,
    InitialBackfill,
    ReconnectCatchUp,
}

#[derive(serde::Deserialize)]
#[serde(rename_all = "snake_case")]
struct SpaceHierarchyResponse {
    rooms: Vec<SpaceHierarchyRoom>,
    #[serde(alias = "nextBatch")]
    next_batch: Option<String>,
}

#[derive(serde::Deserialize)]
#[serde(rename_all = "snake_case")]
struct SpaceHierarchyRoom {
    #[serde(alias = "roomId")]
    room_id: Option<String>,
    #[serde(alias = "roomType")]
    room_type: Option<String>,
    name: Option<String>,
    #[serde(alias = "canonicalAlias")]
    canonical_alias: Option<String>,
    #[serde(default, alias = "childrenState")]
    children_state: Vec<SpaceHierarchyChildEvent>,
}

#[derive(serde::Deserialize)]
#[serde(rename_all = "snake_case")]
struct SpaceHierarchyChildEvent {
    #[serde(rename = "type")]
    event_type: Option<String>,
    #[serde(alias = "stateKey")]
    state_key: Option<String>,
    #[serde(default)]
    content: SpaceHierarchyChildContent,
}

#[derive(Default, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
struct SpaceHierarchyChildContent {
    via: Option<Vec<String>>,
}
