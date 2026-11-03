#!/usr/bin/env python3
"""Fetch DiceBear avatars and bake them into LVGL RGB565 C arrays.

    python3 scripts/gen_avatars.py

States map to styles on purpose: one friendly character per mode, not five
variations of the same face. Images are composited onto black because the
screen is black; that keeps them 2 bytes/pixel and avoids alpha blending on the
ESP32.
"""

from __future__ import annotations

import pathlib
import urllib.request
from urllib.parse import urlencode

from PIL import Image

SIZE = 180
# One mascot (gaze) across all states: the body colour and eyes carry the mood.
STATES = {
    "connecting": {
        "style": "gaze",
        "seed": "plaud-gaze-connecting",
        "params": {"shapeVariant": "circle", "eyesVariant": "dots", "bodyColor": "9cc8fb"},
    },
    "ready": {
        "style": "gaze",
        "seed": "plaud-gaze-ready",
        "params": {"shapeVariant": "circle", "eyesVariant": "happy", "bodyColor": "9bd78d"},
    },
    "recording": {
        "style": "gaze",
        "seed": "plaud-gaze-recording",
        "params": {"shapeVariant": "circle", "eyesVariant": "wide", "bodyColor": "fbabaf"},
    },
    "saving": {
        "style": "gaze",
        "seed": "plaud-gaze-saving",
        "params": {"shapeVariant": "circle", "eyesVariant": "squint", "bodyColor": "d8b1fb"},
    },
    "error": {
        "style": "gaze",
        "seed": "plaud-gaze-error",
        "params": {"shapeVariant": "circle", "eyesVariant": "bars", "bodyColor": "fbb271"},
    },
}

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "firmware" / "assets" / "avatars"
OUT_C = ROOT / "firmware" / "main" / "avatars.c"
OUT_H = ROOT / "firmware" / "main" / "avatars.h"


def fetch(style: str, seed: str, params: dict[str, str]) -> Image.Image:
    ASSETS.mkdir(parents=True, exist_ok=True)
    path = ASSETS / f"{style}_{seed}.png"
    if not path.exists():
        query = {"seed": seed, "size": str(SIZE), "backgroundColor": "00000000", **params}
        url = f"https://api.dicebear.com/10.x/{style}/png?" + urlencode(query)
        print("fetch", url)
        with urllib.request.urlopen(url, timeout=30) as response:
            path.write_bytes(response.read())
    return Image.open(path).convert("RGBA")


def to_rgb565(image: Image.Image) -> bytes:
    canvas = Image.new("RGBA", image.size, (0, 0, 0, 255))
    flat = Image.alpha_composite(canvas, image).convert("RGB")
    out = bytearray()
    for r, g, b in flat.getdata():
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += value.to_bytes(2, "little")
    return bytes(out)


def main() -> None:
    declarations = []
    definitions = []
    for name, spec in STATES.items():
        style, seed, params = spec["style"], spec["seed"], spec["params"]
        image = fetch(style, seed, params).resize((SIZE, SIZE), Image.LANCZOS)
        data = to_rgb565(image)
        assert len(data) == SIZE * SIZE * 2, len(data)
        rows = []
        for i in range(0, len(data), 16):
            rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",")
        definitions.append(
            f"static const uint8_t avatar_{name}_data[] = {{\n"
            + "\n".join(rows)
            + "\n};\n\n"
            + f"const lv_image_dsc_t avatar_{name} = {{\n"
            + "    .header = {\n"
            + "        .magic = LV_IMAGE_HEADER_MAGIC,\n"
            + "        .cf = LV_COLOR_FORMAT_RGB565,\n"
            + f"        .w = {SIZE},\n"
            + f"        .h = {SIZE},\n"
            + f"        .stride = {SIZE * 2},\n"
            + "    },\n"
            + f"    .data_size = {len(data)},\n"
            + f"    .data = avatar_{name}_data,\n"
            + "};\n"
        )
        declarations.append(f"extern const lv_image_dsc_t avatar_{name};")

    OUT_H.write_text(
        "#pragma once\n\n#include \"lvgl.h\"\n\n"
        + "\n".join(declarations)
        + "\n",
        encoding="utf-8",
    )
    OUT_C.write_text(
        "#include \"avatars.h\"\n\n" + "\n".join(definitions), encoding="utf-8"
    )
    print("wrote", OUT_C, OUT_H)


if __name__ == "__main__":
    main()
