from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

if __package__:
    from . import config
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from script import config


DEFAULT_SOURCE = (
    config.MCU_ROOT / "faqiuji_mcu071" / "Src" / "花海.mp3"
)
DEFAULT_OUTPUT = config.TEST_AUDIO_BIN_DEFAULT
DEFAULT_START_TIME = "1:12"
DEFAULT_END_TIME = "1:17"
HEADER = struct.Struct("<4sIII")


def parse_timestamp(value: str) -> float:
    parts = value.strip().split(":")
    if len(parts) == 1:
        seconds = float(parts[0])
    elif len(parts) == 2:
        minutes = int(parts[0])
        remainder = float(parts[1])
        if minutes < 0 or not 0 <= remainder < 60:
            raise ValueError(f"Invalid timestamp: {value}")
        seconds = minutes * 60 + remainder
    elif len(parts) == 3:
        hours = int(parts[0])
        minutes = int(parts[1])
        remainder = float(parts[2])
        if hours < 0 or not 0 <= minutes < 60 or not 0 <= remainder < 60:
            raise ValueError(f"Invalid timestamp: {value}")
        seconds = hours * 3600 + minutes * 60 + remainder
    else:
        raise ValueError(f"Invalid timestamp: {value}")

    if seconds < 0:
        raise ValueError(f"Invalid timestamp: {value}")
    return seconds


def extract_audio(
    source: Path,
    output: Path,
    ffmpeg: str,
    start_time: str,
    end_time: str,
) -> None:
    source = source.expanduser().resolve()
    output = output.expanduser().resolve()
    start_seconds = parse_timestamp(start_time)
    end_seconds = parse_timestamp(end_time)
    if end_seconds <= start_seconds:
        raise ValueError("End time must be later than start time.")
    if not source.is_file():
        raise FileNotFoundError(f"MP3 file not found: {source}")
    if not shutil.which(ffmpeg):
        raise RuntimeError(
            "ffmpeg was not found. Install ffmpeg in the active Conda environment "
            "or pass --ffmpeg with its executable path."
        )

    with tempfile.TemporaryDirectory(prefix="faqiuji-audio-") as directory:
        pcm_path = Path(directory) / "clip.pcm"
        command = [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(source),
            "-ss",
            str(start_seconds),
            "-t",
            str(end_seconds - start_seconds),
            "-vn",
            "-ac",
            "1",
            "-ar",
            str(config.TEST_AUDIO_SAMPLE_RATE),
            "-f",
            "u8",
            str(pcm_path),
        ]
        subprocess.run(command, check=True)
        pcm = pcm_path.read_bytes()

    if not pcm:
        raise RuntimeError("ffmpeg produced an empty PCM clip.")
    if len(pcm) + config.TEST_AUDIO_HEADER_SIZE > config.TEST_AUDIO_REGION.size:
        raise RuntimeError(
            f"Extracted audio is too large: {len(pcm)} PCM bytes; "
            f"maximum is {config.TEST_AUDIO_REGION.size - config.TEST_AUDIO_HEADER_SIZE}."
        )

    header = HEADER.pack(
        b"FQA1",
        config.TEST_AUDIO_SAMPLE_RATE,
        len(pcm),
        1,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + pcm)
    print(f"Source:      {source}")
    print(f"Output:      {output}")
    print(f"Clip:        {start_time}..{end_time}")
    print(f"PCM format:  {config.TEST_AUDIO_SAMPLE_RATE} Hz, 8-bit, mono")
    print(f"Duration:    {len(pcm) / config.TEST_AUDIO_SAMPLE_RATE:.2f} seconds")
    print(f"Image size:  {len(header) + len(pcm)} bytes")
    print(
        f"Flash area:  0x{config.TEST_AUDIO_BASE:08X}.."
        f"0x{config.TEST_AUDIO_BASE + config.TEST_AUDIO_SIZE - 1:08X}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract a short PCM test clip for the PY32F071 DAC demo."
    )
    parser.add_argument("--input", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--start",
        default=DEFAULT_START_TIME,
        help="Clip start (seconds, MM:SS, or HH:MM:SS). Default: 1:12.",
    )
    parser.add_argument(
        "--end",
        default=DEFAULT_END_TIME,
        help="Clip end (seconds, MM:SS, or HH:MM:SS). Default: 1:17.",
    )
    parser.add_argument("--ffmpeg", default=r"C:\Users\JampMar\Downloads\ffmpeg-2026-09-21-git-a9cbcc2bbb-essentials_build\ffmpeg-2026-09-21-git-a9cbcc2bbb-essentials_build\bin\ffmpeg.exe")
    args = parser.parse_args()
    try:
        extract_audio(args.input, args.output, args.ffmpeg, args.start, args.end)
    except (
        FileNotFoundError,
        RuntimeError,
        ValueError,
        subprocess.CalledProcessError,
    ) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
