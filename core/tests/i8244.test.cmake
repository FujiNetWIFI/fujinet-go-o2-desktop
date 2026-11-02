# i8244_test: the Intel 8244 / 8245 video + sound chip (core/o2/i8244.c).
#
# Replays 94 cartridge programs -- static scenes of every object type and
# control combination, random scenes, register mirrors / masks / lockouts, the
# beam registers (live and strobe-latched), raster effects landing mid-line,
# LUM changes mid-frame, and the sound shifter (slow, fast, noise, rewrites,
# volume, the sound IRQ) on NTSC and PAL -- through the C chip and requires the
# frame MAME produced (a hash of every frame), every value MAME's program read
# back, and MAME's 48 kHz sound edges.  The goldens (i8244_golden.h) come from
# tools/ab/gen_goldens.py; MAME ran on a generated test font, so no ROM is
# needed here.  Beside them: the T1 / vblank / hblank positions, the IRQ
# timing, the sound mixer, the CX input and line callback, the debugger's
# snapshot, layer-masked renderer and object bitmaps.
#
# Dev mode (needs MAME and the carts): `i8244_test --replay LOG --frames DUMP`,
# see tools/ab/real_carts.py.
o2_test(i8244_test i8244_test.c)
add_test(NAME i8244 COMMAND i8244_test)
