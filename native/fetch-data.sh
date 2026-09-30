#!/usr/bin/env bash
# Downloads the data files Open Opal loads at runtime into the given directory:
#   tuning_color_low_light.bin   Luxonis low-light ISP tuning (not redistributed here)
#   yunet-s-240x320.rvc2.tar.xz  YuNet face detector from the Luxonis model zoo, RVC2 build
set -euo pipefail
dir=${1:?usage: fetch-data.sh <dir>}
mkdir -p "$dir"
[ -f "$dir/tuning_color_low_light.bin" ] || curl -fsSL -o "$dir/tuning_color_low_light.bin" \
  https://artifacts.luxonis.com/artifactory/luxonis-depthai-data-local/misc/tuning_color_low_light.bin
if [ ! -f "$dir/yunet-s-240x320.rvc2.tar.xz" ]; then
  # Same request depthai-core's model zoo makes; the reply carries signed download links.
  url=$(curl -fsSL -H "Content-Type: application/json" \
    "https://easyml.cloud.luxonis.com/models/api/v1/models/download?slug=luxonis/yunet:320x240&platform=RVC2" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["download_links"][0])')
  curl -fsSL -o "$dir/yunet-s-240x320.rvc2.tar.xz.part" "$url"
  mv "$dir/yunet-s-240x320.rvc2.tar.xz.part" "$dir/yunet-s-240x320.rvc2.tar.xz"
fi
echo "Data files in $dir"
