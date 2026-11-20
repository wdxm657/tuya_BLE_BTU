"""Serial parsing and four-channel continuous water-level algorithm."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime
from statistics import fmean, median
import re
from typing import Deque, Iterable, Optional, Sequence


SENSOR_CHANNEL_COUNT = 10
ALGORITHM_CHANNEL_COUNT = 4
CHANNEL_COUNT = ALGORITHM_CHANNEL_COUNT
CREF_PF = 20.0


@dataclass(frozen=True)
class SensorFrame:
    timestamp: datetime
    temperature_c: Optional[float]
    data_ref: Optional[int]
    freq_ref_mhz: Optional[float]
    data_ch: tuple[Optional[int], ...]
    freq_ch_mhz: tuple[Optional[float], ...]
    cap_pf: tuple[float, ...]
    source: str


@dataclass(frozen=True)
class AlgorithmResult:
    timestamp: datetime
    raw_cap: tuple[float, ...]
    empty_baseline: tuple[float, ...]
    direction: tuple[int, ...]
    raw_delta: tuple[float, ...]
    median_delta: tuple[float, ...]
    average_delta: tuple[float, ...]
    full_span: tuple[float, ...]
    contributions: tuple[float, ...]
    delta_sum: float
    full_sum: float
    raw_level_percent: float
    level_percent: float


class MCP1081Parser:
    """Parse both verbose MCP1081 logs and compact S,... frames."""

    _temp_re = re.compile(r"\bT:\s*([-+]?\d+(?:\.\d+)?)")
    _ref_re = re.compile(
        r"data_ref:\s*(\d+)(?:\s+freq_ref:\s*([-+]?\d+(?:\.\d+)?)\s*MHz)?"
    )
    _ch_re = re.compile(
        r"data_ch\[(\d+)\]:\s*(\d+)"
        r"(?:\s+freq_ch\[\d+\]:\s*([-+]?\d+(?:\.\d+)?)\s*MHz)?"
        r"(?:\s+cap_ch\[\d+\]:\s*([-+]?\d+(?:\.\d+)?)\s*pF)?"
    )

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self._temperature: Optional[float] = None
        self._data_ref: Optional[int] = None
        self._freq_ref: Optional[float] = None
        self._data_ch: list[Optional[int]] = [None] * SENSOR_CHANNEL_COUNT
        self._freq_ch: list[Optional[float]] = [None] * SENSOR_CHANNEL_COUNT
        self._cap_ch: list[Optional[float]] = [None] * SENSOR_CHANNEL_COUNT

    def feed_line(self, line: str) -> Optional[SensorFrame]:
        text = line.strip()
        if not text:
            return None
        if text.startswith("S,"):
            return self._parse_compact(text)

        match = self._temp_re.search(text)
        if match:
            # A temperature line starts a new verbose measurement block.
            self.reset()
            self._temperature = float(match.group(1))
            return None

        match = self._ref_re.search(text)
        if match:
            self._data_ref = int(match.group(1))
            self._freq_ref = float(match.group(2)) if match.group(2) else None
            return None

        match = self._ch_re.search(text)
        if not match:
            return None
        channel = int(match.group(1))
        if not 0 <= channel < SENSOR_CHANNEL_COUNT:
            return None
        self._data_ch[channel] = int(match.group(2))
        self._freq_ch[channel] = float(match.group(3)) if match.group(3) else None
        if match.group(4):
            self._cap_ch[channel] = float(match.group(4))
        elif self._data_ref:
            self._cap_ch[channel] = self._data_ch[channel] / self._data_ref * CREF_PF

        if all(value is not None for value in self._cap_ch):
            frame = self._build_frame("verbose")
            self.reset()
            return frame
        return None

    def _parse_compact(self, text: str) -> Optional[SensorFrame]:
        fields = [field.strip() for field in text.split(",")]
        # Firmware format: S,ref,ch0,...,ch9,temp
        if len(fields) < 13:
            return None
        try:
            data_ref = int(fields[1])
            channel_data = [int(value) for value in fields[2:12]]
            temperature = float(fields[12]) if len(fields) > 12 else None
        except (ValueError, IndexError):
            return None
        if data_ref <= 0:
            return None
        caps = [value / data_ref * CREF_PF for value in channel_data]
        return SensorFrame(
            timestamp=datetime.now(),
            temperature_c=temperature,
            data_ref=data_ref,
            freq_ref_mhz=None,
            data_ch=tuple(channel_data),
            freq_ch_mhz=(None,) * SENSOR_CHANNEL_COUNT,
            cap_pf=tuple(caps),
            source="compact",
        )

    def _build_frame(self, source: str) -> SensorFrame:
        return SensorFrame(
            timestamp=datetime.now(),
            temperature_c=self._temperature,
            data_ref=self._data_ref,
            freq_ref_mhz=self._freq_ref,
            data_ch=tuple(self._data_ch),
            freq_ch_mhz=tuple(self._freq_ch),
            cap_pf=tuple(float(value) for value in self._cap_ch),  # type: ignore[arg-type]
            source=source,
        )


class WaterLevelAlgorithm:
    """Observable implementation of the four-channel sum-normalization algorithm.

    Pipeline per channel:
      cap -> signed delta from empty -> median -> moving average -> sum/normalize.
    Direction is learned from full versus empty calibration, allowing either
    increasing or decreasing capacitance sensors.
    """

    def __init__(self, median_window: int = 5, average_window: int = 10) -> None:
        self.empty_baseline: Optional[tuple[float, ...]] = None
        self.full_calibration: Optional[tuple[float, ...]] = None
        self.median_window = max(1, int(median_window))
        self.average_window = max(1, int(average_window))
        self._median_buffers: list[Deque[float]] = []
        self._average_buffers: list[Deque[float]] = []
        self.reset_filters()

    @property
    def calibrated(self) -> bool:
        return self.empty_baseline is not None and self.full_calibration is not None

    def configure_windows(self, median_window: int, average_window: int) -> None:
        median_window = max(1, int(median_window))
        average_window = max(1, int(average_window))
        if (median_window, average_window) != (self.median_window, self.average_window):
            self.median_window = median_window
            self.average_window = average_window
            self.reset_filters()

    def set_empty(self, caps: Sequence[float]) -> None:
        self.empty_baseline = self._validate_caps(caps)
        self.reset_filters()

    def set_full(self, caps: Sequence[float]) -> None:
        self.full_calibration = self._validate_caps(caps)
        self.reset_filters()

    def clear_calibration(self) -> None:
        self.empty_baseline = None
        self.full_calibration = None
        self.reset_filters()

    def reset_filters(self) -> None:
        self._median_buffers = [deque(maxlen=self.median_window) for _ in range(ALGORITHM_CHANNEL_COUNT)]
        self._average_buffers = [deque(maxlen=self.average_window) for _ in range(ALGORITHM_CHANNEL_COUNT)]

    def process(self, caps: Sequence[float], timestamp: Optional[datetime] = None) -> AlgorithmResult:
        raw_cap = self._validate_caps(caps)
        empty = self.empty_baseline or raw_cap
        full = self.full_calibration or tuple(value + 1.0 for value in empty)
        direction = tuple(1 if full[i] >= empty[i] else -1 for i in range(ALGORITHM_CHANNEL_COUNT))
        raw_delta = tuple(direction[i] * (raw_cap[i] - empty[i]) for i in range(ALGORITHM_CHANNEL_COUNT))
        full_span = tuple(abs(full[i] - empty[i]) for i in range(ALGORITHM_CHANNEL_COUNT))

        medians: list[float] = []
        averages: list[float] = []
        for i, delta in enumerate(raw_delta):
            self._median_buffers[i].append(delta)
            median_value = float(median(self._median_buffers[i]))
            self._average_buffers[i].append(median_value)
            medians.append(median_value)
            averages.append(float(fmean(self._average_buffers[i])))

        delta_sum = sum(averages)
        full_sum = sum(full_span)
        raw_level = 100.0 * delta_sum / full_sum if self.calibrated and full_sum > 1e-12 else 0.0
        level = min(100.0, max(0.0, raw_level))
        contributions = tuple(
            100.0 * averages[i] / full_sum if self.calibrated and full_sum > 1e-12 else 0.0
            for i in range(ALGORITHM_CHANNEL_COUNT)
        )
        return AlgorithmResult(
            timestamp=timestamp or datetime.now(), raw_cap=raw_cap, empty_baseline=empty,
            direction=direction, raw_delta=raw_delta, median_delta=tuple(medians),
            average_delta=tuple(averages), full_span=full_span, contributions=contributions,
            delta_sum=delta_sum, full_sum=full_sum, raw_level_percent=raw_level,
            level_percent=level,
        )

    @staticmethod
    def average_frames(frames: Iterable[SensorFrame]) -> tuple[float, ...]:
        items = list(frames)
        if not items:
            raise ValueError("没有可用于标定的有效数据帧")
        return tuple(fmean(frame.cap_pf[i] for frame in items) for i in range(ALGORITHM_CHANNEL_COUNT))

    @staticmethod
    def _validate_caps(caps: Sequence[float]) -> tuple[float, ...]:
        if len(caps) != ALGORITHM_CHANNEL_COUNT:
            raise ValueError("必须提供 4 个通道的电容值")
        return tuple(float(value) for value in caps)
