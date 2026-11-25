# One private Adreno candidate: unused quaternion reference specialization

Source-only audit,2026-11-25. Proposed candidate, not implemented or timed. No additional device boot, driver experiment, shader compilation, shared default or Mali policy change accompanies this document.

## Exact evidence and addressed scope

The [physical shader census](g-cloud-dark-moon-shader-burst-20261125.md) binds16 vertex sources and46 distinct vertex/fragment pairs in the120-refresh window ending601. An offline scan of the **exact generated fragment bodies** finds11 pairs whose `normquat` identifier is absent or occurs only in its input declaration. The other35 pairs read it or are conservatively classified as unknown. Seven vertex sources are used only by those11 no-read pairs in this captured window; nine are read/unknown-only. No mixed consumer source was observed here, but a general implementation must preserve mixed-use separation.

The seven no-read vertex groups currently account for16.803959s of vertex compilation; their11 pair links account for10.187050s. Those are existing measured costs in a potentially affected subset, **not a savings estimate**. The driver may already eliminate unused values, specialization can change compiler decisions, and this does not address the35 retained read/unknown pair links. No claim that it resolves the whole burst is justified.

The offline check also confirms that all16 captured `cara_run_vertex` bodies reset their declared temporary/output registers, condition code and address registers before calling `exec_shader`. This supports independence of actual-vertex state from the preceding reference evaluation for these exact sources. It does not replace a general input/default-register, source-generation or physical-output proof.

## Existing implementation review

`FragmentUsesLightingQuaternion(regs)` in `gl_shader_gen.cpp` is conservative: lighting disabled returns false; otherwise it examines active TEV RGB and alpha operands for primary/secondary fragment lighting colors, skips true pass-through stages, and handles Dot3_RGBA's combined alpha semantics. `SetupVertexShader` uses it only with packed input modes below3. Its cached answer starts true and is recomputed when `shader_dirty` is set. The TEV configuration words and lighting.disable register paths mark that state dirty. Uniform constant changes alone do not alter the dependency; any newly added shader-consumer or register path must maintain this contract.

When eligible, the existing generator's `omit_triangle_quaternion` path executes the unchanged actual `cara_run_vertex(gl_VertexID)` and emits the same non-quaternion outputs, while omitting the extra triangle-reference evaluation and unused quaternion sign/write. Lit or unknown consumers retain the original reference/sign correction. The in-memory VS key already includes option0x4e515531 when omission is selected. The legacy experimental flag also changes archive identity and filename before decode. These are useful mechanisms, but its process-static flag is not an acceptable new automatic cross-device default.

The archive has generated sources but not complete PICA register snapshots. Therefore11 lexical no-read pairs are **not yet11 proven runtime helper admissions**. Before execution, the exact helper decision must be cross-checked against the actual selected fragment source/key. If a new generator form is unknown or the two disagree, retain the baseline GPU source and report rejection before any draw. Cache that independent source-consumer classification per immutable FS stage, not by rescanning GLSL on every draw.

## The single proposed change

Add a new explicit private opt-in, for example `adreno618_unused_quaternion_v1`, default false. Resolve it once per renderer/manager, before loading archives. Admit it only for:

- Android GLES Atlas, non-separable ordinary packed mode0/1/2; no guest geometry, feedback, native dispatch or alternative renderer route.
- Device/model/product GR0006 and vendor Qualcomm, renderer `Adreno (TM) 618`.
- Exact observed driver identity: `OpenGL ES 3.2 V@0502.0 (GIT@46d1463393, I43876eb25d, 1638292465) (Date:11/30/21)` and GLSL ES3.20. Unknown versions reject the new private policy.
- A proven no-read fragment-stage classification agreeing with the live PICA dependency helper, with mutable-state reset/input independence proved for the selected vertex generator form.

