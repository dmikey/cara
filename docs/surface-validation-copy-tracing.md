# Successful surface-validation copy tracing

`CARA_SURFACE_VALIDATION_COPY_TRACE=1` enables a default-off diagnostic at
`ValidateSurface` → `CopySurface`. It records returned fill/blit copies, separately
from `CARA_SURFACE_UPLOAD_TRACE`, which records guest-RAM fallback uploads.
The common `FrameTiming::counters.enabled` gate may remain **off**. No guest bytes
or image payloads are read, hashed or stored, and validation/copy order is unchanged.
This diagnostic is not an optimization or a measurement of GPU completion.

For an already configured native developer build and fresh task-owned user folder:

```sh
CARA_USER_DIR=/absolute/path/to/test-user \
CARA_SURFACE_VALIDATION_COPY_TRACE=1 \
CARA_SURFACE_VALIDATION_COPY_TRACE_START_FRAME=300 \
CARA_SURFACE_VALIDATION_COPY_TRACE_END_FRAME=1200 \
  /absolute/path/to/build/bin/cara /absolute/path/to/game.3ds
```

The cache destructor writes `surface-validation-copies.json` beside other cache
reports. `enabled:true` and `written_frame` distinguish an actual report from a
missing one. State restoration can destroy an earlier cache and write an empty
report: require fresh output from the expected process and `written_frame` at or
beyond the intended endpoint, even when `totals.calls` is zero. A changed engine
requires a genuinely compatible checkpoint; do not override fingerprint checks.

The window uses original `VideoCore::GetCurrentFrame()` numbers, inclusive start
and exclusive end (defaults 300–1200). That counter advances in `FrameUpdate`
independently of common timing. `actual_min_frame`/`actual_max_frame` are null for
an empty capture. They are observed source frames, not a claimed window length;
initialization/restoration and separate cache lifetimes can change the range.

Collection has fixed limits: 512 chronological details, 256 weak lifetime identity
slots, and 64 layout aggregate buckets. Exact whole-window call/guest-byte totals
continue after details or buckets fill. `detail_overflow` and `bucket_overflow`
independently report omitted calls/bytes/timing; never add those totals together.
The sum of detail records plus detail overflow reconciles with `totals`; bucket
sums plus bucket overflow do likewise. Integer saturation is explicitly counted.
Buckets group source/destination format, type, tiling, width, height, stride, scale,
custom-texture status and fill/blit, rather than texture handles or addresses.

Each detail contains validation invocation and loop-fragment ordinals, original
request, current invalid fragment and actual copied half-open byte intervals,
guest-byte count, scaled source/destination rectangles `[left,top,right,bottom]`,
and both surface layouts, registration status, GL handles and weak allocation IDs.
The source rectangle is null for a fill. Source validity is observed before copying.
Allocation/invalidation identity tracking runs before the exclusive end frame,
including warmup before the start. Expired identity slots can be reused with new
IDs. After the end, allocation/validation/invalidation call counters can still
advance, but allocation/invalidation hooks perform no weak-table scans; skipped
allocations are reported in `post_window_allocation_events`. It never stores a strong surface reference.
Unknown IDs are null, with explicit untracked/unknown totals. Allocation ID does
not identify the provenance of the pixels or prove GPU ownership.

Last observed invalidation metadata records a top-level `InvalidateRegion` serial,
frame, touched interval and CPU/GPU classification. It tracks insertion into invalid
regions, not every mutation/removal path. Several interval-map visits can share one
serial. No observed event is represented by a null serial and reason 0; this is
**unknown/none observed**, not proof of no invalidation. The last observed event
is not a true causal attribution or a complete dirty interval set.

Every in-window returned copy is clocked, including calls whose details overflow.
Wall and current-thread CPU nanoseconds measure the synchronous `CopySurface`
call only, including driver enqueue/wait behavior. They exclude trace metadata
collection and aggregation; the observer itself has overhead. `wall_samples`,
`cpu_samples` and failure counters specify coverage. Unavailable/failed CPU clocks
produce null detail values and add no CPU duration; zero total CPU time with failed
samples is not proof of no CPU work. Calls/bytes are exact unless explicitly
saturated; timings are valid-clock sums, not instruction duration or GPU timestamps.
Counts and repeated layouts alone do not prove redundant copies or CPU savings.

Focused ROM-free helper checks (window boundaries, finite slots, overflow totals,
clock failures and saturation) can be run independently:

```sh
clang++ -std=c++17 -fsanitize=address,undefined -Iemulator/src \
  tests/validation_copy_profile_test.cpp -o /absolute/path/to/test-output
/absolute/path/to/test-output
```

The maintained `tests/test_validation_copy_trace.py` fixture also compiles the
actual collector body with synthetic surface dependencies under ASan/UBSan,
checking weak lifetimes, identity/detail/bucket caps, clock failures and JSON
reconciliation. It needs `clang++` and uses the normal temporary-directory setting.

The offline validator ranks complete-window layout aggregates and rejects totals
that do not reconcile, saturated counters, duplicate aggregate keys, malformed
intervals and unsupported schemas:

```sh
python3 tools/summarize_validation_copies.py /absolute/path/to/surface-validation-copies.json \
  --rank time --limit 10
```

`--rank cpu`, `--rank calls` and `--rank bytes` select alternative orderings.
Overflow remains explicit and chronological detail reasons are labelled as a
biased prefix. The tool does not infer FPS, normalize by observed frame bounds,
or equate logical copied bytes with memory-bus traffic. The caller must still
verify a fresh file from the intended process/cache lifetime and endpoint.
