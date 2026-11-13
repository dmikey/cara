# Cara — Nintendo 3DS emulator

<img src="emulator/src/ios/artwork/cara-orange.png" alt="Cara icon" width="104" align="right">

Cara is an open-source Nintendo 3DS emulator core and emulator shell for iPhone
and iPad. It runs without CPU JIT, with hardware-accelerated OpenGL ES graphics
and a native iOS interface.

Built on Citra MMJ and Azahar's FastInterp, Cara develops its own interpreter,
rendering, memory, and timing improvements with a focus on Apple hardware.
Compatibility and performance vary by game; the project is under active development.

[Build instructions](docs/build.md) ·
[Documentation](#documentation) ·
[Report an issue](https://github.com/dmikey/cara/issues) ·
[Build status](https://github.com/dmikey/cara/actions/workflows/build.yml)

## Features

- Load games from Files, with touch controls and customizable controller skins.
- GPU-accelerated graphics and stereo audio.
- Save states with per-game slots.
- Sync in-game saves through a selected iCloud Drive folder.
- Optional adaptive guest CPU clock for demanding scenes.

Cara separates the emulator core from its native shell. Integration into Delta
is a development target; it is not currently a bundled Delta core.

Game ROMs, Nintendo firmware, and keys are not included.

## Building

The iOS build requires macOS, Xcode, Python 3, CMake, and Ninja. Emulator and
dependency source is bundled in this repository.

```sh
python3 tools/prepare.py
python3 tools/build.py ios --renderer gles3
python3 tools/audit_no_jit.py ios --renderer gles3
```

See the [build guide](docs/build.md) for tool versions, output paths, and signing
requirements.

## Documentation

- [Save states](docs/save-states.md)
- [Adaptive CPU clock](docs/adaptive-clock.md)
- [Performance measurements](docs/performance.md)
- [Core architecture and rendering](docs/technology.md)
- [FastInterp](docs/fastinterp.md)
- [Testing and validation](docs/validation.md)

## Contributing

Bug reports, compatibility testing, documentation, and code contributions are
welcome. Please check [existing issues](https://github.com/dmikey/cara/issues)
before opening a report.

For game issues, include the Cara revision or build, device model, iOS version,
game title and region, relevant settings, and steps to reproduce. Include logs
when available; do not attach game ROMs, keys, or firmware.

For code changes, describe the problem and how you tested the fix. Performance
changes should include comparable before-and-after results and checks for
rendering and emulation correctness. See the [validation guide](docs/validation.md).

## Credits and license

Cara builds on the work of:

- **[Citra](https://github.com/citra-emu)** and its contributors, for the original
  emulator and core emulation work.
- **[Citra MMJ](https://github.com/weihuoya/citra)**, maintained by
  [weihuoya](https://github.com/weihuoya), for the mobile-focused fork that forms
  Cara's source foundation.
- **[Azahar](https://github.com/azahar-emu/azahar)** and its contributors, including
  [warmenhoven](https://github.com/warmenhoven), for
  [FastInterp](https://github.com/azahar-emu/azahar/pull/2376) and adapted core improvements.
- **Hamish Milne and other Citra contributors**, for the original save-state work
  restored from MMJ and adapted for Cara.

Cara is licensed under **GPL-2.0-or-later**. Upstream copyright notices are
preserved, and third-party libraries retain their respective licenses.
See [LICENSE](LICENSE), [NOTICE](NOTICE),
[source revisions](emulator/VENDORED_SOURCE.json), and
[FastInterp provenance](docs/fastinterp-upstream.json).

Cara is not affiliated with Nintendo.
