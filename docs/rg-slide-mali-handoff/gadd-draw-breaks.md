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
