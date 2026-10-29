from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import QThread, pyqtSignal

from .pyocd_backend import FlashError, PyOcdBackend, list_probes


class ProbeScanWorker(QThread):
    probes_ready = pyqtSignal(object)
    failed = pyqtSignal(str)

    def run(self) -> None:
        try:
            self.probes_ready.emit(list_probes())
        except Exception as exc:
            self.failed.emit(str(exc))


class FlashWorker(QThread):
    log_message = pyqtSignal(str)
    completed = pyqtSignal(bool, str)

    def __init__(
        self,
        operation: str,
        pack_path: Path,
        probe_uid: str | None,
        frequency: int,
        target_name: str,
        arguments: dict[str, object] | None = None,
    ) -> None:
        super().__init__()
        self.operation = operation
        self.pack_path = pack_path
        self.probe_uid = probe_uid
        self.frequency = frequency
        self.target_name = target_name
        self.arguments = arguments or {}

    def run(self) -> None:
        try:
            backend = PyOcdBackend(
                pack_path=self.pack_path,
                probe_uid=self.probe_uid,
                frequency=self.frequency,
                target_name=self.target_name,
                log=self.log_message.emit,
            )
            if self.operation == "erase_all":
                backend.erase_all()
            elif self.operation == "reset":
                backend.reset_target()
            elif self.operation.startswith("erase:"):
                backend.erase_region(self.operation.partition(":")[2])
            elif self.operation.startswith("program:"):
                region = self.operation.partition(":")[2]
                backend.program(region, Path(self.arguments["image"]))
            elif self.operation == "factory_setup":
                backend.factory_setup(
                    Path(self.arguments["iap_image"]),
                    Path(self.arguments["app_a_image"]),
                )
            else:
                raise FlashError(f"Unsupported operation: {self.operation}")
        except Exception as exc:
            self.completed.emit(False, str(exc))
        else:
            self.completed.emit(True, "Operation completed.")
