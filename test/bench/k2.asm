// SPDX-License-Identifier: GPL-3.0-only
// I0 and I1 point to the input and output. I2 and I3 step over state pairs.
// I10 walks the circular PM table of b0, b1, b2, a1 and a2 for each stage.
.SECTION/PM seg_pmco;
.GLOBAL _k2;
_k2:
    R8 = 1024;
_sample:
    R0 = DM(0,I0);
    MODIFY(I0,1);
    LCNTR = 4, DO _stage_end UNTIL LCE;
    R1 = DM(I2,M2), R4 = PM(I10,M10);
    R2 = DM(I3,M2), R5 = PM(I10,M10);
    MRF = 0;
    MR1F = R1;
    MRF = MRF + R0 * R4 (SSF);
    R6 = PM(0,I10);
    R4 = PM(1,I10);
    R7 = PM(2,I10);
    MRF = RND MRF (SF);
    R3 = SAT MRF (SF);
    MRF = 0;
    MR1F = R2;
    MRF = MRF + R0 * R5 (SSF);
    MRF = MRF - R3 * R4 (SSF);
    MRF = RND MRF (SF);
    R1 = SAT MRF (SF);
    DM(0,I2) = R1;
    MRF = R0 * R6 (SSF);
    MRF = MRF - R3 * R7 (SSF);
    MRF = RND MRF (SF);
    R2 = SAT MRF (SF);
    DM(0,I3) = R2;
    MODIFY(I2,2);
    MODIFY(I3,2);
    MODIFY(I10,3);
_stage_end:
    R0 = PASS R3;
    DM(0,I1) = R0;
    R8 = R8 - 1;
    MODIFY(I1,1);
    IF NE JUMP(PC,-32);   // _sample
    R9 = R9 - 1;
    R8 = 1024;
    IF NE JUMP(PC,-35);   // _sample
    IDLE;
