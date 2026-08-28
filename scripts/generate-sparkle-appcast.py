#!/usr/bin/env python3
"""Generate Sparkle 2 appcasts for the macOS arm64 and x86_64 zip assets.

The Sparkle EdDSA private key is read from SPARKLE_ED25519_PRIVATE_KEY and is
never written to disk or printed. The public key embedded in the Mac app must
match SPARKLE_PUBLIC_ED_KEY.
"""

from __future__ import annotations

import argparse
import base64
import calendar
import os
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from email.utils import formatdate
from pathlib import Path

SPARKLE_PUBLIC_ED_KEY = "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"


def _fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def _load_signing_key(raw_b64: str):
    try:
        from nacl.signing import SigningKey
    except ImportError:
        _fail("PyNaCl is required to sign Sparkle appcasts (pip install pynacl).")

    try:
        raw = base64.b64decode(raw_b64, validate=True)
    except Exception:
        _fail("SPARKLE_ED25519_PRIVATE_KEY is not valid base64.")

    if len(raw) == 64:
        seed = raw[:32]
    elif len(raw) == 32:
        seed = raw
    else:
        _fail("SPARKLE_ED25519_PRIVATE_KEY must be Sparkle generate_keys format.")

    key = SigningKey(seed)
    public_b64 = base64.b64encode(bytes(key.verify_key)).decode("ascii")
    if public_b64 != SPARKLE_PUBLIC_ED_KEY:
        _fail("SPARKLE_ED25519_PRIVATE_KEY does not match the public key embedded in the Mac app.")
    return key


def _sign_file(key, path: Path) -> str:
    data = path.read_bytes()
    signature = key.sign(data).signature
    return base64.b64encode(signature).decode("ascii")


def _rfc822(timestamp: datetime) -> str:
    return formatdate(calendar.timegm(timestamp.utctimetuple()), usegmt=True)


def _write_appcast(
    output_path: Path,
    *,
    version: str,
    title: str,
    html_url: str,
    zip_path: Path,
    enclosure_url: str,
    signature: str,
    pub_date: datetime,
) -> None:
    ET.register_namespace("sparkle", SPARKLE_NS)
    rss = ET.Element("rss", {"version": "2.0", "xmlns:sparkle": SPARKLE_NS})
    channel = ET.SubElement(rss, "channel")
    ET.SubElement(channel, "title").text = "Matrix Media Share Client"
    ET.SubElement(channel, "link").text = html_url
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = title
    ET.SubElement(item, "pubDate").text = _rfc822(pub_date)
    ET.SubElement(item, f"{{{SPARKLE_NS}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE_NS}}}shortVersionString").text = version
    ET.SubElement(item, "link").text = html_url
    ET.SubElement(
        item,
        "enclosure",
        {
            "url": enclosure_url,
            "length": str(zip_path.stat().st_size),
            "type": "application/octet-stream",
            f"{{{SPARKLE_NS}}}edSignature": signature,
            f"{{{SPARKLE_NS}}}os": "macos",
        },
    )
    tree = ET.ElementTree(rss)
    ET.indent(tree, space="  ")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tree.write(output_path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--release-url", required=True)
    parser.add_argument("--arm64-zip", required=True, type=Path)
    parser.add_argument("--x86_64-zip", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--download-base-url", default="")
    args = parser.parse_args()

    private_key_b64 = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY", "").strip()
    if not private_key_b64:
        _fail("SPARKLE_ED25519_PRIVATE_KEY is required to sign Sparkle appcasts.")

    key = _load_signing_key(private_key_b64)
    version = args.version.lstrip("v")
    title = f"Matrix Media Share Client {version}"
    pub_date = datetime.now(timezone.utc)
    download_base = args.download_base_url.rstrip("/")
    if not download_base:
        download_base = f"https://github.com/bstone108/matrix-media-share-client/releases/download/v{version}"

    for arch, zip_path in (("arm64", args.arm64_zip), ("x86_64", args.x86_64_zip)):
        if not zip_path.is_file():
            _fail(f"macOS {arch} zip not found: {zip_path}")
        signature = _sign_file(key, zip_path)
        enclosure_url = f"{download_base}/{zip_path.name}"
        output_path = args.output_dir / f"appcast-macos-{arch}.xml"
        _write_appcast(
            output_path,
            version=version,
            title=title,
            html_url=args.release_url,
            zip_path=zip_path,
            enclosure_url=enclosure_url,
            signature=signature,
            pub_date=pub_date,
        )
        print(f"Wrote {output_path} for {zip_path.name}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
