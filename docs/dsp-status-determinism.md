# Deterministic DSP mixer status

The existing mixer returned a partly uninitialized status object. [`Mixers::GetCurrentStatus`](../emulator/src/audio_core/hle/mixers.cpp) declared `DspStatus status;` and set its two u16 fields, leaving 28 named padding bytes undefined. [`GenerateCurrentFrame`](../emulator/src/audio_core/hle/hle.cpp) assigns the entire object into guest DSP shared memory on each audio tick, so those bytes were guest-visible and included in save states.

[`DspStatus`](../emulator/src/audio_core/hle/shared_memory.h) is 32 bytes: `unknown` at offset 0, `dropped_frames` at offset 2, and 28 reserved bytes at offsets 4–31. In DSP region 0, the observed range was:

```
region_0                 0x50000
SharedMemory.dsp_status + 0x800
reserved bytes          + 0x004 .. + 0x01f
DSP RAM range           [0x50804, 0x50820)
```

The correction (`436cff8`) changes one declaration to `DspStatus status{};`. The existing two field assignments, mixer arithmetic, status layout, tick scheduling and whole-struct assignment remain unchanged. **Zero is Cara's deterministic policy for these currently reserved bytes; this is not evidence of their values on physical Nintendo hardware.**

## Diagnosis and regression

Fresh native processes loading the same original counter-ELF checkpoint produced equal ARM/VFP registers and guest ticks, but different serialized payloads. A same-configuration repeat also differed, distinguishing the issue from a configuration-specific regression. Every differing byte mapped to the status padding: the archive's 524,288-element DSP array began at offset 14,775,950, putting the typed padding at `[15,105,682, 15,105,710)`. Cross-configuration runs differed in 16 bytes there; the same-configuration repeat differed in 12. All other serialized bytes matched. The varying bytes included host-address-like values; they were not ignored or normalized.

The maintained [`cara-dsp-status-self-test`](../emulator/src/dsp_status_self_test.cpp) calls the actual public `Mixers::Tick`, poisons the assignment destination, and checks all 4,096 returned status bytes across 128 ticks, reset and reconstruction. Zero input must still yield zero output samples. Where supported, CMake compiles the production mixer and test with `-ftrivial-auto-var-init=pattern`. The original implementation fails this negative control on the first tick at byte 4 (`0xaa`); the corrected implementation passes. This tests the production call path rather than a copied status model.

`tools/build.py mac` builds the target, and the macOS checks in [the build workflow](../.github/workflows/build.yml) run it. From an existing configured build:

```sh
cmake --build work/build-mac --target cara-dsp-status-self-test
work/build-mac/bin/cara-dsp-status-self-test
```

## Native save-state proof

After applying the same correction to both native configurations, CMake genuinely regenerated matching engine identities. Six real SDL processes created fresh counter checkpoints, loaded them across configurations through normal guards, and continued for thirty refreshes. Their actual settings receipts show guest software rasterization (`use_hw_renderer=false`, `use_hw_shader=false`) and CPU/shader JIT disabled. The hosts had normal GL contexts and presentation; this was not hardware-rendered game validation. Both comparison pairs matched registers, clocks and ticks, then matched **every byte of their complete 156,063,547-byte decompressed archives**, including guest RAM and DSP buffers. Compressed payload bytes also matched; zero internal bytes were masked or normalized. Both applications normally rejected the untouched earlier-fingerprint checkpoint and left it unchanged. Save requests executed after `RunLoop` returned.

Private M1 receipts are under `profiles/dsp-status-init-20261113/{original,fixed}.log` and `profiles/decoded16-20261113/cross-state-dsp-initialized/`, including `full-state-byte-parity.json` and `old-state-rejection.json`. These establish initialization and fixture determinism, not a speed improvement, audible-quality improvement, full-game compatibility, or physical-hardware reserved-byte behavior.
