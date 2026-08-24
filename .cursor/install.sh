#!/usr/bin/env bash
#
# Cloud Agent install script for Matrix Media Share Client.
#
# Prepares a full desktop-build toolchain (Qt 6, Rust, Go, VLC/FFmpeg runtime)
# and then compiles the Qt app + Rust backend once so the build caches are warm.
# It is idempotent: re-running it re-uses already installed toolchains and only
# refreshes what is missing.
#
# Toolchain versions are pinned to match .github/workflows/desktop-ci.yml so the
# environment reproduces the release build.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

QT_VERSION="6.10.2"
GO_VERSION="1.25.0"
QT_BASE_DIR="/opt/qt"
QT_ROOT_DIR="${QT_BASE_DIR}/${QT_VERSION}/gcc_64"
VLC_ROOT="/opt/vlc-runtime"
AQT_VENV="/opt/aqt-venv"

if [[ "$(id -u)" -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

log() { printf '\n=== %s ===\n' "$*"; }

case "$(uname -m)" in
  x86_64|amd64)
    GO_ARCH="amd64"
    QT_HOST="linux"
    QT_TARGET_ARCH="linux_gcc_64"
    QT_INSTALL_SUBDIR="gcc_64"
    ;;
  aarch64|arm64)
    GO_ARCH="arm64"
    QT_HOST="linux_arm64"
    QT_TARGET_ARCH="linux_gcc_arm64"
    QT_INSTALL_SUBDIR="gcc_arm64"
    ;;
  *)
    echo "Unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac
QT_ROOT_DIR="${QT_BASE_DIR}/${QT_VERSION}/${QT_INSTALL_SUBDIR}"

# ---------------------------------------------------------------------------
log "Installing system build dependencies (apt)"
# ---------------------------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
${SUDO} apt-get update
${SUDO} apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  desktop-file-utils \
  git \
  ninja-build \
  p7zip-full \
  patchelf \
  pkg-config \
  python3 \
  python3-venv \
  zip \
  libgl1-mesa-dev \
  libpulse-dev \
  libpulse0 \
  libgstreamer-gl1.0-0 \
  libgstreamer-plugins-base1.0-0 \
  libgstreamer-plugins-bad1.0-0 \
  libgstreamer-plugins-good1.0-0 \
  libgstreamer1.0-0 \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-good \
  libxcb-cursor0 \
  libxcb-icccm4 \
  libxcb-image0 \
  libxcb-keysyms1 \
  libxcb-randr0 \
  libxcb-render-util0 \
  libxcb-shape0 \
  libxcb-util1 \
  libxcb-xfixes0 \
  libxcb-xkb1 \
  libxkbcommon-x11-0 \
  libxrender1 \
  libfontconfig1 \
  libdbus-1-3 \
  libvlc-bin \
  libvlc5 \
  vlc-plugin-base \
  xvfb \
  x11-utils \
  x11-xserver-utils \
  xauth

# ---------------------------------------------------------------------------
log "Installing Go ${GO_VERSION}"
# ---------------------------------------------------------------------------
if [[ "$(/usr/local/go/bin/go version 2>/dev/null || true)" != *"go${GO_VERSION} "* ]]; then
  tmp_go="$(mktemp -d)"
  curl -fL --retry 3 --retry-all-errors \
    -o "${tmp_go}/go.tar.gz" \
    "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz"
  ${SUDO} rm -rf /usr/local/go
  ${SUDO} tar -C /usr/local -xzf "${tmp_go}/go.tar.gz"
  rm -rf "${tmp_go}"
fi
${SUDO} ln -sf /usr/local/go/bin/go /usr/local/bin/go
${SUDO} ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt
/usr/local/go/bin/go version

