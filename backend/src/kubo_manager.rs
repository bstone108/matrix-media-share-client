use std::{
    path::{Path, PathBuf},
    process::Stdio,
    sync::Arc,
    time::Duration,
};

use anyhow::{Context, Result, anyhow};
use serde::Deserialize;
use tokio::{
    process::{Child, Command},
    sync::Mutex,
    time::sleep,
};

use crate::{
    app_paths::AppPaths,
    domain::{GatewayStatusSnapshot, IpfsRuntimeState, IpfsStatusSnapshot},
};

const DEFAULT_KUBO_API_URL: &str = "http://127.0.0.1:5001";

#[derive(Clone)]
pub struct KuboManager {
    paths: AppPaths,
    client: reqwest::Client,
    process: Arc<Mutex<Option<Child>>>,
    resolved_binary: Arc<Mutex<Option<PathBuf>>>,
}

#[derive(Debug, Deserialize)]
struct KuboIdResponse {
    #[serde(rename = "ID")]
    id: String,
}

impl KuboManager {
    pub fn new(paths: AppPaths) -> Self {
        Self {
            paths,
            client: reqwest::Client::builder()
                .timeout(Duration::from_secs(5))
                .build()
                .expect("kubo client should build"),
            process: Arc::new(Mutex::new(None)),
            resolved_binary: Arc::new(Mutex::new(None)),
        }
    }

    pub async fn start(&self) -> Result<IpfsStatusSnapshot> {
        let Some(binary_path) = self.resolve_binary_path().await? else {
            return Ok(IpfsStatusSnapshot {
                state: IpfsRuntimeState::Unavailable,
                kubo_binary_path: None,
                api_url: Some(DEFAULT_KUBO_API_URL.to_owned()),
                peer_id: None,
                primary_gateway_url: None,
                gateway_statuses: Vec::new(),
                last_error: Some("Bundled Kubo binary was not found.".to_owned()),
            });
        };

        if self.try_id().await.is_ok() {
            let peer_id = self.try_id().await.ok();
            return Ok(IpfsStatusSnapshot {
                state: IpfsRuntimeState::Running,
                kubo_binary_path: Some(binary_path.display().to_string()),
                api_url: Some(DEFAULT_KUBO_API_URL.to_owned()),
                peer_id,
                primary_gateway_url: None,
                gateway_statuses: Vec::<GatewayStatusSnapshot>::new(),
                last_error: None,
            });
        }

        self.init_repo_if_needed(&binary_path).await?;
        let mut command = Command::new(&binary_path);
        command
            .env("IPFS_PATH", &self.paths.kubo_repo_path)
            .arg("daemon")
            .arg("--migrate=true")
            .stdout(Stdio::null())
            .stderr(Stdio::null());

        let child = command
            .spawn()
            .with_context(|| format!("Failed to start Kubo at {}", binary_path.display()))?;
        *self.process.lock().await = Some(child);

        for _ in 0..40 {
            if let Ok(peer_id) = self.try_id().await {
                return Ok(IpfsStatusSnapshot {
                    state: IpfsRuntimeState::Running,
                    kubo_binary_path: Some(binary_path.display().to_string()),
                    api_url: Some(DEFAULT_KUBO_API_URL.to_owned()),
                    peer_id: Some(peer_id),
                    primary_gateway_url: None,
                    gateway_statuses: Vec::new(),
                    last_error: None,
                });
            }
            sleep(Duration::from_millis(500)).await;
        }

        Err(anyhow!("Timed out waiting for the bundled Kubo node to become ready"))
    }

    pub async fn stop(&self) {
        if let Some(mut child) = self.process.lock().await.take() {
            let _ = child.kill().await;
            let _ = child.wait().await;
        }
    }

    pub fn api_url(&self) -> &'static str {
        DEFAULT_KUBO_API_URL
    }

    async fn init_repo_if_needed(&self, binary_path: &Path) -> Result<()> {
        if self.paths.kubo_repo_path.join("config").exists() {
            return Ok(());
        }

        let status = Command::new(binary_path)
            .env("IPFS_PATH", &self.paths.kubo_repo_path)
            .arg("init")
            .arg("--profile=lowpower")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .await
            .context("Failed to initialize the bundled Kubo repo")?;

        if !status.success() {
            return Err(anyhow!("Kubo init failed with status {status}"));
        }

        Ok(())
    }

    async fn resolve_binary_path(&self) -> Result<Option<PathBuf>> {
        if let Some(path) = self.resolved_binary.lock().await.clone() {
            return Ok(Some(path));
        }

        let mut candidates = Vec::new();
        if let Ok(path) = std::env::var("MATRIX_MEDIA_SHARE_CLIENT_KUBO") {
            candidates.push(PathBuf::from(path));
        }

        #[cfg(target_os = "windows")]
        {
            candidates.push(self.paths.kubo_path.join("kubo.exe"));
        }
        #[cfg(not(target_os = "windows"))]
        {
            candidates.push(self.paths.kubo_path.join("kubo"));
        }

        if let Some(path) = which_on_path("kubo") {
            candidates.push(path);
        }

        let resolved = candidates.into_iter().find(|candidate| candidate.is_file());
        *self.resolved_binary.lock().await = resolved.clone();
        Ok(resolved)
    }

    async fn try_id(&self) -> Result<String> {
        let response = self
            .client
            .post(format!("{}/api/v0/id", self.api_url()))
            .send()
            .await
            .context("Failed to query the local Kubo node")?
            .error_for_status()
            .context("Local Kubo node returned an error while fetching ID")?;
        let payload = response
            .json::<KuboIdResponse>()
            .await
            .context("Failed to decode the local Kubo ID response")?;
        Ok(payload.id)
    }
}

fn which_on_path(binary_name: &str) -> Option<PathBuf> {
    std::env::var_os("PATH").and_then(|value| {
        std::env::split_paths(&value).find_map(|directory| {
            let candidate = directory.join(binary_name);
            candidate.is_file().then_some(candidate)
        })
    })
}
