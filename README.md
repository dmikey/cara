# Cara

[![No-JIT builds](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg)](https://github.com/dmikey/cara/actions/workflows/build.yml)

An experimental iOS port of Citra MMJ using an ARM interpreter, with software and experimental native GLES3 rendering. The emulator libraries are statically linked; Apple system frameworks remain dynamic. CPU and host shader JIT backends are excluded at build time.

A development build has booted a user-provided game on a physical M4 iPad. The optimized software path reached 41.2 FPS in a 600-frame device test; the first native GLES3 GPU test reached 107.8 FPS uncapped in the same early scene. Longer gameplay compatibility is still being checked. Audio is disabled. These are bounded tests, not full-game performance claims.

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

Its unsigned app is at `work/build-ios-gles3/bin/citra-ios.app`. This uses iOS's native GLES3 driver and keeps the guest ARM interpreter. GPU shader compilation is separate from CPU JIT.

The unsigned app is at `work/build-ios/bin/citra-ios.app`. Signing and physical-device installation are separate local steps using your own Apple development credentials. CI never signs or installs apps.

For the host probe and its ROM-free file I/O regression check:

```sh
python3 tools/build.py mac
python3 tools/audit_no_jit.py mac
work/build-mac/bin/citra-no-jit-probe --self-test work/io-self-test
CARA_RASTER_THREADS=1 work/build-mac/bin/citra-raster-self-test
CARA_RASTER_THREADS=4 work/build-mac/bin/citra-raster-self-test
```

Optionally test a game you supply locally:

```sh
work/build-mac/bin/citra-no-jit-probe /absolute/path/to/game.3ds work/local-probe 30
```

The probe writes logs, isolated user data, and PPM screen captures only under the output directory you provide. `work/` is ignored. Game files are never fetched by the tools or CI.

`python3 tools/build.py simulator` also configures an ARM64 simulator build; this target is not part of the initial CI matrix.

`tools/prepare.py` validates the bundled files without fetching anything. Game-specific shader caches and proprietary system/game assets are excluded. System dumps may be imported locally when a game requires them; the loader code remains present.

## Current limits

The iOS shell provides file import, both screens, basic button/circle-pad controls, lower-screen touch, Stop, and background pause. Hands-on input, saving/reloading, rotation, long sessions, and thermal behavior still need validation. Native GLES3 is experimental; direct Metal rendering and native audio are future work. See [validation notes](docs/validation.md).

## GitHub builds

Every push and pull request validates the bundled source and builds both unsigned ARM64 iOS renderer variants and the native macOS probe. All jobs audit the final binaries for excluded JIT and executable-memory facilities; the macOS job also runs the file I/O check and synthetic rasterization checks with one and four workers. Successful runs upload the unsigned iOS app or macOS probe plus the no-JIT audit report as workflow artifacts. No commercial game is required for CI.
