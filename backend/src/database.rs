use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration as StdDuration,
};

use anyhow::{Context, Result, anyhow};
use chrono::{DateTime, Duration, Utc};
use rusqlite::{Connection, OptionalExtension, Row, params};
use tokio::sync::Mutex;

use crate::domain::{
    ActivityLogEntry, AppLogLevel, AppSettings, ArchiveFileRecord, AttachmentDiscovery,
    DownloadJobRecord, DownloadJobState, FailedJobRetentionUnit, LocalAssetSourceKind,
    MediaCategory, RoomCheckpoint, RoomHistoryMode, RoomRecord, SpaceAutoJoinRecord,
    TimeWindowUnit, TrackedUploadRecord,
};

const MAX_RETAINED_LOG_ENTRIES: i64 = 5_000;
const LOG_RETENTION_DAYS: i64 = 30;

#[derive(Clone)]
pub struct AppDatabase {
    inner: Arc<Mutex<Connection>>,
    path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct FileHashCacheRecord {
    pub file_path: String,
    pub root_kind: String,
    pub sha256: String,
    pub file_size: i64,
    pub modified_at: Option<DateTime<Utc>>,
    pub last_verified_at: DateTime<Utc>,
}

impl AppDatabase {
    pub async fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_owned();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .with_context(|| format!("Failed to create {}", parent.display()))?;
        }

        let connection = Connection::open(&path)
            .with_context(|| format!("Failed to open {}", path.display()))?;
        connection
            .busy_timeout(StdDuration::from_secs(5))
            .context("Failed to configure SQLite busy timeout")?;
        connection
            .pragma_update(None, "journal_mode", "WAL")
            .context("Failed to enable SQLite WAL mode")?;
        connection
            .pragma_update(None, "synchronous", "NORMAL")
            .context("Failed to configure SQLite synchronous mode")?;
        connection
            .pragma_update(None, "foreign_keys", "ON")
            .context("Failed to enable SQLite foreign keys")?;
        let database = Self {
            inner: Arc::new(Mutex::new(connection)),
            path,
        };
        database.initialize_schema().await?;
        database.harden_permissions()?;
        Ok(database)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub async fn load_settings(&self, default_destination_root_path: &str) -> Result<AppSettings> {
        let connection = self.inner.lock().await;
        let mut statement =
            connection.prepare("SELECT * FROM app_settings ORDER BY id DESC LIMIT 1")?;
        let row = statement
            .query_row([], |row| Self::map_settings(row))
            .optional()
            .context("Failed to load app settings")?;

        if let Some(settings) = row {
            return Ok(normalize_settings(settings));
        }

        let defaults = AppSettings {
            homeserver_url: "https://matrix.org".to_owned(),
            username: String::new(),
            owner_user_id: String::new(),
            destination_root_path: default_destination_root_path.to_owned(),
            library_root_path: format!("{default_destination_root_path}/Shared Files"),
            flat_folder_layout: false,
            archive_root_path: String::new(),
            archive_scan_enabled: false,
            archive_scan_high_priority: false,
            manual_download_root_path: default_destination_root_path.to_owned(),
            message_limit: 5_000,
            time_window_value: 0,
            time_window_unit: TimeWindowUnit::None,
            retry_cooldown_minutes: 5,
            retry_limit: 10,
            download_worker_count: 1,
            failed_job_retention_value: 0,
            failed_job_retention_unit: FailedJobRetentionUnit::None,
            primary_gateway_url: "https://dweb.link".to_owned(),
            preferred_gateway_urls: vec![
                "https://dweb.link".to_owned(),
                "https://ipfs.io".to_owned(),
                "https://eu.orbitor.dev".to_owned(),
                "https://ipfs.ecolatam.com".to_owned(),
                "https://apac.orbitor.dev".to_owned(),
                "https://4everland.io".to_owned(),
            ],
            autostart_enabled: false,
            minimize_to_tray: true,
            start_hidden: false,
            dark_mode_enabled: false,
            bandwidth_limit_kib_per_sec: 0,
            preview_worker_count: 1,
            auto_join_space_rooms: false,
            auto_download_new_media: false,
            self_heal_enabled: false,
            desired_power_state: false,
        };
        drop(statement);
        drop(connection);
        self.save_settings(&defaults).await?;
        Ok(normalize_settings(defaults))
    }

