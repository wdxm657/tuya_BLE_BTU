from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path


TARGET_NAME = "PY32F040xB"
SWD_FREQUENCY_HZ = 1_000_000

FLASH_BASE = 0x08000000
FLASH_SIZE = 0x20000
FLASH_SECTOR_SIZE = 0x2000
APP_SLOT_SIZE = 0xC000

IAP_BASE = 0x08000000
IAP_SIZE = 0x4000
APP_A_BASE = 0x08004000
APP_A_SIZE = APP_SLOT_SIZE
APP_B_BASE = 0x08010000
APP_B_SIZE = APP_SLOT_SIZE
OTA_META_BASE = 0x0801C000
OTA_META_SIZE = 0x2000
CONFIG_BASE = 0x0801E000
CONFIG_SIZE = 0x2000

META_B_BASE = 0x0801D000
META_B_DIRECT_CHECK_SIZE = 0x200

APP_VECTOR_RAM_BASE = 0x20000000
APP_VECTOR_RAM_END = 0x20004000

SCRIPT_DIR = Path(__file__).resolve().parent
MCU_ROOT = SCRIPT_DIR.parent
IAP_BIN_DEFAULT = MCU_ROOT / "faqiuji_iap" / "MDK-ARM" / "Project.bin"
APP_A_BIN_DEFAULT = MCU_ROOT / "faqiuji_mcu" / "MDK-ARM" / "FAQIUJI_APP_A.bin"
APP_B_BIN_DEFAULT = MCU_ROOT / "faqiuji_mcu" / "MDK-ARM" / "FAQIUJI_APP_B.bin"


@dataclass(frozen=True)
class FlashRegion:
    name: str
    start: int
    size: int

    @property
    def end(self) -> int:
        return self.start + self.size

    @property
    def sector_count(self) -> int:
        return self.size // FLASH_SECTOR_SIZE


REGIONS = {
    "iap": FlashRegion("IAP", IAP_BASE, IAP_SIZE),
    "app_a": FlashRegion("APP_A", APP_A_BASE, APP_A_SIZE),
    "app_b": FlashRegion("APP_B", APP_B_BASE, APP_B_SIZE),
    "metadata": FlashRegion("OTA metadata", OTA_META_BASE, OTA_META_SIZE),
    "config": FlashRegion("configuration", CONFIG_BASE, CONFIG_SIZE),
}


def default_pack_path() -> Path | None:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        return None
    path = (
        Path(local_app_data)
        / "Arm"
        / "Packs"
        / "Puya"
        / "PY32F0xx_DFP"
        / "1.2.20"
    )
    return path if (path / "Puya.PY32F0xx_DFP.pdsc").is_file() else None
