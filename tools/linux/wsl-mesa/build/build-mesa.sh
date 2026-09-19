#!/usr/bin/env bash
# build-mesa.sh — Build Mesa from upstream source with the d3d12 gallium
# and microsoft-experimental Vulkan drivers enabled.
#
# Purpose: produce userspace GPU drivers that talk /dev/dxg directly
# (Mesa's own dxcore-equivalent embedded in the d3d12 + dzn drivers)
# so an Ubuntu VM with Hyper-V GPU-PV gets hardware OpenGL/Vulkan
# acceleration without needing Microsoft's proprietary libdxcore.so.
#
# Output: installs to /opt/wsl-mesa/. The caller is expected to tar that
# tree and stash it under tools/wsl-mesa/prebuilt/<ubuntu-codename>-amd64/.
#
# Tested on: Ubuntu 26.04 LTS (resolute), amd64, LLVM 21, Mesa 25.3.x.

set -euo pipefail

MESA_BRANCH="${MESA_BRANCH:-25.3}"
MESA_SRC="${MESA_SRC:-$HOME/mesa}"
PREFIX="${PREFIX:-/opt/wsl-mesa}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="${PATCH_DIR:-$SCRIPT_DIR/../patches}"
TARGET_CODENAME="${TARGET_CODENAME:-resolute}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$SCRIPT_DIR/../prebuilt/ubuntu-26.04-amd64}"
ARTIFACT_PATH="$ARTIFACT_DIR/wsl-mesa.tar.zst"

source /etc/os-release
if [[ "${VERSION_CODENAME:-}" != "$TARGET_CODENAME" ]]; then
    echo "ERROR: production Mesa must be built on Ubuntu $TARGET_CODENAME; found ${VERSION_CODENAME:-unknown}." >&2
    exit 1
fi

echo "==> Installing build dependencies (apt build-dep mesa + LLVM 21 dev pkgs)"
sudo apt-get update
# deb-src must be enabled in /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get build-dep -y mesa
sudo apt-get install -y \
    git meson ninja-build \
    libclc-21-dev llvm-spirv-21 libllvmspirvlib-21-dev libclang-21-dev

echo "==> Cloning Mesa $MESA_BRANCH"
[[ -d "$MESA_SRC" ]] || git clone --depth 1 -b "$MESA_BRANCH" \
    https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_SRC"

echo "==> Configuring meson (prefix=$PREFIX)"
cd "$MESA_SRC"
if [[ -d "$PATCH_DIR" ]]; then
    echo "==> Applying AppSandbox Mesa patches"
    for patch in "$PATCH_DIR"/*.patch; do
        [[ -e "$patch" ]] || continue
        if git apply --reverse --check "$patch" >/dev/null 2>&1; then
            echo "    already applied: $(basename "$patch")"
        else
            git apply --check "$patch"
            git apply "$patch"
            echo "    applied: $(basename "$patch")"
        fi
    done
fi

rm -rf build
meson setup --prefix="$PREFIX" --buildtype=release --strip \
    -D gallium-drivers=llvmpipe,d3d12 \
    -D vulkan-drivers=swrast,microsoft-experimental \
    -D microsoft-clc=enabled \
    -D video-codecs=all \
    -D glvnd=enabled \
    -D platforms=x11,wayland \
    build/

echo "==> Building (this is the slow part, ~30 min)"
ninja -C build -j "$(nproc)"

echo "==> Installing to $PREFIX"
sudo ninja -C build install

echo "==> Checking production D3D12 and dzn outputs"
if ! find "$PREFIX/lib" -type f -name 'libd3d12.so*' -print -quit | grep -q .; then
    echo "ERROR: libd3d12 was not installed; refusing a production artifact." >&2
    exit 1
fi
if ! find "$PREFIX/lib" -type f -iname '*dzn*.so*' -print -quit | grep -q . ||
   ! find "$PREFIX/share/vulkan/icd.d" -type f -iname '*dzn*.json' -print -quit | grep -q .; then
    echo "ERROR: dzn Vulkan driver/ICD was not installed; refusing a production artifact." >&2
    exit 1
fi
if ! grep -Rqs 'ASB_D3D12_DISPLAY' "$MESA_SRC/src"; then
    echo "ERROR: the deterministic AppSandbox display feature gate is absent." >&2
    exit 1
fi

SOURCE_COMMIT="$(git -C "$MESA_SRC" rev-parse HEAD)"
if [[ ! "$SOURCE_COMMIT" =~ ^[0-9a-f]{40}$ ]]; then
    echo "ERROR: Mesa source is not at a full immutable commit." >&2
    exit 1
fi
PATCHSET_HASH="$(find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' -print0 |
    sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
MESA_VERSION="$(git -C "$MESA_SRC" describe --tags --always --dirty 2>/dev/null || echo unknown)"
LLVM_VERSION="$(llvm-config --version 2>/dev/null || echo unknown)"
PATCHSET_VERSION="1.${PATCHSET_HASH:0:16}"
GRAPHICS_VERSION="${GRAPHICS_VERSION:-mesa-${MESA_VERSION}-appsandbox-${PATCHSET_VERSION}}"

mkdir -p "$ARTIFACT_DIR"
tmp_tar="$ARTIFACT_PATH.tmp.$$"
tmp_info="$ARTIFACT_DIR/BUILDINFO.tmp.$$"
trap 'rm -f "$tmp_tar" "$tmp_info"' EXIT
echo "==> Creating reproducible artifact $ARTIFACT_PATH"
sudo tar -C / --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner \
    -cf - opt/wsl-mesa | zstd -T0 -19 -q -o "$tmp_tar"
mv -f "$tmp_tar" "$ARTIFACT_PATH"
ARTIFACT_SHA256="$(sha256sum "$ARTIFACT_PATH" | cut -d' ' -f1)"
cat > "$tmp_info" <<EOF
ubuntu: 26.04 (resolute)
arch: amd64
built-on: $(date -u +%Y-%m-%dT%H:%M:%SZ)
mesa-version: $MESA_VERSION
mesa-branch: $MESA_BRANCH
llvm: $LLVM_VERSION
gallium-drivers: llvmpipe,d3d12
vulkan-drivers: swrast,microsoft-experimental
icd-name: dzn_icd.x86_64.json
mesa-source-commit: $SOURCE_COMMIT
appsandbox-graphics-version: $GRAPHICS_VERSION
mesa-patchset-version: $PATCHSET_VERSION
artifact-sha256: $ARTIFACT_SHA256
production-4k60: true
EOF
mv -f "$tmp_info" "$ARTIFACT_DIR/BUILDINFO"
trap - EXIT
echo "==> Production artifact and BUILDINFO written under $ARTIFACT_DIR"
