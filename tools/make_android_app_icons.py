#!/usr/bin/env python3
"""Build Android's launcher resources from the five Cara painted colorways."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTWORK = ROOT / "emulator/src/android/artwork/launcher"
RES = ROOT / "emulator/src/android/app/src/main/res"

VARIANTS = {
    "ic_cara": "citrus",
    "ic_cara_silver": "silver",
    "ic_cara_amber": "amber",
    "ic_cara_lime": "lime",
    "ic_cara_onyx": "onyx",
}

DEFAULT_DENSITIES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def convert(source, target, *options):
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["magick", str(source), *options, "-strip", str(target)], check=True)


def main():
    for name, variant in VARIANTS.items():
        source = ARTWORK / f"{variant}.png"
        foreground = name + "_artwork"
        layer = RES / f"drawable-nodpi/{foreground}.png"
        # Keep the painted glyph inside Android's adaptive icon safe zone and
        # extend the dark background behind it to the full-bleed area of the layer.
        convert(source, layer, "-resize", "304x304!", "-virtual-pixel", "edge",
                "-set", "option:distort:viewport", "432x432-64-64", "-distort", "SRT", "0")

        names = [name]
        if name == "ic_cara":
            names.append("ic_cara_round")
            for density, pixels in DEFAULT_DENSITIES.items():
                convert(source, RES / f"mipmap-{density}/{name}.png", "-resize", f"{pixels}x{pixels}!")
                center = (pixels - 1) / 2
                convert(source, RES / f"mipmap-{density}/{name}_round.png",
                        "-resize", f"{pixels}x{pixels}!", "-alpha", "off", "(",
                        "-size", f"{pixels}x{pixels}", "xc:black", "-fill", "white",
                        "-draw", f"circle {center},{center} {center},0", "-alpha", "off", ")",
                        "-compose", "CopyOpacity", "-composite")
        else:
            convert(source, RES / f"mipmap-nodpi/{name}.png", "-resize", "192x192!")

        for asset in names:
            target = RES / f"mipmap-anydpi-v26/{asset}.xml"
            target.write_text(
                '<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                f'    <background android:drawable="@color/{name}_background" />\n'
                f'    <foreground android:drawable="@drawable/{foreground}" />\n'
                '</adaptive-icon>\n'
            )
    colors = "".join(f'    <color name="{name}_background">#0b0b0b</color>\n'
                     for name in VARIANTS)
    (RES / "values/cara_launcher_colors.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n' + colors + '</resources>\n')


if __name__ == "__main__":
    main()
