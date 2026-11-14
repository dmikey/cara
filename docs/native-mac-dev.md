# Native Mac development host

The explicit `CARA_MAC_SDL_DEV_HOST=ON` target selection uses `emulator/src/cara/native_mac/`
without replacing the existing Windows/Linux SDL source files or their default selection.
The build helper enables this selection. This separate SDL host runs the no-JIT core on macOS with keyboard/controller,
mouse touchscreen input, SDL CoreAudio and desktop OpenGL LCD presentation.
It is a development frontend, not the iPad shell or a complete replacement for
the old, incompatible SDL movie/network command line. Existing `tools/build.py
mac` probe defaults remain unchanged.

Install SDL2, libpng, CMake and Ninja (for example Homebrew packages `sdl2
libpng cmake ninja`), plus Apple command-line tools. Build in a separate directory
on a volume with room for the compiler output:

```sh
python3 tools/build_mac_sdl.py --build-dir /Volumes/Fast/CaraBuild/mac-sdl-dev
```

`--cmake`, `--ninja`, `--sdl-prefix` and `--prefix-path` support nonstandard tool
locations. Two jobs are the default/maximum for the development helper; its
compiler `TMPDIR` is inside the chosen build directory. Release optimization is
retained; `--lto` is optional. The helper builds only `cara` and audits the linked
binary/build graph for excluded CPU and host shader JIT facilities. SDL2/SDL3 and
libpng are intentional external developer libraries, so the iOS system-only
linkage audit does not apply to this host.

## First launch

Use a **fresh** data directory. `CARA_USER_DIR` is mandatory and must be absolute;
the frontend does not fall back to existing configurations or save data. ROMs
remain at their original paths. For a new directory, create
`config/sdl2-config.ini` with:

```ini
[Core]
use_cpu_jit=false
cpu_clock_percentage=100
[Renderer]
use_hw_renderer=true
use_hw_shader=true
[Audio]
output_engine=sdl2
```

Then launch the built binary:

```sh
CARA_USER_DIR=/Volumes/Fast/CaraBuild/mac-sdl-user/ \
CARA_CPU_BACKEND=fastinterp \
/Volumes/Fast/CaraBuild/mac-sdl-dev/bin/cara '/path/to/game.3ds'
```

Native Mac presentation defaults to swap interval 0. The independent core frame
limiter still enforces 100% speed; disabling display synchronization does not
change guest CPU clocks or hardware timing. This avoids an observed SDL/Cocoa
first-swap wait when display ticks are unavailable. Set `CARA_MAC_VSYNC=1` to opt
into display synchronization. `native-settings.json` records both the setting
and actual SDL swap interval.

CPU and host shader JIT are disabled at build and runtime. The developer host
keeps guest clock and maximum speed at 100%, with SDL audio enabled and audio
stretching disabled. Hardware renderer/shader choices come from the fresh INI;
set both false for the CPU software-rasterizer reference while keeping the GL
LCD presentation backend. The default controls include A/S/Z/X for 3DS A/B/X/Y,
M for Start, N for Select, arrows for the Circle Pad, and mouse-left for the
lower touchscreen. SDL input binding strings can also be supplied in the Controls section; this host has no controller-remapping UI.

Before constructing cores the host supplies ALU_OPERANDS=1, TICK_BUCKETS=1, CHAIN_BUDGET=1, CACHE_WAYS=8, CACHE_ENTRIES=32768, RAM_BURST=1 and AL_DISPATCH=1 using their `CARA_FASTINTERP_` environment names. Existing environment values, including explicit zeroes, are preserved. The manifest includes the effective environment and actual per-core census; historical private runs from before these defaults remain labeled separately. GLES-only tuning flags are not applied.

Startup writes `native-settings.json` with the actual settings and GL version,
and `native.pid` identifies this instance. The window title reports game FPS and
emulation speed. The audio-open log reports the real selected device, sample
rate, channels and driver; an opened audio device is distinct from an independent
physical listening test.

## Graphics constraints

