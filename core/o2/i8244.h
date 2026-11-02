/* i8244.h -- the Intel 8244 (NTSC) / 8245 (PAL) video + sound chip, the
 * Odyssey 2 / Videopac "VDC".
 *
 * Transposed from MAME's src/devices/video/i8244.cpp and i8244.h
 * (BSD-3-Clause, copyright-holders Peter Trauner, Wilbert Pol, hap; thanks to
 * Kevin Horton), plus the bits of src/mame/philips/odyssey2.cpp that wire it
 * up (screen set_raw, T1 = vblank | hblank, P17 LUM, the palette).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/video/i8244.cpp; copyright-holders
 *  Peter Trauner, Wilbert Pol, hap -- see COMPLIANCE.md)
 *
 * ---------------------------------------------------------------------------
 * HOW TO DRIVE IT (the machine glue's contract)
 * ---------------------------------------------------------------------------
 *
 * TIME.  Absolute time is a uint64 count of PIXEL CLOCKS (the 8244's
 * "screen" clock: 7.15909 MHz NTSC, 7.09379 MHz PAL), counted from power-on.
 * One 8048 machine cycle is I8244_NTSC_CYCLE_CLOCKS (20) or
 * I8244_PAL_CYCLE_CLOCKS (18) of them, so  now = cycles * 20  (or 18).
 * The beam follows MAME's screen exactly: t = 0 is the START OF VBLANK (line
 * 243, pixel 0), because that is where MAME's screen sits at machine start;
 * line 0 is 20 lines later.  A line is 455 (NTSC) / 456 (PAL) pixel clocks,
 * a frame 263 / 313 lines.
 *
 * MAME QUIRK TO KNOW ABOUT (A/B against MAME only): MAME's 8048 runs 1.4 ppm
 * faster than 20 (18) pixel clocks a machine cycle -- its XTAL class rounds
 * 7.15909 MHz up to 7.1591 MHz for the CPU (5,369,325 Hz, PAL 5,911,500 Hz)
 * but leaves the screen at 7,159,090 Hz (7,093,790 Hz).  A cycle is 19.999972
 * (17.999975) pixel clocks, so against the beam MAME's CPU gains about one
 * pixel every 36k cycles (six frames).  The real console has one crystal and
 * an exact ratio, which is what `now = cycles * 20` gives; core/tests/
 * i8244_test.c's cyc_to_px() uses MAME's ratio only to reproduce MAME's access
 * times in its goldens.  Raster effects placed to the cycle will sit a few
 * pixels apart from MAME's after a few hundred frames.
 *
 * The chip does NO per-clock work.  Its events (hblank every line, vblank
 * once a frame, the screen's frame start/end) are absolute times kept inside;
 * i8244_run_until() fires everything due, and lines are rendered LAZILY, the
 * same way MAME's screen.update_now() does it: only when a register that
 * affects the picture is about to change, when $A1/$A2 are read, when P17
 * (LUM) changes, and at the end of the frame.  That is what makes mid-frame
 * (raster) writes land on the right pixel.
 *
 *   - `now` passed to i8244_read()/i8244_write()/i8244_set_p1_lum() is the
 *     time of the ACCESS as MAME's scheduler would report it.  For the MCS-48
 *     that is the CPU's local time at the access: MAME burns an instruction's
 *     cycles BEFORE doing its work, so a MOVX sees the time at the END of the
 *     instruction (start + its cycle count).  Pass that.
 *   - Every entry point calls i8244_run_until(now) first (events due at or
 *     before `now` happen before the access).  The glue only has to call
 *     i8244_run_until(cpu_now) itself before it tests i8244_irq() at an
 *     instruction boundary, and to keep the CPU from running past
 *     i8244_next_event() without doing so (or simply run_until every
 *     instruction: it returns at once when nothing is due).
 *   - The console's RESET key resets the 8048 only: MAME's i8244 has no
 *     device_reset, so the VDC keeps its registers.  i8244_reset() is the
 *     power-on (device_start) state.
 *
 * WIRING (odyssey2.cpp):
 *   P13 low  -> VDC chip select;   P16 low -> VDC read enabled (reads are
 *               gated by (P1 & 0x48) == 0, writes by (P1 & 0x08) == 0): the
 *               glue does that gating and calls i8244_read/i8244_write with
 *               the low 8 address bits of the MOVX.
 *   P17 (LUM)   -> i8244_set_p1_lum(v, now, (P1 >> 7) & 1) on every P1 write
 *               (it only does work when the bit changes).  P17 = 0 brightens
 *               every pixel; the chip ORs it in as pixels are rendered, so a
 *               change mid-frame splits the picture exactly as in MAME.
 *   T1          -> i8244_t1(v, now)  (= vblank | hblank, a pure function of
 *               the beam position).  For counter mode, i8244_t1_next_edge().
 *   _INTR       -> i8244_irq(v), or the callback; level, asserted by vblank /
 *               the sound shifter / the external overlap input, cleared by a
 *               read of $A1.  The 8048 takes it as its external interrupt.
 *   CX          -> i8244_write_cx() / the line callback (G7400 only; the O2
 *               and G7000 leave the pin unused).
 *   SND         -> i8244_sound_mix() once a frame (below).
 *
 * VIDEO OUT.  A 360x243 frame (I8244_WIDTH x I8244_HEIGHT, MAME's visible
 * area), XRGB 0x00RRGGBB, is published inside the chip at the screen's
 * VBLANK (line 243): `v->frame` / i8244_frame(), `v->frames` counts them.
 * It is what MAME's bitmap holds after update_partial(242), converted with
 * the driver's palette (default: the calibrated one; I8244_PAL_RGB selects
 * the chip's raw RGB palette, MAME's "RGB palette" option).
 *
 * SOUND.  The 24-bit shift register is clocked from the per-line hblank
 * event; each change of the output level (shift output AND enable, with the
 * volume) is recorded as an event (absolute time, level 0..15).  Once a
 * frame the glue calls i8244_run_until(frame_end) and then
 * i8244_sound_mix(v, t0, t1, out, n): n mono floats box-averaged over
 * [t0, t1), already scaled like MAME's final output (see I8244_SOUND_GAIN),
 * consumed events pruned.
 *
 * FONT.  The 512-byte character generator (MAME charset_i8244.bin) is NOT
 * built in.  i8244_init()/i8244_set_font() take a pointer (kept, not copied);
 * NULL draws blank glyphs (self-overlap bookkeeping still happens).
 *
 * THREADS.  None.  Nothing here locks; the debug functions are const and
 * touch no chip state (they re-render from registers into the caller's
 * buffer), but the caller must not run them against a chip another thread is
 * stepping.
 */

