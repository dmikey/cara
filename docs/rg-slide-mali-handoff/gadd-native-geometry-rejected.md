> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Slide native vertex delivery and GPU saturation

The native-attribute + compact geometry-shader prototype linked the observed cold burst faster, but **regressed warmed Gadd gameplay**. Keep it off. The physical GPU counters show a heavily occupied GPU as well as a heavily occupied emulation thread; aggregate SoC CPU usage would miss the latter.

## Exact scope

Private source commit `19558ebddb7c7120eaef1ea06a644d879a97ec5d`, APK SHA `9d969547b23ac831e34bd96bd7b22c70a2512bb8f6c12dc99fbaf2e3d7dff059`, genuine same-binary save SHA `961bfb97f0cd26ea6370f14ff6ad4eb450ff712b36a2680c9f8172ceee153e9d`. Physical RG Slide, Unisoc T820 / four-core Mali-G57, JIT, guest CLK100%, UBO and raster-state/layout reuse on, frame skip off. This new genuine save is a different lab phase from the earlier original fixed save: do not compare their timings.

Both arms use the same experimental GLES320 monolithic three-stage policy and ignore the two-stage archive symmetrically. The control retains packed vertex fetching; the candidate uses native attributes only for eligible lit triangles, with a compact GS preserving triangle order and quaternion-reference sign correction. This is not a direct comparison against shipping separable GLES. No title-specific eligibility, omitted draws, CPU vertex fallback, altered guest timing or missing GL checks.

## Quiet comparison

Existing counters remain enabled identically; synchronous GL debug and hardware observers are off. One control and one native run, all declared work executes. Windows 481 and721 were prospectively selected as compile-free;601 is kept separately because it discovers additional shaders.

| Arm / window | p50 ms | p99 ms | worst ms | graphics CPU ms/refresh |
|---|---:|---:|---:|---:|
| Packed481 |29.37|50.98|51.52|16.009|
| Native481 |42.62|52.79|74.39|20.242|
| Packed721 |29.21|48.06|55.13|17.449|
| Native721 |43.30|70.87|72.23|22.233|

At refresh496 the total observed compilation scopes in window601 fell35.462→13.285seconds; its worst refresh fell34.523→12.380seconds. These scopes include stage compile and program link calls, not31 independent shaders. This cold improvement does not justify the warmed regression. Driver-private cache warmth is not controlled; neither a public performance claim nor statistical certainty follows from one pair.

## Hardware counters, separately instrumented

The existing Mali sampler read actual kernel hardware counters every100ms. Only fully contained intervals between consecutive restored 120-refresh log endpoints were included. No extrapolated GPU time and no addition of overlapping hardware ratios. Raw CSVs, monotonic logs and clock traces are retained under `counter-pair-2`.

| Arm/window | GPU active Mcycles/s | shader-core activity % of GPU-active capacity | nonfragment % | read GB/s | write GB/s | emulation-thread CPU/wall % |
|---|---:|---:|---:|---:|---:|---:|
| Packed481|842.84|93.36|65.73|1.105|1.052|86.99|
| Native481|841.96|92.08|72.48|1.522|2.078|78.18|
| Packed721|841.01|93.51|64.98|1.053|0.978|92.46|
| Native721|840.27|92.39|72.55|1.502|2.080|81.53|

The GPU active-cycle rates equal98.86–99.16% of the manufacturer's **rated850MHz** capacity. This is explicitly a rated-clock comparison, **not a live GPU frequency measurement or directly measured wall-busy percentage**. GPU current/load/available/max/min/time-in-state sysfs reads are denied to shell and run-as. Privileged stock Perfetto devfreq polling and requested GPU/clock events produced CPU frequency tracks but no GPU frequency track. No root, permissions, governor or frequency changes were made. Arm defines GPU-active cycles to include pending/stalled work; high activity is not synonymous with useful ALU execution.

Actual Perfetto CPU clock samples and direct sysfs reads show core7 at2.704GHz, cores4–6 at2.301GHz and cores0–3 at2.184GHz. The running NativeEmulation thread is sampled on core7. CPU fractions above divide exclusive thread CPU categories by exact consecutive monotonic window durations, not median frame time or aggregate SoC CPU. Hardware instrumentation overhead is included. The auxiliary packed /proc poll could read thread ticks/core but not /proc/uptime; its missing timestamps are not used for the percentages. The native auxiliary poll uses actual wall-clock nanoseconds. Thermalservice status was0 after721, but exposes no temperatures; no temperature-based claim is possible.

Both paths keep the GPU active near rated cycle capacity. Native adds substantial nonfragment work and roughly doubles external write bandwidth per second while taking longer per refresh. This does **not** prove the memory bus is saturated. Arm documents that geometry shaders use the advanced geometry route, which cannot use the ordinary split position/varying optimization and writes full outputs to memory. That provides a source-backed explanation consistent with the measured native regression, not a per-shader causal counter attribution.

## Correctness and cleanup

Independent physical attribute conversion fixture passed54cases/1296words against the packed GLSL reference (byte, unsigned byte, short, float, strides, constants, missing zero inputs and float special bits). Integrated synchronous GL-debug runs recorded no GL errors. All original guest draw/work/upload/LUT/cache counters match; only the explicitly expected lit-route classification differs by exactly the native draw count. All eligible native layouts activated with zero rejection. The scored windows upload zero additional native VBO/EBO bytes, proving resident bank reuse in this workload.

Actual packed and native screenshots were opened and reviewed: both show the ghost-filled Gadd lab with lighting and the expected maroon lower panel. Large black side borders explain the screenshot's dominant black statistic. Host screenshots are not guest-frame-aligned, controls can differ in visibility, and animation/lighting phase differs. This is visual smoke plus component equivalence, not exact integrated pixel/depth parity.

The first observer wrapper attempt failed before collecting any window because its exec namespace omitted the observer symbol; preserved in `counter-pair-1`, corrected orchestration only in pair2. Earlier seed/save-menu failures and original fixture failure are preserved. No retry of a completed timing experiment. Native remains off. Both hardware-counter arms completed full gates; all owned collectors reaped. The previous stable private APK and UBO/state/layout-on profile were restored and hash-verified (`RESTORE-STABLE.json` PASS), with no private process left running. Preserve the new APK, source, save and all investigation records. Player app/data remain unchanged.

Sources: [Anbernic rated specifications](https://anbernic.com/products/rgslide), [Arm Mali-G57 counter and geometry documentation](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html).
