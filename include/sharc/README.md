# Public headers

`core.hpp` and `memory.hpp` define the contract surface without LLVM types.
The library prepares and executes the partial classic ISA subset documented in
`doc/isa-coverage.md`. Unsupported instructions fail preparation explicitly.
`Memory::complete` lends separate normal-word read/write views with optional
mirrored tails; ownership and geometry are described in `doc/memory.md`.
See `doc/state.md` for the state model and remaining architectural gaps.