#ifndef O2_I8244_H
#define O2_I8244_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- geometry and clocks ------------------------------------------------ */

#define I8244_WIDTH   360      /* MAME's visible area (set_screen_size)      */
#define I8244_HEIGHT  243

#define I8244_NTSC_HTOTAL 455
#define I8244_NTSC_VTOTAL 263
#define I8244_PAL_HTOTAL  456
#define I8244_PAL_VTOTAL  313
#define I8244_NTSC_CYCLE_CLOCKS 20  /* pixel clocks in one 8048 machine cycle */
#define I8244_PAL_CYCLE_CLOCKS  18
#define I8244_NTSC_PIXEL_HZ 7159090.0   /* 7.15909 MHz XTAL (= screen clock) */
#define I8244_PAL_PIXEL_HZ  7093790.0   /* MAME: u32(17.734476 MHz / 5) * 2    */

/* MAME routes the chip to "mono" at 0.25 and the stream sample is 0.5 * duty:
 * a full-volume tone averages 0.125 * 15/16.  i8244_sound_mix() returns that
 * final scale; multiply further for your own master volume. */
#define I8244_SOUND_GAIN 0.125f

/* palette selection (odyssey2.cpp: odyssey2_palette / i8244_palette) */
#define I8244_PAL_O2   0       /* calibrated to a real Videopac (default)   */
#define I8244_PAL_RGB  1       /* raw 8244 RGBI levels                      */

/* ---- layer masks and flags for the debug renderer ----------------------- */

#define I8244_L_BG       0x01  /* background fill                           */
#define I8244_L_GRID     0x02  /* grid: horizontal lines                    */
#define I8244_L_VGRID    0x04  /* grid: vertical lines                      */
#define I8244_L_DOTS     0x08  /* grid: dots                                */
#define I8244_L_SPRITES  0x10  /* the four 8x8 sprites ("minor system")     */
#define I8244_L_SINGLES  0x20  /* the twelve single characters              */
#define I8244_L_QUADS    0x40  /* the four quads of four characters         */
#define I8244_L_ALL      0x7f

