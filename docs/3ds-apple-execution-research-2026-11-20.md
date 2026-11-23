# 3DS execution on Apple Silicon: targeted source research

Reviewed November 20, 2026. This is source research supporting the measured
[Metal/FastInterp parity investigation](metal-fastinterp-parity-2026-11-20.md),
not another performance result. No external code was imported, no recompiler was
run, and no ROM, generated game code or private shader capture was uploaded.

## Three different meanings of recompilation

| Path | Actual work | Relevance to Cara |
| --- | --- | --- |
| FastInterp | Decode guest instructions into data records; dispatch those records to already compiled host handlers | Cara's general ROM execution path. An ARM64 handler implements guest semantics; it is not a newly emitted native translation of a particular game |
| Static game recompilation | Recover guest functions, emit C, compile a title-specific native library or executable | Useful analysis and verification research. Its native performance does not predict a no-JIT interpreter's speed |
| PICA shader translation | Decode the GPU program and configured fixed fragment pipeline, generate host shader source and compile it for the host GPU | Cara already generates GLSL, uses glslang for SPIR-V, then SPIRV-Cross for Metal source. This GPU pipeline is separate from guest CPU interpretation |

The original console uses ARM11 MPCore application CPUs and VFPv2; New 3DS adds
cores and a higher application CPU mode. The guest ARMv6K ARM/Thumb ISA is
32-bit and distinct from the Apple host's AArch64 ISA. ARM11's instruction and
exception model must remain the reference even when a host handler uses an
AArch64 arithmetic operation. Sources: [3dbrew hardware research](https://www.3dbrew.org/wiki/Hardware),
[ARM11 MPCore technical manual, integer-core overview](https://documentation-service.arm.com/static/5e8e1cd9fd977155116a4a7a),
and [3DS ARM documentation](https://lioncash.github.io/ARMBook/about_the_3ds.html).

## Newly checked projects and immutable identities

GitHub API commit identities were checked directly; search-engine snippets
sometimes described older interfaces than the pinned sources.

| Project | Reviewed revision | What changed in the research picture |
| --- | --- | --- |
| [3dsdecomp/RedPepper](https://github.com/3dsdecomp/RedPepper/tree/6bd828b729f2442d18e7a586b6858a5fc6aeb6cd) | `6bd828b729f2442d18e7a586b6858a5fc6aeb6cd`, May 22, 2025 | A Super Mario 3D Land EU decompilation, not a universal emulator or a Luigi decompilation. The organization also has SDK/library recovery projects. Function/layout research requires matching title/version/bytes |
| [fearkov/3dsrecomp](https://github.com/fearkov/3dsrecomp/tree/4b6de5d884b0d8bc337e16c07b84a0824e55a0e1) | `4b6de5d884b0d8bc337e16c07b84a0824e55a0e1`, November 17, 2026 | Distinct from the decompilation organization. The current pinned interface describes ABI 8, whereas the prior local review pinned `6746f50b` and indexed web pages still showed older ABIs |
| [fearkov/zakuro](https://github.com/fearkov/zakuro/tree/fdf67b29b6398a7ad0dc2309b09ea4c03c45e9d8) | `fdf67b29b6398a7ad0dc2309b09ea4c03c45e9d8`, November 17, 2026 | The companion AOT runtime, newer than the prior `7e4e1223` research pin. Recompiled-game results must not be represented as interpreter-only results |
| [nathsou/3dsrecomp](https://github.com/nathsou/3dsrecomp/tree/1649de2175fb5a83994917f8a9d78e44d27edbd9) | `1649de2175fb5a83994917f8a9d78e44d27edbd9`, November 15, 2026 | A separate project with an actual native Metal/iOS renderer. Its latest commit includes per-site indirect-call caches. This is a new comparison target, not the fearkov repository |

The nathsou repository declares EUPL-1.2; the previously reviewed fearkov projects
declare MIT. Research attribution is not permission to combine source without
checking the actual file licenses and Cara's distribution obligations. No source
was copied here. See [the earlier pinned comparison](zakuro-recomp-prior-art.md)
for the existing findings rather than treating this as a replacement provenance
record.

## Concrete transferable mechanisms

**Bounded execution-state synchronization.** Fearkov's
[local-register lowering](https://github.com/fearkov/3dsrecomp/blob/4b6de5d884b0d8bc337e16c07b84a0824e55a0e1/src/codegen/locals.rs#L1-L15)
keeps guest state in host locals and synchronizes dirty values at calls, SVCs,
returns, interpreter transitions and budget exits. This suggests inspecting
FastInterp's actual ARM64 code for redundant state traffic at the same boundaries.
It does not authorize omitting a state store visible to callbacks or the scheduler.
[Span hoisting](https://github.com/fearkov/3dsrecomp/blob/4b6de5d884b0d8bc337e16c07b84a0824e55a0e1/src/codegen/hoist.rs#L1-L45)
reuses page resolution only within a region whose mapping cannot change. Cara's
GPU-tracked pages can lose direct pointers; any extension must preserve current
same-page, MMIO, ownership and callback barriers. Existing RAM bursts already
cover part of this mechanism.

**Per-site indirect target hints.** Nathsou's
[GuestIC contract](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/runtime/cpu.h#L38-L50)
checks the complete target, including the Thumb bit, plus a dispatch generation;
[generated indirect calls](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/tools/recomp/gen.py#L1037-L1052)
fall back to normal dispatch on a miss. Cara already has an exact-tag
`CARA_FASTINTERP_SECONDARY_LINK` experiment and invalidation tests. Do not add a
second equivalent cache or promote it without a matched gameplay gain. A hint
must never retain a recycled block as valid based only on its slot or pointer.

**A shared command-buffer arena.** Nathsou's
[Metal arena and command-list ownership](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/runtime/gpu/metal.mm#L467-L505)
retain upload chunks until their consuming command completes. Its
[grouped compute vertex work](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/runtime/gpu/metal.mm#L2021-L2100)
combines jobs by kernel while retaining draw records. These are ownership and
amortization ideas, not permission to merge unrelated guest visibility boundaries.
Cara already implements grouped/unique vertex compute; its measured title-window
result did not establish a win. The same source also skips unknown fragment-pipeline
draws while compiling, skips shadow rendering and documents loss of MSAA depth
subsamples between passes. Those policies are not equivalent-work optimizations
for Cara. Its hash-based ownership checks are not full-byte identity proof.

**General shader-flow specialization.** Nathsou's
[uniform dependency masks](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/runtime/gpu/hwshared.cpp#L197-L212)
select only the boolean and integer-loop uniforms that a program's flow reads.
Its [Metal kernel selection](https://github.com/nathsou/3dsrecomp/blob/1649de2175fb5a83994917f8a9d78e44d27edbd9/runtime/gpu/metal.mm#L1397-L1419)
can retain a generic GPU kernel while a specialized one compiles. A Cara prototype
would require exact admitted program/swizzle identity, entry/configuration and
all analyzed uniform dependencies, bounded variant counts and generic fallback.
This is a generalization candidate for the guarded PC230 experiment, not evidence
that PC230 results transfer to other programs or scenes.

The current fearkov [ABI description](https://github.com/fearkov/3dsrecomp/blob/4b6de5d884b0d8bc337e16c07b84a0824e55a0e1/docs/interface.md#L1-L36)
also adds shared tick/mutex interfaces. Those change how a runtime services guest
operations. They are not renderer optimizations and cannot be substituted for
Cara's scheduler/tick/SVC semantics merely to lower a measured CPU total.

## PICA200 is a different processor with different numeric rules

PICA has programmable vertex/geometry instructions, including address-register
indexing, calls/branches/loops, conditional control flow and geometry emission. The geometry
`SETEMIT`/`EMIT` behavior includes state and winding, so a simple independent-vertex
rewrite needs an explicit eligibility check. The documented numeric format is
24-bit rather than IEEE half; multiply special values, subnormal handling and
non-fused MAD behavior constrain algebraic rewrites. Sources:
[3dbrew shader ISA and hardware tests](https://www.3dbrew.org/wiki/Shader_Instruction_Set),
[the PICA pipeline presentation](https://www.highperformancegraphics.org/previous/www_2011/media/Papers/HPG2011_Papers_Kazakov.pdf),
and Cara's [hardware source audit](hardware-source-audit.md). Neither replacing
all floats with half nor enabling unrestricted fast math is justified.

Fragment lighting and the texture combiner are configured circuitry, which Cara
implements as generated fragment shaders. Light/LUT enable state and TEV inputs
are semantic dependencies for specialization. Textures use tiled addressing and
include ETC1/ETC1A4 formats; a native host texture with the same address is not
necessarily the same bytes or layout. Sources:
[GPU register and texture-format research](https://3dbrew.org/wiki/GPU/Internal_Registers),
[fragment lighting and quaternion research](https://3dbrew.org/wiki/GPU/Fragment_Lighting).
The lighting page explicitly leaves parts of hardware quaternion interpolation
uncertain. Cara must retain its existing per-triangle quaternion correction and
GPU oracle instead of inferring correctness from a faster upstream raster path.

GPU command packets write registers with masks, extra words and optional
consecutive addressing. IRQ enqueue, guest wake, host encoding, GPU completion
and presentation are different observations. See
[3dbrew command/register research](https://3dbrew.org/wiki/GPU/Internal_Registers),
[GBATEK FINALIZE/interrupt research](https://problemkaputt.de/gbatek.htm#3dsgpuinternalregistersfinalizeinterruptregisters)
and [Cara's retained frame-production traces](gpu-frame-production.md). These
sources do not provide a calibrated retail-console completion latency or justify
dropping work older than a display interval.

## Local analysis and acceptance order

Use [Capstone's offline ARM/Thumb interface](https://www.capstone-engine.org/lang_python.html)
for bounded instruction and operand inspection; retain explicit instruction mode,
address and exact bytes. Use [Ghidra's disassembly/decompiler and scripting](https://github.com/NationalSecurityAgency/ghidra)
to recover candidate callers, control flow and data structures from private
images. Inferred function names and extents need byte-identity confirmation.
Neither tool measures dynamic execution time or proves hardware semantics.
No new installation or game disassembly was performed for this review.

For the current 80–100% parity gap, first finish the matched Metal replay and
shared-policy measurements. Attribute actual FastInterp self work separately
from nested graphics/SVC ancestry. Then rank specific guest instruction patterns
using sampled PCs, the existing block census and exact workload identity.
[A cached interpreter precedent](https://dolphin-emu.org/blog/2024/09/04/dolphin-progress-report-release-2407-2409/)
can motivate authored precompiled fusion, but Cara already has decode operands,
AL entries, pairs, loops and chaining experiments. Preserve every original
instruction's ticks, memory effects and scheduling boundary. No new CPU cache,
fusion or shader specialization is accepted by this document.


## Local remote-branch audit: older dispatch experiments

Read-only Git-object inspection on November 20 found the following branches
outside local main. Their names do not identify their CPU backend: **all four
modify DynCom/ARMul_State, not FastInterp**. No checkout, merge or execution was
performed. Their trees predate `docs/hardware-investigation.md`; no physical M4
performance receipt was found in these changes.

| Branch and tip | Actual mechanism | Evidence and current relevance |
| --- | --- | --- |
| `origin/perf/dispatch-cache-4096`, [`a36ed5f`](https://github.com/dmikey/cara/commit/a36ed5f5d6d2d31148418926d44f6bfa0d3c8cb9) | Expands DynCom's generation-tagged direct decoded-data lookup from 1,024 to 4,096 slots | Authored collision test updated. This is not the FastInterp 16K/32K decoded-block capacity policy and is not a measured omitted M4 gain |
| `origin/perf/dispatch-victim-cache`, [`360b2e7`](https://github.com/dmikey/cara/commit/360b2e765467bd2fea8605f9ba47cd2c4e0d606b) | Adds 64 generation-tagged DynCom displaced entries ahead of its normal map lookup | Different cache architecture from FastInterp's NRU sets and successor links; no gameplay performance receipt in the commit |
| `origin/perf/dispatch-load-cmp-branch`, [`ffdaa0f`](https://github.com/dmikey/cara/commit/ffdaa0f1e324d29396eb05db17741236255e99c0) | Tries a checked direct `LDR` to `CMP` dispatch edge in DynCom, retaining the fallback | Bounded authored semantic coverage. Not a FastInterp handler/fusion implementation or evidence of a device gain |
| `origin/perf/dispatch-instruction-profile`, [`827b58b`](https://github.com/dmikey/cara/commit/827b58b26402b490c0d9a9dc61860a7bf6d7e607) | Adds opt-in DynCom instruction/within-block pair sampling every 251 dispatches | Diagnostic overhead, not an optimization; do not enable it for clean throughput comparisons |

The current shared Apple FastInterp defaults already select the documented ALU
operands, tick buckets, chain budget, eight-way cache, RAM bursts and AL entries,
with 32K capacity on devices with at least 6 GiB physical memory. Native Mac
builds currently use generic O3 without LTO. That differs from older iOS ThinLTO
experiments; a compiler-policy comparison needs a controlled rebuild and replay,
not a claim that the four DynCom branches supply the missing improvement.
The earlier private path referenced by the conversation's M4 notes is absent
in this checkout. The available main hardware-investigation document has 1,169
lines; its existence does not imply that an unavailable 3,832-line private Metal
report or its raw receipts were reviewed here.

## November 21: bounded immediate-load pair candidate

`CARA_FASTINTERP_LDR_PAIRS=1` is a default-off experiment in the existing
precompiled interpreter. It recognizes two unconditional ARM immediate word
loads with the same non-PC base, non-PC destinations, and a first destination
that does not overwrite that base. Both original decoded records, instruction
sizes, ticks and invalidation boundaries remain present. The runtime fast path
requires a complete two-instruction budget, ordinary direct RAM, one shared page,
two individually bounded four-byte reads, and no host bookkeeping alias. It
performs the original ordered loads and register writes, with independent u32
address wrap. Step, short budgets, tracked memory, MMIO, crossings and aliases
take the original scalar handlers. It emits no native guest code and changes no
shipping default.

The retained v8 host-PC capture placed 171 samples at the immediate-load
handler's decoded base-register selection. A separate census of the top 128
resident blocks in `gadd-v16-normal-warm-diagnostic/fastinterp-blocks.json` found
236,554,919 entry-weighted immediate-load slots and 12,459,618 eligible adjacent
same-base pair positions, of which 8,842,687 used consecutive word offsets.
These are resident-block entry weights, not retired instruction counts, time
samples or a projected speedup. They motivate testing this pattern instead of
another decode-cache change; they do not establish a useful performance gain.
The conservative runtime checks also add work and can outweigh the saved lookup
and dispatch.

An isolated binary recompiled both FastInterp implementation units and the
authored self-test without profile-generation/use flags; stale-profile warnings
were not suppressed. `tools/check_fastinterp_ldr_pairs.py` passed four processes
of 20,480 differential cases: pairing off/on in both chain-budget modes, with
identical register/flag/tick and budget-trace digests. Enabled runs selected 3,072
pairs and recorded 2,390/2,402 actual fast executions plus 6,802/2,165 fallbacks.
Coverage includes repeated destinations, the second destination overwriting the
base, conditional declines, PC/dependency admission, unaligned/page-crossing and
wrapped addresses, ordered MMIO and second-read exceptions with unchanged
callback-visible PC, tracked VRAM, Step, context restoration, mixed budgets and
second-word invalidation. These renderer-free cases verify coherent dispatch,
not an actual GPU flush.

Both 22,000-case ARM/Thumb regression runs passed. Additional existing suites
passed 328,448 metadata walks, 9,666 AL-dispatch cases, 2,496 PC/budget cases,
268 BL/SVC cases, 3,072 BLX cases, 136 LDM-PC cases, 1,024 cache/event slices,
and 4,224 scalar-double cases. The VLDR-pair checker also passed four 81,920-case
processes with LDR pairing and ALU pairs enabled. Local receipts are retained
under `work/apple-cpu-bridge-checks/ldr-pairs/`; source compilation and link
commands identify the isolated diagnostic binary. No ROM or main application
was run for these tests.

Adding the handler changes `ExecuteBlock`'s compiler-profile identity. A new
IR-PGO generation/use cycle must use matching source and compiler; the old
profile must not be reused by disabling mismatch diagnostics. Train the selected
ALU policy: the earlier gameplay profile was collected with ALU pairs disabled,
while the current best experimental configuration enables them. Training both
LDR modes from the same source can avoid making the new handler artificially
cold in its eventual comparison. Gameplay performance, normal-boot behavior,
other-title coverage and Apple-device benefit remain unproven until controlled
same-binary measurements with exact output and guest-state checks.
