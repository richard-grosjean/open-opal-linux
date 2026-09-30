#!/usr/bin/env bash
# Extracts the Myriad X firmware the app uploads to the camera from a depthai-core install
# (built with DEPTHAI_BINARIES_RESOURCE_COMPILE=OFF, which installs the archive under
# share/depthai) into <dir>/depthai-device-firmware.cmd. The app points DEPTHAI_DEVICE_BINARY
# at that file, so the library does not keep its own decompressed copies in memory.
set -euo pipefail
install_dir=${1:?usage: extract-firmware.sh <depthai install prefix> <dir>}
dir=${2:?usage: extract-firmware.sh <depthai install prefix> <dir>}
archive=$(ls "$install_dir"/share/depthai/depthai-device-fwp-*.tar.xz | head -1)
mkdir -p "$dir"
tar -xJf "$archive" --wildcards -O 'depthai-device-openvino-universal-*.cmd' > "$dir/depthai-device-firmware.cmd.part"
mv "$dir/depthai-device-firmware.cmd.part" "$dir/depthai-device-firmware.cmd"
echo "Firmware in $dir/depthai-device-firmware.cmd"
