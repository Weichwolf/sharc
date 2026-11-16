# Implementation ownership

`decode.cpp` validates declared image boundaries and recognizes top-level classic
ISA and VISA formats during preparation. It does not yet validate every operand
or normalize compute operations. `core.cpp` supplies public preparation refusal,
raw state inspection and snapshot replacement. All lowering and native execution
remain unimplemented; no interpreter or host emitter exists.

LLVM Support supplies SHA-256 for preparation identity. Later native lowering must
follow the SSA and block-accounting rules in `doc/architecture.md` and the common
contract. Device knowledge and peripheral implementations belong to consumers.
