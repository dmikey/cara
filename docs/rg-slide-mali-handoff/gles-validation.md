# GLES validation on RG Slide

The reference target is the physical RG Slide with the Mali-G57 r40p0
driver. Benchmarks use the same ROM, renderer, guest clock, and cache
state in both arms. For Dark Moon, collect the title, opening and fog
transition, and early and later Gadd lab windows separately. The accepted
comparison uses Dynarmic, GLES Atlas, 1× resolution, and fixed CLK 100%.

Android Atlas accepts a saved legacy separable-shader preset through the
linked GLSL ES program path. The legacy separable generator emits desktop
extension and interface declarations, and Atlas updates vertex uniforms
on linked programs. The opt-in GLES diagnostics can be enabled separately
with `[Debug] trace_gles_debug = True` and collected using
`adb logcat -v epoch -s CaraGLES CaraPerf`. The renderer uses core debug
output or KHR_debug, depending on the context. It logs API errors and
undefined-behavior messages reported by that context. Leave this option
off for final timings. Device runs cover the paths exercised during that
run.

Preserve guest depth, stencil, sampling and ordering when testing any
performance change. Match draw counts, compare output, and keep p99 and
worst refreshes alongside guest FPS and SPD. The fog transfer trace is
documented in [the phase report](lit-index-followup.md).

## Mali separable-program experiment

With `[Debug] atlas_separable_programs = True`, Android Atlas can link
GLSL ES 3.20 stages individually on the Mali GLES 3.2 route. This uses
matching explicit shader input and output locations and writes packed
vertex-layout uniforms to the vertex program. Pipeline objects update
only the stages that change. The default uses linked programs and its
existing program-binary cache. The separable experiment keeps stages in
memory during a session and does not yet archive them between launches.

Compare both configurations on one build from independent empty Cara
caches with the driver cache state recorded. Collect emulator-thread
compile and link wall time, first draw and visible output alongside
startup latency, p99, and worst refreshes. Verify cold and warm behavior
before considering wider adoption.

## Bounded Mali binary archive experiment

`[Debug] atlas_extended_program_cache = True` selects a separate binary
archive on Android Atlas with a Mali renderer. The experiment can record
up to 256 pairs and 64 MiB. The existing archive defaults remain
128 pairs and 32 MiB. Both modes check shader/driver identity, source and
binary size, and the checksum before submitting saved binaries to GLES.
Use `trace_gl_shader_census` to record the restore count and binary hits.
On the same `c91c256` APK, a cold normal boot populated a 46.97 MiB
archive. The warm restart restored 197 of 197 programs as driver binaries
with zero source links during restore. The title arrived in 21.34 s on
restart, down from 103.00 s cold. There was zero timed compile/link work
in the warm windows below.

| 120-refresh window end | Cold p99 | Cold worst | Warm p99 | Warm worst |
| --- | ---: | ---: | ---: | ---: |
| 2401, lab entry | 167.45 ms | 40.78 s | 76.56 ms | 114.70 ms |
| 2521, lab entry | 88.30 ms | 30.69 s | 45.71 ms | 90.15 ms |
| 4321, late transition | 46.26 ms | 24.03 s | 37.35 ms | 37.46 ms |
| 4441, late lab | 52.00 ms | 246.16 ms | 41.36 ms | 47.85 ms |
| 4561, late lab | 39.99 ms | 40.63 ms | 39.47 ms | 41.26 ms |

These are windows from normal boots aligned by refresh count. Guest draw
counts vary across cold and warm, especially around the transitions.
The warm late-lab window medians were 36.08 and 36.66 ms. This test
establishes that the larger cache can restore more than 128 entries and
that a fully seeded restart avoids the observed compilation bursts. The
128-entry configuration was verified on an earlier build through early
Gadd, so this comparison does not isolate the effect of the larger limit
on the late transition. The larger limit remains opt-in pending a
same-build 128-versus-256 comparison with matched late coverage.
The original app and checked profile files were restored.

