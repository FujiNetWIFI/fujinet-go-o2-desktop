/* dasm_golden.h -- MAME's own disassembly of every I8048 opcode, from
 * src/devices/cpu/mcs48/mcs48dsm.cpp compiled standalone (tools/ab/
 * mcs48_dasm_golden.cpp, run by tools/ab/gen_dasm_golden.sh). GENERATED;
 * dasm_test.c holds dasm48.c to it.
 *
 * Each opcode is disassembled at the four pc/operand pairs of
 * dasm_variants; a one-byte opcode has one text, a two-byte opcode four.
 * flags is MAME's result >> 28: 8 = SUPPORTED, 4 = STEP_OUT, 2 = STEP_OVER,
 * 1 = STEP_COND. */
static const struct { uint16_t pc; uint8_t operand; } dasm_variants[4] = {
    { 0x123, 0x5A },
    { 0x0FF, 0x00 },
    { 0x7FE, 0xFF },
    { 0x8A0, 0xC3 },
};
static const struct { uint8_t len; uint8_t flags; const char *text[4]; } dasm_golden[256] = {
    { 1, 8, { "nop" } },   /* 00 */
    { 1, 8, { "idl" } },   /* 01 */
    { 1, 8, { "outl bus,a" } },   /* 02 */
    { 2, 8, { "add  a,#$5A", "add  a,#$00", "add  a,#$FF", "add  a,#$C3" } },   /* 03 */
    { 2, 8, { "jmp  $05A", "jmp  $000", "jmp  $0FF", "jmp  $0C3" } },   /* 04 */
    { 1, 8, { "en   i" } },   /* 05 */
    { 1, 8, { "illegal" } },   /* 06 */
    { 1, 8, { "dec  a" } },   /* 07 */
    { 1, 8, { "ins  a,bus" } },   /* 08 */
    { 1, 8, { "in   a,p1" } },   /* 09 */
    { 1, 8, { "in   a,p2" } },   /* 0A */
    { 1, 8, { "illegal" } },   /* 0B */
    { 1, 8, { "movd a,p4" } },   /* 0C */
    { 1, 8, { "movd a,p5" } },   /* 0D */
    { 1, 8, { "movd a,p6" } },   /* 0E */
    { 1, 8, { "movd a,p7" } },   /* 0F */
    { 1, 8, { "inc  @r0" } },   /* 10 */
    { 1, 8, { "inc  @r1" } },   /* 11 */
    { 2, 9, { "jb0  $15A", "jb0  $100", "jb0  $7FF", "jb0  $0C3" } },   /* 12 */
    { 2, 8, { "addc a,#$5A", "addc a,#$00", "addc a,#$FF", "addc a,#$C3" } },   /* 13 */
    { 2, 10, { "call $05A", "call $000", "call $0FF", "call $0C3" } },   /* 14 */
    { 1, 8, { "dis  i" } },   /* 15 */
    { 2, 9, { "jtf  $15A", "jtf  $100", "jtf  $7FF", "jtf  $0C3" } },   /* 16 */
    { 1, 8, { "inc  a" } },   /* 17 */
    { 1, 8, { "inc  r0" } },   /* 18 */
    { 1, 8, { "inc  r1" } },   /* 19 */
    { 1, 8, { "inc  r2" } },   /* 1A */
    { 1, 8, { "inc  r3" } },   /* 1B */
    { 1, 8, { "inc  r4" } },   /* 1C */
    { 1, 8, { "inc  r5" } },   /* 1D */
    { 1, 8, { "inc  r6" } },   /* 1E */
    { 1, 8, { "inc  r7" } },   /* 1F */
    { 1, 8, { "xch  a,@r0" } },   /* 20 */
    { 1, 8, { "xch  a,@r1" } },   /* 21 */
    { 1, 8, { "illegal" } },   /* 22 */
    { 2, 8, { "mov  a,#$5A", "mov  a,#$00", "mov  a,#$FF", "mov  a,#$C3" } },   /* 23 */
    { 2, 8, { "jmp  $15A", "jmp  $100", "jmp  $1FF", "jmp  $1C3" } },   /* 24 */
    { 1, 8, { "en   tcnti" } },   /* 25 */
    { 2, 9, { "jnt0 $15A", "jnt0 $100", "jnt0 $7FF", "jnt0 $0C3" } },   /* 26 */
    { 1, 8, { "clr  a" } },   /* 27 */
    { 1, 8, { "xch  a,r0" } },   /* 28 */
    { 1, 8, { "xch  a,r1" } },   /* 29 */
    { 1, 8, { "xch  a,r2" } },   /* 2A */
    { 1, 8, { "xch  a,r3" } },   /* 2B */
    { 1, 8, { "xch  a,r4" } },   /* 2C */
    { 1, 8, { "xch  a,r5" } },   /* 2D */
    { 1, 8, { "xch  a,r6" } },   /* 2E */
    { 1, 8, { "xch  a,r7" } },   /* 2F */
    { 1, 8, { "xchd a,@r0" } },   /* 30 */
    { 1, 8, { "xchd a,@r1" } },   /* 31 */
    { 2, 9, { "jb1  $15A", "jb1  $100", "jb1  $7FF", "jb1  $0C3" } },   /* 32 */
    { 1, 8, { "illegal" } },   /* 33 */
    { 2, 10, { "call $15A", "call $100", "call $1FF", "call $1C3" } },   /* 34 */
    { 1, 8, { "dis  tcnti" } },   /* 35 */
    { 2, 9, { "jt0  $15A", "jt0  $100", "jt0  $7FF", "jt0  $0C3" } },   /* 36 */
    { 1, 8, { "cpl  a" } },   /* 37 */
    { 1, 8, { "illegal" } },   /* 38 */
    { 1, 8, { "outl p1,a" } },   /* 39 */
    { 1, 8, { "outl p2,a" } },   /* 3A */
    { 1, 8, { "illegal" } },   /* 3B */
    { 1, 8, { "movd p4,a" } },   /* 3C */
    { 1, 8, { "movd p5,a" } },   /* 3D */
    { 1, 8, { "movd p6,a" } },   /* 3E */
    { 1, 8, { "movd p7,a" } },   /* 3F */
    { 1, 8, { "orl  a,@r0" } },   /* 40 */
    { 1, 8, { "orl  a,@r1" } },   /* 41 */
    { 1, 8, { "mov  a,t" } },   /* 42 */
    { 2, 8, { "orl  a,#$5A", "orl  a,#$00", "orl  a,#$FF", "orl  a,#$C3" } },   /* 43 */
    { 2, 8, { "jmp  $25A", "jmp  $200", "jmp  $2FF", "jmp  $2C3" } },   /* 44 */
    { 1, 8, { "strt cnt" } },   /* 45 */
    { 2, 9, { "jnt1 $15A", "jnt1 $100", "jnt1 $7FF", "jnt1 $0C3" } },   /* 46 */
    { 1, 8, { "swap a" } },   /* 47 */
    { 1, 8, { "orl  a,r0" } },   /* 48 */
    { 1, 8, { "orl  a,r1" } },   /* 49 */
    { 1, 8, { "orl  a,r2" } },   /* 4A */
    { 1, 8, { "orl  a,r3" } },   /* 4B */
    { 1, 8, { "orl  a,r4" } },   /* 4C */
    { 1, 8, { "orl  a,r5" } },   /* 4D */
    { 1, 8, { "orl  a,r6" } },   /* 4E */
    { 1, 8, { "orl  a,r7" } },   /* 4F */
    { 1, 8, { "anl  a,@r0" } },   /* 50 */
    { 1, 8, { "anl  a,@r1" } },   /* 51 */
    { 2, 9, { "jb2  $15A", "jb2  $100", "jb2  $7FF", "jb2  $0C3" } },   /* 52 */
    { 2, 8, { "anl  a,#$5A", "anl  a,#$00", "anl  a,#$FF", "anl  a,#$C3" } },   /* 53 */
    { 2, 10, { "call $25A", "call $200", "call $2FF", "call $2C3" } },   /* 54 */
    { 1, 8, { "strt t" } },   /* 55 */
    { 2, 9, { "jt1  $15A", "jt1  $100", "jt1  $7FF", "jt1  $0C3" } },   /* 56 */
    { 1, 8, { "da   a" } },   /* 57 */
    { 1, 8, { "anl  a,r0" } },   /* 58 */
    { 1, 8, { "anl  a,r1" } },   /* 59 */
    { 1, 8, { "anl  a,r2" } },   /* 5A */
    { 1, 8, { "anl  a,r3" } },   /* 5B */
    { 1, 8, { "anl  a,r4" } },   /* 5C */
    { 1, 8, { "anl  a,r5" } },   /* 5D */
    { 1, 8, { "anl  a,r6" } },   /* 5E */
    { 1, 8, { "anl  a,r7" } },   /* 5F */
    { 1, 8, { "add  a,@r0" } },   /* 60 */
    { 1, 8, { "add  a,@r1" } },   /* 61 */
    { 1, 8, { "mov  t,a" } },   /* 62 */
    { 1, 8, { "illegal" } },   /* 63 */
    { 2, 8, { "jmp  $35A", "jmp  $300", "jmp  $3FF", "jmp  $3C3" } },   /* 64 */
    { 1, 8, { "stop tcnt" } },   /* 65 */
    { 1, 8, { "illegal" } },   /* 66 */
    { 1, 8, { "rrc  a" } },   /* 67 */
    { 1, 8, { "add  a,r0" } },   /* 68 */
    { 1, 8, { "add  a,r1" } },   /* 69 */
    { 1, 8, { "add  a,r2" } },   /* 6A */
    { 1, 8, { "add  a,r3" } },   /* 6B */
    { 1, 8, { "add  a,r4" } },   /* 6C */
    { 1, 8, { "add  a,r5" } },   /* 6D */
    { 1, 8, { "add  a,r6" } },   /* 6E */
    { 1, 8, { "add  a,r7" } },   /* 6F */
    { 1, 8, { "addc a,@r0" } },   /* 70 */
    { 1, 8, { "addc a,@r1" } },   /* 71 */
    { 2, 9, { "jb3  $15A", "jb3  $100", "jb3  $7FF", "jb3  $0C3" } },   /* 72 */
    { 1, 8, { "illegal" } },   /* 73 */
    { 2, 10, { "call $35A", "call $300", "call $3FF", "call $3C3" } },   /* 74 */
    { 1, 8, { "ent0 clk" } },   /* 75 */
    { 2, 9, { "jf1  $15A", "jf1  $100", "jf1  $7FF", "jf1  $0C3" } },   /* 76 */
    { 1, 8, { "rr   a" } },   /* 77 */
    { 1, 8, { "addc a,r0" } },   /* 78 */
    { 1, 8, { "addc a,r1" } },   /* 79 */
    { 1, 8, { "addc a,r2" } },   /* 7A */
    { 1, 8, { "addc a,r3" } },   /* 7B */
    { 1, 8, { "addc a,r4" } },   /* 7C */
    { 1, 8, { "addc a,r5" } },   /* 7D */
    { 1, 8, { "addc a,r6" } },   /* 7E */
    { 1, 8, { "addc a,r7" } },   /* 7F */
    { 1, 8, { "movx a,@r0" } },   /* 80 */
    { 1, 8, { "movx a,@r1" } },   /* 81 */
    { 1, 8, { "illegal" } },   /* 82 */
    { 1, 12, { "ret" } },   /* 83 */
    { 2, 8, { "jmp  $45A", "jmp  $400", "jmp  $4FF", "jmp  $4C3" } },   /* 84 */
    { 1, 8, { "clr  f0" } },   /* 85 */
    { 2, 9, { "jni  $15A", "jni  $100", "jni  $7FF", "jni  $0C3" } },   /* 86 */
    { 1, 8, { "illegal" } },   /* 87 */
    { 2, 8, { "orl  bus,#$5A", "orl  bus,#$00", "orl  bus,#$FF", "orl  bus,#$C3" } },   /* 88 */
    { 2, 8, { "orl  p1,#$5A", "orl  p1,#$00", "orl  p1,#$FF", "orl  p1,#$C3" } },   /* 89 */
    { 2, 8, { "orl  p2,#$5A", "orl  p2,#$00", "orl  p2,#$FF", "orl  p2,#$C3" } },   /* 8A */
    { 1, 8, { "illegal" } },   /* 8B */
    { 1, 8, { "orld p4,a" } },   /* 8C */
    { 1, 8, { "orld p5,a" } },   /* 8D */
    { 1, 8, { "orld p6,a" } },   /* 8E */
    { 1, 8, { "orld p7,a" } },   /* 8F */
    { 1, 8, { "movx @r0,a" } },   /* 90 */
    { 1, 8, { "movx @r1,a" } },   /* 91 */
    { 2, 9, { "jb4  $15A", "jb4  $100", "jb4  $7FF", "jb4  $0C3" } },   /* 92 */
    { 1, 12, { "retr" } },   /* 93 */
    { 2, 10, { "call $45A", "call $400", "call $4FF", "call $4C3" } },   /* 94 */
    { 1, 8, { "cpl  f0" } },   /* 95 */
    { 2, 9, { "jnz  $15A", "jnz  $100", "jnz  $7FF", "jnz  $0C3" } },   /* 96 */
    { 1, 8, { "clr  c" } },   /* 97 */
    { 2, 8, { "anl  bus,#$5A", "anl  bus,#$00", "anl  bus,#$FF", "anl  bus,#$C3" } },   /* 98 */
    { 2, 8, { "anl  p1,#$5A", "anl  p1,#$00", "anl  p1,#$FF", "anl  p1,#$C3" } },   /* 99 */
    { 2, 8, { "anl  p2,#$5A", "anl  p2,#$00", "anl  p2,#$FF", "anl  p2,#$C3" } },   /* 9A */
    { 1, 8, { "illegal" } },   /* 9B */
    { 1, 8, { "anld p4,a" } },   /* 9C */
    { 1, 8, { "anld p5,a" } },   /* 9D */
    { 1, 8, { "anld p6,a" } },   /* 9E */
    { 1, 8, { "anld p7,a" } },   /* 9F */
    { 1, 8, { "mov  @r0,a" } },   /* A0 */
    { 1, 8, { "mov  @r1,a" } },   /* A1 */
    { 1, 8, { "illegal" } },   /* A2 */
    { 1, 8, { "movp a,@a" } },   /* A3 */
    { 2, 8, { "jmp  $55A", "jmp  $500", "jmp  $5FF", "jmp  $5C3" } },   /* A4 */
    { 1, 8, { "clr  f1" } },   /* A5 */
    { 1, 8, { "illegal" } },   /* A6 */
    { 1, 8, { "cpl  c" } },   /* A7 */
    { 1, 8, { "mov  r0,a" } },   /* A8 */
    { 1, 8, { "mov  r1,a" } },   /* A9 */
    { 1, 8, { "mov  r2,a" } },   /* AA */
    { 1, 8, { "mov  r3,a" } },   /* AB */
    { 1, 8, { "mov  r4,a" } },   /* AC */
    { 1, 8, { "mov  r5,a" } },   /* AD */
    { 1, 8, { "mov  r6,a" } },   /* AE */
    { 1, 8, { "mov  r7,a" } },   /* AF */
    { 2, 8, { "mov  @r0,#$5A", "mov  @r0,#$00", "mov  @r0,#$FF", "mov  @r0,#$C3" } },   /* B0 */
    { 2, 8, { "mov  @r1,#$5A", "mov  @r1,#$00", "mov  @r1,#$FF", "mov  @r1,#$C3" } },   /* B1 */
    { 2, 9, { "jb5  $15A", "jb5  $100", "jb5  $7FF", "jb5  $0C3" } },   /* B2 */
    { 1, 8, { "jmpp @a" } },   /* B3 */
    { 2, 10, { "call $55A", "call $500", "call $5FF", "call $5C3" } },   /* B4 */
    { 1, 8, { "cpl  f1" } },   /* B5 */
    { 2, 9, { "jf0  $15A", "jf0  $100", "jf0  $7FF", "jf0  $0C3" } },   /* B6 */
    { 1, 8, { "illegal" } },   /* B7 */
    { 2, 8, { "mov  r0,#$5A", "mov  r0,#$00", "mov  r0,#$FF", "mov  r0,#$C3" } },   /* B8 */
    { 2, 8, { "mov  r1,#$5A", "mov  r1,#$00", "mov  r1,#$FF", "mov  r1,#$C3" } },   /* B9 */
    { 2, 8, { "mov  r2,#$5A", "mov  r2,#$00", "mov  r2,#$FF", "mov  r2,#$C3" } },   /* BA */
    { 2, 8, { "mov  r3,#$5A", "mov  r3,#$00", "mov  r3,#$FF", "mov  r3,#$C3" } },   /* BB */
    { 2, 8, { "mov  r4,#$5A", "mov  r4,#$00", "mov  r4,#$FF", "mov  r4,#$C3" } },   /* BC */
    { 2, 8, { "mov  r5,#$5A", "mov  r5,#$00", "mov  r5,#$FF", "mov  r5,#$C3" } },   /* BD */
    { 2, 8, { "mov  r6,#$5A", "mov  r6,#$00", "mov  r6,#$FF", "mov  r6,#$C3" } },   /* BE */
    { 2, 8, { "mov  r7,#$5A", "mov  r7,#$00", "mov  r7,#$FF", "mov  r7,#$C3" } },   /* BF */
    { 1, 8, { "illegal" } },   /* C0 */
    { 1, 8, { "illegal" } },   /* C1 */
    { 1, 8, { "illegal" } },   /* C2 */
    { 1, 8, { "illegal" } },   /* C3 */
    { 2, 8, { "jmp  $65A", "jmp  $600", "jmp  $6FF", "jmp  $6C3" } },   /* C4 */
    { 1, 8, { "sel  rb0" } },   /* C5 */
    { 2, 9, { "jz   $15A", "jz   $100", "jz   $7FF", "jz   $0C3" } },   /* C6 */
    { 1, 8, { "mov  a,psw" } },   /* C7 */
    { 1, 8, { "dec  r0" } },   /* C8 */
    { 1, 8, { "dec  r1" } },   /* C9 */
    { 1, 8, { "dec  r2" } },   /* CA */
    { 1, 8, { "dec  r3" } },   /* CB */
    { 1, 8, { "dec  r4" } },   /* CC */
    { 1, 8, { "dec  r5" } },   /* CD */
    { 1, 8, { "dec  r6" } },   /* CE */
    { 1, 8, { "dec  r7" } },   /* CF */
    { 1, 8, { "xrl  a,@r0" } },   /* D0 */
    { 1, 8, { "xrl  a,@r1" } },   /* D1 */
    { 2, 9, { "jb6  $15A", "jb6  $100", "jb6  $7FF", "jb6  $0C3" } },   /* D2 */
    { 2, 8, { "xrl  a,#$5A", "xrl  a,#$00", "xrl  a,#$FF", "xrl  a,#$C3" } },   /* D3 */
    { 2, 10, { "call $65A", "call $600", "call $6FF", "call $6C3" } },   /* D4 */
    { 1, 8, { "sel  rb1" } },   /* D5 */
    { 1, 8, { "illegal" } },   /* D6 */
    { 1, 8, { "mov  psw,a" } },   /* D7 */
    { 1, 8, { "xrl  a,r0" } },   /* D8 */
    { 1, 8, { "xrl  a,r1" } },   /* D9 */
    { 1, 8, { "xrl  a,r2" } },   /* DA */
    { 1, 8, { "xrl  a,r3" } },   /* DB */
    { 1, 8, { "xrl  a,r4" } },   /* DC */
    { 1, 8, { "xrl  a,r5" } },   /* DD */
    { 1, 8, { "xrl  a,r6" } },   /* DE */
    { 1, 8, { "xrl  a,r7" } },   /* DF */
    { 1, 8, { "illegal" } },   /* E0 */
    { 1, 8, { "illegal" } },   /* E1 */
    { 1, 8, { "illegal" } },   /* E2 */
    { 1, 8, { "movp3 a,@a" } },   /* E3 */
    { 2, 8, { "jmp  $75A", "jmp  $700", "jmp  $7FF", "jmp  $7C3" } },   /* E4 */
    { 1, 8, { "sel  mb0" } },   /* E5 */
    { 2, 9, { "jnc  $15A", "jnc  $100", "jnc  $7FF", "jnc  $0C3" } },   /* E6 */
    { 1, 8, { "rl   a" } },   /* E7 */
    { 2, 10, { "djnz r0,$15A", "djnz r0,$100", "djnz r0,$7FF", "djnz r0,$0C3" } },   /* E8 */
    { 2, 10, { "djnz r1,$15A", "djnz r1,$100", "djnz r1,$7FF", "djnz r1,$0C3" } },   /* E9 */
    { 2, 10, { "djnz r2,$15A", "djnz r2,$100", "djnz r2,$7FF", "djnz r2,$0C3" } },   /* EA */
    { 2, 10, { "djnz r3,$15A", "djnz r3,$100", "djnz r3,$7FF", "djnz r3,$0C3" } },   /* EB */
    { 2, 10, { "djnz r4,$15A", "djnz r4,$100", "djnz r4,$7FF", "djnz r4,$0C3" } },   /* EC */
    { 2, 10, { "djnz r5,$15A", "djnz r5,$100", "djnz r5,$7FF", "djnz r5,$0C3" } },   /* ED */
    { 2, 10, { "djnz r6,$15A", "djnz r6,$100", "djnz r6,$7FF", "djnz r6,$0C3" } },   /* EE */
    { 2, 10, { "djnz r7,$15A", "djnz r7,$100", "djnz r7,$7FF", "djnz r7,$0C3" } },   /* EF */
    { 1, 8, { "mov  a,@r0" } },   /* F0 */
    { 1, 8, { "mov  a,@r1" } },   /* F1 */
    { 2, 9, { "jb7  $15A", "jb7  $100", "jb7  $7FF", "jb7  $0C3" } },   /* F2 */
    { 1, 8, { "illegal" } },   /* F3 */
    { 2, 10, { "call $75A", "call $700", "call $7FF", "call $7C3" } },   /* F4 */
    { 1, 8, { "sel  mb1" } },   /* F5 */
    { 2, 9, { "jc   $15A", "jc   $100", "jc   $7FF", "jc   $0C3" } },   /* F6 */
    { 1, 8, { "rlc  a" } },   /* F7 */
    { 1, 8, { "mov  a,r0" } },   /* F8 */
    { 1, 8, { "mov  a,r1" } },   /* F9 */
    { 1, 8, { "mov  a,r2" } },   /* FA */
    { 1, 8, { "mov  a,r3" } },   /* FB */
    { 1, 8, { "mov  a,r4" } },   /* FC */
    { 1, 8, { "mov  a,r5" } },   /* FD */
    { 1, 8, { "mov  a,r6" } },   /* FE */
    { 1, 8, { "mov  a,r7" } },   /* FF */
};
