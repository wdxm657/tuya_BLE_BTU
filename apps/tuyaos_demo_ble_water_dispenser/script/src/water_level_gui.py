"""PyQt5 upper-computer application for MCP1081 water-level analysis."""

from __future__ import annotations

import csv
import json
import math
from collections import deque
from datetime import datetime
from pathlib import Path
from statistics import fmean, pstdev
import sys
import time
from typing import Optional

import serial
from serial.tools import list_ports
from PyQt5 import QtCore, QtGui, QtWidgets
import pyqtgraph as pg
import numpy as np

from water_level_core import AlgorithmResult, MCP1081Parser, SensorFrame, WaterLevelAlgorithm


APP_DIR = Path(__file__).resolve().parent
CALIBRATION_FILE = APP_DIR / "water_level_calibration.json"
DEFAULT_DISPLAY_FRAMES = 480
PLOT_HISTORY_FRAMES = 10000
PART_CHANNELS = {"A": (0, 1, 2, 3), "B": (6, 7, 8, 9)}


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


class CycleChartsDialog(QtWidgets.QDialog):
    """Visual analysis of the currently loaded cycle CSV records."""

    def __init__(self, records: list[dict], source_name: str, parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"循环图表分析 — {source_name}")
        self.resize(1180, 760)
        self.records = [dict(record) for record in records]
        layout = QtWidgets.QVBoxLayout(self)
        self.summary_label = QtWidgets.QLabel()
        self.summary_label.setWordWrap(True)
        self.summary_label.setStyleSheet("font-weight:bold;color:#344054;padding:6px")
        layout.addWidget(self.summary_label)
        self.tabs = QtWidgets.QTabWidget()
        layout.addWidget(self.tabs, 1)
        self._build_charts()

    @staticmethod
    def _plot(title: str, y_label: str) -> pg.PlotWidget:
        plot = pg.PlotWidget(title=title)
        plot.setLabel("bottom", "高度 ID")
        plot.setLabel("left", y_label)
        plot.showGrid(x=True, y=True, alpha=0.25)
        plot.setXRange(0, 10, padding=0.03)
        plot.addLegend()
        return plot

    def _add_tab(self, widget: QtWidgets.QWidget, title: str) -> None:
        page = QtWidgets.QWidget()
        page_layout = QtWidgets.QVBoxLayout(page)
        page_layout.addWidget(widget)
        self.tabs.addTab(page, title)

    def _values_by_id(self, field: str) -> dict[int, list[float]]:
        values = {index: [] for index in range(11)}
        for record in self.records:
            value = record.get(field)
            if value is not None:
                values[int(record["height_index"])].append(float(value))
        return values

    def _build_charts(self) -> None:
        cycles = sorted({int(record["cycle"]) for record in self.records})

        curves = self._plot("各循环上升/下降百分比趋势", "原始百分比 (%)")
        hysteresis = self._plot("同一循环相同水位误差（Up - Down）", "百分比差 (百分点)")
        hysteresis.addItem(pg.InfiniteLine(pos=0, angle=0, pen=pg.mkPen("#667085", style=QtCore.Qt.DashLine)))
        for position, cycle in enumerate(cycles):
            color = pg.intColor(position, hues=max(1, len(cycles)), values=1, maxValue=220)
            cycle_records = sorted(
                (record for record in self.records if int(record["cycle"]) == cycle),
                key=lambda record: int(record["height_index"]),
            )
            for field, symbol, style, suffix in (
                ("down_percent", "o", QtCore.Qt.SolidLine, "Down"),
                ("up_percent", "t", QtCore.Qt.DashLine, "Up"),
            ):
                points = [(int(record["height_index"]), float(record[field])) for record in cycle_records if record.get(field) is not None]
                if points:
                    curves.plot(
                        [point[0] for point in points], [point[1] for point in points],
                        pen=pg.mkPen(color, width=2, style=style), symbol=symbol,
                        symbolBrush=color, symbolSize=6, name=f"循环{cycle} {suffix}",
                    )
            diff_points = [
                (int(record["height_index"]), float(record["up_percent"]) - float(record["down_percent"]))
                for record in cycle_records
                if record.get("down_percent") is not None and record.get("up_percent") is not None
            ]
            if diff_points:
                hysteresis.plot(
                    [point[0] for point in diff_points], [point[1] for point in diff_points],
                    pen=pg.mkPen(color, width=2), symbol="o", symbolBrush=color,
                    name=f"循环{cycle}",
                )
        self._add_tab(curves, "循环趋势")

        down_values = self._values_by_id("down_percent")
        up_values = self._values_by_id("up_percent")
        ids = list(range(11))
        down_mean = [fmean(down_values[index]) if down_values[index] else math.nan for index in ids]
        up_mean = [fmean(up_values[index]) if up_values[index] else math.nan for index in ids]
        down_std = [pstdev(down_values[index]) if len(down_values[index]) > 1 else 0.0 for index in ids]
        up_std = [pstdev(up_values[index]) if len(up_values[index]) > 1 else 0.0 for index in ids]

        means = self._plot("所有循环均值（误差棒为 ±1 标准差）", "原始百分比 (%)")
        means.plot(ids, down_mean, pen=pg.mkPen("#1565c0", width=3), symbol="o", symbolBrush="#1565c0", name="Down 均值")
        means.plot(ids, up_mean, pen=pg.mkPen("#d97706", width=3), symbol="t", symbolBrush="#d97706", name="Up 均值")
        x_values = np.asarray(ids, dtype=float)
        means.addItem(pg.ErrorBarItem(
            x=x_values, y=np.asarray(down_mean), top=np.asarray(down_std), bottom=np.asarray(down_std),
            beam=0.18, pen=pg.mkPen("#1565c0"),
        ))
        means.addItem(pg.ErrorBarItem(
            x=x_values, y=np.asarray(up_mean), top=np.asarray(up_std), bottom=np.asarray(up_std),
            beam=0.18, pen=pg.mkPen("#d97706"),
        ))
        self._add_tab(means, "所有循环均值")

        repeatability = self._plot("跨循环重复性误差（标准差）", "标准差 (百分点)")
        repeatability.addItem(pg.BarGraphItem(x=[i - 0.18 for i in ids], height=down_std, width=0.34, brush="#5B8FF9", name="Down 标准差"))
        repeatability.addItem(pg.BarGraphItem(x=[i + 0.18 for i in ids], height=up_std, width=0.34, brush="#F6BD16", name="Up 标准差"))
        self._add_tab(repeatability, "重复性误差")
        self._add_tab(hysteresis, "同水位误差")

        down_range = [max(values) - min(values) if values else 0.0 for values in down_values.values()]
        up_range = [max(values) - min(values) if values else 0.0 for values in up_values.values()]
        ranges = self._plot("跨循环最大波动范围（最大值 - 最小值）", "极差 (百分点)")
        ranges.addItem(pg.BarGraphItem(x=[i - 0.18 for i in ids], height=down_range, width=0.34, brush="#2E90FA", name="Down 极差"))
        ranges.addItem(pg.BarGraphItem(x=[i + 0.18 for i in ids], height=up_range, width=0.34, brush="#F79009", name="Up 极差"))
        self._add_tab(ranges, "极差")

        differences = [
            (abs(float(record["up_percent"]) - float(record["down_percent"])), int(record["cycle"]), int(record["height_index"]))
            for record in self.records
            if record.get("down_percent") is not None and record.get("up_percent") is not None
        ]
        worst_hysteresis = max(differences, default=(0.0, 0, 0))
        repeatability_candidates = [
            (down_range[index], "Down", index) for index in ids
        ] + [(up_range[index], "Up", index) for index in ids]
        worst_repeatability = max(repeatability_candidates, default=(0.0, "--", 0))
        self.summary_label.setText(
            f"共 {len(cycles)} 次循环；最大同水位误差绝对值 {worst_hysteresis[0]:.4f} 个百分点"
            f"（循环 {worst_hysteresis[1]} / ID {worst_hysteresis[2]}）；"
            f"最大跨循环极差 {worst_repeatability[0]:.4f} 个百分点"
            f"（{worst_repeatability[1]} / ID {worst_repeatability[2]}）。"
        )


