#!/bin/bash
# Configure and build qemu-system-aarch64 for the Sidecar emulator.
# usage: sidecar/build.sh [build-dir]   (default: build/ in the repo root)
# Needs: gcc or clang, ninja, python3 (with venv), pkg-config, glib-2.0 and
# libslirp development files. Debian/Ubuntu/WSL:
#   sudo apt install build-essential ninja-build python3-venv pkg-config libglib2.0-dev libslirp-dev
set -euo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$SRC/build}
mkdir -p "$B"
cd "$B"
if [ ! -f build.ninja ]; then
  "$SRC/configure" --target-list=aarch64-softmmu \
    --without-default-features --without-default-devices \
    --with-devices-aarch64=sidecar \
    --enable-slirp --enable-tcg \
    --disable-docs --disable-tools --disable-user --disable-werror
fi
ninja qemu-system-aarch64
echo "built $B/qemu-system-aarch64"