#define I8244_F_BOXES        0x01  /* outline every object (+ its index)    */
#define I8244_F_COLLISION    0x02  /* tint pixels that set $A2 bits under the
                                    * current $A2 mask (red)               */
#define I8244_F_OVERLAP      0x04  /* tint any overlap of two objects (yellow) */
#define I8244_F_IGNORE_ENABLE 0x08 /* draw grid/objects although control
                                    * bits 3/5 have them switched off      */
#define I8244_F_SHOW_HIDDEN  0x10  /* ignore the NTSC "Y < 14 is not drawn"
                                    * rule                                 */
#define I8244_F_BEAM         0x20  /* frame-in-progress: cross-hair at the beam */

typedef struct i8244 i8244_t;

/* _INTR output changed to `level` (1 = asserted).  Called inside whichever
 * call caused it. */
typedef void (*i8244_irq_cb)(void *user, int level);

/* Row y, pixels x0..x1 of v->bitmap have just been rendered (raw palette
 * indices 0..7 for background, 8..15 for bright objects; P17 LUM NOT yet
 * applied).  Installing this takes over the LUM step, as vpp_state's
 * screen_update does: the callback must apply i8244_lum(v) itself, and may
 * call i8244_write_cx() for x in x0..x1 and replace pixels.  Without a
 * callback the chip ORs LUM into the row itself. */
typedef void (*i8244_line_cb)(void *user, i8244_t *v, int y, int x0, int x1);

/* ---- the chip ------------------------------------------------------------ */

#define I8244_SND_EVENTS 4096   /* sound level changes kept between mixes */

struct i8244 {
    /* configuration */
    int is_pal;
    const uint8_t *font;          /* 512 bytes or NULL (blank glyphs)       */
    double pixel_hz;
    i8244_irq_cb irq_cb;  void *irq_user;
    i8244_line_cb line_cb; void *line_user;
    uint32_t palette[16];
    int palette_kind;

    /* MAME's set_default_params() */
    int htotal, vtotal;
    int vblank_start, vblank_end;
    int hblank_start, hblank_end;
    int bgate_start;
    int cycle_clocks;             /* pixel clocks per 8048 machine cycle    */
    uint64_t frame_clocks;        /* htotal * vtotal                        */
    uint64_t origin;              /* t = 0 is line 243 pixel 0              */

    /* the register file, MAME's vdc_t ($00-$FF) */
    uint8_t reg[0x100];
    uint16_t collision_map[0x200];
    uint8_t priority_map[0x200];

    uint8_t x_beam_pos;
    uint8_t y_beam_pos;
    uint8_t control_status;       /* $A1 sticky bits 7,6,3,2                */
    uint8_t collision_status;     /* $A2                                    */

    /* sound shifter */
    int sh_written;
    int sh_pending;
    uint8_t sh_prescaler;
    uint8_t sh_count;
    int sh_output;

    int irq;                      /* _INTR level                            */
    int lum;                      /* 8 when P17 is low, else 0              */
    int p17;

    /* MAME-compat: see i8244_set_access_lag() */
    uint64_t access_lag;

    /* the timers, absolute pixel-clock times */
    uint64_t next_hblank;         /* sound_update every line                */
    uint64_t next_vblank;         /* the 8244's own vblank (242:365)        */
    uint64_t next_svblank;        /* screen VBLANK (243:0): frame ends      */
    uint64_t next_sstart;         /* screen line 0: partial updates restart */
    uint64_t next_event;          /* min of the four                        */

    /* MAME screen_device's partial-update state */
    int last_scan;                /* m_last_partial_scan                    */
    int part_hpos;                /* m_partial_scan_hpos                    */

    /* picture */
    uint8_t bitmap[I8244_HEIGHT * I8244_WIDTH];  /* palette indices         */
    uint32_t frame[I8244_HEIGHT * I8244_WIDTH];  /* XRGB, at each VBLANK    */
    uint64_t frames;              /* frames published                       */

    /* sound events: (time, level 0..15) changes */
    uint64_t snd_t[I8244_SND_EVENTS];
    uint8_t snd_level[I8244_SND_EVENTS];
    uint32_t snd_head, snd_count;
    uint8_t snd_base;             /* level before the first queued event    */
    uint8_t snd_cur;              /* level after the last                   */
};

/* ---- life cycle ---------------------------------------------------------- */

/* device_start + set_default_params: is_pal selects the 8245's timing.  Calls
 * i8244_reset(v, 0). */
