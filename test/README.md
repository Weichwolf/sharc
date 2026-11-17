# Independent verification

Exactly three verification layers follow `doc/core-contract.md`:

- `isa/`: authored installed-image instruction programs, with independent fixed
  ALU and paired-status branch expectations under Strict/FastFP32 identities.
- `core/`: public preparation and lifecycle boundaries, artifacts, consumer-linked
  IR, exact run bounds, raw state, snapshots, installation and code validation.
- `bench/`: K1–K6 with complete output and separate numerical-mode measurements.
  No kernels are implemented yet.

`support/isa.hpp` authors instruction fields independently of production. Rejection
fixtures use public `Core::lower`; execution fixtures use declared, installed
LLVM images. There is no private decoder or instruction-capture test interface.

`support/reference.py` supplies the S2 integer/rational oracle. Run
`python3 test/support/generate_expectations.py` to validate and export expectations.
This generation step is not a fourth test layer or ISA acceptance. Value-only
floating records and unqualified cases cannot establish complete instruction
coverage. See `doc/numeric-reference.md` for its domain and remaining gaps.

Builds and tests require no manuals, manufacturer code, network or device firmware.
The installed CMake consumer uses only the two public headers and executes its
own instruction program. Linux x86-64 is tested; macOS ARM64 remains untested.
