# Experimental FastInterp backend

FastInterp is bundled from Azahar 2127.0-rc2 at the revision in
fastinterp-upstream.json. No upstream download is needed to build Cara.
Copyright and GPL-2.0-or-later notices are retained.

Set CARA_CPU_BACKEND=fastinterp when launching to opt in. Without this setting,
Cara retains its tested DynCom backend. Both are compiled into the same signed
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

Compiler experiments are opt-in: tools/build.py supports --cpu apple-m4 --lto,
--pgo-generate, --pgo-use PROFILE, and --build-dir DIRECTORY. PGO profiles must
come from the same source and target variant. Benchmark runs mute physical audio
by default while retaining audio processing; CARA_BENCHMARK_AUDIO opts into output.
