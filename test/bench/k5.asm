// SPDX-License-Identifier: GPL-3.0-only
// Authored classic ISA K5. The offline image in k5.cpp encodes these instructions.
// Entry: I0=input, I1=output, R3=0, R4=512; MODE1=0; L0=L1=0.
.SECTION/PM seg_pmco;
.GLOBAL _k5;
_k5:
    R2 = 4096;
_word:
    R0 = DM(0,I0);
    R3 = R3 + R0;
    DM(0,I1) = R0;
    MODIFY(I0,1);
    MODIFY(I1,1);
    R2 = R2 - 1;
    IF NE JUMP(PC,-6);   // _word
    MODIFY(I0,-4096);
    MODIFY(I1,-4096);
    R4 = R4 - 1;
    IF NE JUMP(PC,-11);  // _k5
    NOP;
