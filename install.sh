#!/usr/bin/env bash
# Sets up the venv and adds Open Opal to the application menu. The two system steps
# (udev rule, v4l2loopback) need sudo and are printed rather than run.
set -euo pipefail
cd "$(dirname "$0")"
python3 -m venv .venv
.venv/bin/pip install -q --upgrade pip
.venv/bin/pip install -q -e .
mkdir -p ~/.local/share/applications
sed "s|@VENV@|$PWD/.venv|" open-opal.desktop > ~/.local/share/applications/open-opal.desktop
echo "Installed. Launch 'Open Opal' from the menu or run: $PWD/.venv/bin/open-opal"
[ -f /etc/udev/rules.d/80-movidius.rules ] || echo "Missing udev rule, see README (step 1)."
grep -q "^v4l2loopback " /proc/modules || echo "v4l2loopback not loaded, see README (step 2)."
