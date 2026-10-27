#!/usr/bin/env python3
"""Convert images or Image2LCD output to GDEP073E01 4 bpp data.

Target panel: Good Display GDEP073E01, 7.3 inch, 800x480, E Ink Spectra 6.
The generated buffer uses the GxEPD2 7-colour palette indices:

    0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow

Each byte packs two pixels: high nibble = left pixel, low nibble = right pixel.
Full frame size: 800 * 480 / 2 = 192000 bytes.

Examples:
    python3 tools/convert_image.py foto.png -o src/foto.h
    python3 tools/convert_image.py foto.png --format bin --dither
    python3 tools/convert_image.py IMAGE.c --preview preview.png
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from collections import Counter
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: python3 -m pip install Pillow")

WIDTH = 800
HEIGHT = 480
PIXELS = WIDTH * HEIGHT
PACKED_BYTES = PIXELS // 2

# (GxEPD2 index, name, sRGB)
PALETTE_IDEAL = [
    (0, "black", (0, 0, 0)),
    (1, "white", (255, 255, 255)),
    (2, "green", (0, 255, 0)),
    (3, "blue", (0, 0, 255)),
    (4, "red", (255, 0, 0)),
    (5, "yellow", (255, 255, 0)),
]

# Approximate measured Spectra 6 appearance, useful for photo conversion.
PALETTE_PANEL = [
    (0, "black", (20, 20, 20)),
    (1, "white", (235, 235, 230)),
    (2, "green", (40, 110, 60)),
    (3, "blue", (40, 60, 120)),
    (4, "red", (180, 30, 40)),
    (5, "yellow", (230, 200, 40)),
]

# Image2LCD "256 colors" writes RGB332 bytes. These are the six exact values
# produced for the panel's native colours.
IMAGE2LCD_MARKERS = {
    0x00: 0,  # black
    0xFF: 1,  # white
    0x1C: 2,  # green
    0x03: 3,  # blue
    0xE0: 4,  # red
    0xFC: 5,  # yellow
}


def clamp_u8(value: float) -> int:
    if value <= 0:
        return 0
    if value >= 255:
        return 255
    return int(value + 0.5)


def nearest_index(rgb: tuple[int, int, int], palette) -> int:
    r, g, b = rgb
    best_index = 0
    best_distance = None
    for index, _name, (pr, pg, pb) in palette:
        dr = r - pr
        dg = g - pg
        db = b - pb
        distance = dr * dr + dg * dg + db * db
        if best_distance is None or distance < best_distance:
            best_distance = distance
            best_index = index
    return best_index


def build_rgb332_lut(palette) -> bytes:
    table = bytearray(256)
    for value in range(256):
        r = ((value >> 5) & 0x07) * 255 // 7
        g = ((value >> 2) & 0x07) * 255 // 7
        b = (value & 0x03) * 255 // 3
        if value in IMAGE2LCD_MARKERS:
            table[value] = IMAGE2LCD_MARKERS[value]
        else:
            table[value] = nearest_index((r, g, b), palette)
    return bytes(table)


def open_rgb(path: Path) -> Image.Image:
    image = Image.open(path)
    if image.mode in ("RGBA", "LA") or "transparency" in image.info:
        rgba = image.convert("RGBA")
        background = Image.new("RGB", rgba.size, (255, 255, 255))
        background.paste(rgba, mask=rgba.getchannel("A"))
        return background
    return image.convert("RGB")


def fit_image(image: Image.Image, mode: str) -> Image.Image:
    if image.size == (WIDTH, HEIGHT):
        return image

    if mode == "stretch":
        return image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    if mode == "cover":
        scale = max(WIDTH / image.width, HEIGHT / image.height)
        size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
        resized = image.resize(size, Image.Resampling.LANCZOS)
        left = (resized.width - WIDTH) // 2
        top = (resized.height - HEIGHT) // 2
        return resized.crop((left, top, left + WIDTH, top + HEIGHT))

    # contain: preserve aspect ratio, pad with white
    copy = image.copy()
    copy.thumbnail((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), (255, 255, 255))
    canvas.paste(copy, ((WIDTH - copy.width) // 2, (HEIGHT - copy.height) // 2))
    return canvas


def quantize_no_dither(image: Image.Image, palette) -> bytearray:
    pixels = image.load()
    indices = bytearray(PIXELS)
    for y in range(HEIGHT):
        row = y * WIDTH
        for x in range(WIDTH):
            indices[row + x] = nearest_index(pixels[x, y], palette)
    return indices


def quantize_floyd_steinberg(image: Image.Image, palette) -> bytearray:
    pixels = image.load()
    indices = bytearray(PIXELS)
    palette_rgb = {index: rgb for index, _name, rgb in palette}

    current = [[0.0, 0.0, 0.0] for _ in range(WIDTH + 2)]
    following = [[0.0, 0.0, 0.0] for _ in range(WIDTH + 2)]

    for y in range(HEIGHT):
        row = y * WIDTH
        for x in range(WIDTH):
            r, g, b = pixels[x, y]
            er, eg, eb = current[x + 1]
            nr = clamp_u8(r + er)
            ng = clamp_u8(g + eg)
            nb = clamp_u8(b + eb)

            index = nearest_index((nr, ng, nb), palette)
            pr, pg, pb = palette_rgb[index]
            qr = nr - pr
            qg = ng - pg
            qb = nb - pb

            current[x + 2][0] += qr * 7.0 / 16.0
            current[x + 2][1] += qg * 7.0 / 16.0
            current[x + 2][2] += qb * 7.0 / 16.0

            following[x][0] += qr * 3.0 / 16.0
            following[x][1] += qg * 3.0 / 16.0
            following[x][2] += qb * 3.0 / 16.0

            following[x + 1][0] += qr * 5.0 / 16.0
            following[x + 1][1] += qg * 5.0 / 16.0
            following[x + 1][2] += qb * 5.0 / 16.0

            following[x + 2][0] += qr * 1.0 / 16.0
            following[x + 2][1] += qg * 1.0 / 16.0
            following[x + 2][2] += qb * 1.0 / 16.0

            indices[row + x] = index

        current = following
        following = [[0.0, 0.0, 0.0] for _ in range(WIDTH + 2)]

    return indices


def parse_text_bytes(text: str) -> bytes:
    # Strip comments first: Image2LCD headers often carry width/height metadata
    # inside a /* ... */ comment, which must not become image data.
    without_comments = re.sub(r"//.*?$|/\*.*?\*/", "", text, flags=re.S | re.M)

    hex_tokens = re.findall(r"0[xX]([0-9A-Fa-f]{2})(?![0-9A-Fa-f])", without_comments)
    if hex_tokens:
        return bytes(int(token, 16) for token in hex_tokens)

    # Fallback for decimal arrays.
    decimal_tokens = re.findall(r"\b(\d{1,3})\b", without_comments)
    values = [int(token) for token in decimal_tokens if int(token) <= 255]
    return bytes(values)


def unpack4(data: bytes) -> bytearray:
    indices = bytearray(PIXELS)
    out = 0
    for value in data:
        indices[out] = value >> 4
        indices[out + 1] = value & 0x0F
        out += 2
    return indices


def pack4(indices: bytes | bytearray) -> bytes:
    packed = bytearray(PACKED_BYTES)
    out = 0
    for y in range(HEIGHT):
        row = y * WIDTH
        for x in range(0, WIDTH, 2):
            packed[out] = (indices[row + x] << 4) | indices[row + x + 1]
            out += 1
    return bytes(packed)


def sanitize_name(name: str) -> str:
    cleaned = re.sub(r"\W", "_", name)
    if not cleaned or cleaned[0].isdigit():
        cleaned = "image_" + cleaned
    return cleaned


def write_header(path: Path, name: str, data: bytes, source: str) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        f"// Generated by tools/convert_image.py from {source}",
        "// GDEP073E01 800x480, 4 bpp, GxEPD2 palette indices.",
        f"static const uint8_t {name}[] PROGMEM = {{",
    ]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset : offset + 16])
        lines.append(f"  {chunk},")
    lines.extend(
        [
            "};",
            f"static const uint16_t {name}_width = {WIDTH};",
            f"static const uint16_t {name}_height = {HEIGHT};",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def save_preview(path: Path, indices: bytes | bytearray, palette) -> None:
    palette_rgb = {index: rgb for index, _name, rgb in palette}
    image = Image.new("RGB", (WIDTH, HEIGHT))
    image.putdata([palette_rgb.get(int(index), (0, 0, 0)) for index in indices])
    image.save(path)


def load_indices(path: Path, palette, dither: bool, fit: str):
    suffix = path.suffix.lower()

    if suffix in {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff"}:
        image = fit_image(open_rgb(path), fit)
        if dither:
            return quantize_floyd_steinberg(image, palette)
        return quantize_no_dither(image, palette)

    if suffix in {".c", ".h", ".txt"}:
        data = parse_text_bytes(path.read_text(encoding="utf-8", errors="replace"))
    elif suffix == ".bin":
        data = path.read_bytes()
    else:
        raise ValueError(f"unsupported input type: {suffix or '<none>'}")

    if len(data) == PACKED_BYTES:
        return unpack4(data)
    if len(data) == PIXELS:
        return bytearray(data.translate(build_rgb332_lut(palette)))
    raise ValueError(
        f"unexpected data length {len(data)} bytes; expected {PIXELS} (8 bpp) "
        f"or {PACKED_BYTES} (4 bpp)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert an image or Image2LCD C array to GDEP073E01 4 bpp data."
    )
    parser.add_argument("input", type=Path, help="PNG/JPG/BMP, Image2LCD .c/.h or raw .bin")
    parser.add_argument("-o", "--output", type=Path, help="output .h or .bin")
    parser.add_argument("--format", choices=("c", "bin"), help="output format")
    parser.add_argument("--name", help="C array name")
    parser.add_argument("--dither", action="store_true", help="Floyd-Steinberg dithering")
    parser.add_argument(
        "--fit",
        choices=("contain", "cover", "stretch"),
        default="contain",
        help="resize mode for non-800x480 images (default: contain)",
    )
    parser.add_argument(
        "--palette",
        choices=("ideal", "panel"),
        default="panel",
        help="quantization palette (default: panel)",
    )
    parser.add_argument("--preview", type=Path, help="write a quantized PNG preview")
    args = parser.parse_args()

    if not args.input.exists():
        parser.error(f"input not found: {args.input}")

    palette = PALETTE_IDEAL if args.palette == "ideal" else PALETTE_PANEL

    output_format = args.format
    if output_format is None:
        if args.output and args.output.suffix.lower() in {".h", ".c"}:
            output_format = "c"
        elif args.output and args.output.suffix.lower() == ".bin":
            output_format = "bin"
        else:
            output_format = "c"

    output = args.output
    if output is None:
        output = args.input.with_name(
            args.input.stem + ("_4bpp.h" if output_format == "c" else "_4bpp.bin")
        )

    try:
        indices = load_indices(args.input, palette, args.dither, args.fit)
        packed = pack4(indices)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    name = sanitize_name(args.name or (args.input.stem + "_800x480"))

    if output_format == "c":
        write_header(output, name, packed, str(args.input))
    else:
        output.write_bytes(packed)

    if args.preview:
        save_preview(args.preview, indices, palette)

    counts = Counter(indices)
    digest = hashlib.sha256(packed).hexdigest()

    print(f"input   : {args.input}")
    print(f"output  : {output} ({len(packed)} bytes)")
    print(f"palette : {args.palette}  dither={args.dither}  fit={args.fit}")
    print("colors  :")
    for index, label, _rgb in palette:
        print(f"  {index} {label:<6} {counts.get(index, 0):>7} px")
    if any(index > 5 for index in counts):
        print("warning : data contains indices above 5 (orange/white codes)")
    print(f"sha256  : {digest}")
    if output_format == "c":
        print(f"usage   : display.drawNative({name}, nullptr, 0, 0, {WIDTH}, {HEIGHT}, false, false, true);")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