    pub async fn save_settings(&self, settings: &AppSettings) -> Result<()> {
        let settings = normalize_settings(settings.clone());
        let connection = self.inner.lock().await;
        connection.execute("DELETE FROM app_settings", [])?;
        connection.execute(
            "INSERT INTO app_settings (
                homeserver_url, username, owner_user_id, destination_root_path,
                library_root_path, flat_folder_layout, archive_root_path, archive_scan_enabled, archive_scan_high_priority,
                manual_download_root_path,
                message_limit, time_window_value, time_window_unit,
                retry_cooldown_minutes, retry_limit, download_worker_count,
                failed_job_retention_value, failed_job_retention_unit,
                primary_gateway_url, preferred_gateway_urls,
                autostart_enabled, minimize_to_tray, start_hidden, dark_mode_enabled,
                bandwidth_limit_kib_per_sec, preview_worker_count,
                auto_join_space_rooms, auto_download_new_media, self_heal_enabled,
                desired_power_state, updated_at
            ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30, ?31)",
            params![
                settings.homeserver_url,
                settings.username,
                settings.owner_user_id,
                settings.destination_root_path,
                settings.library_root_path,
                if settings.flat_folder_layout { 1 } else { 0 },
                settings.archive_root_path,
                if settings.archive_scan_enabled { 1 } else { 0 },
                if settings.archive_scan_high_priority { 1 } else { 0 },
                settings.manual_download_root_path,
                settings.message_limit,
                settings.time_window_value,
                time_window_unit_key(settings.time_window_unit),
                settings.retry_cooldown_minutes,
                settings.retry_limit,
                clamp_download_worker_count(settings.download_worker_count),
                settings.failed_job_retention_value,
                failed_retention_unit_key(settings.failed_job_retention_unit),
                settings.primary_gateway_url,
                serde_json::to_string(&settings.preferred_gateway_urls)?,
                if settings.autostart_enabled { 1 } else { 0 },
                if settings.minimize_to_tray { 1 } else { 0 },
                if settings.start_hidden { 1 } else { 0 },
                if settings.dark_mode_enabled { 1 } else { 0 },
                settings.bandwidth_limit_kib_per_sec,
                settings.preview_worker_count,
                if settings.auto_join_space_rooms { 1 } else { 0 },
                if settings.auto_download_new_media { 1 } else { 0 },
                if settings.self_heal_enabled { 1 } else { 0 },
                if settings.desired_power_state { 1 } else { 0 },
                iso_now(),
            ],
        )?;
        drop(connection);
        self.harden_permissions()?;
        Ok(())
    }

    pub async fn upsert_room(
        &self,
        room_id: &str,
        display_name: Option<&str>,
        canonical_alias: Option<&str>,
        active_folder_label: &str,
        is_space: bool,
        membership: &str,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "INSERT INTO rooms (room_id, display_name, canonical_alias, active_folder_label, is_space, membership, updated_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
             ON CONFLICT(room_id) DO UPDATE SET
                display_name = excluded.display_name,
                canonical_alias = excluded.canonical_alias,
                active_folder_label = excluded.active_folder_label,
                is_space = excluded.is_space,
                membership = excluded.membership,
                updated_at = excluded.updated_at",
            params![
                room_id,
                display_name,
                canonical_alias,
                active_folder_label,
                if is_space { 1 } else { 0 },
                membership,
                iso_now(),
            ],
        )?;
        Ok(())
    }

    pub async fn fetch_rooms(&self) -> Result<Vec<RoomRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT room_id, display_name, canonical_alias, active_folder_label, is_space, membership, updated_at
             FROM rooms
             ORDER BY COALESCE(display_name, canonical_alias, room_id) COLLATE NOCASE ASC",
        )?;
        let rows = statement.query_map([], Self::map_room_record)?;
        collect_rows(rows)
    }

    pub async fn room_record(&self, room_id: &str) -> Result<Option<RoomRecord>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT room_id, display_name, canonical_alias, active_folder_label, is_space, membership, updated_at
                 FROM rooms WHERE room_id = ?1",
                params![room_id],
                Self::map_room_record,
            )
            .optional()
            .context("Failed to load room record")
    }

    pub async fn all_folder_labels(
        &self,
        excluding_room_id: Option<&str>,
    ) -> Result<HashSet<String>> {
        let connection = self.inner.lock().await;
        let mut labels = HashSet::new();
        match excluding_room_id {
            Some(room_id) => {
                let mut statement = connection
                    .prepare("SELECT active_folder_label FROM rooms WHERE room_id <> ?1")?;
                let rows = statement.query_map(params![room_id], |row| row.get::<_, String>(0))?;
                for value in rows {
                    labels.insert(value?.to_lowercase());
                }
            }
            None => {
                let mut statement = connection.prepare("SELECT active_folder_label FROM rooms")?;
                let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
                for value in rows {
                    labels.insert(value?.to_lowercase());
                }
            }
        }
        Ok(labels)
    }

    pub async fn insert_alias_history(&self, room_id: &str, aliases: &[String]) -> Result<()> {
        if aliases.is_empty() {
            return Ok(());
        }

        let connection = self.inner.lock().await;
        let now = iso_now();
        for alias in aliases {
            connection.execute(
                "INSERT INTO room_alias_history (room_id, alias, seen_at)
                 VALUES (?1, ?2, ?3)
                 ON CONFLICT(room_id, alias) DO UPDATE SET seen_at = excluded.seen_at",
                params![room_id, alias, now],
            )?;
        }
        Ok(())
    }

    pub async fn alias_history(&self, room_id: &str) -> Result<Vec<String>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT alias FROM room_alias_history WHERE room_id = ?1 ORDER BY seen_at DESC",
        )?;
        let rows = statement.query_map(params![room_id], |row| row.get::<_, String>(0))?;
        collect_rows(rows)
    }

    pub async fn upsert_space_auto_join_link(
        &self,
        space_room_id: &str,
        child_room_id: &str,
        auto_joined_by_bot: bool,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        let now = iso_now();
        connection.execute(
            "INSERT INTO space_auto_joins (
                space_room_id, child_room_id, auto_joined_by_bot, created_at, updated_at
             ) VALUES (?1, ?2, ?3, ?4, ?5)
             ON CONFLICT(space_room_id, child_room_id) DO UPDATE SET
                auto_joined_by_bot = CASE
                    WHEN space_auto_joins.auto_joined_by_bot = 1 OR excluded.auto_joined_by_bot = 1 THEN 1
                    ELSE 0
                END,
                updated_at = excluded.updated_at",
            params![
                space_room_id,
                child_room_id,
                if auto_joined_by_bot { 1 } else { 0 },
                now,
                now,
            ],
        )?;
        Ok(())
    }

    pub async fn fetch_space_auto_join_links_for_space(
        &self,
        space_room_id: &str,
    ) -> Result<Vec<SpaceAutoJoinRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT * FROM space_auto_joins WHERE space_room_id = ?1 ORDER BY child_room_id COLLATE NOCASE ASC",
        )?;
        let rows = statement.query_map(params![space_room_id], Self::map_space_auto_join)?;
        collect_rows(rows)
    }

    pub async fn fetch_space_auto_join_links_for_child(
        &self,
        child_room_id: &str,
    ) -> Result<Vec<SpaceAutoJoinRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT * FROM space_auto_joins WHERE child_room_id = ?1 ORDER BY space_room_id COLLATE NOCASE ASC",
        )?;
        let rows = statement.query_map(params![child_room_id], Self::map_space_auto_join)?;
        collect_rows(rows)
    }

    pub async fn delete_space_auto_join_link(
        &self,
        space_room_id: &str,
        child_room_id: &str,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM space_auto_joins WHERE space_room_id = ?1 AND child_room_id = ?2",
            params![space_room_id, child_room_id],
        )?;
        Ok(())
    }

    pub async fn load_checkpoint(&self, room_id: &str) -> Result<RoomCheckpoint> {
        let connection = self.inner.lock().await;
        let checkpoint = connection
            .query_row(
                "SELECT * FROM room_scan_state WHERE room_id = ?1",
                params![room_id],
                Self::map_checkpoint,
            )
            .optional()?;

        if let Some(checkpoint) = checkpoint {
            return Ok(checkpoint);
        }

        connection.execute(
            "INSERT INTO room_scan_state (
                room_id, historical_message_count, initial_backfill_complete, last_history_mode
             ) VALUES (?1, 0, 0, ?2)",
            params![room_id, RoomHistoryMode::Idle.as_storage_key()],
        )?;

        Ok(RoomCheckpoint {
            room_id: room_id.to_owned(),
            last_processed_event_id: None,
            last_processed_timestamp: None,
            oldest_backfilled_event_id: None,
            oldest_backfilled_timestamp: None,
            historical_message_count: 0,
            initial_backfill_complete: false,
            last_history_mode: RoomHistoryMode::Idle,
            last_history_run_at: None,
        })
    }

    pub async fn save_checkpoint(&self, checkpoint: &RoomCheckpoint) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "INSERT INTO room_scan_state (
                room_id, last_processed_event_id, last_processed_ts,
                oldest_backfilled_event_id, oldest_backfilled_ts,
                historical_message_count, initial_backfill_complete,
                last_history_mode, last_history_run_at
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
             ON CONFLICT(room_id) DO UPDATE SET
                last_processed_event_id = excluded.last_processed_event_id,
                last_processed_ts = excluded.last_processed_ts,
                oldest_backfilled_event_id = excluded.oldest_backfilled_event_id,
                oldest_backfilled_ts = excluded.oldest_backfilled_ts,
                historical_message_count = excluded.historical_message_count,
                initial_backfill_complete = excluded.initial_backfill_complete,
                last_history_mode = excluded.last_history_mode,
                last_history_run_at = excluded.last_history_run_at",
            params![
                checkpoint.room_id,
                checkpoint.last_processed_event_id,
                checkpoint.last_processed_timestamp.as_ref().map(iso_string),
                checkpoint.oldest_backfilled_event_id,
                checkpoint
                    .oldest_backfilled_timestamp
                    .as_ref()
                    .map(iso_string),
                checkpoint.historical_message_count,
                if checkpoint.initial_backfill_complete {
                    1
                } else {
                    0
                },
                checkpoint.last_history_mode.as_storage_key(),
                checkpoint.last_history_run_at.as_ref().map(iso_string),
            ],
        )?;
        Ok(())
    }

    pub async fn reset_all_history_scans_for_full_rescan(&self) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE room_scan_state
             SET
                last_processed_event_id = NULL,
                last_processed_ts = NULL,
                oldest_backfilled_event_id = NULL,
                oldest_backfilled_ts = NULL,
                historical_message_count = 0,
                initial_backfill_complete = 0,
                last_history_mode = ?1,
                last_history_run_at = NULL",
            params![RoomHistoryMode::Idle.as_storage_key()],
        )?;
        connection.execute("DELETE FROM discovered_attachments", [])?;
        connection.execute("DELETE FROM download_jobs", [])?;
        Ok(())
    }

    pub async fn enqueue_discovery(
        &self,
        discovery: &AttachmentDiscovery,
        queue_download: bool,
    ) -> Result<bool> {
        let connection = self.inner.lock().await;
        let existed = connection
            .query_row(
                "SELECT 1 FROM discovered_attachments WHERE room_id = ?1 AND event_id = ?2",
                params![discovery.room_id, discovery.event_id],
                |_| Ok(()),
            )
            .optional()?
            .is_some();
        connection.execute(
            "INSERT OR IGNORE INTO discovered_attachments (
                room_id, event_id, origin_ts, source_kind, direct_url, mxc_url, fallback_source_url, thumbnail_source_url, thumbnail_cached_path, original_filename, mime_type, category
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
            params![
                discovery.room_id,
                discovery.event_id,
                iso_string(&discovery.origin_server_timestamp),
                discovery.source_kind.as_storage_key(),
                discovery.direct_url,
                discovery.mxc_url,
                discovery.fallback_source_url,
                discovery.thumbnail_source_url,
                discovery.thumbnail_cached_path,
                discovery.original_filename,
                discovery.mime_type,
                discovery.category.as_storage_key(),
            ],
        )?;
        connection.execute(
            "UPDATE discovered_attachments
             SET source_kind = ?3,
                 direct_url = COALESCE(?4, direct_url),
                 mxc_url = ?5,
                 fallback_source_url = COALESCE(?6, fallback_source_url),
                 thumbnail_source_url = COALESCE(?7, thumbnail_source_url),
                 thumbnail_cached_path = COALESCE(?8, thumbnail_cached_path),
                 original_filename = COALESCE(?9, original_filename),
                 mime_type = COALESCE(?10, mime_type),
                 category = ?11
             WHERE room_id = ?1 AND event_id = ?2",
            params![
                discovery.room_id,
                discovery.event_id,
                discovery.source_kind.as_storage_key(),
                discovery.direct_url,
                discovery.mxc_url,
                discovery.fallback_source_url,
                discovery.thumbnail_source_url,
                discovery.thumbnail_cached_path,
                discovery.original_filename,
                discovery.mime_type,
                discovery.category.as_storage_key(),
            ],
        )?;

        if existed {
            return Ok(false);
        }

        if !queue_download {
            return Ok(true);
        }

        let now = iso_now();
        connection.execute(
            "INSERT OR IGNORE INTO download_jobs (
                room_id, event_id, mxc_url, fallback_source_url, source_kind, direct_url, original_filename, mime_type, category,
                state, retry_count, created_at, updated_at
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0, ?11, ?12)",
            params![
                discovery.room_id,
                discovery.event_id,
                discovery.mxc_url,
                discovery.fallback_source_url,
                discovery.source_kind.as_storage_key(),
                discovery.direct_url,
                discovery.original_filename,
                discovery.mime_type,
                discovery.category.as_storage_key(),
                DownloadJobState::Queued.as_storage_key(),
                now,
                now,
            ],
        )?;
        Ok(true)
    }

    pub async fn enqueue_direct_download(
        &self,
        room_id: &str,
        event_id: &str,
        source_kind: crate::domain::MediaSourceKind,
        direct_url: &str,
        original_filename: Option<&str>,
        mime_type: Option<&str>,
        category: MediaCategory,
    ) -> Result<bool> {
        let connection = self.inner.lock().await;
        let now = iso_now();
        connection.execute(
            "INSERT OR IGNORE INTO download_jobs (
                room_id, event_id, mxc_url, fallback_source_url, source_kind, direct_url, original_filename, mime_type, category,
                state, retry_count, created_at, updated_at
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0, ?11, ?12)",
            params![
                room_id,
                event_id,
                direct_url,
                Option::<String>::None,
                source_kind.as_storage_key(),
                direct_url,
                original_filename,
                mime_type,
                category.as_storage_key(),
                DownloadJobState::Queued.as_storage_key(),
                now,
                now,
            ],
        )?;
        Ok(connection.changes() > 0)
    }

    pub async fn discovery_record(
        &self,
        room_id: &str,
        event_id: &str,
    ) -> Result<Option<AttachmentDiscovery>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT room_id, event_id, origin_ts, source_kind, direct_url, mxc_url, fallback_source_url, thumbnail_source_url, thumbnail_cached_path, original_filename, mime_type, category
                 FROM discovered_attachments
                 WHERE room_id = ?1 AND event_id = ?2",
                params![room_id, event_id],
                Self::map_discovery_record,
            )
            .optional()
            .context("Failed to load discovery record")
    }

    pub async fn set_discovery_thumbnail_cached_path(
        &self,
        room_id: &str,
        event_id: &str,
        thumbnail_cached_path: Option<&str>,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE discovered_attachments
             SET thumbnail_cached_path = ?3
             WHERE room_id = ?1 AND event_id = ?2",
            params![room_id, event_id, thumbnail_cached_path],
        )?;
        Ok(())
    }

    pub async fn set_discovery_thumbnail_source_url(
        &self,
        room_id: &str,
        event_id: &str,
        thumbnail_source_url: Option<&str>,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE discovered_attachments
             SET thumbnail_source_url = ?3
             WHERE room_id = ?1 AND event_id = ?2",
            params![room_id, event_id, thumbnail_source_url],
        )?;
        Ok(())
    }

    pub async fn fetch_room_discoveries_missing_thumbnails(
        &self,
        room_id: &str,
        limit: i64,
    ) -> Result<Vec<AttachmentDiscovery>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT room_id, event_id, origin_ts, source_kind, direct_url, mxc_url, fallback_source_url, thumbnail_source_url, thumbnail_cached_path, original_filename, mime_type, category
             FROM discovered_attachments
             WHERE room_id = ?1
               AND (thumbnail_cached_path IS NULL OR TRIM(thumbnail_cached_path) = '')
             ORDER BY origin_ts DESC, id DESC
             LIMIT ?2",
        )?;
        let rows = statement.query_map(params![room_id, limit], Self::map_discovery_record)?;
        collect_rows(rows)
    }

    pub async fn prune_discovery_cache(&self, max_entries: usize) -> Result<Vec<String>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT id, thumbnail_cached_path
             FROM discovered_attachments
             ORDER BY origin_ts DESC, id DESC
             LIMIT -1 OFFSET ?1",
        )?;
        let stale_rows = statement
            .query_map(params![max_entries as i64], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, Option<String>>(1)?))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        drop(statement);

        if stale_rows.is_empty() {
            return Ok(Vec::new());
        }

        let stale_ids = stale_rows.iter().map(|(id, _)| *id).collect::<Vec<_>>();
        let thumbnail_paths = stale_rows
            .into_iter()
            .filter_map(|(_, path)| path)
            .collect::<Vec<_>>();

        let placeholders = std::iter::repeat("?")
            .take(stale_ids.len())
            .collect::<Vec<_>>()
            .join(",");
        let sql = format!("DELETE FROM discovered_attachments WHERE id IN ({placeholders})");
        connection.execute(&sql, rusqlite::params_from_iter(stale_ids.iter()))?;
        Ok(thumbnail_paths)
    }

    pub async fn mark_job_queued(&self, id: i64, last_error: Option<&str>) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, last_error = ?2, next_eligible_at = NULL, received_bytes = 0, total_bytes = NULL, updated_at = ?3
             WHERE id = ?4",
            params![
                DownloadJobState::Queued.as_storage_key(),
                last_error,
                iso_now(),
                id
            ],
        )?;
        Ok(())
    }

    pub async fn retry_permanent_failed_job(&self, id: i64) -> Result<bool> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, retry_count = 0, next_eligible_at = NULL, last_failure_at = NULL, last_error = NULL, received_bytes = 0, total_bytes = NULL, updated_at = ?2
             WHERE id = ?3 AND state = ?4",
            params![
                DownloadJobState::Queued.as_storage_key(),
                iso_now(),
                id,
                DownloadJobState::FailedPermanent.as_storage_key(),
            ],
        )?;
        Ok(connection.changes() > 0)
    }

    pub async fn retry_all_permanent_failed_jobs(&self) -> Result<usize> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, retry_count = 0, next_eligible_at = NULL, last_failure_at = NULL, last_error = NULL, received_bytes = 0, total_bytes = NULL, updated_at = ?2
             WHERE state = ?3",
            params![
                DownloadJobState::Queued.as_storage_key(),
                iso_now(),
                DownloadJobState::FailedPermanent.as_storage_key(),
            ],
        )?;
        Ok(connection.changes() as usize)
    }

    pub async fn clear_permanent_failed_job(&self, id: i64) -> Result<bool> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM download_jobs WHERE id = ?1 AND state = ?2",
            params![id, DownloadJobState::FailedPermanent.as_storage_key()],
        )?;
        Ok(connection.changes() > 0)
    }

    pub async fn clear_all_permanent_failed_jobs(&self) -> Result<usize> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM download_jobs WHERE state = ?1",
            params![DownloadJobState::FailedPermanent.as_storage_key()],
        )?;
        Ok(connection.changes() as usize)
    }

    pub async fn prune_permanent_failed_jobs(&self, older_than: DateTime<Utc>) -> Result<usize> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM download_jobs
             WHERE state = ?1
               AND COALESCE(last_failure_at, updated_at, created_at) <= ?2",
            params![
                DownloadJobState::FailedPermanent.as_storage_key(),
                iso_string(&older_than)
            ],
        )?;
        Ok(connection.changes() as usize)
    }

    pub async fn reset_interrupted_jobs(&self) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, updated_at = ?2, last_error = COALESCE(last_error, 'Interrupted download was reset on launch')
             WHERE state = ?3",
            params![
                DownloadJobState::Queued.as_storage_key(),
                iso_now(),
                DownloadJobState::Downloading.as_storage_key(),
            ],
        )?;
        Ok(())
    }

    pub async fn claim_next_eligible_job(
        &self,
        now: DateTime<Utc>,
    ) -> Result<Option<DownloadJobRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT * FROM download_jobs
             WHERE
                state = ?1
                OR (state = ?2 AND (next_eligible_at IS NULL OR next_eligible_at <= ?4))
                OR (state = ?3 AND (next_eligible_at IS NULL OR next_eligible_at <= ?4))
             ORDER BY COALESCE(last_failure_at, created_at) ASC, id ASC
             LIMIT 1",
        )?;
        let job = statement
            .query_row(
                params![
                    DownloadJobState::Queued.as_storage_key(),
                    DownloadJobState::CoolingDown.as_storage_key(),
                    DownloadJobState::UndecryptablePending.as_storage_key(),
                    iso_string(&now),
                ],
                Self::map_job,
            )
            .optional()?;
        drop(statement);

        let Some(mut job) = job else {
            return Ok(None);
        };

        let updated_at = Utc::now();
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, last_error = NULL, next_eligible_at = NULL, received_bytes = 0, total_bytes = NULL, updated_at = ?2
             WHERE id = ?3",
            params![
                DownloadJobState::Downloading.as_storage_key(),
                iso_string(&updated_at),
                job.id
            ],
        )?;

        job.state = DownloadJobState::Downloading;
        job.last_error = None;
        job.next_eligible_at = None;
        job.received_bytes = 0;
        job.total_bytes = None;
        job.updated_at = updated_at;
        Ok(Some(job))
    }

    pub async fn mark_job_completed(
        &self,
        id: i64,
        sha256: &str,
        saved_relative_path: &str,
        final_size: i64,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, sha256 = ?2, saved_relative_path = ?3, received_bytes = ?4, total_bytes = ?5, updated_at = ?6
             WHERE id = ?7",
            params![
                DownloadJobState::Completed.as_storage_key(),
                sha256,
                saved_relative_path,
                final_size,
                final_size,
                iso_now(),
                id,
            ],
        )?;
        Ok(())
    }

    pub async fn mark_job_duplicate(
        &self,
        id: i64,
        sha256: &str,
        saved_relative_path: Option<&str>,
        final_size: i64,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, sha256 = ?2, saved_relative_path = ?3, received_bytes = ?4, total_bytes = ?5, updated_at = ?6
             WHERE id = ?7",
            params![
                DownloadJobState::DuplicateCompleted.as_storage_key(),
                sha256,
                saved_relative_path,
                final_size,
                final_size,
                iso_now(),
                id,
            ],
        )?;
        Ok(())
    }

    pub async fn update_job_progress(
        &self,
        id: i64,
        received_bytes: i64,
        total_bytes: Option<i64>,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET received_bytes = ?1, total_bytes = ?2, updated_at = ?3
             WHERE id = ?4",
            params![received_bytes, total_bytes, iso_now(), id],
        )?;
        Ok(())
    }

    pub async fn mark_job_cooling_down(
        &self,
        id: i64,
        retry_count: i32,
        next_eligible_at: DateTime<Utc>,
        error: &str,
        permanently_failed: bool,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, retry_count = ?2, next_eligible_at = ?3, last_failure_at = ?4, last_error = ?5, received_bytes = 0, total_bytes = NULL, updated_at = ?6
             WHERE id = ?7",
            params![
                if permanently_failed {
                    DownloadJobState::FailedPermanent.as_storage_key()
                } else {
                    DownloadJobState::CoolingDown.as_storage_key()
                },
                retry_count,
                iso_string(&next_eligible_at),
                iso_now(),
                error,
                iso_now(),
                id,
            ],
        )?;
        Ok(())
    }

    pub async fn mark_job_undecryptable(
        &self,
        id: i64,
        next_eligible_at: DateTime<Utc>,
        error: &str,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        let failed_at = Utc::now();
        connection.execute(
            "UPDATE download_jobs
             SET state = ?1, next_eligible_at = ?2, last_failure_at = ?3, last_error = ?4, received_bytes = 0, total_bytes = NULL, updated_at = ?5
             WHERE id = ?6",
            params![
                DownloadJobState::UndecryptablePending.as_storage_key(),
                iso_string(&next_eligible_at),
                iso_string(&failed_at),
                error,
                iso_string(&failed_at),
                id,
            ],
        )?;
        Ok(())
    }

    pub async fn find_completed_job(
        &self,
        room_id: &str,
        category: MediaCategory,
        sha256: &str,
    ) -> Result<Option<DownloadJobRecord>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT * FROM download_jobs
                 WHERE room_id = ?1 AND category = ?2 AND sha256 = ?3 AND state IN (?4, ?5)
                 LIMIT 1",
                params![
                    room_id,
                    category.as_storage_key(),
                    sha256,
                    DownloadJobState::Completed.as_storage_key(),
                    DownloadJobState::DuplicateCompleted.as_storage_key(),
                ],
                Self::map_job,
            )
            .optional()
            .context("Failed to query completed job")
    }

    pub async fn discovery_origin_timestamp(
        &self,
        room_id: &str,
        event_id: &str,
    ) -> Result<Option<DateTime<Utc>>> {
        let connection = self.inner.lock().await;
        let value: Option<String> = connection
            .query_row(
                "SELECT origin_ts FROM discovered_attachments WHERE room_id = ?1 AND event_id = ?2 LIMIT 1",
                params![room_id, event_id],
                |row| row.get(0),
            )
            .optional()?
            .flatten();
        parse_optional_datetime(value)
    }

    pub async fn fetch_jobs(
        &self,
        limit: i64,
        now: DateTime<Utc>,
    ) -> Result<Vec<DownloadJobRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT * FROM download_jobs
             WHERE state IN (?1, ?2, ?3, ?4)
             ORDER BY
                CASE state
                    WHEN ?5 THEN 0
                    WHEN ?6 THEN
                        CASE
                            WHEN next_eligible_at IS NULL OR next_eligible_at <= ?8 THEN 0
                            ELSE 1
                        END
                    WHEN ?7 THEN
                        CASE
                            WHEN next_eligible_at IS NULL OR next_eligible_at <= ?8 THEN 0
                            ELSE 1
                        END
                    WHEN ?9 THEN 2
                    ELSE 3
                END,
                COALESCE(last_failure_at, created_at) ASC,
                id ASC
             LIMIT ?10",
        )?;
        let rows = statement.query_map(
            params![
                DownloadJobState::Queued.as_storage_key(),
                DownloadJobState::CoolingDown.as_storage_key(),
                DownloadJobState::UndecryptablePending.as_storage_key(),
                DownloadJobState::FailedPermanent.as_storage_key(),
                DownloadJobState::Queued.as_storage_key(),
                DownloadJobState::CoolingDown.as_storage_key(),
                DownloadJobState::UndecryptablePending.as_storage_key(),
                iso_string(&now),
                DownloadJobState::FailedPermanent.as_storage_key(),
                limit,
            ],
            Self::map_job,
        )?;
        collect_rows(rows)
    }

    pub async fn fetch_waiting_job_count(&self) -> Result<i64> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT COUNT(*) FROM download_jobs WHERE state IN (?1, ?2, ?3)",
                params![
                    DownloadJobState::Queued.as_storage_key(),
                    DownloadJobState::CoolingDown.as_storage_key(),
                    DownloadJobState::UndecryptablePending.as_storage_key(),
                ],
                |row| row.get(0),
            )
            .context("Failed to count waiting jobs")
    }

    pub async fn upsert_archive_file(
        &self,
        sha256: &str,
        file_path: &str,
        file_size: i64,
        modified_at: Option<DateTime<Utc>>,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "INSERT INTO archive_files (
                sha256, file_path, file_size, modified_at, last_seen_at
             ) VALUES (?1, ?2, ?3, ?4, ?5)
             ON CONFLICT(sha256) DO UPDATE SET
                file_path = excluded.file_path,
                file_size = excluded.file_size,
                modified_at = excluded.modified_at,
                last_seen_at = excluded.last_seen_at",
            params![
                sha256,
                file_path,
                file_size,
                modified_at.map(|value| value.to_rfc3339()),
                iso_now(),
            ],
        )?;
        Ok(())
    }

    pub async fn find_archive_file_by_sha256(
        &self,
        sha256: &str,
    ) -> Result<Option<ArchiveFileRecord>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT sha256, file_path, file_size, modified_at, last_seen_at
                 FROM archive_files WHERE sha256 = ?1",
                params![sha256],
                Self::map_archive_file_record,
            )
            .optional()
            .context("Failed to load archive file record")
    }

    pub async fn upsert_tracked_upload(&self, record: &TrackedUploadRecord) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "INSERT INTO tracked_uploads (
                sha256, source_kind, source_path, bundle_path, library_path, archive_path,
                file_cid, thumbnail_cid, page_cid, landing_page_url,
                room_id, category, original_filename, mime_type, file_size,
                created_at, updated_at
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)
             ON CONFLICT(sha256) DO UPDATE SET
                source_kind = excluded.source_kind,
                source_path = excluded.source_path,
                bundle_path = excluded.bundle_path,
                library_path = excluded.library_path,
                archive_path = excluded.archive_path,
                file_cid = excluded.file_cid,
                thumbnail_cid = excluded.thumbnail_cid,
                page_cid = excluded.page_cid,
                landing_page_url = excluded.landing_page_url,
                room_id = excluded.room_id,
                category = excluded.category,
                original_filename = excluded.original_filename,
                mime_type = excluded.mime_type,
                file_size = excluded.file_size,
                updated_at = excluded.updated_at",
            params![
                record.sha256,
                record.source_kind.as_storage_key(),
                record.source_path,
                record.bundle_path,
                record.library_path,
                record.archive_path,
                record.file_cid,
                record.thumbnail_cid,
                record.page_cid,
                record.landing_page_url,
                record.room_id,
                record.category.as_storage_key(),
                record.original_filename,
                record.mime_type,
                record.file_size,
                record.created_at.to_rfc3339(),
                record.updated_at.to_rfc3339(),
            ],
        )?;
        Ok(())
    }

    pub async fn find_tracked_upload_by_sha256(
        &self,
        sha256: &str,
    ) -> Result<Option<TrackedUploadRecord>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT *
                 FROM tracked_uploads WHERE sha256 = ?1",
                params![sha256],
                Self::map_tracked_upload_record,
            )
            .optional()
            .context("Failed to load tracked upload record")
    }

    pub async fn fetch_tracked_uploads(&self) -> Result<Vec<TrackedUploadRecord>> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT *
             FROM tracked_uploads
             ORDER BY updated_at DESC, sha256 ASC",
        )?;
        let rows = statement.query_map([], Self::map_tracked_upload_record)?;
        collect_rows(rows)
    }

    pub async fn remove_tracked_upload(&self, sha256: &str) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM tracked_uploads WHERE sha256 = ?1",
            params![sha256],
        )?;
        Ok(())
    }

    pub async fn remove_file_hash_cache_entry(&self, file_path: &str) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "DELETE FROM file_hash_cache WHERE file_path = ?1",
            params![file_path],
        )?;
        Ok(())
    }

    pub async fn upsert_file_hash_cache(
        &self,
        file_path: &str,
        root_kind: &str,
        sha256: &str,
        file_size: i64,
        modified_at: Option<DateTime<Utc>>,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute(
            "INSERT INTO file_hash_cache (
                file_path, root_kind, sha256, file_size, modified_at, last_verified_at
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)
             ON CONFLICT(file_path) DO UPDATE SET
                root_kind = excluded.root_kind,
                sha256 = excluded.sha256,
                file_size = excluded.file_size,
                modified_at = excluded.modified_at,
                last_verified_at = excluded.last_verified_at",
            params![
                file_path,
                root_kind,
                sha256,
                file_size,
                modified_at.map(|value| value.to_rfc3339()),
                iso_now(),
            ],
        )?;
        Ok(())
    }

    pub async fn file_hash_cache_record(
        &self,
        file_path: &str,
    ) -> Result<Option<FileHashCacheRecord>> {
        let connection = self.inner.lock().await;
        connection
            .query_row(
                "SELECT file_path, root_kind, sha256, file_size, modified_at, last_verified_at
                 FROM file_hash_cache WHERE file_path = ?1",
                params![file_path],
                Self::map_file_hash_cache_record,
            )
            .optional()
            .context("Failed to load file hash cache record")
    }

    pub async fn prune_file_hash_cache_for_root_kind(
        &self,
        root_kind: &str,
        valid_paths: &HashSet<String>,
    ) -> Result<usize> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT file_path FROM file_hash_cache WHERE root_kind = ?1",
        )?;
        let existing_paths = statement
            .query_map(params![root_kind], |row| row.get::<_, String>(0))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        drop(statement);

        let mut removed = 0usize;
        for file_path in existing_paths {
            if valid_paths.contains(&file_path) {
                continue;
            }
            connection.execute(
                "DELETE FROM file_hash_cache WHERE file_path = ?1",
                params![file_path],
            )?;
            removed += 1;
        }
        Ok(removed)
    }

    pub async fn prune_archive_files_to_paths(
        &self,
        valid_paths: &HashSet<String>,
    ) -> Result<usize> {
        let connection = self.inner.lock().await;
        let mut statement = connection.prepare(
            "SELECT sha256, file_path FROM archive_files",
        )?;
        let existing = statement
            .query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        drop(statement);

        let mut removed = 0usize;
        for (sha256, file_path) in existing {
            if valid_paths.contains(&file_path) {
                continue;
            }
            connection.execute(
                "DELETE FROM archive_files WHERE sha256 = ?1",
                params![sha256],
            )?;
            removed += 1;
        }
        Ok(removed)
    }

    pub async fn fetch_recent_logs(&self, limit: i64) -> Result<Vec<ActivityLogEntry>> {
        let connection = self.inner.lock().await;
        let mut statement =
            connection.prepare("SELECT * FROM activity_log ORDER BY id DESC LIMIT ?1")?;
        let rows = statement.query_map(params![limit], Self::map_log_entry)?;
        let mut collected = collect_rows(rows)?;
        collected.reverse();
        Ok(collected)
    }

    pub async fn insert_log(
        &self,
        level: AppLogLevel,
        subsystem: &str,
        message: &str,
    ) -> Result<()> {
        let connection = self.inner.lock().await;
        let now = Utc::now();
        connection.execute(
            "INSERT INTO activity_log (created_at, level, subsystem, message) VALUES (?1, ?2, ?3, ?4)",
            params![iso_string(&now), level.as_storage_key(), subsystem, message],
        )?;

        let cutoff = now - Duration::days(LOG_RETENTION_DAYS);
        connection.execute(
            "DELETE FROM activity_log WHERE created_at < ?1",
            params![iso_string(&cutoff)],
        )?;
        connection.execute(
            "DELETE FROM activity_log
             WHERE id IN (
                SELECT id FROM activity_log ORDER BY id DESC LIMIT -1 OFFSET ?1
             )",
            params![MAX_RETAINED_LOG_ENTRIES],
        )?;
        Ok(())
    }

    async fn initialize_schema(&self) -> Result<()> {
        let connection = self.inner.lock().await;
        connection.execute_batch(
            "CREATE TABLE IF NOT EXISTS app_settings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                homeserver_url TEXT NOT NULL,
                username TEXT NOT NULL,
                owner_user_id TEXT NOT NULL,
                destination_root_path TEXT NOT NULL,
                library_root_path TEXT NOT NULL DEFAULT '',
                flat_folder_layout INTEGER NOT NULL DEFAULT 0,
                archive_root_path TEXT NOT NULL DEFAULT '',
                archive_scan_enabled INTEGER NOT NULL DEFAULT 0,
                archive_scan_high_priority INTEGER NOT NULL DEFAULT 0,
                manual_download_root_path TEXT NOT NULL DEFAULT '',
                message_limit INTEGER NOT NULL,
                time_window_value INTEGER NOT NULL,
                time_window_unit TEXT NOT NULL,
                retry_cooldown_minutes INTEGER NOT NULL,
                retry_limit INTEGER NOT NULL,
                download_worker_count INTEGER NOT NULL DEFAULT 1,
                failed_job_retention_value INTEGER NOT NULL DEFAULT 0,
                failed_job_retention_unit TEXT NOT NULL DEFAULT 'none',
                primary_gateway_url TEXT NOT NULL DEFAULT 'https://dweb.link',
                preferred_gateway_urls TEXT NOT NULL DEFAULT '[]',
                autostart_enabled INTEGER NOT NULL DEFAULT 0,
                minimize_to_tray INTEGER NOT NULL DEFAULT 1,
                start_hidden INTEGER NOT NULL DEFAULT 0,
                dark_mode_enabled INTEGER NOT NULL DEFAULT 0,
                bandwidth_limit_kib_per_sec INTEGER NOT NULL DEFAULT 0,
                preview_worker_count INTEGER NOT NULL DEFAULT 1,
                auto_join_space_rooms INTEGER NOT NULL DEFAULT 0,
                auto_download_new_media INTEGER NOT NULL DEFAULT 0,
                self_heal_enabled INTEGER NOT NULL DEFAULT 0,
                desired_power_state INTEGER NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS rooms (
                room_id TEXT PRIMARY KEY,
                display_name TEXT,
                canonical_alias TEXT,
                active_folder_label TEXT NOT NULL,
                is_space INTEGER NOT NULL DEFAULT 0,
                membership TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS room_alias_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                room_id TEXT NOT NULL,
                alias TEXT NOT NULL,
                seen_at TEXT NOT NULL,
                UNIQUE(room_id, alias)
            );
            CREATE TABLE IF NOT EXISTS room_scan_state (
                room_id TEXT PRIMARY KEY,
                last_processed_event_id TEXT,
                last_processed_ts TEXT,
                oldest_backfilled_event_id TEXT,
                oldest_backfilled_ts TEXT,
                historical_message_count INTEGER NOT NULL DEFAULT 0,
                initial_backfill_complete INTEGER NOT NULL DEFAULT 0,
                last_history_mode TEXT NOT NULL DEFAULT 'idle',
                last_history_run_at TEXT
            );
            CREATE TABLE IF NOT EXISTS discovered_attachments (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                room_id TEXT NOT NULL,
                event_id TEXT NOT NULL,
                origin_ts TEXT NOT NULL,
                source_kind TEXT NOT NULL DEFAULT 'matrix',
                direct_url TEXT,
                mxc_url TEXT NOT NULL,
                fallback_source_url TEXT,
                thumbnail_source_url TEXT,
                thumbnail_cached_path TEXT,
                original_filename TEXT,
                mime_type TEXT,
                category TEXT NOT NULL,
                UNIQUE(room_id, event_id)
            );
            CREATE TABLE IF NOT EXISTS download_jobs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                media_item_id INTEGER,
                room_id TEXT NOT NULL,
                event_id TEXT NOT NULL,
                mxc_url TEXT NOT NULL,
                fallback_source_url TEXT,
                source_kind TEXT NOT NULL DEFAULT 'matrix',
                direct_url TEXT,
                original_filename TEXT,
                mime_type TEXT,
                category TEXT NOT NULL,
                state TEXT NOT NULL,
                retry_count INTEGER NOT NULL DEFAULT 0,
                next_eligible_at TEXT,
                last_failure_at TEXT,
                received_bytes INTEGER NOT NULL DEFAULT 0,
                total_bytes INTEGER,
                last_error TEXT,
                sha256 TEXT,
                saved_relative_path TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                UNIQUE(room_id, event_id)
            );
            CREATE TABLE IF NOT EXISTS activity_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at TEXT NOT NULL,
                level TEXT NOT NULL,
                subsystem TEXT NOT NULL,
                message TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS archive_files (
                sha256 TEXT PRIMARY KEY,
                file_path TEXT NOT NULL,
                file_size INTEGER NOT NULL DEFAULT 0,
                modified_at TEXT,
                last_seen_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS tracked_uploads (
                sha256 TEXT PRIMARY KEY,
                source_kind TEXT NOT NULL DEFAULT 'library',
                source_path TEXT NOT NULL,
                bundle_path TEXT,
                library_path TEXT,
                archive_path TEXT,
                file_cid TEXT,
                thumbnail_cid TEXT,
                page_cid TEXT,
                landing_page_url TEXT,
                room_id TEXT NOT NULL,
                category TEXT NOT NULL,
                original_filename TEXT,
                mime_type TEXT,
                file_size INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS file_hash_cache (
                file_path TEXT PRIMARY KEY,
                root_kind TEXT NOT NULL,
                sha256 TEXT NOT NULL,
                file_size INTEGER NOT NULL DEFAULT 0,
                modified_at TEXT,
                last_verified_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS space_auto_joins (
                space_room_id TEXT NOT NULL,
                child_room_id TEXT NOT NULL,
                auto_joined_by_bot INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                PRIMARY KEY(space_room_id, child_room_id)
            );",
        )?;
        Self::ensure_column(
            &connection,
            "discovered_attachments",
            "source_kind",
            "TEXT NOT NULL DEFAULT 'matrix'",
        )?;
        Self::ensure_column(
            &connection,
            "discovered_attachments",
            "direct_url",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "discovered_attachments",
            "fallback_source_url",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "discovered_attachments",
            "thumbnail_source_url",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "discovered_attachments",
            "thumbnail_cached_path",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "download_jobs",
            "fallback_source_url",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "download_jobs",
            "received_bytes",
            "INTEGER NOT NULL DEFAULT 0",
        )?;
        Self::ensure_column(
            &connection,
            "download_jobs",
            "total_bytes",
            "INTEGER",
        )?;
        Self::ensure_column(
            &connection,
            "app_settings",
            "self_heal_enabled",
            "INTEGER NOT NULL DEFAULT 0",
        )?;
        Self::ensure_column(
            &connection,
            "tracked_uploads",
            "bundle_path",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "tracked_uploads",
            "file_cid",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "tracked_uploads",
            "thumbnail_cid",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "tracked_uploads",
            "page_cid",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "tracked_uploads",
            "landing_page_url",
            "TEXT",
        )?;
        Self::ensure_column(
            &connection,
            "app_settings",
            "flat_folder_layout",
            "INTEGER NOT NULL DEFAULT 0",
        )?;
        if !column_exists(&connection, "app_settings", "dark_mode_enabled")? {
            connection.execute(
                "ALTER TABLE app_settings ADD COLUMN dark_mode_enabled INTEGER NOT NULL DEFAULT 0",
                [],
            )?;
        }
        Ok(())
    }

    fn ensure_column(
        connection: &Connection,
        table: &str,
        column: &str,
        definition: &str,
    ) -> Result<()> {
        let pragma = format!("PRAGMA table_info({table})");
        let mut statement = connection.prepare(&pragma)?;
        let existing_columns = statement
            .query_map([], |row| row.get::<_, String>(1))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        drop(statement);

        if existing_columns.iter().any(|existing| existing == column) {
            return Ok(());
        }

        let sql = format!("ALTER TABLE {table} ADD COLUMN {column} {definition}");
        connection.execute(&sql, [])?;
        Ok(())
    }

    fn harden_permissions(&self) -> Result<()> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;

            for path in [
                self.path.clone(),
                self.path.with_extension("sqlite-wal"),
                self.path.with_extension("sqlite-shm"),
                PathBuf::from(format!("{}-wal", self.path.display())),
                PathBuf::from(format!("{}-shm", self.path.display())),
            ] {
                if path.exists() {
                    fs::set_permissions(&path, fs::Permissions::from_mode(0o600))
                        .with_context(|| format!("Failed to secure {}", path.display()))?;
                }
            }
        }
        Ok(())
    }

    fn map_settings(row: &Row<'_>) -> rusqlite::Result<AppSettings> {
        Ok(AppSettings {
            homeserver_url: row.get("homeserver_url")?,
            username: row.get("username")?,
            owner_user_id: row.get("owner_user_id")?,
            destination_root_path: row.get("destination_root_path")?,
            library_root_path: row.get("library_root_path")?,
            flat_folder_layout: row.get::<_, i64>("flat_folder_layout")? != 0,
            archive_root_path: row.get("archive_root_path")?,
            archive_scan_enabled: row.get::<_, i64>("archive_scan_enabled")? != 0,
            archive_scan_high_priority: row.get::<_, i64>("archive_scan_high_priority")? != 0,
            manual_download_root_path: row.get("manual_download_root_path")?,
            message_limit: row.get("message_limit")?,
            time_window_value: row.get("time_window_value")?,
            time_window_unit: parse_time_window_unit(
                row.get::<_, String>("time_window_unit")?.as_str(),
            ),
            retry_cooldown_minutes: row.get("retry_cooldown_minutes")?,
            retry_limit: row.get("retry_limit")?,
            download_worker_count: clamp_download_worker_count(row.get("download_worker_count")?),
            failed_job_retention_value: row.get("failed_job_retention_value")?,
            failed_job_retention_unit: parse_failed_retention_unit(
                row.get::<_, String>("failed_job_retention_unit")?.as_str(),
            ),
            primary_gateway_url: row.get("primary_gateway_url")?,
            preferred_gateway_urls: serde_json::from_str(
                &row.get::<_, String>("preferred_gateway_urls")?,
            )
            .unwrap_or_default(),
            autostart_enabled: row.get::<_, i64>("autostart_enabled")? != 0,
            minimize_to_tray: row.get::<_, i64>("minimize_to_tray")? != 0,
            start_hidden: row.get::<_, i64>("start_hidden")? != 0,
            dark_mode_enabled: row.get::<_, i64>("dark_mode_enabled")? != 0,
            bandwidth_limit_kib_per_sec: row.get("bandwidth_limit_kib_per_sec")?,
            preview_worker_count: row.get("preview_worker_count")?,
            auto_join_space_rooms: row.get::<_, i64>("auto_join_space_rooms")? != 0,
            auto_download_new_media: row.get::<_, i64>("auto_download_new_media")? != 0,
            self_heal_enabled: row.get::<_, i64>("self_heal_enabled")? != 0,
            desired_power_state: row.get::<_, i64>("desired_power_state")? != 0,
        })
    }

    fn map_archive_file_record(row: &Row<'_>) -> rusqlite::Result<ArchiveFileRecord> {
        Ok(ArchiveFileRecord {
            sha256: row.get("sha256")?,
            file_path: row.get("file_path")?,
            file_size: row.get("file_size")?,
            modified_at: parse_datetime_optional(row.get("modified_at")?)?,
            last_seen_at: parse_datetime_required(row.get("last_seen_at")?)?,
        })
    }

    fn map_tracked_upload_record(row: &Row<'_>) -> rusqlite::Result<TrackedUploadRecord> {
        Ok(TrackedUploadRecord {
            sha256: row.get("sha256")?,
            source_kind: LocalAssetSourceKind::from_storage_key(
                row.get::<_, String>("source_kind")?.as_str(),
            ),
            source_path: row.get("source_path")?,
            bundle_path: row.get("bundle_path")?,
            library_path: row.get("library_path")?,
            archive_path: row.get("archive_path")?,
            file_cid: row.get("file_cid")?,
            thumbnail_cid: row.get("thumbnail_cid")?,
            page_cid: row.get("page_cid")?,
            landing_page_url: row.get("landing_page_url")?,
            room_id: row.get("room_id")?,
            category: MediaCategory::from_storage_key(row.get::<_, String>("category")?.as_str()),
            original_filename: row.get("original_filename")?,
            mime_type: row.get("mime_type")?,
            file_size: row.get("file_size")?,
            created_at: parse_datetime_required(row.get("created_at")?)?,
            updated_at: parse_datetime_required(row.get("updated_at")?)?,
        })
    }

    fn map_file_hash_cache_record(row: &Row<'_>) -> rusqlite::Result<FileHashCacheRecord> {
        Ok(FileHashCacheRecord {
            file_path: row.get("file_path")?,
            root_kind: row.get("root_kind")?,
            sha256: row.get("sha256")?,
            file_size: row.get("file_size")?,
            modified_at: parse_datetime_optional(row.get("modified_at")?)?,
            last_verified_at: parse_datetime_required(row.get("last_verified_at")?)?,
        })
    }

    fn map_room_record(row: &Row<'_>) -> rusqlite::Result<RoomRecord> {
        Ok(RoomRecord {
            room_id: row.get("room_id")?,
            current_display_name: row.get("display_name")?,
            current_canonical_alias: row.get("canonical_alias")?,
            active_folder_label: row.get("active_folder_label")?,
            is_space: row.get::<_, i64>("is_space")? != 0,
            membership: row.get("membership")?,
            updated_at: parse_datetime_required(row.get("updated_at")?)?,
        })
    }

    fn map_checkpoint(row: &Row<'_>) -> rusqlite::Result<RoomCheckpoint> {
        Ok(RoomCheckpoint {
            room_id: row.get("room_id")?,
            last_processed_event_id: row.get("last_processed_event_id")?,
            last_processed_timestamp: parse_datetime_optional_row(row, "last_processed_ts")?,
            oldest_backfilled_event_id: row.get("oldest_backfilled_event_id")?,
            oldest_backfilled_timestamp: parse_datetime_optional_row(row, "oldest_backfilled_ts")?,
            historical_message_count: row.get("historical_message_count")?,
            initial_backfill_complete: row.get::<_, i64>("initial_backfill_complete")? != 0,
            last_history_mode: RoomHistoryMode::from_storage_key(
                &row.get::<_, String>("last_history_mode")?,
            ),
            last_history_run_at: parse_datetime_optional_row(row, "last_history_run_at")?,
        })
    }

    fn map_discovery_record(row: &Row<'_>) -> rusqlite::Result<AttachmentDiscovery> {
        Ok(AttachmentDiscovery {
            room_id: row.get("room_id")?,
            event_id: row.get("event_id")?,
            origin_server_timestamp: parse_datetime_required(row.get("origin_ts")?)?,
            source_kind: crate::domain::MediaSourceKind::from_storage_key(
                &row.get::<_, String>("source_kind")?,
            ),
            direct_url: row.get("direct_url")?,
            mxc_url: row.get("mxc_url")?,
            fallback_source_url: row.get("fallback_source_url")?,
            thumbnail_source_url: row.get("thumbnail_source_url")?,
            thumbnail_cached_path: row.get("thumbnail_cached_path")?,
            original_filename: row.get("original_filename")?,
            mime_type: row.get("mime_type")?,
            category: MediaCategory::from_storage_key(&row.get::<_, String>("category")?),
        })
    }

    fn map_job(row: &Row<'_>) -> rusqlite::Result<DownloadJobRecord> {
        Ok(DownloadJobRecord {
            id: row.get("id")?,
            media_item_id: row.get("media_item_id")?,
            room_id: row.get("room_id")?,
            event_id: row.get("event_id")?,
            mxc_url: row.get("mxc_url")?,
            fallback_source_url: row.get("fallback_source_url")?,
            source_kind: crate::domain::MediaSourceKind::from_storage_key(
                &row.get::<_, String>("source_kind")?,
            ),
            direct_url: row.get("direct_url")?,
            original_filename: row.get("original_filename")?,
            mime_type: row.get("mime_type")?,
            category: MediaCategory::from_storage_key(&row.get::<_, String>("category")?),
            state: DownloadJobState::from_storage_key(&row.get::<_, String>("state")?),
            retry_count: row.get("retry_count")?,
            next_eligible_at: parse_datetime_optional_row(row, "next_eligible_at")?,
            last_failure_at: parse_datetime_optional_row(row, "last_failure_at")?,
            received_bytes: row.get("received_bytes")?,
            total_bytes: row.get("total_bytes")?,
            last_error: row.get("last_error")?,
            sha256: row.get("sha256")?,
            saved_relative_path: row.get("saved_relative_path")?,
            created_at: parse_datetime_required(row.get("created_at")?)?,
            updated_at: parse_datetime_required(row.get("updated_at")?)?,
        })
    }

    fn map_log_entry(row: &Row<'_>) -> rusqlite::Result<ActivityLogEntry> {
        Ok(ActivityLogEntry {
            id: row.get("id")?,
            created_at: parse_datetime_required(row.get("created_at")?)?,
            level: AppLogLevel::from_storage_key(&row.get::<_, String>("level")?),
            subsystem: row.get("subsystem")?,
            message: row.get("message")?,
        })
    }

    fn map_space_auto_join(row: &Row<'_>) -> rusqlite::Result<SpaceAutoJoinRecord> {
        Ok(SpaceAutoJoinRecord {
            space_room_id: row.get("space_room_id")?,
            child_room_id: row.get("child_room_id")?,
            auto_joined_by_bot: row.get::<_, i64>("auto_joined_by_bot")? != 0,
            created_at: parse_datetime_required(row.get("created_at")?)?,
            updated_at: parse_datetime_required(row.get("updated_at")?)?,
        })
    }
}

