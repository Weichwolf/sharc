# Independent verification

- `isa/`: authored installed-image instruction programs, with independent fixed
  ALU, paired-status branch and direct-memory expectations under Strict/FastFP32
  identities.
- `core/`: public preparation and lifecycle boundaries, artifacts, consumer-linked
  IR, exact run bounds, raw state, snapshots, installation, code validation and memory binding/refusal.
- `bench/`: checked K4 and K5 workloads with separate numerical-mode
  measurements. K1–K3 and K6 remain open.

`support/isa.hpp` authors instruction fields independently of production. Tests
use public `Core::lower` for rejection and installed LLVM images for execution.

`support/reference.py` supplies the S2 integer/rational oracle. Run
`python3 test/support/generate_expectations.py` to validate and export expectations.
See `doc/numeric-reference.md` for the oracle’s domain and coverage limits.

Builds and tests require no manuals, manufacturer code, network or device firmware.
The installed CMake consumer uses only the two public headers and executes its
own instruction program. Linux x86-64 is tested; macOS ARM64 remains untested.

Run the suites with `-DSHARC_POISON_NATIVE_STATE=ON` to trap on generated
writes to old pinned register/status fields before the common spill. The
option is disabled by default. Loop controls check instruction prefixes and
deadlines across complete and partial grants in both numeric identities and
low data banks. DAG tests cover register selectors, quarter banks, linear and
circular arithmetic, overflow latches and moving-index memory faults.

See the [benchmark protocol and results](bench/README.md) for checked runs
and independent Clang O3 measurements.
