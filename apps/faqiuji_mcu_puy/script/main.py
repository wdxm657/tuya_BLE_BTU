from __future__ import annotations

import sys
from pathlib import Path

from PyQt6.QtWidgets import QApplication

if __package__:
    from .main_window import MainWindow
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from script.main_window import MainWindow


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
