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
app.update(icons)
with app_info.open('wb') as stream:
    plistlib.dump(app, stream, sort_keys=False)
