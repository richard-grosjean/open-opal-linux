import argparse
import ctypes
import sys

# Serve 3 MB frame buffers straight from mmap so they go back to the OS when freed.
# Otherwise glibc's adaptive threshold keeps them in the heap and RSS drifts up by ~100 MB.
M_MMAP_THRESHOLD = -3
try:
    ctypes.CDLL("libc.so.6").mallopt(M_MMAP_THRESHOLD, 1 << 20)
except OSError:
    pass

from PySide6.QtWidgets import QApplication  # noqa: E402

from .ui import MainWindow  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(prog="open-opal", description="Opal C1 controls for Linux")
    parser.add_argument("--device", help="v4l2loopback device to write to (default: first one found)")
    args, qt_args = parser.parse_known_args()

    app = QApplication([sys.argv[0], *qt_args])
    app.setApplicationName("Open Opal")
    app.setDesktopFileName("open-opal")
    window = MainWindow(output_device=args.device)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
