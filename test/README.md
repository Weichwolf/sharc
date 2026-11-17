# Independent verification

Exactly three verification layers follow `doc/core-contract.md`:

- `isa/`: authored installed-image instruction programs, with independent fixed
  ALU, paired-status branch and direct-memory expectations under Strict/FastFP32
  identities.
- `core/`: public preparation and lifecycle boundaries, artifacts, consumer-linked
  IR, exact run bounds, raw state, snapshots, installation, code validation and memory binding/refusal.
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

The explicit `-DSHARC_POISON_NATIVE_STATE=ON` CMake build poisons the old pinned
register/status fields during generated execution and traps on an unexpected
write before the common spill. Run the same ISA, core and installed SDK tests
in that build; it is excluded from production code by default. Loop controls
check every instruction prefix and cycle deadline across complete grants and
their final partial turn, in both numeric identities and both low data banks.

`isa/dag.cpp` independently encodes immediate index modification/reversal and
compares complete states for all source/destination choices, DAG quarter banks,
linear/circular arithmetic and canonical overflow latches. Moving-index memory
controls exercise complete-space grants and exact sparse failures; the small
copy/checksum control is not a common benchmark.

`bench_k5` is the first common benchmark correctness case. It executes a complete
4096-word copy/checksum workload exceeding ten million guest instructions in
both numeric identities and checks all output, guards and progress. Hardware
measurement is an explicit `--measure` run, separate from CTest. See
`bench/README.md` for the reproducible protocol and the opaque Clang O3 reference.
