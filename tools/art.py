"""The app's HOME Menu art, in the style of its own screens (app/source/ui.h), after the
3DS UA installer's tools/installer_art.py.

    icon.png    48x48: the blue badge with a "user +" glyph, as on the main screen
    banner.png  256x128: the top screen while the app is selected on the HOME Menu
    banner.wav  the banner's sound: a short soft two-note chime

Text is Nunito 800 (OFL, app/assets/fonts), instanced by `make fonts`.

Usage (`make art`):
    python3 tools/art.py build/fonts/Nunito-800.ttf app/art
"""

from __future__ import annotations

import math
import struct
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BLUE = (0x14, 0x58, 0xC7)
INK = (0x1B, 0x24, 0x33)
BODY = (0x3A, 0x45, 0x56)
GROUND = (0xE4, 0xE8, 0xEF)
WHITE = (0xFF, 0xFF, 0xFF)
SCALE = 8  # drawn large, scaled down: smooth edges at 48 px


def badge(size: int) -> Image.Image:
    """The blue rounded square with the "user +" icon of the main screen, `size` px."""
    s = size * SCALE
    im = Image.new("RGB", (s, s), GROUND)
    d = ImageDraw.Draw(im)
    pad = s * 0.04
    d.rounded_rectangle([pad, pad, s - pad, s - pad], s * 0.24, fill=BLUE)

    # the icon on a 24-unit grid, centred, 60% of the badge
    k = s * 0.6 / 24
    ox = oy = s * 0.2
    w = max(2, round(2.3 * k))

    def p(x: float, y: float) -> tuple[float, float]:
        return ox + x * k, oy + y * k

    cx, cy, r = p(8.5, 7) + (3.5 * k,)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=WHITE, width=w)
    bx, by = p(8.5, 18)
    br = 6.5 * k
    d.arc([bx - br, by - br, bx + br, by + br], 180, 360, fill=WHITE, width=w)
    d.line([p(2, 18), p(2, 19)], fill=WHITE, width=w)
    d.line([p(15, 18), p(15, 19)], fill=WHITE, width=w)
    d.line([p(19, 8), p(19, 14)], fill=WHITE, width=w)
    d.line([p(16, 11), p(22, 11)], fill=WHITE, width=w)
    return im.resize((size, size), Image.LANCZOS)


def banner(font: Path) -> Image.Image:
    w, h = 256, 128
    im = Image.new("RGB", (w * SCALE, h * SCALE), GROUND)
    draw = ImageDraw.Draw(im)
    title = ImageFont.truetype(str(font), 21 * SCALE)
    sub = ImageFont.truetype(str(font), 11 * SCALE)
    draw.text((92 * SCALE, 34 * SCALE), "Friend Auto", font=title, fill=INK)
    draw.text((92 * SCALE, 58 * SCALE), "Add Tool", font=title, fill=INK)
    draw.text((92 * SCALE, 88 * SCALE), "Друзі з бази n3ds_ua", font=sub, fill=BODY)
    im = im.resize((w, h), Image.LANCZOS)
    im.paste(badge(68), (16, 30))
    return im


def chime(path: Path) -> None:
    """Two soft notes (A5, E5 - the installer's chime turned round), 0.6 s, 16-bit mono."""
    rate, length = 32000, 0.6
    frames = bytearray()
    for i in range(int(rate * length)):
        t = i / rate
        v = 0.0
        for start, freq in ((0.0, 880.0), (0.12, 659.25)):
            if t >= start:
                v += math.sin(2 * math.pi * freq * (t - start)) * math.exp(-(t - start) * 7)
        frames += struct.pack("<h", int(v * 0.25 * 32767))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def main() -> None:
    font, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    badge(48).save(out / "icon.png")
    banner(font).save(out / "banner.png")
    chime(out / "banner.wav")
    print(f"{out}: icon.png, banner.png, banner.wav")


if __name__ == "__main__":
    main()
