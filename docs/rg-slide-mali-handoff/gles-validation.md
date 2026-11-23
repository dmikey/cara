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