[Exact Android archive APK](https://github.com/dmikey/cara/actions/runs/45211463641)

## Linked-program validation on the release APK

Android ARM64 build `7a0d1bc` ran the legacy separable preset using the
default linked-program path. A cold normal boot reached the title in
105.29 s and continued through the opening into Gadd's lab. During this
route the renderer context's GLES debug observer emitted no error or
undefined-behavior messages. The shader census recorded no failed compile
or link status. Syntax checks compiled the 16 generated logic-op fragment
variants for GLSL ES 3.00 and 3.20. The old CopyInverted expression was
rejected by the compiler. A host fixture also checked that a rejected
shader with no driver log is deleted and that failed creation cannot
reach source upload.

A restart seeded from the exact APK restored 128 program binaries and
reached the title in 21.51 s. Warm Gadd medians were 28.87–29.62 ms,
p99 was 33.86–50.90 ms across four windows, and the worst refresh was
53.46 ms. The lab HUD showed 27 game FPS at 55% SPD, CLK 100%.
An attempted restart with a seed from an earlier shader signature was
discarded from the comparison. The original installed APK and profile
were restored and the checked files matched.

## Emulation versus graphics

An Android presentation context already handles the window swap. It does
not own PICA draw preparation. A draw-worker probe needs owned draw state
and inputs, in-order submission, and measured joins for guest-visible
reads, fills, transfers and save/restore. The existing Mac command queue
is a reference for API-level ordering. Its GL context ownership and
global entrypoint hooks need independent validation with Android EGL
presentation and the shader worker.

The current memory layer can intercept writes to rasterizer-cached VRAM
and linear-heap mappings. Ordinary Dynarmic writes can use direct
page-table pointers, and GPU copies and fills use physical pointers.
Index-cache generation reuse needs a proof for every write path and alias
covered by the optimization. Start with a narrow watched range and log
write epochs alongside live comparisons before allowing a compare to
be skipped. Keep GPU work and PICA timing counters separate from
emulation-thread CPU and graphics worker CPU.

### First Mali stage measurements

APK `a036217` passed Android ARM64 CI and reached the title in 75.65 s and
the matched lab checkpoint in 170.44 s from a normal boot. Both used an
empty Cara cache and the existing driver cache. The linked-program
diagnostic APK `7a0d1bc` reached those checkpoints in 105.29 s and
332.45 s in its separate cold run. The input harness held the title for
24 s in both runs. The first measured lab-entry burst dropped from
41.63 s to 8.26 s. These are single cold controls on different builds,
with the separate-stage configuration opt-in.

The shader census counted 22 standalone vertex links costing 42.78 s by
the title and 37 standalone fragment links costing 3.06 s. Shader stages
linked successfully and the renderer debug observer reported no API
errors. This identifies vertex-stage link as the larger remaining
startup cost on this route. Four later lab refresh medians were
29.26–29.70 ms, p99 ranged from 34.31 to 52.34 ms and the worst refresh
was 70.63 ms. The HUD showed 27 FPS at 55% SPD. Device pixel/depth
equivalence and durable stage caching remain open.

[Exact Android stage APK](https://github.com/dmikey/cara/actions/runs/45210827729)

## Apple GLES paths on the Slide

Android Atlas already uses the iPad's packed vertex inputs, prepared
indexed input cache, 80 MiB per-bank upload limit, byte-validated resident
upload reuse, surface remap and checked depth reinterpretation. The
native-depth and surface-remap repairs removed the measured GPU-to-CPU
downloads from the relevant Slide traces. Warm Gadd measurements show
zero timed downloads. Removing more CPU work there needs a different
target.

Android EGL supplies a default for VS uniform reuse, and the Atlas JNI
setting `atlas_reuse_vs_uniforms` overrides it to false. iPad enables the
same path by default. Isolated reuse reduced matched M4 iPad Stories
refresh times from about 27.4 ms to 25.1–25.3 ms. The Slide Dark Moon
comparison used the same `7a0d1bc` APK and exact warm seed in both arms, at
JIT + GLES Atlas, 1×, Performance Mode and CLK 100%.

| Warm Gadd, four 120-refresh windows | Default | VS uniform reuse |
| --- | ---: | ---: |
| Mean uniform region per refresh | 1.290 ms | 0.905 ms |
| Mean GPU-command CPU category per refresh | 19.337 ms | 18.507 ms |
| Window median range | 28.30–29.65 ms | 28.88–29.51 ms |
| Window p99 range | 34.61–49.87 ms | 35.66–49.38 ms |
| Worst refresh | 50.95 ms | 69.51 ms |

Title time was 21.01 s with the default and 21.13 s with reuse. The
measured title uniform region also dropped, from 1.293 to 0.884 ms per
refresh. The lab arms processed 169,266 and 168,916 draws respectively.
The instrumented uniform region improved in both scenes. This single
pair gives no reason to change the Android default on frame time alone.
The original APK and profile were restored and the checked files matched.

The quaternion-only reference slice is present in the shared GLSL
generator, and the current Android settings do not expose it. Earlier
device tests showed a small isolated effect. Used-input pruning reduced
logical register counts on iPad while leaving staged bytes and CPU time
similar. Apple raster-state reuse is also opt-in on Android. The earlier
combined raster and uniform reuse probe did not establish a lab gain.

The title and fog traces give a way to test destination-resource
lifetime separately. The title loses 17–18 ms per refresh
inside the LUT upload call, while the lab-entry burst loses 4.49–7.05 ms
and warm lab LUT traffic drops to zero. An isolated fog destination
rotation should retain the existing lighting path and measure stalls
around the exact same guest writes. See [phase results](lit-index-followup.md#lighting-and-fog-transfer-result).
