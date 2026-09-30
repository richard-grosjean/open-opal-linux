import argparse
import ctypes
import glob
import os
import re
import sys

# Serve 3 MB frame buffers straight from mmap so they go back to the OS when freed.
# Otherwise glibc's adaptive threshold keeps them in the heap and RSS drifts up by ~100 MB.
M_MMAP_THRESHOLD = -3
try:
    ctypes.CDLL("libc.so.6").mallopt(M_MMAP_THRESHOLD, 1 << 20)
except OSError:
    pass

from PySide6.QtCore import QCoreApplication, qVersion  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from .ui import MainWindow  # noqa: E402

# Distro plugin directories, in the order the common ones use.
SYSTEM_QT_PLUGIN_DIRS = [
    "/usr/lib/qt6/plugins",  # Arch
    "/usr/lib64/qt6/plugins",  # Fedora, openSUSE
    "/usr/lib/x86_64-linux-gnu/qt6/plugins",  # Debian, Ubuntu
    "/usr/lib/aarch64-linux-gnu/qt6/plugins",
]
KDE_THEME_PLUGIN = "platformthemes/KDEPlasmaPlatformTheme6.so"


def _system_qt_minor() -> tuple[int, int] | None:
    """Major.minor of the distro's Qt, read from the versioned libQt6Core soname."""
    for pattern in ("/usr/lib*/libQt6Core.so.6.*", "/usr/lib/*/libQt6Core.so.6.*"):
        for path in glob.glob(pattern):
            m = re.search(r"libQt6Core\.so\.(\d+)\.(\d+)\.", path)
            if m:
                return int(m.group(1)), int(m.group(2))
    return None


def use_system_qt_theme() -> str | None:
    """Let the pip-installed Qt find the distro's KDE platform theme (Breeze style, colours, icons).

    The wheel ships its own Qt, so on Plasma the app otherwise falls back to the plain Fusion
    style. The KDE plugin uses Qt private API, which is only ABI-stable within a minor release,
    so this is skipped unless the distro's Qt matches the bundled one. Returns the directory
    added, or None. Set OPEN_OPAL_NO_SYSTEM_THEME=1 to opt out.
    """
    if not sys.platform.startswith("linux") or os.environ.get("OPEN_OPAL_NO_SYSTEM_THEME"):
        return None
    if os.environ.get("QT_PLUGIN_PATH"):
        return None  # the user is already steering plugin lookup
    bundled = tuple(int(x) for x in qVersion().split(".")[:2])
    if _system_qt_minor() != bundled:
        return None
    for plugin_dir in SYSTEM_QT_PLUGIN_DIRS:
        if os.path.isfile(os.path.join(plugin_dir, KDE_THEME_PLUGIN)):
            QCoreApplication.addLibraryPath(plugin_dir)
            return plugin_dir
    return None


def main() -> int:
    parser = argparse.ArgumentParser(prog="open-opal", description="Opal C1 controls for Linux")
    parser.add_argument("--device", help="v4l2loopback device to write to (default: first one found)")
    args, qt_args = parser.parse_known_args()

    use_system_qt_theme()
    app = QApplication([sys.argv[0], *qt_args])
    app.setApplicationName("Open Opal")
    app.setDesktopFileName("open-opal")
    window = MainWindow(output_device=args.device)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