fn collect_rows<T>(
    rows: rusqlite::MappedRows<'_, impl FnMut(&Row<'_>) -> rusqlite::Result<T>>,
) -> Result<Vec<T>> {
    let mut collected = Vec::new();
    for row in rows {
        collected.push(row?);
    }
    Ok(collected)
}

fn column_exists(connection: &Connection, table_name: &str, column_name: &str) -> Result<bool> {
    let mut statement =
        connection.prepare(&format!("PRAGMA table_info({table_name})"))?;
    let rows = statement.query_map([], |row| row.get::<_, String>(1))?;
    for column in rows {
        if column? == column_name {
            return Ok(true);
        }
    }
    Ok(false)
}

fn parse_datetime_required(value: String) -> rusqlite::Result<DateTime<Utc>> {
    DateTime::parse_from_rfc3339(&value)
        .map(|value| value.with_timezone(&Utc))
        .map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(
                0,
                rusqlite::types::Type::Text,
                Box::new(error),
            )
        })
}

fn parse_datetime_optional(value: Option<String>) -> rusqlite::Result<Option<DateTime<Utc>>> {
    match value {
        Some(value) => Ok(Some(parse_datetime_required(value)?)),
        None => Ok(None),
    }
}

fn parse_datetime_optional_row(
    row: &Row<'_>,
    column: &str,
) -> rusqlite::Result<Option<DateTime<Utc>>> {
    parse_datetime_optional(row.get(column)?)
}

