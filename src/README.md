# Implementation ownership

`decode.cpp` validates declared image boundaries and recognizes top-level classic
ISA and VISA formats during preparation. `compute.cpp` normalizes the compute
fields and rejects reserved compute opcodes/modifiers. Transfer operand restrictions and all execution semantics
remain unqualified. `core.cpp` supplies public preparation refusal,
raw state inspection and snapshot replacement. All lowering and native execution
remain unimplemented; no interpreter or host emitter exists.

LLVM Support supplies SHA-256 for preparation identity. Later native lowering must
follow the SSA and block-accounting rules in `doc/architecture.md` and the common
contract. Device knowledge and peripheral implementations belong to consumers.
