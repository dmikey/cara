# VS shader packet census

`CARA_PICA_COMMAND_PROFILE=1` also counts packet metadata for a possible repeated
VS program/swizzle upload optimization. It does not batch, skip, compare or
read ahead into payloads. The original header write and scalar extra-word loop
are unchanged, including invalid-offset prefixes and existing truncated-list
behavior. No guest timing policy changes. Profiling adds overhead: counts are
frequency evidence, **not exclusive CPU time or predicted savings**.

Observation is after the original scalar header. Only VS program/swizzle ports
are classified; direct GS uploads and every other packet remain in `all_packets`
only. `all_packets` counts observations after the scalar header returns; a
throwing header is not observed. Counters distinguish repeated/grouped headers and extra-length buckets:
0, 1, 2–3, 4–7, 8–15, 16–31, 32–63, 64–127, 128–511, 512–2047.

`eligible` means repeated, at least two extras, a complete **declared** extra
span and enough post-header capacity (VS program 512, swizzle 4096). It describes
a future seam, not an executed optimization. It proves neither actual physical
backing bounds nor general grouped-packet validity. `repeated_capacity_packets`
is a hypothetical repeated-stream capacity check even on grouped rows. Empty
spans at exactly capacity fit; nonempty spans there do not. All 16 masks,
including zero, are allowed: special shader data consumes raw payloads, while
backing-register writes remain masked. Mask and mirroring histograms count
packets. `extra_words` sums the header's declared extra length, including
incomplete declared spans; it does not verify payload contents or physical reads.
Eligible extra totals use the whole-span eligibility predicate. Both count each
incoming declared extra once, excluding the header;
existing `code_words`/`swizzle_words` may count VS→GS destination stores twice.

Every existing 256-list profile endpoint gains a separate schema-1 block: begin,
four totals rows, forty length rows (including explicit zeros), four mask rows,
and end. All rows carry process-session and snapshot identifiers; the footer
repeats `all_packets`. Original options/profile log schemas stay unchanged.
Session counters are cumulative, include warmup/restoration, and are not
serialized or reset on save-state loading. Difference two complete endpoints
in one session; do not interpret a missing/incomplete block as zero.

Disabled mode performs no classification, clocks, allocations or logging for
this census: one cached flag per list and a guarded branch per header remain.
The fixed-size counters and integer classifier live in
`emulator/src/video_core/pica_shader_packet_profile.h`. No trace-enabled probe is
called. This diagnostic does not repair the scalar parser's pre-existing lack
of header/extra bounds checks.

Summarize a complete log with `python3 tools/summarize_shader_packets.py LOG`.
Use `--session ID --snapshots FIRST LAST` to select exact cumulative endpoints.
The helper rejects missing, duplicate, reset or inconsistent rows; percentages
use source-packet/extra-word denominators, never mirrored-store or CPU-time units.
The existing `summarize_pica_commands.py` format and behavior are unchanged.
