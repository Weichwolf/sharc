# Prepared memory access

The initial memory implementation executes classic type 15 pre-indexed transfers
between R0–R15/S0–S15 and declared direct normal-word regions. Other transfers,
runtime I/O callbacks and code-changing stores remain open; this is
not complete memory or ADSP-21489 profile coverage. The reference is Programming
Reference 2.4, sections 6-10, 6-19–6-29, 7-36–7-40, 9-56–9-59 and 10-21/31/32,
pinned in `reference-manifest.json`.

## Layout and storage

A region in the instruction's program or data address space selects the view and
permissions. Data-view regions in the same space must not overlap; an instruction
view may alias a data view. Zero-length regions contribute no addresses. A region
must fit the 32-bit address space.

For the implemented normal-word view, each value is an unsigned 32-bit number
stored in four little-endian bytes. The address selects
`byte_offset + (address - first) * byte_stride` in the lent storage id. A direct
region has `initialized` set, stride at least four, and sufficient actual storage
for its entire declared range. Padding and unaligned byte offsets are supported.
Installation validates geometry and binds actual host pointers. Borrowed nonempty
byte ranges must not overlap under different storage ids: physical storage has
one canonical id, and logical aliases use that id in their layouts. No host address
or memory contents enter LLVM artifacts; loading an artifact does not lower code.

A normal-word load places those 32 bits in register bits 39:8 and clears the
extension byte. A store discards the extension byte. These are raw transfers in
both execution modes; they perform no floating conversion and change no flags.

## Address and lane behavior

The address is I plus the instruction's 32-bit displacement. A type 15 transfer
leaves I unchanged. Circular and bit-reverse post-modification do not apply to
this pre-indexed operation. Programs must obey the manual's restriction that
pre-indexing does not change the address space.

The I register comes from the selected physical DAG bank: I0–3, I4–7, I8–11 and
I12–15 have independent bank selections. Data-file bank selection remains separate.
An explicit S register can transfer in SISD. SIMD transfers the explicit register
and its complement at consecutive normal-word addresses, including odd explicit
addresses. Read broadcast through I1 or I9 loads both complementary registers
from one word independently of PEYEN. Broadcast does not duplicate stores.

For an index unchanged within a unit, LLVM computes the normal-word host
addresses and combines layout, permission
and code-alias proofs before entering the admitted unit or loop. Its loads and
stores use those proven pointers without access checks. If any proof fails, the
same raw transfer lowering runs in the cold exact tail with precise access
failures. An invalid access on an unexecuted branch does not fault early.
Values stay in SSA across known control flow; memory accesses do not split
ordinary blocks for budget accounting. Full consumer-lent PM/DM virtual mappings
and protected-page runtime I/O remain open. A consumer can already lend a complete
normal-word space: `first = 0`, `count = 2^32`, stride four and a 16 GiB virtual
reservation. Its address proof is constant true, so the generated access needs
only base plus the word address scaled by four. The Linux core fixture lends
such a lazily committed reservation and checks SISD/SIMD wrapping from the last
word to word zero in both execution modes. It does not install a fault handler
or model protected holes.

Classic type 19 immediate MODIFY/BITREV updates I in SSA, selecting source and
destination DAG banks independently. Its circular adjustment uses the source B/L,
independently of CBUFEN when L is nonzero; BITREV reverses the adjusted 32-bit
value. The source remains unchanged for a distinct destination. No register-file
lane or floating value is involved. Source I7/I15 wrap events update STKYx and
IRPTL; enhanced overflow routing and immediate-consumer timing retain the
qualification gaps described in `isa-coverage.md`.

For a changing index, a complete normal-word space needs only a fresh address
calculation at each access; no range comparison is emitted. Sparse layouts with
changing indices currently use the cold exact path until their affine proof is
implemented. Both forms use the same raw transfer and DAG instruction lowering.
They never reuse a host pointer computed from an obsolete I value.

## Boundaries and current gaps

Denied reads/writes report `access_fault`. Unprepared addresses, callback regions,
unsupported views and stores whose declared storage may overlap code report
`unsupported_feature`. Diagnostics retain image identity, guest PC, instruction
and data-space/address context. The failed instruction does not retire; completed
instructions and their effects remain. Both SIMD destinations are validated
before a store changes memory, so a missing implicit binding produces no partial
store. These are preparation/access boundaries, not a complete model of hardware
memory exceptions.

Declared byte aliases work for reads. Potential code-alias writes are conservatively
refused before changing bytes. Stores also require complete instruction-view
coverage of the declared code, so opaque instruction callbacks cannot hide an
unknown alias. Actual code transitions, unchanged code stores and
all other memory views still require implementation and independent qualification.
`Io` currently supports installation-time instruction reads; runtime data access
through callbacks is not implemented by this increment.

`set_memory` revokes bindings. A subsequent install binds the new lent storage.
Snapshots restore processor state and continuation, including I registers, and
leave consumer memory alone. No obsolete host pointer is restored with a snapshot.

The installed-image ISA fixtures compare full registers and padded RAM under both
numerical identities, all sixteen index registers, both DAG banks, all data-bank
selections, SISD/SIMD, broadcast and every instruction/cycle prefix. Core fixtures
cover complete fault prefixes, rebinding, code aliases, invalid layouts/storage
and missing implicit SIMD storage. Cached consumer-bound IR and the installed
two-header SDK also execute memory accesses. K4/K5 and their hardware measurements
remain open.
