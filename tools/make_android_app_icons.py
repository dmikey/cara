#!/usr/bin/env python3
"""Build Android's launcher assets from the five bundled Cara iOS icons."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "emulator/src/ios/Assets.xcassets"
RES = ROOT / "emulator/src/android/app/src/main/res"

VARIANTS = {
    "ic_cara": ("AppIcon", "#f8f4f6"),
    "ic_cara_silver": ("AppIconSilver", "#eeeeee"),
    "ic_cara_amber": ("AppIconAmber", "#f6e0cb"),
    "ic_cara_lime": ("AppIconLime", "#e7f2d8"),
    "ic_cara_onyx": ("AppIconOnyx", "#212327"),
}

DEFAULT_DENSITIES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def convert(source, target, *options):
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["magick", str(source), *options, "-strip", str(target)], check=True)


def main():
    for name, (variant, background) in VARIANTS.items():
        source = CATALOG / f"{variant}.appiconset/icon-1024.png"
        foreground = name + "_artwork"
        layer = RES / f"drawable-nodpi/{foreground}.png"
        # Place the citrus glyph inside Android's adaptive icon safe zone and extend
        # the artwork's backplate to the full bleed area of the foreground layer.
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
    colors = "".join(f'    <color name="{name}_background">{color}</color>\n'
                     for name, (_, color) in VARIANTS.items())
    (RES / "values/cara_launcher_colors.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n' + colors + '</resources>\n')


if __name__ == "__main__":
    main()