Apple Silicon exposes desktop OpenGL up to 4.1. On the tested M1, ARB texture
storage is available, but ARB image-copy and `glCopyImageSubData` are absent.
The two native copy paths use existing framebuffer blits when image-copy is
unavailable. Every fallback checks framebuffer completeness and GL errors before
allowing validation. Color-to-color copies retain the existing legal framebuffer
conversion semantics; identical depth formats use nearest blits. Depth-format
or attachment-category mismatches, incomplete framebuffers and GL errors fail
before destination validation. Color/depth reinterpretation is not replaced
with an unsafe blit.

Desktop GL and iPad GLES have different vertex, depth-transfer and driver paths.
This host is useful for local core/CPU and native GPU development; its timings
are not iPad performance evidence. Native shadow-image capabilities are also
limited. Validate each title and required behavior before drawing accuracy or
speed conclusions. Ordinary GPU-resident framebuffer RAM can be stale:
independent CPU-LCD snapshots are authoritative only with the software rasterizer.

## Opt-in development controls

`CARA_NATIVE_DIAGNOSTICS=1` enables instrumented request counters, known-frame
captures and a fixed RTC seed (`946684801`) matching the iOS benchmark. Normal
interactive mode uses system RTC. Neither mode changes the guest CPU clock.

- `CARA_NATIVE_PRESS_A_FRAMES=480,720,...` and
  `CARA_NATIVE_PRESS_START_FRAMES=2520` emit six-refresh button holds after those
  completed VideoCore refreshes.
- `CARA_NATIVE_CAPTURE_FRAMES=120,480,1440,2400` writes PNG, RGBA and JSON from
  the in-process renderer screenshot API. JSON records exact refresh, ticks,
  guest PCs, requests, RTC mode and display metadata. It is not a desktop capture.
- In diagnostic mode, a bounded `native-command.txt` in this instance's data
  directory accepts one command: `a 6`, `b 6`, `start 6`, `right 120`, `left 120`,
  `up 120`, `down 120`, or `capture`. Holds are clamped to 1–600 refreshes. The
  file is consumed every 30 refreshes; wait for consumption before sending the
  next command, and use atomic file replacement to publish it.

These controls and counters add host overhead; use separate uninstrumented runs
for performance comparisons. Heavy VMSR/vector-register witness instrumentation
is a separate private patch and is not required by this production frontend.

## Compile-only CI

The separate `native-sdl` job installs SDL2/libpng, compiles this target with two
jobs and retains the no-JIT audit and configure command. It does not launch a ROM
or require a logged-in GUI. It verifies frontend compilation and linkage; GPU
pixels, interactive input and sound still require a local run. The existing mac
probe build and its software-presentation defaults remain separate.

## Native CPU profiling

For a separate diagnostic run, attach Time Profiler to the already-running Mac
process. This tool never launches or restarts Cara:

```sh
python3 tools/profile_cpu_stacks.py --local-pid PID \
  --binary /absolute/path/to/the/running/cara --duration 20 \
  --output /Volumes/Fast/CaraBuild/profiles/new-capture
```

