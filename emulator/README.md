# Cara engine

This directory contains Cara's Nintendo 3DS emulation engine and bundled
dependencies. It derives from Citra MMJ, incorporates Azahar's FastInterp CPU
interpreter, and includes Cara's iOS, no-JIT, graphics, audio and save-state work.

Build and development instructions are in the [project README](../README.md).

## Source attribution

- [Citra Emulator Project](https://github.com/citra-emu) and its contributors:
  the original emulator and core emulation work.
- [Citra MMJ](https://github.com/weihuoya/citra), maintained by
  [weihuoya](https://github.com/weihuoya): Cara's source foundation.
- [Azahar](https://github.com/azahar-emu/azahar) and its contributors:
  FastInterp and adapted core improvements.

Exact revisions and dependency provenance are recorded in
[VENDORED_SOURCE.json](VENDORED_SOURCE.json) and
[FastInterp provenance](../docs/fastinterp-upstream.json). Additional credits are
in [NOTICE](../NOTICE).

## License

Cara-specific modifications and additions are copyright 2026 Cara contributors.
Original upstream copyright notices are retained. The engine and Cara's changes
are licensed under GPL-2.0-or-later; see [license.txt](license.txt) and the
[project license](../LICENSE). Third-party dependencies retain their respective
licenses in `externals/`.
