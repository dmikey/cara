# GLES depth residency and byte coherence

## Observed failure

On the M4 iPad, the Stories grass trace repeatedly acquires an RGBA8 view at
`0x183fd400`, 256 by 512 pixels, with a 256 by 400 active rectangle. The same
memory is used as a D24S8 depth target. The old capture reported 7,939 missing
format reinterpretation warnings, all D24S8 to RGBA8. A RAM fallback can therefore
be a GPU alias request rather than a new CPU-produced texture.

The GLES cache previously skipped depth downloads in `FlushRegion` while still
removing dirty ownership. Its depth RAM validation path also did not upload the
RAM data. A zero readback counter did not establish RAM coherence. These paths
must publish current bytes or retain ownership and report failure.

## M4 upload precision probe

The device reports `OpenGL ES 3.0 Metal - 105`, renderer `Apple M4 GPU`. A strict
ROM-free test uploads generated D24S8 codes, exports them to RGBA8 and compares
all depth and stencil bytes. The first failure was depth `0x7fffff`, exported as
`0x800000`. An integer render target then captured the sampler's raw float bits
and the conversion's arithmetic. Separately writing the correctly normalized
float through `gl_FragDepth` preserved the expected value.

| Guest depth | Correct normalized float bits | Packed upload sampled bits | Fragment-written sampled bits |
| --- | --- | --- | --- |
| `0x7ffffe` | `0x3efffffd` | `0x3efffffe` | `0x3efffffd` |
| `0x7fffff` | `0x3effffff` | `0x3f000000` | `0x3effffff` |
| `0x800000` | `0x3f000001` | `0x3f000001` | `0x3f000001` |
| `0xfffffd` | `0x3f7ffffe` | `0x3f7fffff` | `0x3f7ffffe` |
| `0xfffffe` | `0x3f7fffff` | `0x3f800000` | `0x3f7fffff` |
| `0xffffff` | `0x3f800000` | `0x3f800000` | `0x3f800000` |

The last two packed inputs collide. Changing export rounding cannot recover
information already lost at upload. This is a measured conversion issue; it
does not identify the driver's internal storage format or establish a general
GLES conformance violation.

## Repair and validation

Keep existing depth/stencil texture formats. Upload canonical bytes through an
integer texture, construct the normalized depth float exactly, and write stencil
bits with masked fixed-function operations. Preserve pixels outside the upload
rectangle and all caller GL state. Depth export samples depth and recovers stencil
using read-only stencil tests; ordinary GPU aliases need no CPU readback.

For nonzero 24-bit code `k`, let `p = floor(log2(k))`. Its correctly rounded
binary32 normalized value has bits
`((p + 102) << 23) + (k << (23 - p)) + 1`; zero is handled separately. A native
exhaustive check passed all 16,777,215 nonzero codes. The M4 iPad then passed the
unchanged byte oracle for all D24 and D24S8 codes, all D16 codes, stencil values,
partial rectangles and outside pixels. It also passed 504 generated CPU-write
cases, eight mixed color/depth-owner cases and 508 failed-export ownership
checks, including poisoned RAM/staging and simulated restoration. The hardened
build fingerprint was `80280362f28d99beda54faedd319a0bdbe2ba06dfbde7b74409585cd26135b77`.
The later matched-checkpoint and cross-title results are recorded below.

Canonical CPU staging is D24 `[padding, Zlo, Zmid, Zhi]`, D24S8
`[stencil, Zlo, Zmid, Zhi]`. Guest memory retains its existing packed format.
Dirty intervals are removed only after their actual owned bytes are published.
The current CPU depth export prototype is limited to native scale and unpadded
rows; unsupported export fails without treating stale RAM as valid.

## Alias rectangle selection

A subsequent Stories owner trace recorded 3,200 RAM fallback uploads during
480 refreshes, totaling 1,310,720,000 requested bytes. Almost all bytes were
owned by the GPU's D24S8 surface. The dominant invalid span was 409,344 bytes,
while rectangle reconstruction loaded 409,600 bytes from the same 256-stride,
512-high tiled allocation. The prior checked alias path rejected the extra
256-byte tail even when the depth source remained valid there.

The repaired selection may copy a larger rectangle only when it lies within the
destination, overlaps the outstanding invalid interval, is exactly representable
by the destination, remains valid throughout the source, and passes source
copyability for the actual rectangle. CPU or other GPU writes invalidate the
source's corresponding bytes; these cannot be overwritten through this path.
Only the successfully copied interval is marked valid. The overlap requirement
also prevents a disjoint valid source region from causing a validation loop
without progress. This changes cache work, not guest timing or hardware events.
The additional iPad ownership fixture passed six partial/source-authority cases
and four non-progress checks. Matched game results follow.

## Matched M4 iPad results (2026-11-13)

Stories used the same moving grass checkpoint and scripted route, 100% guest
CPU, sound, a 100% speed cap, and unchanged guest hardware timing. Each run
measured 900 refreshes after 300 warm-up refreshes.

| Configuration | Runs | Wall time per refresh | Emulation thread CPU per refresh |
| --- | ---: | ---: | ---: |
| Correct RAM depth fallback, 16K interpreter cache | 2 | 37.548418 ms | 25.026771 ms |
| GPU depth alias, 16K interpreter cache | 4 | 17.785075 ms | 15.752413 ms |
| GPU depth alias, 32K interpreter cache | 4 | 17.282122 ms | 14.435953 ms |

This compares against the **correct RAM fallback**, not the earlier incorrect
path that skipped depth publication. The RAM controls performed 5,960 readbacks
per measured run; GPU-alias runs performed none. All ten runs matched guest
PC/ticks, draw and vertex workload, and final pixels. The 32K cache separately
saved 1.316460 ms of thread CPU and 0.502953 ms of wall time versus GPU alias
with 16K. Heavier route windows still reached roughly 20–21 ms; this does not
establish sustained full speed throughout the game.

Luigi's Mansion: Dark Moon opening dialogue, Zelda outdoors, and Xenoblade's
intro were also compared across all three configurations. Each scene matched
pixels and workload, with approximately 16.712–16.715 ms per refresh. These are
scene-level checks, not full-game compatibility claims. The tested renderer
source fingerprint was `fef2bc39dde82df12d5b64cefdc6abe613de1f328c4f0f2080157d4413de428a`.

GPU depth aliasing now defaults on in the core. The iOS frontend selects the
32K interpreter cache on devices with at least 6 GiB of physical RAM, preserving
explicit overrides. It adds approximately 42 MiB across four guest cores; the
target M4 iPad has approximately 8 GB of RAM. Absolute-PC dispatch, deferred
tick accounting, prepared-texture reuse and relative-dispatch prototypes have
no established device gain and are not enabled by this integration.

## Primary references

- [OpenGL ES 3.0 specification](https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf): UNORM conversion, valid depth upload format/type combinations, and matching-format depth/stencil blit restrictions.
- [GLSL ES 3.00 precision rules](https://registry.khronos.org/OpenGL/specs/es/3.0/GLSL_ES_Specification_3.00.pdf): high precision arithmetic and division accuracy do not justify assuming byte-exact depth normalization.
- [Apple D24S8 support](https://developer.apple.com/documentation/metal/mtldevice/isdepth24stencil8pixelformatsupported): the Metal format is unsupported on Apple silicon. This alone does not document GLES internal storage.
