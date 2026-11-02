# Cara Boost patches

The bundled Boost snapshot remains based on azahar-emu/ext-boost revision
`36603a1e665e849d29b1735a12c0a51284a10dd0`.

The ICL map/set associative-container adaptors are backported from
[boostorg/icl revision b587925](https://github.com/boostorg/icl/tree/b587925c05ab626436822a5d5e3317a5da97f320).
The added files retain their upstream copyright and Boost Software License.
`map_adaptor.hpp` and `set_adaptor.hpp` use the existing bundled implementation
selection instead of the newer `ICL_IMPL_PATH` macro. The two base containers
select these adaptors; other ICL algorithms are unchanged. Cara uses C++17.

This addresses overlapping interval queries with libc++ 22. A surface registered
across existing cached ranges separated by a gap could be registered only over
part of its address range. Subsequent emulated GPU fills then missed that surface,
leaving stale depth values. See upstream issues
[Boost ICL #51](https://github.com/boostorg/icl/issues/51) and
[LLVM #183189](https://github.com/llvm/llvm-project/issues/183189).

`citra-interval-cache-self-test` checks expansion across a gap, overlap lookup,
removal without losing other owners, and invalid-region set operations. With the
unpatched headers and the current Apple compiler it fails at address 100; with
the backport it passes. The test contains no game data.
