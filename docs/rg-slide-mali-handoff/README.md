# RG Slide Mali performance handoff

This branch preserves the measured Android GLES Atlas stack and shared
fragment generator changes. The latest tested source was `53770ae`.
Base for the current Android results was `120623b`. GitHub main was
`9a99f46` when this handoff was written. Integrate main as a separate
step and re-establish the benchmark baseline after integration.

- [Renderer and profiling options](../rg-slide-rendering.md)
- [Cold and warm shader results, with trace data](shader-census.md)
- [Android ARM64 build at 53770ae](https://github.com/dmikey/cara/actions/runs/45142152732)

The trace files record experiment markers, GLES shader events, and 120-refresh
timing windows. Run `python3 docs/rg-slide-mali-handoff/summarize-census.py docs/rg-slide-mali-handoff/census.log.txt`
to summarize the shader census. The hardware counter capture is for the FBO
probe alone. Use a locally owned ROM and isolated profiles for the comparison.

## Resume warm profiling

Use the RG Slide T820/Mali-G57 r40p0 on JIT + GLES Atlas, Performance Mode,
1×, adaptive frame skip disabled, and the guest clock fixed at 100%. Boot
normally through title and save selection into Gadd’s lab. Keep the ROM,
display mode, shader cache seed, and settings matched across runs.

At fixed clock, the earlier exclusive CPU measurements in Gadd were
18.59 ms per refresh in PICA command lists and host graphics work,
5.49 ms in guest CPU execution and memory, 2.30 ms in SVC/HLE, and
0.80 ms elsewhere. Total was 27.18 CPU-ms per refresh. The latest
warm control HUD showed 27 guest FPS at 58% SPD. The target is 100% SPD,
roughly a 16.7 ms system-refresh budget, and a smooth game-specific
output cadence. Guest FPS and system refreshes are separate measurements.

1. Trace each rejection reason for lit indexed draws. The measured unlit
   indexed path cut Mali position requests per 1,000 primitives by 28% and
   tiler-active cycles per input primitive by about 30%. The earlier
   measured lit route accepted zero draws. Test the opt-in
   `atlas_lit_indexed_lists` route with `atlas_indexed_draws`, keep triangle
   order and the quaternion reference vertex, and compare pixels and depth.
2. Profile live guest index validation in `PrepareIOSVertices`. Prove
   invalidation across direct JIT writes, DMA/transfers, aliasing, and state
   restore before relying on guest-memory generations in place of byte
   comparison. Upload identity and source validity are separate.
3. Test captured draw preparation on a worker with immutable inputs.
   Preserve draw order and guest-visible completion across reads, fills,
   copies, and transfers. Compare emulation-thread CPU time as well as p99,
   worst refreshes, actual game FPS, and Mali vertex/tiler/fragment activity.
4. If investigating FBO reuse again, match GPU counter captures for both
   arms. The current probe saved about 0.16 ms per refresh in submission
   with both arms at 58% SPD. Keep the paired-FBO probe opt-in.

## Resume cold profiling

The regular GLES program path spent 104.57 s in link/status and 3.41 s
in stage compile/status on the emulation thread after save selection.
Known lit programs account for 89.18 s of that link time. The existing
single shared EGL worker builds only optional indexed variants. A pixel-
and depth-correct lit fallback for regular draws remains open. Reuse
the shared stage cache, fragment generator, and worker. Verify actual
overlap on Mali before expanding the worker count, and compare cold and
warm output and timings. The separable shader probe reached GLSL errors
before the title, and unused TEV operands did not change any of the 95
matched Dark Moon shader keys. Do not count either as a measured speedup.

Both prototypes and the shader census remain opt-in where applicable.
Keep Apple and Android using the same semantics in the shared renderer.
