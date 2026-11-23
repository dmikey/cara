# RG Slide Mali performance handoff

This handoff covers the measured Android GLES Atlas stack and shared
fragment generator changes. Re-establish the benchmark baseline when
integrating changes from main.

- [Renderer and profiling options](../rg-slide-rendering.md)
- [Cold and warm shader census and trace data](shader-census.md)
- [GLES validation, Apple parity audit and separate-stage measurements](gles-validation.md)
- [Active lit indexing and fog transfer measurements](lit-index-followup.md)
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

1. The unlit indexed path cut Mali position requests per 1,000 primitives
   by 28% and tiler-active cycles per input primitive by about 30%. The
   active lit Shader-topology probe also reduced position requests, with
   higher emulation-thread CPU time and no established frame-time gain.
   Keep that probe opt-in and verify color and depth if revisiting it.
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
warm output and timings. The corrected opt-in GLES 3.2 separate-stage
path reached the title and lab and reduced the measured cold stalls.
Vertex-stage link accounts for most remaining stage cost. Unused TEV
operands did not affect any of the 95 matched Dark Moon shader keys.

Both prototypes and the shader census remain opt-in where applicable.
Keep Apple and Android using the same semantics in the shared renderer.
