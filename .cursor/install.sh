#!/usr/bin/env bash
# Cloud Agent bootstrap for VCMI (Linux / Ubuntu).
# Idempotent: safe to re-run. Installs system dependencies, fetches the
# submodules required for a native Linux build, provisions onnxruntime for
# the MMAI module, then configures and builds the project.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

export DEBIAN_FRONTEND=noninteractive

echo "==> Installing system packages"
sudo apt-get -yq -o Acquire::Retries=3 update
sudo apt-get -yq --no-install-recommends -o Acquire::Retries=3 -o Dpkg::Use-Pty=0 install \
  cmake ninja-build ccache g++ clang git wget p7zip-full \
  libboost-dev libboost-filesystem-dev libboost-date-time-dev \
  libboost-program-options-dev libboost-iostreams-dev \
  libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev \
  qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools \
  qt6-l10n-tools qt6-svg-dev \
  zlib1g-dev libavformat-dev libswscale-dev libtbb-dev \
  libluajit-5.1-dev libminizip-dev libsqlite3-dev \
  libsquish-dev libfmt-dev gettext liblzma-dev

echo "==> Fetching required git submodules"
git submodule update --init --recursive test/googletest launcher/lib/innoextract

echo "==> Provisioning onnxruntime (required by the MMAI module)"
ONNXRUNTIME_ROOT=/opt/onnxruntime
if [ ! -f "$ONNXRUNTIME_ROOT/lib/libonnxruntime.so" ]; then
  sudo bash CI/before_install/linux_onnxruntime.sh
fi
echo "$ONNXRUNTIME_ROOT/lib" | sudo tee /etc/ld.so.conf.d/onnxruntime.conf >/dev/null
sudo ldconfig

echo "==> Configuring and building (linux-gcc-test preset)"
cmake -DENABLE_CCACHE:BOOL=ON -DENABLE_DISCORD:BOOL=OFF --preset linux-gcc-test
cmake --build --preset linux-gcc-test

echo "==> VCMI build complete. Binaries are in out/build/linux-gcc-test/bin"
