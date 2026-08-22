from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent / "icons"
NAMES = ("tray-chevron", "tray-device", "tray-network", "tray-volume", "tray-battery", "tray-input")
SIZES = (16, 22, 24, 32, 48, 64, 96, 128)

for size in SIZES:
    src_dir = ROOT / f"{size}x{size}" / "apps"
    for name in NAMES:
        src = src_dir / f"{name}.png"
        if not src.exists():
            continue
        image = Image.open(src).convert("RGBA")
        alpha = image.getchannel("A")
        black = Image.new("RGBA", image.size, (24, 24, 24, 0))
        black.putalpha(alpha)
        black.save(src)
        print(f"blackened {src}")

print("black tray assets ready")
