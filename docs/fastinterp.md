# FastInterp backend

FastInterp is bundled from Azahar 2127.0-rc2 at the revision in
fastinterp-upstream.json. No upstream download is needed to build Cara.
Copyright and GPL-2.0-or-later notices are retained.

FastInterp is the default no-JIT CPU backend. Set CARA_CPU_BACKEND=dyncom
when launching to use the previous backend for compatibility or comparison. Both are compiled into the same signed
binary, with CPU and vertex-shader JIT disabled. FastInterp decodes instructions
into data records and dispatches to precompiled C++/ARM64 handlers; the block
cache does not contain newly generated native executable code.

The port adapts the older polymorphic thread context and raw page-table APIs,
provides GPU-coherent atomic exclusive-store fallbacks, guards unaligned
cross-page RAM accesses, and repairs single-step Thumb decoding/chaining.
FastInterp uses upstream's per-instruction cycle model and incremental scheduler
tick flushes. DynCom's historical instruction-count budgets differ, so identical
refresh numbers alone do not prove identical guest work between the backends.
Device results must include game progression and rendering checks.

The macOS regression compares ARM/Thumb ALU results, predication and flags against
DynCom, plus context save/restore and address-space changes. This is bounded
coverage, not a complete ARM/VFP conformance suite. The iOS build must pass the
linked no-JIT/executable-memory-symbol audit before installation.

`CARA_FASTINTERP_RAM_BURST=1` enables an experimental RAM transfer path and is
unset/off by default. In addition to multiple-register transfers, scalar
`VLDR`/`VSTR` doubles can copy eight bytes with one page check when the complete
access fits in ordinary mapped RAM. Unaligned same-page accesses are supported;
page crossings, address wrap, MMIO, and rasterizer-tracked memory retain the
ordered 32-bit access fallback. This changes host memory handling only, preserving
guest instructions, tick charges, scheduler boundaries, and floating-point bits.

The native `CARA_FASTINTERP_SCALAR_DOUBLE_TEST=1` regression runs 4,224 off/on
cases through the real decoder and handlers. It checks all 32 double registers,
predication, exact register/memory bits and charged ticks, unaligned/cross-page/
wrap accesses, unmapped memory, tracked-memory pointer transitions, and ordered
MMIO widths, addresses, values, and partial completion. Its throwing MMIO fixture
checks access ordering rather than ARM data-abort conformance. The fixture restores
the prior `CARA_FASTINTERP_RAM_BURST` environment value after constructing both
CPUs. The broader native differential regression also runs with RAM bursts enabled.
These checks establish bounded semantic coverage; they do not establish a device
performance gain or isolate scalar-double costs from other RAM burst paths.

`CARA_FASTINTERP_PRECISE_BLOCK_END` defaults on; explicit `0` retains the old
return-analysis behavior for comparison. Every ARM LDM that loads PC is a flag
liveness barrier, including conditional returns. Previously, decoding beyond a
return could let an unreachable flag setter erase live flags before the return:
`ADDS; LDM {...,pc}; CMP` could return with the pre-ADDS flags. The regression
checks the correct CPSR independently and retains the old mode as a bug witness.

With `CARA_FASTINTERP_CHAIN_BUDGET=1`, unconditional AL LDM-PC returns also stop
decoding at the return. This avoids fetching and profiling unreachable tail
bytes. Conditional LDM-PC spans and legacy chain-budget-off spans retain their
previous shape to preserve failed-predicate slice boundaries and chain quanta.
No instruction tick constants, clocks, SVC charges, or event policies change.
The census exports `precise_block_end_enabled`; shortening additionally requires
the chain-budget condition above. This is a correctness change, not a measured
performance improvement.

`CARA_FASTINTERP_LDM_PC_TEST=1` checks address modes, writeback, conditional
outcomes, Step and cold/warm Run, canonical charged ticks, post-return MMIO fetch
avoidance, text invalidation, and conditional/unconditional flag regressions.
Tiny-budget cases compare Run with an instruction-Step oracle grouped at the
retained control-transfer boundaries, including scheduled-event lateness. S-bit
fixtures preserve the existing handler behavior; they do not establish hardware
SPSR/exception-return conformance. A separate private six-case native probe also
verifies the flag correction with chain budget disabled: unconditional and
conditional returns retain their original decoded spans and charged ticks, while
precise mode restores the independently expected NZCV at a taken return.

