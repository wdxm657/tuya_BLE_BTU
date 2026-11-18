from __future__ import annotations

from pathlib import Path
import tempfile
import struct
import wave
import unittest
from unittest.mock import patch

from apps.faqiuji_mcu_puy.script import config, pyocd_backend
from apps.faqiuji_mcu_puy.script.pyocd_backend import FlashError, PyOcdBackend


class FakeMemoryRegion:
    start = config.FLASH_BASE
    length = config.FLASH_SIZE
    is_flash = True

    def __init__(self, flash: object) -> None:
        self.flash = flash


class FakePageFlash:
    class Operation:
        PROGRAM = "program"

    def __init__(self, target: object) -> None:
        self.target = target
        self.pages: list[int] = []
        self.cleaned = False
        self.corrupt = False

    def get_page_info(self, _address: int) -> object:
        return type("PageInfo", (), {"size": 256})()

    def init(self, operation: str) -> None:
        assert operation == self.Operation.PROGRAM
        assert not self.target.running
        assert self.target.reset_count > 0

    def program_page(self, address: int, data: list[int]) -> None:
        assert len(data) == 256
        assert address % 256 == 0
        self.pages.append(address)
        start = address - config.FLASH_BASE
        assert self.target.flash[start:start + 256] == b"\xFF" * 256
        self.target.flash[start:start + 256] = bytes(data)
        if self.corrupt:
            self.target.flash[start + 128] ^= 1

    def cleanup(self) -> None:
        self.cleaned = True


class FakeMemoryMap:
    def __init__(self, flash: FakePageFlash) -> None:
        self.region = FakeMemoryRegion(flash)

    def get_region_for_address(self, _address: int) -> FakeMemoryRegion:
        return self.region


class FakeTarget:
    def __init__(self) -> None:
        self.flash = bytearray(b"\xFF" * config.FLASH_SIZE)
        self.page_flash = FakePageFlash(self)
        self.memory_map = FakeMemoryMap(self.page_flash)
        self.reset_count = 0
        self.running = True

    def halt(self) -> None:
        self.running = False

    def is_running(self) -> bool:
        return self.running

    def resume(self) -> None:
        self.running = True

    def reset(self) -> None:
        self.reset_count += 1

    def reset_and_halt(self) -> None:
        self.reset()
        self.halt()

    def read_memory_block8(self, address: int, length: int) -> list[int]:
        start = address - config.FLASH_BASE
        return list(self.flash[start : start + length])


class FakeSession:
    def __init__(self, target: FakeTarget) -> None:
        self.board = type("FakeBoard", (), {"target": target})()

    def __enter__(self) -> FakeSession:
        return self

    def __exit__(self, *_args: object) -> None:
        pass


class FakeConnectHelper:
    target = FakeTarget()
    last_options: dict[str, object] = {}

    @classmethod
    def session_with_chosen_probe(cls, **_kwargs: object) -> FakeSession:
        cls.last_options = _kwargs.get("options", {})
        return FakeSession(cls.target)


class FakeFlashEraser:
    class Mode:
        SECTOR = "sector"

    erased_addresses: list[int] = []

    def __init__(self, session: FakeSession, _mode: str) -> None:
        self.target = session.board.target

    def erase(self, address_ranges: list[str]) -> None:
        for address_range in address_ranges:
            address_text, size_text = address_range.split("+", maxsplit=1)
            address = int(address_text, 16)
            size = int(size_text, 16)
            if size != config.FLASH_SECTOR_SIZE:
                raise AssertionError(f"Unexpected erase size: {size:#x}")
            self.erased_addresses.append(address)
            start = address - config.FLASH_BASE
            end = start + size
            self.target.flash[start:end] = b"\xFF" * size


class FakeFileProgrammer:
    def __init__(self, session: FakeSession, **_kwargs: object) -> None:
        self.target = session.board.target

    def program(
        self,
        image_path: str,
        *,
        file_format: str,
        base_address: int,
    ) -> None:
        if file_format != "bin":
            raise AssertionError("Unexpected file format")
        image = Path(image_path).read_bytes()
        start = base_address - config.FLASH_BASE
        self.target.flash[start : start + len(image)] = image


