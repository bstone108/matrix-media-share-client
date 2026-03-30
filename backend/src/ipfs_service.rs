use std::{collections::HashSet, path::Path, sync::Arc};

use anyhow::{Context, Result, anyhow};
use reqwest::multipart::{Form, Part};
use tokio::fs;

use crate::{
    app_paths::AppPaths,
    domain::IpfsStatusSnapshot,
    gateway_health::{GatewayHealthService, select_primary_gateway},
    gateway_registry::{default_gateways, page_url, raw_file_url},
    kubo_manager::KuboManager,
    landing_page::render_landing_page,
};

#[derive(Clone, Debug)]
pub struct PublishedShare {
    pub file_cid: String,
    pub thumbnail_cid: Option<String>,
    pub page_cid: String,
    pub landing_page_url: String,
}

#[derive(Clone)]
pub struct IpfsService {
    client: reqwest::Client,
    paths: AppPaths,
    manager: Arc<KuboManager>,
    gateway_health: GatewayHealthService,
}

impl IpfsService {
    pub fn new(paths: AppPaths) -> Self {
        let manager = Arc::new(KuboManager::new(paths.clone()));
        Self {
            client: reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(60))
                .build()
                .expect("ipfs client should build"),
            paths,
            manager,
            gateway_health: GatewayHealthService::new(),
        }
    }

    pub async fn start(&self) -> Result<IpfsStatusSnapshot> {
        let mut status = self.manager.start().await?;
        let gateway_statuses = self.gateway_health.probe_gateways(None).await;
        status.primary_gateway_url = Some(select_primary_gateway(&gateway_statuses));
        status.gateway_statuses = gateway_statuses;
        Ok(status)
    }

    pub async fn stop(&self) {
        self.manager.stop().await;
    }

    pub async fn refresh_status(&self, last_published_landing_page_cid: Option<&str>) -> Result<IpfsStatusSnapshot> {
        let mut status = self.manager.start().await?;
        let gateway_statuses = self
            .gateway_health
            .probe_gateways(last_published_landing_page_cid)
            .await;
        status.primary_gateway_url = Some(select_primary_gateway(&gateway_statuses));
        status.gateway_statuses = gateway_statuses;
        Ok(status)
    }

    pub async fn publish_share(
        &self,
        title: &str,
        file_path: &Path,
        thumbnail_path: Option<&Path>,
        landing_page_output_path: &Path,
        preferred_gateway_url: Option<&str>,
        preferred_gateway_urls: &[String],
    ) -> Result<PublishedShare> {
        let status = self.refresh_status(None).await?;
        if !matches!(status.state, crate::domain::IpfsRuntimeState::Running) {
            return Err(anyhow!(
                "{}",
                status
                    .last_error
                    .unwrap_or_else(|| "The bundled Kubo node is unavailable.".to_owned())
            ));
        }

        let file_bytes = fs::read(file_path)
            .await
            .with_context(|| format!("Failed to read {}", file_path.display()))?;
        let file_name = file_path
            .file_name()
            .and_then(|value| value.to_str())
            .unwrap_or("shared-file");
        let file_cid = self.add_named_bytes(file_name, file_bytes, None).await?;

        let thumbnail_cid = match thumbnail_path {
            Some(path) if path.exists() => {
                let bytes = fs::read(path)
                    .await
                    .with_context(|| format!("Failed to read {}", path.display()))?;
                let name = path.file_name().and_then(|value| value.to_str()).unwrap_or("thumbnail.jpg");
                Some(self.add_named_bytes(name, bytes, Some("image/jpeg")).await?)
            }
            _ => None,
        };

        let share_gateways = configured_share_gateways(
            preferred_gateway_url,
            status.primary_gateway_url.as_deref(),
            preferred_gateway_urls,
        );
        let primary_gateway = select_share_primary_gateway(
            preferred_gateway_url,
            status.primary_gateway_url.as_deref(),
            &share_gateways,
        );
        let alternate_gateways = share_gateways
            .iter()
            .map(|gateway| (
                gateway.region_label.as_str(),
                gateway.gateway_url.as_str(),
                gateway.supports_html,
            ))
            .collect::<Vec<_>>();

        let html = render_landing_page(
            title,
            &file_cid,
            thumbnail_cid.as_deref(),
            None,
            &primary_gateway,
            &alternate_gateways,
        );
        let html_bytes = html.clone().into_bytes();
        let page_cid = self
            .add_named_bytes("index.html", html_bytes.clone(), Some("text/html"))
            .await?;
        let landing_page_url = page_url(&primary_gateway, &page_cid);

        if let Some(parent) = landing_page_output_path.parent() {
            fs::create_dir_all(parent)
                .await
                .with_context(|| format!("Failed to create {}", parent.display()))?;
        }
        fs::write(landing_page_output_path, html_bytes)
        .await
        .with_context(|| format!("Failed to persist {}", landing_page_output_path.display()))?;

        Ok(PublishedShare {
            file_cid,
            thumbnail_cid,
            page_cid,
            landing_page_url,
        })
    }

    pub fn raw_gateway_urls(&self, cid: &str) -> Vec<(String, String, bool)> {
        default_gateways()
            .into_iter()
            .filter(|gateway| gateway.enabled_by_default && gateway.raw_file_ok)
            .map(|gateway| {
                (
                    gateway.region_label.to_owned(),
                    raw_file_url(gateway.gateway_url, cid),
                    gateway.supports_html,
                )
            })
            .collect()
    }

    async fn add_named_bytes(
        &self,
        name: &str,
        bytes: Vec<u8>,
        content_type: Option<&str>,
    ) -> Result<String> {
        #[derive(serde::Deserialize)]
        struct AddResponse {
            #[serde(rename = "Hash")]
            hash: String,
        }

        let part = Part::bytes(bytes).file_name(name.to_owned());
        let part = match content_type {
            Some(value) => part.mime_str(value)?,
            None => part,
        };
        let form = Form::new().part("file", part);

        let response = self
            .client
            .post(format!(
                "{}/api/v0/add?pin=true&cid-version=1&wrap-with-directory=false",
                self.manager.api_url()
            ))
            .multipart(form)
            .send()
            .await
            .context("Failed to add content to the bundled Kubo node")?
            .error_for_status()
            .context("The bundled Kubo node rejected the add request")?;
        let payload = response
            .json::<AddResponse>()
            .await
            .context("Failed to decode the bundled Kubo add response")?;
        Ok(payload.hash)
    }
}

