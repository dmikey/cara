# Native M1 decoded-layout measurement (2026-11-13)

The optional 16-byte decoded-record prototype reduced emulation-thread CPU time
by **0.5591 ms per refresh (1.13%)** in this matched native M1 Stories bracket.
This is a small local signal, below the requested 2–5 ms target. The option stays
**default-off**. This measurement does not establish an iPad/M4 improvement or a
cross-title result.

The storage implementation and independent validation are described in
[the decoded-layout contract](fastinterp-decoded-layout.md).

## Source and build identity

Both real native SDL applications were built from the same clean private source
`6e86769`: prototype `d77304e`, accepted native diagnostic controls, narrow native
self-test target exposure, and the one-line DSP status initialization fix
(equivalent to `436cff8`). This is a **private variable-isolation basis, not
latest main**. It deliberately excludes the later main LUT initialization fix.
The documentation integration base is separate from the measured source.

Both use optimized Release `-O3`, generic CPU targeting, LTO off and no CPU or
shader JIT. CMake genuinely generated layout headers containing 0 and 1; no
compiled identifiers or state guards were edited. Both naturally generated
state fingerprint
`cd68ca8a6e5567d402e7c0642d9bf4441e496b9deb8c1fa894c001605841db24`.
The record witnesses confirm instruction/block sizes of 20/672 and 16/544 bytes.
All requested targets were genuinely built for each configuration; both default
22,000-case interpreter suites and native no-JIT audits passed.

| Native layout | Application UUID (arm64) | Application SHA-256 |
|---|---|---|
| 20 bytes | `7FF944E8-C7ED-3615-BA94-FE8184D52C97` | `a3ca5918fa2c8cd210d2bbbb5868e785821a77e7d1b80d7adedec8d988c27f76` |
| 16 bytes | `31AB518C-F24E-386F-9F53-DC6C4EFE7E1C` | `1d0c6aba690749f6d2321b0604fc12e531ad1824735e3310547017868728e219` |

## Scene and controls

The machine was an Apple M1 Macmini9,1 using native desktop OpenGL 4.1 Metal
91.7. Actual startup receipts required both **hardware renderer and hardware
vertex shader enabled**, CPU/shader JIT disabled, Legacy 3DS mode, 100% guest
clock and cap, swap interval zero, and SDL2 audio without stretching. A GL context
alone was not treated as proof that guest rendering used the GPU. All four core
records required the requested compiled layout, 32,768 cache entries / eight
ways, matching AL/chain/operand flags, and no absolute-PC or secondary-link mode.
Each timer reported scale 1, effective guest clock 100%, downcount hack shift 0
and divisor 1. VLDR pairs, Mac GPU depth reinterpretation and GL state reuse were
explicitly off in every run. No hardware timing was changed.

A freshly copied user directory initially lacked an explicit renderer INI and
selected software rendering. That failed preparation was diagnosed, stopped and
quarantined; none of its frames, timings or states enter this comparison. The
corrected launcher wrote an explicit hardware-renderer INI and checked actual
startup manifests immediately. Its guard also rejected 16 deliberately mutated
bad startup cases, including wrong renderer, clocks, candidates and mixed core
layouts. The launcher, guard and failed-startup fixtures remain private artifacts.

Copied Stories extra-data containers did **not** provide a usable ordinary save:
the actual Load Game menu said no save data existed. The accepted checkpoint
therefore came from **normal New Game**, normal A presses and an offered Start
skip, followed by opening dialogue. A renderer capture confirmed controllable
Secret Path gameplay with player and minimap, rather than a movie or dialogue
wait. A queued save executed only after `System::RunLoop` returned, at original
VideoCore frame **7860**, actual guest tick **35122655271**. The state was
24,549,432 bytes, SHA-256
`2ee8d2abe06bb9e7efe640bda8ce49cb2cd0c7fd7a0edf5f341835c4ebea618c`.
Loading normally reset the host video-frame counter to zero while preserving
actual guest ticks. Older states were not used or bypassed.

