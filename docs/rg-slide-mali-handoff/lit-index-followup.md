# Warm Gadd lit indexing and index upload identity

Both comparisons used Android APK `53770ae` on the RG Slide Mali-G57,
Dark Moon USA from normal boot through save selection, JIT + GLES Atlas,
Performance Mode, 1×, fixed CLK 100%, adaptive frame skip disabled. Both
arms copied the same warmed shader cache seed. No timed shader work or
surface downloads occurred in the four sampled Gadd windows per arm.

| Comparison | Median of four refresh-window medians | Per-window p99 range | Worst refresh | Emulation CPU/refresh | PICA and host graphics CPU/refresh | Lit indexed draws |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Lit-list comparison control | 28.74 ms | 32.29–52.95 ms | 75.24 ms | 28.49 ms | 19.29 ms | 0 |
| Lit-list opt-in | 29.76 ms | 36.47–52.36 ms | 71.07 ms | 28.16 ms | 19.47 ms | 0 |
| Index identity comparison control | 29.37 ms | 34.41–51.74 ms | 52.51 ms | 27.83 ms | 19.18 ms | 0 |
| Verified index upload identity | 29.08 ms | 34.85–51.22 ms | 51.96 ms | 27.44 ms | 18.74 ms | 0 |

The first opt-in ran after its control. The index identity opt-in ran
before its control. The estimated 0.39 CPU-ms improvement in the identity
comparison is from a single pair. Both index identity arms showed 56% SPD
at the lab checkpoint. Identity skips comparison with a previously
uploaded index payload when the validated cache entry is the same.
Guest index bytes still undergo the live validation check.

The lit-list option failed to activate in all sampled windows. Hardware
position requests per 1,000 primitives stayed at 538.5–538.6 in all arms.
This test cannot measure the cost or visual equivalence of an active lit
route. Each comparison produced different instantaneous HUD FPS readings,
so the refresh statistics and CPU times are better comparison measures.

A second APK adds selection counters for packed inputs, the shadow
capability gate, feedback mode, missing pair data, estimated reuse, and
indexed shader availability. The counters only report when the existing
renderer timing trace is active. They leave the selection rules intact.

On-device rejection counters from `1921a52` showed 27,830–28,060 lit
Gadd candidates in each sampled window. Every candidate had packed inputs
and pair data. They all used PICA Shader topology and missed the strict
reuse threshold applied outside the literal List topology. No indexed
program request was made for those candidates.

The opt-in now admits Shader topology after the existing geometry-stage
rejection, for ordinary non-shadow draws. The mode draws the same
assembled triangles as the regular path, so pair indices preserve the
same triangle order and first-vertex orientation reference.

## Phase-level Android tracing

Set `trace_renderer_timing = True`, `trace_cpu_categories = True`, and
`trace_render_phases = True` under `[Debug]`. Keep JIT, GLES Atlas, the
gameplay mode and guest clock fixed, then capture `adb logcat -v epoch -s CaraPerf`.

- `cpu` attributes thread CPU time to exclusive emulator categories.
- `draw_phases` reports wall time for framebuffer acquisition, texture
  acquisition, shader setup, resource invalidation, index/input reuse
  checks, stream mapping and surface remap.
- `lut_phases` separates lighting/fog from procedural LUT updates. Within
  the 2D LUT path it splits saving and setting GL state, texture transfer,
  and restoring GL state. Upload count and bytes are also logged. Subtract
  the three texture scopes from the combined LUT scopes to estimate
  the remaining staging and conversion time. The native texture-buffer
  path contributes to LUT totals but has no 2D texture transfer scope.
- `draw_sample` and `texture_phase` report sampled thread CPU and wall time
  plus sample counts. They refer to sampled calls only. Nested phases
  overlap, and should be compared across matching 120-refresh windows.

Use a trace-disabled comparison for final performance claims. Keep
GPU sampler measurements separate from CPU phase wall times.

## Active lit indexing on the Slide

APK `b62bfdc` passed Android ARM64 CI. The opt-in admitted the prepared
Shader-topology candidates. Once the EGL worker had linked the matching
programs, it issued 25,492–27,528 lit indexed draws in each of three later
Gadd windows. Before those programs were ready the regular path continued
drawing. The earlier Gadd windows showed zero executed lit indexed draws,
so their timings cannot measure the active path.

