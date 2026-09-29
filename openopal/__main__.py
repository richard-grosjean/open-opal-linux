import argparse
import sys

from PySide6.QtWidgets import QApplication

from .ui import MainWindow


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