fn parse_optional_datetime(value: Option<String>) -> Result<Option<DateTime<Utc>>> {
    match value {
        Some(value) => Ok(Some(
            DateTime::parse_from_rfc3339(&value)
                .with_context(|| format!("Invalid timestamp: {value}"))?
                .with_timezone(&Utc),
        )),
        None => Ok(None),
    }
}

fn normalize_settings(mut settings: AppSettings) -> AppSettings {
    let destination = if settings.destination_root_path.trim().is_empty() {
        settings.manual_download_root_path.trim().to_owned()
    } else {
        settings.destination_root_path.trim().to_owned()
    };
    settings.destination_root_path = destination.clone();
    settings.manual_download_root_path = destination;
    settings
}

fn iso_now() -> String {
    iso_string(&Utc::now())
}

fn iso_string(value: &DateTime<Utc>) -> String {
    value.to_rfc3339_opts(chrono::SecondsFormat::Millis, true)
}

fn time_window_unit_key(unit: TimeWindowUnit) -> &'static str {
    match unit {
        TimeWindowUnit::None => "none",
        TimeWindowUnit::Day => "day",
        TimeWindowUnit::Week => "week",
        TimeWindowUnit::Month => "month",
    }
}

fn parse_time_window_unit(value: &str) -> TimeWindowUnit {
    match value {
        "day" => TimeWindowUnit::Day,
        "week" => TimeWindowUnit::Week,
        "month" => TimeWindowUnit::Month,
        _ => TimeWindowUnit::None,
    }
}

