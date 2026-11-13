# Exact vertex upload reuse across moving draw order

The default-off `CARA_VERTEX_UPLOAD_ORDER_REUSE=1` experiment reduces avoidable
packed vertex/index texture uploads when visible draw order changes. A repeated
Stories walking route measured **19.055433 -> 18.085450 ms per completed display
refresh**, a **0.969983 ms / 5.09% reduction**. Emulation-thread CPU fell
17.289138 -> 16.344886 ms/ref (5.46%). This is a checkpoint/route result, not
locked full speed, unique game FPS, cross-title validation or default promotion.

## Method and implementation

Physical-iPad Stories USA, 100% guest clock, no JIT, sound synthesis/output queue
running, existing clock/event policy, 100% speed cap. Each full route completes
1,200 display refreshes; timing excludes the first 300, leaving 900. Scripted
circle inputs: left 360–480, right 600–720, up 840–960. Touch input is locked during
capture. Full routes use identical compatible checkpoint SHA-256
`23226fabbc422479195ef2a532b03fb82ba6744bc482c9302b5bc94069a0e086` and build
fingerprint `52ffaea00ec2ef9c4907020b3178d50360f7fd6dde9777c02280474eb224e242`.
Phase CPU/category sampling was disabled; phase scopes are elapsed host time,
not hardware GPU time. Counts/bytes below use their actual 60-refresh windows.

Existing three rotating upload banks partition textures by format/height and
select ordinal slots within each class. The candidate searches at most 32 later
**unused** entries in that same class, rejecting unequal lengths/endpoints and
then requiring **complete byte equality of both input and indices**. It swaps
whole texture/shadow owners before GL binding; it never searches the used prefix,
changes guest data/draw order, or skips changed payloads. New allocations,
fallbacks and direct/lit-pair/feedback modes retain existing behavior. Initial
slot comparison results feed normal reuse, avoiding duplicate full comparisons.
Search and shadow work remains in `VertexReuseCheck`. The measured helper passed
12,335 ASan/UBSan cases including permutations, unequal middles with equal ends,
index/size/format/extent mismatches, used prefixes, search limits and empty data.

## Full-route comparisons

The exploratory six-run set separates graphics order reuse from interpreter
SIMD tag lookup. The independent four-run confirmation repeats order reuse alone.

| Set / configuration | Wall ms/ref | Thread CPU ms/ref |
| --- | ---: | ---: |
| Initial baseline A / B | 18.779282 / 19.019301 | 17.025970 / 17.276502 |
| Initial order only | 18.070815 | 16.340220 |
| Initial order + SIMD A / B | 18.456467 / 18.380884 | 16.786606 / 16.745129 |
| Initial SIMD only | 19.298549 | 17.662014 |
| Confirmation order A / B | 18.044980 / 18.125921 | 16.302387 / 16.387385 |
| Confirmation baseline A / B | 19.012132 / 19.098734 | 17.256050 / 17.322226 |

The isolated repeats differ by .080941ms, versus .086602ms between their controls.
The initial isolated gain was 4.38%; the confirmation gives 5.09%. SIMD alone was
slower in the initial set, so its inclusion is not supported by these results.
These are finite repeated measurements, not statistical confidence intervals.

## Movement spikes and quiet-window tradeoff

Confirmation means, each row describing the preceding 60-refresh window:

| Window end | Baseline -> order wall ms/ref | Baseline -> order upload scope ms/ref | Reuse/search scope ms/ref |
| --- | ---: | ---: | ---: |
| 360, before first move | 20.355901 -> 20.621097 | .005104 -> .005155 | .672502 -> .787384 |
| 420, first move | 24.985239 -> 20.634059 | 4.978120 -> .530458 | .486508 -> .615856 |
| 660, second move | 28.023460 -> 22.293970 | 6.919266 -> .781255 | .558920 -> .807994 |

At 420 wall falls 17.42% and upload scope 89.34%; at 660 wall falls 20.45% and
upload scope 88.71%. Quiet 360 shows overhead: wall +.265196ms and search +.114882ms,
with unchanged traffic. The experiment targets changing draw membership/order;
it does not establish a benefit for already stable scenes.

In the initial 900-refresh benchmark, actual uploads fall 1,011,253.76 -> 87,617.71
bytes/ref (**91.34%**), and subimage calls 122.8189 -> 17.9511/ref (**85.38%**).
Total uploaded-plus-reused payload stays exactly 8,995,277.94 bytes/ref. Storage
allocation counts remain 1.826667/ref. All three flagged runs have identical
traffic/reuse counts. Cumulative log snapshots match exactly: swaps 5924 at 360,
25619 at 480,27947 at 600,52300 at 720 and54291 at 1080. The two strongest movement
spans add 19,695 and 24,353 exact-payload swaps;720–840 adds none. Logs are bounded
snapshots near frame boundaries, not complete lifetime accounting.

These deterministic avoided writes explain the upload-scope change. The upload
scope excludes packing, comparison and pool selection; over 99% of the original
movement spikes was inside subimage calls. Native driver CPU versus blocking/
synchronization shares remain separate questions for host profiling.

## Workload, pixels and boundary checks

All corresponding windows in the six exploratory and four confirmation routes
match GPU draw/vertex counts, PICA draw requests/command lists, CPU vertex draws,
input-record counts and lit draws exactly. Their independently hashed endpoint
RGBA files all match:
`9d92b24caec606b9ddf970c0f0e3e913eb663957f27ed7d96ae4837689c5ba65`.

Two shorter baseline/order captures stop during the active movement spikes:

| Stop | Last60-refresh wall ms/ref | Last60 upload ms/ref | Matching raw endpoint SHA-256 |
| --- | ---: | ---: | --- |
| 420 | 24.727946 -> 20.620603 | 4.904291 -> .523207 | `4ccfb8876076514a4fbfe3c64f86d16c1ab3b1e7e29e0bf1de2b501da8b32910` |
| 660 | 27.948790 -> 22.249513 | 6.860490 -> .782798 | `cd9cdb7492254a14e3016cdab038fe642575fff3a29866821e92be5ef684c598` |

Their corresponding per-window workload vectors also match exactly. These add
pixel checks at the spikes; they do not compare every intermediate frame.

## Audio and remaining boundaries

All 14 captures report `queue_running=true`, `muted=false`, no output error,
nonzero PCM and changing samples. Residual DSP underruns remain. Confirmation
controls report 258/1506 and 266/1510 underrun/total callbacks; order runs 187/1438
and 202/1453. Aggregated descriptive proportions are 17.37% versus 13.46%; callback
counts and playback durations differ, so these are not a matched audio-quality
metric or proof of smooth sound. Every saved status reports system
`output_volume=0`; active synthesis/queue validation does not prove audible
speaker output or subjective listening quality.

An initialization-only ordinal-comparison refinement subsequently passed
32,335 optimized and ASan/UBSan native cases. It retains direct full comparisons
for the initial slot and edge filtering for alternate candidates; its device
results must be measured separately from the build above.

Cross-title checks and remaining host profiling belong in a later addition.
The experiment remains default-off pending those results. No PBO staging,
extra bank capacity, transformed-vertex cache, timing changes or skipped guest
work are included.

Private evidence roots: `stories-lunch-grass-20261113/candidate-comparison`,
`order-confirmation`, `boundary-420`, `boundary-660`; each has comparison receipts,
raw framebuffer, window timings, runtime/audio status and logs. Derived checked
values are retained privately in `order-consolidated-evidence.json`.