Each fresh process loaded that identical checkpoint through normal guards. The
same absolute runtime user path was retained for file-backend consistency, while
task-owned NAND, SDMC, sysdata and configuration were restored from one immutable
basis before every run. The sequence was **20 → 16 → 16 → 20**. Each run excluded
300 warmup refreshes and measured the 900-refresh interval **[300,1200)**. Inputs
were left [360,480), right [600,720), and up [840,960); other refreshes were idle.
The final renderer capture occurred at 1201, outside the timed interval. The
endpoint was still Secret Path gameplay, with the player partway up the stairs.

Timing/counter scopes and per-instruction guest-PC profiling were off. Input
handling and read-only boundary snapshots remained active, so these are not
fully uninstrumented runs. Deltas use the before snapshot's completion clocks
and the after snapshot's initial clocks, excluding most beginning snapshot I/O;
the final completion-record write/stream close remains observer overhead.
Cold surface/shader reconstruction received the same warmup. No compiler,
profiler or other agent test workload ran during the bracket.

## Results

Values are milliseconds per measured refresh. Thread CPU covers the emulation
thread; wall time also includes driver waits. Neither is a direct playback-FPS
measurement or a percentage of total host CPU allocation.

| Run order | Record bytes | Wall ms/refresh | Emulation-thread CPU ms/refresh |
|---|---:|---:|---:|
| Baseline A | 20 | 72.348572 | 49.653731 |
| Compact A | 16 | 71.319552 | 49.083315 |
| Compact B | 16 | 71.419239 | 49.138888 |
| Baseline B | 20 | 71.790644 | 49.686605 |
| **Baseline mean** | **20** | **72.069608** | **49.670168** |
| **Compact mean** | **16** | **71.369395** | **49.111101** |

The mean differences were **0.700213 ms wall (0.97%)** and **0.559066 ms CPU
(1.13%)**. Raw max-minus-min spreads were:

| Layout | Wall spread ms/refresh | CPU spread ms/refresh |
|---|---:|---:|
| 20 bytes | 0.557928 | 0.032874 |
| 16 bytes | 0.099688 | 0.055572 |

The closing baseline's wall time was 0.557928 ms lower than the opening baseline,
so wall drift is material beside the 0.700213 ms mean difference. Baseline CPU
changed only 0.032874 ms, and both compact CPU observations were lower than both
baseline observations. This supports a modest native M1 main-thread CPU signal
in this scene. There are only two observations per layout; this is not a broad
statistical or cross-game performance result and does not justify an iPad default
promotion.

## Parity and limits

At startup, refresh 300 and refresh 1200, all four runs exactly matched all 16
captured ARM registers, CPSR, FPSCR and 64 raw VFP words per core, timer settings,
actual guest ticks, host frame and HID endpoint state. At refresh 1200 the actual
tick was **40484890860**; final capture tick **40489359391** and final PCs also
matched. Full final RGBA bytes matched across all four runs, SHA-256
`72b9413006b62ed02fa8b84793fc9d65bedabf6ea293ddee423f09d5b7e7e58d`;
RGB-only bytes matched as well. This proves captured native-baseline parity,
not hardware pixel correctness or complete architectural state equivalence.

Separately, the generated-guest actual-SDL state gate verified 20→16 and 16→20
restoration and 30-refresh continuation against same-layout controls. Complete
156,063,547-byte decompressed archives and compressed payloads compared byte for
byte with zero ignored/normalized bytes after the common DSP initialization fix.
Both new applications normally rejected untouched older 703-fingerprint states.
That ROM-free state gate is distinct from the Stories timing run: **whole-game
RAM archives were not compared for these four game runs**.

Draw/instruction counts were not measured because counters/profiling were off.
Each actual startup log recorded an opened 32,728 Hz stereo SDL/CoreAudio device;
output-underrun counters and physical listening were unavailable. No auditory
smoothness or zero-underrun claim follows. Desktop driver behavior differs from
the iPad GLES path, and no physical iPad actions were taken.

Local evidence is retained under the `decoded16-20261113` profiling artifact
root: `native-build-receipt.json`, `cross-state-dsp-initialized/`, and
`stories-matched/`. The latter contains the immutable checkpoint receipt, scene
captures, private launcher/preflight and mutation fixtures, per-run launches,
actual startup guards, boundary profiles, raw final captures, logs,
`matrix-results.json` and `matched-summary.json`. No game assets or user data are
bundled in the repository documentation.
