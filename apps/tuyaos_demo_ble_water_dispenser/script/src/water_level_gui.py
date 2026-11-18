"""PyQt5 upper-computer application for MCP1081 water-level analysis."""

from __future__ import annotations

import csv
import json
import math
from collections import deque
from datetime import datetime
from pathlib import Path
import sys
import time
from typing import Optional

import serial
from serial.tools import list_ports
from PyQt5 import QtCore, QtGui, QtWidgets
import pyqtgraph as pg

from water_level_core import AlgorithmResult, MCP1081Parser, SensorFrame, WaterLevelAlgorithm


APP_DIR = Path(__file__).resolve().parent
CALIBRATION_FILE = APP_DIR / "water_level_calibration.json"
DEFAULT_DISPLAY_FRAMES = 480
PLOT_HISTORY_FRAMES = 10000


class SerialWorker(QtCore.QThread):
    line_received = QtCore.pyqtSignal(str)
    connection_error = QtCore.pyqtSignal(str)

    def __init__(self, port: str, baudrate: int) -> None:
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self._running = True

    def run(self) -> None:
        try:
            with serial.Serial(self.port, self.baudrate, timeout=0.2) as device:
                while self._running:
                    raw = device.readline()
                    if raw:
                        self.line_received.emit(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        except Exception as exc:
            if self._running:
                self.connection_error.emit(str(exc))

    def stop(self) -> None:
        self._running = False
        self.wait(1500)


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("MCP1081 四通道连续水位分析")
        self.resize(1500, 930)
        self.parser = MCP1081Parser()
        self.algorithm = WaterLevelAlgorithm()
        self.worker: Optional[SerialWorker] = None
        self.frames: deque[SensorFrame] = deque(maxlen=500)
        self.plot_time: deque[float] = deque(maxlen=PLOT_HISTORY_FRAMES)
        self.plot_level: deque[float] = deque(maxlen=PLOT_HISTORY_FRAMES)
        self.plot_caps = [deque(maxlen=PLOT_HISTORY_FRAMES) for _ in range(4)]
        self.start_monotonic = time.monotonic()
        self.csv_file = None
        self.csv_writer = None
        self.frame_count = 0
        self._build_ui()
        self._load_calibration()
        self.refresh_ports()

    def _build_ui(self) -> None:
        root = QtWidgets.QWidget()
        self.setCentralWidget(root)
        outer = QtWidgets.QVBoxLayout(root)

        connection = QtWidgets.QGroupBox("串口连接")
        connection_layout = QtWidgets.QHBoxLayout(connection)
        self.port_combo = QtWidgets.QComboBox()
        self.baud_combo = QtWidgets.QComboBox()
        self.baud_combo.addItems(["115200", "57600", "38400", "19200", "9600"])
        self.refresh_button = QtWidgets.QPushButton("刷新串口")
        self.connect_button = QtWidgets.QPushButton("连接")
        self.status_label = QtWidgets.QLabel("未连接")
        self.status_label.setStyleSheet("color:#b42318;font-weight:bold")
        for label, widget in (("端口", self.port_combo), ("波特率", self.baud_combo)):
            connection_layout.addWidget(QtWidgets.QLabel(label))
            connection_layout.addWidget(widget)
        connection_layout.addWidget(self.refresh_button)
        connection_layout.addWidget(self.connect_button)
        connection_layout.addWidget(self.status_label)
        connection_layout.addStretch()
        outer.addWidget(connection)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        outer.addWidget(splitter, 1)
        left = QtWidgets.QWidget()
        right = QtWidgets.QWidget()
        splitter.addWidget(left)
        splitter.addWidget(right)
        splitter.setSizes([850, 650])
        left_layout = QtWidgets.QVBoxLayout(left)
        right_layout = QtWidgets.QVBoxLayout(right)

        level_box = QtWidgets.QGroupBox("计算结果")
        level_layout = QtWidgets.QHBoxLayout(level_box)
        self.level_lcd = QtWidgets.QLCDNumber()
        self.level_lcd.setDigitCount(6)
        self.level_lcd.display(0.0)
        self.level_lcd.setMinimumHeight(90)
        self.level_bar = QtWidgets.QProgressBar()
        self.level_bar.setRange(0, 1000)
        self.level_bar.setFormat("水位 %p%")
        self.temp_label = QtWidgets.QLabel("温度: -- °C")
        self.frame_label = QtWidgets.QLabel("有效帧: 0")
        level_layout.addWidget(QtWidgets.QLabel("水位 %"))
        level_layout.addWidget(self.level_lcd, 1)
        level_layout.addWidget(self.level_bar, 2)
        level_layout.addWidget(self.temp_label)
        level_layout.addWidget(self.frame_label)
        left_layout.addWidget(level_box)

        self.level_plot = pg.PlotWidget(title=f"连续水位（最近 {DEFAULT_DISPLAY_FRAMES} 帧）")
        self.level_plot.setLabel("left", "水位", units="%")
        self.level_plot.setLabel("bottom", "运行时间", units="s")
        self.level_plot.setYRange(0, 100)
        self.level_plot.showGrid(x=True, y=True, alpha=0.25)
        self.level_curve = self.level_plot.plot(pen=pg.mkPen("#1565c0", width=2))
        left_layout.addWidget(self.level_plot, 2)

        self.cap_plot = pg.PlotWidget(title="四通道原始电容")
        self.cap_plot.setLabel("left", "电容", units="pF")
        self.cap_plot.setLabel("bottom", "运行时间", units="s")
        self.cap_plot.showGrid(x=True, y=True, alpha=0.25)
        self.cap_plot.addLegend()
        colors = ["#e76f51", "#2a9d8f", "#457b9d", "#8e6bbd"]
        self.cap_curves = [
            self.cap_plot.plot(pen=pg.mkPen(color, width=1.5), name=f"CH{i}") for i, color in enumerate(colors)
        ]
        cap_controls = QtWidgets.QHBoxLayout()
        self.cap_view_button = QtWidgets.QPushButton("切换到 CH0 基准偏移图")
        self.cap_view_button.setCheckable(True)
        self.cap_view_hint = QtWidgets.QLabel("当前：原始电容（算法始终使用原始值）")
        self.display_frames_spin = QtWidgets.QSpinBox()
        self.display_frames_spin.setRange(10, PLOT_HISTORY_FRAMES)
        self.display_frames_spin.setSingleStep(10)
        self.display_frames_spin.setValue(DEFAULT_DISPLAY_FRAMES)
        self.display_frames_spin.setSuffix(" 帧")
        self.display_frames_spin.setToolTip("两张实时图显示的最近数据帧数量；内部最多保留 10000 帧")
        cap_controls.addWidget(self.cap_view_button)
        cap_controls.addWidget(self.cap_view_hint)
        cap_controls.addStretch()
        cap_controls.addWidget(QtWidgets.QLabel("显示最近"))
        cap_controls.addWidget(self.display_frames_spin)
        left_layout.addWidget(self.cap_plot, 2)
        left_layout.addLayout(cap_controls)

        steps = QtWidgets.QGroupBox("算法流水线（每个有效帧均按 ①→⑦ 执行）")
        steps_layout = QtWidgets.QVBoxLayout(steps)
        self.step_labels = []
        descriptions = [
            "① 同步采集：解析 MCU 一帧，得到 C0…C3（pF）",
            "② 空杯差值：ΔCi = direction_i × (Ci − Ci,0)",
            "③ 中值滤波：各通道最近 N 帧 ΔCi 的中位数",
            "④ 滑动平均：各通道最近 M 个中值的平均值",
            "⑤ 四通道求和：ΔCsum = Σ filtered_ΔCi",
            "⑥ 满量程：ΔC100 = Σ |Ci,100 − Ci,0|",
            "⑦ 归一限幅：Level = clamp(ΔCsum / ΔC100 × 100%, 0, 100)",
        ]
        for description in descriptions:
            label = QtWidgets.QLabel(description)
            label.setWordWrap(True)
            label.setStyleSheet("padding:3px")
            steps_layout.addWidget(label)
            self.step_labels.append(label)
        right_layout.addWidget(steps)

        filter_box = QtWidgets.QGroupBox("滤波与标定")
        filter_layout = QtWidgets.QGridLayout(filter_box)
        self.median_spin = QtWidgets.QSpinBox()
        self.median_spin.setRange(1, 51)
        self.median_spin.setSingleStep(2)
        self.median_spin.setValue(5)
        self.average_spin = QtWidgets.QSpinBox()
        self.average_spin.setRange(1, 200)
        self.average_spin.setValue(10)
        self.cal_count_spin = QtWidgets.QSpinBox()
        self.cal_count_spin.setRange(1, 200)
        self.cal_count_spin.setValue(30)
        self.empty_button = QtWidgets.QPushButton("采集空杯标定")
        self.full_button = QtWidgets.QPushButton("采集满杯标定")
        self.clear_cal_button = QtWidgets.QPushButton("清除标定")
        self.save_cal_button = QtWidgets.QPushButton("保存标定文件")
        self.load_cal_button = QtWidgets.QPushButton("加载标定文件")
        self.cal_status = QtWidgets.QLabel("尚未完成空杯和满杯标定")
        filter_layout.addWidget(QtWidgets.QLabel("中值窗口 N"), 0, 0)
        filter_layout.addWidget(self.median_spin, 0, 1)
        filter_layout.addWidget(QtWidgets.QLabel("平均窗口 M"), 0, 2)
        filter_layout.addWidget(self.average_spin, 0, 3)
        filter_layout.addWidget(QtWidgets.QLabel("标定帧数"), 1, 0)
        filter_layout.addWidget(self.cal_count_spin, 1, 1)
        filter_layout.addWidget(self.empty_button, 1, 2)
        filter_layout.addWidget(self.full_button, 1, 3)
        filter_layout.addWidget(self.clear_cal_button, 1, 4)
        filter_layout.addWidget(self.save_cal_button, 2, 2)
        filter_layout.addWidget(self.load_cal_button, 2, 3)
        filter_layout.addWidget(self.cal_status, 3, 0, 1, 5)
        right_layout.addWidget(filter_box)

        self.data_table = QtWidgets.QTableWidget(4, 9)
        self.data_table.setHorizontalHeaderLabels([
            "通道", "原始 C/pF", "空杯 C0", "方向", "原始 ΔC", "中值 ΔC",
            "平均 ΔC", "满量程 ΔC", "水位贡献/%",
        ])
        self.data_table.verticalHeader().setVisible(False)
        self.data_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        for row in range(4):
            self.data_table.setItem(row, 0, QtWidgets.QTableWidgetItem(f"CH{row}"))
        right_layout.addWidget(self.data_table)

        log_box = QtWidgets.QGroupBox("实时串口原文")
        log_layout = QtWidgets.QVBoxLayout(log_box)
        self.serial_log = QtWidgets.QPlainTextEdit()
        self.serial_log.setReadOnly(True)
        self.serial_log.setMaximumBlockCount(3000)
        log_buttons = QtWidgets.QHBoxLayout()
        self.clear_log_button = QtWidgets.QPushButton("清空显示")
        self.csv_button = QtWidgets.QPushButton("开始 CSV 记录")
        log_buttons.addWidget(self.clear_log_button)
        log_buttons.addWidget(self.csv_button)
        log_buttons.addStretch()
        log_layout.addWidget(self.serial_log)
        log_layout.addLayout(log_buttons)
        right_layout.addWidget(log_box, 1)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button.clicked.connect(self.toggle_connection)
        self.empty_button.clicked.connect(lambda: self.capture_calibration(False))
        self.full_button.clicked.connect(lambda: self.capture_calibration(True))
        self.clear_cal_button.clicked.connect(self.clear_calibration)
        self.save_cal_button.clicked.connect(self.save_calibration_as)
        self.load_cal_button.clicked.connect(self.load_calibration_from)
        self.clear_log_button.clicked.connect(self.serial_log.clear)
        self.csv_button.clicked.connect(self.toggle_csv)
        self.cap_view_button.toggled.connect(self.toggle_cap_view)
        self.display_frames_spin.valueChanged.connect(self.update_display_frames)
        self.median_spin.valueChanged.connect(self.update_filter_windows)
        self.average_spin.valueChanged.connect(self.update_filter_windows)

    def refresh_ports(self) -> None:
        current = self.port_combo.currentData()
        self.port_combo.clear()
        for port in list_ports.comports():
            self.port_combo.addItem(f"{port.device} — {port.description}", port.device)
        if current:
            index = self.port_combo.findData(current)
            if index >= 0:
                self.port_combo.setCurrentIndex(index)

    def toggle_connection(self) -> None:
        if self.worker:
            self.disconnect_serial()
            return
        port = self.port_combo.currentData()
        if not port:
            QtWidgets.QMessageBox.warning(self, "串口", "未发现可用串口，请连接设备后刷新。")
            return
        self.parser.reset()
        self.worker = SerialWorker(port, int(self.baud_combo.currentText()))
        self.worker.line_received.connect(self.on_serial_line)
        self.worker.connection_error.connect(self.on_serial_error)
        self.worker.finished.connect(self.on_worker_finished)
        self.worker.start()
        self.connect_button.setText("断开")
        self.status_label.setText(f"已连接 {port}")
        self.status_label.setStyleSheet("color:#067647;font-weight:bold")

    def disconnect_serial(self) -> None:
        worker, self.worker = self.worker, None
        if worker:
            worker.stop()
        self.connect_button.setText("连接")
        self.status_label.setText("未连接")
        self.status_label.setStyleSheet("color:#b42318;font-weight:bold")

    @QtCore.pyqtSlot(str)
    def on_serial_line(self, line: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.serial_log.appendPlainText(f"[{stamp}] {line}")
        frame = self.parser.feed_line(line)
        if frame:
            self.process_frame(frame)

    def process_frame(self, frame: SensorFrame) -> None:
        self.frames.append(frame)
        self.frame_count += 1
        result = self.algorithm.process(frame.cap_pf, frame.timestamp)
        self.update_result(frame, result)
        self.write_csv(frame, result)

    def update_result(self, frame: SensorFrame, result: AlgorithmResult) -> None:
        level = result.level_percent
        self.level_lcd.display(f"{level:.2f}")
        self.level_bar.setValue(round(level * 10))
        self.temp_label.setText(
            f"温度: {frame.temperature_c:.1f} °C" if frame.temperature_c is not None else "温度: -- °C"
        )
        self.frame_label.setText(f"有效帧: {self.frame_count}")
        for row in range(4):
            values = [
                f"{result.raw_cap[row]:.4f}", f"{result.empty_baseline[row]:.4f}",
                "+" if result.direction[row] > 0 else "−", f"{result.raw_delta[row]:.4f}",
                f"{result.median_delta[row]:.4f}", f"{result.average_delta[row]:.4f}",
                f"{result.full_span[row]:.4f}", f"{result.contributions[row]:.2f}",
            ]
            for column, value in enumerate(values, start=1):
                self.data_table.setItem(row, column, QtWidgets.QTableWidgetItem(value))

        caps = ", ".join(f"C{i}={value:.3f}" for i, value in enumerate(result.raw_cap))
        deltas = ", ".join(f"ΔC{i}={value:.4f}" for i, value in enumerate(result.raw_delta))
        medians = ", ".join(f"{value:.4f}" for value in result.median_delta)
        averages = ", ".join(f"{value:.4f}" for value in result.average_delta)
        self.step_labels[0].setText(f"① 同步采集：{caps} pF")
        self.step_labels[1].setText(f"② 空杯差值：{deltas} pF")
        self.step_labels[2].setText(f"③ 中值滤波（N={self.algorithm.median_window}）：[{medians}] pF")
        self.step_labels[3].setText(f"④ 滑动平均（M={self.algorithm.average_window}）：[{averages}] pF")
        self.step_labels[4].setText(f"⑤ 四通道求和：ΔCsum = {result.delta_sum:.5f} pF")
        self.step_labels[5].setText(f"⑥ 满量程：ΔC100 = {result.full_sum:.5f} pF")
        self.step_labels[6].setText(
            f"⑦ 归一限幅：原始 {result.raw_level_percent:.2f}% → 输出 {result.level_percent:.2f}%"
        )

        elapsed = time.monotonic() - self.start_monotonic
        self.plot_time.append(elapsed)
        self.plot_level.append(level)
        for i in range(4):
            self.plot_caps[i].append(frame.cap_pf[i])
        self.redraw_plots()

    def cap_plot_offsets(self) -> tuple[float, float, float, float]:
        """Return display-only offsets that align every channel to CH0."""
        if self.algorithm.empty_baseline is not None:
            baseline = self.algorithm.empty_baseline
        elif self.plot_caps[0] and all(self.plot_caps[i] for i in range(4)):
            baseline = tuple(self.plot_caps[i][0] for i in range(4))
        else:
            return (0.0, 0.0, 0.0, 0.0)
        return tuple(baseline[0] - baseline[i] for i in range(4))

    def redraw_cap_plot(self) -> None:
        count = self.display_frames_spin.value()
        x = list(self.plot_time)[-count:]
        aligned = self.cap_view_button.isChecked()
        offsets = self.cap_plot_offsets() if aligned else (0.0, 0.0, 0.0, 0.0)
        for i, curve in enumerate(self.cap_curves):
            values = [value + offsets[i] for value in list(self.plot_caps[i])[-count:]]
            curve.setData(x, values)

    def redraw_plots(self) -> None:
        count = self.display_frames_spin.value()
        x = list(self.plot_time)[-count:]
        self.level_curve.setData(x, list(self.plot_level)[-count:])
        self.redraw_cap_plot()

    def update_display_frames(self, count: int) -> None:
        self.level_plot.setTitle(f"连续水位（最近 {count} 帧）")
        self.redraw_plots()

    def toggle_cap_view(self, aligned: bool) -> None:
        if aligned:
            offsets = self.cap_plot_offsets()
            source = "空杯标定" if self.algorithm.empty_baseline is not None else "首个数据帧"
            offset_text = ", ".join(f"CH{i} {value:+.4f}" for i, value in enumerate(offsets))
            self.cap_plot.setTitle("四通道电容（偏移到 CH0 初始基准）")
            self.cap_view_button.setText("切换到原始图")
            self.cap_view_hint.setText(f"当前：偏移图；基准={source}；offset/pF：{offset_text}")
        else:
            self.cap_plot.setTitle("四通道原始电容")
            self.cap_view_button.setText("切换到 CH0 基准偏移图")
            self.cap_view_hint.setText("当前：原始电容（算法始终使用原始值）")
        self.redraw_cap_plot()

    def capture_calibration(self, full: bool) -> None:
        count = self.cal_count_spin.value()
        if len(self.frames) < count:
            QtWidgets.QMessageBox.warning(self, "标定", f"需要至少 {count} 个有效帧，当前只有 {len(self.frames)} 个。")
            return
        frames = list(self.frames)[-count:]
        caps = self.algorithm.average_frames(frames)
        if full:
            if self.algorithm.empty_baseline is None:
                QtWidgets.QMessageBox.warning(self, "标定", "请先在空杯状态采集空杯标定。")
                return
            self.algorithm.set_full(caps)
        else:
            self.algorithm.set_empty(caps)
            self.algorithm.full_calibration = None
        self._save_calibration()
        self.update_calibration_status()
        if self.cap_view_button.isChecked():
            self.toggle_cap_view(True)

    def clear_calibration(self) -> None:
        self.algorithm.clear_calibration()
        if CALIBRATION_FILE.exists():
            CALIBRATION_FILE.unlink()
        self.update_calibration_status()
        if self.cap_view_button.isChecked():
            self.toggle_cap_view(True)

    def update_filter_windows(self) -> None:
        self.algorithm.configure_windows(self.median_spin.value(), self.average_spin.value())

    def update_calibration_status(self) -> None:
        empty = self.algorithm.empty_baseline
        full = self.algorithm.full_calibration
        if empty and full:
            spans = [abs(full[i] - empty[i]) for i in range(4)]
            directions = ["+" if full[i] >= empty[i] else "−" for i in range(4)]
            self.cal_status.setText(
                "标定完成 | 满量程 ΔC=" + ", ".join(f"{v:.4f}" for v in spans)
                + " pF | 方向=" + ",".join(directions)
            )
            self.cal_status.setStyleSheet("color:#067647;font-weight:bold")
        elif empty:
            self.cal_status.setText("空杯标定完成；请加满水并采集满杯标定。")
            self.cal_status.setStyleSheet("color:#b54708;font-weight:bold")
        else:
            self.cal_status.setText("尚未完成空杯和满杯标定，水位输出保持 0%。")
            self.cal_status.setStyleSheet("color:#b42318;font-weight:bold")

    def calibration_data(self) -> dict:
        return {
            "format": "mcp1081-water-level-calibration",
            "version": 1,
            "empty_pf": self.algorithm.empty_baseline,
            "full_pf": self.algorithm.full_calibration,
            "median_window": self.median_spin.value(),
            "average_window": self.average_spin.value(),
            "calibration_frames": self.cal_count_spin.value(),
            "saved_at": datetime.now().isoformat(timespec="seconds"),
        }

    def _save_calibration(self, path: Path = CALIBRATION_FILE) -> None:
        path.write_text(json.dumps(self.calibration_data(), indent=2, ensure_ascii=False), encoding="utf-8")

    @staticmethod
    def _validated_calibration_values(data: dict, key: str) -> tuple[float, float, float, float]:
        values = data.get(key)
        if not isinstance(values, (list, tuple)) or len(values) != 4:
            raise ValueError(f"{key} 必须包含 4 个通道值")
        result = tuple(float(value) for value in values)
        if not all(math.isfinite(value) for value in result):
            raise ValueError(f"{key} 包含无效数值")
        return result  # type: ignore[return-value]

    def apply_calibration_data(self, data: dict) -> None:
        if not isinstance(data, dict):
            raise ValueError("标定文件内容必须是 JSON 对象")
        empty = self._validated_calibration_values(data, "empty_pf")
        full = self._validated_calibration_values(data, "full_pf")
        if sum(abs(full[i] - empty[i]) for i in range(4)) <= 1e-12:
            raise ValueError("空杯和满杯标定值完全相同，无法计算水位")
        self.algorithm.set_empty(empty)
        self.algorithm.set_full(full)
        if "median_window" in data:
            self.median_spin.setValue(int(data["median_window"]))
        if "average_window" in data:
            self.average_spin.setValue(int(data["average_window"]))
        if "calibration_frames" in data:
            self.cal_count_spin.setValue(int(data["calibration_frames"]))
        self.update_filter_windows()
        self.update_calibration_status()
        if self.cap_view_button.isChecked():
            self.toggle_cap_view(True)

    def save_calibration_as(self) -> None:
        if not self.algorithm.calibrated:
            QtWidgets.QMessageBox.warning(self, "保存标定", "请先完成空杯和满杯标定。")
            return
        default = APP_DIR / f"water_level_calibration_{datetime.now():%Y%m%d_%H%M%S}.json"
        filename, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "保存空杯和满杯标定", str(default), "标定文件 (*.json)"
        )
        if not filename:
            return
        path = Path(filename)
        if path.suffix.lower() != ".json":
            path = path.with_suffix(".json")
        try:
            self._save_calibration(path)
        except OSError as exc:
            QtWidgets.QMessageBox.critical(self, "保存标定失败", str(exc))
            return
        QtWidgets.QMessageBox.information(self, "保存标定", f"标定已保存到：\n{path}")

    def load_calibration_from(self) -> None:
        filename, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "加载空杯和满杯标定", str(APP_DIR), "标定文件 (*.json)"
        )
        if not filename:
            return
        try:
            data = json.loads(Path(filename).read_text(encoding="utf-8"))
            self.apply_calibration_data(data)
            # Make the manually selected calibration the default for next startup.
            self._save_calibration()
        except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
            QtWidgets.QMessageBox.critical(self, "加载标定失败", str(exc))
            return
        QtWidgets.QMessageBox.information(self, "加载标定", f"标定已加载：\n{filename}")

    def _load_calibration(self) -> None:
        try:
            data = json.loads(CALIBRATION_FILE.read_text(encoding="utf-8"))
            self.apply_calibration_data(data)
        except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError):
            pass
        else:
            return
        self.update_calibration_status()

    def toggle_csv(self) -> None:
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = self.csv_writer = None
            self.csv_button.setText("开始 CSV 记录")
            return
        default = APP_DIR / f"water_level_{datetime.now():%Y%m%d_%H%M%S}.csv"
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "保存 CSV", str(default), "CSV (*.csv)")
        if not path:
            return
        self.csv_file = open(path, "w", newline="", encoding="utf-8-sig")
        self.csv_writer = csv.writer(self.csv_file)
        header = ["timestamp", "temperature_c", "data_ref"]
        for prefix in ("raw_cap_pf", "empty_pf", "raw_delta_pf", "median_delta_pf", "average_delta_pf", "full_span_pf"):
            header.extend(f"{prefix}_ch{i}" for i in range(4))
        header.extend(["delta_sum_pf", "full_sum_pf", "raw_level_percent", "level_percent"])
        self.csv_writer.writerow(header)
        self.csv_button.setText("停止 CSV 记录")

    def write_csv(self, frame: SensorFrame, result: AlgorithmResult) -> None:
        if not self.csv_writer:
            return
        row = [frame.timestamp.isoformat(timespec="milliseconds"), frame.temperature_c, frame.data_ref]
        for values in (result.raw_cap, result.empty_baseline, result.raw_delta, result.median_delta,
                       result.average_delta, result.full_span):
            row.extend(values)
        row.extend([result.delta_sum, result.full_sum, result.raw_level_percent, result.level_percent])
        self.csv_writer.writerow(row)
        self.csv_file.flush()

    def on_serial_error(self, message: str) -> None:
        QtWidgets.QMessageBox.critical(self, "串口错误", message)
        self.disconnect_serial()

    def on_worker_finished(self) -> None:
        if self.worker and not self.worker.isRunning():
            self.worker = None
            self.connect_button.setText("连接")

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:
        self.disconnect_serial()
        if self.csv_file:
            self.csv_file.close()
        event.accept()


def main() -> int:
    pg.setConfigOptions(antialias=True)
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle("Fusion")
    window = MainWindow()
    window.show()
    return app.exec_()


if __name__ == "__main__":
    raise SystemExit(main())
