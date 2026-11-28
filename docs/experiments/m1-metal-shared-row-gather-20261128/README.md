# M1 Metal: share the checked row-copy pipeline

Status: selected-output and separate exclusive-CPU gates PASS. Both row-gather fixes remain ON in the private test preset. Public/installed defaults are unchanged. The original quiet plan was blocked before release. The user then authorized a separately reviewed loaded comparison, now complete; see ../m1-metal-pacing-20261128/README.md for its actual result and subsequent pacing investigation.

## Choosing the next target

After the first row-gather fix, the retained CPU categories had roughly 1.55 ms in remaining binding and 1.31 ms in byte resolution. A new counter-free stack diagnostic with the first fix ON still showed Metal copy encoding and AGX copy-dependency tracking. It did not establish the binding category as a new isolated production hotspot: category clocks have their own overhead. Exact vertex-image checks remain visible, but no complete RAM-write version contract supports bypassing them.

The earlier plan incidence left 2,407,538 original blits outside accepted gathers. Source inspection found that non-transfer byte consumers did not receive the existing copy_rows pipeline. This follow-up passes that already-loaded pipeline into the renderer lane and retains it for the lane's lifetime. Explicit transfer pipelines retain precedence.

## Change and contract

Baseline: `CARA_METAL_GATHER_RESOLVED_ROWS=1` in every Metal case.
Follow-up: `CARA_METAL_GATHER_SHARED_RESOLVED_ROWS=1`.
Both flags remain opt-in in core source; the private performance preset sets both to 1. Neither is added to public frontend defaults.

The same two-source planner, word-copy kernel, argument ABI, alignment/extent/resource checks, original authority queries, post-plan producer revalidation, checked RAM gaps, waits and ownership/failure receipts remain intact. A nil or wrong-device shared pipeline declines to the original blits. The shared path requires both exact-1 flags. No new library, shader, queue-depth policy, host clock, guest timing or draw change. The already-closed 2,517-case planner gate was not rerun because its source and kernel are unchanged.

The engine built at `36c38a1`. GPU library bytes match the previously tested `e6f6c86` build exactly. The new counter-free stack sample and all new native jobs reaped cleanly without forced signals.

## Actual incidence and selected correctness

One normal-boot 3,000-refresh M1 JIT replay used 100% guest CPU, all four guest cores, cap 100, native 1× and SDL sound. It kept the first fix ON and enabled only the shared-pipeline follow-up. It matched all 3,001 selected tick/core-PC/work rows, guest files and both LCD images at 800/1600/2400/3000 against the retained Metal control. This is selected correctness, not complete VRAM, all effects or all-game proof. It is the retained Gadd-lab replay, not mansion traversal.

| Whole-process counter | Result |
| --- | ---: |
| Original logical copy spans | 7,230,642 |
| Span bytes, unchanged | 5,609,274,368 |
| Gathered spans with both fixes | 7,213,504 |
| Compute dispatches with both fixes | 17,570 |
| Newly eligible shared-pipeline plans | 2,988 |
| Additional blits replaced versus first fix | 2,390,400 |
| Original blits remaining | 17,138 |

The logical-copy counter remains a count of original span work before grouping, not actual ON blit calls. Replacing commands does not remove the required byte copies.

## Separate CPU pair

OFF and ON here mean shared-pipeline follow-up OFF/ON. The first row-gather fix is ON in both controls. Detailed timing counters were OFF and exclusive CPU categories ON in both. Same binary, normal-boot inputs, workload and selected LCDs; both native jobs exited 0/reaped/group-empty with cleanup[]. State, work and files matched exactly.

| ms per scored refresh | Shared OFF | Shared ON |
| --- | ---: | ---: |
| Resolve CPU, exclusive | 1.31920 | 0.93067 |
| Owner-thread CPU | 18.12596 | 17.53526 |
| Process CPU | 22.21011 | 21.37834 |
| Mean refresh | 18.45529 | 17.93457 |
| p99 refresh | 19.98150 | 19.16817 |
| Worst refresh | 21.10867 | 20.32621 |
| Slowest rolling 120 average | 18.58227 | 18.01926 |

Resolve CPU fell 0.38853 ms; other attributed graphics categories fell 0.14540 ms, for about 0.53392 ms less graphics CPU. Guest execution varied by -0.05361 ms. Owner/process CPU fell 0.59070/0.83177 ms. This supports keeping the follow-up enabled privately; it did not merely move the sampled CPU cost into another graphics category. The mean and tails improved in this pair, but APFS was active. One ordered diagnostic pair does not establish a quiet gameplay win, statistical certainty, GPU-time improvement, or an advantage over OpenGL. Do not add these savings to earlier independently ordered pairs or compare absolute category timings with uninstrumented controls.

## Quiet renderer comparison

A new reviewed bracket is GL/Metal/Metal/GL, with both verified fixes ON in both Metal cases. It binds current source, binary, local ROM, actual correctness gates, original harness and frozen reference captures. It requires three quiet preflight checks and service CPU <=50% of one logical CPU throughout the scored late scene. JIT CLK 100/cap 100, native 1×, SDL, full reference and actual vertex work, software-vertex 0 and skipped-draw 0 remain required. CPU categories and detailed counters are OFF. Same-renderer selected pixels/state/work/files must match; cross-renderer pixel differences remain explicitly reported rather than declaring top-panel parity. Startup and cold-driver behavior are not this scored window.

Source-ready SHA: `4e61d15423e08874ea8e71b495311cf4210f7393dc5b72ff925c8924f5076491`, binding 31 harness/reference files and 10 source files. Exact identity validation finishes before release. Each next case requires the previous case's complete PASS. Stop on first failure, preserve everything, no automatic retry or policy relaxation. The older consumed index-snapshot release is not reused.

Admission1 saw 196.0% background service CPU, chiefly APFS 195.8%, against the 50% gate. It returned before creating a release or any native case. Both Files and Backups remain mounted; no service, user app or volume was stopped to force admission. A later explicit attempt may perform a fresh append-only admission check against this same unconsumed source-ready plan. Driver warmth remains unknown; this bracket is capped and order-limited, not a universal throughput result.

## Artifacts

Private source: `archive/m1-metal-profile-20261127` at `36c38a1`.
Attached patch applies to `e6f6c86`, with the first fix already present.
Local evidence root: `/Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex/`.

- `PRIVATE-PERFORMANCE-PRESET.json`: both fixes ON for future private controls.
- `row-gather-owner-sample-1/RESULT.json`: counter-free post-fix stack sample.
- `shared-row-gather-build-1/RESULT.json`: actual binary gate.
- `shared-row-gather-incidence-1/RESULT.json`: activation, incidence and retained correctness.
- `shared-row-gather-cpu-1/RESULT.json`: separate exclusive-CPU pair.
- `ROW-GATHER-RENDERER-SOURCE-READY.json`: frozen future renderer plan.
- `ROW-GATHER-RENDERER-ADMISSION-1.json`: blocked before release; native_started=false.

Do not repeat closed fixtures, counting or CPU gates unchanged. The separate loaded renderer bracket has now completed; the old quiet admission plan is historical and must not be reused against changed binaries. Future renderer work should retain this distinction: fewer API commands, less CPU, less GPU time and better frame tails are separate claims.

The accompanying quiet-runner/admission scripts preserve the reviewed orchestration for inspection. Their authoritative local copies are in the evidence root above and require its frozen source-ready manifest and retained inputs. This folder contains no ROMs, saves, screen payloads or release permission.
