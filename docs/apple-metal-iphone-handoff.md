# Apple Metal and iPhone handoff

Copy the prompt below into a Codex session on the other Mac. Run it from
a checkout of [Cara](https://github.com/dmikey/cara). The verification
snapshot includes the private iPad Metal policy recovery at `cf7e9e6`,
the Android GLES and discovery work at `7818aff`, and the iOS-reference
display changes at `27e41ad`.

> Work on Cara's shared Apple Metal renderer and FastInterp, with iPhone
> 17e as the first physical phone target and iPad/iPhone as the reference
> frontend. Fetch GitHub `main`, confirm the revision and clean working
> tree, and work on a `dmikey/` branch. Read
> `docs/native-metal-build.md`, `docs/metal-queued-depth.md`,
> `docs/apple-atlas-luigi-2026-11-23.md`,
> `docs/rg-slide-mali-handoff/dark-moon-fog-lut-se3.md`,
> `docs/display-settings.md`, and the compressed ROM section of the README.
>
> Start with the documented build scripts. The public iOS path is GLES
> with FastInterp and no JIT. Use the private native Metal developer build
> for physical comparisons. On Apple Silicon, build the Mac Metal runner
> with `python3 tools/build_native_metal.py mac --cpu-jit` so the same
> runner can switch between FastInterp and JIT using `CARA_CPU_BACKEND`.
> Build the iOS developer app with `python3 tools/build_native_metal.py ios`
> and the shipping GLES control with `python3 tools/build.py ios`. Use
> `CARA_PRIVATE_IOS_METAL_ONLY=1` in the private iOS launch environment
> to select Metal. See the build guide for timing variables and signing. Keep signing secrets,
> ROMs, saves, and learned shader archives local.
>
> Use the locally owned Luigi’s Mansion: Dark Moon USA cartridge in
> Downloads. Run two gates. Boot normally through the first Gadd lab
> conversation for sustained speed, then replay the mansion entrance
> from a copy of slot 4 for cold and warm shader behavior. Use matched
> inputs and isolated copies of guest storage. Run FastInterp Metal
> against FastInterp GLES on the same device. On the Mac, add JIT Metal
> to isolate CPU cost and JIT OpenGL as the practical baseline.
> Start at 1× internal resolution, 100% guest CPU, 100% speed cap, sound
> on, and adaptive presentation disabled for the controlled tests.
> Keep the Performance Mode guest timing policy in both primary arms.
>
> Report emulation speed and guest FPS independently from host
> presentation rate. For each gate record per-refresh p50, p99, maximum,
> the slowest rolling 120-refresh window, startup and first-use stalls,
> emulation-thread CPU time, queue and arena waits, and any thermal or
> foreground-pause events. Compare guest ticks, PICA draws and matched
> pixels from both LCDs. Normalize Metal BGRA and GLES RGBA captures,
> their orientations, and their resolutions before a pixel comparison.
> Retest without HUD or detailed instrumentation after a traced run.
> Call an archive cold only at the level you cleared. OS file cache and
> driver cache state can still differ.
>
> Use these findings as baselines.
>
> - **Warm Metal.** The M4 iPad’s matched normal-boot Gadd tests reached
>   100% speed with adaptive Metal, at 17.15–17.27 ms lab p99 and
>   16.72 ms in the slowest 120-refresh window. The matched GLES control
>   was around 20.5–20.6 ms p99. A signed, HUD-hidden M3 Pro Mac run
>   reached 100.27% at 16.83 ms p99. Use these device and build results as
>   comparison points for the new machine.
> - **Queue and render-pass limits.** Preserve the shared Apple startup
>   policy, ordered depth receipts, and grouped Metal passes. The
>   validated M4 iPad starts at 32 tracked submissions with queued
>   pending depth export. The Mac starts at eight and can try 16 or 32
>   when evidence supports it. A larger queue can move a wait into the
>   upload arena. In one private M4 warm A/B, pass grouping reduced
>   mean time from 36.593 to 16.858 ms and cut actual render passes
>   from 1,521,894 to 38,708 with matched draws and captures. Recheck
>   memory and thermal pressure on the iPhone before changing its
>   defaults.
> - **Cold Metal.** At the 100%-paced iPad Gate, the compatible StageIn
>   fallback reduced refresh 18 from 610.94 to 46.33 ms. First refresh
>   still took 6.81 s and a later 271.02 ms hitch at refresh 2 consisted
>   of three pipeline creates, a fragment library, and translation.
>   The learned-cache repeat started in 31.64 ms. Keep vertex MSL
>   reuse, exact pipeline keys, descriptor deduplication, and bounded
>   on-device warmup. Time translation, vertex and fragment library
>   build, PSO creation, first draw, and GPU submission separately.
>   A general fragment or CPU vertex fallback must preserve PICA
>   rasterization, lighting, texture, depth, stencil, and blending.
>   Kirby benefited from adaptive runtime TEV admission. The
>   always-on version regressed Dark Moon and Zelda in cold runs.
>   Resident-only StageIn reuse covered only one of 28 eligible
>   busy-worker draws in a Kirby Mac probe. A separate private iPad
>   startup trace recorded a 13.164 s refresh at 531, with overlapping
>   vertex, fragment, and PSO build spans. The driver cache was
>   uncontrolled in that run.
> - **FastInterp and guest timing.** Profile guest ARM/Thumb separately
>   from the PICA command dispatcher, memory and attribute prep, shader
>   translation, uploads, and GPU waits. Keep the experimental fused
>   load/compare/branch path off unless a fresh controlled A/B earns
>   promotion. A GPU submission worker may speed up host execution
>   while the emulated guest completion schedule stays on guest ticks.
>   Include Monster Hunter Stories as a timing regression check.
> - **Phone evidence.** The iPhone SE 3 is an A15 GLES reference,
>   separate from the 17e. In an uninterrupted SE 3 Gadd comparison,
>   upload reuse avoided about 99% of upload bytes and moved mean
>   refresh time from 23.74 to 21.72 ms at a roughly 52 MB footprint
>   increase. The isolated LUT study measured only about 0.31–0.33 ms
>   per refresh in uploads during Gadd. Investigate vertex prep and
>   submission before importing the Mali LUT workaround. Reproduce on
>   17e and measure the phone's working-set budget and thermal state.
> - **Frontend and files.** Keep iPhone portrait and landscape controls,
>   bottom LCD touch, library and pause settings, and private suspend
>   checkpoints working. Retest Files import and kill/relaunch resume
>   using disposable saves. Preserve the distinction between 2× Native
>   Resolution and Lossless output scaling. The physical 17e previously
>   loaded Ultra Moon to language selection in 18.41/17.87 s from
>   LZMA2 7z and 9.29/7.80 s from ZIP under matching app trials. Measure
>   archive indexing separately from renderer time and compare raw
>   with indexed ZIP before attributing a tail difference to Metal.
>   Keep one original cartridge and the lightweight seek index where
>   the archive path supports it.
>
> Prioritize the exact cold fallback coverage and largest queue or
> foreground waits that the new device trace identifies. Make
> content-agnostic changes in the shared core with platform adapters at
> the boundary. Use one measured change per A/B. Verify both gates
> and at least Kirby, Stories, and an iPhone suspend and resume.
> Put exact commits, device/OS, test conditions, commands, traces,
> p99 and worst windows, pixel checks, and rejected experiments in
> the docs. Push the branch and mainline validated changes.
>
> Treat old Apple snapshots and experimental Metal retile branches as
> research references. Compare their changes with current main before
> porting a measured improvement. The renderer, FastInterp, and
> frontend can have independent compatibility requirements.

## Source records

- [Metal build and measured scene replays](native-metal-build.md)
- [Queued depth validation and replay conditions](metal-queued-depth.md)
- [Apple GLES vertex evidence](apple-atlas-luigi-2026-11-23.md)
- [Phone LUT lifetime evidence](rg-slide-mali-handoff/dark-moon-fog-lut-se3.md)
- [Platform display settings](display-settings.md)
- [iPhone Files and library behavior](ios-library-redesign.md)
- [17e compressed ROM measurements](../README.md#play-compressed-cartridge-roms)
