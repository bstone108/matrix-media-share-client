#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="${ROOT_DIR}/.work/macos"
BUILD_DIR="${BUILD_DIR:-${WORK_DIR}/build-release}"
STAGE_DIR="${STAGE_DIR:-${WORK_DIR}/stage-release}"
BUILDS_DIR="${BUILDS_DIR:-${ROOT_DIR}/builds}"
VERSION_FILE="${ROOT_DIR}/VERSION.txt"
APP_NAME="MatrixMediaShareClientQt"
ARCH="$(uname -m)"
ENTITLEMENTS_FILE="${ROOT_DIR}/packaging/macos/MatrixMediaShareClientQt.entitlements"
# Hardened-runtime entitlements are only for JIT/plugin loading. The app is not
# sandboxed, so network/camera/mic sandbox entitlements are omitted: playback
# uses QMediaPlayer, VLC, and WKWebView <video>, with no capture APIs.
CODESIGN_IDENTITY="${MACOS_CODESIGN_IDENTITY:-}"
NOTARY_TIMEOUT_SECONDS="${MACOS_NOTARY_TIMEOUT_SECONDS:-1800}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This packaging script must be run on macOS." >&2
  exit 1
fi

if [[ ! -f "${VERSION_FILE}" ]]; then
  echo "Version file not found: ${VERSION_FILE}" >&2
  exit 1
fi

APP_VERSION="$(tr -d '\r\n' < "${VERSION_FILE}")"

if [[ -z "${QT_PREFIX:-}" ]]; then
  if [[ -n "${QT_ROOT_DIR:-}" ]]; then
    QT_PREFIX="${QT_ROOT_DIR}"
  elif command -v qmake6 >/dev/null 2>&1; then
    QT_PREFIX="$(qmake6 -query QT_INSTALL_PREFIX)"
  elif command -v qmake >/dev/null 2>&1; then
    QT_PREFIX="$(qmake -query QT_INSTALL_PREFIX)"
  elif command -v macdeployqt >/dev/null 2>&1; then
    QT_PREFIX="$(cd "$(dirname "$(command -v macdeployqt)")/.." && pwd)"
  else
    QT_PREFIX="/opt/homebrew/opt/qt"
  fi
fi

if [[ -z "${MACDEPLOYQT_BIN:-}" ]]; then
  if [[ -n "${QT_ROOT_DIR:-}" && -x "${QT_ROOT_DIR}/bin/macdeployqt" ]]; then
    MACDEPLOYQT_BIN="${QT_ROOT_DIR}/bin/macdeployqt"
  elif command -v macdeployqt >/dev/null 2>&1; then
    MACDEPLOYQT_BIN="$(command -v macdeployqt)"
  else
    MACDEPLOYQT_BIN="${QT_PREFIX}/bin/macdeployqt"
  fi
fi

if [[ ! -x "${MACDEPLOYQT_BIN}" ]]; then
  echo "macdeployqt not found at ${MACDEPLOYQT_BIN}" >&2
  exit 1
fi

