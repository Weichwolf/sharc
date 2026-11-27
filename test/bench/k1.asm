// SPDX-License-Identifier: GPL-3.0-only
// At entry I0 points to the first sample after zero history, I1 to the output.
// R5 holds the block count.
// I2 walks backward by M2. I10 wraps around the 64 coefficients with M10 = 1.
.SECTION/PM seg_pmco;
.GLOBAL _k1;
_k1:
    R7 = 1024;
_sample:
    I2 = I0;
    MODIFY(I0,1);
    MRF = 0;
    R0 = DM(I2,M2), R4 = PM(I10,M10);
    LCNTR = 63, DO _tap UNTIL LCE;
_tap:
    MRF = MRF + R0 * R4 (SSF), R0 = DM(I2,M2), R4 = PM(I10,M10);
    MRF = MRF + R0 * R4 (SSF);
    MRF = RND MRF (SF);
    R3 = SAT MRF (SF);
    DM(0,I1) = R3;
    R7 = R7 - 1;
    MODIFY(I1,1);
    IF NE JUMP(PC,-12);   // _sample
    R5 = R5 - 1;
    R7 = 1024;
    IF NE JUMP(PC,-15);   // _sample
    IDLE;

_irq2:
    R6 = R6 + 1;
    RTI;