class CycleAnalysisWindow(QtWidgets.QDialog):
    """Manually capture A-route percentages for up/down cycle analysis."""

    CSV_HEADER = [
        "循环", "ID", "容量(ML)", "下降容量差(ML)", "上升容量差(ML)",
        "下降百分比", "上升百分比", "百分比差", "下降时间", "上升时间",
    ]
    PAIRED_HEADER_V3 = [
        "cycle", "height_index", "volume_ml",
        "down_volume_difference_ml", "up_volume_difference_ml",
        "down_percent", "up_percent", "difference_up_minus_down",
        "down_timestamp", "up_timestamp",
    ]
    PAIRED_HEADER_V2 = [
        "cycle", "height_index", "volume_ml", "down_percent", "up_percent",
        "difference_up_minus_down", "down_timestamp", "up_timestamp",
    ]
    PAIRED_HEADER_V1 = [
        "cycle", "height_index", "down_percent", "up_percent",
        "difference_up_minus_down", "down_timestamp", "up_timestamp",
    ]
    LEGACY_HEADER = ["timestamp", "cycle", "direction", "height_index", "level_percent"]

    def __init__(self, parent: QtWidgets.QWidget) -> None:
        super().__init__(parent)
        self.setWindowTitle("A路升降循环分析")
        self.resize(780, 560)
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose, False)
        self.current_level: Optional[float] = None
        self.csv_file = None
        self.csv_writer = None
        self.csv_path: Optional[Path] = None
        self.records: list[dict] = []
        self.actions: list[tuple[str, int, str, int]] = []
        self.charts_window: Optional[CycleChartsDialog] = None
        self._build_ui()
        self._open_default_csv()

    def _build_ui(self) -> None:
        layout = QtWidgets.QVBoxLayout(self)
        controls = QtWidgets.QGridLayout()
        self.cycle_spin = QtWidgets.QSpinBox()
        self.cycle_spin.setRange(1, 999999)
        self.cycle_spin.setValue(1)
        self.direction_combo = QtWidgets.QComboBox()
        self.direction_combo.addItem("Down（最高往最低流）", "down")
        self.direction_combo.addItem("Up（最低往最高升）", "up")
        self.index_spin = QtWidgets.QSpinBox()
        self.index_spin.setRange(0, 10)
        self.index_spin.setReadOnly(True)
        self.index_spin.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        self.index_spin.setToolTip("由方向自动控制：Down 从 0 递增，Up 从 10 递减")
        self.volume_label = QtWidgets.QLabel("-- mL")
        self.volume_label.setStyleSheet("font-size:18px;font-weight:bold;color:#067647")
        self.volume_difference_label = QtWidgets.QLabel("-- mL")
        self.level_label = QtWidgets.QLabel("-- %")
        self.level_label.setStyleSheet("font-size:28px;font-weight:bold;color:#1565c0")
        self.save_button = QtWidgets.QPushButton("保存当前百分比")
        self.save_button.setMinimumHeight(42)
        self.choose_csv_button = QtWidgets.QPushButton("另存/切换 CSV")
        self.next_cycle_button = QtWidgets.QPushButton("下一循环")
        self.delete_last_button = QtWidgets.QPushButton("删除最近一条")
        self.charts_button = QtWidgets.QPushButton("图表分析")
        controls.addWidget(QtWidgets.QLabel("循环编号"), 0, 0)
        controls.addWidget(self.cycle_spin, 0, 1)
        controls.addWidget(QtWidgets.QLabel("当前方向"), 0, 2)
        controls.addWidget(self.direction_combo, 0, 3)
        controls.addWidget(QtWidgets.QLabel("高度 Index"), 1, 0)
        controls.addWidget(self.index_spin, 1, 1)
        controls.addWidget(QtWidgets.QLabel("A路原始百分比"), 1, 2)
        controls.addWidget(self.level_label, 1, 3)
        controls.addWidget(QtWidgets.QLabel("对应容量"), 2, 0)
        controls.addWidget(self.volume_label, 2, 1)
        controls.addWidget(QtWidgets.QLabel("本步容量差分"), 2, 2)
        controls.addWidget(self.volume_difference_label, 2, 3)
        controls.addWidget(self.save_button, 3, 0, 1, 2)
        controls.addWidget(self.next_cycle_button, 3, 2)
        controls.addWidget(self.choose_csv_button, 3, 3)
        controls.addWidget(self.delete_last_button, 4, 0, 1, 2)
        controls.addWidget(self.charts_button, 4, 2, 1, 2)
        layout.addLayout(controls)

        self.path_label = QtWidgets.QLabel()
        self.path_label.setWordWrap(True)
        self.path_label.setStyleSheet("color:#475467")
        layout.addWidget(self.path_label)
        note = QtWidgets.QLabel(
            "操作：Down 自动从 index 0 递增到 10；Up 自动从 index 10 递减到 0。"
            "等待 A 路水位稳定后点击保存，每次点击都会立即追加并刷新到 CSV。"
        )
        note.setWordWrap(True)
        layout.addWidget(note)

        self.table = QtWidgets.QTableWidget(0, 10)
        self.table.setHorizontalHeaderLabels([
            "循环", "高度 Index", "容量/mL", "Down容量差/mL", "Up容量差/mL",
            "Down/%", "Up/%", "差值 Up-Down", "Down 时间", "Up 时间",
        ])
        self.table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        layout.addWidget(self.table, 1)
        self.save_button.clicked.connect(self.save_current_level)
        self.choose_csv_button.clicked.connect(self.choose_csv)
        self.next_cycle_button.clicked.connect(self.next_cycle)
        self.delete_last_button.clicked.connect(self.delete_last_record)
        self.charts_button.clicked.connect(self.show_charts)
        self.direction_combo.currentIndexChanged.connect(self.reset_index_for_direction)
        self.index_spin.valueChanged.connect(self.update_volume_display)
        self.reset_index_for_direction()

    def _open_default_csv(self) -> None:
        directory = APP_DIR / "analysis_data"
        directory.mkdir(parents=True, exist_ok=True)
        existing = [path for path in directory.glob("water_level_cycles_*.csv") if path.is_file()]
        if existing:
            latest = max(existing, key=lambda path: path.stat().st_mtime)
            self.open_csv(latest)
        else:
            self.open_csv(directory / f"water_level_cycles_{datetime.now():%Y%m%d_%H%M%S}.csv")

    def open_csv(self, path: Path) -> None:
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = self.csv_writer = None
        self.csv_path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path_label.setText(f"当前 CSV：{path}")
        self.load_existing_rows(path)
        self.rewrite_csv()

    def choose_csv(self) -> None:
        default = self.csv_path or (APP_DIR / "analysis_data" / "water_level_cycles.csv")
        filename, _ = QtWidgets.QFileDialog.getSaveFileName(self, "选择测试 CSV", str(default), "CSV (*.csv)")
        if not filename:
            return
        path = Path(filename)
        if path.suffix.lower() != ".csv":
            path = path.with_suffix(".csv")
        self.open_csv(path)

    def show_charts(self) -> None:
        if not self.records:
            QtWidgets.QMessageBox.warning(self, "图表分析", "当前 CSV 没有可绘制的数据。")
            return
        source_name = self.csv_path.name if self.csv_path else "当前数据"
        if self.charts_window is not None:
            self.charts_window.close()
        self.charts_window = CycleChartsDialog(self.records, source_name, self)
        self.charts_window.show()

    def update_level(self, level: float) -> None:
        self.current_level = float(level)
        self.level_label.setText(f"{self.current_level:.2f} %")

    def save_current_level(self) -> None:
        if self.current_level is None:
            QtWidgets.QMessageBox.warning(self, "保存测试数据", "尚未收到有效的 A 路水位数据。")
            return
        timestamp = datetime.now().isoformat(timespec="milliseconds")
        cycle = self.cycle_spin.value()
        direction = self.direction_combo.currentData()
        height_index = self.index_spin.value()
        record = next(
            (item for item in self.records if item["cycle"] == cycle and item["height_index"] == height_index),
            None,
        )
        if record is None:
            record = {
                "cycle": cycle, "height_index": height_index,
                "volume_ml": self.volume_for_index(height_index),
                "down_percent": None, "up_percent": None,
                "down_timestamp": "", "up_timestamp": "",
            }
            self.records.append(record)
        record[f"{direction}_percent"] = round(self.current_level, 4)
        record[f"{direction}_timestamp"] = timestamp
        self.actions.append((timestamp, cycle, direction, height_index))
        self.rewrite_csv()
        self.refresh_table()
        self.advance_index()

    def reset_index_for_direction(self) -> None:
        self.index_spin.setValue(0 if self.direction_combo.currentData() == "down" else 10)
        self.update_volume_display()

    @staticmethod
    def volume_for_index(height_index: int) -> int:
        volumes = (2400, 2200, 2000, 1700, 1400, 1100, 700, 300, 200, 100, 0)
        if not 0 <= height_index < len(volumes):
            raise ValueError(f"无效高度 Index：{height_index}")
        return volumes[height_index]

    @classmethod
    def volume_difference_for_index(cls, height_index: int, direction: str) -> Optional[int]:
        if direction == "down":
            if height_index == 0:
                return None
            return cls.volume_for_index(height_index) - cls.volume_for_index(height_index - 1)
        if direction == "up":
            if height_index == 10:
                return None
            return cls.volume_for_index(height_index) - cls.volume_for_index(height_index + 1)
        raise ValueError(f"无效方向：{direction}")

    def update_volume_display(self) -> None:
        volume = self.volume_for_index(self.index_spin.value())
        difference = self.volume_difference_for_index(
            self.index_spin.value(), self.direction_combo.currentData()
        )
        self.volume_label.setText(f"{volume} mL")
        self.volume_difference_label.setText("-- mL" if difference is None else f"{difference:+d} mL")

    def advance_index(self) -> None:
        if self.direction_combo.currentData() == "down":
            self.index_spin.setValue(min(10, self.index_spin.value() + 1))
        else:
            self.index_spin.setValue(max(0, self.index_spin.value() - 1))

    def next_cycle(self) -> None:
        self.cycle_spin.setValue(self.cycle_spin.value() + 1)
        self.reset_index_for_direction()

    def delete_last_record(self) -> None:
        if not self.actions:
            QtWidgets.QMessageBox.information(self, "删除记录", "当前 CSV 中没有可删除的记录。")
            return
        timestamp, cycle, direction, height_index = self.actions.pop()
        record = next(
            item for item in self.records
            if item["cycle"] == cycle and item["height_index"] == height_index
        )
        record[f"{direction}_percent"] = None
        record[f"{direction}_timestamp"] = ""
        if record["down_percent"] is None and record["up_percent"] is None:
            self.records.remove(record)
        self.rewrite_csv()
        self.refresh_table()
        self.cycle_spin.setValue(cycle)
        direction_index = self.direction_combo.findData(direction)
        if direction_index >= 0:
            self.direction_combo.setCurrentIndex(direction_index)
        self.index_spin.setValue(height_index)

    def rewrite_csv(self) -> None:
        if self.csv_path is None:
            return
        if self.csv_file:
            self.csv_file.close()
        temporary = self.csv_path.with_suffix(self.csv_path.suffix + ".tmp")
        with open(temporary, "w", newline="", encoding="utf-8-sig") as target:
            writer = csv.writer(target)
            writer.writerow(self.CSV_HEADER)
            for record in sorted(self.records, key=lambda item: (item["cycle"], item["height_index"])):
                down = record["down_percent"]
                up = record["up_percent"]
                difference = round(up - down, 4) if down is not None and up is not None else ""
                writer.writerow([
                    record["cycle"], record["height_index"],
                    record["volume_ml"],
                    "" if self.volume_difference_for_index(record["height_index"], "down") is None
                    else self.volume_difference_for_index(record["height_index"], "down"),
                    "" if self.volume_difference_for_index(record["height_index"], "up") is None
                    else self.volume_difference_for_index(record["height_index"], "up"),
                    "" if down is None else down, "" if up is None else up, difference,
                    record["down_timestamp"], record["up_timestamp"],
                ])
            target.flush()
        temporary.replace(self.csv_path)

    def append_table_row(self, record: dict) -> None:
        table_row = self.table.rowCount()
        self.table.insertRow(table_row)
        down = record["down_percent"]
        up = record["up_percent"]
        difference = up - down if down is not None and up is not None else None
        display = [
            record["cycle"], record["height_index"],
            record["volume_ml"],
            "" if self.volume_difference_for_index(record["height_index"], "down") is None
            else self.volume_difference_for_index(record["height_index"], "down"),
            "" if self.volume_difference_for_index(record["height_index"], "up") is None
            else self.volume_difference_for_index(record["height_index"], "up"),
            "" if down is None else f"{down:.4f}",
            "" if up is None else f"{up:.4f}",
            "" if difference is None else f"{difference:+.4f}",
            record["down_timestamp"], record["up_timestamp"],
        ]
        for column, value in enumerate(display):
            self.table.setItem(table_row, column, QtWidgets.QTableWidgetItem(str(value)))

    def refresh_table(self) -> None:
        self.table.setRowCount(0)
        for record in sorted(self.records, key=lambda item: (item["cycle"], item["height_index"])):
            self.append_table_row(record)
        self.table.scrollToBottom()

    def load_existing_rows(self, path: Path) -> None:
        self.table.setRowCount(0)
        self.records = []
        self.actions = []
        try:
            if not path.exists() or path.stat().st_size == 0:
                return
            with open(path, "r", newline="", encoding="utf-8-sig") as source:
                all_rows = list(csv.reader(source))
            if not all_rows:
                return
            # Accept CSV files that were manually column-aligned with spaces.
            header = [cell.strip() for cell in all_rows[0]]
            rows = [[cell.strip() for cell in row] for row in all_rows[1:]]
            if header == self.LEGACY_HEADER:
                grouped: dict[tuple[int, int], dict] = {}
                for row in rows:
                    if len(row) != len(self.LEGACY_HEADER):
                        continue
                    timestamp, cycle_text, direction, index_text, level_text = row
                    cycle, height_index = int(cycle_text), int(index_text)
                    record = grouped.setdefault((cycle, height_index), {
                        "cycle": cycle, "height_index": height_index,
                        "volume_ml": self.volume_for_index(height_index),
                        "down_percent": None, "up_percent": None,
                        "down_timestamp": "", "up_timestamp": "",
                    })
                    record[f"{direction}_percent"] = float(level_text)
                    record[f"{direction}_timestamp"] = timestamp
                    self.actions.append((timestamp, cycle, direction, height_index))
                self.records = list(grouped.values())
            elif header in (self.CSV_HEADER, self.PAIRED_HEADER_V3, self.PAIRED_HEADER_V2, self.PAIRED_HEADER_V1):
                for row in rows:
                    if len(row) != len(header):
                        continue
                    cycle, height_index = int(row[0]), int(row[1])
                    if header in (self.CSV_HEADER, self.PAIRED_HEADER_V3):
                        down_index, up_index, down_time_index, up_time_index = 5, 6, 8, 9
                    elif header == self.PAIRED_HEADER_V2:
                        down_index, up_index, down_time_index, up_time_index = 3, 4, 6, 7
                    else:
                        down_index, up_index, down_time_index, up_time_index = 2, 3, 5, 6
                    record = {
                        "cycle": cycle, "height_index": height_index,
                        "volume_ml": self.volume_for_index(height_index),
                        "down_percent": float(row[down_index]) if row[down_index] else None,
                        "up_percent": float(row[up_index]) if row[up_index] else None,
                        "down_timestamp": row[down_time_index], "up_timestamp": row[up_time_index],
                    }
                    self.records.append(record)
                    if row[down_time_index]:
                        self.actions.append((row[down_time_index], cycle, "down", height_index))
                    if row[up_time_index]:
                        self.actions.append((row[up_time_index], cycle, "up", height_index))
            else:
                raise ValueError("CSV 表头格式不受支持")
            self.actions.sort(key=lambda action: action[0])
            self.refresh_table()
            self.restore_capture_position()
        except (OSError, ValueError):
            pass

    def restore_capture_position(self) -> None:
        """Continue at the step after the latest timestamped save."""
        if not self.actions:
            self.cycle_spin.setValue(1)
            self.set_direction_and_index("down", 0)
            return
        _, cycle, direction, height_index = self.actions[-1]
        if direction == "down" and height_index < 10:
            next_cycle, next_direction, next_index = cycle, "down", height_index + 1
        elif direction == "down":
            next_cycle, next_direction, next_index = cycle, "up", 10
        elif height_index > 0:
            next_cycle, next_direction, next_index = cycle, "up", height_index - 1
        else:
            next_cycle, next_direction, next_index = cycle + 1, "down", 0
        self.cycle_spin.setValue(next_cycle)
        self.set_direction_and_index(next_direction, next_index)

    def set_direction_and_index(self, direction: str, height_index: int) -> None:
        direction_index = self.direction_combo.findData(direction)
        self.direction_combo.blockSignals(True)
        if direction_index >= 0:
            self.direction_combo.setCurrentIndex(direction_index)
        self.direction_combo.blockSignals(False)
        self.index_spin.setValue(height_index)
        self.update_volume_display()

    def close_csv(self) -> None:
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = self.csv_writer = None


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("MCP1081 双路八通道连续水位分析")
        self.resize(1500, 930)
        self.parser = MCP1081Parser()
        self.algorithms = {"A": WaterLevelAlgorithm(), "B": WaterLevelAlgorithm()}
        self.algorithm = self.algorithms["A"]  # Backward-compatible alias for A.
        self.worker: Optional[SerialWorker] = None
        self.frames: deque[SensorFrame] = deque(maxlen=500)
        self.plot_time: deque[float] = deque(maxlen=PLOT_HISTORY_FRAMES)
        self.plot_levels = {part: deque(maxlen=PLOT_HISTORY_FRAMES) for part in PART_CHANNELS}
        self.plot_caps = {channel: deque(maxlen=PLOT_HISTORY_FRAMES) for channel in (0, 1, 2, 3, 6, 7, 8, 9)}
        self.start_monotonic = time.monotonic()
        self.csv_file = None
        self.csv_writer = None
        self.frame_count = 0
        self.latest_results: dict[str, AlgorithmResult] = {}
        self.cycle_analysis_window: Optional[CycleAnalysisWindow] = None
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
        self.cycle_analysis_button = QtWidgets.QPushButton("A路升降循环分析")
        self.status_label.setStyleSheet("color:#b42318;font-weight:bold")
        for label, widget in (("端口", self.port_combo), ("波特率", self.baud_combo)):
            connection_layout.addWidget(QtWidgets.QLabel(label))
            connection_layout.addWidget(widget)
        connection_layout.addWidget(self.refresh_button)
        connection_layout.addWidget(self.connect_button)
        connection_layout.addWidget(self.cycle_analysis_button)
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
        self.level_displays = {}
        for part, color in (("A", "#1565c0"), ("B", "#d97706")):
            lcd = QtWidgets.QLCDNumber()
            lcd.setDigitCount(6)
            lcd.display(0.0)
            lcd.setMinimumHeight(80)
            bar = QtWidgets.QProgressBar()
            bar.setRange(0, 1000)
            bar.setFormat(f"{part}路 %p%")
            level_layout.addWidget(QtWidgets.QLabel(f"{part}路水位 %"))
            level_layout.addWidget(lcd, 1)
            level_layout.addWidget(bar, 2)
            self.level_displays[part] = (lcd, bar)
        self.level_lcd, self.level_bar = self.level_displays["A"]
        self.temp_label = QtWidgets.QLabel("温度: -- °C")
        self.frame_label = QtWidgets.QLabel("有效帧: 0")
        level_layout.addWidget(self.temp_label)
        level_layout.addWidget(self.frame_label)
        left_layout.addWidget(level_box)

        self.level_plot = pg.PlotWidget(title=f"连续水位（最近 {DEFAULT_DISPLAY_FRAMES} 帧）")
        self.level_plot.setLabel("left", "水位", units="%")
        self.level_plot.setLabel("bottom", "运行时间", units="s")
        self.level_plot.setYRange(0, 100)
        self.level_plot.showGrid(x=True, y=True, alpha=0.25)
        self.level_plot.addLegend()
        self.level_curves = {
            "A": self.level_plot.plot(pen=pg.mkPen("#1565c0", width=2), name="A路 CH0-CH3"),
            "B": self.level_plot.plot(pen=pg.mkPen("#d97706", width=2), name="B路 CH6-CH9"),
        }
        self.level_curve = self.level_curves["A"]
        left_layout.addWidget(self.level_plot, 2)

        self.cap_plot = pg.PlotWidget(title="A路 CH0-CH3 原始电容")
        self.cap_plot.setLabel("left", "电容", units="pF")
        self.cap_plot.setLabel("bottom", "运行时间", units="s")
        self.cap_plot.showGrid(x=True, y=True, alpha=0.25)
        self.cap_plot.addLegend()
        colors = ["#e76f51", "#2a9d8f", "#457b9d", "#8e6bbd", "#f4a261", "#00a6a6", "#264653", "#6a4c93"]
        channels = (0, 1, 2, 3, 6, 7, 8, 9)
        self.cap_curves = {
            channel: self.cap_plot.plot(pen=pg.mkPen(colors[i], width=1.5), name=f"CH{channel}")
            for i, channel in enumerate(channels)
        }
        cap_controls = QtWidgets.QHBoxLayout()
        self.cap_view_button = QtWidgets.QPushButton("切换到 CH0 基准偏移图")
        self.cap_view_button.setCheckable(True)
        self.cap_view_hint = QtWidgets.QLabel("当前：原始电容（算法始终使用原始值）")
        self.cap_part_combo = QtWidgets.QComboBox()
        self.cap_part_combo.addItem("A路（CH0-CH3）", "A")
        self.cap_part_combo.addItem("B路（CH6-CH9）", "B")
        self.cap_part_combo.addItem("两路同时", "AB")
        self.display_frames_spin = QtWidgets.QSpinBox()
        self.display_frames_spin.setRange(10, PLOT_HISTORY_FRAMES)
        self.display_frames_spin.setSingleStep(10)
        self.display_frames_spin.setValue(DEFAULT_DISPLAY_FRAMES)
        self.display_frames_spin.setSuffix(" 帧")
        self.display_frames_spin.setToolTip("两张实时图显示的最近数据帧数量；内部最多保留 10000 帧")
        cap_controls.addWidget(self.cap_view_button)
        cap_controls.addWidget(QtWidgets.QLabel("电容图"))
        cap_controls.addWidget(self.cap_part_combo)
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
            "① 同步采集：A路 CH0-CH3，B路 CH6-CH9（CH4/CH5 不参与）",
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
        self.cal_part_combo = QtWidgets.QComboBox()
        self.cal_part_combo.addItem("A路（CH0-CH3）", "A")
        self.cal_part_combo.addItem("B路（CH6-CH9）", "B")
        self.detail_part_combo = QtWidgets.QComboBox()
        self.detail_part_combo.addItem("显示 A 路算法详情", "A")
        self.detail_part_combo.addItem("显示 B 路算法详情", "B")
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
        filter_layout.addWidget(self.cal_part_combo, 2, 0, 1, 2)
        filter_layout.addWidget(self.empty_button, 1, 2)
        filter_layout.addWidget(self.full_button, 1, 3)
        filter_layout.addWidget(self.clear_cal_button, 1, 4)
        filter_layout.addWidget(self.save_cal_button, 2, 2)
        filter_layout.addWidget(self.load_cal_button, 2, 3)
        filter_layout.addWidget(self.detail_part_combo, 2, 4)
        filter_layout.addWidget(self.cal_status, 3, 0, 1, 5)
        right_layout.addWidget(filter_box)

        self.data_table = QtWidgets.QTableWidget(8, 9)
        self.data_table.setHorizontalHeaderLabels([
            "通道", "原始 C/pF", "空杯 C0", "方向", "原始 ΔC", "中值 ΔC",
            "平均 ΔC", "满量程 ΔC", "水位贡献/%",
        ])
        self.data_table.verticalHeader().setVisible(False)
        self.data_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        for row, channel in enumerate((0, 1, 2, 3, 6, 7, 8, 9)):
            self.data_table.setItem(row, 0, QtWidgets.QTableWidgetItem(f"CH{channel}"))
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
        self.cycle_analysis_button.clicked.connect(self.show_cycle_analysis)
        self.empty_button.clicked.connect(lambda: self.capture_calibration(False))
        self.full_button.clicked.connect(lambda: self.capture_calibration(True))
        self.clear_cal_button.clicked.connect(self.clear_calibration)
        self.save_cal_button.clicked.connect(self.save_calibration_as)
        self.load_cal_button.clicked.connect(self.load_calibration_from)
        self.clear_log_button.clicked.connect(self.serial_log.clear)
        self.csv_button.clicked.connect(self.toggle_csv)
        self.cap_view_button.toggled.connect(self.toggle_cap_view)
        self.cap_part_combo.currentIndexChanged.connect(lambda: self.toggle_cap_view(self.cap_view_button.isChecked()))
        self.detail_part_combo.currentIndexChanged.connect(self.refresh_latest_detail)
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
        results = {
            part: self.algorithms[part].process(tuple(frame.cap_pf[i] for i in channels), frame.timestamp)
            for part, channels in PART_CHANNELS.items()
        }
        self.latest_results = results
        if self.cycle_analysis_window is not None:
            # Cycle analysis must preserve the unbounded algorithm result for
            # calibration/error analysis; do not store the 0..100 UI clamp.
            self.cycle_analysis_window.update_level(results["A"].raw_level_percent)
        self.update_result(frame, results)
        self.write_csv(frame, results)

    def update_result(self, frame: SensorFrame, results: dict[str, AlgorithmResult]) -> None:
        for part, result in results.items():
            lcd, bar = self.level_displays[part]
            lcd.display(f"{result.level_percent:.2f}")
            bar.setValue(round(result.level_percent * 10))
        self.temp_label.setText(
            f"温度: {frame.temperature_c:.1f} °C" if frame.temperature_c is not None else "温度: -- °C"
        )
        self.frame_label.setText(f"有效帧: {self.frame_count}")
        for part_index, part in enumerate(("A", "B")):
            result = results[part]
            channels = PART_CHANNELS[part]
            for local_index, channel in enumerate(channels):
                row = part_index * 4 + local_index
                values = [
                    f"{result.raw_cap[local_index]:.4f}", f"{result.empty_baseline[local_index]:.4f}",
                    "+" if result.direction[local_index] > 0 else "−", f"{result.raw_delta[local_index]:.4f}",
                    f"{result.median_delta[local_index]:.4f}", f"{result.average_delta[local_index]:.4f}",
                    f"{result.full_span[local_index]:.4f}", f"{result.contributions[local_index]:.2f}",
                ]
                for column, value in enumerate(values, start=1):
                    self.data_table.setItem(row, column, QtWidgets.QTableWidgetItem(value))
        self.refresh_latest_detail()

        elapsed = time.monotonic() - self.start_monotonic
        self.plot_time.append(elapsed)
        for part, result in results.items():
            self.plot_levels[part].append(result.level_percent)
        for channel in self.plot_caps:
            self.plot_caps[channel].append(frame.cap_pf[channel])
        self.redraw_plots()

    def refresh_latest_detail(self) -> None:
        part = self.detail_part_combo.currentData() or "A"
        if part not in self.latest_results:
            return
        result = self.latest_results[part]
        channels = PART_CHANNELS[part]
        caps = ", ".join(f"C{channels[i]}={value:.3f}" for i, value in enumerate(result.raw_cap))
        deltas = ", ".join(f"ΔC{channels[i]}={value:.4f}" for i, value in enumerate(result.raw_delta))
        medians = ", ".join(f"{value:.4f}" for value in result.median_delta)
        averages = ", ".join(f"{value:.4f}" for value in result.average_delta)
        algorithm = self.algorithms[part]
        self.step_labels[0].setText(f"① {part}路同步采集：{caps} pF")
        self.step_labels[1].setText(f"② 空杯差值：{deltas} pF")
        self.step_labels[2].setText(f"③ 中值滤波（N={algorithm.median_window}）：[{medians}] pF")
        self.step_labels[3].setText(f"④ 滑动平均（M={algorithm.average_window}）：[{averages}] pF")
        self.step_labels[4].setText(f"⑤ 四通道求和：ΔCsum = {result.delta_sum:.5f} pF")
        self.step_labels[5].setText(f"⑥ 满量程：ΔC100 = {result.full_sum:.5f} pF")
        self.step_labels[6].setText(
            f"⑦ {part}路归一限幅：原始 {result.raw_level_percent:.2f}% → 输出 {result.level_percent:.2f}%"
        )

    def cap_plot_offsets(self, part: str) -> dict[int, float]:
        channels = PART_CHANNELS[part]
        algorithm = self.algorithms[part]
        if algorithm.empty_baseline is not None:
            baseline = algorithm.empty_baseline
        elif all(self.plot_caps[channel] for channel in channels):
            baseline = tuple(self.plot_caps[channel][0] for channel in channels)
        else:
            return {channel: 0.0 for channel in channels}
        return {channel: baseline[0] - baseline[i] for i, channel in enumerate(channels)}

    def redraw_cap_plot(self) -> None:
        count = self.display_frames_spin.value()
        x = list(self.plot_time)[-count:]
        aligned = self.cap_view_button.isChecked()
        selection = self.cap_part_combo.currentData() or "A"
        visible_parts = ("A", "B") if selection == "AB" else (selection,)
        visible_channels = {channel for part in visible_parts for channel in PART_CHANNELS[part]}
        offsets = {channel: 0.0 for channel in self.cap_curves}
        if aligned:
            for part in visible_parts:
                offsets.update(self.cap_plot_offsets(part))
        for channel, curve in self.cap_curves.items():
            curve.setVisible(channel in visible_channels)
            values = [value + offsets[channel] for value in list(self.plot_caps[channel])[-count:]]
            curve.setData(x, values)

    def redraw_plots(self) -> None:
        count = self.display_frames_spin.value()
        x = list(self.plot_time)[-count:]
        for part, curve in self.level_curves.items():
            curve.setData(x, list(self.plot_levels[part])[-count:])
        self.redraw_cap_plot()

    def update_display_frames(self, count: int) -> None:
        self.level_plot.setTitle(f"连续水位（最近 {count} 帧）")
        self.redraw_plots()

    def toggle_cap_view(self, aligned: bool) -> None:
        selection = self.cap_part_combo.currentData() or "A"
        selected_parts = ("A", "B") if selection == "AB" else (selection,)
        if aligned:
            offsets = {channel: value for part in selected_parts for channel, value in self.cap_plot_offsets(part).items()}
            offset_text = ", ".join(f"CH{i} {value:+.4f}" for i, value in offsets.items())
            self.cap_plot.setTitle(f"{selection}路电容（各路偏移到首通道初始基准）")
            self.cap_view_button.setText("切换到原始图")
            self.cap_view_hint.setText(f"当前：偏移图；offset/pF：{offset_text}")
        else:
            self.cap_plot.setTitle(f"{selection}路原始电容")
            self.cap_view_button.setText("切换到各路基准偏移图")
            self.cap_view_hint.setText("当前：原始电容（算法始终使用原始值）")
        self.redraw_cap_plot()

    def capture_calibration(self, full: bool) -> None:
        part = self.cal_part_combo.currentData() or "A"
        channels = PART_CHANNELS[part]
        algorithm = self.algorithms[part]
        count = self.cal_count_spin.value()
        if len(self.frames) < count:
            QtWidgets.QMessageBox.warning(self, "标定", f"需要至少 {count} 个有效帧，当前只有 {len(self.frames)} 个。")
            return
        frames = list(self.frames)[-count:]
        caps = tuple(fmean(frame.cap_pf[channel] for frame in frames) for channel in channels)
        if full:
            if algorithm.empty_baseline is None:
                QtWidgets.QMessageBox.warning(self, "标定", f"请先采集 {part} 路空杯标定。")
                return
            algorithm.set_full(caps)
        else:
            algorithm.set_empty(caps)
            algorithm.full_calibration = None
        self._save_calibration()
        self.update_calibration_status()
        if self.cap_view_button.isChecked():
            self.toggle_cap_view(True)

    def clear_calibration(self) -> None:
        part = self.cal_part_combo.currentData() or "A"
        self.algorithms[part].clear_calibration()
        if any(algorithm.empty_baseline is not None for algorithm in self.algorithms.values()):
            self._save_calibration()
        elif CALIBRATION_FILE.exists():
            CALIBRATION_FILE.unlink()
        self.update_calibration_status()
        if self.cap_view_button.isChecked():
            self.toggle_cap_view(True)

    def update_filter_windows(self) -> None:
        for algorithm in self.algorithms.values():
            algorithm.configure_windows(self.median_spin.value(), self.average_spin.value())

    def update_calibration_status(self) -> None:
        summaries = []
        all_calibrated = True
        for part, algorithm in self.algorithms.items():
            if algorithm.calibrated:
                spans = [abs(algorithm.full_calibration[i] - algorithm.empty_baseline[i]) for i in range(4)]
                summaries.append(f"{part}路完成 ΔC=[" + ",".join(f"{v:.3f}" for v in spans) + "]")
            elif algorithm.empty_baseline is not None:
                all_calibrated = False
                summaries.append(f"{part}路仅空杯")
            else:
                all_calibrated = False
                summaries.append(f"{part}路未标定")
        self.cal_status.setText(" | ".join(summaries))
        if all_calibrated:
            self.cal_status.setStyleSheet("color:#067647;font-weight:bold")
        elif any(algorithm.empty_baseline is not None for algorithm in self.algorithms.values()):
            self.cal_status.setStyleSheet("color:#b54708;font-weight:bold")
        else:
            self.cal_status.setStyleSheet("color:#b42318;font-weight:bold")

    def calibration_data(self) -> dict:
        return {
            "format": "mcp1081-water-level-calibration",
            "version": 2,
            "parts": {
                part: {"channels": PART_CHANNELS[part], "empty_pf": algorithm.empty_baseline,
                       "full_pf": algorithm.full_calibration}
                for part, algorithm in self.algorithms.items()
            },
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
        parts = data.get("parts")
        if not isinstance(parts, dict):
            # Version 1 files contained only CH0-CH3; load them as A route.
            parts = {"A": data}
        applied = 0
        for part in ("A", "B"):
            part_data = parts.get(part)
            if part_data is None:
                continue
            if not isinstance(part_data, dict):
                raise ValueError(f"{part}路标定内容无效")
            if part_data.get("empty_pf") is None:
                continue
            empty = self._validated_calibration_values(part_data, "empty_pf")
            self.algorithms[part].set_empty(empty)
            if part_data.get("full_pf") is not None:
                full = self._validated_calibration_values(part_data, "full_pf")
                if sum(abs(full[i] - empty[i]) for i in range(4)) <= 1e-12:
                    raise ValueError(f"{part}路空杯和满杯标定值完全相同")
                self.algorithms[part].set_full(full)
            applied += 1
        if not applied:
            raise ValueError("文件中没有可用的 A/B 路标定")
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
        if not all(algorithm.calibrated for algorithm in self.algorithms.values()):
            QtWidgets.QMessageBox.warning(self, "保存标定", "请先完成 A、B 两路的空杯和满杯标定。")
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
        for part in ("A", "B"):
            for prefix in ("raw_cap_pf", "empty_pf", "raw_delta_pf", "median_delta_pf", "average_delta_pf", "full_span_pf"):
                header.extend(f"{part}_{prefix}_ch{channel}" for channel in PART_CHANNELS[part])
            header.extend([f"{part}_delta_sum_pf", f"{part}_full_sum_pf", f"{part}_raw_level_percent", f"{part}_level_percent"])
        self.csv_writer.writerow(header)
        self.csv_button.setText("停止 CSV 记录")

    def write_csv(self, frame: SensorFrame, results: dict[str, AlgorithmResult]) -> None:
        if not self.csv_writer:
            return
        row = [frame.timestamp.isoformat(timespec="milliseconds"), frame.temperature_c, frame.data_ref]
        for part in ("A", "B"):
            result = results[part]
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

    def show_cycle_analysis(self) -> None:
        if self.cycle_analysis_window is None:
            self.cycle_analysis_window = CycleAnalysisWindow(self)
        if "A" in self.latest_results:
            self.cycle_analysis_window.update_level(self.latest_results["A"].raw_level_percent)
        self.cycle_analysis_window.show()
        self.cycle_analysis_window.raise_()
        self.cycle_analysis_window.activateWindow()

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:
        self.disconnect_serial()
        if self.csv_file:
            self.csv_file.close()
        if self.cycle_analysis_window is not None:
            self.cycle_analysis_window.close_csv()
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