restore_sql_drivers() {
  if [[ -z "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR:-}" ]]; then
    return
  fi

  shopt -s nullglob
  for plugin_path in "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR}"/*; do
    mv "${plugin_path}" "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_DIR}/"
  done
  shopt -u nullglob
  rmdir "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR}" 2>/dev/null || true
}

using_developer_id() {
  [[ -n "${CODESIGN_IDENTITY}" && "${CODESIGN_IDENTITY}" != "-" ]]
}

list_macho_files() {
  local root="$1"
  python3 - "${root}" <<'PY'
import os
import sys

root = sys.argv[1]
magics = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
}
files = []
for dirpath, _, filenames in os.walk(root):
    for name in filenames:
        path = os.path.join(dirpath, name)
        try:
            with open(path, "rb") as handle:
                magic = handle.read(4)
        except OSError:
            continue
        if magic in magics:
            files.append(path)
files.sort(key=lambda path: path.count(os.sep), reverse=True)
for path in files:
    print(path)
PY
}

item_wants_entitlements() {
  local item="$1"
  if [[ -d "${item}" && "${item}" == *.app ]]; then
    return 0
  fi
  # Entitlements apply to processes, not libraries. Nested Qt/VLC dylibs and
  # framework binaries only need a hardened-runtime signature.
  if [[ "${item}" == *.dylib || "${item}" == *.so || "${item}" == *.framework/* ]]; then
    return 1
  fi
  return 0
}

sign_item() {
  local item="$1"
  if using_developer_id; then
    local args=(
      --force
      --options runtime
      --timestamp
      --sign "${CODESIGN_IDENTITY}"
    )
    if item_wants_entitlements "${item}" && [[ -f "${ENTITLEMENTS_FILE}" ]]; then
      args+=(--entitlements "${ENTITLEMENTS_FILE}")
    fi
    echo "Signing (Developer ID) ${item}"
    codesign "${args[@]}" "${item}"
  else
    echo "Signing (ad-hoc) ${item}"
    codesign --force --sign - "${item}"
  fi
}

sign_app_bundle() {
  local app_bundle="$1"

  if [[ ! -d "${app_bundle}" ]]; then
    echo "App bundle not found: ${app_bundle}" >&2
    exit 1
  fi

  xattr -cr "${app_bundle}"

  if using_developer_id; then
    if [[ ! -f "${ENTITLEMENTS_FILE}" ]]; then
      echo "Entitlements file not found: ${ENTITLEMENTS_FILE}" >&2
      exit 1
    fi
    if ! security find-identity -v -p codesigning | grep -F "${CODESIGN_IDENTITY}" >/dev/null; then
      echo "Signing identity not found in keychain: ${CODESIGN_IDENTITY}" >&2
      security find-identity -v -p codesigning >&2 || true
      exit 1
    fi

    while IFS= read -r macho_path; do
      [[ -z "${macho_path}" ]] && continue
      sign_item "${macho_path}"
    done < <(list_macho_files "${app_bundle}")

    # Sign the bundle last so the sealed resources include nested signatures.
    sign_item "${app_bundle}"
  else
    # Local/dev fallback: keep the previous ad-hoc deep sign.
    codesign --force --deep --sign - "${app_bundle}"
  fi

  codesign --verify --deep --strict --verbose=2 "${app_bundle}"

  if using_developer_id; then
    local signature_info
    signature_info="$(codesign --display --verbose=2 "${app_bundle}" 2>&1)"
    echo "${signature_info}"
    if grep -q "Signature=adhoc" <<<"${signature_info}"; then
      echo "Developer ID signing produced an ad-hoc signature." >&2
      exit 1
    fi
    if ! grep -Eq "flags=.*runtime" <<<"${signature_info}"; then
      echo "Hardened runtime is missing from the app signature." >&2
      exit 1
    fi
  fi
}

zip_app_bundle() {
  local app_bundle="$1"
  local archive_path="$2"
  rm -f "${archive_path}"
  ditto -c -k --sequesterRsrc --keepParent "${app_bundle}" "${archive_path}"
}

create_signed_dmg() {
  local app_bundle="$1"
  local dmg_path="$2"
  local dmg_stage="${WORK_DIR}/dmg-stage"

  rm -rf "${dmg_stage}" "${dmg_path}"
  mkdir -p "${dmg_stage}"
  cp -R "${app_bundle}" "${dmg_stage}/"
  ln -s /Applications "${dmg_stage}/Applications"

  hdiutil create \
    -volname "Matrix Media Share Client" \
    -srcfolder "${dmg_stage}" \
    -ov \
    -format UDZO \
    -fs HFS+ \
    "${dmg_path}"

  rm -rf "${dmg_stage}"

  if using_developer_id; then
    echo "Signing (Developer ID) ${dmg_path}"
    codesign --force --timestamp --sign "${CODESIGN_IDENTITY}" "${dmg_path}"
    codesign --verify --verbose=2 "${dmg_path}"
  fi
}

parse_notary_field() {
  local json="$1"
  local field="$2"
  python3 - "${json}" "${field}" <<'PY'
import json
import sys

text = sys.argv[1].strip()
field = sys.argv[2]
obj = None
try:
    obj = json.loads(text)
except json.JSONDecodeError:
    start = text.rfind("{")
    end = text.rfind("}")
    if start == -1 or end == -1 or end < start:
        raise
    obj = json.loads(text[start:end + 1])
if isinstance(obj, list):
    obj = obj[-1]
print(obj.get(field, ""))
PY
}

notarize_artifact() {
  local artifact="$1"
  local label="$2"

  echo "Submitting ${label} for notarization: ${artifact}"
  local output
  if ! output="$(xcrun notarytool submit "${artifact}" \
    --apple-id "${APPLE_ID}" \
    --password "${APPLE_APP_SPECIFIC_PASSWORD}" \
    --team-id "${APPLE_TEAM_ID}" \
    --wait \
    --timeout "${NOTARY_TIMEOUT_SECONDS}" \
    --output-format json)"; then
    echo "notarytool submit failed for ${label}:" >&2
    echo "${output}" >&2
    exit 1
  fi

  echo "${output}"
  local status submission_id
  status="$(parse_notary_field "${output}" status)"
  submission_id="$(parse_notary_field "${output}" id)"
  if [[ "${status}" != "Accepted" ]]; then
    echo "Notarization of ${label} was not accepted (status=${status})." >&2
    if [[ -n "${submission_id}" ]]; then
      xcrun notarytool log "${submission_id}" \
        --apple-id "${APPLE_ID}" \
        --password "${APPLE_APP_SPECIFIC_PASSWORD}" \
        --team-id "${APPLE_TEAM_ID}" >&2 || true
    fi
    exit 1
  fi
}

should_notarize() {
  if [[ "${MACOS_REQUIRE_NOTARIZATION:-0}" == "1" ]]; then
    if ! using_developer_id; then
      echo "MACOS_REQUIRE_NOTARIZATION=1 requires MACOS_CODESIGN_IDENTITY." >&2
      exit 1
    fi
    : "${APPLE_ID:?APPLE_ID is required for notarization}"
    : "${APPLE_APP_SPECIFIC_PASSWORD:?APPLE_APP_SPECIFIC_PASSWORD is required for notarization}"
    : "${APPLE_TEAM_ID:?APPLE_TEAM_ID is required for notarization}"
    return 0
  fi

  using_developer_id \
    && [[ -n "${APPLE_ID:-}" ]] \
    && [[ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ]] \
    && [[ -n "${APPLE_TEAM_ID:-}" ]]
}

ARCHIVE_PATH="${BUILDS_DIR}/${APP_NAME}-${APP_VERSION}-macos-${ARCH}.zip"
DMG_PATH="${BUILDS_DIR}/${APP_NAME}-${APP_VERSION}-macos-${ARCH}.dmg"
NOTARY_ZIP_PATH="${WORK_DIR}/${APP_NAME}-${APP_VERSION}-macos-${ARCH}-notary.zip"
MATRIX_MEDIA_ARCHIVER_SQLDRIVER_DIR="${QT_PREFIX}/plugins/sqldrivers"
MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR=""

mkdir -p "${WORK_DIR}" "${BUILDS_DIR}"
rm -rf "${BUILD_DIR}" "${STAGE_DIR}"

cmake_args=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_PREFIX_PATH="${QT_PREFIX}"
  -DMATRIX_MEDIA_ARCHIVER_BUILD_TESTS=OFF
)

if [[ -n "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT:-}" && -d "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}" ]]; then
  cmake_args+=("-DMATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT}")
fi
if [[ -n "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT:-}" && -d "${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT}" ]]; then
  cmake_args+=("-DMATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT=${MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_FFMPEG_ROOT}")
fi

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --config Release

APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"
if [[ ! -d "${APP_BUNDLE}" ]]; then
  echo "Built app bundle not found: ${APP_BUNDLE}" >&2
  exit 1
fi

mkdir -p "${STAGE_DIR}"
cp -R "${APP_BUNDLE}" "${STAGE_DIR}/"
STAGED_APP="${STAGE_DIR}/${APP_NAME}.app"

if [[ -d "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_DIR}" ]]; then
  MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR="$(mktemp -d "${WORK_DIR}/sqldrivers.XXXXXX")"
  trap restore_sql_drivers EXIT
  shopt -s nullglob
  for plugin_path in "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_DIR}"/libqsql*.dylib; do
    plugin_name="$(basename "${plugin_path}")"
    if [[ "${plugin_name}" != "libqsqlite.dylib" ]]; then
      mv "${plugin_path}" "${MATRIX_MEDIA_ARCHIVER_SQLDRIVER_STASH_DIR}/"
    fi
  done
  shopt -u nullglob
fi

"${MACDEPLOYQT_BIN}" "${STAGED_APP}" -always-overwrite

# Clear Finder/resource metadata before signing. Leftover xattrs can produce a
# malformed signature that Finder refuses to launch on Apple Silicon.
sign_app_bundle "${STAGED_APP}"

if should_notarize; then
  zip_app_bundle "${STAGED_APP}" "${NOTARY_ZIP_PATH}"
  notarize_artifact "${NOTARY_ZIP_PATH}" "app zip"
  rm -f "${NOTARY_ZIP_PATH}"
  xcrun stapler staple "${STAGED_APP}"
  xcrun stapler validate "${STAGED_APP}"
fi

zip_app_bundle "${STAGED_APP}" "${ARCHIVE_PATH}"
create_signed_dmg "${STAGED_APP}" "${DMG_PATH}"

if should_notarize; then
  notarize_artifact "${DMG_PATH}" "disk image"
  xcrun stapler staple "${DMG_PATH}"
  xcrun stapler validate "${DMG_PATH}"
fi

echo "Created ${ARCHIVE_PATH}"
echo "Created ${DMG_PATH}"
