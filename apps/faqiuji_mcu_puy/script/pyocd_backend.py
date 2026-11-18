from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import tempfile
import struct
import wave
from typing import Callable, Iterator

from . import config


LogCallback = Callable[[str], None]


class FlashError(RuntimeError):
    pass


def _pyocd_api():
    try:
        from pyocd.core.helpers import ConnectHelper
        from pyocd.flash.eraser import FlashEraser
        from pyocd.flash.file_programmer import FileProgrammer
    except ImportError as exc:
        raise FlashError(
            "pyOCD is not installed. Run: python -m pip install -r requirements.txt"
        ) from exc
    return ConnectHelper, FlashEraser, FileProgrammer


def list_probes() -> list[tuple[str, str]]:
    ConnectHelper, _, _ = _pyocd_api()
    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    return [
        (
            probe.unique_id or "",
            " ".join(
                part
                for part in (probe.product_name, probe.vendor_name, probe.unique_id)
                if part
            ),
        )
        for probe in probes
    ]


class PyOcdBackend:
    def __init__(
        self,
        pack_path: Path,
        probe_uid: str | None = None,
        frequency: int = config.SWD_FREQUENCY_HZ,
        target_name: str = config.DEFAULT_TARGET_NAME,
        log: LogCallback | None = None,
    ) -> None:
        self.pack_path = pack_path.expanduser().resolve()
        self.probe_uid = probe_uid or None
        self.frequency = frequency
        if target_name not in config.TARGETS:
            raise FlashError(f"Unsupported target: {target_name}")
        self.target_name = target_name
        self.log = log or (lambda _message: None)

        if not self.pack_path.exists():
            raise FlashError(f"Puya CMSIS-Pack not found: {self.pack_path}")
        if self.frequency <= 0:
            raise FlashError("SWD frequency must be positive.")

    @contextmanager
    def _session(self, *, attach: bool = False) -> Iterator[object]:
        ConnectHelper, _, _ = _pyocd_api()
        options = {
            "target_override": self.target_name,
            "pack": [str(self.pack_path)],
            "frequency": self.frequency,
            "chip_erase": "sector",
            "auto_unlock": False,
            "resume_on_disconnect": False,
            "cache.enable_memory": False,
        }
        if attach:
            options["connect_mode"] = "attach"
        else:
            options["reset_type"] = "sysresetreq"
        probe_uid = self.probe_uid
        if probe_uid is None:
            probes = ConnectHelper.get_all_connected_probes(blocking=False)
            if not probes:
                raise FlashError("No SWD probe detected. Check USB and probe drivers.")
            if len(probes) > 1:
                raise FlashError(
                    "Multiple SWD probes detected. Select one in the Probe list."
                )
            probe_uid = probes[0].unique_id
        session = ConnectHelper.session_with_chosen_probe(
            unique_id=probe_uid,
            options=options,
        )
        if session is None:
            raise FlashError("No compatible SWD probe was selected or detected.")
        self.log(f"Connecting to {self.target_name} over SWD...")
        with session:
            yield session
        self.log("Probe disconnected.")

    @staticmethod
    def _validate_target_flash(target: object) -> None:
        region = target.memory_map.get_region_for_address(config.FLASH_BASE)
        if (
            region is None
            or not region.is_flash
            or region.start > config.FLASH_BASE
            or region.start + region.length < config.FLASH_BASE + config.FLASH_SIZE
        ):
            raise FlashError(
                "The selected target's flash map does not contain the expected "
                "128 KB PY32F040xB/PY32F071xB address range."
            )

    def erase_all(self) -> None:
        self.log(
            f"ERASE ALL: 0x{config.FLASH_BASE:08X}.."
            f"0x{config.FLASH_BASE + config.FLASH_SIZE - 1:08X}"
        )
        with self._session() as session:
            _, FlashEraser, _ = _pyocd_api()
            target = session.board.target
            self._validate_target_flash(target)
            eraser = FlashEraser(session, FlashEraser.Mode.SECTOR)
            for offset in range(0, config.FLASH_SIZE, config.FLASH_SECTOR_SIZE):
                address = config.FLASH_BASE + offset
                self.log(f"Erasing sector at 0x{address:08X}")
                eraser.erase([self._address_range(address, config.FLASH_SECTOR_SIZE)])
            self._verify_erased(target, config.FLASH_BASE, config.FLASH_SIZE)
        self.log("Full 128 KB flash erase completed.")

    def reset_target(self) -> None:
        self.log("Reset target; IAP will run again.")
        with self._session() as session:
            target = session.board.target
            self._validate_target_flash(target)
            target.reset()
        self.log("Target reset requested.")

    def erase_region(self, region_key: str) -> None:
        region = self._get_region(region_key)
        self.log(
            f"Erase {region.name}: 0x{region.start:08X}.."
            f"0x{region.end - 1:08X}"
        )
        with self._session() as session:
            _, FlashEraser, _ = _pyocd_api()
            target = session.board.target
            self._validate_target_flash(target)
            eraser = FlashEraser(session, FlashEraser.Mode.SECTOR)
            for offset in range(0, region.size, config.FLASH_SECTOR_SIZE):
                address = region.start + offset
                self.log(f"Erasing sector at 0x{address:08X}")
                eraser.erase([self._address_range(address, config.FLASH_SECTOR_SIZE)])
            self._verify_erased(target, region.start, region.size)
        self.log(f"{region.name} erase completed.")

    def program(self, region_key: str, image_path: Path) -> None:
        region = self._get_region(region_key)
        image_path = image_path.expanduser().resolve()
        image = self._validate_image(region_key, image_path)
        self._program_image(region, image_path, image, region_key)

    def program_test_audio(self, image_path: Path) -> None:
        if self.target_name != "PY32F071xB":
            raise FlashError(
                "The test audio layout is reserved for the PY32F071xB DAC demo."
            )
        image_path = image_path.expanduser().resolve()
        if not image_path.is_file():
            raise FlashError(f"Audio BIN file not found: {image_path}")
        image = image_path.read_bytes()
        if len(image) < config.TEST_AUDIO_HEADER_SIZE:
            raise FlashError("Audio BIN is missing its 16-byte FQA1 header.")
        if len(image) > config.TEST_AUDIO_REGION.size:
            raise FlashError(
                f"Audio BIN exceeds the {config.TEST_AUDIO_REGION.size}-byte "
                "test audio region."
            )
        magic = image[:4]
        sample_rate = int.from_bytes(image[4:8], "little")
        sample_count = int.from_bytes(image[8:12], "little")
        if (
            magic != b"FQA1"
            or sample_rate != config.TEST_AUDIO_SAMPLE_RATE
            or sample_count != len(image) - config.TEST_AUDIO_HEADER_SIZE
        ):
            raise FlashError(
                "Invalid audio BIN. Expected FQA1 header, 16 kHz sample rate, "
                "and a sample count matching the PCM data."
            )
        self._program_image(
            config.TEST_AUDIO_REGION,
            image_path,
            image,
        )

    def export_recording(self, output_path: Path) -> None:
        if self.target_name != "PY32F071xB":
            raise FlashError("Recording export requires the PY32F071xB test layout.")
        with self._session(attach=True) as session:
            target = session.board.target
            self._validate_target_flash(target)
            was_running = target.is_running()
            target.halt()
            try:
                header = bytes(target.read_memory_block8(
                    config.TEST_AUDIO_BASE, config.TEST_AUDIO_HEADER_SIZE
                ))
                if len(header) != config.TEST_AUDIO_HEADER_SIZE:
                    raise FlashError("Incomplete audio header read.")
                magic, rate, count, encoding = struct.unpack("<4I", header)
                if (
                    magic != 0x31415146
                    or rate != config.TEST_AUDIO_SAMPLE_RATE
                    or encoding != 1
                    or not 0 < count <= config.TEST_AUDIO_SIZE - config.TEST_AUDIO_HEADER_SIZE
                ):
                    raise FlashError("No valid completed PCM recording in the audio region.")
                pcm = bytearray()
                for offset in range(0, count, 4096):
                    size = min(4096, count - offset)
                    block = bytes(target.read_memory_block8(
                        config.TEST_AUDIO_BASE + config.TEST_AUDIO_HEADER_SIZE + offset,
                        size,
                    ))
                    if len(block) != size:
                        raise FlashError("Incomplete audio data read.")
                    pcm.extend(block)
            finally:
                if was_running:
                    target.resume()
        output_path = output_path.expanduser().resolve()
        with wave.open(str(output_path), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(1)
            audio.setframerate(rate)
            audio.writeframes(pcm)
        self.log(f"Exported {count} samples ({count / rate:.3f} s): {output_path}")

    def _program_image(
        self,
        region: config.FlashRegion,
        image_path: Path,
        image: bytes,
        region_key: str | None = None,
    ) -> None:
        expected = image + b"\xFF" * (region.size - len(image))
        self.log(
            f"Program {region.name}: {image_path} ({len(image)} bytes), "
            f"address 0x{region.start:08X}"
        )

        with tempfile.TemporaryDirectory(prefix="faqiuji-pyocd-") as directory:
            padded_path = Path(directory) / f"{region.name}.bin"
            padded_path.write_bytes(expected)
            with self._session() as session:
                _, FlashEraser, FileProgrammer = _pyocd_api()
                target = session.board.target
                self._validate_target_flash(target)
                target.halt()
                if region_key is not None:
                    self._check_direct_b_metadata(target, region_key)
                if region == config.TEST_AUDIO_REGION:
                    self.log("Resetting and halting MCU before audio programming...")
                    target.reset_and_halt()
                    self.log("Erasing and checking the test audio region before programming...")
                    eraser = FlashEraser(session, FlashEraser.Mode.SECTOR)
                    for address in range(region.start, region.end, config.FLASH_SECTOR_SIZE):
                        eraser.erase([self._address_range(address, config.FLASH_SECTOR_SIZE)])
                    self._verify_erased(target, region.start, region.size)
                    self._program_audio_pages(target, expected)
                else:
                    FileProgrammer(
                        session,
                        chip_erase="sector",
                        no_reset=True,
                    ).program(
                        str(padded_path),
                        file_format="bin",
                        base_address=region.start,
                    )
                target.halt()
                self.log(f"Verifying {region.name} ({region.size} bytes)...")
                actual = bytes(target.read_memory_block8(region.start, region.size))
                if len(actual) != len(expected):
                    raise FlashError(f"Incomplete read-back: {len(actual)}/{len(expected)} bytes.")
                if actual != expected:
                    mismatch = next(
                        index
                        for index, (actual_byte, expected_byte) in enumerate(
                            zip(actual, expected)
                        )
                        if actual_byte != expected_byte
                    )
                    raise FlashError(
                        f"Verify failed at address 0x{region.start + mismatch:08X}: "
                        f"expected 0x{expected[mismatch]:02X}, read 0x{actual[mismatch]:02X}."
                    )
                self.log(f"{region.name} programming and read-back verification passed.")
                if region_key == "app_b":
                    current_a = bytes(
                        target.read_memory_block8(config.APP_A_BASE, config.APP_A_SIZE)
                    )
                    if current_a == expected:
                        self.log(
                            "Warning: APP_A already matches APP_B. IAP will not "
                            "install an identical image."
                        )

    def _program_audio_pages(self, target: object, expected: bytes) -> None:
        region = target.memory_map.get_region_for_address(config.TEST_AUDIO_BASE)
        flash = region.flash
        if flash is None:
            raise FlashError("The selected pack has no audio-region flash algorithm.")
        page = flash.get_page_info(config.TEST_AUDIO_BASE)
        if page is None or page.size != 256:
            raise FlashError("Expected the PY32F071 256-byte flash programming page.")
        page_size = page.size
        self.log("Programming audio in serial 256-byte pages with immediate read-back...")
        try:
            flash.init(flash.Operation.PROGRAM)
            for offset in range(0, len(expected), page_size):
                data = expected[offset:offset + page_size]
                if data == b"\xFF" * page_size:
                    continue
                address = config.TEST_AUDIO_BASE + offset
                # Synchronous programming avoids concurrent SWD writes to a second RAM buffer.
                flash.program_page(address, list(data))
                actual = bytes(target.read_memory_block8(address, len(data)))
                if len(actual) != len(data):
                    raise FlashError(
                        f"Incomplete audio page read at 0x{address:08X}: "
                        f"{len(actual)}/{len(data)} bytes."
                    )
                if actual != data:
                    mismatch = next(i for i, (a, b) in enumerate(zip(actual, data)) if a != b)
                    begin = max(0, mismatch - 8)
                    end = min(len(data), mismatch + 9)
                    raise FlashError(
                        f"Audio page verify failed at 0x{address + mismatch:08X}: "
                        f"expected 0x{data[mismatch]:02X}, read 0x{actual[mismatch]:02X}. "
                        f"Context at 0x{address + begin:08X}: "
                        f"expected [{data[begin:end].hex(' ')}], "
                        f"read [{actual[begin:end].hex(' ')}]."
                    )
                if offset % config.FLASH_SECTOR_SIZE == 0:
                    self.log(f"Audio page verified at 0x{address:08X}")
        finally:
            flash.cleanup()

    def factory_setup(self, iap_path: Path, app_a_path: Path) -> None:
        iap = self._validate_image("iap", iap_path)
        app_a = self._validate_image("app_a", app_a_path)
        self.log(
            "FACTORY SETUP: erase all 128 KB, program APP_A, then program IAP."
        )
        self.log(
            "This destroys all existing firmware, OTA metadata, and configuration."
        )
        with self._session() as session:
            _, FlashEraser, FileProgrammer = _pyocd_api()
            target = session.board.target
            self._validate_target_flash(target)
            eraser = FlashEraser(session, FlashEraser.Mode.SECTOR)
            for offset in range(0, config.FLASH_SIZE, config.FLASH_SECTOR_SIZE):
                address = config.FLASH_BASE + offset
                self.log(f"Erasing sector at 0x{address:08X}")
                eraser.erase([self._address_range(address, config.FLASH_SECTOR_SIZE)])
            self._verify_erased(target, config.FLASH_BASE, config.FLASH_SIZE)

            # Keep IAP last so it cannot start before APP_A has been installed.
            self._program_in_session(
                session, target, FileProgrammer, "app_a", app_a
            )
            self._program_in_session(session, target, FileProgrammer, "iap", iap)
        self.log("Factory initialization completed.")

    def _program_in_session(
        self,
        session: object,
        target: object,
        programmer_type: type,
        region_key: str,
        image: bytes,
    ) -> None:
        region = self._get_region(region_key)
        expected = image + b"\xFF" * (region.size - len(image))
        self.log(
            f"Programming {region.name} at 0x{region.start:08X} "
            f"({len(image)} bytes)..."
        )
        with tempfile.TemporaryDirectory(prefix="faqiuji-pyocd-") as directory:
            padded_path = Path(directory) / f"{region.name}.bin"
            padded_path.write_bytes(expected)
            programmer_type(
                session,
                chip_erase="sector",
                no_reset=True,
            ).program(
                str(padded_path),
                file_format="bin",
                base_address=region.start,
            )
        target.halt()
        actual = bytes(target.read_memory_block8(region.start, region.size))
        if actual != expected:
            mismatch = next(
                index
                for index, (actual_byte, expected_byte) in enumerate(zip(actual, expected))
                if actual_byte != expected_byte
            )
            raise FlashError(
                f"{region.name} verify failed at 0x{region.start + mismatch:08X}."
            )
        self.log(f"{region.name} verified.")

    def _check_direct_b_metadata(self, target: object, region_key: str) -> None:
        if region_key != "app_b":
            return
        metadata = bytes(
            target.read_memory_block8(
                config.META_B_BASE,
                config.META_B_DIRECT_CHECK_SIZE,
            )
        )
        if metadata != b"\xFF" * config.META_B_DIRECT_CHECK_SIZE:
            raise FlashError(
                "APP_B OTA marker/record pages are not erased. IAP will reject a "
                "direct BIN image in this state. The metadata shares an erase sector "
                "with APP_A metadata, so it was not modified."
            )

    @staticmethod
    def _verify_erased(target: object, start: int, size: int) -> None:
        chunk_size = 0x400
        for offset in range(0, size, chunk_size):
            length = min(chunk_size, size - offset)
            data = target.read_memory_block8(start + offset, length)
            if any(value != 0xFF for value in data):
                mismatch = next(
                    index for index, value in enumerate(data) if value != 0xFF
                )
                raise FlashError(
                    f"Erase verification failed at 0x{start + offset + mismatch:08X}."
                )

    @staticmethod
    def _get_region(region_key: str) -> config.FlashRegion:
        try:
            return config.REGIONS[region_key]
        except KeyError as exc:
            raise FlashError(f"Unknown flash region: {region_key}") from exc

    @staticmethod
    def _address_range(address: int, size: int) -> str:
        return f"0x{address:08X}+0x{size:X}"

    @staticmethod
    def _validate_image(region_key: str, image_path: Path) -> bytes:
        if not image_path.is_file():
            raise FlashError(f"BIN file not found: {image_path}")
        image = image_path.read_bytes()
        region = config.REGIONS[region_key]
        if not image or len(image) > region.size:
            raise FlashError(
                f"{region.name} BIN size must be between 1 and {region.size} bytes; "
                f"got {len(image)}."
            )

        if region_key == "iap":
            stack_pointer = int.from_bytes(image[0:4], "little")
            reset_handler = int.from_bytes(image[4:8], "little")
            reset_address = reset_handler & ~1
            if (
                len(image) < 8
                or not config.APP_VECTOR_RAM_BASE
                <= stack_pointer
                <= config.APP_VECTOR_RAM_END
                or stack_pointer & 7
                or not reset_handler & 1
                or not region.start <= reset_address < region.end
            ):
                raise FlashError("IAP BIN vector table is invalid for the IAP region.")

        if region_key in ("app_a", "app_b"):
            if len(image) < 8:
                raise FlashError("APP BIN is too small to contain its vector table.")
            stack_pointer = int.from_bytes(image[0:4], "little")
            reset_handler = int.from_bytes(image[4:8], "little")
            reset_address = reset_handler & ~1
            if (
                not config.APP_VECTOR_RAM_BASE
                <= stack_pointer
                <= config.APP_VECTOR_RAM_END
                or stack_pointer & 7
                or not reset_handler & 1
                or not config.APP_A_BASE
                <= reset_address
                < config.APP_A_BASE + config.APP_A_SIZE
            ):
                raise FlashError(
                    "APP BIN must contain a valid vector table linked to APP_A "
                    "at 0x08004000, including when programming APP_B."
                )
        return image
