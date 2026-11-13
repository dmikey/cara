# GPU surface alias conversion

## Luigi diagnostic, 2026-11-13

A physical M4 iPad replay of the advanced laboratory dialogue used FastInterp,
GLES3, native resolution, 100% guest CPU, 100% speed cap and sound. Bounded host
pacing was enabled; GPU completion timing and persistent shader caching were
disabled. The run covered 1,200 refreshes with the first 300 excluded. Exclusive
CPU categories, renderer bookkeeping and sampled API CPU timing were enabled,
so these numbers include instrumentation overhead.

| Measurement per emulated refresh | Time |
| --- | ---: |
| Elapsed, including pacing | 24.685 ms |
| Emulation-thread CPU | 15.737 ms |
| Surface download wall time | 9.949 ms |
| Framebuffer acquisition wall time | 10.418 ms |
| Exclusive guest CPU execution including memory | 7.239 ms |
| Exclusive GPU command processing including renderer | 7.945 ms |

The wall scopes overlap: surface download is nested in framebuffer acquisition
and GPU command processing. They must not be added together. The wall/thread-CPU
gap includes blocking and scheduling, not exclusively GPU execution. The current
capture does not isolate thread CPU spent inside `glReadPixels`.

Earlier validation traces identified a frequent RGB8 linear 128 by 416 surface
aliased as RGBA8 tiled 240 by 320. The original GPU remapper supports RGBA8 only
and requires the entire destination allocation inside one source, even when the
caller needs a smaller rectangle. Unsupported aliases fall back through GPU
readback, guest-memory packing, CPU reload and GPU upload.

The optimization target is byte-preserving GPU conversion of valid source-owned
regions. RGB8/RGBA8 reinterpretation must gather guest bytes across pixel
boundaries, preserve channel packing and tiling, and mark only the converted
interval valid. Unsupported formats, scale, padding, or invalid source regions
must retain the existing fallback. This is a shared cache operation; there are
no title IDs, ROM edits, guest-clock changes or draw dropping in its contract.

Evidence: `luigi-alias-diagnostic-20261113/bounded_diagnostic-frame-timing.json`
and its comparison receipt under the private profiling directory. Baseline code
is b7a343b (documentation HEAD 5ffd683); state fingerprint alone does not identify
all frontend/header changes. This document records a bottleneck, not a validated
optimization gain.

## Experimental byte remapper

`CARA_GPU_BYTE_SURFACE_REMAP=1` enables source-owned partial-interval conversion
in the existing remapper. It is off by default and also requires surface remapping
to be enabled. Dense native RGB8/RGBA8 surfaces are supported in either tiling
layout. The original equal-format one-fetch path remains available. Unsupported
formats, padding, scaling and ownership seams inside a destination pixel/tile
fall back to existing validation.

The physical M4 iPad surface-coherence fixture passed on 2026-11-13. It exercises
all RGB/RGBA layout directions, partial updates, CPU-invalidated neighbors and
the Luigi-sized destination split across two producers. Poisoned RAM checks
verify that eligible validation stays on the GPU. The native interval regression
also passed under ASan/UBSan and is wired into macOS CI. These correctness checks
do not establish a game performance gain; matched replay results follow below
when available.

Readback diagnostics now sample wall and thread CPU separately for the complete
surface download and the `glReadPixels` call itself. Explicit diagnostic
`glFinish()` remains a separate scope and is not enabled for normal comparisons.
