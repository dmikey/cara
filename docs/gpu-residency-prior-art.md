# GPU residency: source review and small CPU writes

This is a read-only source/evidence review dated 2026-11-13. It records existing
contracts and experiment readiness; it establishes no new performance gain.
Current byte-remap measurements are documented separately.

## Inspecting surface flush traces

`tools/summarize_surface_flushes.py` reads an existing `surface-flushes.json`
without touching a device or emulator state. For example:

```sh
python3 tools/summarize_surface_flushes.py /private/path/surface-flushes.json
python3 tools/summarize_surface_flushes.py /private/path/surface-flushes.json --rank calls
python3 tools/summarize_surface_flushes.py /private/path/surface-flushes.json --rank bytes --limit 10
```

The output ranks groups by wall time, call count, or transferred readback bytes.
Groups retain reason, source format, tiling, stride, dimensions, scale and
source/destination allocation ranges. `end_exclusive` is allocation address plus
size; it is not the last downloaded address. Distinct download rectangles and
request sizes within the same allocation group can be combined. The displayed
request sizes remain distinct per-call values rather than summed byte totals.
Missing older allocation metadata remains explicitly unavailable.

Field meanings follow `RecordFlush` and `WriteFlushTrace` in
`emulator/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp`:

| Field | Meaning and limitation |
| --- | --- |
| `calls` | Cumulative recorded download calls across the entire trace interval. |
| `readback_bytes` | Cumulative GL transfer bytes, including repeated transfers and GLES RGBA expansion; not unique guest bytes. |
| `wall_ms` | Cumulative elapsed time around surface download, including nested waits, read and packing. |
| `wait_ms`, `read_ms` | Available only with `wait_instrumented: true`. Otherwise JSON `null` means unavailable, including when the original counters contain zero. Missing instrumented fields also remain unavailable. |
| `pack_ms` | Cumulative CPU repacking elapsed time when supplied by the producer. It overlaps download wall time. |
| `request_sizes_bytes` | Distinct sizes supplied to `FlushRegion`, not total traffic and not necessarily the initiating CPU-write size. The small-write path can request a whole-surface flush. |
| `copy_reject_bits` | Distinct producer diagnostic masks, retained without interpreting them as definitive reasons a new optimization is safe. |
| `overflow_mixed` | The producer's bounded catch-all bucket. Reason and allocation placeholders are not real attribution; their grouped output is unknown. |

These counters contain no timestamps or refresh-window boundaries. They may
include warmup and activity outside the requested benchmark refresh interval;
they cannot be filtered to the measured windows in `frame-timing.json`. Totals
always include every input bucket even when `--limit` truncates the ranked rows.
The tool does not infer a frame count. Use `--frames N` only when the caller has
independently established that N covers the **same whole trace interval**, not
just the measured frames after warmup. The output labels this normalization as
user-asserted. Reused allocations can also share a group; the trace has no
resource generation identifier.

`CARA_READBACK_WAIT_TRACE=1` adds an explicit `glFinish` before `glReadPixels`.
It changes synchronization and is a diagnostic, not an uninstrumented throughput
comparison. Wait, read and pack times overlap wall time and must not be added to
it. A CPU/wall gap also includes scheduling and other waits; it does not directly
measure GPU execution time.

Generated regression fixtures run with:

```sh
python3 -m unittest discover -s tests -p 'test_summarize_surface_flushes.py' -v
```

## Existing small-write experiment

`CARA_RETAIN_SMALL_CPU_WRITES=1` enables the existing RGBA8-only retention path.
The default path flushes and removes a cached surface on a CPU write of at most
eight bytes. The retained path instead marks only the written byte interval
invalid, invalidates watchers and removes those written bytes from GPU dirty
ownership. Other dirty bytes remain authoritative on the GPU until an actual
consumer needs them. `skip_cpu_write` must remain disabled for this experiment.
The existing linear RGBA8/RGB8 flush helper preserves neighboring CPU-owned bytes
when dirty ownership splits inside a pixel.

