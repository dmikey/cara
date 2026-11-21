#!/usr/bin/env python3
"""Merge actool's icon metadata into the built app's existing Info.plist."""
import plistlib
import sys
from pathlib import Path

app_info, icon_info = map(Path, sys.argv[1:])
with app_info.open('rb') as stream:
    app = plistlib.load(stream)
with icon_info.open('rb') as stream:
    icons = plistlib.load(stream)
expected_alternates = {"AppIconSilver", "AppIconAmber", "AppIconLime", "AppIconOnyx"}
for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
    metadata = icons.get(key, {})
    if metadata.get("CFBundlePrimaryIcon", {}).get("CFBundleIconName") != "AppIcon":
        raise SystemExit(f"Missing Cara primary icon in {key}")
    alternates = metadata.get("CFBundleAlternateIcons", {})
    missing = {name for name in expected_alternates
               if alternates.get(name, {}).get("CFBundleIconName") != name}
    if missing:
        raise SystemExit(f"Missing Cara alternate icons in {key}: {', '.join(sorted(missing))}")

app.update(icons)
with app_info.open('wb') as stream:
    plistlib.dump(app, stream, sort_keys=False)
