use crate::gateway_registry::page_url;
use crate::gateway_registry::raw_file_url;

pub fn render_landing_page(
    title: &str,
    file_cid: &str,
    thumbnail_cid: Option<&str>,
    page_cid: Option<&str>,
    primary_gateway_url: &str,
    alternates: &[(&str, &str, bool)],
) -> String {
    let escaped_title = escape_html(title);
    let primary_download = raw_file_url(primary_gateway_url, file_cid);
    let thumbnail_html = thumbnail_cid
        .map(|cid| {
            format!(
                "<img src=\"{}\" alt=\"{} thumbnail\" style=\"max-width: 420px; width: 100%; border-radius: 12px; display: block; margin-bottom: 18px;\" />",
                raw_file_url(primary_gateway_url, cid),
                escaped_title
            )
        })
        .unwrap_or_default();

    let alternate_links = alternates
        .iter()
        .enumerate()
        .map(|(index, (label, gateway_url, supports_html))| {
            let note = if *supports_html { "" } else { " <span>(file only)</span>" };
            let href = if *supports_html {
                page_cid
                    .map(|cid| page_url(gateway_url, cid))
                    .unwrap_or_else(|| raw_file_url(gateway_url, file_cid))
            } else {
                raw_file_url(gateway_url, file_cid)
            };
            format!(
                "<li><a id=\"gateway-link-{index}\" data-gateway=\"{}\" data-supports-html=\"{}\" href=\"{}\">{} via {}</a>{}</li>",
                escape_html(gateway_url),
                if *supports_html { "1" } else { "0" },
                href,
                escaped_title,
                escape_html(label),
                note
            )
        })
        .collect::<Vec<_>>()
        .join("\n");

    format!(
        "<!doctype html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
  <title>{title}</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f4f1ea;
      --panel: #fffaf1;
      --ink: #1f2a30;
      --accent: #0d6b57;
      --accent-2: #b85c38;
      --line: #d8cfbf;
    }}
    body {{
      margin: 0;
      background: radial-gradient(circle at top, #fffaf1 0%, var(--bg) 70%);
      color: var(--ink);
      font-family: Georgia, \"Iowan Old Style\", serif;
    }}
    main {{
      max-width: 720px;
      margin: 40px auto;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 28px;
      box-shadow: 0 18px 50px rgba(70, 52, 24, 0.12);
    }}
    h1 {{ margin-top: 0; }}
    .button {{
      display: inline-block;
      background: var(--accent);
      color: white;
      text-decoration: none;
      padding: 12px 18px;
      border-radius: 999px;
      font-weight: bold;
    }}
    .meta {{
      color: #5a635f;
      margin-bottom: 20px;
    }}
    .warning {{
      margin-top: 20px;
      padding: 12px 14px;
      border-radius: 12px;
      background: #fff1e8;
      border: 1px solid #efc1ab;
    }}
    ul {{ line-height: 1.7; }}
  </style>
</head>
<body>
  <main>
    <h1>{title}</h1>
    <p class=\"meta\">Shared with Matrix Media Share Client over IPFS.</p>
    {thumbnail}
    <p><a class=\"button\" href=\"{primary_download}\">Download link</a></p>
    <h2>Other gateways</h2>
    <ul>{alternate_links}</ul>
    <div class=\"warning\">If a gateway is slow or unavailable, try another one. Gateways labeled <strong>file only</strong> are fallback download links rather than preferred HTML landing-page hosts.</div>
  </main>
  <script>
    (function() {{
      const currentPath = window.location.pathname + window.location.search;
      const fileCid = {file_cid:?};
      document.querySelectorAll('a[data-gateway]').forEach(function(link) {{
        const gateway = (link.getAttribute('data-gateway') || '').replace(/\\/+$/, '');
        if (!gateway) {{
          return;
        }}
        const supportsHtml = link.getAttribute('data-supports-html') === '1';
        link.href = supportsHtml ? (gateway + currentPath) : (gateway + '/ipfs/' + fileCid);
      }});
    }})();
  </script>
</body>
</html>",
        title = escaped_title,
        thumbnail = thumbnail_html,
        primary_download = primary_download,
        alternate_links = alternate_links,
        file_cid = file_cid,
    )
}

fn escape_html(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

#[cfg(test)]
mod tests {
    use super::render_landing_page;

    #[test]
    fn includes_alternate_gateway_links() {
        let html = render_landing_page(
            "Example",
            "bafyfile",
            Some("bafythumb"),
            Some("bafypage"),
            "https://dweb.link",
            &[("Global", "https://ipfs.io", true), ("Global CDN", "https://4everland.io", false)],
        );

        assert!(html.contains("https://dweb.link/ipfs/bafyfile"));
        assert!(html.contains("window.location.pathname"));
        assert!(html.contains("data-gateway=\"https://ipfs.io\""));
        assert!(html.contains("href=\"https://ipfs.io/ipfs/bafypage\""));
        assert!(html.contains("href=\"https://4everland.io/ipfs/bafyfile\""));
        assert!(html.contains("file only"));
    }
}
