#!/usr/bin/env python3
"""Reject obsolete product branding while retaining attribution and compatibility."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCOPES = ['core', 'common', 'video_core', 'audio_core', 'network', 'web_service', 'ios', 'cara', 'cara_qt', 'dedicated_room', 'android']
# These are provenance or external/on-disk contracts, not Cara product branding.
ALLOWED = {
    'emulator/src/core/memory.cpp': ['adapted from Azahar commit'],
    'emulator/src/core/memory.h': ['Adapted from Azahar PR'],
    'emulator/src/core/cheats/gateway_cheat.cpp': ['"*citra_enabled"'],
    'emulator/src/common/file_util.cpp': ['"citra-emu"'],
    'emulator/src/web_service/verify_user_jwt.cpp': ['issuer("citra-core")'],
    'emulator/src/cara/config.cpp': ['"citra_username"', '"citra_token"'],
    'emulator/src/cara_qt/configuration/config.cpp': ['"citra_username"', '"citra_token"'],
    'emulator/src/dedicated_room/cara-room.cpp': ['"CitraRoom-BanList-1"'],
    'emulator/src/android/app/src/main/java/org/cara/emu/utils/CaraDirectory.java': ['"citra-emu"'],
    'emulator/src/android/app/src/main/java/org/cara/emu/ui/EditorActivity.java': ['"*citra_enabled"'],
}
failures = []
checked = 0
for scope in SCOPES:
    for path in sorted((ROOT / 'emulator/src' / scope).rglob('*')):
        if not path.is_file() or path.suffix not in {'.cpp', '.h', '.mm', '.inc', '.txt', '.java', '.xml', '.gradle', '.ui', '.qrc'}:
            continue
        relative = path.relative_to(ROOT).as_posix()
        checked += 1
        for number, line in enumerate(path.read_text(errors='surrogateescape').splitlines(), 1):
            if not re.search(r'citra|azahar', line, re.I):
                continue
            if re.search(r'copyright|https?://', line, re.I):
                continue
            if relative.startswith('emulator/src/android/') and '*citra_enabled' in line and (path.name == 'strings.xml' or '/assets/cheats/' in relative):
                continue
            if any(token in line for token in ALLOWED.get(relative, [])):
                continue
            failures.append(f'{relative}:{number}: {line.strip()}')
notice = (ROOT / 'NOTICE').read_text()
if 'Copyright 2026 Cara contributors' not in notice or 'GPL-2.0-or-later' not in notice:
    failures.append('NOTICE must identify Cara contributions and their license')
if failures:
    raise SystemExit('\n'.join(failures))
print(f'Cara branding verified in {checked} source files; attribution and compatibility retained.')
