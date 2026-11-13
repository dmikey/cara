# Inspection tool attributions

Cara's development inspection and source research use the following external
work. These tools and reference projects are separate from the bundled emulator
dependencies recorded in [source provenance](../emulator/VENDORED_SOURCE.json).

| Project | Credit and license information | Use in Cara development | Reviewed version |
| --- | --- | --- | --- |
| [Capstone](https://www.capstone-engine.org/) | Designed and implemented by Nguyen Anh Quynh and contributors. Copyright (c) 2013, COSEINC. BSD-style license in the installed distribution's `LICENSE.TXT`. | Python bindings for offline ARM/Thumb disassembly of privately held instruction bytes; not bundled with the application. | 5.0.6 |
| [3dsrecomp](https://github.com/fearkov/3dsrecomp) | fearkov; its `Cargo.toml` declares MIT. | Read-only reference for guest-image formats, extraction/decompression inspection, code analysis and verification approaches. No source imported and no generated native guest code executed. | `6746f50bdce75c058005b2fb5d0404413f4bef9d` |
| [Zakuro](https://github.com/fearkov/zakuro) | Copyright (c) 2026 fearkov; MIT. | Read-only research into vertex input selection, resource ownership and GPU command handling. No source imported. | `7e4e1223c06710f2e9e009652eed025dc1d1b160` |

Capstone's installed license notice is the authoritative notice for that tool.
Its source and license are available from the
[5.0.6 release](https://github.com/capstone-engine/capstone/tree/5.0.6).
Reference-project license information was checked against the reviewed checkout:
[3dsrecomp manifest](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/Cargo.toml)
and [Zakuro license](https://github.com/fearkov/zakuro/blob/7e4e1223c06710f2e9e009652eed025dc1d1b160/LICENSE).

The repository's guest-range mapper uses Python's standard library. Private ROM
images, decoded guest code and disassembly outputs remain outside the repository.
See [offline guest mapping](guest-hot-range-mapping.md) and
[the source comparison](zakuro-recomp-prior-art.md) for methods and limitations.
