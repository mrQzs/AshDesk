"""Generate the application icon from the transparent brand mark.

Run from the repository root after replacing the mark (needs Pillow):
  python tools/make_app_icon.py

Writes ui/icons/app/ashdesk.ico (Windows exe/window resource),
ui/icons/app/ashdesk-N.png (Qt window icon, Linux hicolor theme),
ui/icons/app/ashdesk.icns (macOS bundle icon) and ui/icons/app/ashdesk.desktop
(Linux menu entry). Never edit the outputs by hand.
"""
import io
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Logo" / "透明背景的蓝灰色A标志.png"
OUTPUT = ROOT / "ui" / "icons" / "app"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
# PNG-coded .icns entries (macOS 10.7+); ic11..ic14 are the @2x variants of 16..256.
ICNS_TYPES = (("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512),
              ("ic10", 1024), ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512))
DESKTOP_ENTRY = """[Desktop Entry]
Type=Application
Name=AshDesk
Name[zh_CN]=极帧
Exec=ashdesk
Icon=ashdesk
Terminal=false
Categories=Network;RemoteAccess;
"""
MARGIN = 0.03
SOLID = 32


def main():
    mark = Image.open(SOURCE).convert("RGBA")
    mark.putalpha(clean_alpha(mark.getchannel("A")))
    mark = mark.crop(mark.getchannel("A").getbbox())
    square = pad_to_square(mark)
    frames = [square.resize((size, size), Image.Resampling.LANCZOS) for size in SIZES]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for frame in frames:
        frame.save(OUTPUT / f"ashdesk-{frame.width}.png", optimize=True)
    frames[-1].save(OUTPUT / "ashdesk.ico", sizes=[frame.size for frame in frames], append_images=frames[:-1])
    (OUTPUT / "ashdesk.icns").write_bytes(icns(square))
    (OUTPUT / "ashdesk.desktop").write_bytes(DESKTOP_ENTRY.encode("utf-8"))
    print(f"mark {mark.width}x{mark.height} -> {square.width}x{square.height}; "
          f"wrote {len(frames)} PNG + ico + icns + desktop to {OUTPUT}")


def icns(square):
    # Container: b"icns" + big-endian total length, then per entry the OSType and a
    # big-endian length that counts its own 8-byte header, followed by the PNG.
    pngs = {}
    entries = b""
    for ostype, size in ICNS_TYPES:
        if size not in pngs:
            buffer = io.BytesIO()
            square.resize((size, size), Image.Resampling.LANCZOS).save(buffer, "PNG", optimize=True)
            pngs[size] = buffer.getvalue()
        entries += ostype.encode("ascii") + struct.pack(">I", 8 + len(pngs[size])) + pngs[size]
    return b"icns" + struct.pack(">I", 8 + len(entries)) + entries


def clean_alpha(alpha):
    # The matted source keeps alpha <= 3 specks across the whole canvas and an
    # opaque body of 253; drop specks away from the mark and restore full opacity.
    body = body_alpha(alpha)
    near_mark = alpha.point(lambda value: 255 if value >= SOLID else 0).filter(ImageFilter.MaxFilter(7))
    return ImageChops.multiply(alpha.point(lambda value: min(255, round(value * 255 / body))), near_mark)


def body_alpha(alpha):
    histogram = alpha.histogram()
    return max(range(128, 256), key=histogram.__getitem__)


def pad_to_square(image):
    side = round(max(image.size) / (1 - 2 * MARGIN))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(image, ((side - image.width) // 2, (side - image.height) // 2))
    return canvas


if __name__ == "__main__":
    main()
