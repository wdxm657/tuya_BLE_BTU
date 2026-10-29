from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from apps.faqiuji_mcu_puy.script import config, pyocd_backend
from apps.faqiuji_mcu_puy.script.pyocd_backend import FlashError, PyOcdBackend


class FakeMemoryRegion:
    start = config.FLASH_BASE
    length = config.FLASH_SIZE
    is_flash = True


class FakeMemoryMap:
    def get_region_for_address(self, _address: int) -> FakeMemoryRegion:
        return FakeMemoryRegion()


class FakeTarget:
    def __init__(self) -> None:
        self.flash = bytearray(b"\xFF" * config.FLASH_SIZE)
        self.memory_map = FakeMemoryMap()
        self.reset_count = 0

    def halt(self) -> None:
        pass

    def reset(self) -> None:
        self.reset_count += 1

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
