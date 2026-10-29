from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from . import config
from .worker import FlashWorker, ProbeScanWorker


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("PY32F0 Flash Utility")
        self.resize(780, 720)
        self.worker: FlashWorker | ProbeScanWorker | None = None
        self.image_inputs: dict[str, QLineEdit] = {}
        self._build_ui()
        self._scan_probes()

    def _build_ui(self) -> None:
        central = QWidget(self)
        root = QVBoxLayout(central)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(10)

        connection_box = QGroupBox("Connection")
        connection_form = QFormLayout(connection_box)
        self.target_combo = QComboBox()
        for target_name in config.TARGETS:
            self.target_combo.addItem(target_name, target_name)
        default_target_index = self.target_combo.findData(config.DEFAULT_TARGET_NAME)
        if default_target_index >= 0:
            self.target_combo.setCurrentIndex(default_target_index)
        connection_form.addRow("PUYA MCU", self.target_combo)

        pack_row = QHBoxLayout()
        self.pack_input = QLineEdit()
        pack = config.default_pack_path()
        if pack:
            self.pack_input.setText(str(pack))
        pack_browse = QPushButton("Browse...")
        pack_browse.clicked.connect(self._browse_pack)
        pack_row.addWidget(self.pack_input, 1)
        pack_row.addWidget(pack_browse)
        connection_form.addRow("Puya CMSIS-Pack", pack_row)

        probe_row = QHBoxLayout()
        self.probe_combo = QComboBox()
        self.probe_combo.addItem("Auto select", "")
        self.refresh_probes_button = QPushButton("Refresh")
        self.refresh_probes_button.clicked.connect(self._scan_probes)
        probe_row.addWidget(self.probe_combo, 1)
        probe_row.addWidget(self.refresh_probes_button)
        connection_form.addRow("SWD probe", probe_row)

        self.frequency_input = QSpinBox()
        self.frequency_input.setRange(100_000, 10_000_000)
        self.frequency_input.setSingleStep(100_000)
        self.frequency_input.setValue(config.SWD_FREQUENCY_HZ)
        self.frequency_input.setSuffix(" Hz")
        connection_form.addRow("SWD frequency", self.frequency_input)
        self.reset_button = QPushButton("Reset MCU")
        self.reset_button.setToolTip(
            "Reset the PUYA MCU through pyOCD. This does not erase or program Flash."
        )
        self.reset_button.clicked.connect(
            lambda: self._run_operation("reset")
        )
        connection_form.addRow("Target control", self.reset_button)
        root.addWidget(connection_box)

        files_box = QGroupBox("Firmware images")
        files_form = QFormLayout(files_box)
        self._add_file_row(
            files_form,
            "iap",
            "IAP BIN",
            config.IAP_BIN_DEFAULT,
        )
        self._add_file_row(
            files_form,
            "app_a",
            "APP_A BIN",
            config.APP_A_BIN_DEFAULT,
        )
        self._add_file_row(
            files_form,
            "app_b",
            "APP_B BIN",
            config.APP_B_BIN_DEFAULT,
        )
        root.addWidget(files_box)

        destructive_box = QGroupBox("Erase")
        destructive_grid = QGridLayout(destructive_box)
        self.erase_all_button = QPushButton("Erase entire flash")
        self.erase_all_button.setStyleSheet(
            "QPushButton { background: #a93226; color: white; font-weight: 600; }"
        )
        self.erase_all_button.clicked.connect(self._confirm_erase_all)
        destructive_grid.addWidget(self.erase_all_button, 0, 0, 1, 2)
        self._add_action_button(
            destructive_grid, "Erase IAP (0x08000000)", "erase:iap", 1, 0,
            "Erase the 16 KB IAP partition?",
        )
        self._add_action_button(
            destructive_grid, "Erase APP_A (0x08004000)", "erase:app_a", 1, 1,
            "Erase the 48 KB APP_A partition?",
        )
        self._add_action_button(
            destructive_grid, "Erase APP_B (0x08010000)", "erase:app_b", 2, 0,
            "Erase the 48 KB APP_B partition?",
        )
        self._add_action_button(
            destructive_grid, "Erase OTA metadata (0x0801C000)", "erase:metadata", 2, 1,
            "Erase both A/B OTA metadata records and the APP_A factory guard?",
        )
        self._add_action_button(
            destructive_grid, "Erase config (0x0801E000)", "erase:config", 3, 0,
            "Erase the 8 KB configuration partition?",
        )
        root.addWidget(destructive_box)

        program_box = QGroupBox("Program")
        program_grid = QGridLayout(program_box)
        self._add_action_button(
            program_grid,
            "Program IAP",
            "program:iap",
            0,
            0,
            "Programming erases and replaces the 16 KB IAP partition at "
            "0x08000000. Continue?",
        )
        self._add_action_button(
            program_grid,
            "Program APP_A",
            "program:app_a",
            0,
            1,
            "Programming erases and replaces the 48 KB APP_A partition at "
            "0x08004000. Continue?",
        )
        self._add_action_button(
            program_grid,
            "Program APP_B",
            "program:app_b",
            0,
            2,
            "Programming erases and replaces the 48 KB APP_B partition at "
            "0x08010000. Continue?",
        )
        self.factory_button = QPushButton("Factory setup: erase all + APP_A + IAP")
        self.factory_button.setToolTip(
            "Erases all 128 KB, programs APP_A, then IAP. Configuration and OTA "
            "metadata are erased."
        )
        self.factory_button.clicked.connect(self._confirm_factory_setup)
        program_grid.addWidget(self.factory_button, 1, 0, 1, 3)
        root.addWidget(program_box)

        log_box = QGroupBox("Operation log")
        log_layout = QVBoxLayout(log_box)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(2000)
        self.log_view.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        log_layout.addWidget(self.log_view)
        root.addWidget(log_box, 1)

        self.status_label = QLabel("Ready")
        self.status_label.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        root.addWidget(self.status_label)
        self.setCentralWidget(central)

    def _add_file_row(
        self,
        form: QFormLayout,
        key: str,
        label: str,
        default_path: Path,
    ) -> None:
        row = QHBoxLayout()
        path_input = QLineEdit(str(default_path))
        browse = QPushButton("Browse...")
        browse.clicked.connect(lambda _checked=False, field=path_input: self._browse_bin(field))
        row.addWidget(path_input, 1)
        row.addWidget(browse)
        form.addRow(label, row)
        self.image_inputs[key] = path_input

    def _add_action_button(
        self,
        layout: QGridLayout,
        label: str,
        operation: str,
        row: int,
        column: int,
        confirmation: str | None = None,
    ) -> None:
        button = QPushButton(label)
        button.clicked.connect(
            lambda _checked=False, op=operation, prompt=confirmation: (
                self._confirm_then_run(op, prompt)
                if prompt
                else self._run_operation(op)
            )
        )
        layout.addWidget(button, row, column)

    def _browse_pack(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Select Puya CMSIS-Pack directory")
        if path:
            self.pack_input.setText(path)

    def _browse_bin(self, field: QLineEdit) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Select firmware BIN",
            field.text(),
            "Binary images (*.bin);;All files (*)",
        )
        if path:
            field.setText(path)

    def _scan_probes(self) -> None:
        if self.worker and self.worker.isRunning():
            return
        self.refresh_probes_button.setEnabled(False)
        worker = ProbeScanWorker(self)
        worker.probes_ready.connect(self._probes_ready)
        worker.failed.connect(self._scan_failed)
        worker.finished.connect(self._scan_finished)
        self.worker = worker
        worker.start()

    def _probes_ready(self, probes: list[tuple[str, str]]) -> None:
        selected_uid = self.probe_combo.currentData()
        self.probe_combo.clear()
        self.probe_combo.addItem("Auto select", "")
        for uid, description in probes:
            self.probe_combo.addItem(description or uid, uid)
        index = self.probe_combo.findData(selected_uid)
        if index >= 0:
            self.probe_combo.setCurrentIndex(index)
        self._log(f"Found {len(probes)} SWD probe(s).")

    def _scan_failed(self, message: str) -> None:
        self._log(f"Probe scan failed: {message}")

    def _scan_finished(self) -> None:
        self.refresh_probes_button.setEnabled(True)

    def _confirm_erase_all(self) -> None:
        answer = QMessageBox.warning(
            self,
            "Erase entire flash",
            "This permanently erases all 128 KB, including IAP, APP_A, APP_B, "
            "OTA metadata, and configuration.\n\nContinue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer == QMessageBox.StandardButton.Yes:
            self._run_operation("erase_all")

    def _confirm_factory_setup(self) -> None:
        answer = QMessageBox.warning(
            self,
            "Factory setup",
            "This erases the entire 128 KB flash, then programs APP_A and IAP. "
            "All current firmware, OTA metadata, and configuration will be lost.\n\n"
            "Continue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer == QMessageBox.StandardButton.Yes:
            self._run_operation("factory_setup")

    def _confirm_then_run(self, operation: str, prompt: str) -> None:
        answer = QMessageBox.question(
            self,
            "Confirm erase",
            prompt,
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer == QMessageBox.StandardButton.Yes:
            self._run_operation(operation)

    def _run_operation(self, operation: str) -> None:
        if self.worker and self.worker.isRunning():
            return
        pack_path = Path(self.pack_input.text().strip())
        if not pack_path.exists():
            QMessageBox.critical(self, "Missing CMSIS-Pack", "Select the Puya DFP path.")
            return

        arguments: dict[str, object] = {}
        if operation.startswith("program:"):
            region_key = operation.partition(":")[2]
            arguments["image"] = self.image_inputs[region_key].text().strip()
        elif operation == "factory_setup":
            arguments["iap_image"] = self.image_inputs["iap"].text().strip()
            arguments["app_a_image"] = self.image_inputs["app_a"].text().strip()

        worker = FlashWorker(
            operation=operation,
            pack_path=pack_path,
            probe_uid=self.probe_combo.currentData(),
            frequency=self.frequency_input.value(),
            target_name=self.target_combo.currentData(),
            arguments=arguments,
        )
        worker.log_message.connect(self._log)
        worker.completed.connect(self._operation_completed)
        worker.finished.connect(self._operation_finished)
        self.worker = worker
        self._set_busy(True)
        self._log(f"=== Start: {operation} ===")
        self.status_label.setText("Working...")
        worker.start()

    def _operation_completed(self, succeeded: bool, message: str) -> None:
        self._log(message)
        self.status_label.setText("Completed" if succeeded else "Failed")
        if not succeeded:
            QMessageBox.critical(self, "Operation failed", message)
        else:
            QMessageBox.information(self, "Operation complete", message)

    def _operation_finished(self) -> None:
        self._set_busy(False)

    def _set_busy(self, busy: bool) -> None:
        for widget in self.findChildren(QPushButton):
            widget.setEnabled(not busy)
        self.pack_input.setEnabled(not busy)
        self.probe_combo.setEnabled(not busy)
        self.target_combo.setEnabled(not busy)
        self.frequency_input.setEnabled(not busy)
        for field in self.image_inputs.values():
            field.setEnabled(not busy)

    def _log(self, message: str) -> None:
        self.log_view.appendPlainText(message)

    def closeEvent(self, event) -> None:
        if self.worker and self.worker.isRunning():
            QMessageBox.warning(
                self,
                "Operation in progress",
                "Wait for the current operation to finish before closing.",
            )
            event.ignore()
            return
        event.accept()
