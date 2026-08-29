#!/usr/bin/env python3
"""Generate Sparkle appcast XML from provided signatures. Never uses the production private key."""

from __future__ import annotations

import base64
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate-sparkle-appcast.py"
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
PRODUCTION_PUBLIC_KEY = "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="
THROWAY_SIGNATURE = base64.b64encode(bytes(range(64))).decode("ascii")


def _load_module():
    spec = importlib.util.spec_from_file_location("generate_sparkle_appcast", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class GenerateSparkleAppcastTests(unittest.TestCase):
    def test_embedded_public_key_matches_sparkle_config(self):
        module = _load_module()
        self.assertEqual(module.SPARKLE_PUBLIC_ED_KEY, PRODUCTION_PUBLIC_KEY)

    def test_writes_arch_appcasts_from_sign_update_signatures(self):
        module = _load_module()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            arm64_zip = tmp_path / "MatrixMediaShareClientQt-2026.8.28.2-macos-arm64.zip"
            x86_zip = tmp_path / "MatrixMediaShareClientQt-2026.8.28.2-macos-x86_64.zip"
            arm64_zip.write_bytes(b"arm64-zip-bytes")
            x86_zip.write_bytes(b"x86-zip-bytes")
            out_dir = tmp_path / "appcasts"

            argv = sys.argv
            try:
                sys.argv = [
                    str(SCRIPT),
                    "--version",
                    "2026.8.28.2",
                    "--release-url",
                    "https://github.com/bstone108/matrix-media-share-client/releases/tag/v2026.8.28.2",
                    "--arm64-zip",
                    str(arm64_zip),
                    "--x86_64-zip",
                    str(x86_zip),
                    "--arm64-signature",
                    THROWAY_SIGNATURE,
                    "--x86_64-signature",
                    THROWAY_SIGNATURE,
                    "--output-dir",
                    str(out_dir),
                ]
                self.assertEqual(module.main(), 0)
            finally:
                sys.argv = argv

            for arch, zip_name in (
                ("arm64", arm64_zip.name),
                ("x86_64", x86_zip.name),
            ):
                path = out_dir / f"appcast-macos-{arch}.xml"
                self.assertTrue(path.is_file(), path)
                tree = ET.parse(path)
                enclosure = tree.find(".//enclosure")
                self.assertIsNotNone(enclosure)
                self.assertTrue(enclosure.attrib["url"].endswith(zip_name))
                self.assertEqual(enclosure.attrib[f"{{{SPARKLE_NS}}}edSignature"], THROWAY_SIGNATURE)
                self.assertEqual(enclosure.attrib[f"{{{SPARKLE_NS}}}os"], "macos")
                version = tree.find(f".//{{{SPARKLE_NS}}}version")
                self.assertIsNotNone(version)
                self.assertEqual(version.text, "2026.8.28.2")

    def test_production_public_key_rejects_throwaway_private_key(self):
        module = _load_module()
        throwaway = base64.b64encode(os.urandom(64)).decode("ascii")
        with self.assertRaises(SystemExit):
            module.verify_generate_keys_matches_embedded_public(throwaway)

    def test_verify_env_key_requires_secret(self):
        module = _load_module()
        previous = os.environ.pop("SPARKLE_ED25519_PRIVATE_KEY", None)
        argv = sys.argv
        try:
            sys.argv = [str(SCRIPT), "--verify-env-key"]
            with self.assertRaises(SystemExit):
                module.main()
        finally:
            sys.argv = argv
            if previous is not None:
                os.environ["SPARKLE_ED25519_PRIVATE_KEY"] = previous

    def test_sign_update_resolver_skips_dsym_and_old_dsa(self):
        script = ROOT / "scripts" / "sign-sparkle-zip.sh"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sparkle = root / "Sparkle-2.9.6"
            dwarf = sparkle / "Symbols" / "sign_update.dSYM" / "Contents" / "Resources" / "DWARF" / "sign_update"
            old_dsa = sparkle / "bin" / "old_dsa_scripts" / "sign_update"
            cli = sparkle / "bin" / "sign_update"
            dwarf.parent.mkdir(parents=True)
            old_dsa.parent.mkdir(parents=True)
            # Create the tarball's extra basename collisions first, matching
            # Sparkle-2.9.6.tar.xz extract order.
            dwarf.write_text("dwarf-debug-file\n", encoding="utf-8")
            old_dsa.write_text("old-dsa-helper\n", encoding="utf-8")
            cli.write_text("official-cli\n", encoding="utf-8")

            resolved = subprocess.check_output(
                ["bash", str(script), "--resolve-tool", str(root)],
                text=True,
            ).strip()
            self.assertEqual(Path(resolved).resolve(), cli.resolve())

    def test_sign_update_resolver_errors_when_cli_missing(self):
        script = ROOT / "scripts" / "sign-sparkle-zip.sh"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dwarf = root / "Sparkle-2.9.6" / "Symbols" / "sign_update.dSYM" / "Contents" / "Resources" / "DWARF" / "sign_update"
            dwarf.parent.mkdir(parents=True)
            dwarf.write_text("dwarf-debug-file\n", encoding="utf-8")
            completed = subprocess.run(
                ["bash", str(script), "--resolve-tool", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout.strip(), "")


if __name__ == "__main__":
    unittest.main()
