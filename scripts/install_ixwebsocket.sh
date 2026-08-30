#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${TMPDIR:-/tmp}/cm5audio-ixwebsocket-build"
VERSION="${IXWEBSOCKET_VERSION:-v12.0.1}"
PREFIX="${IXWEBSOCKET_PREFIX:-/usr/local}"

if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || {
        echo "sudo is required to install ixwebsocket into $PREFIX." >&2
        exit 1
    }
    SUDO="sudo"
fi

command -v git >/dev/null 2>&1 || { echo "Missing prerequisite: git" >&2; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "Missing prerequisite: cmake" >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Missing prerequisite: make" >&2; exit 1; }

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
git clone --depth 1 --branch "$VERSION" \
    https://github.com/machinezone/IXWebSocket.git \
    "$WORK_DIR/source"

cmake -S "$WORK_DIR/source" -B "$WORK_DIR/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DUSE_TLS=ON \
    -DUSE_OPEN_SSL=ON \
    -DUSE_ZLIB=ON \
    -DIXWEBSOCKET_INSTALL=ON
cmake --build "$WORK_DIR/build" --parallel
$SUDO cmake --install "$WORK_DIR/build"

# Refresh the dynamic linker cache when installing under a system prefix.
if command -v ldconfig >/dev/null 2>&1; then
    $SUDO ldconfig
fi

if pkg-config --exists ixwebsocket; then
    echo "Installed ixwebsocket $(pkg-config --modversion ixwebsocket) with TLS support."
else
    echo "ixwebsocket was installed, but pkg-config cannot find it." >&2
    echo "Ensure $PREFIX/lib/pkgconfig is in PKG_CONFIG_PATH." >&2
    exit 1
fi