class BackendTests(unittest.TestCase):
    def test_export_recording_wav(self) -> None:
        target = FakeConnectHelper.target
        pcm = bytes(range(256)) * 20
        image = struct.pack("<4I", 0x31415146, 16000, len(pcm), 1) + pcm
        start = config.TEST_AUDIO_BASE - config.FLASH_BASE
        target.flash[start:start + len(image)] = image
        original = bytes(target.flash)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "record.wav"
            self.backend.export_recording(output)
            with wave.open(str(output), "rb") as audio:
                self.assertEqual(audio.getnchannels(), 1)
                self.assertEqual(audio.getsampwidth(), 1)
                self.assertEqual(audio.getframerate(), 16000)
                self.assertEqual(audio.readframes(len(pcm)), pcm)
        self.assertTrue(target.running)
        self.assertEqual(bytes(target.flash), original)
        self.assertEqual(FakeConnectHelper.last_options["connect_mode"], "attach")

    def test_export_invalid_recording_restores_running(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "record.wav"
            with self.assertRaises(FlashError):
                self.backend.export_recording(output)
            self.assertFalse(output.exists())
        self.assertTrue(FakeConnectHelper.target.running)

    def test_export_preserves_halted_state(self) -> None:
        target = FakeConnectHelper.target
        target.running = False
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FlashError):
                self.backend.export_recording(Path(directory) / "record.wav")
        self.assertFalse(target.running)

    def setUp(self) -> None:
        FakeConnectHelper.target = FakeTarget()
        FakeFlashEraser.erased_addresses = []
        self.api_patch = patch.object(
            pyocd_backend,
            "_pyocd_api",
            return_value=(FakeConnectHelper, FakeFlashEraser, FakeFileProgrammer),
        )
        self.api_patch.start()
        self.addCleanup(self.api_patch.stop)
        self.backend = PyOcdBackend(
            Path("."),
            probe_uid="fake-probe",
            target_name=config.DEFAULT_TARGET_NAME,
            log=lambda _message: None,
        )

    def test_partition_layout_is_contiguous_and_sector_aligned(self) -> None:
        regions = list(config.REGIONS.values())
        self.assertEqual(regions[0].start, config.FLASH_BASE)
        for left, right in zip(regions, regions[1:]):
            self.assertEqual(left.end, right.start)
        self.assertEqual(regions[-1].end, config.FLASH_BASE + config.FLASH_SIZE)
        for region in regions:
            self.assertEqual(region.start % config.FLASH_SECTOR_SIZE, 0)
            self.assertEqual(region.size % config.FLASH_SECTOR_SIZE, 0)

    def test_erase_app_b_does_not_touch_other_partitions(self) -> None:
        target = FakeConnectHelper.target
        target.flash[:] = b"\x00" * config.FLASH_SIZE

        self.backend.erase_region("app_b")

        expected_addresses = list(
            range(
                config.APP_B_BASE,
                config.APP_B_BASE + config.APP_B_SIZE,
                config.FLASH_SECTOR_SIZE,
            )
        )
        self.assertEqual(FakeFlashEraser.erased_addresses, expected_addresses)
        self.assertEqual(
            target.read_memory_block8(config.APP_A_BASE, config.APP_A_SIZE),
            [0x00] * config.APP_A_SIZE,
        )
        self.assertEqual(
            target.read_memory_block8(config.OTA_META_BASE, config.OTA_META_SIZE),
            [0x00] * config.OTA_META_SIZE,
        )

    def test_erase_all_covers_flash_exactly(self) -> None:
        self.backend.erase_all()
        self.assertEqual(
            FakeFlashEraser.erased_addresses,
            list(
                range(
                    config.FLASH_BASE,
                    config.FLASH_BASE + config.FLASH_SIZE,
                    config.FLASH_SECTOR_SIZE,
                )
            ),
        )
        self.assertEqual(
            FakeConnectHelper.target.flash,
            bytearray(b"\xFF" * config.FLASH_SIZE),
        )

    def test_reset_requests_target_reset_without_erasing_flash(self) -> None:
        target = FakeConnectHelper.target
        target.flash[:] = b"\xA5" * config.FLASH_SIZE

        self.backend.reset_target()

        self.assertEqual(target.reset_count, 1)
        self.assertEqual(target.flash, bytearray(b"\xA5" * config.FLASH_SIZE))
        self.assertEqual(FakeFlashEraser.erased_addresses, [])
        self.assertEqual(
            FakeConnectHelper.last_options["target_override"],
            config.DEFAULT_TARGET_NAME,
        )

    def test_selected_target_is_passed_to_pyocd(self) -> None:
        target_name = "PY32F071xB"
        backend = PyOcdBackend(
            Path("."),
            probe_uid="fake-probe",
            target_name=target_name,
            log=lambda _message: None,
        )

        backend.reset_target()

        self.assertEqual(
            FakeConnectHelper.last_options["target_override"],
            target_name,
        )

    def test_unknown_target_is_rejected(self) -> None:
        with self.assertRaisesRegex(FlashError, "Unsupported target"):
            PyOcdBackend(
                Path("."),
                target_name="PY32F099x9",
            )

    def test_program_app_b_writes_full_slot_and_verifies_it(self) -> None:
        target = FakeConnectHelper.target
        app_image = config.APP_A_BIN_DEFAULT.read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "app_b.bin"
            image_path.write_bytes(app_image)
            self.backend.program("app_b", image_path)

        actual = bytes(target.read_memory_block8(config.APP_B_BASE, config.APP_B_SIZE))
        self.assertEqual(
            actual,
            app_image + b"\xFF" * (config.APP_B_SIZE - len(app_image)),
        )

    def test_program_test_audio_writes_only_audio_flash_area(self) -> None:
        target = FakeConnectHelper.target
        target.flash[:] = b"\x00" * config.FLASH_SIZE
        pcm = bytes(range(32))
        image = (
            b"FQA1"
            + config.TEST_AUDIO_SAMPLE_RATE.to_bytes(4, "little")
            + len(pcm).to_bytes(4, "little")
            + (1).to_bytes(4, "little")
            + pcm
        )
        target.flash[: config.TEST_AUDIO_BASE - config.FLASH_BASE] = b"\xA5" * (
            config.TEST_AUDIO_BASE - config.FLASH_BASE
        )
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "test_audio.bin"
            image_path.write_bytes(image)
            self.backend.program_test_audio(image_path)

        prefix = target.read_memory_block8(
            config.FLASH_BASE,
            config.TEST_AUDIO_BASE - config.FLASH_BASE,
        )
        self.assertEqual(prefix, [0xA5] * len(prefix))
        actual = bytes(
            target.read_memory_block8(
                config.TEST_AUDIO_BASE,
                config.TEST_AUDIO_REGION.size,
            )
        )
        self.assertEqual(
            actual,
            image + b"\xFF" * (config.TEST_AUDIO_REGION.size - len(image)),
        )
        self.assertEqual(
            FakeFlashEraser.erased_addresses,
            list(range(config.TEST_AUDIO_REGION.start,
                       config.TEST_AUDIO_REGION.end, config.FLASH_SECTOR_SIZE)),
        )
        self.assertFalse(FakeConnectHelper.last_options["cache.enable_memory"])
        self.assertEqual(FakeConnectHelper.last_options["reset_type"], "sysresetreq")
        self.assertEqual(target.reset_count, 1)
        self.assertEqual(target.page_flash.pages, [config.TEST_AUDIO_BASE])
        self.assertTrue(target.page_flash.cleaned)
        self.assertFalse(target.running)

    def test_audio_page_mismatch_stops_programming_and_cleans_up(self) -> None:
        target = FakeConnectHelper.target
        target.page_flash.corrupt = True
        pcm = bytes(range(256)) * 3
        image = struct.pack("<4I", 0x31415146, 16000, len(pcm), 1) + pcm
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "audio.bin"
            image_path.write_bytes(image)
            with self.assertRaisesRegex(FlashError, "Audio page verify failed"):
                self.backend.program_test_audio(image_path)
        self.assertEqual(target.page_flash.pages, [config.TEST_AUDIO_BASE])
        self.assertTrue(target.page_flash.cleaned)
        self.assertFalse(target.running)

    def test_audio_serial_programming_covers_multiple_pages(self) -> None:
        target = FakeConnectHelper.target
        pcm = bytes(range(256)) * 2
        image = struct.pack("<4I", 0x31415146, 16000, len(pcm), 1) + pcm
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "audio.bin"
            image_path.write_bytes(image)
            self.backend.program_test_audio(image_path)
        self.assertEqual(
            target.page_flash.pages,
            [config.TEST_AUDIO_BASE + offset for offset in (0, 256, 512)],
        )
        self.assertEqual(
            bytes(target.read_memory_block8(config.TEST_AUDIO_BASE, len(image))),
            image,
        )

    def test_program_test_audio_rejects_invalid_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "bad_audio.bin"
            image_path.write_bytes(b"not-an-audio-image")
            with self.assertRaisesRegex(FlashError, "Invalid audio BIN"):
                self.backend.program_test_audio(image_path)

    def test_program_test_audio_is_restricted_to_f071(self) -> None:
        backend = PyOcdBackend(
            Path("."),
            probe_uid="fake-probe",
            target_name="PY32F040xB",
            log=lambda _message: None,
        )
        with self.assertRaisesRegex(FlashError, "reserved for the PY32F071xB"):
            backend.program_test_audio(Path("unused.bin"))

    def test_default_images_have_valid_vectors_for_their_regions(self) -> None:
        PyOcdBackend._validate_image("iap", config.IAP_BIN_DEFAULT)
        PyOcdBackend._validate_image("app_a", config.APP_A_BIN_DEFAULT)
        PyOcdBackend._validate_image("app_b", config.APP_B_BIN_DEFAULT)

    def test_factory_setup_erases_all_then_programs_app_a_and_iap(self) -> None:
        self.backend.factory_setup(config.IAP_BIN_DEFAULT, config.APP_A_BIN_DEFAULT)

        self.assertEqual(
            FakeFlashEraser.erased_addresses,
            list(
                range(
                    config.FLASH_BASE,
                    config.FLASH_BASE + config.FLASH_SIZE,
                    config.FLASH_SECTOR_SIZE,
                )
            ),
        )
        target = FakeConnectHelper.target
        iap = config.IAP_BIN_DEFAULT.read_bytes()
        app_a = config.APP_A_BIN_DEFAULT.read_bytes()
        self.assertEqual(
            bytes(target.read_memory_block8(config.IAP_BASE, config.IAP_SIZE)),
            iap + b"\xFF" * (config.IAP_SIZE - len(iap)),
        )
        self.assertEqual(
            bytes(target.read_memory_block8(config.APP_A_BASE, config.APP_A_SIZE)),
            app_a + b"\xFF" * (config.APP_A_SIZE - len(app_a)),
        )
        self.assertEqual(
            bytes(target.read_memory_block8(config.APP_B_BASE, config.APP_B_SIZE)),
            b"\xFF" * config.APP_B_SIZE,
        )

    def test_app_image_must_be_linked_for_a(self) -> None:
        image = bytearray(config.APP_A_BIN_DEFAULT.read_bytes())
        image[4:8] = (config.APP_B_BASE | 1).to_bytes(4, "little")
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "wrong_base.bin"
            image_path.write_bytes(image)
            with self.assertRaises(FlashError):
                PyOcdBackend._validate_image("app_b", image_path)

    def test_app_b_programming_rejects_existing_ota_metadata(self) -> None:
        target = FakeConnectHelper.target
        target.flash[
            config.META_B_BASE - config.FLASH_BASE :
            config.META_B_BASE - config.FLASH_BASE + config.META_B_DIRECT_CHECK_SIZE
        ] = b"\x00" * config.META_B_DIRECT_CHECK_SIZE
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "app_b.bin"
            image_path.write_bytes(config.APP_A_BIN_DEFAULT.read_bytes())
            with self.assertRaisesRegex(FlashError, "OTA marker/record pages"):
                self.backend.program("app_b", image_path)


if __name__ == "__main__":
    unittest.main()
