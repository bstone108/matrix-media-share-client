#!/usr/bin/env bash
# Sign a macOS Sparkle zip with Sparkle's sign_update. Never prints the private key.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIP_PATH=""
SEARCH_ROOT=""
SIGNATURE_OUT=""
PRINT_TOOL=0
KEYFILE=""
SIGN_LOG=""

usage() {
  echo "Usage: $0 --zip <archive.zip> --search-root <dir> --signature-out <file>" >&2
  echo "       $0 --print-tool --search-root <dir>" >&2
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
  if [[ -n "${SIGN_LOG:-}" && -f "${SIGN_LOG}" ]]; then
    rm -f "${SIGN_LOG}"
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
    --print-tool)
      PRINT_TOOL=1
      shift
      ;;
    *)
      usage
      ;;
  esac
done

if [[ -z "${SEARCH_ROOT}" ]]; then
  usage
fi
if [[ "${PRINT_TOOL}" -eq 0 && ( -z "${ZIP_PATH}" || -z "${SIGNATURE_OUT}" ) ]]; then
  usage
fi

resolve_sign_update() {
  local root="$1"
  local candidate=""
  local found=""

  shopt -s nullglob
  local preferred=(
    "${root}/Sparkle-"*/bin/sign_update
    "${root}"/*/bin/sign_update
    "${root}/bin/sign_update"
  )
  shopt -u nullglob
  for candidate in "${preferred[@]}"; do
    if [[ -f "${candidate}" && "${candidate}" != *"/old_dsa_scripts/"* ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done

  # Never pick bin/old_dsa_scripts/sign_update. Prefer Sparkle's bin/sign_update.
  found="$(
    find "${root}" \
      \( -path '*/old_dsa_scripts' -o -path '*/old_dsa_scripts/*' \) -prune -o \
      -type f -name sign_update -path '*/bin/sign_update' -print \
      2>/dev/null | head -n 1 || true
  )"
  if [[ -n "${found}" && -f "${found}" ]]; then
    printf '%s\n' "${found}"
    return 0
  fi
  return 1
}

SIGN_UPDATE="$(resolve_sign_update "${SEARCH_ROOT}" || true)"
if [[ -z "${SIGN_UPDATE}" || ! -f "${SIGN_UPDATE}" ]]; then
  echo "Sparkle sign_update was not found under ${SEARCH_ROOT} (ignored old_dsa_scripts)." >&2
  exit 1
fi
if [[ "${SIGN_UPDATE}" == *"/old_dsa_scripts/"* ]]; then
  echo "Refusing Sparkle DSA sign_update at ${SIGN_UPDATE}; expected bin/sign_update." >&2
  exit 1
fi

if [[ "${PRINT_TOOL}" -eq 1 ]]; then
  printf '%s\n' "${SIGN_UPDATE}"
  exit 0
fi

if [[ ! -f "${ZIP_PATH}" ]]; then
  echo "Sparkle zip not found: ${ZIP_PATH}" >&2
  exit 1
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Sparkle sign_update must run on macOS." >&2
  exit 1
fi

chmod +x "${SIGN_UPDATE}"
if command -v xattr >/dev/null 2>&1; then
  sign_bin_dir="$(dirname "${SIGN_UPDATE}")"
  xattr -dr com.apple.quarantine "${sign_bin_dir}" 2>/dev/null || true
  xattr -d com.apple.quarantine "${SIGN_UPDATE}" 2>/dev/null || true
fi
echo "Using Sparkle sign_update at ${SIGN_UPDATE}" >&2

PUBLIC_KEY="t9iMCbw9VgjkeAVRokGcvFWD2dZFyU852Um2/7SwRfc="

# Do not print the private key. Disable xtrace around any expansion of it.
set +x
python3 "${SCRIPT_DIR}/generate-sparkle-appcast.py" --verify-env-key
KEYFILE="$(mktemp "${TMPDIR:-/tmp}/sparkle-ed25519.XXXXXX")"
chmod 600 "${KEYFILE}"
# Sparkle 2.9.6 sign_update rejects 64-byte decoded secrets:
# "Imported key must be 64 bytes or 96 bytes ... Instead it is 64 bytes decoded."
# That error is wrong; decodePrivateAndPublicKeys only accepts 32 or 96 bytes.
# Reshape 64-byte secrets (never print them) before invoking sign_update.
set +e
python3 -c 'import os,sys; sys.stdout.write("".join(os.environ.get("SPARKLE_ED25519_PRIVATE_KEY","").split()))' \
  | python3 "${SCRIPT_DIR}/normalize-sparkle-ed-key.py" "${PUBLIC_KEY}" "${KEYFILE}"
