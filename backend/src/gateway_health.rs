use std::time::Instant;

use chrono::Utc;
use reqwest::StatusCode;

use crate::{
    domain::GatewayStatusSnapshot,
    gateway_registry::{BOOTSTRAP_PRIMARY_GATEWAY, TINY_PROBE_CID, default_gateways, raw_file_url},
};

#[derive(Clone)]
pub struct GatewayHealthService {
    client: reqwest::Client,
}

impl GatewayHealthService {
    pub fn new() -> Self {
        Self {
            client: reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(20))
                .build()
                .expect("gateway health client should build"),
        }
    }

    pub async fn probe_gateways(
        &self,
        last_published_landing_page_cid: Option<&str>,
    ) -> Vec<GatewayStatusSnapshot> {
        let probe_cid = last_published_landing_page_cid.unwrap_or(TINY_PROBE_CID);
        let mut statuses = Vec::new();

        for descriptor in default_gateways() {
            let probe_url = raw_file_url(descriptor.gateway_url, probe_cid);
            let started_at = Instant::now();
            let result = self.client.get(&probe_url).send().await;
            let duration_ms = started_at.elapsed().as_millis() as i64;
            let success = result
                .as_ref()
                .map(|response| {
                    response.status().is_success()
                        || response.status() == StatusCode::MOVED_PERMANENTLY
                        || response.status() == StatusCode::FOUND
                })
                .unwrap_or(false);

            statuses.push(GatewayStatusSnapshot {
                gateway_url: descriptor.gateway_url.to_owned(),
                region_label: descriptor.region_label.to_owned(),
                supports_html: descriptor.supports_html,
                supports_subdomain: descriptor.supports_subdomain,
                raw_file_ok: descriptor.raw_file_ok,
                enabled_by_default: descriptor.enabled_by_default,
                last_success_at: success.then(Utc::now),
                recent_success_rate: if success { 1.0 } else { 0.0 },
                p50_ttfb_ms: success.then_some(duration_ms),
                selected_as_primary: false,
            });
        }

        let primary = select_primary_gateway(&statuses);
        for status in &mut statuses {
            status.selected_as_primary = status.gateway_url == primary;
        }

        statuses
    }
}

pub fn select_primary_gateway(statuses: &[GatewayStatusSnapshot]) -> String {
    statuses
        .iter()
        .filter(|status| status.supports_html)
        .max_by(|left, right| gateway_sort_key(left).cmp(&gateway_sort_key(right)))
        .map(|status| status.gateway_url.clone())
        .unwrap_or_else(|| BOOTSTRAP_PRIMARY_GATEWAY.to_owned())
}

fn gateway_sort_key(status: &GatewayStatusSnapshot) -> (i32, i64) {
    let success_rank = (status.recent_success_rate * 1000.0) as i32;
    let latency_rank = status.p50_ttfb_ms.unwrap_or(i64::MAX);
    (success_rank, -latency_rank)
}

#[cfg(test)]
mod tests {
    use chrono::Utc;

    use super::select_primary_gateway;
    use crate::domain::GatewayStatusSnapshot;

    #[test]
    fn selects_successful_low_latency_html_gateway() {
        let statuses = vec![
            GatewayStatusSnapshot {
                gateway_url: "https://dweb.link".into(),
                region_label: "Global".into(),
                supports_html: true,
                supports_subdomain: true,
                raw_file_ok: true,
                enabled_by_default: true,
                last_success_at: Some(Utc::now()),
                recent_success_rate: 1.0,
                p50_ttfb_ms: Some(450),
                selected_as_primary: false,
            },
            GatewayStatusSnapshot {
                gateway_url: "https://ipfs.io".into(),
                region_label: "Global".into(),
                supports_html: true,
                supports_subdomain: false,
                raw_file_ok: true,
                enabled_by_default: true,
                last_success_at: Some(Utc::now()),
                recent_success_rate: 1.0,
                p50_ttfb_ms: Some(150),
                selected_as_primary: false,
            },
        ];

        assert_eq!(select_primary_gateway(&statuses), "https://ipfs.io");
    }
}
