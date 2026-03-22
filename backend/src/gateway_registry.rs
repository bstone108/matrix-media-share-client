use crate::domain::GatewayStatusSnapshot;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GatewayDescriptor {
    pub gateway_url: &'static str,
    pub region_label: &'static str,
    pub supports_html: bool,
    pub supports_subdomain: bool,
    pub raw_file_ok: bool,
    pub enabled_by_default: bool,
}

pub const BOOTSTRAP_PRIMARY_GATEWAY: &str = "https://dweb.link";
pub const TINY_PROBE_CID: &str = "bafybeifx7yeb55armcsxwwitkymga5xf53dxiarykms3ygqic223w5sk3m";

pub fn default_gateways() -> Vec<GatewayDescriptor> {
    vec![
        GatewayDescriptor {
            gateway_url: "https://dweb.link",
            region_label: "Global",
            supports_html: true,
            supports_subdomain: true,
            raw_file_ok: true,
            enabled_by_default: true,
        },
        GatewayDescriptor {
            gateway_url: "https://ipfs.io",
            region_label: "Global",
            supports_html: true,
            supports_subdomain: false,
            raw_file_ok: true,
            enabled_by_default: true,
        },
        GatewayDescriptor {
            gateway_url: "https://eu.orbitor.dev",
            region_label: "Europe",
            supports_html: true,
            supports_subdomain: false,
            raw_file_ok: true,
            enabled_by_default: true,
        },
        GatewayDescriptor {
            gateway_url: "https://ipfs.ecolatam.com",
            region_label: "Latin America",
            supports_html: true,
            supports_subdomain: false,
            raw_file_ok: true,
            enabled_by_default: true,
        },
        GatewayDescriptor {
            gateway_url: "https://apac.orbitor.dev",
            region_label: "Asia-Pacific",
            supports_html: true,
            supports_subdomain: false,
            raw_file_ok: true,
            enabled_by_default: true,
        },
        GatewayDescriptor {
            gateway_url: "https://4everland.io",
            region_label: "Global CDN",
            supports_html: false,
            supports_subdomain: true,
            raw_file_ok: true,
            enabled_by_default: true,
        },
    ]
}

pub fn raw_file_url(gateway_url: &str, cid: &str) -> String {
    format!("{}/ipfs/{}", gateway_url.trim_end_matches('/'), cid)
}

pub fn page_url(gateway_url: &str, cid: &str) -> String {
    format!("{}/ipfs/{}", gateway_url.trim_end_matches('/'), cid)
}

pub fn bootstrap_gateway_statuses() -> Vec<GatewayStatusSnapshot> {
    default_gateways()
        .into_iter()
        .map(|gateway| GatewayStatusSnapshot {
            gateway_url: gateway.gateway_url.to_owned(),
            region_label: gateway.region_label.to_owned(),
            supports_html: gateway.supports_html,
            supports_subdomain: gateway.supports_subdomain,
            raw_file_ok: gateway.raw_file_ok,
            enabled_by_default: gateway.enabled_by_default,
            last_success_at: None,
            recent_success_rate: 0.0,
            p50_ttfb_ms: None,
            selected_as_primary: gateway.gateway_url == BOOTSTRAP_PRIMARY_GATEWAY,
        })
        .collect()
}
