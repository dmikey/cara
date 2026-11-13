# Offline guest hot-range mapping

`tools/map_guest_hot_ranges.py` reads an existing FastInterp resident-block
census, groups block starts into explicitly supplied half-open virtual address
ranges, and optionally verifies captured source bytes against a privately decoded
image. It uses Python's standard library, executes no guest code, and emits no
instruction bytes. Keep ROMs, decoded images and disassembly outside the repository.

Example using invented addresses and private input files:

```sh
python3 tools/map_guest_hot_ranges.py /private/tmp/census.json \
  --range copy=0x101000:0x101100 \
  --range pacing=0x202000:0x203000 \
  --image /private/tmp/decoded-code.bin \
  --segment 0x100000:0:0x400000 > /private/tmp/ranges.json
python3 -m unittest discover -s tests -p test_map_guest_hot_ranges.py
```

Specify each image segment as `virtual_address:file_offset:byte_length`; segments
must not overlap in virtual address space. Captured bytes may cross adjacent
segments. A source mismatch or missing image mapping gives a failing exit status;
absent captured bytes are explicitly unavailable. No image means no identity
verification. Byte matches establish image correspondence only for captured
blocks, not symbols, function boundaries, instruction-set mode outside those
blocks, or the semantic identity of a function.

The census is **not a time profiler**. FastInterp emits only its hottest 128
resident decoded blocks per core, excludes evicted entry counts, and counts block entries rather than individual instruction dispatches. A range's count is the sum of its included block counters, not function
calls or individual instructions. Overlapping ranges are independent; never add
them as exclusive categories. No automatic per-frame normalization is provided.

Review function boundaries and direct callers separately with a static decoder.
Mixed ARM/Thumb code, inline literal pools, indirect calls, tail branches and
inlining make raw instruction-pattern call scans candidates, not proof. A
behavioral name such as plane classification should remain an inference unless
symbols or a separately verified source identity establish the exact name.

## Existing runtime diagnostics and a focused follow-up

`ARM_FastInterp::WriteBlockProfile` exports resident hotness, not CPU samples.
The `last_guest_pc` FrameTiming field is an execution-exit snapshot. Existing
guest GPU traces record sleep requests and scheduler events, but not every
GetSystemTick call. No current flag attributes CPU milliseconds to arbitrary
guest PC ranges.

A low-overhead next probe could aggregate GetSystemTick call counts by guest
thread and guest return address over the existing bounded trace window. It must
count without recording every call or altering the existing 150-tick charge.
Such counts identify a polling callsite and distinguish it from HID or explicit
SleepThread requests, but remain counts rather than CPU time.

CPU attribution needs an independent sampled timer or bounded generic PC fences.
A fence that spans helper calls and SVCs must include those costs while excluding
execution of other guest threads after a scheduler switch. Measuring only Run's
entry PC misattributes chained decoded blocks. Instrumentation must account for
all chain paths and preserve timer flushes, reschedules, guest instruction counts,
and memory/FP behavior. A polling loop alone does not justify skipping guest work
or changing timer timing.