fn failed_retention_unit_key(unit: FailedJobRetentionUnit) -> &'static str {
    match unit {
        FailedJobRetentionUnit::None => "none",
        FailedJobRetentionUnit::Minute => "minute",
        FailedJobRetentionUnit::Hour => "hour",
        FailedJobRetentionUnit::Day => "day",
    }
}

fn parse_failed_retention_unit(value: &str) -> FailedJobRetentionUnit {
    match value {
        "minute" => FailedJobRetentionUnit::Minute,
        "hour" => FailedJobRetentionUnit::Hour,
        "day" => FailedJobRetentionUnit::Day,
        _ => FailedJobRetentionUnit::None,
    }
}

fn clamp_download_worker_count(value: i32) -> i32 {
    value.clamp(1, 6)
}

pub fn failed_job_cutoff_date(settings: &AppSettings) -> Option<DateTime<Utc>> {
    let value = settings.failed_job_retention_value;
    if value <= 0 {
        return None;
    }

    match settings.failed_job_retention_unit {
        FailedJobRetentionUnit::None => None,
        FailedJobRetentionUnit::Minute => Some(Utc::now() - Duration::minutes(value as i64)),
        FailedJobRetentionUnit::Hour => Some(Utc::now() - Duration::hours(value as i64)),
        FailedJobRetentionUnit::Day => Some(Utc::now() - Duration::days(value as i64)),
    }
}

