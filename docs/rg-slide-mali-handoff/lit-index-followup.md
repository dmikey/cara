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
