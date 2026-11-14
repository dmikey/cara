# Paired floating-point loads

`CARA_FASTINTERP_VLDR_PAIRS=1` enables a default-off interpreter experiment.
It combines dispatch for two unconditional ARM VLDR.S instructions sharing a
non-PC base register. Register numbers and offsets need not be contiguous;
positive/negative strides and repeated destinations retain their original order.

The decoder retains both original slots, sizes and tick costs. It does not
extend a block, cross an original decode boundary, or change the scheduler's
block-level stopping policy. Single stepping still decodes one instruction.
At execution, both words must lie wholly in one directly mapped RAM page.
Tracked graphics memory, MMIO, crossings and register-backed aliases use the
original scalar handlers. Two ordered word loads preserve the original bit
patterns without floating-point arithmetic. No code is generated at runtime.

## Validation

Build `cara-fastinterp-self-test`, then run:

```sh
python3 tools/check_fastinterp_vldr_pairs.py /path/to/cara-fastinterp-self-test
```

Each of four runs covers 81,920 differential cases, original 31-slot boundaries,
positive/negative 16-byte strides, unaligned/page-crossing addresses, predicates,
arbitrary/repeated FP destinations, and context round trips. Separate cases
check PC operands, single stepping, register-backed source/destination aliasing,
ordered MMIO callbacks, real rasterizer tracking, and second-word invalidation.
The script compares complete state and mixed-budget execution traces with the
candidate enabled and disabled in both chaining-budget modes. The tracked-memory
host fixture has no renderer; it verifies the coherent fallback is taken, not
physical GPU cache behavior.

The full existing 22,000-case ARM/Thumb differential suite also passes with the
candidate and normal RAM/AL-dispatch specializations enabled. Native M1 synthetic
30-load blocks measured 46.18/41.25, 48.08/45.22 and 49.98/44.58 nanoseconds per block
(original/paired). Each pass calls ExecuteBlock 1,000,000 times, executing 30 loads
and charging 30,000,000 guest ticks; the reported denominator is block calls,
not individual loads. These are short load-dispatch
measurements, not gameplay results. The candidate stays disabled until matched
gameplay demonstrates improvement with unchanged work, images and progression.

For the optional synthetic measurement, set
`CARA_FASTINTERP_VLDR_PAIR_TEST=1 CARA_FASTINTERP_VLDR_PAIRS=1 CARA_FASTINTERP_VLDR_PAIR_MICROBENCH=1`.