void i8244_init(i8244_t *v, int is_pal, const uint8_t *font512);
/* Power-on state at absolute time `now` (zeroed registers, timers placed
 * with screen.time_until_pos).  Keeps the font, callbacks and palette. */
void i8244_reset(i8244_t *v, uint64_t now);
void i8244_set_font(i8244_t *v, const uint8_t *font512);
void i8244_set_palette(i8244_t *v, int kind);
void i8244_set_irq_callback(i8244_t *v, i8244_irq_cb cb, void *user);
void i8244_set_line_callback(i8244_t *v, i8244_line_cb cb, void *user);

/* ---- running ------------------------------------------------------------- */

/* Fire every event due at or before `now`. */
void i8244_run_until(i8244_t *v, uint64_t now);
/* The next event's time (the CPU must not run its accesses past it without a
 * run_until). */
static inline uint64_t i8244_next_event(const i8244_t *v) { return v->next_event; }
/* screen().update_now(): render everything up to the beam.  Neutral (MAME
 * would render the same pixels later); the debugger uses it before looking. */
void i8244_catch_up(i8244_t *v, uint64_t now);

/* MOVX accesses (offset = the low 8 address bits).  `now` as described at the
 * top.  Reads have MAME's side effects ($A1 clears IRQ and the sticky bits,
 * $A2 clears the collision latch). */
uint8_t i8244_read(i8244_t *v, uint64_t now, unsigned offset);
void i8244_write(i8244_t *v, uint64_t now, unsigned offset, uint8_t data);
/* MAME ordering of an access against the chip's own timers.  MAME runs the CPU
 * a whole instruction at a time and fires a timer only after the slice ends,
 * so a timer that falls inside the instruction that makes the access (the
 * access is at the END of its MOVX) has NOT happened yet when the access sees
 * the chip, although its time is earlier.  Real hardware has no such rule.  A
 * `lag` of one MOVX (2 cycles = 40 pixel clocks NTSC, 36 PAL) makes read() and
 * write() apply only the events due at or before `now - lag`, which
 * reproduces MAME exactly (the beam position still uses `now`).  Default 0:
 * the event strictly before-or-at the access happens first. */
void i8244_set_access_lag(i8244_t *v, uint64_t lag_px);
/* The debugger's views: no side effects, no catch-up. */
uint8_t i8244_peek(const i8244_t *v, uint64_t now, unsigned offset);
/* Debugger store while stopped: catch up, apply MAME's data masks, ignore the
 * write lockouts and the read-only list. */
void i8244_poke(i8244_t *v, uint64_t now, unsigned offset, uint8_t data);

/* P17: p17 = (P1 >> 7) & 1.  Catches up (with the OLD level) when it changes. */
void i8244_set_p1_lum(i8244_t *v, uint64_t now, int p17);
static inline int i8244_lum(const i8244_t *v) { return v->lum; }

/* The _INTR output. */
static inline int i8244_irq(const i8244_t *v) { return v->irq; }

/* T1 = vblank | hblank, pure functions of the beam at `now`. */
int i8244_vblank(const i8244_t *v, uint64_t now);
int i8244_hblank(const i8244_t *v, uint64_t now);
static inline int i8244_t1(const i8244_t *v, uint64_t now)
{
    return i8244_vblank(v, now) | i8244_hblank(v, now);
}
/* The first time after `now` at which T1 differs from its level at `now`
 * (for T1 counter mode / edge tracking). */
uint64_t i8244_t1_next_edge(const i8244_t *v, uint64_t now);

/* The beam at `now` (screen().vpos()/hpos()). */
void i8244_beam(const i8244_t *v, uint64_t now, int *vpos, int *hpos);

/* write_cx(): the CX pin for pixel x of the line being rendered (call it from
 * the line callback).  Raises the external-overlap interrupt (control bit 4)
 * and the $A2 bit 6 collision like MAME. */
void i8244_write_cx(i8244_t *v, int x, int cx);

static inline const uint32_t *i8244_frame(const i8244_t *v) { return v->frame; }

/* ---- sound --------------------------------------------------------------- */

/* Box-average the recorded level over [t_start, t_end) into n mono samples
 * (sample i covers [t_start + i*L/n, t_start + (i+1)*L/n)), scaled to MAME's
 * output (0 .. I8244_SOUND_GAIN * 15/16, never negative).  Events before
 * t_end are consumed.  Call i8244_run_until(v, t_end) first. */