norm_rc=$?
set -euo pipefail
if [[ "${norm_rc}" -ne 0 || ! -s "${KEYFILE}" ]]; then
  echo "failed to normalize Sparkle EdDSA secret for sign_update" >&2
  exit 1
fi

redact_and_print() {
  local raw="$1"
  # Redact in Python so base64 '/' in the secret cannot break bash substitution.
  python3 - "${raw}" "${KEYFILE}" <<'PY'
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
key = os.environ.get("SPARKLE_ED25519_PRIVATE_KEY", "")
fragments = {key, key.strip(), "".join(key.split())}
norm_path = Path(sys.argv[2]) if len(sys.argv) > 2 else None
if norm_path is not None and norm_path.is_file():
    fragments.add(norm_path.read_text(encoding="utf-8", errors="replace").strip())
for fragment in fragments:
    if fragment:
        text = text.replace(fragment, "[redacted]")
if text:
    sys.stdout.write(text)
    if not text.endswith("\n"):
        sys.stdout.write("\n")
PY
}

parse_signature_line() {
  local raw="$1"
  ed_signature=""
  if [[ -s "${raw}" ]]; then
    ed_signature="$(sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p' "${raw}" | tail -n 1 || true)"
  fi
  if [[ -z "${ed_signature}" && -s "${raw}" ]]; then
    ed_signature="$(grep -E '^[A-Za-z0-9+=/]{80,}$' "${raw}" | tail -n 1 || true)"
  fi
}

dump_sign_update_diagnostics() {
  local rc="$1"
  echo "sign_update failed (exit ${rc}) at ${SIGN_UPDATE}" >&2
  if command -v file >/dev/null 2>&1; then
    file "${SIGN_UPDATE}" >&2 || true
  fi
  if command -v xattr >/dev/null 2>&1; then
    xattr -l "${SIGN_UPDATE}" >&2 || true
  fi
  echo "sign_update --help:" >&2
  "${SIGN_UPDATE}" --help >&2 || true
}

stdin_rejected() {
  local raw="$1"
  grep -Eiq 'standard input|stdin|unable to read EdDSA private key from standard input' "${raw}" 2>/dev/null
}

# Never capture sign_update in a command substitution: Sparkle prints errors with
# print() (stdout), and a non-zero exit would discard that output.
ed_signature=""
SIGN_LOG="$(mktemp "${TMPDIR:-/tmp}/sparkle-sign.XXXXXX")"
invoke_sign_update() {
  local method="$1"
  : >"${SIGN_LOG}"
  set +x
  set +e
  case "${method}" in
    stdin)
      "${SIGN_UPDATE}" --ed-key-file - "${ZIP_PATH}" <"${KEYFILE}" >"${SIGN_LOG}" 2>&1
      ;;
    file)
      "${SIGN_UPDATE}" --ed-key-file "${KEYFILE}" "${ZIP_PATH}" >"${SIGN_LOG}" 2>&1
      ;;
    *)
      set -euo pipefail
      echo "internal error: unknown sign_update method ${method}" >&2
      return 1
      ;;
  esac
  local rc=$?
  set -euo pipefail
  echo "sign_update (${method}) exit ${rc}" >&2
  redact_and_print "${SIGN_LOG}" >&2
  return "${rc}"
}

sign_rc=0
invoke_sign_update stdin || sign_rc=$?
parse_signature_line "${SIGN_LOG}"

if [[ -z "${ed_signature}" || "${sign_rc}" -ne 0 ]]; then
  if stdin_rejected "${SIGN_LOG}" || [[ "${sign_rc}" -ne 0 && -z "${ed_signature}" ]]; then
    echo "stdin sign_update did not produce a signature (exit ${sign_rc}); trying --ed-key-file temp file fallback" >&2
    sign_rc=0
    invoke_sign_update file || sign_rc=$?
    parse_signature_line "${SIGN_LOG}"
  fi
fi

if [[ "${sign_rc}" -ne 0 || -z "${ed_signature}" ]]; then
  dump_sign_update_diagnostics "${sign_rc}"
  echo "Sparkle sign_update produced no signature." >&2
  exit 1
fi

mkdir -p "$(dirname "${SIGNATURE_OUT}")"
printf '%s\n' "${ed_signature}" > "${SIGNATURE_OUT}"
echo "Signed ${ZIP_PATH} with Sparkle sign_update." >&2
