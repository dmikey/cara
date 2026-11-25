# G Cloud Dark Moon: shader burst attribution

2026-11-25. One actual normal-boot shader census on the physical Logitech G Cloud (GR0006, Snapdragon720G / Adreno618), private package only. JIT Dynarmic, GLES Atlas monolithic programs,1×, fixed guest CLK100%, Adaptive Frame Skip off. No failed separable route, CPU-vertex fallback change, missing draw/effect or clock substitution. This is diagnostic evidence, not a quiet performance comparison or cold-driver claim.

## Finding

The long refresh is a **burst of distinct shader combinations built synchronously on the emulation thread**, rather than one83-second shader or repeated identical-stage compilation. The existing asynchronous option did not create a shader-build worker during this boot.

The actual frame515 reported84,357.76ms elapsed and83,844.30ms in its shader compile/link scope. It emitted16 explicit `vertex_hint refresh=515` records with16 distinct source hashes. Those source bytes are recovered and SHA-256 bound. Fifteen vertex variants are75,717–75,986 bytes; the remaining variant is14,637 bytes.

Use the complete **120-refresh window ending601** for the per-stage/API totals below. Individual link and fragment records do not carry refresh IDs, so they must not all be assigned to frame515.

| Observed scope, window481→601 | Count | Host return time |
|---|---:|---:|
| Vertex compilation plus status query |16|37.291487s|
| Fragment compilation plus status query |36|1.113325s|
| Program link plus status query |46|84.528409s|
| Sum of those disjoint same-thread API return spans |98|122.933221s|
| Existing encompassing compile/link phase counter |—|122.969420s|
| First observed draw submissions for those programs |46|11.888ms total,2.810ms maximum|
| Fragment-source generation |36|3.269ms total|

Every observed shader census record in that window is on NativeEmulation TID19482, PID19448. Initial and windows121/481/601/1201 thread inventories contain no CaraShaderBuild worker. No worker/foreground overlap was observed; there is **no evidence for a driver lock between those two contexts in this run**. Census return timestamps have millisecond log precision; source durations are recorded in microseconds. These are host API/status spans, not physical GPU execution time.

The46 links represent46 different vertex/fragment source pairs. There are no repeated compilations of an identical stage/source in this window and no duplicate exact-pair links. A retained compiled vertex stage is nevertheless linked with as many as8 different fragment variants. That is real additional program-link work; compiled-stage reuse alone does not remove the driver's per-pair work.

The first draws are small beside the122.9-second compiler/link interval. They do not explain this startup burst. Later zero-compile title windows still have severe tails; this diagnostic does not claim the warm rendering problem is solved.

## Why async did not hide it

In current source, Performance Mode enables `gles_async_programmable_shaders`. Vertex selection permits deferred creation, but `ApplyIOSPipeline` only queues the programmable build when software vertices are allowed **and** the matching software-vertex program for that exact fragment source is already linked. If that ready fallback does not exist, obtaining the real vertex handle and creating the required program executes compilation/linking synchronously on NativeEmulation. The actual single-thread census and missing worker agree with this source path.

Removing that guard would not by itself provide a correct first-use draw. A future change needs an exact already-correct program, truly earlier exact-key preparation, or lower source/driver compilation cost while preserving PICA work. No skipped draw, empty shader, removed quaternion correction or CPU-vertex fallback was introduced here.

The large sources include `cara_run_vertex` for the triangle's reference quaternion when `gl_VertexID % 3 != 0`, followed by evaluation of the actual vertex. This preserves the existing PICA sign correction. Existing unused-quaternion specialization is only a potential next discriminator on proven fragment configurations that do not consume it; global removal is not justified. Cache identity and exact output must be reviewed before such a test. Native-attribute/geometry and other closed driver experiments are not being retried by this report.

## Source identity and workload limits

Built source0645df2 and main f92bf4e have the identical tree68d3293c888affed801ef75416efdc869cc13741. The existing reviewed APK is058f062f1a585fa18d170aaf9e5f8fc911862207a457b9e29e57746729b32000. No new build was needed. Only the existing shader-census observer was enabled over the previous default normal-boot profile; previously enabled renderer/phase/CPU counters remain equal. Private Cara cache began empty; the stock driver's private cache was preserved and its warmth is unknown.

The normal pipeline cache checkpoint was exported before shutdown: SHA-2566e1de155dac09ff2a808bce51a941a68f1da662be1230d2fb69ed1b94e3ec3b1,47 entries/54 unique sources. Its bounded header, exact length and FNV digest were validated using the existing archive format. Each of the16 explicit hot vertex sources matches the same-run archive's SHA-256 and Android two-lane hardware-CRC source ID. There are no source-ID collisions among archived sources. The older87-pair archive was used for early offline analysis only and was subsequently reconciled to this same-run export. Retail-derived GLSL and binary cache bytes remain local, not committed to the repository.

Census does not independently time vertex source generation. It does time fragment generation. Per-frame compile counters and explicit VS refresh markers are independent observations; a numerical API-prefix exploration was retained as a hypothesis only and is **not** used to claim an exact per-frame link count. Source capture, logs and screenshots are not calibrated for FPS or exact frame-aligned pixel parity. Title and both panels were physically inspected; no new Gadd seed or warm ON/OFF comparison was attempted in this diagnostic.

## Preserved failures and final state

A preflight rejected two live Cara processes before any census launch. Its premature cleanup incorrectly renamed the current private folder even though it did not own that profile. The exact rename was immediately reversed under explicit corrective authorization, with no overwrite or app stop. Open handles remained live; path-based behavior during that interval was not independently verified. The cleanup now requires a profile-prepared ownership flag before it can archive anything. The user subsequently explicitly authorized closing both apps for the trace; their current private data was preserved separately before the one actual census boot.

The actual census completed endpoints121/481/601/1201 and exported its complete source archive. Normal Exit Game was not confirmed before its90-second deadline while analysis continued, so the owned private app was force-stopped. This remains a failed cleanup receipt, not a clean native-exit claim and not a reason to repeat the successful capture. Three late scroll gestures based on an old menu image landed on the launcher after timeout; no tap, launch, deletion or main-app action followed. Future UI actions must recheck the current owned PID and foreground.

RESTORE-RECEIPT.json verifies the exact user private folder was restored; the player APK3a4d42e9af128a1e32a5f3916b563090ea8c18bbdc7acebb93902126cdc96f98 and config/state/sdmc/nand inventory are unchanged. Both Cara PIDs are absent. Stock `security.perf_harden` remains1, with no root/security changes. All failed receipts, corrective restoration, full logs, source bindings and hypotheses remain preserved under `outputs/cara-gcloud-shader-census-20261125`.

## Source-only follow-up candidate

The exact fragment-consumer audit identifies11 no-quaternion-read pairs, associated with seven of the16 vertex sources. A single private Adreno618/V@0502.0-gated specialization is proposed, with explicit source/cache separation and correctness gates; it has not been implemented or run. Mali defaults remain untouched. [Candidate design and test gates](g-cloud-unused-quaternion-candidate.md).
