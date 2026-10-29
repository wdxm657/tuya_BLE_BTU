#!/usr/bin/env python3
"""Command-line entry point for programming a BIN into APP_B."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from script import config
from script.pyocd_backend import FlashError, PyOcdBackend


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Program an APP A-linked PY32F040 image into APP_B."
    )
    parser.add_argument(
        "--bin",
        type=Path,
        default=config.APP_B_BIN_DEFAULT,
        help="APP_B BIN file.",
    )
    parser.add_argument("--pack", type=Path, default=config.default_pack_path())
    parser.add_argument("--probe", help="SWD probe UID.")
    parser.add_argument(
        "--frequency",
        type=int,
        default=config.SWD_FREQUENCY_HZ,
        help="SWD frequency in Hz.",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.pack is None:
        parser.error("Puya CMSIS-Pack not found; pass --pack with its directory.")
    try:
        image = PyOcdBackend._validate_image("app_b", args.bin.resolve())
    except (OSError, FlashError) as exc:
        parser.error(str(exc))

    print(f"Image: {args.bin.resolve()}")
    print(f"Image size: {len(image)} bytes")
    print(
        f"Target address: 0x{config.APP_B_BASE:08X}.."
        f"0x{config.APP_B_BASE + config.APP_B_SIZE - 1:08X}"
    )
    if args.dry_run:
        print("Dry run: image validated; no probe was accessed.")
        return 0

    try:
        backend = PyOcdBackend(
            pack_path=args.pack,
            probe_uid=args.probe,
            frequency=args.frequency,
            log=print,
        )
        backend.program("app_b", args.bin)
    except Exception as exc:
        print(f"Programming failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
