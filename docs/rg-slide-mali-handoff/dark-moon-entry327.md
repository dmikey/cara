# Reducing indexed geometry work in Dark Moon

Measurements used the physical RG Slide with a Mali-G57, Dark Moon USA, normal boot, Dynarmic JIT, ordered GLES at 1×, Performance Mode and fixed CLK 100%. Fog and lighting texture generations, async mailbox publication, and checked remap were held constant. The timing rows use compile-free lab windows ending at refreshes 3121, 3241, and 3361, and separate uninstrumented GPU sampling during the held lab scene.

## Native strip and fan test

The existing native element option gives GLES the original index stream when the packed unlit path can use a strip or fan. The app recorded zero eligible draws after enabling it. The stable position-shading request count confirms that this option removed no work in the tested scene. The alternate boot bracketed by controls had similar frame times despite the lack of activation. [Same APK](https://github.com/dmikey/cara/actions/runs/45277898425)

| Run | Median range, ms | p99 range, ms | Worst frame | Position requests per 1,000 primitives |
| --- | ---: | ---: | ---: | ---: |
| First control | 29.89–30.77 | 44.48–45.43 | 88.78 ms @ 3004 | 538.597 |
| Native elements enabled | 29.77–30.31 | 42.12–43.18 | 84.65 ms @ 3071 | 538.604 |
| Repeat control | 30.12–30.61 | 37.23–44.20 | 84.70 ms @ 3148 | 538.519 |

A separate trace with native elements enabled still attributed 3.814 ms per refresh of producer overlap to unlit `glDrawElements` at entry 327, including 3.811 ms while the remap result was pending. An entry offset identifies a vertex program entry point and can be shared by multiple programs. The counters report the scene's GPU workload, including other draws and compositor work. [Frame windows](dark-moon-entry327-windows.tsv) · [Hardware counters](dark-moon-entry327-counters.tsv) · [Queue trace](dark-moon-entry327-native-backlog.txt) · [Arm G57 counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## Packed float decode in the vertex shader

The optional packed-fetch decoder now reinterprets four aligned float words as a vector. Four fixed swizzles cover all aligned starting word offsets, and the existing scalar path handles unaligned floats and integers. Missing components keep the existing zero and one defaults. The Mali-G57 EGL fixture matched 2,160 vectors against their original 32-bit input words across component counts, strides, a sixteen-byte texel boundary, and the end of a texture row. [Device fixture](https://github.com/dmikey/cara/actions/runs/45281742820)

The ARM64 game APK used separate on-device archives for the ordinary decoder and the optional packed-fetch decoder with the float fast path. The first run in each configuration generated its archive. The measured control, candidate and repeat control all restored 128 binaries with zero source links and had no measured compilation in the comparison windows. The candidate enables texel reuse per attribute as well as the aligned-float fast path, so this comparison measures the combined decoder. [Exact game build](https://github.com/dmikey/cara/actions/runs/45281631205)

| Warm Gadd run | Median range, ms | p99 range, ms | Worst frame | Shader-core cycles per primitive |
| --- | ---: | ---: | ---: | ---: |
| First control | 30.14–30.76 | 39.63–44.96 | 68.81 ms @ 3088 | 821.83 |
| Packed-fetch decoder and float fast path | 28.07–28.85 | 35.37–41.88 | 85.13 ms @ 3022 and 3129 | 770.56 |
| Repeat control | 30.08–30.76 | 37.00–43.88 | 85.36 ms @ 3166 | 826.04 |

The candidate used 6.2–6.7% fewer shader-core cycles per primitive with nearly the same number of position requests. The position-shading stall fraction fell from 34.44–34.60% of GPU-active cycles to 32.08%. The measured warm median improved by about 2 ms. The worst frame remained comparable to the worst repeated control. The same matched title windows improved from approximately 37 ms to 35 ms at the median. [Full warm windows](dark-moon-entry327-vector-windows.tsv) · [Hardware counters](dark-moon-entry327-counters.tsv)

## Duplicate opaque draws

A separate build used the existing PICA packet and GPU-resource independence proof to test skipping a redundant second host draw. It required a real writable depth attachment, strict LESS or GREATER depth testing, and disabled blending and stencil. Guest packets and timing still ran in order. The eligibility check saw 13,104 pairs in the three matched lab windows and elided zero. Shader-core cycles per primitive were 818.06 in the trial and 817.13 in the first control. The zero-coverage switch was removed. [Trial APK](https://github.com/dmikey/cara/actions/runs/45283922019) · [Per-window measurements](dark-moon-entry327-elision-windows.tsv)

## Entry 327 after the decoder change

Two separately instrumented normal boots on the same APK compared the warm windows ending at 2641, 2761 and 2881. The raw-decoder boot had the duplicate-depth trial enabled and elided zero draws. The optimized-decoder boot used its warm archive and ran the ordinary draw path with the packed-fetch decoder and aligned-float fast path. Both restored 128 binaries. The traced producer waited **4.025 to 3.245 ms per refresh** behind entry 327 indexed draws before the remap result. Total pre-call wait moved from 9.650 to 8.507 ms per refresh. These overlap figures locate the part of the producer's critical path that improved. The hardware counters above separately measure scene-wide GPU work. [Matched backlog traces](dark-moon-entry327-vector-backlog.txt)

The optimized decoder remains controlled by `gpu_packed_fetch_reuse` for Android. Frame times still exceed the 16.7 ms budget. The Slide's original signed APK, configuration, and shader archive were restored after all three test sets.

## Direct GLES retest with exact table residency

The physical RG Slide booted Dark Moon from an empty save slot in each
profile on preview APK `ec7c1df`. Both arms used Dynarmic JIT, the GLES
atlas at 1×, threaded presentation, fixed guest CLK 100%, and disabled
Adaptive Frame Skip. Exact lighting and fog table residency stayed
enabled. The candidate enabled `atlas_indexed_draws`. This tests the direct
submission path separately from the earlier ordered-worker experiments.
[Android preview build](https://github.com/dmikey/cara/actions/runs/45458212650)

Five 120-refresh Gadd windows in each arm had zero reported foreground
compile time, with 41,880–46,993 guest draws in the arrays arm and
42,229–44,635 with indexed draws. The indexed arm was still switching
from its exact ordinary draw fallback to ready indexed shaders in the
first qualifying window.

| Warm Gadd | p50 range | Window p99 range | Worst refresh | Wall time per 120 refreshes |
| --- | ---: | ---: | ---: | ---: |
| Arrays, five windows | 31.97–32.51 ms | 39.07–60.87 ms | 78.34 ms at 2330 | 3.906–4.040 s |
| Indexed, five windows | 28.79–31.55 ms | 34.27–59.00 ms | 62.65 ms at 2433 | 3.513–3.838 s |

In the exact 42,229-draw match, arrays took 3.911 and 3.933 s per
120 refreshes. Indexed took 3.513 s with 14,399 indexed draws and zero
lit-indexed draws. Throughput rose from 30.51–30.68 to 34.16 refreshes/s.
The indexed window had 28.93 ms p50, 34.27 ms p99, and a 51.94 ms maximum.
The arrays windows had 31.97 and 32.22 ms p50, 40.42 and 39.07 ms p99,
and maxima of 78.34 and 63.72 ms. All three had zero lighting or fog uploads.

A separate seven-second interval in each running arm sampled the
Mali-G57 counters after the first compile-free Gadd window. The device
reported thermal status 0 at startup. The counters describe whole-device
graphics activity during the lab scene.

| Hardware metric | Arrays | Indexed |
| --- | ---: | ---: |
| Position requests per 1,000 primitives | 750.94 | 538.53 |
| Varying requests per 1,000 primitives | 90.08 | 81.49 |
| Tiler cycles per primitive | 184.35 | 127.78 |
| Shader-core cycles per primitive | 906.10 | 793.02 |
| Position shading stalls as share of GPU-active cycles | 56.42% | 34.20% |

The arrays first boot compiled the title and lab shaders. The indexed
boot restored 120 program binaries and linked additional optional
indexed variants on the EGL worker as they became visible. Some
background links overlapped the sampled gameplay. The cold shader
stalls and per-window frame tails need a cold indexed arm and a repeated
warm control before choosing a default. Both screenshots showed the lit
lab, E. Gadd and translucent ghosts at different animation ticks. This
was a visual check. Exact same-refresh pixel, depth and gameplay
validation remains open. The benchmark preview was uninstalled and
the original player configuration checksum matched.

## Cold archive and warm replay matrix

The same physical RG Slide and `ec7c1df` debug APK ran four normal
boots in order. Arrays and indexed each began with an independently
empty Cara shader archive. The next arrays and indexed boots each
restored the archive produced by their corresponding cold boot.
Every boot used an empty preview save, the GLES atlas at 1×, Dynarmic
JIT, threaded presentation, fixed CLK 100%, exact LUT residency, and
disabled Adaptive Frame Skip. The stock driver cache was left in its
existing state throughout. The cold comparison measures first use
with empty Cara archives under that driver-cache history.

The cold title window ending at refresh 601 had exactly 25,561 guest
draws in each arm. Arrays recorded 30.870 s p99 and a 47.223 s worst
refresh. Indexed recorded 30.293 s p99 and a 48.665 s worst refresh.
Both worst points were refresh 515. Foreground compile and link
scopes totaled 78.687 s for arrays and 79.223 s for indexed across
those 120 refreshes. The title still exhibited a first-use pause on
both paths.

The heaviest lab-entry refresh fell in the window ending at 2161 in both
cold boots. Arrays peaked at 39.791 s (refresh 2088) and indexed at
40.781 s (2064). Foreground compile/link totaled 39.630 s and 40.578 s,
respectively. That window had 33,104 vs 37,566 draws. It identifies the
same transition in both boots with different draw streams. The next lab
window still peaked at 29.748 s for arrays (2268) and 30.275 s for indexed
(2243). Both were first-use link stalls despite the indexed path’s
regular-draw fallback for optional program variants.

After exact shaders were ready, a 42,927-draw Gadd window was available
in each cold run. The indexed path sent 14,637 direct indexed draws
and recorded zero indexed fallbacks. No shader worker compile/link scope
intersected either sample.

| Cold boot, qualified 42,927-draw Gadd window | Window end | p50 (ms) | p99 (ms) | Worst (ms) | Elapsed (s / 120) | Submit scope (ms / 120) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Arrays | 2401 | 31.97 | 55.95 | 179.98 | 4.074 | 521.14 |
| Indexed | 2521 | 28.87 | 52.35 | 53.12 | 3.542 | 475.15 |

The draw count controls workload size. The windows end at different
refreshes, so the table cannot establish identical guest state or
repeatability of the largest isolated refresh.

The paired emulation-thread GPU-command category spent 22.70 ms per
refresh for arrays and 20.78 ms for indexed. That counter measures CPU
time charged inside the category, including GLES calls made there. The
GLES submit scope in the table measures wall time.

The warm arrays boot restored 120 of 120 binaries (30,224,497-byte
archive). Indexed restored 128 of 128 (29,948,646 bytes). Both reported
zero source links during restore. The exact 25,561-draw title window
ending at refresh 601 recorded 130.84 ms p99 and 369.03 ms worst for
arrays, and 131.19 ms p99 and 411.26 ms worst for indexed, both at
refresh 515. Both recorded zero timed compile/link work in that window.
The warm title captures had identical PNG SHA-256 digests. At a later
transition beyond refresh 3241, arrays still needed 13 foreground links
that totaled 27.88 s before capture ended. The warm archive can avoid
sampled first-use bursts while still missing later shaders.

The early warm lab window ending at 2281 completed 42,229 draws in
both runs. Neither ran a shader compile or link. The LUT scope still
took 672.96 ms across that window on arrays and 594.12 ms on indexed.

| Warm replay, matched Gadd draws | Draws | Path | p50 (ms) | p99 (ms) | Worst (ms) | Elapsed (s / 120) |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Refresh 2281, entry | 42,229 | Arrays | 36.79 | 45.25 | 88.30 | 4.319 |
| Refresh 2281, entry | 42,229 | Indexed | 35.31 | 42.70 | 78.93 | 4.043 |
| Refresh 2641, settled | 42,577 | Arrays | 32.73 | 53.38 | 56.89 | 4.004 |
| Refresh 2641, settled | 42,577 | Indexed | 29.44 | 52.24 | 52.32 | 3.597 |
| Refresh 2761, settled | 42,578 | Arrays | 32.60 | 55.03 | 55.33 | 4.004 |
| Refresh 2761, settled | 42,578 | Indexed | 29.20 | 53.50 | 56.98 | 3.590 |

The two settled indexed windows had 14,517 and 14,518 direct indexed
draws with zero shader fallbacks or overlapping background compile/link.
There were zero draws on the experimental lit-pair indexed route.
Elapsed time improved about 10% in both pairs. The GPU-command CPU
category fell from 19.99 to 19.85 ms per refresh at 2641 and from
20.03 to 19.92 ms at 2761, so CPU savings measured there explain
little of the elapsed improvement. These are refresh timings.
The prior G57 hardware counter comparison measured GPU-side vertex and
tiler savings separately.

Across the first five qualified warm windows per arm, arrays recorded
31.85–32.95 ms p50, 53.20–73.39 ms p99 and a 74.49 ms worst refresh.
Indexed recorded 28.94–29.77 ms p50, 36.80–53.50 ms p99 and a 71.67 ms
worst refresh. These ranges describe the individual windows. The five
windows in each arm include different camera and animation states.
The selection excludes windows with foreground linking, worker
compile/link overlap, fallback draws, or over 100 ms in the LUT scope.

A later indexed archive miss produced a 37.198 s refresh (2819), of
which 37.107 s was timed shader work across the surrounding 120-refresh
window. The p99 for that window was 52.12 ms. Arrays hit a later
foreground link burst as well, after its last completed 120-refresh
window. It lacked a corresponding worst-refresh measurement for that
burst. Both arms can still encounter a late shader freeze with a warm
archive. These measurements support retaining indexed draws as a
steady-state candidate and pursuing program linking independently.

All four lab captures showed translucent ghosts and the lit room.
They were captured at different animation ticks. A same-refresh
comparison of color, depth and subsequent guest reads remains open.
The experiment changed no renderer defaults. The preview package was
removed, the original player reopened, and its configuration SHA-256
matched the pretest digest.

The four raw captures and screenshots are in
`/tmp/cara-gcloud.8yZRID/mali-matrix-*` on the profiling host. The arrays
warm log recovered from the Android ring buffer is
`mali-matrix-array-warm-recovered.log`.
[Exact Android benchmark build](https://github.com/dmikey/cara/actions/runs/45458212650)

## Cold lab program-link attribution

The shader census correlates each driver link with the original vertex
and fragment source hashes on the same GL thread. The window ending at
refresh 2161 contains the first lab door transition in both linked-program
controls. Link scopes cover `glLinkProgram` and the blocking
`glGetProgramiv(GL_LINK_STATUS)` query. First-draw scopes cover CPU
submission. They do not measure GPU completion.

| Cold lab window ending at 2161 | Arrays | Indexed |
| --- | ---: | ---: |
| Worst refresh | 39.791 s | 40.781 s |
| Foreground links | 21 | 21 |
| Foreground link wall time | 38.202 s | 39.237 s |
| Linked combinations with known lit fragments | 16 | 16 |
| Foreground link wall time for those lit combinations | 32.828 s | 33.150 s |
| Foreground stage compile wall time | 1.424 s | 1.338 s |
| Longest timed foreground first-draw submission | 0.245 ms | 0.235 ms |

The most expensive individual link in both runs paired vertex source
`043c304f6262673f` with lit fragment `bd8666435f474b43`. It took 3.075 s
with arrays and 3.091 s with indexed draws. Four shared vertex sources
(`4d30a54875fec2a2`, `2808e2f12aa3a68f`, `043c304f6262673f`, and
`a8e2ab8b7c723753`) account for 28.237 s of indexed link time. They
already reuse GL shader objects. The cost repeats when Mali links each
vertex and fragment pairing. Five indexed worker links ran in this
window and used unlit variants. The worker left the dominant lit links
on the emulation thread. In Performance Mode, the existing fallback
requires a prelinked software-vertex program for that exact fragment.

The next lab burst, ending at 2281, repeated the pattern. Sixteen
foreground links took 29.546 s for arrays and 30.157 s for indexed.
Indexed foreground stage compile work fell to 1.765 ms in that window.
A larger binary archive can cover later repeat visits. A first visit
requires reducing or deferring the linker work itself.

### Separable GLES stage probe

The next run used the same `ec7c1df` APK and settings as the indexed
linked-program control, with GLES 3.2 separable stages and a GLES
debug observer enabled on the preview. It began with an empty Cara
archive and empty preview save on the stock driver. The driver-private
cache was retained. The observer reported no GLES warnings or errors.

| Cold checkpoint | Indexed linked p99 | Indexed linked worst | Separable p99 | Separable worst |
| --- | ---: | ---: | ---: | ---: |
| Smoky title, end 601 | 30,292.77 ms | 48,664.67 ms | 10,576.50 ms | 42,338.69 ms |
| First lab entry, end 2161 | 155.77 ms | 40,780.70 ms | 94.23 ms | 8,346.56 ms |
| Next lab window, end 2281 | 95.54 ms | 30,275.45 ms | 95.39 ms | 787.66 ms |
| Later lab transition, end 2881 | 52.34 ms | 34,440.95 ms | 53.57 ms | 1,126.82 ms |
| Settled lab, end 2521 | 52.35 ms | 53.12 ms | 53.15 ms | 53.85 ms |

The title window had 25,561 guest draws in every arm. At lab entry
the animations produced different draw counts. The settled window
had 42,927 draws, 14,637 direct indexed draws and zero fallback
draws in both arms. Its median was 28.87 ms with linked programs
and 29.33 ms with separate stages. The GPU-command CPU category
spent 20.78 and 20.87 ms per refresh respectively, while Dynarmic
execution accounted for 5.36 and 5.68 ms. The observed cold link
improvement did not carry over to a faster settled draw loop.

The remaining first lab stall comes primarily from three standalone
vertex links totaling 6.404 s. Fourteen separate fragment links
totaled 0.746 s. Stage compile calls totaled another 1.051 s.
At the smoky title the 20 standalone vertex links still took
41.807 s. All 36 fragment links there totaled 3.019 s. Reducing
the cost of the first vertex lowering/link is the remaining cold
priority on this path.

The second separable normal boot reused the installation and stock
driver cache. It used an empty preview save again. Both separable
runs kept the Cara archive directory empty. The title recorded
9,615.69 ms p99 and a 41,800.73 ms worst refresh on the repeat.
The first lab-transition refresh took 7,819.32 ms with a 46.64 ms
window p99. That window ended at 2041 on this run. The next lab
window ending at 2281 recorded 73.00 ms p99 and 530.38 ms worst.
The driver reused too little stage compilation work to remove
the large first-use waits on a repeat boot.

Three of the four expensive vertex hashes from the linked control
match sources in the earlier captured Dark Moon Cara archive.
The sources are 75,804–75,833 bytes and emit the same opcode and
first generated statement for all 500 captured guest instruction
PCs. Their vertex program entry offsets are 198, 218 and 230.
Each generated `exec_shader` calls its literal `sub_<entry>_4096`
and initializes `jmp_to` to that entry. The decompiler also cuts
different dispatch blocks at the entry. The PC 198 variant loads
packed input register 5 where the PC 218 variant zeroes it.
Changing only a uniform would select neither the missing branch
labels nor that input. The common guest program is a candidate for sharing one
compiled stage across proven compatible entry points. An exact
implementation must validate the guest program and swizzle bytes,
input/output interfaces, and entry-dependent per-draw state before
sharing a compiled stage.

A bounded separable-stage binary archive is an independent repeat
boot improvement. It can reuse the existing driver identity,
checksums and limits. Loading a GLES binary includes the program's
separable state under the ES 3.2 specification. A driver can still
reject a binary, so any implementation needs to retry the exact
generated source. The existing extended linked archive has already
restored 197 of 197 binaries on the Slide. That is evidence for warm
coverage on that path. Separable archive restore and stable
same-refresh color and depth have yet to be validated.
[OpenGL ES 3.2 specification](https://registry.khronos.org/OpenGL/specs/es/3.2/es_spec_3.2.pdf) ·
[Linked archive measurements](gles-validation.md#bounded-mali-binary-archive-experiment)

Both separable runs reached the lit lab and showed translucent
ghosts and lightning. The screenshots were captured at different
animation ticks. The experiment changed no shipped defaults.
The preview was removed and the original app configuration
hash matched its pretest value. The raw captures are
`/tmp/cara-gcloud.8yZRID/mali-link-separable-*.log`.

### One guest vertex call site

The generated expanded-triangle path calls the same translated PICA
vertex program once for the triangle's reference corner and once for
the actual corner. Corner zero uses only the actual call. Mali can
spend more time compiling and linking two call sites for the same
large program. The opt-in `atlas_single_vertex_call` probe for the
GLES 3.2 separable path emits one call site in a loop. The first
iteration reads the reference only when needed, and the second exits
after evaluating the actual corner. Both use the original shader
inputs, clear the same temporary state, preserve the reference
quaternion correction, and have the same sequence of evaluations.
The probe leaves direct indexed specializations, pair inputs and
the existing quaternion-slice path on their existing generators.

The first EGL pbuffer replay used five archived Dark Moon vertex
sources. Four exact hashes entered the measured cold title window.
A fifth related source used the same 500-instruction-PC family.
Both arms had explicit ES 3.2 output locations. The fixture linked
each vertex shader as a standalone stage and requested a binary
retrieval hint, matching the Cara path. Every pair linked
successfully on the stock RG Slide driver. The test alternated the
arms and then ran them in reverse order in a fresh process. The
driver's private blob cache could not be disabled from this fixture.

| Five standalone vertex links | Two call sites | One call site |
| --- | ---: | ---: |
| First ordering | 15.248 s | 9.686 s |
| Reverse ordering | 15.169 s | 9.569 s |

The same generated wrapper ran in a controlled GPU output check
with varied positions, texcoords, colors, and opposing
quaternions. Both variants produced 1,152 matching 32-bit words
with 444 outputs varying across vertices. A deliberately broken
control omitted quaternion correction and produced 64 mismatching
words. This proves the tested wrapper behavior. The attempt to
run an instrumented full PICA program with synthetic uniform inputs
returned a GLES out-of-memory error on the baseline, so this
fixture provides no full-game pixel or depth result.

#### Same-APK normal boots

APK `faf0cb0` passed the Atlas and GLES32 Android builds.
The RG Slide ran its same Atlas preview with the single site first
and the standard two sites second. Both arms had independent empty
Cara archives and preview saves, the original driver cache, JIT,
fixed CLK 100%, 1×, and the separable GLES path. Indexed draws and
LUT residency were enabled for both arms. Adaptive Frame Skip was off.
The only setting changed between these boots was the single-site probe.

| Matched scene and draw count | Two call sites p50 / p99 / worst | One call site p50 / p99 / worst |
| --- | ---: | ---: |
| Cold smoky title, end 601, 25,561 draws | 34.63 / 8,752.34 / 41,033.97 ms | 41.56 / 10,554.56 / 32,064.29 ms |
| Settled smoky title, end 721, 45,932 draws | 36.58 / 41.83 / 52.27 ms | 44.56 / 58.83 / 59.40 ms |

At the cold title the worst refresh was 515 for both arms. The
timed shader scope across its 120-refresh window fell from 49.253 s
to 42.225 s. The single site shortened the worst refresh by 8.97 s
and increased the p99. With no shader work in the matched settled
title window, the single site increased the median by 7.98 ms.
The emulation-thread GPU-command CPU category was 26.04 vs 25.98 ms
per refresh, while the submit wall scope increased from 5.43 to
7.64 ms per refresh. No GPU hardware counters were taken on these
boots, so that difference cannot be assigned exclusively to
vertex-shader execution.

At the lab door, the first shader-dominated refresh took 7,478.22 ms
with two sites and 8,239.95 ms with one site. The surrounding p99 was
48.88 and 53.39 ms. It occurred in different 120-refresh windows
because the input and scene timing shifted between launches. In a
settled window ending at 2401, the arms processed 42,228 and 42,229
draws with zero timed compile work. The control recorded 30.16 ms
p50, 56.91 ms p99 and 73.26 ms worst. The single-site run recorded
38.20 ms p50, 49.60 ms p99 and 80.85 ms worst. The GPU-command CPU
category was 21.44 vs 21.81 ms per refresh there.

A later lab window ending at 3121 had 46,584 draws in both arms,
zero indexed fallbacks and zero timed shader work. The control
recorded 32.88 ms p50, 38.96 ms p99 and 40.90 ms worst. The single
site recorded 43.86 ms p50, 49.13 ms p99 and 54.10 ms worst.
Graphics CPU time was 23.07 vs 23.25 ms per refresh, and GLES
submit wall time rose from 5.25 to 9.29 ms per refresh. The
sampled drawing subroutes varied slightly within the equal total
draw count. Screenshots showed the lab lighting and translucency
at different animation ticks.

The slower settled behavior outweighed the cold title improvement.
The switch was removed from the branch tip. Commit `faf0cb0` and
its exact Android artifact retain the experiment for reproduction.
The original Cara was reopened after removing the preview and the
original configuration SHA-256 matched. Raw timing and oracle logs
are under `/tmp/cara-gcloud.8yZRID/mali-vertex-link-*`.
[Android benchmark build](https://github.com/dmikey/cara/actions/runs/45478137471)

### Unchanged separable vertex binaries

A standalone GLES 3.2 test saved five stock Dark Moon vertex stages on
the same RG Slide. Four have exact shader-source hashes from the smoky
title trace. The fifth is from the same guest instruction family. All
five were linked independently with binary retrieval enabled. The test
kept the two-call shader and used the normal GLES 3.2 output locations.

| Five vertex stages on the stock driver | Time |
| --- | ---: |
| Source compile and compile status | 2.587 s |
| Standalone link and link status | 15.211 s |
| Binary restore and program status in a separate process | 14.61 ms |
| Repeated binary restore in another process | 15.01 ms |

The five native binaries totaled 1,520,804 bytes. Each stayed below the
existing 1 MiB binary entry cap. A fresh GL program with its initial
nonseparable setting rejected all five binary loads. Explicitly setting
`GL_PROGRAM_SEPARABLE` on the destination before calling
`glProgramBinary` restored all five. Each reported successful link
and separable status, bound to a program pipeline as a vertex stage,
and retained its `vs_config` uniform block. The restore timing
excludes file reads, pipeline setup, and the first draw.

A controlled draw test then compared a source-linked separable vertex
stage with a fresh destination restored from its binary. Both stage
pairs passed GLES pipeline validation with an independently linked
fragment stage. The opposing-quaternion test matched all 1,152 output
words and produced 444 values that varied across vertices. This is a
controlled GPU output check. The five captured Dark Moon binaries were
validated through restore and binding, with full-game first draw,
color and depth still unmeasured.

Cara currently skips linked-pair archive loading when separable mode is
active. A bounded vertex-stage archive can reuse the existing driver
identity, checksum, atomic write and entry limits. Store it separately
from linked pairs and key each entry by stage kind and exact generated
source and dialect. On restore, set separability before binary upload,
check the result, and bind uniforms and samplers as for ordinary source
creation. Fall back to the unchanged source if a binary is rejected.
Record first-draw time and compare pixels before enabling it.
This can remove recorded vertex links on repeat boots. An empty
archive still requires the initial links, and the baked entry and input
differences above must be preserved on first encounters.
[OpenGL ES 3.2 specification](https://registry.khronos.org/OpenGL/specs/es/3.2/es_spec_3.2.pdf)

No app was installed for this test. The fixture logs are under
`/tmp/cara-gcloud.8yZRID/mali-stage-binary-fixture`.
