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