Pass this immutable policy separately from the legacy shared prune probe. Do not set the old global flag automatically, alter Performance Mode generally or touch any Mali UBO/layout/state/packed-fetch decision. Configuration request, effective hardware gate, eligible pair count, actual specialized source hashes and conservative rejections need direct Android diagnostics. A requested flag or zero eligible programs is not activation proof.

Give the whole archive a distinct algorithm/policy identity **before decode**, explicitly containing `adreno618-unused-quaternion-v1=0/1`, effective source policy and driver identity. Use separate policy filenames; preserve old archives. Keep the existing per-VS option and complete generated-source hashing so a vertex source used with both consuming and non-consuming fragments gets the correct distinct stage. Any optional compile retry must select/record the original complete GPU source; a retry cannot silently count as specialized activation. Fragment source bytes and interface declarations must remain exact. No new cache/source policy may be borrowed by Mali.

This is one specialization candidate. It does not include a separable-program retry, native geometry, new async fallback or another texture/queue change.

## Gates before one physical comparison

1. Source/component proof: generate baseline and specialized output from actual captured PICA inputs, not rewritten retail GLSL alone. Prove all PICA function bodies, actual input conversion, state resets, actual vertex evaluation, clip/position/color/texture outputs and interface bytes are unchanged. Independently check live dependency helper against exact FS consumers, including RGB/alpha operands, pass-through stages, Dot3_RGBA, lighting toggles, unknown grammar, missing/default inputs and mixed consuming/non-consuming uses of one vertex program. Reject partial reset or alias uncertainty. Include cache-policy mismatch and same-process renderer reconstruction tests.
2. Physical correctness first: same reviewed private binary and genuinely matching state/normal boot; GL error/status checks enabled, all required draws/transfers/timers retained. Capture exact guest-aligned color on both LCDs and depth/stencil where the fixture exposes it, plus selected CPU/timer/guest-file state. Host-scheduled screenshots alone are only smoke evidence. Exercise a lighting toggle and a real consuming pair so retained reference correction is witnessed. Stop on first discrepancy and preserve it; no automatic retry or suppression.
3. Matched timing only after correctness: JIT100%,1×, frame skip off, same inputs/guest ticks and every original draw/list/transfer/LUT/upload count. Use fresh-process OFF/ON/ON/OFF controls with explicit drift and source/compiler/firstdraw/native-wall/thread-CPU scopes. Source and compiler-work counts can legitimately differ by the reviewed specialization, but original guest rendering and state must match. Prove positive effective activation.
4. Keep first-play and warm timing separate. Empty Cara caches can be matched for each source policy while preserving the driver's unknown private cache; do not delete or label it cold. For warm runs, prepare corresponding policy0/1 archives on this same device, bind every source hash, and clone the same matching policy archive for each repeated arm. The two source policies cannot share a falsely relabeled archive. Report cold order/cache limits and require no warm regression or shifted queue waits.
5. Mali non-regression: a pure policy matrix must reject the new flag for Mali-G57 and every unknown device/driver, while existing UBO/layout/state/packed-fetch resolution and explicit overrides remain exact. Shared generator changes require the existing correctness coverage before any wider rollout; a G Cloud result cannot establish a Mali gain or permit a Mali default change.

## Why the async guard is not the candidate

The measured run has no shader worker. `ApplyIOSPipeline` needs `allow_software_vertices && HasReadySoftwareVertexProgram()` before it can use the programmable background path. The first exact fragment combinations lack that linked fallback, so the complete GPU program compiles/links on NativeEmulation. Removing the readiness condition would merely create a missing first-use path, an extra synchronous fallback build or a wait; it is not a proven optimization. Earlier exact-key preparation remains separate future work and must preserve every required first draw.

The numerical source audit is in `g-cloud-unused-quaternion-audit.json`; it contains hashes/metadata only. Full source bytes, raw captures and unsuccessful hypothesis files remain local. No candidate win, physical eligibility, exact pixel parity or implementation completeness is claimed.
