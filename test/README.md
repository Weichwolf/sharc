# Independent verification plan

Use isa/ for independent encodings/semantics, core/ for memory, exceptions and
architectural boundaries, jit/ for code generation and ownership, bench/ for
checked continuous workloads, and support/ for independent fixture construction.
No ISA tests exist yet; an empty test directory is not passing coverage.

Expected results must not be computed with production semantics. Include every
legal form, aliases, rejected forms, numerical boundaries, partial fault effects,
interrupt admission and self-modifying code. Test x86-64 and ARM64 independently.
Keep manufacturer firmware and program extracts out of fixtures and benchmarks.