| Later Gadd, refresh windows 3841 / 3961 / 4081 | Regular | Lit indexed |
| --- | ---: | ---: |
| Guest PICA command lists | 2,160 | 2,160 |
| Guest draw requests | 139,064 | 139,224 |
| Emulation-thread CPU per refresh | 29.45 ms | 32.91 ms |
| PICA and graphics CPU per refresh | 20.61 ms | 23.20 ms |
| Per-window p99 range | 32.05–43.70 ms | 38.67–41.43 ms |
| Worst refresh | 44.79 ms | 79.53 ms |
| Sampled Mali position requests per 1,000 primitives | 525.7 | 506.2 |

Draw counts differed by 0.12%. Per-draw coverage, frame composition, and
worker activity can still differ. The evidence supports higher CPU cost
on the measured opt-in run and a small reduction in Mali position work.
It does not establish an exact per-draw CPU penalty. Keep the mode opt-in.
The raw comparison is in [the 120-refresh logs](mali-lit-active-comparison.log.txt).
The host assembler and pair-cache fixture passed 3,000 cases, including
Shader winding and guest memory mutation. Exact device color/depth output
has not been verified.

## Lighting and fog transfer result

APK `f27f71c` passed Android ARM64 CI and captured the same normal boot at
fixed CLK 100%. The trace-disabled performance baselines remain separate.
The phase trace found zero procedural LUT time in these windows.

| Phase | 2D LUT transfer wall time per refresh | Observed upload traffic |
| --- | ---: | --- |
| Warm title, windows 721–961 | 17.08–18.18 ms | 480 uploads and about 5.35 MB per 120 refreshes |
| Early opening, window 1681 | 0.99 ms | 96 uploads of 1 KiB, matching fog-only table size |
| Quiet opening, windows 1801–1921 | 0 ms | No uploads |
| Entry to lab, windows 2401 and 2521 | 4.49 ms and 7.05 ms | 74 / 111 uploads, totaling 84,992 / 162,816 bytes |
| Warm lab, window 2641 | 0 ms | No uploads |

Title GL setup and restore together took about 0.03 ms per refresh. The
bulk was inside the `glTexSubImage2D` scope, despite small transfers.
This locates the stall at the transfer API boundary. It does not identify
the internal driver work. Rotating the destination texture or using a
separate fenced buffer for fog can test whether reusing a texture sampled
by earlier draws is responsible. Use the existing native texture-buffer
path as a control. Its earlier all-table test accelerated the title and
slowed warm Gadd. A persistently mapped PBO alone would change the source
staging while retaining the destination texture hazard.

See [phase windows](mali-render-phases.log.txt) and
[Android CI](https://github.com/dmikey/cara/actions/runs/45202872936).
The original signed APK and profile were restored after the tests.

### Dedicated fog texture trial

Exact Android APK `1fb7a46` booted Dark Moon four times at JIT + GLES Atlas,
1×, Performance Mode and CLK 100%. The trial put the 128-entry fog table
in its own 8 KiB allocation, sampled it with the same interpolation, and
kept lighting on the shared 2D texture. Both restarts restored 128 out of
128 shader binaries from their own cold runs. Each arm used the same
route through title and save selection to Gadd's lab.

| Measurement | Shared texture | Separate fog |
| --- | ---: | ---: |
| Warm title texture transfer per refresh, windows 721–961 | 18.22–19.09 ms | 17.76–18.86 ms |
| Warm lab-entry texture transfer per refresh, windows 2401 and 2521 | 5.08 / 7.97 ms | 5.04 / 7.11 ms |
| Warm lab-entry p99, same two windows | 89.46 / 47.00 ms | 41.78 / 71.62 ms |
| Warm Gadd window median range | 29.17–29.97 ms | 28.92–29.50 ms |
| Warm Gadd window p99 range | 36.03–56.34 ms | 34.04–52.19 ms |
| Worst warm Gadd refresh | 71.85 ms | 52.19 ms |

Cold title arrived at 103.20 s shared and 103.10 s split. GLSL validation
passed 16 generated fog and logic-op variants. No renderer API errors
were logged in these arms. The measured warm transfer reductions were
under 1 ms per refresh and lab-entry p99 moved in different directions.
Scene draw counts varied and pixel equivalence was not established.
The branch removes the experiment and its test switch. The original
signed APK and checked user profile files were restored.

[Exact Android test APK](https://github.com/dmikey/cara/actions/runs/45218080631)
