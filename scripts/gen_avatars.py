#!/usr/bin/env python3
"""Fetch DiceBear avatars and bake them into LVGL RGB565 C arrays.

    python3 scripts/gen_avatars.py

States map to styles on purpose: one friendly character per mode, not five
variations of the same face. Images are composited onto black because the
screen is black; that keeps them 2 bytes/pixel and avoids alpha blending on the
ESP32.
"""

from __future__ import annotations

import io
import pathlib
import urllib.request
from urllib.parse import urlencode

from PIL import Image

SIZE = 240
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
    "paused": {
        "style": "gaze",
        "seed": "plaud-gaze-paused",
        "params": {"shapeVariant": "circle", "eyesVariant": "tall", "bodyColor": "f2c14e"},
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
BIN_DIR = ROOT / "firmware" / "main" / "assets"
OUT_H = ROOT / "firmware" / "main" / "avatars.h"


def fetch(style: str, seed: str, params: dict[str, str]) -> Image.Image:
    ASSETS.mkdir(parents=True, exist_ok=True)
    path = ASSETS / f"{style}_{seed}.png"
    if not path.exists():
        query = {"seed": seed, "size": str(SIZE), "backgroundColor": "00000000", **params}
        url = f"https://api.dicebear.com/10.x/{style}/png?" + urlencode(query)
        print("fetch", url)
        with urllib.request.urlopen(url, timeout=30) as response:
            raw = response.read()
        # Re-encode without the IPTC/XMP chunks DiceBear embeds.
        Image.open(io.BytesIO(raw)).convert("RGBA").save(path)
    return Image.open(path).convert("RGBA")


def to_rgb565(image: Image.Image) -> bytes:
    # The screen is black, so flatten onto black: the square corners vanish and
    # only the coloured body of the mascot is visible.
    canvas = Image.new("RGBA", image.size, (0, 0, 0, 255))
    flat = Image.alpha_composite(canvas, image).convert("RGB")
    raw = flat.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        value = ((raw[i] >> 3) << 11) | ((raw[i + 1] >> 2) << 5) | (raw[i + 2] >> 3)
        out += value.to_bytes(2, "little")
    return bytes(out)


def main() -> None:
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    declarations = []
    for name, spec in STATES.items():
        style, seed, params = spec["style"], spec["seed"], spec["params"]
        image = fetch(style, seed, params).resize((SIZE, SIZE), Image.LANCZOS)
        data = to_rgb565(image)
        assert len(data) == SIZE * SIZE * 2, len(data)
        (BIN_DIR / f"avatar_{name}.bin").write_bytes(data)
        declarations.append(f"extern const lv_image_dsc_t avatar_{name};")
        print(f"wrote avatar_{name}.bin ({len(data)} bytes)")

    OUT_H.write_text(
        "#pragma once\n\n#include \"lvgl.h\"\n\n" + "\n".join(declarations) + "\n",
        encoding="utf-8",
    )
    print("wrote", OUT_H)


if __name__ == "__main__":
    main()
