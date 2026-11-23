# Dark Moon LUT upload lifetime on iPhone SE 3

This investigates whether Apple's OpenGL ES driver reproduces the
partial-texture upload wait measured on the RG Slide. The Android handoff
and its measurements are pinned to `ac8a372fb7e4ec25e20c458b32396c55eff99da7`.
The Android implementation leaves earlier draws on the old LUT texture,
binds a new RG32F image for subsequent draws, and retires old texture names
in batches. Apple GLES still uses the normal partial atlas update.

## Method

The diagnostic used a private bundle on a physical iPhone SE (3rd generation),
reported Apple A15 GPU and OpenGL ES 3.0 Metal 104.1. It used FastInterp,
1× rendering, Performance Mode, a fixed 100% guest clock, audio, and full
GPU vertex work. The deterministic benchmark disables adaptive presentation
and leaves all emulated rendering work enabled. The app booted the USA
retail ROM normally from separate disposable guest profiles.

The iOS executable used the previously verified generic ARM64 GLES archives
from `0a5ca6815bad32091ff94a1ce35508c711d575a3` and the iOS frontend overlay
from `9a99f46d1706e84013d14b3abe44546d2f8641d0`. The diagnostic compiled the
frontend and LUT upload helper, then relinked those archives. The pinned
Android branch was the comparison source for inspection. No Android Mali
worker or LUT behavior was backported.

The profile counts RG and RGBA payloads at the actual partial texture
upload call, with bytes computed from dimensions and format. One logical
update may require multiple row uploads. The diagnostic times actual
indexed draws, array draws, framebuffer blits, and explicit GLES wait
calls independently. The LUT off-CPU estimate is elapsed time minus
calling-thread CPU time, clamped at zero. It can include host scheduling
and cannot identify an Apple driver fence or internal GPU time. Direct
iOS GLES has no Cara GLES worker queue. Implicit waits may appear inside
other GLES entry points. Renderbuffer submission is a separate host scope.

The first 4,800-refresh route verified the smoky title at 1080 and the
file selector at 1800. Its last screenshot showed the lab introduction
cutscene. That workload is kept separate from the Gadd conversation.
The title and file selector windows contained no explicit shader compile
or link calls. This first launch started with an empty Cara shader archive.
Driver cache state was uncontrolled.

| Initial observation | p50 | p99 | Worst refresh | RG upload calls/refresh | RG bytes/refresh | RG upload ms/refresh | Largest RG call | Array draw ms/refresh | Blit ms/refresh |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Smoky title, 841–1080 | 25.57 ms | 32.48 ms | 33.27 ms at 1008 | 18.00 | 82,586 | 0.253 | 0.398 ms | 2.799 | 0.439 |
| File selector, 1561–1800 | 28.03 ms | 34.95 ms | 35.51 ms at 1682 | 17.99 | 82,752 | 0.263 | 0.351 ms | 3.069 | 0.463 |

Both sampled windows issued zero indexed draws, client waits, server wait
calls, and finishes. Actual array draws retained the indexed guest input
and hardware vertex work. The 4,800-refresh empty-archive run saw 128
explicit stage compiles and 143 links across the whole replay. Their
combined host time was 315.81 ms. The largest whole refresh was
156.47 ms at 531. The data do not establish a driver-cold boot.

A longer empty-archive pass ended at refresh 11,700, with all 11,700
refreshes observed and no foreground pause. The capture sequence showed
the TV, Luigi in his house, and the teleport. Its slowest refresh took
601.30 ms at 11,652. This pass recorded 270 stage compiles and 333
links with 731.75 ms of combined host API time across the whole run.
The capture establishes a cutscene and transition, so those timing
windows are excluded from the warmed lab comparison.
### Matched cached controls

The two normal launches used the same local shader archive. The seed SHA-256 was
`e892b1122953788bd6249cbcc9d543b3058948e669ae146076ef506b9ae566d9`.
control-a prewarmed 128 sources in 309.82 ms and ended at thermal state 2.
control-b prewarmed 128 sources in 130.01 ms and ended at thermal state 2.
Both runs reported zero binary formats. Both LCD captures matched byte for byte
at the title, file selector, and four later checkpoints. Guest ticks, draw requests,
GPU vertex work, presented refresh counts, and profiled GL call counts matched.
Each timing window below recorded zero shader compiles and links.

| Scene | Control | p50 | p99 | Worst refresh | LUT updates/refresh | LUT bytes/refresh | LUT ms/refresh | Array draw ms/refresh | Blit ms/refresh |
| --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Smoky title | A | 26.84 ms | 33.80 ms | 34.82 ms at 1068 | 18.00 | 82586 | 0.257 | 2.913 | 0.457 |
| Smoky title | B | 27.78 ms | 34.77 ms | 34.96 ms at 981 | 18.00 | 82586 | 0.263 | 3.001 | 0.473 |
| File selector | A | 29.57 ms | 37.04 ms | 38.11 ms at 1682 | 17.99 | 82752 | 0.269 | 3.215 | 0.486 |
| File selector | B | 38.13 ms | 48.91 ms | 49.31 ms at 1643 | 17.99 | 82752 | 0.343 | 4.117 | 0.633 |
| Warm Gadd dialogue | A | 38.17 ms | 49.77 ms | 50.65 ms at 13939 | 18.00 | 82669 | 0.334 | 4.071 | 0.646 |
| Warm Gadd dialogue | B | 37.93 ms | 50.35 ms | 64.47 ms at 13971 | 18.00 | 82669 | 0.313 | 3.760 | 0.596 |

Refreshes 841–1080 cover the title, 1561–1800 the file selector, and
13621–14100 the first Gadd lab conversation. The lab captures at 13500
and 14100 show the same dialogue prompt. The composite includes both LCDs.
The longest measured RG upload call across these windows was 0.568 ms at refresh 1601 in control-b.
The controls made no host indexed draw or explicit wait calls in those
windows. The array-draw measurements cover the GLES atlas path.

[Complete window measurements](dark-moon-fog-lut-se3-windows.tsv)

Control B recorded 7.09 seconds paused. The frontend removes paused time
from subsequent refresh samples and resets pacing on resume. The pause
position was not logged. Control B failed the uninterrupted whole-run
gate and is retained for work, pixel, and per-window API observations.
It supplies no independent elapsed-play or thermal comparison. Both
runs ended at the serious thermal level. The first control ran
uninterrupted.

## Decision

Apple GLES did not exhibit a costly LUT upload wait in these scenes.
The lab spends about 0.31–0.33 ms per refresh in LUT upload despite
roughly 18 uploads and 82.7 kB each refresh. The largest single LUT
call in the timed zones stayed below 0.57 ms. Mean estimated LUT
off-CPU time in the lab was 0.18–0.19 ms per refresh. The array draw,
blit, and present scopes were measured separately. Zero explicit sync
calls leaves implicit driver waiting possible.

The Mali evidence identifies a scheduler wait in the upload path.
Here the same class of partial uploads contributes under 1% of the
lab median interval. There is no evidence from these controls that
a different LUT texture lifetime solves the Apple slowdown. Whole-image
replacement and distinct generations were not enabled on Apple. The
Android-specific ownership and retirement behavior stays gated.

The steady lab is still well over the 16.7 ms target. On control A,
vertex preparation spent 7.80 ms per refresh, vertex upload 3.06 ms,
and actual array draw calls 4.07 ms. These host scopes can overlap
broader draw and guest-loop timings. The lab issued 248 lighting-enabled
draws per refresh. These observations preserve lighting and do not
identify GPU fragment shader execution time. A native Metal comparison
requires a separate same-device benchmark and stage counters.