void i8244_sound_mix(i8244_t *v, uint64_t t_start, uint64_t t_end, float *out, int n);

/* ---- debugger: snapshot --------------------------------------------------- */

typedef struct {
    int index;
    uint8_t reg_y, reg_x, reg_color;
    int x, y;                 /* top-left on screen: (reg_x + 5) * 2, reg_y   */
    int color;                /* 0..7 (bright variant is always drawn)        */
    int zoom;                 /* control/colour bit 2: 4 px per bit, else 2   */
    int s, x9;                /* colour bits 1 and 0                          */
    uint8_t shape[8];         /* 8 rows, bit 0 = leftmost pixel               */
    int width, height;        /* on-screen bounding box (incl. S/X9 shift)    */
    int visible;              /* some part is inside the 360x243 frame        */
    int clipped;              /* ... and some part is outside                 */
} i8244_sprite_info;

typedef struct {
    int index;                /* single: 0..11; quad member: 0..15 (4*quad+j) */
    uint8_t reg_y, reg_x, reg_ptr, reg_color;
    int x, y;                 /* top-left on screen: (reg_x + 5) * 2, reg_y   */
    int color;                /* 0..7 */
    int bit8;                 /* colour bit 0: the font's upper 256 bytes     */
    int rom_offset;           /* first row's byte in the 512-byte font        */
    int glyph;                /* rom_offset / 8 (0..63)                       */
    int first_row;            /* rom_offset % 8                               */
    int rows;                 /* rows drawn (1..8), each 2 lines high         */
    int width, height;        /* 16 x rows*2 on screen                        */
    int ntsc_hidden;          /* NTSC and Y < 14: the chip does not draw it   */
    int visible;              /* drawn (inside the frame, not NTSC-hidden)    */
    int clipped;
} i8244_char_info;

typedef struct {
    int index;                /* 0..3 */
    int x, y;                 /* shared position (the quad's first char)      */
    int rows;                 /* from the 4th character's pointer             */
    i8244_char_info chars[4]; /* positions after MAME's mirroring, 32 px pitch */
} i8244_quad_info;

typedef struct {
    /* every register, raw */
    uint8_t regs[0x100];

    /* timing */
    int is_pal;
    double pixel_hz, line_hz, frame_hz;
    int hpos, vpos;           /* the beam at the snapshot's time              */
    int x_beam, y_beam;       /* what $A5 / $A4 read live                     */
    uint8_t x_beam_latched, y_beam_latched;  /* strobe-latched values         */
    int last_scan, part_hpos; /* how far the lazy renderer has got            */
    uint64_t frames;

    /* $A0 control */
    uint8_t control;
    int ctl_bit0;             /* bit 0 (undocumented / unused by MAME)        */
    int strobe;               /* bit 1: beam position latch / live            */
    int sound_irq_en;         /* bit 2                                        */
    int grid_en;              /* bit 3                                        */
    int ext_irq_en;           /* bit 4: external (CX) overlap interrupt       */
    int fg_en;                /* bit 5: foreground (objects) enabled          */
    int dots_en;              /* bit 6                                        */
    int fill;                 /* bit 7: vertical grid lines 16 units wide     */
    /* write lockouts (MAME write()/read()) */
    int lock_objects;         /* $00-$7F ignore writes / read 0 (fg on)       */
    int lock_grid;            /* $C0-$FF ignore writes / read 0 (grid on)     */

    /* $A1 status as a read would give it now (no side effects) */
    uint8_t status;
    int st_hstatus, st_strobe, st_sound, st_vblank, st_ext_overlap, st_char_overlap;

    /* $A2 */
    uint8_t col_mask;         /* what the program wrote                       */
    uint8_t col_result;       /* the accumulated collision latch (read value) */

    /* $A3 colours */
    uint8_t color_reg;
    int bg_color;             /* 0..7 */
    int grid_color;           /* 0..7 */
    int grid_lum;             /* bit 6                                        */
    int p17_lum;              /* 8 when P17 is low                            */

    /* sound */
    uint8_t snd_reg;          /* $AA                                          */
    uint32_t shift;           /* 24-bit register ($A7..$A9)                   */
    int snd_enable, snd_fast, snd_noise, snd_volume;
    int sh_output, sh_count, sh_prescaler, sh_pending, sh_written;
    double shift_clock_hz;    /* shift rate: line rate / 4 (fast) or / 16     */
    int pattern_period;       /* smallest repeat of the 24 bits (1,2,3,4,6,8,12,24); 0 in noise mode */
    double freq_hz;           /* tone frequency; 0 for DC and noise           */

    /* objects */
    i8244_sprite_info sprites[4];
    i8244_char_info singles[12];
    i8244_quad_info quads[4];

    /* grid */
    uint16_t hgrid[9];        /* column i: bit y = horizontal line y (9 rows) */
    uint8_t vgrid[10];        /* column i: bit y = vertical line, row y (8 rows) */
    int grid_x0, grid_y0, cell_w, cell_h;  /* 13 (units: 2 px), 24, 16, 24    */
    int irq;
} i8244_snapshot;

