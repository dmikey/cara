# Zakuro and 3dsrecomp: read-only comparison

Reviewed 2026-11-13, pinned to Zakuro
`7e4e1223c06710f2e9e009652eed025dc1d1b160` and 3dsrecomp
`6746f50bdce75c058005b2fb5d0404413f4bef9d`. No source was imported and neither
project nor generated game code was executed. No comparative performance or
retail-hardware accuracy test was performed.

## Useful CPU ideas

[3dsrecomp local-state lowering](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/src/codegen/locals.rs#L477-L537)
keeps state local and synchronizes dirty/live values at explicit boundaries.
This suggests reviewing redundant FastInterp state traffic and precompiled
superinstructions, not loading game-specific native code into Cara.

[Memory-span hoisting](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/src/codegen/hoist.rs#L1-L7)
shares guarded page resolution for stable-base accesses. Its
[ABI contract](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/docs/interface.md#L230-L256)
requires direct-page mappings to remain stable through the applicable interval.
Cara must preserve GPU ownership transitions, MMIO and page invalidation; existing
RAM-burst/contiguous-transfer work already covers some of this opportunity.

[Short-slice differential verification](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/src/verify.rs#L318-L363)
is useful prior art for boundary tests. It uses another interpreter as reference,
not hardware; its comparison tolerates NaN payload differences in places where
Cara requires exact bytes. Its budget counts instructions at block boundaries,
not calibrated ARM cycles, and can charge an early-exited block fully. See the
[budget contract](https://github.com/fearkov/3dsrecomp/blob/6746f50bdce75c058005b2fb5d0404413f4bef9d/docs/interface.md#L350-L366).

## GPU path and timing

Zakuro's [Vulkan shading path](https://github.com/fearkov/zakuro/blob/7e4e1223c06710f2e9e009652eed025dc1d1b160/crates/zakuro-gpu/src/raster/hardware.rs#L3441)
stages raw attributes in buffer rings and uses indexed draws. Its draw path still
issues individual vkCmdDrawIndexed calls; it is not evidence of general draw
fusion. Explicit command recording, storage-buffer inputs and fenced ring reuse
are relevant architectural ideas, but not GLES drop-in changes or measured Cara
gains. Cara's existing transform-feedback and lit-pair attempts must not be
repeated without addressing their measured transfer/allocation costs.

[Framebuffer backpressure](https://github.com/fearkov/zakuro/blob/7e4e1223c06710f2e9e009652eed025dc1d1b160/crates/zakuro-core/src/services/gsp.rs#L382-L421)
holds the GX ring when a pending framebuffer changes before refresh. Its comment
motivates this with Monster Hunter 3 Ultimate producing many pictures per refresh.
P3D/PPF delivery otherwise follows synchronous submission/transfer handling. This
is a pacing policy, not measured GPU execution latency, and does not explain
Cara's observed Stories windows with equal command-list/completion counts and one
top swap per refresh. Do not adopt it as a proven hardware rule.

A source-review concern remains: the software rasterizer explicitly aligns
triangle quaternions to the first vertex, while the inspected GPU vertex path
emits them directly. This is a candidate equivalence gap, not a demonstrated bug.
Cara must retain its exact quaternion and raster fixtures before adopting any
ordinary indexed lit-vertex scheme.

## Guest-code analysis for Stories

An isolated ARM disassembler was used on the instruction bytes already present
in Cara's private Slot 2 FastInterp resident-block census. No guest instructions
were executed by that analysis. The highest-count entries include a 32-byte
multi-register copy loop, floating-point dot-product/comparison work, bit/count-
leading-zero helpers and short returns. This provides concrete candidates for
function recovery and call-site attribution, but does not prove their identities
or CPU-time shares. The census is cumulative and excludes evicted blocks; it is
not a sampled profile. Private disassembly is not repository content.

Next useful evidence: attribute sampled execution to guest PCs and their callers,
recover containing functions, and correlate GPU command construction with GX
submission records. Distinguish memory copies, transform/culling math and input
polling using actual dataflow rather than names inferred from instruction counts.
Preserve current guest timing and all observable memory effects in any optimization.
