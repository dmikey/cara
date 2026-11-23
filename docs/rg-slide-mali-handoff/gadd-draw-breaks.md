# Where warm Gadd splits GLES draws on the RG Slide

The US Dark Moon ROM booted normally on the physical Unisoc T820 / Mali-G57.
The test used Dynarmic JIT, GLES Atlas, fixed CLK 100%, 1× and Performance Mode.
Android presented on a second EGL context. The native emulation thread ran the
PICA decoder and prepared and submitted the gameplay draws. Each boot restored
the same on-device program cache. Window p99 is the second-longest system
refresh in each 120-refresh window. Graphics CPU is exclusive emulation-thread
CPU time charged to GPU commands.

## Draw boundaries

The [trace build](https://github.com/dmikey/cara/actions/runs/45239138449)
observed 169,963 accelerated submissions over 480 refreshes in the lab. Each
120-refresh count matched the guest draw-trigger counter. A boundary can have
several causes. The first column assigns each draw to one reason in the order
shown below. The last column includes overlapping reasons.

| Boundary | First reason | Any occurrence |
| --- | ---: | ---: |
| New system refresh | 480 | 480 |
| Command list | 1,468 | 1,948 |
| Guest-visible operation or IRQ | 0 | 1,948 |
| Sampled output or special feedback path | 0 | 0 |
| Render attachments | 12,654 | 14,602 |
| Shader/program | 30,198 | 44,800 |
| Primitive or vertex route | 0 | 10,714 |
| Raster, blend, depth, stencil or clip | 974 | 31,164 |
| Samplers and sampled textures | 54,544 | 99,344 |
| Common uniforms | 0 | 45,774 |
| Lighting uniforms or LUT upload | 0 | 1,956 |
| Vertex uniforms | 36,529 | 99,835 |
| Packed layout | 0 | 30,194 |
| Vertex or index payload / count | 974 | 131,490 |
| Physical upload storage | 32,142 | 169,963 |

Every adjacent draw used different physical upload storage. For 32,142 draws
(18.9%), this was the only observed difference. Those comparisons checked
staged vertex, index and element bytes. They establish an upper bound on
adjacent candidates under this equivalence test. The [packet trace build](https://github.com/dmikey/cara/actions/runs/45241122057)
also captured intervening PICA commands. All 48 samples at refreshes 2640,
2760, 2880 and 3000 included a rewrite of 16 vertex uniform words, two TEV
colors, attribute buffer base/offset, and the index buffer/count before another
indexed trigger. Packet side effects and possible resource aliases require a
separate proof before combining submissions.

Reproduce the counts with the [losslessly compressed draw masks](gadd-draw-breaks.log.gz).

```sh
cd docs/rg-slide-mali-handoff
python3 summarize-draw-breaks.py gadd-draw-breaks.log.gz 2641 2761 2881 3001
```

## GPU activity in the lab

A separate untraced baseline boot captured 79 hardware-counter samples over
7.90 seconds immediately after the warm timing windows. Shader-core active
cycles divided by four times GPU-active cycles averaged 94.07%. Fragment and
nonfragment queues were active for 62.47% and 75.04% of GPU-active cycles
respectively. These intervals overlap. Tiling was active for 61.97%.
The tiler could not send a position-shading request because its queue was full
for 33.35% of GPU-active cycles. That identifies backpressure in geometry on
this workload in addition to the measured host submission overhead. The device
did not expose a clock trace for this run. The hardware counters do not establish
wall-clock occupancy or the duration of any one draw.
[Counter capture](gadd-tiler-counters.csv.gz) · [Arm counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## Test of equivalent adjacent draws

The private Android trial peeks at packets through the next indexed or array
trigger and checks the complete float32 vectors against the live PICA uniforms.
It accepts a small set of scalar register writes. Texture and vertex inputs,
index data and command memory must be disjoint from the selected framebuffer
outputs. A qualifying pair issues two instances of the original primitive in
one GLES call. Both guest triggers and the intervening PICA writes still execute
in order, including guest timing charges. It keeps the ordinary path for shader
hooks, geometry processing, feedback and unproven ranges. The first build also
requires textures without mipmaps. This is a private diagnostic in a release
configuration APK. Production defaults are unchanged.

Four normal boots on the [same APK](https://github.com/dmikey/cara/actions/runs/45243481062)
restored 197/197 program binaries and used the same fixed CLK 100% configuration.
Both controls and both candidates included four complete warm-lab windows with
zero shader compile time. The title comparison uses three full windows per boot,
with exactly 153,320 guest triggers in each boot. Times are milliseconds.

| Run in execution order | Gadd pairs combined / guest draws | Gadd graphics CPU per refresh | Window p99 range | Worst @ refresh | Title graphics CPU per refresh | Title worst |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [On 1](gadd-instance-on-b.log.gz) | 5,368 / 170,312 | 19.18 | 33.49–51.83 | 72.20 @ 2591 | 23.22 | 59.39 |
| [Off 1](gadd-instance-off-a.log.gz) | 0 / 169,265 | 19.07 | 33.74–50.99 | 69.58 @ 2529 | 22.84 | 61.17 |
| [On 2](gadd-instance-on-d.log.gz) | 5,313 / 168,566 | 19.13 | 34.69–51.06 | 76.37 @ 2779 | 23.20 | 57.81 |
| [Off 2](gadd-instance-off-c.log.gz) | 0 / 168,915 | 19.15 | 35.15–52.07 | 73.02 @ 2572 | 22.94 | 63.19 |

The bounded search combined 3.15% of the guest draws in Gadd. Average graphics
CPU was 19.15 ms enabled and 19.11 ms disabled. It merged zero pairs at the
title, where the search increased graphics CPU by 0.32 ms per refresh across
the two boots per arm. This variant did not earn activation.

## Allowing disjoint mip levels

The [second APK](https://github.com/dmikey/cara/actions/runs/45245219403)
also checks each existing mip-level source selected by the surface cache.
Each watcher must still be valid and disjoint from the color and depth outputs
in guest memory and in host texture identity. Incomplete and custom mip chains
use the ordinary path. Each warm Gadd lookahead that passed the packet proof
in these three boots could also pass the renderer proof when enabled.

| Same APK, normal boot | Gadd pairs combined / guest draws | Gadd graphics CPU per refresh | Window p99 range | Worst @ refresh | Title graphics CPU per refresh | Title worst |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [On 1](gadd-mip-on.log.gz) | 17,460 / 169,266 | 18.49 | 34.71–50.77 | 72.52 @ 2929 | 23.58 | 59.01 |
| [Off](gadd-mip-off.log.gz) | 0 / 169,614 | 19.06 | 35.03–50.99 | 54.34 @ 2593 | 22.90 | 59.03 |
| [On 2](gadd-mip-repeat.log.gz) | 17,424 / 168,916 | 18.12 | 35.50–50.08 | 53.60 @ 2929 | 23.15 | 63.91 |

Combining about 36 pairs per refresh eliminated 10.3% of host submissions
and saved 0.58–0.95 ms of emulation-thread graphics CPU in Gadd against the
intervening control. The title merged zero pairs and the candidate cost
0.25–0.68 ms of graphics CPU there. The slowest window in each boot still had
a p99 above 50 ms. Guest draw totals vary across normal boots; all three
restored 197/197 program binaries with zero source links and zero measured
lab-window compilation. The original package/profile was restored after
each boot. A host disk-space interruption in an earlier run occurred before
title measurement and was excluded; the repeated run completed.

A separate eight-pixel fixture on this Mali-G57 compared two direct array
draws against one two-instance array draw and repeated the comparison with
an element buffer. Its vertex-ID colors distinguish the indexed and array
paths. The results matched within each comparison after blending, a farther
depth-rejected draw and a stencil-equals-two test. This verifies GLES
instancing behavior for the fixture. Exact output parity for the real
multi-pass Gadd scene and cross-title regression coverage remain open.
[Standalone fixture](https://github.com/dmikey/cara/blob/aa58aa1/emulator/src/android/jni/tests/ordered_gles_probe.cpp)

## Independent graphics worker experiment

A separate [Android worker build](https://github.com/dmikey/cara/actions/runs/45248433508)
transferred the main GLES submission context to an ordered worker and grouped
LUT upload state setup, transfer and restoration in one owned command.
The PICA register parser and draw preparation remained on the emulation thread.
The direct and worker normal boots used the same APK, packed layout uniform
reuse and a matching 128-entry binary cache. Both restored 128 binaries with
zero source links. Each of the four warm Gadd windows had zero shader compile
time and zero surface download time.

| Gadd, 480 refreshes per arm | Emulation-thread graphics CPU ms/refresh | Worker CPU ms/refresh | Window p99 range, ms | Worst ms @ refresh |
| --- | ---: | ---: | ---: | ---: |
| Direct | 19.17 | — | 35.51–50.82 | 70.33 @ 2969 |
| Ordered worker with grouped LUT uploads | 12.57 | 12.61 | 35.05–57.74 | 110.85 @ 2591 |

The grouped worker moved 6.60 ms off the producer. During the three sustained
windows after the first, it recorded about 25,000 commands, 398 submissions
of command batches and 19 synchronous calls per refresh. The two threads
overlapped their work. Title window 961 had the same 51,168 guest draws in
both boots; median refresh time improved from 50.28 to 43.10 ms and p99
from 57.22 to 48.94 ms. The producer's elapsed lighting/fog LUT scope fell
from 2,225.83 ms to 14.57 ms across the window, including the effect of
moving driver time to the worker. Warm Gadd's longer worst frame keeps this
experimental. This trial ran independently of the draw-combining branch.