Compiler experiments are opt-in: tools/build.py supports --cpu apple-m4 --lto,
--pgo-generate, --pgo-use PROFILE, and --build-dir DIRECTORY. PGO profiles must
come from the same source and target variant. Benchmark runs mute physical audio
by default while retaining audio processing; CARA_BENCHMARK_AUDIO opts into output.

## Unconditional handler entries (experiment)

`CARA_FASTINTERP_AL_DISPATCH=1` selects a default-off predecoded dispatch
index for instructions whose ARM condition is AL. These entries land after
the predicate gate in the same compiled handler body. Conditional instructions
retain the original condition and lazy-flag checks. Handlers with initialization
before their gate, including exclusive-memory helper macros, keep their normal
entry. No executable code is generated, and canonical ticks, event boundaries,
semantic opcodes and conditions remain unchanged.

The index occupies two former padding bytes, retaining a 20-byte `DecodedInst`.
Complete decode, Step, terminators, synthetic prefixes and hot opcode rewrites
prepare it; test-only mode changes clear the decoded cache. Census reports
`al_dispatch_enabled`. The implementation includes table-capacity/layout guards
and independent checks that both dispatch-table halves retain opcode order.

`CARA_FASTINTERP_AL_DISPATCH_TEST=1 cara-fastinterp-self-test` compares disabled
and enabled execution in 9,666 native cases, covering instruction predicates,
NZCV combinations, pending lazy flags, Step, blocks and hot rewrites. Existing
ARM/Thumb, poisoned-decode, SVC, budget, return, cache, ALU operand/pair and VCMP
regressions also passed in both modes. These are correctness checks; device
performance remains an independent measurement.

A subsequent fixed-clock M4 iPad moving-grass comparison on build fingerprint
`64cd37b23a1d4f19595daaf7c3a0307807b98e00c15ad2070e8256bd564a9d3d`
measured 18.274363/18.294200 ms per refresh with checked entries versus
18.054946/18.014223 with unconditional entries. Upload-order reuse was enabled
in both, sound synthesis was active, and guest CPU/speed cap were both 100%.
The repeated means are 18.284282 versus 18.034585 ms (1.37% lower wall time);
emulation-thread CPU means are 16.603791 versus 16.269044 ms. Corresponding
window workloads and endpoint pixels matched exactly. This bounded route result
does not establish locked full speed or every-frame equivalence. The core flag defaults off. After matching 600-refresh
Luigi opening-dialogue, Zelda outdoor-gameplay and Xenoblade intro comparisons
at 100% guest CPU, the iOS GLES frontend selects it by default unless explicitly
overridden. These paced scene pairs all matched pixels/work and were already
at full speed, so no additional cross-title speedup is claimed.

## Secondary successor slot (experiment)

`CARA_FASTINTERP_SECONDARY_LINK=1` is a default-off shortcut for a successful
fallback block lookup. A block remembers the last fallback successor's cache
slot. Before repeating the tag search, the cache validates that slot against the
full current PC tag. A stale or empty hint falls through to the original search.
A valid hint performs the original request/hit accounting and exact NRU touch;
ARM/Thumb and poisoned-entry checks remain with the original caller. Primary
chain selection, guest ticks, chain limits and event checks are unchanged.

The token is cleared on allocation, decode, invalidation and full cache clear.
A recycled slot containing the same PC identifies its current block, so the hint
does not hold an obsolete allocation alive. The additional block-header storage
increases host cache footprint slightly; census exports actual object size,
`secondary_link_enabled`, `cache_secondary_probes` and `cache_secondary_hits`.
Reported tag comparisons remain logical scalar-equivalent prefix probes, not a
count of physically executed comparisons after a shortcut.

