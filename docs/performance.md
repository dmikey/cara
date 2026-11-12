# Performance measurements

Xenoblade Chronicles 3D's opening sequence on an M4 iPad, at native resolution
with sound enabled and JIT disabled. All runs used **100% guest CPU clock**.

| Build | Emulation speed | Peak sampled memory |
|---|---:|---:|
| **Cara GLES3** | **99.5%** | **758 MiB** |
| Manic / Azahar | 97.4% | 1,373 MiB |
| Azahar / MoltenVK | 84.6% | 1,261 MiB |
| Manic / Citra (frozen scene) | 82.1%† | 1,302 MiB |

† Manic / Citra's scene froze while its counter continued. Its figure does not
represent a completed, playable replay.

The first three builds completed the opening replay. Cara used about 40–45% less
sampled memory than the two Azahar-based builds in this test. These are whole-app
results, including differences in frontend and presentation behavior. Cara was
measured at checkpoint `a9e0658`, before save-state support.

## Interpreting the results

Emulation speed measures progress relative to the emulated system’s normal speed;
it is not a rendered game frame-rate measurement. Peak sampled memory is the
largest observed sample, not a continuous measurement of every allocation.
These results compare whole apps and do not isolate a single renderer or CPU
optimization. They are historical measurements, not a guarantee for current
builds or other games.

See [validation](validation.md) for additional tests, measurement constraints,
and optimization work.
