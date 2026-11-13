# Stories performance history, 2026-11-13

The November 12 records support the user's memory of near-full speed at a 100%
guest CPU clock in a lighter 3D section. The same recorded sequence already
slowed as its rendering workload increased. Matched November 13 windows do not
show a sustained new slowdown in that sequence. This does not rule out a
regression in another scene or an unrecorded configuration.

## Matched capture windows

Private evidence is under `/Volumes/Fast/CaraBuild/profiles/`:

- November 12: `decode-clean/stories-stage2`, `stories-stage3`, `stories-stage4`.
- November 13: `residency-final-four-title-20261113/stories-stage2`,
  `stories-stage3`, `stories-stage4`.

The capture scripts and final-window draw/vertex totals match. Each row below
covers 60 emulated refreshes; times are milliseconds per refresh. Both report
100% guest CPU with adaptive CPU disabled, FastInterp, native GLES and sound.

| Capture window | Nov 12 wall / thread CPU | Nov 13 wall / thread CPU | GPU draws / vertices per 60 refreshes, both runs |
| --- | ---: | ---: | ---: |
| Stage 2, ending 2100 | 16.470 / 15.201 | 16.653 / 15.349 | 40,490 / 99,649,014 |
| Stage 2, ending 3000 | 19.937 / 17.820 | 19.882 / 17.802 | 53,400 / 132,777,600 |
| Stage 3, ending 4200 | 25.084 / 21.527 | 24.810 / 21.160 | 62,861 / 167,290,437 |
| Stage 4, ending 2400 | 21.838 / 20.036 | 21.329 / 19.553 | 58,800 / 144,435,600 |

The stage-2 window ending 2100 is substantial 3D work: approximately 675 GPU draws
and 1.661 million vertices per refresh. Its November 12 throughput was sufficient
for approximately native refresh speed. Later in that same capture, 890 draws
and 2.213 million vertices per refresh required about 19.94 ms. The near-100
observation should not be dismissed as a 50% guest clock or only opening-video
work. It also should not be extended to sustained full speed in the heavier
sections.

## Conditions and limits

November 12 captures were **uncapped**; November 13 captures used a 100% speed
cap and bounded host pacing. Wall time near the refresh target can include
pacing sleep. Thread CPU time helps separate execution cost from that sleep;
it is not hardware GPU execution time. The small difference in the lighter
window is not an isolated model-regression measurement.

Equal draw/vertex totals and scripted input strengthen comparability but are
not pixel proof. The archived final screenshots capture the shell without the
GLES image, and a raw old image for the earlier 3D window is unavailable. Exact
intermediate pixels, animation phase and game progression are not established
by these counters alone. Boot-wide averages mix light and heavy scenes and must
not replace the specific windows above.

The relevant engine fingerprints are:

- November 12: `9bb6c222022a3c16b6a830ea113f44c13eb47d3ff95276b7f8b20f18f6327639`.
- November 13: `a7014a4617b3ef115525d1e917ec3c2625b85837963915df4afc7245e1de547e`.

Use fingerprints and build receipts rather than embedded revision strings alone:
those strings can predate uncommitted source included in a binary. Experimental
P3D/PPF completion timing was disabled in the November 13 captures. The older
capture predates those experiments.

## Remaining question

The records show a workload increase; they do not explain whether all of the
additional submissions are necessary scene work or partly driven by the
completion/wait contract. A bounded submission-to-IRQ-to-guest-wait-to-resumption
trace remains the next diagnostic. Preserve required reads, writes, rendering,
interrupt ordering and the requested 100% guest clock while testing that
hypothesis. No old-binary rollback is required to establish the matched-window
finding above.