pub fn should_stop_initial_backfill(
    checkpoint: &RoomCheckpoint,
    settings: &AppSettings,
) -> Result<bool> {
    if settings.message_limit > 0 && checkpoint.historical_message_count >= settings.message_limit {
        return Ok(true);
    }

    if settings.time_window_value <= 0 || matches!(settings.time_window_unit, TimeWindowUnit::None)
    {
        return Ok(false);
    }

    let Some(oldest_timestamp) = checkpoint.oldest_backfilled_timestamp else {
        return Ok(false);
    };

    let cutoff = match settings.time_window_unit {
        TimeWindowUnit::None => return Ok(false),
        TimeWindowUnit::Day => Utc::now() - Duration::days(settings.time_window_value as i64),
        TimeWindowUnit::Week => Utc::now() - Duration::weeks(settings.time_window_value as i64),
        TimeWindowUnit::Month => {
            Utc::now() - Duration::days((settings.time_window_value as i64) * 30)
        }
    };

    Ok(oldest_timestamp <= cutoff)
}

pub fn ensure_relative_to_root(root: &str, path: &str) -> Result<String> {
    path.strip_prefix(&(root.to_owned() + "/"))
        .map(ToOwned::to_owned)
        .ok_or_else(|| anyhow!("Path {path} is not inside root {root}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn temp_database_path() -> PathBuf {
        std::env::temp_dir().join(format!(
            "matrix-media-archiver-db-test-{}.sqlite3",
            Uuid::new_v4()
        ))
    }

    fn sample_discovery(event_id: &str) -> AttachmentDiscovery {
        AttachmentDiscovery {
            room_id: "!room:example.org".to_owned(),
            event_id: event_id.to_owned(),
            origin_server_timestamp: Utc::now(),
            source_kind: crate::domain::MediaSourceKind::Matrix,
            direct_url: None,
            mxc_url: format!("mxc://example.org/{event_id}"),
            fallback_source_url: None,
            thumbnail_source_url: None,
            thumbnail_cached_path: None,
            original_filename: Some(format!("{event_id}.bin")),
            mime_type: Some("application/octet-stream".to_owned()),
            category: MediaCategory::Other,
        }
    }

    async fn set_job_created_at(
        database: &AppDatabase,
        event_id: &str,
        created_at: DateTime<Utc>,
    ) -> Result<()> {
        let connection = database.inner.lock().await;
        connection.execute(
            "UPDATE download_jobs SET created_at = ?1, updated_at = ?1 WHERE event_id = ?2",
            params![iso_string(&created_at), event_id],
        )?;
        Ok(())
    }

    async fn remove_database_files(path: &Path) {
        let _ = tokio::fs::remove_file(path).await;
        let _ = tokio::fs::remove_file(path.with_extension("sqlite3-shm")).await;
        let _ = tokio::fs::remove_file(path.with_extension("sqlite3-wal")).await;
    }

    #[tokio::test]
    async fn claim_next_eligible_job_skips_ineligible_undecryptable_entries() {
        let path = temp_database_path();
        let database = AppDatabase::open(&path).await.expect("open test database");
        let now = Utc::now();

        database
            .enqueue_discovery(&sample_discovery("$blocked"), true)
            .await
            .expect("enqueue blocked job");
        database
            .enqueue_discovery(&sample_discovery("$ready"), true)
            .await
            .expect("enqueue ready job");

        set_job_created_at(&database, "$blocked", now - Duration::minutes(10))
            .await
            .expect("age blocked job");
        set_job_created_at(&database, "$ready", now - Duration::minutes(5))
            .await
            .expect("age ready job");

        let jobs = database.fetch_jobs(10, now).await.expect("fetch jobs");
        let blocked_job = jobs
            .iter()
            .find(|job| job.event_id == "$blocked")
            .expect("blocked job should exist");
        database
            .mark_job_undecryptable(blocked_job.id, now + Duration::minutes(5), "missing keys")
            .await
            .expect("mark blocked job undecryptable");

        let claimed = database
            .claim_next_eligible_job(now)
            .await
            .expect("claim job")
            .expect("expected an eligible job");

        assert_eq!(claimed.event_id, "$ready");

        drop(database);
        remove_database_files(&path).await;
    }

    #[tokio::test]
    async fn fetch_jobs_keeps_ineligible_retries_behind_ready_queue_items() {
        let path = temp_database_path();
        let database = AppDatabase::open(&path).await.expect("open test database");
        let now = Utc::now();

        database
            .enqueue_discovery(&sample_discovery("$first"), true)
            .await
            .expect("enqueue first job");
        database
            .enqueue_discovery(&sample_discovery("$blocked"), true)
            .await
            .expect("enqueue blocked job");
        database
            .enqueue_discovery(&sample_discovery("$third"), true)
            .await
            .expect("enqueue third job");

        set_job_created_at(&database, "$first", now - Duration::minutes(12))
            .await
            .expect("age first job");
        set_job_created_at(&database, "$blocked", now - Duration::minutes(11))
            .await
            .expect("age blocked job");
        set_job_created_at(&database, "$third", now - Duration::minutes(10))
            .await
            .expect("age third job");

        let jobs = database.fetch_jobs(10, now).await.expect("fetch jobs");
        let blocked_job = jobs
            .iter()
            .find(|job| job.event_id == "$blocked")
            .expect("blocked job should exist");
        database
            .mark_job_undecryptable(blocked_job.id, now + Duration::minutes(5), "missing keys")
            .await
            .expect("mark blocked job undecryptable");

        let ordered_jobs = database
            .fetch_jobs(10, now)
            .await
            .expect("fetch ordered jobs");
        let ordered_event_ids: Vec<_> = ordered_jobs
            .iter()
            .map(|job| job.event_id.as_str())
            .collect();

        assert_eq!(ordered_event_ids, vec!["$first", "$third", "$blocked"]);
        assert_eq!(
            ordered_jobs[2].state,
            DownloadJobState::UndecryptablePending
        );
        assert!(ordered_jobs[2].next_eligible_at.is_some());
        assert!(ordered_jobs[2].last_failure_at.is_some());

        drop(database);
        remove_database_files(&path).await;
    }
}
