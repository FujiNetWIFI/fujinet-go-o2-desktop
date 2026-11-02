; common.inc -- the head of every MCS-48 probe (tools/ab/probes/*.a48).
;
; A probe is an Odyssey 2 "BIOS" of its own: the MAME run loads it as o2bios.rom
; (with a 2K cartridge for the code above $400), and mcs48_ops_test runs the
; same bytes on the C core. So it runs from reset, owns all of memory, and
; must end by falling into `done` ($3FE), the end marker both sides stop at.
;
; The vectors: reset at 0, INT at 3, timer at 7. A probe defines `start`,
; `xint` (the external interrupt) and `tint` (the timer interrupt); the ones
; it does not use are `retr`.
;
; Notes for writing probes (AS quirks):
;   - a trailing backslash in a comment continues the line
;   - conditional jumps (jc, jz, djnz, jb0 ...) are page-relative: the target
;     must be in the page of the instruction (use db for the page edge cases)
;   - labels defined inside a macro are not visible outside it
;   - hex is 0FFh / 0AFh, not 0xFF; the location counter is *
;   - AS puts SEL MB0 / SEL MB1 in front of a JMP or CALL it thinks changes
;     bank; jmpx / callx below emit the bare instruction
;
; SPDX-License-Identifier: GPL-3.0-or-later

    cpu 8048

; jmpx / callx: a JMP / CALL to a 12-bit address without the SEL MB0 / SEL MB1
; that AS would put in front of it. Only bits 8-10 of the address are encoded;
; bit 11 comes from the MB flag at run time, as on the chip.
jmpx macro addr
    db 04h+(((addr)>>8)&7)*20h, (addr)&0FFh
    endm
callx macro addr
    db 14h+(((addr)>>8)&7)*20h, (addr)&0FFh
    endm

    org 0
    jmp start
    org 3
    jmp xint
    org 7
    jmp tint
    org 10h
