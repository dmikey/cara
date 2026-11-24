# PICA shader hints prototype

Generate a portable hint file from a local decrypted dump:

```sh
python3 tools/scan_pica_shader_hints.py --output ./shader-hints "/path/to/game.3ds"
```

The scanner also accepts decrypted NCCH (`.cxi`) and a standalone SHBIN.
For a standalone SHBIN, pass the program ID as `--title 00040000…`.
Copy the resulting `<program-id>.pica-hints` into the `shaders` directory
under Cara's user data directory and start the game. Remove the file to
disable this experiment for the title.

The file contains a version marker and up to 512 entry-point fingerprints.
It carries no ROM data, generated shader source or driver binaries. The
scanner recognizes directly visible DVLB/DVLP/DVLE structures. Embedded
or compressed formats it cannot decode may yield an empty list.

Cara treats a matching fingerprint as a scheduling hint. It still derives
the complete source and pipeline key from live registers, program code,
output mapping and the active vertex layout. A file containing only the
version marker enables the bounded live-miss path for games where the
scan finds no entry points.

- Metal queues one exact runtime vertex-function build at a time before
  sampler planning. On a matching first use, fragment compilation can
  overlap that build. The draw still resolves the ordinary pipeline key.
- Android GLES Atlas queues exact separable vertex stages on a shared
  EGL context when its vertex-stage archive misses. Eligible draws use
  Cara's CPU vertex path until the stage is ready, then resume hardware
  vertex processing. A draw that requires the stage can take its queued
  job off the worker and compile immediately. The linked
  GLES path can start its existing async program build earlier when it
  already has a usable software-vertex fallback.

Fragment shaders and required vertex stages can still compile on the
draw thread. The shader census records ROM fingerprints, generated source
hashes, CPU fallback decisions, and long link calls separately from link
status queries. Apple GLES continues to use the existing shader cache path.

### RG Slide cold-boot validation

Dark Moon was booted through the title and Gadd's lab on the RG Slide
with Dynarmic, GLES Atlas separable stages, native resolution, fixed CLK
100%, and adaptive frame skip off. Each run started with an empty Cara
archive. The device's driver-private cache was retained.

| 120-refresh window | Original hints p99 / worst | CPU fallback and queue fix p99 / worst |
| --- | ---: | ---: |
| Smoky title | 8,159.32 / 42,388.20 ms | 2,286.93 / 2,295.33 ms |
| Lab entry | 94.83 / 8,286.81 ms | 175.03 / 1,145.93 ms |

The intermediate CPU fallback run exposed a 20.24-second queue wait
on a shadow draw. The hardware path was preparing a vertex stage before
the renderer reached its existing unsupported-shadow check. Early
rejection removes that work. A required stage that is still queued can
also be taken by the renderer, so it cannot wait behind unrelated jobs.

In the final run, the title p99 was refresh 515, with 1,732.89 ms in
foreground compilation. The whole title window recorded 36 foreground
fragment links totaling 3.26 seconds. The next title window's p99 was
82.74 ms with zero foreground compilation and 80 CPU vertex draws.

The lab-entry p99 was refresh 2491 with 701 draw requests, 72 CPU vertex
draws, 162.02 ms of emulation-thread graphics CPU time and zero foreground
compilation. The worst refresh, 2432, had 982.06 ms in foreground
compilation. Lab window draw counts differ with the transition's
alignment to the measurement boundary. Three settled lab windows had
zero foreground compile work, 32.44–66.34 ms p99, and 72.65 ms worst.

The single ROM fingerprint matched two generated vertex sources at
guest entry 0 during the title and opening. Lab-entry stages at guest
entries 315, 327 and 344 were discovered from live state.

[Instrumented Android build](https://github.com/dmikey/cara/actions/runs/45507523389)
