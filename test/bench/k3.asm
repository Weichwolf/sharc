// SPDX-License-Identifier: GPL-3.0-only
// I3 points to interleaved, bit-reversed input. Output overwrites the same words.
// I11 points to complex twiddles and I9 to the eight stage descriptors.
.SECTION/PM seg_pmco;
.GLOBAL _k3;
_k3:
    I8 = I9;
    R14 = 8;
_stage:
    I1 = I3;
    R8 = PM(0,I8);
    R9 = PM(1,I8);
    R10 = PM(2,I8);
    R11 = PM(3,I8);
    M8 = R11;
    MODIFY(I8,4);
_group:
    I2 = I1;
    R13 = I1;
    R13 = R13 + R10;
    I10 = I11;
    I1 = R13;
    LCNTR = R9, DO _butterfly_end UNTIL LCE;
    R5 = PM(1,I10);
    R0 = DM(I2,M0), R4 = PM(I10,M8);
    R1 = DM(1,I2);
    R2 = DM(0,I1);
    R3 = DM(1,I1);
    MRF = R2 * R4 (SSF);
    MRF = MRF - R3 * R5 (SSF);
    R6 = RND MRF (SF);
    MRF = R2 * R5 (SSF);
    MRF = MRF + R3 * R4 (SSF);
    R7 = RND MRF (SF);
    R2 = -R6;
    R3 = -R7;
    R2 = (R0 + R2)/2;
    R0 = (R0 + R6)/2;
    R3 = (R1 + R3)/2;
    R1 = (R1 + R7)/2;
    DM(0,I2) = R0;
    DM(1,I2) = R1;
    DM(0,I1) = R2;
    DM(1,I1) = R3;
    MODIFY(I2,2);
_butterfly_end:
    MODIFY(I1,2);
    R8 = R8 - 1;
    IF NE JUMP(PC,-30);  // _group
    R14 = R14 - 1;
    IF NE JUMP(PC,-39);  // _stage
    R12 = R12 - 1;
    I3 = I1;
    IF NE JUMP(PC,-44);  // _k3
    IDLE;
