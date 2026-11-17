// SPDX-License-Identifier: GPL-3.0-only
// Authored classic ISA K4; the consumer places the three PM sections.
// Entry: R0=3, R1=0x2468ace0, R3=3, R4=1048576, R5=table, R7=7.
// MODE1=0; M8=0; all DAG lengths/bases=0; eight table words name the handlers.
.SECTION/PM k4_main;
.GLOBAL _k4;
_k4:
    R0 = R0 AND R7;
    R2 = R0 + R5;
    I0 = R2;
    R2 = DM(0,I0);
    I8 = R2;
    CALL(M8,I8);
    R4 = R4 - 1;
    IF NE JUMP _k4;
    NOP;
.SECTION/PM k4_handlers;
_handler0:
    R1 = R1 + R0;
    R2 = 1;
    R1 = R1 XOR R2;
    R0 = 3;
    CALL _common;
    RTS;
_handler1:
    R1 = R1 + R0;
    R2 = 2;
    R1 = R1 XOR R2;
    R0 = 4;
    CALL _common;
    RTS;
_handler2:
    R1 = R1 + R0;
    R2 = 3;
    R1 = R1 XOR R2;
    R0 = 5;
    CALL _common;
    RTS;
_handler3:
    R1 = R1 + R0;
    R2 = 4;
    R1 = R1 XOR R2;
    R0 = 6;
    CALL _common;
    RTS;
_handler4:
    R1 = R1 + R0;
    R2 = 5;
    R1 = R1 XOR R2;
    R0 = 7;
    CALL _common;
    RTS;
_handler5:
    R1 = R1 + R0;
    R2 = 6;
    R1 = R1 XOR R2;
    R0 = 0;
    CALL _common;
    RTS;
_handler6:
    R1 = R1 + R0;
    R2 = 7;
    R1 = R1 XOR R2;
    R0 = 1;
    CALL _common;
    RTS;
_handler7:
    R1 = R1 + R0;
    R2 = 8;
    R1 = R1 XOR R2;
    R0 = 2;
    CALL _common;
    RTS;
.SECTION/PM k4_common;
_common:
    R2 = ROT R0 BY -1;
    R1 = R1 XOR R2;
    R1 = R1 + R3;
    RTS;
