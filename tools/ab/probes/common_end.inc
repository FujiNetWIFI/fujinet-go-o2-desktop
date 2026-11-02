; common_end.inc -- the tail of every MCS-48 probe.
;
; A probe ends with `jmp finish`: it folds the 64 bytes of internal RAM into A
; (so a stray store anywhere shows up in the trace), then falls into the end
; marker `done` ($3FE) that both the MAME run and mcs48_ops_test stop at. The
; fold sits at $3E0, so a probe's own code must stay below that in the ROM.
;
; SPDX-License-Identifier: GPL-3.0-or-later

    org 3E0h
finish:
    sel rb0
    clr c
    clr a
    mov r0,#3Fh
fold:
    add a,@r0
    rl a
    djnz r0,fold
    jmp done

    org 3FEh
done:
    jmp done
