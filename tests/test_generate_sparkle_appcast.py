#!/usr/bin/env python3
"""Sign Sparkle appcasts with a throwaway Ed25519 key. Never uses the production private key."""

from __future__ import annotations

import base64
import importlib.util
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate-sparkle-appcast.py"
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
PRODUCTION_PUBLIC_KEY = "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="


def _load_module():
    spec = importlib.util.spec_from_file_location("generate_sparkle_appcast", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _throwaway_key():
    from nacl.signing import SigningKey

    key = SigningKey.generate()
    private_b64 = base64.b64encode(bytes(key) + bytes(key.verify_key)).decode("ascii")
    public_b64 = base64.b64encode(bytes(key.verify_key)).decode("ascii")
    return private_b64, public_b64


class GenerateSparkleAppcastTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import nacl.signing  # noqa: F401
        except ImportError as exc:  # pragma: no cover
            raise unittest.SkipTest("pynacl is required") from exc

    def test_throwaway_key_writes_arch_appcasts(self):
        module = _load_module()
        private_b64, public_b64 = _throwaway_key()
        self.assertNotEqual(public_b64, PRODUCTION_PUBLIC_KEY)
        module.SPARKLE_PUBLIC_ED_KEY = public_b64

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            arm64_zip = tmp_path / "MatrixMediaShareClientQt-2026.8.28.2-macos-arm64.zip"
            x86_zip = tmp_path / "MatrixMediaShareClientQt-2026.8.28.2-macos-x86_64.zip"
            arm64_zip.write_bytes(b"arm64-zip-bytes")
            x86_zip.write_bytes(b"x86-zip-bytes")
            out_dir = tmp_path / "appcasts"

            previous = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY")
            os.environ["SPARKLE_ED25519_PRIVATE_KEY"] = private_b64
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
                    "--output-dir",
                    str(out_dir),
                ]
                self.assertEqual(module.main(), 0)
            finally:
                sys.argv = argv
                if previous is None:
                    os.environ.pop("SPARKLE_ED25519_PRIVATE_KEY", None)
                else:
                    os.environ["SPARKLE_ED25519_PRIVATE_KEY"] = previous

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
                self.assertTrue(enclosure.attrib[f"{{{SPARKLE_NS}}}edSignature"])
                self.assertEqual(enclosure.attrib[f"{{{SPARKLE_NS}}}os"], "macos")
                version = tree.find(f".//{{{SPARKLE_NS}}}version")
                self.assertIsNotNone(version)
                self.assertEqual(version.text, "2026.8.28.2")

    def test_production_public_key_rejects_throwaway_private_key(self):
        module = _load_module()
        private_b64, public_b64 = _throwaway_key()
        self.assertNotEqual(public_b64, module.SPARKLE_PUBLIC_ED_KEY)
        with self.assertRaises(SystemExit):
            module._load_signing_key(private_b64)


if __name__ == "__main__":
    unittest.main()
