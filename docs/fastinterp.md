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
