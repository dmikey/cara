# Borrowed RAM comparison for immutable texture snapshots

`CARA_METAL_BORROW_RAM_SNAPSHOT_COMPARE=1` is a default-off CPU-copy experiment.
The native renderer supplies an optional fourth lane-constructor argument,
`GuestRamView(PAddr, std::size_t) -> std::span<const u8>`, backed by an exactly
bounded contiguous `MemorySystem::GetPhysicalRef` region.

DecodeTexture consults the callback only under the existing RAM-only snapshot
admission: no tracked VRAM residency, eligible bounded cache tier, a valid copying
reader, no native authority anywhere in the complete source, and the original
pending-write ordering. A hit compares all source bytes, including padding,
directly against the owned cache snapshot. A miss copies the view into owned
scratch before publication, callbacks or GPU work. Empty, null, wrong-size,
throwing or absent views preserve the original reader fallback. Views are never
stored in a cache, captured in deferred work, or submitted to Metal. The callback
must be synchronous on the owner thread, must not reenter the lane or mutate guest
memory, and must keep its bytes stable until DecodeTexture returns or invokes
another callback. Cache keys, budgets, eviction and native-owner proofs remain
unchanged.

Run `python3 tests/run_metal_texture_snapshot_cache_test.py` outside benchmark
measurements. On the local Apple M3 Pro, eight fresh processes (borrow OFF/ON ×
FIFO/keyed × large cache OFF/ON) passed 100 production DecodeTexture GPU cases
under Metal API Validation. Unchanged hits invoked the copying reader once OFF
and zero times ON; ON invoked the view once. Cases cover full-byte mutations and
padding, key/format/alpha changes, FIFO/keyed and 64MiB large-cache eviction,
malformed/throwing/absent callback fallback, GPU-owned poisoned RAM, actual pending
raster output, source mutation before gated GPU completion, and command-queue
lifetime changes. The fixture uses CPU golden decoded pixels and actual GPU
readback; it does not claim throughput gains, every texture format, real driver
fault injection or GPU shader-validation coverage. Results are in
`work/portable-metal-checks/texture-snapshot-cache/borrow-results.json`.
