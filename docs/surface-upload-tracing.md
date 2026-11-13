# Bounded surface upload tracing

`CARA_SURFACE_UPLOAD_TRACE=1` enables an internal, read-only metadata diagnostic
for the actual `ValidateSurface` guest-RAM fallback: `LoadGLBuffer`, followed by
`UploadGLTexture`. It includes framebuffer/native-color consumers outside the
texture-acquisition profiling context. It does not change validation, ownership,
copy/remap selection or guest timing policy. The default is off; the off path
adds pointer checks without trace clocks, scans or allocations.

## Running a capture

Use the same installed binary, checkpoint, replay, guest clock, sound, renderer
flags and pacing as the verified control. Add `surface_upload_trace: true` to its
benchmark configuration. Include `texture_acquisition_profile: true` if the
`inside_texture_acquisition_context` flag is needed: that flag reflects the
existing profiling context and is false everywhere when that profiler is off.
`render_bookkeeping_profile: true` supplies aggregate consumer timings separately.
A generic diagnostic invocation is:

```sh
python3 tools/benchmark_gpu_inputs.py \
  --device DEVICE_ID --rom OWN_ROM_FILENAME \
  --output PRIVATE_PROFILE_DIRECTORY --frames 1200 --warmup 300 --paced \
  --config '{"name":"uploads","packed":true,"bank_mib":80,"surface_upload_trace":true,"texture_acquisition_profile":true,"render_bookkeeping_profile":true}'
```

The minimal example is not a substitute for copying the control's full flag
configuration. The harness sets these environment variables:

| Control | Meaning |
| --- | --- |
| `CARA_SURFACE_UPLOAD_TRACE` | Exact value `1` enables tracing. |
| `CARA_SURFACE_UPLOAD_TRACE_START_FRAME` | Inclusive start; default 300. Harness uses `--warmup`. |
| `CARA_SURFACE_UPLOAD_TRACE_END_FRAME` | Exclusive end; default 1200. Harness uses `--frames`. |

Identities and observed invalidations are tracked before and after the recording
window while tracing is enabled. Frame values come from
`VideoCore::GetCurrentFrame`, not implicit benchmark refresh numbers. Save/load
or a restored renderer can change their relationship. An empty or reversed
window records no uploads; actual frame bounds are then JSON `null`.

The cache destructor writes `surface-uploads.json` beside `surface-flushes.json`.
The harness archives it as `NAME-surface-uploads.json`. Restoring a checkpoint
can destroy an early cache and write a valid but incomplete/empty report. Require
`enabled: true`, the expected `kind`, `read_only_diagnostic: true`, and
`written_frame >= requested final frame` before accepting the completed report.
That frame check is necessary freshness evidence, not a unique run identifier;
retain the matching benchmark report, binary and configuration too.

## Fields and bounds

There are at most 256 live identity slots and 256 aggregate entries. Expired weak
identities can release slots; a new tracked surface lifetime receives a new
monotonic `allocation_generation`. This identifies a cache surface allocation,
not an independently tracked GL storage reallocation. Generation zero means
identity unavailable. `untracked_surface_allocations_all_frames` and
`unknown_identity_calls` expose identity saturation; unknown records must not be
used to prove that two uses belong to the same allocation.

Aggregation keys include allocation identity, original frame/60 group, layout,
requested load interval, destination rectangle, first-fallback flag, texture
handle, custom-texture/context flags and observed invalidation metadata. The
first 256 distinct keys are retained. Every further unmatched event contributes
to `overflow_totals`, with no fabricated representative allocation. Existing
keys continue accumulating. For calls and byte totals, the sum of all entries
plus overflow must equal top-level `totals`. This cap can limit attribution in a
busy route even though total work remains represented.

| Field | Meaning |
| --- | --- |
| `window_start_inclusive`, `window_end_exclusive` | Requested recording bounds in the original frame clock. |
| `actual_min_frame`, `actual_max_frame` | Observed upload frame bounds, inclusive, or null. |
| `written_frame` | Original frame clock at destructor serialization; inspect freshness separately from upload bounds. |
| `surface_address`, `surface_end_exclusive`, `surface_size` | Allocation address metadata, without guest memory contents. |
| `format`, `surface_type`, `width`, `height`, `stride`, `tiled`, `scale` | Native `SurfaceParams` values. Format/type are numeric enums, not inferred from a filename. |
| `allocation_generation`, `created_frame`, `texture_handle` | Surface lifetime identity and observed creation metadata. GL names alone can be reused. |
| `first_fallback_for_allocation` | First observed RAM fallback in that tracked lifetime, including observation outside the output window. This is not first use, first render or proof of a redundant upload. |
| `load_start`, `load_end_exclusive` | Arguments requested from `LoadGLBuffer`; its existing internal VRAM clipping/null-source behavior remains unchanged. |
| `destination_rect` | Surface rectangle requested from `UploadGLTexture`, before its existing custom-texture/scaling handling. |
| `frame_group_start`, `first_frame`, `last_frame`, `distinct_original_frames` | Per-entry frame metadata; distinct frames are counted within the original 60-frame group. A frame reset/revisit is not a new unique frame number. |
| `inside_texture_acquisition_context` | Existing context active at recording; requires texture acquisition profiling. False does not identify a particular framebuffer caller. |
| `requested_guest_bytes` | Sum of requested load interval sizes, not unique bytes or proof of a completed RAM load. |
| `gl_upload_payload_bytes` | Logical upload payload bytes, accounting for custom replacement dimensions; excludes row padding, GPU copies and driver-internal traffic. |
| `load_seconds`, `upload_seconds` | Host wall durations around the two calls. No GPU timestamps, GPU completion or on-CPU attribution. |

`last_observed_invalidation` uses the exported label table: unknown, initial
allocation, CPU write, other GPU owner or mixed. Multiple observed events since
the previous fallback become mixed; their envelope can contain gaps.
`observed_invalidations_since_previous_fallback`, `last_invalidation_frame` and
the interval envelope provide context. They are **not proof of the fallback's
cause**: a partially invalid allocation can contain older outstanding regions,
and a first fallback may follow additional writes. The trace observes creation
and the existing interval-invalidation path; it does not reconstruct every
memory producer or silently assume that every CPU write changed its bytes.

No game assets, images or RAM payloads are written. Calls do not establish GL
success or completion; errors retain their existing renderer behavior. Diagnostic
clocks, fixed-table scans and serialization perturb execution, so compare
performance using separate matched runs with tracing disabled. A successful
aggregation/syntax fixture is not device validation of a new diagnostic build.
