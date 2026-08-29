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
SIGN_ZIP = ROOT / "scripts" / "sign-sparkle-zip.sh"
SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
PRODUCTION_PUBLIC_KEY = "t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="
THROWAY_SIGNATURE = base64.b64encode(bytes(range(64))).decode("ascii")


def _load_module():
    spec = importlib.util.spec_from_file_location("generate_sparkle_appcast", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _matching_expanded_key() -> str:
    public = base64.b64decode(PRODUCTION_PUBLIC_KEY)
    seed = bytes(range(32))
    return base64.b64encode(seed + public).decode("ascii")


def _matching_old_96_key() -> str:
    public = base64.b64decode(PRODUCTION_PUBLIC_KEY)
    blob = bytes(range(64)) + public
    return base64.b64encode(blob).decode("ascii")


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
        with self.assertRaises(SystemExit) as raised:
            module.verify_generate_keys_matches_embedded_public(throwaway)
        self.assertNotIn(throwaway, str(raised.exception))

    def test_accepts_length_88_expanded_ed25519_matching_public_key(self):
        module = _load_module()
        key = _matching_expanded_key()
        self.assertEqual(len(key), 88)
        module.verify_generate_keys_matches_embedded_public(key)
        self.assertEqual(len(module.decode_sparkle_private_key(key)), 64)

    def test_accepts_old_96_byte_generate_keys_format(self):
        module = _load_module()
        key = _matching_old_96_key()
        self.assertEqual(len(key), 128)
        module.verify_generate_keys_matches_embedded_public(key)
        self.assertEqual(len(module.decode_sparkle_private_key(key)), 96)

    def test_accepts_32_byte_seed_without_echoing_it(self):
        module = _load_module()
        seed = base64.b64encode(bytes(range(32))).decode("ascii")
        self.assertGreaterEqual(len(seed), 43)
        module.verify_generate_keys_matches_embedded_public(seed)

    def test_accepts_whitespace_and_missing_padding(self):
        module = _load_module()
        key = _matching_expanded_key()
        wrapped = key[:40] + "\n" + key[40:]
        module.verify_generate_keys_matches_embedded_public(wrapped)
        unpadded = key.rstrip("=")
        module.verify_generate_keys_matches_embedded_public(unpadded)

    def test_rejects_too_short_secret_without_echoing_it(self):
        module = _load_module()
        short = "YWJjZA=="
        with self.assertRaises(SystemExit) as raised:
            module.verify_generate_keys_matches_embedded_public(short)
        message = str(raised.exception)
        self.assertNotIn(short, message)

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

    def test_write_key_file_normalizes_seed_public_to_32_byte_seed(self):
        module = _load_module()
        key = _matching_expanded_key()
        previous = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY")
        argv = sys.argv
        with tempfile.TemporaryDirectory() as tmp:
            key_path = Path(tmp) / "ed.key"
            os.environ["SPARKLE_ED25519_PRIVATE_KEY"] = f"\n{key[:40]}\n{key[40:]}\n"
            try:
                sys.argv = [str(SCRIPT), "--write-key-file", str(key_path)]
                self.assertEqual(module.main(), 0)
            finally:
                sys.argv = argv
                if previous is None:
                    os.environ.pop("SPARKLE_ED25519_PRIVATE_KEY", None)
                else:
                    os.environ["SPARKLE_ED25519_PRIVATE_KEY"] = previous
            written = key_path.read_bytes()
            self.assertEqual(base64.b64decode(written), bytes(range(32)))
            self.assertEqual(key_path.stat().st_mode & 0o777, 0o600)
            self.assertNotIn(key.encode("ascii"), written)


class NormalizeSparkleEdKeyTests(unittest.TestCase):
    HELPER = ROOT / "scripts" / "normalize-sparkle-ed-key.py"

    def _run(self, secret_b64: str):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "normalized.b64"
            result = subprocess.run(
                [sys.executable, str(self.HELPER), PRODUCTION_PUBLIC_KEY, str(out)],
                input=secret_b64,
                capture_output=True,
                text=True,
                check=False,
            )
            body = out.read_bytes() if out.is_file() else b""
            return result.returncode, result.stdout, result.stderr, body

    def test_64_byte_seed_public_becomes_32_byte_seed(self):
        public = base64.b64decode(PRODUCTION_PUBLIC_KEY)
        secret = bytes(range(32)) + public
        key = base64.b64encode(secret).decode("ascii")
        self.assertEqual(len(key), 88)
        rc, stdout, stderr, body = self._run(key)
        self.assertEqual(rc, 0, stderr)
        self.assertEqual(stdout, "")
        self.assertEqual(base64.b64decode(body), bytes(range(32)))
        self.assertIn("decoded 64 bytes -> seed (32 bytes)", stderr)
        self.assertNotIn(key, stdout)
        self.assertNotIn(key, stderr)
        self.assertNotIn(body.decode("ascii"), stderr)

    def test_64_byte_expanded_becomes_old_96(self):
        public = base64.b64decode(PRODUCTION_PUBLIC_KEY)
        expanded = bytes([0x42]) * 64
        key = base64.b64encode(expanded).decode("ascii")
        rc, stdout, stderr, body = self._run(key)
        self.assertEqual(rc, 0, stderr)
        self.assertEqual(stdout, "")
        got = base64.b64decode(body)
        self.assertEqual(len(got), 96)
        self.assertEqual(got[:64], expanded)
        self.assertEqual(got[64:], public)
        self.assertIn("decoded 64 bytes -> old-96 (96 bytes)", stderr)
        self.assertNotIn(key, stdout)
        self.assertNotIn(key, stderr)
        self.assertNotIn(body.decode("ascii"), stderr)

    def test_32_and_96_pass_through(self):
        public = base64.b64decode(PRODUCTION_PUBLIC_KEY)
        seed = base64.b64encode(bytes(range(32))).decode("ascii")
        rc, stdout, stderr, body = self._run(seed)
        self.assertEqual(rc, 0, stderr)
        self.assertEqual(stdout, "")
        self.assertEqual(base64.b64decode(body), bytes(range(32)))
        self.assertIn("decoded 32 bytes -> seed (32 bytes)", stderr)
        self.assertNotIn(seed, stderr)

        old = base64.b64encode(bytes([0x42]) * 64 + public).decode("ascii")
        rc, stdout, stderr, body = self._run(old)
        self.assertEqual(rc, 0, stderr)
        self.assertEqual(stdout, "")
        self.assertEqual(len(base64.b64decode(body)), 96)
        self.assertIn("decoded 96 bytes -> old-96 (96 bytes)", stderr)
        self.assertNotIn(old, stderr)

    def test_refuses_other_lengths_without_echoing_secret(self):
        key = base64.b64encode(b"too-short").decode("ascii")
        rc, stdout, stderr, body = self._run(key)
        self.assertNotEqual(rc, 0)
        self.assertEqual(stdout, "")
        self.assertIn("unsupported length", stderr)
        self.assertNotIn(key, stderr)
        self.assertEqual(body, b"")


class SignSparkleZipTests(unittest.TestCase):
    def _write_tool(self, path: Path, marker: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!/bin/sh\necho {marker}\n", encoding="utf-8")
        path.chmod(0o755)

    def test_print_tool_prefers_bin_sign_update_over_old_dsa_scripts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old = root / "Sparkle-2.9.6" / "bin" / "old_dsa_scripts" / "sign_update"
            new = root / "Sparkle-2.9.6" / "bin" / "sign_update"
            self._write_tool(old, "old-dsa")
            self._write_tool(new, "ed25519")
            result = subprocess.run(
                [str(SIGN_ZIP), "--print-tool", "--search-root", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(Path(result.stdout.strip()), new)
            self.assertNotIn("old_dsa_scripts", result.stdout)

    def test_print_tool_ignores_old_dsa_scripts_when_it_would_be_find_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # A nested old_dsa_scripts path that naive find -print -quit can hit first.
            old = root / "aaa" / "bin" / "old_dsa_scripts" / "sign_update"
            new = root / "zzz" / "bin" / "sign_update"
            self._write_tool(old, "old-dsa")
            self._write_tool(new, "ed25519")
            result = subprocess.run(
                [str(SIGN_ZIP), "--print-tool", "--search-root", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(Path(result.stdout.strip()), new)

    def test_script_does_not_capture_sign_update_in_command_substitution(self):
        text = SIGN_ZIP.read_text(encoding="utf-8")
        self.assertNotRegex(text, r"\$\(\s*\"\$\{SIGN_UPDATE\}\"")
        self.assertIn("Never capture sign_update in a command substitution", text)
        self.assertIn("normalize-sparkle-ed-key.py", text)

    def test_print_tool_fails_when_only_old_dsa_scripts_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old = root / "Sparkle-2.9.6" / "bin" / "old_dsa_scripts" / "sign_update"
            self._write_tool(old, "old-dsa")
            result = subprocess.run(
                [str(SIGN_ZIP), "--print-tool", "--search-root", str(root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("old_dsa_scripts", result.stderr)


if __name__ == "__main__":
    unittest.main()
