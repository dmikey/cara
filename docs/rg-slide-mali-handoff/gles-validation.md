# GLES validation on RG Slide

The reference target is the physical RG Slide with the Mali-G57 r40p0
driver. Benchmarks use the same ROM, renderer, guest clock, and cache
state in both arms. For Dark Moon, collect the title, opening and fog
transition, and early and later Gadd lab windows separately. The accepted
comparison uses Dynarmic, GLES Atlas, 1× resolution, and fixed CLK 100%.

Android Atlas accepts a saved legacy separable-shader preset through the
linked GLSL ES program path. The legacy separable generator emits desktop
extension and interface declarations, and Atlas updates vertex uniforms
on linked programs. The opt-in GLES diagnostics can be enabled separately
with `[Debug] trace_gles_debug = True` and collected using
`adb logcat -v epoch -s CaraGLES CaraPerf`. The renderer uses core debug
output or KHR_debug, depending on the context. It logs API errors and
undefined-behavior messages reported by that context. Leave this option
off for final timings. Device runs cover the paths exercised during that
run.

Preserve guest depth, stencil, sampling and ordering when testing any
performance change. Match draw counts, compare output, and keep p99 and
worst refreshes alongside guest FPS and SPD. The fog transfer trace is
documented in [the phase report](lit-index-followup.md).

## Mali separable-program experiment

With `[Debug] atlas_separable_programs = True`, Android Atlas can link
GLSL ES 3.20 stages individually on the Mali GLES 3.2 route. This uses
matching explicit shader input and output locations and writes packed
vertex-layout uniforms to the vertex program. Pipeline objects update
only the stages that change. The default uses linked programs and its
existing program-binary cache. The separable experiment keeps stages in
memory during a session and does not yet archive them between launches.

Compare both configurations on one build from independent empty Cara
caches with the driver cache state recorded. Collect emulator-thread
compile and link wall time, first draw and visible output alongside
startup latency, p99, and worst refreshes. Verify cold and warm behavior
before considering wider adoption.

## Bounded Mali binary archive experiment

`[Debug] atlas_extended_program_cache = True` selects a separate binary
archive on Android Atlas with a Mali renderer. The experiment can record
up to 256 pairs and 64 MiB. The existing archive defaults remain
128 pairs and 32 MiB. Both modes check shader/driver identity, source and
binary size, and the checksum before submitting saved binaries to GLES.
Use `trace_gl_shader_census` to record the restore count and binary hits.
Compare a late lab checkpoint beyond the first 128 programs, and check
checkpoint cost, title startup and shader-related p99 before promotion.

## Linked-program validation on the release APK

Android ARM64 build `7a0d1bc` ran the legacy separable preset using the
default linked-program path. A cold normal boot reached the title in
105.29 s and continued through the opening into Gadd's lab. During this
route the renderer context's GLES debug observer emitted no error or
undefined-behavior messages. The shader census recorded no failed compile
or link status. Syntax checks compiled the 16 generated logic-op fragment
variants for GLSL ES 3.00 and 3.20. The old CopyInverted expression was
rejected by the compiler. A host fixture also checked that a rejected
shader with no driver log is deleted and that failed creation cannot
reach source upload.

A restart seeded from the exact APK restored 128 program binaries and
reached the title in 21.51 s. Warm Gadd medians were 28.87–29.62 ms,
p99 was 33.86–50.90 ms across four windows, and the worst refresh was
53.46 ms. The lab HUD showed 27 game FPS at 55% SPD, CLK 100%.
An attempted restart with a seed from an earlier shader signature was
discarded from the comparison. The original installed APK and profile
were restored and the checked files matched.

## Emulation versus graphics

An Android presentation context already handles the window swap. It does
not own PICA draw preparation. A draw-worker probe needs owned draw state
and inputs, in-order submission, and measured joins for guest-visible
reads, fills, transfers and save/restore. The existing Mac command queue
is a reference for API-level ordering. Its GL context ownership and
global entrypoint hooks need independent validation with Android EGL
presentation and the shader worker.

The current memory layer can intercept writes to rasterizer-cached VRAM
and linear-heap mappings. Ordinary Dynarmic writes can use direct
page-table pointers, and GPU copies and fills use physical pointers.
Index-cache generation reuse needs a proof for every write path and alias
covered by the optimization. Start with a narrow watched range and log
write epochs alongside live comparisons before allowing a compare to
be skipped. Keep GPU work and PICA timing counters separate from
emulation-thread CPU and graphics worker CPU.
