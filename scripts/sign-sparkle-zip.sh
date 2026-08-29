#!/usr/bin/env bash
# Sign a macOS Sparkle zip with Sparkle's sign_update. Never prints the private key.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIP_PATH=""
SEARCH_ROOT=""
SIGNATURE_OUT=""
KEYFILE=""
ERRFILE=""

usage() {
  echo "Usage: $0 --zip <archive.zip> --search-root <dir> --signature-out <file>" >&2
  echo "       $0 --resolve-tool <search-root>" >&2
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
  if [[ -n "${ERRFILE:-}" && -f "${ERRFILE}" ]]; then
    rm -f "${ERRFILE}"
  fi
}

trap cleanup EXIT

# Sparkle-*.tar.xz ships extra files named sign_update:
#   Symbols/sign_update.dSYM/.../DWARF/sign_update  (not executable Mach-O CLI)
#   bin/old_dsa_scripts/sign_update                 (deprecated DSA helper)
# The official tarball lists the DWARF file before bin/sign_update, so
# `find -name sign_update -print -quit` can pick the wrong one and die with
# almost no log output (set -e on a pipeline, or GitHub masking key-bearing
# Swift errors).
find_sparkle_sign_update() {
  local search_root="$1"
  local candidate=""
  local matches=()

  shopt -s nullglob
  matches=("${search_root}"/Sparkle-*/bin/sign_update)
  shopt -u nullglob
  if [[ "${#matches[@]}" -gt 0 && -f "${matches[0]}" ]]; then
    printf '%s\n' "${matches[0]}"
    return 0
  fi

  candidate="$(find "${search_root}" \
    -path '*/bin/sign_update' \
    ! -path '*/old_dsa_scripts/*' \
    ! -path '*.dSYM/*' \
    -type f \
    -print -quit 2>/dev/null || true)"
  if [[ -n "${candidate}" && -f "${candidate}" ]]; then
    printf '%s\n' "${candidate}"
    return 0
  fi
  return 1
}

if [[ "${1:-}" == "--resolve-tool" ]]; then
  if [[ "$#" -ne 2 || -z "${2:-}" ]]; then
    usage
  fi
  find_sparkle_sign_update "$2"
  exit $?
fi

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
    --resolve-tool)
      usage
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

echo "Checking Sparkle EdDSA secret against the embedded SUPublicEDKey." >&2
python3 "${SCRIPT_DIR}/generate-sparkle-appcast.py" --verify-env-key

SIGN_UPDATE="$(find_sparkle_sign_update "${SEARCH_ROOT}" || true)"
if [[ -z "${SIGN_UPDATE}" || ! -f "${SIGN_UPDATE}" ]]; then
  echo "Sparkle bin/sign_update was not found under ${SEARCH_ROOT}." >&2
  exit 1
fi
case "${SIGN_UPDATE}" in
  *.dSYM/*|*/old_dsa_scripts/*)
    echo "Refusing to run non-CLI sign_update at ${SIGN_UPDATE}." >&2
    exit 1
    ;;
esac
chmod +x "${SIGN_UPDATE}"
echo "Using Sparkle sign_update at ${SIGN_UPDATE}" >&2

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

ERRFILE="$(mktemp "${TMPDIR:-/tmp}/sparkle-sign-update.XXXXXX")"
set +e
signature="$("${SIGN_UPDATE}" --ed-key-file "${KEYFILE}" -p "${ZIP_PATH}" 2>"${ERRFILE}")"
status=$?
set -e
signature="$(printf '%s' "${signature}" | tr -d '[:space:]')"
if [[ "${status}" -ne 0 || -z "${signature}" ]]; then
  echo "Sparkle sign_update failed for ${ZIP_PATH} (exit ${status})." >&2
  echo "sign_update path: ${SIGN_UPDATE}" >&2
  if command -v file >/dev/null 2>&1; then
    file "${SIGN_UPDATE}" >&2 || true
  fi
  # sign_update can echo the private key on a decode error. Never replay stderr.
  if grep -Eiq 'cannot execute|exec format|no such file|permission denied|not found|invalid byte|64 bytes or 96 bytes' "${ERRFILE}"; then
    grep -Ei 'cannot execute|exec format|no such file|permission denied|not found|invalid byte|64 bytes or 96 bytes' "${ERRFILE}" >&2 || true
  fi
  echo "If SPARKLE_ED25519_PRIVATE_KEY is set, it must be Sparkle generate_keys material whose public half matches SUPublicEDKey." >&2
  exit 1
fi

mkdir -p "$(dirname "${SIGNATURE_OUT}")"
printf '%s\n' "${signature}" > "${SIGNATURE_OUT}"
echo "Signed ${ZIP_PATH} with Sparkle sign_update." >&2
