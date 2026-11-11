# Cara

[![No-JIT builds](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg)](https://github.com/dmikey/cara/actions/workflows/build.yml)

An experimental iOS port of Citra MMJ using an ARM interpreter, with software and experimental native GLES3 rendering. The emulator libraries are statically linked; Apple system frameworks remain dynamic. CPU and host shader JIT backends are excluded at build time.

A development build has booted a user-provided game on a physical M4 iPad. The optimized software path reached 41.2 FPS in a 600-frame device test; the first native GLES3 GPU test reached 107.8 FPS uncapped in the same early scene. Direct GPU presentation followed by GPU vertex processing reached 79.5 FPS uncapped in a 3,600-frame sequence. These older counters measure emulated display refreshes, not unique game frames. The app now shows speed percentage; 100% is full emulation speed. Some scenes still fall below full speed. Longer gameplay compatibility is still being checked. Native iOS audio is available; direct playback is the default, with optional audio stretching. These are bounded tests, not full-game performance claims.

## Repository contents

- `emulator/`: the complete emulator source needed by the iOS/macOS builds, with Cara changes applied directly.
- `emulator/src/ios/`: native app shell.
- `emulator/externals/`: bundled library source, including the previously unavailable MMJ-specific forks.
- `emulator/VENDORED_SOURCE.json`: original source revisions and the excluded non-source assets.
- `tools/`: local builds, source validation, and linked-binary audits.
- `.github/workflows/build.yml`: independent ARM64 iOS and macOS builds.

An ordinary clone contains the emulator and dependency sources. No build step clones MMJ, downloads patches, or initializes source submodules. Xcode, Python, CMake and Ninja are standard external build tools. Their availability does not depend on the original MMJ repository.

No ROMs, game screenshots, Nintendo NAND/firmware data, shared fonts, PKSM sprite assets, signing credentials, profiles, or signed app packages belong in this repository. Upstream code and the changes here retain their applicable licenses; see [NOTICE](NOTICE), [LICENSE](LICENSE), and each dependency's license.

## Build on macOS

Install full Xcode and select it, or set `DEVELOPER_DIR` to its `Contents/Developer` directory. Python 3 and Git are required. Network access is needed only to clone Cara and install standard build tools.

```sh
python3 -m venv work/tools
work/tools/bin/python -m pip install cmake==3.31.10 ninja==1.13.2
python3 tools/prepare.py
python3 tools/build.py ios
python3 tools/audit_no_jit.py ios
```

For the experimental hardware renderer:

```sh
python3 tools/build.py ios --renderer gles3
python3 tools/audit_no_jit.py ios --renderer gles3
```

Its unsigned app is at `work/build-ios-gles3/bin/cara-ios.app`. This uses iOS's native GLES3 driver and keeps the guest ARM interpreter. GPU shader compilation is separate from CPU JIT.

The unsigned app is at `work/build-ios/bin/cara-ios.app`. Signing and physical-device installation are separate local steps using your own Apple development credentials. CI never signs or installs apps.

For the host probe and its ROM-free file I/O regression check:

```sh
python3 tools/build.py mac
python3 tools/audit_no_jit.py mac
work/build-mac/bin/cara-no-jit-probe --self-test work/io-self-test
CARA_RASTER_THREADS=1 work/build-mac/bin/cara-raster-self-test
CARA_RASTER_THREADS=4 work/build-mac/bin/cara-raster-self-test
```

Optionally test a game you supply locally:

```sh
work/build-mac/bin/cara-no-jit-probe /absolute/path/to/game.3ds work/local-probe 30
```

The probe writes logs, isolated user data, and PPM screen captures only under the output directory you provide. `work/` is ignored. Game files are never fetched by the tools or CI.

The supplied builds omit guest GDB polling from the interpreter's inner loop.
Use `--guest-debugger` with `tools/build.py` when building for guest breakpoints.
This setting is separate from Xcode device debugging and CPU JIT remains disabled.

`python3 tools/build.py simulator` also configures an ARM64 simulator build; this target is not part of the initial CI matrix.

`tools/prepare.py` validates the bundled files without fetching anything. Game-specific shader caches and proprietary system/game assets are excluded. System dumps may be imported locally when a game requires them; the loader code remains present.

## Current limits

The iOS shell provides file import, both screens, basic button/circle-pad controls, lower-screen touch, Stop, and background pause. Hands-on input, saving/reloading, rotation, long sessions, and thermal behavior still need validation. Native GLES3 is experimental; direct Metal rendering and native audio are future work. See [validation notes](docs/validation.md).

## GitHub builds

Every push and pull request validates the bundled source and builds both unsigned ARM64 iOS renderer variants and the native macOS probe. All jobs audit the final binaries for excluded JIT and executable-memory facilities; the macOS job also runs the file I/O check and synthetic rasterization checks with one and four workers. Successful runs upload the unsigned iOS app or macOS probe plus the no-JIT audit report as workflow artifacts. No commercial game is required for CI.

### Files, controller skins, and save sync

Cara can open extracted `.3ds`, `.cci`, `.cxi`, `.3dsx`, and `.elf` files from
**Menu → Open game**, or from **Open in Cara** in Files. Archives must be extracted
first. Files are opened in place; keep the provider and original file available.
**Open recent game** retains Files access with a system bookmark.

**Menu → Choose iCloud save folder** opens the native folder picker. Select a
folder in iCloud Drive on each device to share saves. Cara writes a `Cara Saves`
subfolder there; iCloud Drive handles its upload and download. A successful local
folder write does not confirm server upload. This also works with local folders
and other writable Files providers, without CloudKit credentials.

Only SD/NAND game save and extdata files are included. ROMs, title content,
shader caches, and controller skins are excluded. Cara snapshots committed saves
at an emulation boundary after a short quiet period and again after shutdown.
Before opening a game, it checks cloud versions while the emulator is stopped.
Stop the game to use **Sync saves now** or change folders. Concurrent edits prompt
for a version; no conflict silently overwrites local saves. Checksummed immutable
cloud snapshots and local `Documents/cara/SaveBackups` preserve recovery copies.
History is retained rather than automatically pruned. Keep the app foregrounded
long enough for pending provider writes; offline/provider failures remain visible
and can be retried with Sync saves now.

The included original **Carbon Duo** skin uses Delta's ZIP/`info.json`/PDF layout
format with Cara's 3DS controls (Circle Pad, C-stick, ZL/ZR). Import `.deltaskin`
or `.caraskin` from the menu or Files. Compatible DS layouts can be used as a
starting point; Cara fits the native 3DS screens inside their display windows.
This format support does not add 3DS emulation to Delta itself. Regenerate the
bundled artwork/archive with `tools/make_skin.py` (ReportLab and Pillow required
only for regeneration). All shipped skin assets are included in this repository.

The shipping app and Cara-owned targets use the Cara name and bundle identifier
`com.dmikey.cara`. Its bundled emulator engine is derived from Citra MMJ; upstream
copyrights, GPL license notices, and provenance remain in the source.

### iOS sound and speed display

Cara outputs the DSP’s stereo PCM16 stream through native iOS Audio Queue playback
at the 3DS sample rate (32,728 Hz). Audio follows game/menu/background pauses and
system interruptions. **Mute sound** in the menu is remembered between launches.
Direct playback is the default to avoid stretching artifacts at full speed. The menu
can enable audio stretching when emulation is slow; that choice is remembered.
Direct playback uses a short source prebuffer and fades on underrun/recovery.
The existing PCM/ADPCM DSP mixer is used; the optional AAC decoder is still absent
in the bundled iOS configuration and must be verified separately for titles using AAC.

The HUD reports emulation **speed**, with 100% corresponding to about 59.834
emulated display refreshes per second. Xenoblade Chronicles 3D targets about 30
unique game frames per second; display refresh count is not unique game FPS.
For diagnostics, `CARA_AUDIO_DIAGNOSTICS=1` writes native queue/callback/PCM
activity to `Documents/cara/audio-status.json` from the emulation thread. No
recorded game audio is included. `CARA_AUDIO_DISABLED=1` selects the silent sink
for controlled performance comparisons. Neither setting enables host JIT.
