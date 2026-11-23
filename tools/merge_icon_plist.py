#!/usr/bin/env python3
"""Merge actool's icon metadata into the built app's existing Info.plist."""
import plistlib
import re
import subprocess
import sys
from pathlib import Path

app_info, icon_info = map(Path, sys.argv[1:3])
platform = sys.argv[3]
supported_platforms = {"iphoneos": "iPhoneOS", "iphonesimulator": "iPhoneSimulator"}
if platform not in supported_platforms:
    raise SystemExit(f"Unexpected iOS SDK platform: {platform}")


def xcrun(*args):
    return subprocess.check_output(["xcrun", *args], text=True).strip()

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

sdk_name = xcrun("--sdk", platform, "--show-sdk-version")
sdk_build = xcrun("--sdk", platform, "--show-sdk-build-version")
xcode = xcrun("xcodebuild", "-version")
version = re.fullmatch(r"Xcode (\d+)\.(\d+)(?:\.(\d+))?\nBuild version ([A-Za-z0-9]+)", xcode)
if not version or not re.fullmatch(r"\d+(?:\.\d+){1,2}", sdk_name) or not sdk_build:
    raise SystemExit("Could not identify the selected iOS SDK and Xcode")
major, minor, patch, xcode_build = version.groups()
app.update(icons)
app.update({
    "CFBundleSupportedPlatforms": [supported_platforms[platform]],
    "DTPlatformName": platform,
    "DTPlatformVersion": sdk_name,
    "DTSDKName": platform + sdk_name,
    "DTSDKBuild": sdk_build,
    "DTXcode": f"{major}{minor}{patch or '0'}",
    "DTXcodeBuild": xcode_build,
})
with app_info.open('wb') as stream:
    plistlib.dump(app, stream, sort_keys=False)
