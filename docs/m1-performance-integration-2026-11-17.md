# M1 performance integration, November 17, 2026

This integrates the source used by the isolated M1 performance campaign into the main tree. It includes the Mac atlas path, render-worker queue and owned payloads, checked GPU surface remapping, shader cache, full GPU shader generation, native File > Open, diagnostic source capture, host API latency summaries and transaction attribution. Existing main save-archive relocation and networking/UI changes are retained. Installed apps and user data are not modified by this source integration.

## Availability and defaults

The native atlas frontend is an explicit build option, `CARA_MAC_ATLAS_PIPELINE=ON`, now available through `python3 tools/build_mac_sdl.py --atlas`. The ordinary build default remains OFF. Neither CPU nor shader JIT is required. The shared core preserves original GPU reference and actual vertex work, quaternion correction, checked error/FBO queries, synchronization, uploads, saves and guest hardware timing.

The SDL-supported `SDL_MAC_OPENGL_ASYNC_DISPATCH=1` default is set before frontend initialization. It prevents the observed render-worker/AppKit synchronous dispatch deadlock while main waits for the render queue. Explicit environment overrides survive. This addresses that specific deadlock; later Apple shader-driver stalls remain unresolved.

`CARA_NATIVE_SHADER_CACHE=1` opts into the source cache. Cache identity includes source/configuration policy and the generator signature; the observed driver exposes no binary program formats, so source sharing rather than binary restoration supplies the measured startup reduction. Corrupt or incompatible entries do not bypass identity checks.

`CARA_NATIVE_SHADER_SUFFIX_SHARING=1` additionally requires `CARA_NATIVE_DIAGNOSTICS=1`. It remains OFF by default. The conservative generated-subset helper emits an exact existing suffix call only after the full shader and input observation have been generated. Unsupported or ambiguous input returns the complete original source. Inverse expansion must recover the complete original function bodies and unaffected bytes. This is source restructuring, not permission to remove vertex work or change arithmetic.

The rejected 256-command queue and relative-dispatch experiments are not introduced by this integration. Existing unchecked producer recording, narrowed Blit scopes, compact records and other research paths remain explicit opt-ins. The queue default remains 64 commands. Mainlining code does not promote a rejected experiment or enable diagnostic capture during ordinary play. Host API spans are measurements; they never supply guest ticks.

## Retained results and limits

| Change | Retained result | Interpretation |
|---|---|---|
| Native checked surface remap | Luigi checkpoint 28.411711 → 16.650617 ms/refresh, selected parity passed | About 41% less elapsed time at this checkpoint; not universal game performance |
| Shader source cache | 48 duplicate startup stage compiles removed; one prewarm 4.437 → 3.310 s | Startup observation; warm comparison 17.200383 → 17.217507 ms/refresh showed no gain |
| Larger queue batches | 72.38% fewer batches; 17.108347 → 17.184416 ms/refresh | Rejected; default remains 64 |
| Narrow checked Blit state | 264,000 fewer inline state calls/1,200 refreshes; −0.146% elapsed difference with +0.213% OFF drift | Negligible; no promotion |
| Suffix sharing | 21 changed compiled VS variants, 16 linked; selected state, pixels, files and complete required queue/RPC/inline work matched | Correctness/incidence evidence for one window, not complete shader or whole-VRAM proof |
| Suffix first-observed discriminator | OFF/ON/ON/OFF first-draw totals 47.769/35.649/15.630/40.062 ms; OFF maxima 2.647/3.789 ms | Neither OFF reached the predeclared 20 ms pause threshold. Separation 4.413 ms was below OFF drift 7.707 ms; activation support absent. No speed or freeze-fix claim |
| Relative dispatch records | All 16 authored component means increased CPU cost, +3.79–10.24% | Rejected; no native promotion |

The suffix pair used the same frozen binary and a genuine matching-fingerprint checkpoint made by normal boot. The four first-observed cases used source capture OFF and the same host observer ON, with normal queries, sound, FastInterp, all four timers and 100% guest CPU. All cases exited cleanly and preserved both original app data directories and the seed. Driver-cache warmth was unknown; a fresh process or application cache OFF does not mean a cold driver cache. The observer records complete aggregate API counts/sums/histograms but its bounded slow ring cannot retain every source identity. These host spans are not physical GPU time or internal compiler-only cost.

The newly integrated tree has its own normal source fingerprint. Old experimental save-state headers must not be edited or reused across mismatched fingerprints. No renderer migration, diagnostic-clock promotion, dropped effects, CPU vertex fallback, dummy draw or driver-cache deletion is part of this integration.

The [retained shader comparison receipt](evidence/m1-shader-sharing-2026-11-17.json) preserves counts, histograms and measurements without captured game shaders or save data.

## Validation

The isolated campaign retained actual component, full-engine build, no-JIT object/symbol audits, genuine boot/save receipts and selected M1 game comparisons. This mainline integration receives a new native build and CPU checks because newer main save relocation fixes are preserved. The mainline M1 atlas build and no-JIT audit passed, along with 94 Python tests; the actual FastInterp differential, decoded-record (20/672), cache codec, shader policy, index-range, interval, relay-config, touch-bounds and six-pair save-relocation checks passed. These are compile/CPU integration checks, not a repeat of the closed game experiments or a claim that the integrated binary has completed physical gameplay. The native audit permits only the two source-checked file-backed RW journal mappings in an explicit atlas build; JIT objects/symbols and executable-permission APIs remain rejected. Ordinary builds retain the existing stricter mmap rejection.

`cara-shader-suffix-sharing-self-test` runs authored positive and conservative rejection cases without a GL context or captured game code. Existing source-cache, index/range, interval, interpreter and save-relocation checks remain applicable. Diagnostic observers and benchmark runners are not ordinary product UI.

Build `cara-gl-host-latency-reader` for the acquire-load journal reader. `tools/gl_host_latency_summary.py` supplies the strict post-exit raw/header/histogram validator and predeclared decision screen. `tests/test_gl_host_latency_summary.py` uses synthetic journals only; it does not manufacture real-driver performance evidence.

File-backed pipeline and latency observers are compiled only in the explicit native atlas build. Other products link unavailable/no-op implementations, even if those environment variables are set; no diagnostic mmap is added to ordinary or iOS binaries. The unavailable-path CPU fixture exercises every public observer method and verifies no directory is created.

The 146 GL entrypoint wrappers and RPC enumeration are reproducible with `python3 tools/generate_gl_command_queue.py`. Its `--check` mode compares both committed outputs byte for byte without writing files; CI runs that check.
