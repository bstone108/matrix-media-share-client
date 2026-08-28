#!/usr/bin/env bash
# Sign a macOS Sparkle zip with Sparkle's sign_update. Never prints the private key.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIP_PATH=""
SEARCH_ROOT=""
SIGNATURE_OUT=""
KEYFILE=""

usage() {
  echo "Usage: $0 --zip <archive.zip> --search-root <dir> --signature-out <file>" >&2
  exit 2
}

cleanup() {
  if [[ -n "${KEYFILE:-}" && -f "${KEYFILE}" ]]; then
    if rm -P "${KEYFILE}" 2>/dev/null; then
      :
    else
      rm -f "${KEYFILE}"
    fi
  fi
}

trap cleanup EXIT

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --zip)
      ZIP_PATH="${2:-}"
      shift 2
      ;;
    --search-root)
      SEARCH_ROOT="${2:-}"
      shift 2
      ;;
    --signature-out)
      SIGNATURE_OUT="${2:-}"
      shift 2
      ;;
    *)
      usage
      ;;
  esac
done

if [[ -z "${ZIP_PATH}" || -z "${SEARCH_ROOT}" || -z "${SIGNATURE_OUT}" ]]; then
  usage
fi
if [[ ! -f "${ZIP_PATH}" ]]; then
  echo "Sparkle zip not found: ${ZIP_PATH}" >&2
  exit 1
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Sparkle sign_update must run on macOS." >&2
  exit 1
fi

python3 "${SCRIPT_DIR}/generate-sparkle-appcast.py" --verify-env-key

SIGN_UPDATE="$(find "${SEARCH_ROOT}" -name sign_update -type f -print -quit || true)"
if [[ -z "${SIGN_UPDATE}" || ! -f "${SIGN_UPDATE}" ]]; then
  echo "Sparkle sign_update was not found under ${SEARCH_ROOT}." >&2
  exit 1
fi
chmod +x "${SIGN_UPDATE}"

KEYFILE="$(mktemp "${TMPDIR:-/tmp}/sparkle-ed25519.XXXXXX")"
chmod 600 "${KEYFILE}"
python3 - "${KEYFILE}" <<'PY'
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
key = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY", "").strip()
if not key:
    raise SystemExit("SPARKLE_ED25519_PRIVATE_KEY is required to sign Sparkle appcasts.")
path.write_text(key + "\n", encoding="utf-8")
os.chmod(path, 0o600)
PY

signature="$("${SIGN_UPDATE}" --ed-key-file "${KEYFILE}" -p "${ZIP_PATH}" | tr -d '[:space:]')"
if [[ -z "${signature}" ]]; then
  echo "Sparkle sign_update produced no signature." >&2
  exit 1
fi

mkdir -p "$(dirname "${SIGNATURE_OUT}")"
printf '%s\n' "${signature}" > "${SIGNATURE_OUT}"
echo "Signed ${ZIP_PATH} with Sparkle sign_update." >&2
