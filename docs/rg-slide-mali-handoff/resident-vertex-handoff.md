# Warm Gadd on the RG Slide, November 23, 2026

A single exact vertex check eliminated most packed staging writes. It increased
graphics CPU time in both comparisons. The candidate remains on
[its experiment branch](https://github.com/dmikey/cara/tree/dmikey/mali-resident-ordinal).
The standalone layout-uniform experiment uses a setting already present in the baseline.

## Device and workload

Physical Unisoc T820, Mali-G57 r40p0, GLES Atlas, Dynarmic JIT, fixed guest
CLK 100%, 1×, Performance Mode, async indexed shaders, adaptive frame skip off.
Each run booted the US Dark Moon ROM normally, went through title and file
selection, and reached Gadd's lab. Each restored the same 197 program binaries,
with zero source links on restore and zero shader compiles in the four measured
windows. Every window contains 120 system refreshes. Graphics CPU is the
exclusive emulation-thread CPU time attributed to GPU commands. Prep is an
elapsed-time scope. P99 is the second-longest refresh in each window.

The baseline APK was [f202e4e](https://github.com/dmikey/cara/actions/runs/45224342082).
The [a548534 candidate](https://github.com/dmikey/cara/actions/runs/45231731805)
compares the packed constants, live source spans and zero padding against the
next unused entry in the regular packed size class. An immutable shadow holds
the validated bytes through submission. The draw checks the current texture
owner, format, extent and allocation before bypassing the pool search. It
checks the index payload independently. Changed owners and incompatible draw
modes retain the standard path. A host sanitizer fixture exercised changed
source and index data, owner replacement, pool relocation and disabled modes.

## Warm lab measurements

| Boot in execution order | Draws/refresh | Graphics CPU ms/refresh | Prep ms/refresh | Staging writes MB/refresh | Window p99 range, ms | Worst ms @ refresh |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A, baseline | 352.6 | 19.16 | 1.86 | 13.35 | 34.74–50.92 | 57.46 @ 2900 |
| B, resident | 352.6 | 19.34 | 2.77 | 5.65 | 34.77–51.81 | 51.85 @ 2793 |
| C, baseline | 351.9 | 18.92 | 1.85 | 13.32 | 33.18–49.82 | 50.19 @ 2559 |
| D, resident | 351.9 | 19.24 | 2.82 | 5.66 | 35.12–52.05 | 73.43 @ 2572 |
| E, layout cache | 351.9 | 18.44 | 1.85 | 13.32 | 34.63–50.63 | 51.83 @ 2559 |
| F, layout cache | 353.4 | 19.16 | 1.90 | 13.37 | 33.94–50.33 | 74.75 @ 2542 |

B and D consumed 222,111 resident matches across 268,329 exact checks. That
covers about two thirds of all guest draws. A and B matched draw counts and
GLES upload bytes in every measured window. Both candidate boots increased
graphics CPU relative to their preceding controls. The measured staging writes
are software byte counts. Physical DRAM traffic requires separate counters.

| Refresh window ending | A p99 / worst | B p99 / worst | C p99 / worst | D p99 / worst | E p99 / worst | F p99 / worst |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2641 | 39.50 / 50.68 | 42.53 / 51.66 | 48.35 / 50.19 | 52.05 / 73.43 | 49.76 / 51.83 | 34.51 / 74.75 |
| 2761 | 34.74 / 34.90 | 34.77 / 35.26 | 36.71 / 40.87 | 50.47 / 67.96 | 35.36 / 37.82 | 33.94 / 54.75 |
| 2881 | 50.67 / 52.02 | 51.81 / 51.85 | 33.18 / 33.71 | 46.12 / 50.55 | 34.63 / 35.39 | 50.14 / 50.62 |
| 3001 | 50.92 / 57.46 | 50.50 / 51.12 | 49.82 / 50.02 | 35.12 / 35.14 | 50.63 / 50.74 | 50.33 / 50.56 |

## Where the thread goes

Nine-second Simpleperf samples ran after the four timed windows, on the
NativeEmulation TID, at 199 Hz using cpu-clock and DWARF stacks. Cara PCs were
resolved with each installed APK's exact symbol map. Percentages below are
weighted shares of total sampled on-core time. Driver samples measure CPU
inside the vendor binary. They do not measure GPU execution time.

| CPU sample share | C, baseline | B, resident | D, resident | E, layout cache | F, layout cache |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mali driver | 32.03% | 32.82% | 34.96% | 30.71% | 30.09% |
| DrawArrays + DrawElements | 10.21% | 11.85% | 13.61% | 11.54% | 11.05% |
| FramebufferTexture2D | 4.21% | 3.67% | 4.40% | 3.67% | 4.16% |
| BindTexture | 3.81% | 3.26% | 2.64% | 3.26% | 2.74% |
| GetUniformLocation | 1.56% | 2.00% | 0.94% | 0.00% | 0.00% |

On the control, 4.04% of thread CPU samples landed in `memcmp` under the
upload-order search, and 3.35% in `__memcpy` under packed preparation. On the
two resident boots, 3.11–3.38% landed in `memcmp` under the new exact
comparison. Packed preparation copies fell to 0.63–0.76%. The first exact
check still reads live bytes to prove that guest writes have not changed them.
The CPU pays for that proof even when the GPU texture is already resident.

The B build's driver sample at Cara PC `0x9295dc` called
`glGetUniformLocation` with the ELF string `cara_vertex_layout[0]`.
E and F enabled only `packed_layout_uniform_reuse` on the original APK.
This caches the location and skips redundant layout uploads. C and E matched
draw counts and GLES upload bytes in all four measured windows. Live-boot
timings establish CPU cost and frame times. Pixel-exact equivalence and
coverage of program replacement across games remain open.

Across the two boots per arm, the baseline averaged 19.04 ms of graphics CPU
and 5.39 ms of guest execution per refresh. The resident path averaged 19.29 ms
of graphics CPU, with preparation rising from 1.86 to 2.80 ms elapsed.
The layout cache averaged 18.80 ms of graphics CPU. On its exactly
draw-matched comparison (C/E), it saved 0.48 ms per refresh, but its second
boot recorded 19.16 ms and a 74.75 ms worst frame. Neither experiment earned
a default change.

The most expensive identifiable driver calls in all five stack captures were
draws. To establish how much of that cost can be removed, count consecutive
draws that share attachments, textures, shader and primitive state and record
the first state change or guest-visible dependency that splits each run.
The earlier paired-framebuffer test saved only 0.16 ms in submission.
The vertex result also puts a bound on copy-only changes. Skipping the live
comparison itself would require a reliable write signal for every covered
guest span, including direct JIT stores, DMA, aliases and save-state restore.
