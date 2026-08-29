#!/usr/bin/env python3
"""Write Sparkle 2 appcasts from Sparkle sign_update signatures.

This script never reads, prints, or stores the Sparkle private key. Release
signing is done by Sparkle's sign_update tool on macOS.
"""

from __future__ import annotations

import argparse
import base64
import calendar
import html
import os
import sys
from datetime import datetime, timezone
from email.utils import formatdate
from pathlib import Path

SPARKLE_PUBLIC_ED_KEY = "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"


def _fail(message: str) -> None:
    print(message, file=sys.stderr, flush=True)
    raise SystemExit(1)


def verify_generate_keys_matches_embedded_public(raw_b64: str) -> None:
    """Check Sparkle generate_keys material against SUPublicEDKey without printing it."""
    try:
        raw = base64.b64decode(raw_b64.strip(), validate=True)
    except Exception:
        _fail("SPARKLE_ED25519_PRIVATE_KEY is not valid base64.")
    if len(raw) != 64:
        _fail("SPARKLE_ED25519_PRIVATE_KEY must be Sparkle generate_keys format.")
    public_b64 = base64.b64encode(raw[32:]).decode("ascii")
    if public_b64 != SPARKLE_PUBLIC_ED_KEY:
        _fail("SPARKLE_ED25519_PRIVATE_KEY does not match the public key embedded in the Mac app.")


def _rfc822(timestamp: datetime) -> str:
    return formatdate(calendar.timegm(timestamp.utctimetuple()), usegmt=True)


def normalize_ed_signature(signature: str) -> str:
    cleaned = "".join(signature.split())
    if not cleaned:
        _fail("Sparkle sign_update returned an empty signature.")
    try:
        decoded = base64.b64decode(cleaned, validate=True)
    except Exception:
        _fail("Sparkle sign_update returned a signature that is not valid base64.")
    if len(decoded) != 64:
        _fail("Sparkle sign_update returned a signature that is not 64 decoded bytes.")
    return cleaned


def write_appcast(
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
    signature = normalize_ed_signature(signature)
    xml = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="{html.escape(SPARKLE_NS, quote=True)}">
  <channel>
    <title>Matrix Media Share Client</title>
    <link>{html.escape(html_url)}</link>
    <item>
      <title>{html.escape(title)}</title>
      <pubDate>{html.escape(_rfc822(pub_date))}</pubDate>
      <sparkle:version>{html.escape(version)}</sparkle:version>
      <sparkle:shortVersionString>{html.escape(version)}</sparkle:shortVersionString>
      <link>{html.escape(html_url)}</link>
      <enclosure url="{html.escape(enclosure_url, quote=True)}" length="{zip_path.stat().st_size}" type="application/octet-stream" sparkle:edSignature="{html.escape(signature, quote=True)}" sparkle:os="macos" />
    </item>
  </channel>
</rss>
"""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(xml, encoding="utf-8")


def _write_one(
    *,
    version: str,
    html_url: str,
    download_base: str,
    zip_path: Path,
    arch: str,
    signature: str,
    output_path: Path,
    pub_date: datetime,
) -> None:
    if not zip_path.is_file():
        _fail(f"macOS {arch} zip not found: {zip_path}")
    title = f"Matrix Media Share Client {version}"
    enclosure_url = f"{download_base}/{zip_path.name}"
    write_appcast(
        output_path,
        version=version,
        title=title,
        html_url=html_url,
        zip_path=zip_path,
        enclosure_url=enclosure_url,
        signature=signature,
        pub_date=pub_date,
    )
    print(f"Wrote {output_path} for {zip_path.name}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-env-key", action="store_true")
    parser.add_argument("--version")
    parser.add_argument("--release-url")
    parser.add_argument("--download-base-url", default="")
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--arch")
    parser.add_argument("--signature", default="")
    parser.add_argument("--signature-file", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--arm64-zip", type=Path)
    parser.add_argument("--x86_64-zip", type=Path)
    parser.add_argument("--arm64-signature", default="")
    parser.add_argument("--x86_64-signature", default="")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    if args.verify_env_key:
        raw_b64 = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY", "").strip()
        if not raw_b64:
            _fail("SPARKLE_ED25519_PRIVATE_KEY is required to sign Sparkle appcasts.")
        verify_generate_keys_matches_embedded_public(raw_b64)
        print("Sparkle EdDSA secret matches the embedded SUPublicEDKey.", file=sys.stderr)
        return 0

    if not args.version or not args.release_url:
        _fail("--version and --release-url are required to write appcasts.")

    version = args.version.lstrip("v")
    pub_date = datetime.now(timezone.utc)
    download_base = args.download_base_url.rstrip("/")
    if not download_base:
        download_base = f"https://github.com/bstone108/matrix-media-share-client/releases/download/v{version}"

    if args.zip is not None:
        if not args.arch or args.output is None:
            _fail("Single-zip mode requires --arch and --output.")
        signature = args.signature
        if args.signature_file is not None:
            signature = args.signature_file.read_text(encoding="utf-8")
        if not signature.strip():
            _fail("A Sparkle sign_update signature is required.")
        _write_one(
            version=version,
            html_url=args.release_url,
            download_base=download_base,
            zip_path=args.zip,
            arch=args.arch,
            signature=signature,
            output_path=args.output,
            pub_date=pub_date,
        )
        return 0

    if args.arm64_zip is None or args.x86_64_zip is None or args.output_dir is None:
        _fail("Provide --zip/--arch/--output or both macOS zips with signatures and --output-dir.")
    if not args.arm64_signature.strip() or not args.x86_64_signature.strip():
        _fail("Sparkle sign_update signatures are required for both macOS zips.")

    for arch, zip_path, signature in (
        ("arm64", args.arm64_zip, args.arm64_signature),
        ("x86_64", args.x86_64_zip, args.x86_64_signature),
    ):
        _write_one(
            version=version,
            html_url=args.release_url,
            download_base=download_base,
            zip_path=zip_path,
            arch=arch,
            signature=signature,
            output_path=args.output_dir / f"appcast-macos-{arch}.xml",
            pub_date=pub_date,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
