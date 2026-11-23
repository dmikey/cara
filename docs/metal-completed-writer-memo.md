# Terminal-success Metal surface-writer memo

`CARA_METAL_CACHE_COMPLETED_WRITERS=1` is a default-off CPU-bookkeeping
experiment. `MetalSurfaceWriter` can publish an atomic success proof only after
its own command is Completed without an error, all predecessor commands and
receipts are Complete, and its own receipt (if present) belongs to that exact
command and is Complete. Pending and Failed writer results are never memoized.

[Apple's command-buffer status contract](https://developer.apple.com/documentation/metal/mtlcommandbuffer/status)
states that lifecycle states advance in one direction and Completed/Error are
final. The writer's resource, command, predecessor snapshot, receipt, generation,
range and exporter fields are immutable. Once the full success proof is stored
with release ordering, later acquire reads may avoid repeated Objective-C status,
error and already-complete wait calls. Resources are never cleared by the memo;
concurrent readers and later exports retain the original ownership. A successful
`ValidateProducerCompletion` establishes the same proof as a successful Status.

Covers still checks resource/command/queue, nonzero generation, exporter and
requested range. It skips only the redundant command-status read after known
success. Export still checks output size and calls the original exporter, whose
own failures remain authoritative. No shader validation, predecessor check,
queue retirement or guest-memory coverage guard is removed. The flag is separate
from completed-predecessor memoization, and no shared default is changed.

## Validation

```sh
python3 tests/run_metal_completed_writers_test.py
```

The runner requires a complete matching engine archive, rejecting stale headers.
The local v31 build passed all four fresh configurations: writer memo OFF/ON ×
predecessor memo OFF/ON, with Metal API Validation enabled. Counted command
adapters forward to actual Metal command buffers and prove repeated own status,
error and wait observations disappear only after validated success when enabled.
The fixture also covers:

- Initially unsubmitted and GPU-gated pending producers, and an independently
  completed writer whose predecessor remains pending.
- GPU-written error flags in either receipt buffer, mismatched own/predecessor
  receipts, malformed predecessor count, and failed predecessors.
- Authored own-command status/error failures; these do not inject a real driver
  fault or violate the actual Metal command's final-state contract.
- Missing resource/generation/exporter, invalid ranges/output size, exact GPU
  export bytes and a proof first established through blocking validation.
- Nine concurrent readers polling, waiting and exporting across valid and invalid
  own/predecessor receipt completion. Pending exports remain blocked.

Results and exact build commands are retained in
`work/portable-metal-checks/completed-writers/`. This proves the bounded contract
on the local GPU, not a throughput improvement or every cross-device fault mode.
Matched real-scene controls must determine whether the saved CPU bookkeeping
improves useful work before promotion.

## Luigi matched result: no demonstrated gain

The v31 FastInterp build ran an OFF/ON/ON/OFF restored Gadd sequence from the
same verified global-3000 checkpoint, measuring local refreshes 120–600. All
same-build pixel, CPU/tick, input, checkpoint, filesystem and policy guards pass.

| Arm | Memo | Completed ms/ref | Thread CPU ms/ref |
| --- | --- | ---: | ---: |
| A | OFF | 16.684749 | 14.446756 |
| B | ON | 16.690297 | 14.504836 |
| C | ON | 16.703190 | 14.453221 |
| D | OFF | 16.702244 | 14.610215 |

Mean elapsed time is 16.693497 OFF versus 16.696744 ON. The approximately
0.049 ms/ref lower mean thread CPU with ON is smaller than the control drift;
this sequence does not establish a useful gain. Keep the feature default-off.
Near-cap restored timings are not uninterrupted normal-play acceptance.
Receipts: `work/luigi-metal-cce1a67/gadd-v31-restored-{control-A,writer-B,writer-C,control-D}`.