/* Decode everything.  `now` only positions the beam and the live $A1/$A4/$A5. */
void i8244_snapshot_get(const i8244_t *v, uint64_t now, i8244_snapshot *s);

/* A register's name ("SPR0 Y", "CHAR3 PTR", "QUAD1 CHAR2 COLOR", "CONTROL",
 * "HGRID1 7", ...) with its mirror note, and a decoded value line. */
void i8244_describe_register(const i8244_t *v, unsigned offset, char *name, size_t name_n,
                             char *text, size_t text_n);

/* ---- debugger: renderers --------------------------------------------------- */

/* Re-render the CURRENT register state (not the beam-accurate frame) into
 * dst (dst_w x dst_h XRGB, stride dst_w; only the top-left 360x243 is
 * drawn), with the layers in `layers` (I8244_L_*) and `flags` (I8244_F_*).
 * Uses the chip's palette, LUM and font; changes no chip state. */
void i8244_debug_render(const i8244_t *v, unsigned layers, unsigned flags,
                        uint32_t *dst, int dst_w, int dst_h);

/* The frame so far: lines the lazy renderer has completed (plus the part of
 * the current line) in colour, the rest of the previous frame dimmed, and
 * with I8244_F_BEAM a cross-hair at the beam.  Call i8244_catch_up() first
 * for "to the beam".  Fills dst (360x243, stride 360); beam_x/y optional. */
void i8244_frame_in_progress(const i8244_t *v, uint64_t now, unsigned flags,
                             uint32_t *dst, int *beam_x, int *beam_y);

/* Object bitmaps for the tables.  Each draws the object as the chip would,
 * every font/shape pixel `scale` x `scale` (use 2 for true screen size),
 * lit pixels in the object's colour, the rest `bg` (XRGB; 0xFF000000 and
 * friends are fine, the high byte is yours).  *w,*h receive the size; dst may
 * be NULL to just ask; otherwise it must hold *w * *h pixels (stride *w) and
 * cap is checked (returns -1 if too small). */
int i8244_render_sprite(const i8244_t *v, int index, int scale, uint32_t bg,
                        uint32_t *dst, size_t cap, int *w, int *h);
/* kind: 0 = single (index 0..11), 1 = quad member (index 0..15) */
int i8244_render_char(const i8244_t *v, int kind, int index, int scale, uint32_t bg,
                      uint32_t *dst, size_t cap, int *w, int *h);
/* One font glyph (0..63), 8x8 logical pixels, lit pixels `fg`. */
int i8244_render_glyph(const i8244_t *v, int glyph, int scale, uint32_t fg, uint32_t bg,
                       uint32_t *dst, size_t cap, int *w, int *h);
/* The font as a sheet of 64 glyphs, 16 per row; rows 0-1 are the lower 256
 * bytes (colour bit 0 = 0), rows 2-3 the upper (bit 0 = 1); 1 logical pixel
 * gap. */
int i8244_render_font_sheet(const i8244_t *v, int scale, uint32_t fg, uint32_t bg,
                            uint32_t *dst, size_t cap, int *w, int *h);
/* The four sprite shape tables side by side, in their colours. */
int i8244_render_shape_sheet(const i8244_t *v, int scale, uint32_t bg,
                             uint32_t *dst, size_t cap, int *w, int *h);
/* The 16 palette entries as XRGB. */
void i8244_palette_colors(const i8244_t *v, uint32_t out[16]);
/* A strip: 8 colours dim above 8 bright, cell x cell pixels each. */
int i8244_render_palette_strip(const i8244_t *v, int cell, uint32_t *dst, size_t cap,
                               int *w, int *h);

/* Name of a layer bit / of a colour index, for labels. */
const char *i8244_colour_name(int colour8);

#ifdef __cplusplus
}
#endif

#endif /* O2_I8244_H */
