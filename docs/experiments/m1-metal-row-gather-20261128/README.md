# M1 Metal resolved-byte row gathering

Status: private default-OFF candidate. CPU planner, engine build and actual OFF/ON selected-output gates PASS. Separate exclusive-CPU comparison also PASS; quiet renderer comparison remains unavailable. Public OpenGL/GLES defaults and installed apps are unchanged.

## Evidence, rather than another queue-depth change

A retained late-Luigi Metal stack sample attributes active CPU samples to `ResolveBytes` → Metal buffer blit encoding → Apple's `AGX::BlitUtil::ResourceOverlappingTracker::checkDependentBlits`. The new normal-render-worker-on OpenGL sample also finished successfully, with both corrected LCD captures matching the independently checked OpenGL reference at refresh 2400. The samples have different durations, thread layouts and background load: sample counts are not comparable CPU percentages or GPU utilization.

Two successive narrow diagnostic revisions examined the actual Metal copy plan without changing or removing any command. The first established that no consecutive copies could be merged. The refinement distinguishes globally alternating sources from many separately allocated exports. Both normal-boot replays matched the retained 3,001 guest-state/work rows, files, and both LCD images at 800/1600/2400/3000; each native child exited 0, reaped and left its group empty.

| Whole 3,000-refresh replay | Count |
| --- | ---: |
| Resolve calls | 81,129 |
| Resolve plans containing GPU byte copies | 20,235 |
| Buffer-copy commands | 7,230,642 |
| Bytes copied by those commands | 5,609,274,368 |
| Consecutive same-source pairs | 0 |
| Contiguous pairs in both source and destination | 0 |
| Plans with one source | 5,763 |
| Plans with exactly two sources | 14,471 |
| Plans with more than two sources | 1 |
| Copies belonging to two-source plans | 7,224,876 |

Thus 99.92% of commands belong to two-source plans, although neighboring commands alternate buffers. Simply increasing queue depth, joining neighboring copies, or assuming missing invalidation would miss the actual cost. The existing full byte count is required work; it is not removable traffic.

## Candidate and safety contract

Exact private opt-in: `CARA_METAL_GATHER_RESOLVED_ROWS=1`; default OFF.

The planner accepts at most two source identities and strictly increasing, nonoverlapping destination spans. Each source must contribute at least two equal-width rows with exactly repeated source and destination strides. It checks alignment, extent, count and shader argument overflow. Actual resource bounds, device, tracked hazards, retained command and disjoint fresh destination are checked before encoding. Any irregular, overlapping, many-source, oversized or uncertain plan retains every original blit.

Eligible rows use the existing `cara_pica_copy_rows` kernel, one dispatch per source. Its MSL and 24-byte arguments are unchanged. Only transfer callers pass their already-loaded exact pipeline; other byte consumers retain the original path. Guest-owned gaps are still read into the fresh staging allocation, and the kernel only touches the same validated copied words. Source ownership/status and post-plan producer checks remain. An encoding failure rejects the uncommitted command rather than falling back after partial dispatch. Queue dependencies, failure receipts and later ownership publication remain intact. No PICA draw, quaternion reference, shader effect, guest timer, audio or CPU clock change.

The pure planner passed 2,517 actual C++ full-destination byte-equivalence and rejection checks, including untouched gap canaries, changing widths, missing/third sources, unaligned and overlapping spans, descending offsets, irregular strides and arithmetic overflow. This proves the planner's mapping against the unchanged kernel formula; it is not physical GPU or gameplay evidence. The private engine then built and signed at `e6f6c86`.

## Actual OFF/ON detailed-counter pair

Both 3,000-refresh native jobs completed cleanly, exited 0, reaped and left no owned process group. All 3,001 selected tick/core-PC/work rows, guest files and both LCD captures at 800/1600/2400/3000 match exactly between OFF and ON. Activation was absent OFF and witnessed ON. Existing Metal library bytes match the parent build exactly; this change adds no new MSL.

ON accepted 5,797 plans, replacing 4,823,104 blit commands with 11,594 dispatches. The logical plans, copy payload bytes and original PICA workload are unchanged. The remaining irregular or unprovided-pipeline consumers retain their original blits. This is not removal of required GPU work.

| Diagnostic measure | OFF | ON |
| --- | ---: | ---: |
| Resolve host span, last 600 refreshes, ms/refresh | 2.40480 | 1.76649 |
| Display transfer host span, last 600, ms/refresh | 2.47573 | 1.79448 |
| Texture copy host span, last 600, ms/refresh | 0.97914 | 1.03710 |
| Owner CPU, scored 400 refreshes, ms/refresh | 14.86897 | 14.35486 |
| Process CPU, scored 400, ms/refresh | 18.97473 | 18.49682 |
| Refresh p99, scored 400, ms | 17.70721 | 17.20263 |

