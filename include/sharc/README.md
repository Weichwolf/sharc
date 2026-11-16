# Public headers

`core.hpp` and `memory.hpp` define the contract surface without LLVM types.
The current library recognizes preparation formats and supports raw architectural
state storage; it explicitly refuses all native preparation/execution.
See `doc/state.md` for the storage model and its unimplemented semantics.
