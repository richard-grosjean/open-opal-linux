import json
import os
from dataclasses import asdict, dataclass, fields
from pathlib import Path

CONFIG_PATH = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "open-opal-linux" / "settings.json"

# White balance modes
WB_AUTO = "auto"  # the chip's own auto white balance
WB_AUTO_BIASED = "auto_biased"  # host-side feedback loop with a warmth bias
WB_MANUAL = "manual"


@dataclass
class Settings:
    # focus
    auto_focus: bool = False
    lens_position: int = 120  # 0..255

    # white balance
    wb_mode: str = WB_AUTO
    wb_warmth: int = 4  # -10 (cooler) .. +10 (warmer), used by WB_AUTO_BIASED; 0 = grey-world
    wb_kelvin: int = 4000  # 1000..12000, used by WB_MANUAL

    # exposure
    auto_exposure: bool = True
    exposure_compensation: int = 0  # -9..9
    exposure_us: int = 20000  # 1..33000 keeps 30 fps
    iso: int = 800  # 100..1600
    anti_banding: str = "MAINS_50_HZ"  # OFF, AUTO, MAINS_50_HZ, MAINS_60_HZ

    # image processing on the chip
    tuning: str = "low_light"  # "low_light" (Luxonis low-light blob) or "default"; applies on connect
    brightness: int = 0  # -10..10
    contrast: int = 0  # -10..10
    saturation: int = 0  # -10..10
    sharpness: int = 1  # 0..4
    luma_denoise: int = 2  # 0..4
    chroma_denoise: int = 2  # 0..4

    # output
    output_enabled: bool = True

    def save(self) -> None:
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_PATH.write_text(json.dumps(asdict(self), indent=2))

    @classmethod
    def load(cls) -> "Settings":
        try:
            data = json.loads(CONFIG_PATH.read_text())
        except (OSError, ValueError):
            return cls()
        known = {f.name for f in fields(cls)}
        return cls(**{k: v for k, v in data.items() if k in known})