#[derive(Clone, Debug)]
struct ShareGatewayDescriptor {
    region_label: String,
    gateway_url: String,
    supports_html: bool,
}

fn configured_share_gateways(
    preferred_gateway_url: Option<&str>,
    status_primary_gateway_url: Option<&str>,
    preferred_gateway_urls: &[String],
) -> Vec<ShareGatewayDescriptor> {
    let defaults = default_gateways();
    let mut urls = Vec::new();
    if let Some(url) = preferred_gateway_url {
        urls.push(url.to_owned());
    }
    if let Some(url) = status_primary_gateway_url {
        urls.push(url.to_owned());
    }
    if preferred_gateway_urls.is_empty() {
        urls.extend(
            defaults
                .iter()
                .filter(|gateway| gateway.enabled_by_default)
                .map(|gateway| gateway.gateway_url.to_owned()),
        );
    } else {
        urls.extend(preferred_gateway_urls.iter().cloned());
    }

    let mut seen = HashSet::new();
    let mut gateways = Vec::new();
    for raw_url in urls {
        let normalized = normalize_gateway_url(&raw_url);
        if normalized.is_empty() || !seen.insert(normalized.clone()) {
            continue;
        }

        if let Some(descriptor) = defaults
            .iter()
            .find(|gateway| normalize_gateway_url(gateway.gateway_url) == normalized)
        {
            gateways.push(ShareGatewayDescriptor {
                region_label: descriptor.region_label.to_owned(),
                gateway_url: normalized,
                supports_html: descriptor.supports_html,
            });
        } else {
            gateways.push(ShareGatewayDescriptor {
                region_label: "Custom Gateway".to_owned(),
                gateway_url: normalized,
                supports_html: true,
            });
        }
    }

    gateways
}

fn select_share_primary_gateway(
    preferred_gateway_url: Option<&str>,
    status_primary_gateway_url: Option<&str>,
    gateways: &[ShareGatewayDescriptor],
) -> String {
    let preferred = preferred_gateway_url
        .map(normalize_gateway_url)
        .filter(|value| !value.is_empty());
    if let Some(url) = preferred.as_deref() {
        if gateways
            .iter()
            .any(|gateway| gateway.supports_html && gateway.gateway_url == url)
        {
            return url.to_owned();
        }
    }

    let status_primary = status_primary_gateway_url
        .map(normalize_gateway_url)
        .filter(|value| !value.is_empty());
    if let Some(url) = status_primary.as_deref() {
        if gateways
            .iter()
            .any(|gateway| gateway.supports_html && gateway.gateway_url == url)
        {
            return url.to_owned();
        }
    }

    gateways
        .iter()
        .find(|gateway| gateway.supports_html)
        .map(|gateway| gateway.gateway_url.clone())
        .unwrap_or_else(|| "https://dweb.link".to_owned())
}

fn normalize_gateway_url(value: &str) -> String {
    value.trim().trim_end_matches('/').to_owned()
}
