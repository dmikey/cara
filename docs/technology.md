# Cara’s architecture and foundations

## What makes Cara different

Cara builds on **Citra MMJ**, weihuoya's performance-focused Android fork.
MMJ brought a mobile-first approach to 3DS rendering. Cara carries that foundation
forward for Apple hardware with its own GPU pipeline, memory optimizations, and
native presentation.

| MMJ's mobile foundation | Cara's Apple-focused enhancements |
|---|---|
| **Mobile GPU rendering** | A custom Apple vertex pipeline builds on MMJ's OpenGL ES renderer, putting more graphics work on the GPU. |
| **Shader generation and reuse** | MMJ's shader foundation powers Cara's custom Apple GPU shaders, moving vertex processing and packed-data decoding off the CPU. |
| **Mobile performance tuning** | Cara extends that performance focus with prepared-input reuse, bounded GPU caches, and fewer CPU/GPU transfers. |

- **Azahar FastInterp.** Cara adapts Azahar's ARM interpreter for execution without
  CPU JIT. Guest instructions become decoded data dispatched to precompiled
  handlers in the signed app. Cara adds integration with its memory, scheduling,
  and save-state systems.
- **Custom rendering enhancements.** Cara's GLES3 pipeline moves PICA vertex work
  and packed vertex decoding onto the GPU, reuses prepared input data, and reduces
  repeated uploads and CPU/GPU transfers. Direct GPU presentation avoids copying
  every displayed frame through CPU memory. Rendering correctness and bounded
  cache memory are part of the performance work.

## A reusable core and a native shell

Cara is developed as a reusable 3DS core for integration into Delta, alongside
its own standalone iOS shell. The shell provides game loading from Files, touch
controls, customizable Delta-compatible skins, audio, save states, and iCloud
save-folder support. Delta integration is a development target; the standalone
Cara app is the playable build provided here.

## Source and attribution

Cara builds on GPL-2.0-or-later emulator code. Original copyright notices are
preserved. See [NOTICE](../NOTICE), [source revisions](../emulator/VENDORED_SOURCE.json),
and [FastInterp provenance](fastinterp-upstream.json) for contributors and pinned revisions.
