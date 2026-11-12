#!/usr/bin/env python3
"""Prepare the bundled Cara artwork as a complete iOS app-icon catalog."""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'emulator/src/ios/Assets.xcassets'
ARTWORK = ROOT / 'emulator/src/ios/artwork/cara-orange.png'


def main():
    target = CATALOG / 'AppIcon.appiconset'
    target.mkdir(parents=True, exist_ok=True)
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
        subprocess.run(['sips', '--resampleHeightWidth', str(pixels), str(pixels),
                        str(ARTWORK), '--out', str(target / f'icon-{pixels}.png')],
                       check=True, stdout=subprocess.DEVNULL)
    info = dict(version=1, author='cara')
    (CATALOG / 'Contents.json').write_text(json.dumps(dict(info=info), indent=2) + '\n')
    (target / 'Contents.json').write_text(json.dumps(dict(images=images, info=info), indent=2) + '\n')
    print(target)


if __name__ == '__main__':
    main()