The output directory must be new. The tool archives the executable, records its
hash and Mach-O identity, checks the process identity before and after capture,
and requires matching image identity in target-process trace rows. Individual
weighted leaf addresses remain available even when many share `ExecuteBlock`.
Only Time Profiler data is accepted; CPU-counter cycle weights are rejected.
An optional `--source-fingerprint` records a caller-supplied value and labels it
as unverified. Profiling perturbs execution: keep these captures separate from
matched performance comparisons. Parser and capture-command tests pass. A live 20-second M1 capture also passed
process and executable identity checks; see [the verified capture](host-pc-sampling.md#native-m1-time-profiler-verification-2026-11-13)
for results and interpretation limits.

## Opt-in checkpoint and measurement controls

`CARA_NATIVE_DIAGNOSTICS=1` enables developer commands in the private user
folder's `native-command.txt`. Write one command per request; the host consumes
and removes the file at its next 30-refresh polling point. Existing `a`, `b`,
`start`, `left`, `right`, `up`, `down` commands take a refresh duration (1–600).
`capture` requests a renderer screenshot and may add GPU readback cost.

Additional commands are `save`, `load` and `profile`. Save/load use slot 1 in the
selected user directory and can replace that slot: use a dedicated development
user directory and preserve any desired earlier state. Requests are **queued**
by event polling and consumed only after `System::RunLoop` returns, while the
GL context remains alive. This avoids serializing inside an active VBlank
callback. `native-state-status.json` records completed operation, slot, actual
VideoCore frame, build fingerprint and current timer ticks. Normal state title,
platform and fingerprint checks remain active. `CARA_NATIVE_LOAD_SLOT=1` loads
slot 1 once after the ROM loads; it also requires diagnostic mode. Create a new
checkpoint under the executable being compared; never bypass a fingerprint
rejection. These controls change no guest clock or scheduling policy.

`CARA_NATIVE_PROFILE_SNAPSHOT=1` additionally writes read-only JSON snapshots
after startup, on the `profile` command, and at benchmark boundaries. It requires
`CARA_NATIVE_DIAGNOSTICS=1`. Snapshots include effective Legacy/New 3DS mode,
per-core clock scale/divisor, captured ARM/VFP registers, cumulative block census,
actual `current_timer_ticks`, thread CPU and monotonic wall clocks, and selected
existing counters. Legacy-mode `global_ticks` can be zero; use
`current_timer_ticks` for progression. Census execution counts cover retained
blocks and can reset on eviction; they are not interval CPU time.

Set `CARA_NATIVE_TIMING_COUNTERS=0` to disable existing timed scopes/counters
while keeping diagnostic input and snapshots. Disabled counter values do not
measure work; in particular zero draw counters are not proof of zero draws.
Snapshots explicitly report counter and requested script modes, and do not
perform a GPU readback. A screenshot receipt reports `capture_readback:true`.
No output-underrun counter or physical sound-smoothness proof is supplied.

For a bounded comparison, `CARA_NATIVE_BENCHMARK=1` starts a refresh-relative
window after startup/state load. `CARA_NATIVE_BENCHMARK_WARMUP` defaults to 300;
`CARA_NATIVE_BENCHMARK_FRAMES` is the exclusive measurement end (default 1200),
not an additional frame count. Both are bounded to 100000, with warmup strictly
below end. The host records before/after snapshots and captures the next refresh
outside the measured interval, then requests exit after the capture completes.
Use an external timeout as well in case the game or driver stops progressing.

There is **no built-in movement route**. Optionally set
`CARA_NATIVE_CIRCLE_SEGMENTS='360:480:-1:0;600:720:1:0;840:960:0:1'`.
Each segment is `start:end:x:y`, inclusive start/exclusive end, with digital axes
-1/0/1 (positive y is up). Up to 16 ordered, nonoverlapping segments are accepted;
invalid input fails before running. Normal keyboard diagonal normalization is
retained. A/Start/capture frame lists remain absolute VideoCore frame controls;
the circle route alone is relative to benchmark startup. Do not mix manual
state loads or unrelated inputs into a matched benchmark.

For example, with a same-build slot-1 checkpoint already in a dedicated folder:

```sh
CARA_USER_DIR=/absolute/path/to/development-user \
CARA_NATIVE_DIAGNOSTICS=1 CARA_NATIVE_PROFILE_SNAPSHOT=1 \
CARA_NATIVE_TIMING_COUNTERS=0 CARA_NATIVE_LOAD_SLOT=1 \
CARA_NATIVE_BENCHMARK=1 CARA_NATIVE_BENCHMARK_WARMUP=300 \
CARA_NATIVE_BENCHMARK_FRAMES=1200 \
CARA_NATIVE_CIRCLE_SEGMENTS='360:480:-1:0;600:720:1:0;840:960:0:1' \
/path/to/native-build/bin/cara /absolute/path/to/owned-game.3ds
```

For each compared mode preserve the executable hash, fingerprint, checkpoint
hash, complete launch environment, boundary profiles and final image. Derive
clock deltas from the before snapshot's `snapshot_complete_*` clocks to the
after snapshot's initial clocks to exclude the bulk of the before snapshot's
formatting/output. The final completion-record write and stream close remain
observer overhead.
The interval includes normal driver waits in wall time; thread CPU excludes
other threads and GPU time. Final image readback is outside it. Report warmup,
measured refreshes and remaining observer activity explicitly. Pixel/register
parity does not substitute for full guest-memory, hardware or audio validation.

The bounded route parser has a ROM-free compiler test:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_native_mac_controls.py
```