# ---------------------------------------------------------------------------
log "Ensuring Rust toolchain from backend/rust-toolchain.toml"
# ---------------------------------------------------------------------------
RUST_CHANNEL="$(sed -n 's/^\s*channel\s*=\s*"\([^"]*\)".*/\1/p' backend/rust-toolchain.toml | head -n1)"
RUST_CHANNEL="${RUST_CHANNEL:-1.93.0}"
if command -v rustup >/dev/null 2>&1; then
  rustup toolchain install "${RUST_CHANNEL}" --profile minimal --no-self-update || \
    rustup toolchain install "${RUST_CHANNEL}" --profile minimal
else
  echo "rustup not found on PATH; expected the base image to provide it." >&2
  exit 1
fi
( cd backend && cargo --version )

# ---------------------------------------------------------------------------
log "Installing Qt ${QT_VERSION} via aqtinstall"
# ---------------------------------------------------------------------------
if [[ ! -x "${QT_ROOT_DIR}/bin/qmake6" ]]; then
  ${SUDO} mkdir -p "${QT_BASE_DIR}"
  ${SUDO} chown "$(id -u):$(id -g)" "${QT_BASE_DIR}"
  if [[ ! -x "${AQT_VENV}/bin/python" ]]; then
    ${SUDO} python3 -m venv "${AQT_VENV}"
    ${SUDO} chown -R "$(id -u):$(id -g)" "${AQT_VENV}"
  fi
  "${AQT_VENV}/bin/pip" install --upgrade pip
  "${AQT_VENV}/bin/pip" install "aqtinstall==3.3.*" "py7zr==1.0.*"
  "${AQT_VENV}/bin/python" -m aqt install-qt \
    --outputdir "${QT_BASE_DIR}" \
    "${QT_HOST}" desktop "${QT_VERSION}" "${QT_TARGET_ARCH}" \
    --modules qtmultimedia
fi
"${QT_ROOT_DIR}/bin/qmake6" -query QT_VERSION

# ---------------------------------------------------------------------------
log "Normalizing bundled VLC runtime into ${VLC_ROOT}"
# ---------------------------------------------------------------------------
MULTIARCH="$(gcc -dumpmachine)"
${SUDO} rm -rf "${VLC_ROOT}"
${SUDO} mkdir -p "${VLC_ROOT}/lib" "${VLC_ROOT}/plugins"
${SUDO} chown -R "$(id -u):$(id -g)" "${VLC_ROOT}"
cp -a "/usr/lib/${MULTIARCH}/libvlc.so"* "${VLC_ROOT}/lib/"
cp -a "/usr/lib/${MULTIARCH}/libvlccore.so"* "${VLC_ROOT}/lib/"
cp -a "/usr/lib/${MULTIARCH}/vlc/plugins/." "${VLC_ROOT}/plugins/"

# ---------------------------------------------------------------------------
log "Writing environment profile (/etc/profile.d)"
# ---------------------------------------------------------------------------
# Expose the toolchains and build defaults to every login shell. The GNU
# compilers are pinned because the default /usr/bin/c++ (clang) cannot locate
# libstdc++ on this image, while gcc/g++ match the CI build.
PROFILE_FILE="/etc/profile.d/matrix-media-share-client-env.sh"
${SUDO} tee "${PROFILE_FILE}" >/dev/null <<EOF
# Managed by .cursor/install.sh for Matrix Media Share Client
export QT_ROOT_DIR="${QT_ROOT_DIR}"
export MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT="${VLC_ROOT}"
export CC=gcc
export CXX=g++
case ":\${CMAKE_PREFIX_PATH}:" in
  *":${QT_ROOT_DIR}:"*) ;;
  *) export CMAKE_PREFIX_PATH="${QT_ROOT_DIR}\${CMAKE_PREFIX_PATH:+:\${CMAKE_PREFIX_PATH}}" ;;
esac
case ":\${PATH}:" in
  *":${QT_ROOT_DIR}/bin:"*) ;;
  *) export PATH="${QT_ROOT_DIR}/bin:/usr/local/go/bin:\${PATH}" ;;
esac
EOF
# shellcheck disable=SC1090
source "${PROFILE_FILE}"

# ---------------------------------------------------------------------------
log "Building Qt app + Rust backend (warms cargo/Go/FFmpeg caches)"
# ---------------------------------------------------------------------------
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_PREFIX_PATH="${QT_ROOT_DIR}" \
  -DMATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT="${VLC_ROOT}"
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure

log "Install complete"