Host spans are inclusive and overlap each other; do not add resolve, display and texture-copy spans. They differ from thread CPU. This is one ordered diagnostic pair under active APFS/Spotlight, not a quiet gameplay or statistical win. The approximately 0.638 ms resolve-span reduction and 0.478 ms process-CPU reduction justify a separate exclusive-CPU observation, which must not combine with these detailed timing counters. The original prelaunch harness refusal is preserved; no native child was launched by that failed attempt.

## Separate exclusive CPU observation

A further one OFF/ON pair used the same private binary and inputs, with detailed timing counters OFF and exclusive CPU categories ON in both cases. Both jobs exited 0/reaped/group empty without signals; guest state, work, files and both four-checkpoint LCD sets again matched exactly.

| CPU ms per scored refresh | OFF | ON |
| --- | ---: | ---: |
| Resolve bytes, exclusive | 1.97067 | 1.30703 |
| All other graphics categories combined, ON minus OFF | — | +0.04572 |
| Owner-thread CPU | 18.80818 | 18.15079 |
| Process CPU | 22.87890 | 22.21887 |

The resolve category fell 0.66364 ms; other graphics categories rose 0.04572 ms, for about 0.61792 ms less attributed graphics CPU. Guest execution varied by -0.04304 ms. Owner and process CPU fell about 0.657 and 0.660 ms respectively, supporting a real CPU-cost target rather than only moving a wait between graphics categories. This remains a single ordered diagnostic pair under filesystem/indexing load, with added category-clock overhead. Do not compare its absolute timings to the detailed-counter pair, to uninstrumented GL controls, or claim statistical certainty, GPU-time reduction, all-game correctness, or a Metal-over-GL result.

Core source stays default OFF. At the user's request, the verified fix is ON in the private performance preset; public/installed defaults are unchanged. A newly reviewed quiet renderer comparison remains required. The consumed index-snapshot release is not reused. There are still 2,407,538 original blits outside accepted row-gather plans in the whole process. Next work should distinguish consumers lacking the shared pipeline from layouts rejected by the regular-row proof; neither may be assumed eligible or silently widened.

## Capture correction and previous comparison limits

The first reviewed GL/OFF/ON/ON/OFF/GL index-snapshot bracket stopped in its first ON case when Spotlight updater exceeded the background CPU gate. No automatic case retry followed. Its completed GL 16.642 ms and Metal 16.968 ms controls are provisional and do not establish a candidate win.

Visual inspection also found a private native screenshot observer bug: a 400×480 framebuffer retained an 800×960 viewport. Commit`d3a33c7` sets and restores the native capture viewport. One corrected terminal capture matches independently decoded authoritative guest LCD bytes exactly on both panels. An offline reader review then corrected an erroneously selected retained refresh 3000 versus 2400; its original receipt remains. With the correct retained Metal 2400, the lower panel matches exactly and upper RGB MAE is 0.122 with precision/edge differences. No exact cross-renderer top-panel parity claim.

Files and Backups remain mounted; no remount action was needed. APFS and Spotlight activity returned. Diagnostic timing under that load cannot be promoted as a quiet gameplay gain, and no Metal-over-OpenGL claim is made here.

## Artifacts

Private source archive: `archive/m1-metal-profile-20261127`. The attached patch applies to its `c81835f` parent. This review branch only preserves source, checks and findings; it does not integrate or activate the candidate in public main.

Local evidence root: `/Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex/`.
- `resolve-copy-incidence-1/RESULT.json`: original finalized-plan counts.
- `resolve-source-incidence-1/RESULT.json`: refined distinct-source incidence.
- `copy-row-gather-component-1/RESULT.json`: CPU planner gate.
- `copy-row-gather-build-1/RESULT.json`: actual private binary/build gate.
- `copy-row-gather-diagnostic-1`: preserved harness refusal before native launch; CPU categories may not combine with detailed timing counters.
- `copy-row-gather-diagnostic-2`: first admitted OFF/ON detailed-counter pair, PASS.
- `copy-row-gather-cpu-1`: separate exclusive-CPU pair, PASS.
- `native-viewport-correctness-1` and `gl-worker-sample-20261128-1`: corrected captures and GL worker sample.

Do not rerun closed diagnostics unchanged. A later cost comparison must keep diagnostic clocks separate from quiet controls, assess total CPU and frame tails, and retain all original work and checks. Grouping proven independent data movements is also a portable renderer design lesson for future Vulkan work; merely pushing the same per-span calls onto another thread would not remove them.

The [shared-pipeline follow-up](../m1-metal-shared-row-gather-20261128/README.md) covered another 2.39M blits with the same checked kernel and reduced resolution CPU in a separate pair. Both verified fixes are now ON in the private preset; do not add savings across independent ordered pairs.
