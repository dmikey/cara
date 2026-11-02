# Cara

[![No-JIT builds](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg)](https://github.com/dmikey/cara/actions/workflows/build.yml)

An experimental iOS port of Citra MMJ using an ARM interpreter and software rendering. The emulator libraries are statically linked; Apple system frameworks remain dynamic. CPU and host shader JIT backends are excluded at build time.

A development build has booted a user-provided game on a physical M4 iPad. Early samples ranged from about 10–16 FPS. This is a working compatibility baseline, with audio disabled—not a full-speed or broadly validated emulator.

## Repository contents

- `port/`: iOS app, software screen presentation, and a ROM-free host diagnostic.
- `patches/`: no-JIT switches and portability fixes against the exact upstream revision.
- `upstream.lock.json`: pinned MMJ source revision.
- `tools/`: reproducible source preparation, builds, and linked-binary audit.
- `.github/workflows/build.yml`: independent ARM64 iOS and macOS builds.

No ROMs, game screenshots, firmware, keys, signing credentials, profiles, or signed app packages belong in this repository. Upstream code and the changes here retain their applicable GPL licensing; see [NOTICE](NOTICE) and [LICENSE](LICENSE).

## Build on macOS

Install full Xcode and select it, or set `DEVELOPER_DIR` to its `Contents/Developer` directory. Python 3, Git, and network access to the public dependency repositories are required.

```sh
python3 -m venv work/tools
work/tools/bin/python -m pip install cmake==3.31.10 ninja==1.13.2
python3 tools/prepare.py
python3 tools/build.py ios
python3 tools/audit_no_jit.py ios
```

The unsigned app is at `work/build-ios/bin/citra-ios.app`. Signing and physical-device installation are separate local steps using your own Apple development credentials. CI never signs or installs apps.

For the host probe and its ROM-free file I/O regression check:

```sh
python3 tools/build.py mac
python3 tools/audit_no_jit.py mac
work/build-mac/bin/citra-no-jit-probe --self-test work/io-self-test
```

Optionally test a game you supply locally:

```sh
work/build-mac/bin/citra-no-jit-probe /absolute/path/to/game.3ds work/local-probe 30
```

The probe writes logs, isolated user data, and PPM screen captures only under the output directory you provide. `work/` is ignored. Game files are never fetched by the tools or CI.

`python3 tools/build.py simulator` also configures an ARM64 simulator build; this target is not part of the initial CI matrix.

Source preparation refuses to overwrite an existing unmarked checkout or silently discard local changes after port inputs change. Preserve your edits and move `work/citra-mmj` aside before preparing updated inputs. Upstream submodules are checked against their exact pinned commits. Unavailable JIT/web-service dependencies are excluded from the no-JIT build.

## Current limits

The iOS shell provides file import, both screens, basic button/circle-pad controls, lower-screen touch, Stop, and background pause. Hands-on input, saving/reloading, rotation, long sessions, and thermal behavior still need validation. Hardware graphics acceleration and native audio are future work. See [validation notes](docs/validation.md).

## GitHub builds

Every push and pull request builds the unsigned ARM64 iOS app and the native macOS probe. Both jobs audit the final binaries for excluded JIT and executable-memory facilities; the macOS job also runs the file I/O regression check. Successful runs upload the unsigned iOS app or macOS probe plus the no-JIT audit report as workflow artifacts. No commercial game is required for CI.