The generated production-path coherence suite tests retention off and on,
tiled/linear layouts, five color formats, CPU-read and texture-revalidation
consumers, pixel interiors/edges, tile and row edges, and the final byte. It
poisons CPU RAM and staging memory so stale copies cannot accidentally satisfy
the byte comparison. Only RGBA8 takes the retention branch; other formats verify
the unchanged fallback. The suite calls cache invalidation and writes scratch
memory directly. It does not by itself prove every guest-memory-hook path or a
performance improvement. Archived `surface-coherence-self-test.json` receipts
report success in `round2/luigi-remap-oracle`, `texture-decode/oracles` and
`uniform-uploads/final-stories-stage1` under the private profiles directory.
These are historical receipts, not a new run of the current build.

Existing measured gameplay evidence does not identify small writes as the
current residency bottleneck:

| Private profile capture | Measured refreshes after warmup | Small-write surface invalidations | Surface downloads |
| --- | ---: | ---: | ---: |
| `luigi-alias-diagnostic-20261113/bounded_diagnostic` | 900 | 0 | 5,400 |
| `core-p3d-four-title-20261113/stories-comparison/control` | 900 | 0 | 0 |
| `lit-pair/grass-pinned-ab/baseline_a` | 900 | 0 | 0 |

The Stories control raw framebuffer was inspected: it shows the player in the
3D Secret Path forest with a lower-screen map, not a menu or FMV. The older
`grass-pinned-ab` capture includes scripted stick movement. Neither capture
establishes a retention win. No stored `comparison.json` run enabling
`retain_small_writes` was found in this review.

Some older stage captures do contain small-write invalidations. For example,
`core-p3d-four-title-20261113/stories-stage4/frame-timing.json` records 259 across
2,100 measured refreshes, with only 15 downloads and 10.663 ms total download
wall time. That stage's scene has not been independently classified here.
Invalidation counts alone are not a readback cost measurement: clean cached
surfaces can be removed without downloading dirty bytes.

Keep this experiment opt-in. A useful next comparison needs a gameplay
checkpoint with sustained RGBA8 small-write eviction cost, the same binary and
checkpoint, a matched retention-off/on/off run, workload and final-byte checks,
and the existing coherence suite. These captures do not justify widening format
support or changing the default merely to improve residency in theory.

## Mikage public implementation

Primary source was retrieved from the public `mikage-emu/mikage-dev` repository
on 2026-11-13, resolving `main` to commit
`3e1952bf0cab6c02fa82814fde390a42111c4d49`. These observations apply to that
Developer Edition snapshot, not an uninspected product build or all renderers.

- The [resource state contract](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/resource_manager.hpp#L140-L176)
  distinguishes GPU-authoritative, synchronized, staging-authoritative and
  invalid states, and describes read/write hooks protecting guest memory.
  It also records incomplete read behavior, preservation on writes and
  rectangular invalidation. This is a useful ownership vocabulary.
- The [read hook](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/resource_manager.cpp#L102-L134)
  flushes a GPU-authoritative render target when guest memory is read. The
  [flush implementation](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/resource_manager.cpp#L521-L589)
  submits a GPU copy and blocks on a fence before CPU mapping/encoding. It
  explicitly leaves batching to avoid the stall and GPU-side encoding as TODOs.
- The [write hook](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/resource_manager.cpp#L137-L152)
  records TODOs for iterating all overlapping resources, preserving unaffected
  bytes before invalidation and narrowing invalidation to a page. The
  [overlap invalidation code](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/resource_manager.cpp#L756-L767)
  likewise identifies preservation of non-overlapping parts as unfinished.
- [CPUAwaitable](https://github.com/mikage-emu/mikage-dev/blob/3e1952bf0cab6c02fa82814fde390a42111c4d49/source/video_core/src/video_core/vulkan/awaitable.hpp#L9-L47)
  encapsulates fence readiness and waiting. It is a synchronization mechanism,
  not evidence that all consumers avoid blocking.

The transferable idea is explicit ownership plus consumer-triggered
synchronization. This source review supplies no benchmark comparison with Cara
and does not establish that Mikage has completed finer coherence or eliminated
blocking readbacks. Cara's existing interval ownership already addresses part
of that contract; preserve it when testing GPU-resident aliases or small writes.
