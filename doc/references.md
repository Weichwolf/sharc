# Primary references

Start with the SHARC Processor Programming Reference revision 2.4 for the core
programmer's model, instruction formats, SIMD, numeric modes, sequencer, DAGs
and ISA/VISA details. Use ADSP-214xx Hardware Reference revision 1.1 to define
memory aliases, interrupt/I/O boundaries and which effects belong to consumers.
The ADSP-2148x datasheet supplies derivative-specific capacities and features.
Manufacturer-authored mirror copies are identified explicitly in the manifest;
their hashes prove the local file identity, not equality to an unfetched official PDF.

doc/reference-manifest.json is the authoritative local-file inventory. Entries
record manufacturer URLs, the actual source used, revision, byte count, SHA-256,
page count and provenance limitations. PDFs/text stay ignored under doc/local/.
Verification runs offline; fetching never bypasses a changed identity silently.

Read the manufacturer's license/copyright terms in each document. Local access
does not grant redistribution rights. No manual content or manufacturer program
bytes are compiled into this project, and no manuals are committed to Git.

The initial design was compared read-only with ../cpu12 and ../dsp56300 at
revisions 717346a and ec25ce0 respectively. Their generic native-region,
ownership and verification principles are a design reference; their CPU/DSP
instruction semantics do not implement this target architecture.

## Local PDF inventory

| Document | Revision | Pages | Manufacturer source |
|---|---|---:|---|
| SHARC Processor Programming Reference | 2.4, April 2013 | 694 | [Official reference](https://www.analog.com/media/en/dsp-documentation/processor-manuals/ADSP-2136x_2137x_214xx_pgr_rev2.4.pdf) |
| ADSP-214xx SHARC Processor Hardware Reference | 1.1, April 2013 | 1304 | [Official reference](https://www.analog.com/media/en/dsp-documentation/processor-manuals/adsp-214xx_hwr_rev1.1.pdf) |

Use the filenames in reference-manifest.json to open the corresponding PDFs and
extracted .txt files under doc/local/. All local copies have verified identities.

## Additional manufacturer reference

[ADSP-2148x datasheet, revision I](https://www.analog.com/media/en/technical-documentation/data-sheets/ADSP-21483_21486_21487_21488_21489.pdf)
is an online derivative reference. Direct PDF download timed out; it is not
claimed as a locally verified PDF. The complete core programming and hardware
manuals listed above are available locally.
