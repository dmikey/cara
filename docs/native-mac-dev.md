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
