# Guest-PC CPU summary

```sh
python3 tools/summarize_guest_pc_cpu.py /private/tmp/fastinterp-blocks.json \
  > /private/tmp/guest-pc-cpu-summary.json
python3 -m unittest discover -s tests -p test_summarize_guest_pc_cpu.py
```

The input is FastInterp's `cores[].guest_pc_cpu` report with kind
`thread_cpu_by_block_start_disjoint_ranges`. That kind string is retained for
compatibility; `mode` selects `disjoint` or `episode`. Reports without `mode`
retain the original disjoint interpretation. The offline tool executes no guest
instructions, reads no ROM, and requires only Python's standard library.

`cpu_ns` is raw instrumented host thread CPU time recorded during the sampled
guest execution intervals. It includes attributed interpreter dispatch, memory
access and SVC work; work outside the instrumented Run intervals is excluded.
It is neither guest CPU cycles nor an extrapolated whole-run CPU total.

The tool checks that configured PC ranges are disjoint and bucket names are
unique. Every thread must have all configured range buckets plus `other_guest`
and `svc_external`, including zeros, in disjoint mode. In episode mode the ranges
are an allowlist and every thread has exactly `episode`, `other_guest` and
`svc_external`. Each thread's buckets must sum exactly to
its CPU total; threads must sum exactly to their core total. Schema errors or
failed reconciliation stop processing.

Sampled refreshes come only from the explicit, ordered, unique absolute frame-ID
list. Window bounds are absolute and end exclusive; configured start offsets are
relative to the base frame. Frame IDs must belong to the listed windows, whose
observed counts must agree with the ID list. Started windows must be closed,
complete and fully observed before per-refresh CPU values are available.
Configured windows which have not started are reported separately and keep
the configured measurement set incomplete, suppressing per-refresh values.

Clock or calibration failures, unassigned intervals, straddling blocks,
thread-table overflow, exhausted sample capacity, frame resets or a false
`coverage_complete` mark the result incomplete. Raw
valid-interval CPU totals remain visible; per-refresh values become `null`.
This deliberately favors explicit incompleteness over a plausible-looking
normalization. Missing or disabled profiling is unavailable, not measured zero.
An aggregate per-refresh result is produced only when measured cores have the
same complete sampled frame IDs. Core refresh counts are never added together.

`clock_read_budget.estimated_cpu_ns` is runtime clock-read count multiplied by
the calibrated per-read cost. This is an approximate overhead budget, displayed
separately and **never subtracted** from the measured CPU totals. Calibration
with zero reads or zero cost is unavailable, not a free clock. Calibration loop
work is not itself included in that estimate.

PC buckets are exclusive by PC location, not by caller. A global integer/float
conversion helper bucket includes every caller of that helper. Adding it to a
polling-loop body does not establish loop-inclusive CPU cost. Exact attribution
through helper calls needs recorded caller/return-address context or a separate
validated inclusive measurement. The summary intentionally emits no inferred
loop-inclusive total.

## Episode measurements

Episode mode reports a per-thread owned span between the explicit start and end
PC fences. It includes allowed helper and SVC work while that guest thread runs;
other guest threads and time outside Run are excluded. Repeated start-fence
visits within an active episode are reentries, not new episodes. Fence tracking
continues outside sampling windows without clock reads, so a window can begin
or end partway through a known episode.

The summary checks that these four raw subtotals sum exactly to the thread's
`episode` CPU bucket:

- `sampled_completed_cpu_ns`: properly ended episodes fully inside sampling.
- `sampled_partial_cpu_ns`: properly ended episodes cut by a sample boundary.
- `sampled_aborted_cpu_ns`: sampled work from episodes aborted by an unsafe exit.
- `active_sampled_cpu_ns`: sampled work still owned by an active episode.

Their sum is `sampled_span_cpu_ns`. With complete window coverage and no errors,
its per-refresh value describes sampled owned-span CPU, including partial spans;
it does not claim that every episode started and finished inside those windows.
`partial_sampled_episodes` counts episode identities, not window segments.
Started/completed/aborted/reentry fence counts include unsampled execution, so the
summary does not divide CPU time by them to invent a per-episode average.

Any `sampled_aborted_episodes` suppresses per-refresh normalization, even if a
malformed producer reports complete coverage. Unsampled aborts remain visible
without invalidating sampled CPU. Inactive episodes cannot retain active CPU or
an active-partial flag. Clock, ownership and window errors retain the original
strict checks. This measurement is independent of the earlier disjoint helper
buckets; adding the old helper totals is unnecessary and would double count.
