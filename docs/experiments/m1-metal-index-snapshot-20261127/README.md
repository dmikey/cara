# M1 Metal checked index-snapshot ownership proof

Status: private build tested, default OFF. Shadow validation and selected Luigi
correctness passed. Quiet Metal/OpenGL performance remains pending. The review
branch records the experiment; it does not turn it on in the production app.

## Why this scope

The previous draw profile found vertex/index image checking and lookup among
the largest draw-encoding costs. A previous detailed run had 1,528,036 exact
index-scan memo hits versus 8,578 misses. Those hits already compared every
current index byte, and the upload cache compared the same image again.

The general FCRAM vertex path cannot safely use address equality or lack of
invalidation as proof. Ownerless RAM watch pages can be released, and mutable
host-pointer access is not a complete RAM-write version contract. This candidate
does not install extra RAM watches or bypass that uncertainty. It reuses the
index scan's existing exact-byte check only, with a retained captured-image owner.
The much larger vertex payload retains its original complete checks.

## Contract

Exact opt-in: `CARA_METAL_INDEX_SNAPSHOT_PROOF=1`; default OFF.

1. The original authoritative guest read happens on every draw, with the same
   GPU producer/queue/failure checks. The scan memo still compares every byte.
2. Only a successful complete exact scan read can issue a revision identity.
   Replacement clears that identity before overwriting the memo buffer. A
   retained old identity cannot be recycled for different bytes after eviction,
   save/load, a new memo, or address reuse.
3. The read record includes the actual captured payload pointer and size.
   Even a different byte-equal allocation cannot inherit its proof.
4. Upload binding takes a typed retained const index-vector view of the exact
   binding-storage object. It validates the literal header, full image bounds,
   row shape and every zero-padding byte. The existing exclusive Prepared-to-
   Upload phase cannot reuse that storage while a draw retains it. All payload
   transformations revoke the read record before modifying the image.
5. Only the same revision, header, full image size and original texture shape
   may bypass the second image comparison. Missing, changed or uncertain proofs
   take the original complete byte-check path. Other vertex modes are unchanged.
6. The upload cache retains only the content identity, not mutable capture
   storage. Deferred draw packets drop the capture proof after uploading their
   immutable inputs. Cache capacities and GPU work/order remain unchanged.

The distinction matters: a memo's raw entry pointer alone is not a lease. Its
storage can be overwritten. The new identity represents one checked revision,
and the captured buffer owner pins the exact read being consumed.

## Actual checks

The final source passed 38 new actual C++ ownership/lifetime/negative checks:
misses, partial reads, malformed keys and addresses, changed payloads, retired
identities, memo eviction/reinsertion, wrong allocation/extent/header, nonzero
padding, owner pin/release and typed alias lifetime. Earlier source-only API
revisions and their passing component receipts remain locally preserved; they
are not substituted for the final gate. A padding-negative fixture was tightened
to provide valid payload provenance so it actually exercises padding rejection.
No existing closed component suites were rerun.

The private M1 build then completed one 3,000-refresh normal-boot Luigi replay
with `CARA_METAL_VERIFY_INDEX_SNAPSHOT_PROOF=1`. In this shadow mode every
proposed proof reuse also executes the original complete image comparison and
throws on any disagreement. All **1,238,970** proposed hits passed, covering
**2,806,618,112 image bytes**. These bytes were still compared in shadow mode;
they are not a measurement of bytes avoided by that diagnostic run.

Two further fresh OFF/ON replays used the same binary with shadow disabled and
identical exclusive CPU-category diagnostics. All 3,001 observed guest tick,
core-PC and workload rows, guest files and both native LCDs at refreshes 800,
1600, 2400 and 3000 matched. ON emitted its actual non-shadow proof witness;
OFF did not. All three native jobs exited 0 and reaped without forced signals.
This is selected-output/state/work correctness, not complete VRAM or all-game
proof. It is the retained Gadd-lab replay, not a mansion gate traversal.

JIT used all four guest cores at fixed CLK100, cap100, 1x and SDL sound. The
400-refresh measurement window issued 278,153 draw requests, 4,782 command
lists and display transfers, 3,985 texture copies and 6,376 fills. Software
vertex and skipped draws remained zero. Shader code, quaternion correction,
original GPU work, guest timers and completion validation were unchanged.

## Diagnostic cost and limits

| Diagnostic CPU, ms/refresh | OFF | ON |
| --- | ---: | ---: |
| Vertex/index upload excluding eviction | 0.78761 | 0.72231 |
| Remaining binding work | 1.55540 | 1.60558 |
| Vertex capture | 0.98056 | 0.97261 |
| Combined binding/capture/upload/eviction | 3.36779 | 3.34465 |
| Owner thread | 18.78228 | 18.67172 |
| Process | 22.64869 | 22.46750 |

The upload category fell 0.0653 ms, but proof bookkeeping shifted some work into
binding. The combined selected categories fell only 0.0231 ms. Guest execution
also changed by about 0.0503 ms in this single ordered pair, indicating host/
frequency/scheduling variability. Added diagnostic clocks have overhead, and
APFS maintenance remained active. Do not call these a quiet gameplay win,
statistical result, whole-vertex speedup or Metal advantage over OpenGL.

Public Metal/OpenGL defaults and installed apps are unchanged. All unrelated
private scan-status, completed-writer, raw-resolution, rejected-probe, ordered-
eviction and sampler-binding candidates were explicitly OFF.

## Pending clean comparison

Frozen plan: OpenGL / Metal OFF / Metal ON / Metal ON / Metal OFF / OpenGL,
with this exact binary and the same inputs. Diagnostic clocks/counters are OFF.
The host must pass proper M1/app/lock/compiler checks and three quiet samples;
background service CPU must remain <=50% of one logical CPU from capture2400
through clean exit. Both ON measurements must be assessed against both OFF
controls and their drift, including total CPU and frame tails. Stop on failure;
no automatic case retry or policy relaxation. Preserve existing upper-screen
cross-renderer precision/edge differences rather than declaring pixel parity.

The blocking service was identified in live macOS logs as APFS defragmentation
on Files and Backups, two volumes of the same external disk. A private 1GiB
byte-identical ROM copy is prepared on Fast so a clean test can be independent
of that disk. No ROM/save/image payload is committed. Temporary normal unmount
of the external volumes requires the user's decision because their files would
be unavailable; no service or user app will be killed to force it.

## Artifacts and reproduction

Small receipt: [hardware gates and counters](../../evidence/m1-metal-index-snapshot-20261127.json).
The patch applies to the private `6b2bd51` parent; actual tested source is
`db208d1` on `archive/m1-metal-profile-20261127`. It includes the previously
frozen CPU observers with their flags OFF in timing runs. A clean production
integration on newer main would need its own build identity and checks.

Local root:
`/Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex/`.
Final gate: `index-snapshot-component-4/RESULT.json`; build:
`index-snapshot-build-1/RESULT.json`; shadow: `index-snapshot-shadow-1/RESULT.json`;
CPU pair: `index-snapshot-diagnostic-1/RESULT.json`.
`INDEX-SNAPSHOT-SOURCE-READY.json` binds the future runner, harness, binaries,
shadow/CPU gates and verified local ROM. Source-only runner review corrected
stale prior-experiment paths before any timing case; the original staged source
and source-ready receipt are retained. No reviewed comparison has run yet.

Do not rerun the closed component or diagnostic cases. The next runtime action
is one admitted `matched_index_snapshot.py` bracket after fresh host checks.
