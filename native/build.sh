#!/usr/bin/env bash
# Builds the native Open Opal: a slim static depthai-core plus the Qt 6 app.
#
#   native/build.sh            build into native/build (depthai-core in native/deps)
#   native/build.sh install    ... and install to ~/.local (binary, menu entry, data files)
#
# Needs: cmake >= 3.20, ninja, git, curl, zip, pkgconf, a C++17 compiler, qt6-base,
# libusb, systemd-libs (libudev). On CachyOS/Arch: sudo pacman -S --needed cmake ninja git curl zip pkgconf qt6-base
set -euo pipefail
cd "$(dirname "$0")"
here=$PWD
depthai_tag=${DEPTHAI_TAG:-v3.10.0}
deps=${DEPS_DIR:-$here/deps}
prefix=${PREFIX:-$HOME/.local}
jobs=${JOBS:-$(nproc)}

# --- depthai-core, static, only what the Opal needs ------------------------------------
if [ ! -d "$deps/depthai-core" ]; then
  mkdir -p "$deps"
  git clone --branch "$depthai_tag" --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/luxonis/depthai-core.git "$deps/depthai-core"
  # firmware is loaded from a file at runtime (saves ~75 MB RSS); see patches/ for why this is needed
  git -C "$deps/depthai-core" apply "$here"/patches/*.patch
fi
if [ ! -f "$deps/install/lib/cmake/depthai/depthaiConfig.cmake" ]; then
  cmake -S "$deps/depthai-core" -B "$deps/depthai-core/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$deps/install" \
    -DDEPTHAI_OPENCV_SUPPORT=OFF -DDEPTHAI_ENABLE_PROTOBUF=OFF -DDEPTHAI_ENABLE_CURL=OFF \
    -DDEPTHAI_ENABLE_MP4V2=OFF -DDEPTHAI_ENABLE_APRIL_TAG=OFF -DDEPTHAI_ENABLE_KOMPUTE=OFF \
    -DDEPTHAI_DYNAMIC_CALIBRATION_SUPPORT=OFF -DDEPTHAI_BUILD_BETA=OFF \
    -DDEPTHAI_ENABLE_DEVICE_RVC4_FW=OFF -DDEPTHAI_ENABLE_DEVICE_RVC3_FW=OFF \
    -DDEPTHAI_BUILD_EXAMPLES=OFF -DDEPTHAI_BUILD_TESTS=OFF -DDEPTHAI_CLANG_FORMAT=OFF \
    -DDEPTHAI_BINARIES_RESOURCE_COMPILE=OFF -DDEPTHAI_ENABLE_BACKWARD=OFF
  cmake --build "$deps/depthai-core/build" --parallel "$jobs"
  cmake --install "$deps/depthai-core/build"
fi

# --- the app ----------------------------------------------------------------------------
vcpkg_root=$deps/depthai-core/build/vcpkg
cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_INSTALLED_DIR="$deps/depthai-core/build/vcpkg_installed" \
  -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DCMAKE_PREFIX_PATH="$deps/install" -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$here/build" --parallel "$jobs"
echo "Built $here/build/open-opal"

[ "${1:-}" = install ] || exit 0

# --- install: binary, data files, menu entry --------------------------------------------
cmake --install "$here/build"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/open-opal-linux"
mkdir -p "$data_dir"
"$here/fetch-data.sh" "$data_dir"
"$here/extract-firmware.sh" "$deps/install" "$data_dir"
mkdir -p ~/.local/share/applications
sed "s|@EXEC@|$prefix/bin/open-opal|" "$here/../open-opal.desktop" | sed 's|@VENV@/bin/open-opal|'"$prefix"'/bin/open-opal|' \
  > ~/.local/share/applications/open-opal.desktop
echo "Installed $prefix/bin/open-opal and the 'Open Opal' menu entry."
[ -f /etc/udev/rules.d/80-movidius.rules ] || echo "Missing udev rule, see README (step 1)."
grep -q "^v4l2loopback " /proc/modules || echo "v4l2loopback not loaded, see README (step 2)."
