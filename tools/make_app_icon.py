#!/usr/bin/env python3
"""Prepare the bundled Cara artwork as a complete iOS app-icon catalog."""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'emulator/src/ios/Assets.xcassets'
ARTWORK = ROOT / 'emulator/src/ios/artwork'
ICONS = {
    'AppIcon': 'cara-orange.png',
    'AppIconSilver': None,
    'AppIconAmber': None,
    'AppIconLime': None,
    'AppIconOnyx': None,
}
INFO = dict(version=1, author='cara')


def resize(source, target, pixels):
    subprocess.run(['sips', '--resampleHeightWidth', str(pixels), str(pixels),
                    str(source), '--out', str(target)], check=True, stdout=subprocess.DEVNULL)


def generate_icon(name, artwork):
    target = CATALOG / f'{name}.appiconset'
    target.mkdir(parents=True, exist_ok=True)
    source = ARTWORK / artwork if artwork else target / 'icon-1024.png'
    images = []
    for idiom, sizes in [
        ('iphone', [('20', [2, 3]), ('29', [2, 3]), ('40', [2, 3]), ('60', [2, 3])]),
        ('ipad', [('20', [1, 2]), ('29', [1, 2]), ('40', [1, 2]),
                  ('76', [1, 2]), ('83.5', [2])]),
        ('ios-marketing', [('1024', [1])]),
    ]:
        for size, scales in sizes:
            for scale in scales:
                pixels = round(float(size) * scale)
                images.append(dict(idiom=idiom, size=f'{size}x{size}', scale=f'{scale}x',
                                   filename=f'icon-{pixels}.png'))
    for pixels in sorted({round(float(item['size'].split('x')[0]) * int(item['scale'][0]))
                          for item in images}):
        output = target / f'icon-{pixels}.png'
        if output != source:
            resize(source, output, pixels)
    (target / 'Contents.json').write_text(json.dumps(dict(images=images, info=INFO), indent=2) + '\n')

    preview = CATALOG / f'{name}Preview.imageset'
    preview.mkdir(parents=True, exist_ok=True)
    resize(source, preview / 'icon-88.png', 88)
    images = [dict(idiom='universal', scale='2x', filename='icon-88.png')]
    (preview / 'Contents.json').write_text(json.dumps(dict(images=images, info=INFO), indent=2) + '\n')


def main():
    CATALOG.mkdir(parents=True, exist_ok=True)
    (CATALOG / 'Contents.json').write_text(json.dumps(dict(info=INFO), indent=2) + '\n')
    for name, artwork in ICONS.items():
        generate_icon(name, artwork)
    print(CATALOG)


if __name__ == '__main__':
    main()
