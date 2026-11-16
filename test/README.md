# Independent verification

Exactly three verification layers follow `doc/core-contract.md`:

- `isa/`: instruction programs executed through declared, installed LLVM images,
  with independently calculated expected results. No execution coverage yet.
- `core/`: public preparation and lifecycle boundaries. The initial tests exercise
  format/compute recognition, declared PC boundaries, reserved-opcode rejection,
  raw state storage, snapshot replacement and an installed CMake consumer. They do not call private decoder functions.
- `bench/`: K1–K6 with complete output checks and separate Strict/FastFP32 results.
  No kernels are implemented yet.

`support/isa.hpp` authors numerical instruction fields independently of production.
Preparation rejection tests deliberately use `Core::lower`; there is no successful
execution path or installed image in this increment. They prove structural format
recognition and explicit refusal, not legal compute operands or ISA semantics.
State storage round trips are lifecycle checks, not arithmetic expectations.

Build and tests require no manuals, manufacturer code, network or device firmware.