The standalone native test compares 800,000 real cache operations, including
exact tags, NRU states, victims and logical counters. The CPU fixture compares
12,288 slices across both AL-entry modes, 4/8-way policies and 75/100/133%
clocks, including SVCs, mode changes and address-space changes. The existing
full instruction and timing regressions pass in all four AL/secondary mode
combinations. Device performance remains a separate measurement.


## Bounded decoded-cache capacity experiment

`CARA_FASTINTERP_CACHE_ENTRIES` accepts exactly `16384` or `32768`; the default
and unsupported-value fallback are `16384`. Capacity is fixed when each CPU's
cache is constructed. Its decoded blocks occupy one contiguous heap allocation
sized to that capacity; metadata arrays support at most 32,768 entries. No live
resize, executable allocation or serialized-cache format is introduced.

Both capacities retain the original PC hash, 4/8-way NRU replacement, primary
chain policy and exact-tag secondary-slot validation. Lookup uses the selected
capacity's set mask and a stable block-storage pointer. Instruction ticks,
decoded block limits, chain budgets and hot-loop iteration limits are unchanged.

The census reports effective `cache_entries` and `cache_sets`, plus
`cache_metadata_bytes`, `cache_block_storage_bytes` and `cache_owned_bytes`.
Owned bytes sum the metadata object and its block allocation, excluding allocator
bookkeeping. `cache_object_bytes` continues to mean `sizeof(BlockCache)`; it no
longer includes the separately allocated blocks. On the native ARM64 build:

| Entries per CPU | Metadata bytes | Block bytes | Owned bytes |
|---|---:|---:|---:|
| 16,384 | 439,456 | 11,010,048 | 11,449,504 |
| 32,768 | 439,456 | 22,020,096 | 22,459,552 |

The standalone capacity test checks strict environment parsing, construction-time
immutability, byte accounting and 400,000 operations against an independent
hash/NRU/victim model. Existing SIMD-tag and secondary-slot differential fixtures
each compare 1,600,000 operations across both capacities; recency/coherence and
lookup-counter fixtures also cover both sizes.

`CARA_FASTINTERP_CAPACITY_TEST=1` runs 24,576 native slices against an independent
original-block/timer/event oracle. It combines both capacities, AL-entry and
secondary-slot modes, 4/8-way policies, 75/100/133% guest clocks, text invalidation
and cache clear. Forced synthetic conflicts produce 851,780 versus 132,216
allocation misses; these are fixture counts, not game measurements. Victim
identities are compared within a policy, not across different capacities.

All 59 native regression configurations passed, including eight full 22,000-case
ARM/Thumb runs with RAM bursts, hot/cold budget oracles, SVC boundaries, LDM-PC,
decode reuse, cache diagnostics and both 4,224-case scalar-double runs. These
checks provide bounded semantic and timing coverage. Device speed and memory
must be measured separately, including a current-build control against the new
heap-backed 16k default before attributing any 32k result to capacity.

## Plain register-copy specialization (private, not integrated)

A November 14, 2026 native M1 experiment selected original ARM `MOV` instructions
with S=0, LSL #0 and no PC operands. It retained conditions, original decoded
slots, tick charges, single-step behavior and existing ALU-pair priority. The
compiled handler removed a shift-byte load and variable shift: two host
instructions, with the same register loads and shared writeback.

The private prototype passed 324,430 register/condition/flag/budget cases per
configuration across both record layouts and operand/pair settings. Existing
instruction regressions and 328,448 metadata walks also passed. Four ABBA
microbenchmark brackets used the same executable off/on, 310 million original
guest instructions per interval, matching ticks and final-state digests, with
operand specialization enabled and ALU pairs disabled.

For the default 20-byte records, MOV-only median wall time changed from
0.496786 to 0.497676 seconds; a MOV/ADD/LDR mixture changed from 0.747127 to
0.742456 seconds. These results do not establish a useful default-layout gain.
The compact 16-byte layout showed a small synthetic signal (roughly 1–1.5%
lower median time). This is not game throughput, iOS or physical-device proof,
and no confidence interval was calculated. The opcode and runtime flag remain
only in the private prototype; this experiment does not change the default
engine. Revisit only with a stronger measured workload or dispatch-level reason.
