/* machine.c -- the Odyssey 2 / Videopac console: memory and I/O maps, the P1/P2
 * decode, the keyboard matrix and the joysticks, the VDC's interrupt and T1,
 * the cartridge slot and the mix of the console's sound.
 *
 * Transposed from MAME's src/mame/philips/odyssey2.cpp (BSD-3-Clause,
 * copyright-holders Peter Trauner, Wilbert Pol, hap -- see COMPLIANCE.md).
 * See o2_internal.h for how the pieces are clocked.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>

#include "o2_internal.h"

/* ---- the machines ---------------------------------------------------------- */

int o2_machine_is_pal(int machine)
{
    return machine != O2_MACHINE_ODYSSEY2;
}

const char *o2_machine_name(int machine)
{
    switch (machine) {
    case O2_MACHINE_ODYSSEY2:      return "Magnavox Odyssey 2 (NTSC)";
    case O2_MACHINE_VIDEOPAC:      return "Philips Videopac G7000 (PAL)";
    case O2_MACHINE_VIDEOPAC_PLUS: return "Philips Videopac+ G7400 (PAL)";
    default:                       return "Odyssey 2";
    }
}

/* ---- helpers -------------------------------------------------------------- */

static inline uint64_t now_of(const o2_machine_t *m)
{
    return o2_machine_now(m);
}

/* std::countl_zero(u8) */
static int countl_zero8(uint8_t v)
{
    int n = 0;

    if (!v)
        return 8;
    while (!(v & 0x80)) {
        v = (uint8_t)(v << 1);
        n++;
    }
    return n;
}

uint8_t o2_machine_key_row(const o2_machine_t *m, int sel)
{
    return (uint8_t)(m->keys >> ((sel & 7) * 8));
}

static inline void bus_hook(o2_machine_t *m, unsigned flag, int kind, uint16_t addr, uint8_t data)
{
    if ((m->watch & flag) && m->bus_hook)
        m->bus_hook(m, m->bus_hook_user, kind, addr, data);
}

/* ---- the CPU's buses (odyssey2.cpp) ------------------------------------------ */

/* odyssey2_mem: $000-$3FF the internal ROM, $400-$FFF the slot. */
static uint8_t program_read(void *user, uint16_t addr, int kind)
{
    o2_machine_t *m = user;
    uint8_t data;

    addr &= 0xfff;
    data = addr < O2_BIOS_SIZE ? m->rom[addr] : o2_cart_read(m->cart, addr, 0);
    if (kind == MCS48_READ_DATA)
        bus_hook(m, O2_WATCH_MEMR, O2_BUS_MEMR, addr, data);
    return data;
}

/* odyssey2_io: io_read() */
static uint8_t io_read(void *user, uint8_t offset)
{
    o2_machine_t *m = user;
    uint8_t data = o2_cart_io_read(m->cart, offset, 0);

    /* P14, A7: RAM _CS */
    if (!(m->p1 & 0x10) && !(offset & 0x80))
        data &= m->ram[offset];

    /* P13: 824x _CS, 824x RD is only enabled if P16 is low */
    if ((m->p1 & 0x48) == 0)
        data &= i8244_read(&m->vdc, now_of(m), offset);

    /* G7400, vpp_state::io_vpp: P15 low selects the EF9341, A2 is its R/W */
    if (m->have_ef && !(m->p1 & 0x20))
        data = ef9340_io(&m->ef, m->insn_start, offset, data);

    bus_hook(m, O2_WATCH_XR, O2_BUS_XR, offset, data);
    return data;
}

/* io_write() */
static void io_write(void *user, uint8_t offset, uint8_t data)
{
    o2_machine_t *m = user;

    bus_hook(m, O2_WATCH_XW, O2_BUS_XW, offset, data);

    /* external write is only enabled if P16 is low */
    if (!(m->p1 & 0x40)) {
        o2_cart_io_write(m->cart, offset, data);
        if (!(m->p1 & 0x10) && !(offset & 0x80))
            m->ram[offset] = data;
    }

    if (!(m->p1 & 0x08))
        i8244_write(&m->vdc, now_of(m), offset, data);

    if (m->have_ef && !(m->p1 & 0x20))
        ef9340_io(&m->ef, m->insn_start, offset, data);
}

static void port_write(void *user, int port, uint8_t data)
{
    o2_machine_t *m = user;

    if (port == 1) {
        /* LUM changed: the VDC catches the picture up with the old level */
        i8244_set_p1_lum(&m->vdc, now_of(m), (data >> 7) & 1);
        m->p1 = data;
        o2_cart_write_p1(m->cart, m->p1 & 0x13);
        bus_hook(m, O2_WATCH_P1, O2_BUS_P1W, 1, data);
    } else {
        m->p2 = data;
        o2_cart_write_p2(m->cart, m->p2 & 0x0f);
        if (m->have_ef)
            i8243_p2_w(&m->expander, m->p2 & 0x0f);
        bus_hook(m, O2_WATCH_P2, O2_BUS_P2W, 2, data);
    }
}

static uint8_t port_read(void *user, int port)
{
    o2_machine_t *m = user;
    uint8_t data = 0xff;

    if (port == 2 && !(m->p1 & 0x04)) {
        /* P12: 74156 keyboard decoder enable, 74156 inputs from P20-P22;
         * 74148 priority encoder, GS to P24, outputs to P25-P27 */
        int inp = countl_zero8(o2_machine_key_row(m, m->p2 & 0x07));

        if (inp < 8)
            data &= (uint8_t)(inp << 5 | 0xf);
    }
    return data;
}

static uint8_t bus_read(void *user)
{
    o2_machine_t *m = user;
    uint8_t data = o2_cart_bus_read(m->cart);

    /* same chip as keyboard */
    if (!(m->p1 & 0x04)) {
        uint8_t sel = m->p2 & 0x07;

        if (sel < 2)
            data &= (uint8_t)~m->joy[sel];
    }
    return data;
}

static void bus_write(void *user, uint8_t data)
{
    o2_machine_t *m = user;

    o2_cart_bus_write(m->cart, data);
    bus_hook(m, O2_WATCH_BUS, O2_BUS_BUSW, 0, data);
}

static int test_read(void *user, int n)
{
    o2_machine_t *m = user;

    if (n == 0)
        return o2_cart_t0(m->cart);
    return i8244_t1(&m->vdc, now_of(m));
}

/* ---- The Voice (bus/odyssey2/voice.cpp) -----------------------------------
 * A cartridge in the slot with the game's behind it: the program space, P1's
 * bank bits and the bus go through; P2's A10 (bit 2) is held low; the data
 * space writes drive the SP0256 and the cart's T0 is its !LRQ. As in MAME the
 * sub-cartridge sees no I/O. The chip runs on its own 3.12 MHz crystal, kept
 * in step with the console's clock at every access and at each frame's end. */

#define SP0256_HZ 3120000.0

static inline uint64_t speech_clock(const o2_machine_t *m)
{
    return (uint64_t)((double)m->cpu.cycles * SP0256_HZ / m->cycle_hz);
}

static uint8_t voice_read(o2_cart_t *c, uint16_t addr, int peek)
{
    o2_machine_t *m = c->priv;

    return o2_cart_read(m->voice_sub, addr, peek);
}

static uint8_t voice_io_read(o2_cart_t *c, uint8_t off, int peek)
{
    (void)c; (void)off; (void)peek;
    return 0xff;
}

static void voice_io_write(o2_cart_t *c, uint8_t off, uint8_t data)
{
    o2_machine_t *m = c->priv;

    sp0256_sync_to(&m->speech, speech_clock(m));
    sp0256_o2_io_write(&m->speech, m->voice_control, off, data);
}

static void voice_write_p1(o2_cart_t *c, uint8_t data)
{
    o2_machine_t *m = c->priv;

    m->voice_control = data;
    o2_cart_write_p1(m->voice_sub, data & 3);
}

static void voice_write_p2(o2_cart_t *c, uint8_t data)
{
    o2_machine_t *m = c->priv;

    o2_cart_write_p2(m->voice_sub, data & (uint8_t)~4);
}

static uint8_t voice_bus_read(o2_cart_t *c)
{
    o2_machine_t *m = c->priv;

    return o2_cart_bus_read(m->voice_sub);
}

static void voice_bus_write(o2_cart_t *c, uint8_t data)
{
    o2_machine_t *m = c->priv;

    o2_cart_bus_write(m->voice_sub, data);
}

static int voice_t0(o2_cart_t *c)
{
    o2_machine_t *m = c->priv;

    sp0256_sync_to(&m->speech, speech_clock(m));
    return sp0256_o2_t0(&m->speech);
}

static const o2_cart_ops_t voice_ops = {
    voice_read, voice_io_read, voice_io_write, voice_write_p1, voice_write_p2,
    voice_bus_read, voice_bus_write, voice_t0,
};

/* PROG: the 8243 expander's strobe (G7400) */
static void prog_write(void *user, int level)
{
    o2_machine_t *m = user;

    if (m->have_ef)
        i8243_prog_w(&m->expander, level);
}

/* vpp_state::i8243_port_w<P>: P4,P5 colour mix on the EF9340 side, P6,P7 on
 * the 8245's. The picture is brought up to the beam with the old tables
 * first, as update_now() does. */
static void expander_out(void *user, int port, uint8_t data)
{
    o2_machine_t *m = user;

    i8244_catch_up(&m->vdc, now_of(m));
    ef9340_mix_port_w(&m->mix, port, data);
}

/* vpp_state::screen_update: the text layer is mixed into every row the 8245
 * draws, and its CX line goes back to the 8245. */
static void line_mix(void *user, i8244_t *v, int y, int x0, int x1)
{
    o2_machine_t *m = user;
    uint8_t *row = v->bitmap + (size_t)y * I8244_WIDTH;
    uint8_t cx[I8244_WIDTH];
    int x;

    ef9340_mix_span(&m->ef, &m->mix, y, x0, x1 + 1, row, i8244_lum(v), m->cart_b, row, cx);
    for (x = x0; x <= x1; x++)
        if (cx[x])
            i8244_write_cx(v, x, 1);
}

/* the VDC's _INTR -> the 8048's INT */
static void vdc_irq(void *user, int level)
{
    o2_machine_t *m = user;

    mcs48_set_int(&m->cpu, level);
}

/* ---- power on ----------------------------------------------------------------- */

void o2_machine_init(o2_machine_t *m, const o2_machine_opts_t *opts)
{
    mcs48_bus_t bus;

    memset(m, 0, sizeof *m);
    m->machine = opts->machine;
    m->is_pal = o2_machine_is_pal(opts->machine);
    m->cycle_clocks = m->is_pal ? I8244_PAL_CYCLE_CLOCKS : I8244_NTSC_CYCLE_CLOCKS;
    m->cycle_hz = m->is_pal ? O2_PAL_CYCLE_HZ : O2_NTSC_CYCLE_HZ;
    m->pixel_hz = m->is_pal ? I8244_PAL_PIXEL_HZ : I8244_NTSC_PIXEL_HZ;
    m->audio_gain = O2_AUDIO_BOOST;

    if (opts->bios) {
        memcpy(m->rom, opts->bios, O2_BIOS_SIZE);
        m->have_bios = true;
    } else {
        memcpy(m->rom, o2_stub, O2_BIOS_SIZE);
    }

    m->cart = opts->cart;
    m->fujinet = opts->fujinet;
    m->voice_wanted = opts->voice;
    if (opts->voice && opts->voice_rom && opts->voice_speech) {
        /* sp0256_load() takes the 16 KB speech ROM at $4000 and a game's own
         * at $8000 (MAME's "speech" region as voice.cpp loads it) */
        sp0256_init(&m->speech, opts->voice_rom, NULL, 0);
        sp0256_load(&m->speech, 0x4000, opts->voice_speech, 16384);
        if (opts->voice_game)
            sp0256_load(&m->speech, 0x8000, opts->voice_game, 16384);
        m->have_voice = true;
        m->voice_sub = opts->cart;
        memset(&m->voice_wrap, 0, sizeof m->voice_wrap);
        m->voice_wrap.ops = &voice_ops;
        m->voice_wrap.type = O2_CART_VOICE;
        m->voice_wrap.priv = m;
        m->cart = &m->voice_wrap;
    }
    m->p1 = m->p2 = 0xff;

    i8244_init(&m->vdc, m->is_pal, opts->font);
    i8244_set_irq_callback(&m->vdc, vdc_irq, m);
    /* MAME fires a chip's timer only after the instruction that runs past it:
     * one machine cycle of lag, which decides the $A1 status bits and the
     * interrupt's arrival to the cycle (i8244.h) */
    i8244_set_access_lag(&m->vdc, m->cycle_clocks);

    memset(&bus, 0, sizeof bus);
    bus.user = m;
    bus.program_read = program_read;
    bus.ext_read = io_read;
    bus.ext_write = io_write;
    bus.port_read = port_read;
    bus.port_write = port_write;
    bus.bus_read = bus_read;
    bus.bus_write = bus_write;
    bus.test_read = test_read;
    bus.prog_write = prog_write;
    if (m->machine == O2_MACHINE_VIDEOPAC_PLUS) {
        m->have_ef = true;
        ef9340_init(&m->ef, opts->font_ef);
        ef9340_mix_reset(&m->mix);
        i8243_init(&m->expander, NULL, expander_out, m);
        i8244_set_line_callback(&m->vdc, line_mix, m);
    }
    mcs48_init(&m->cpu, &bus);
    mcs48_reset(&m->cpu);

    m->fj_swaps = 0;
}

/* ---- inputs ----------------------------------------------------------------------- */

void o2_machine_latch_inputs(o2_machine_t *m)
{
    uint64_t mask = m->machine == O2_MACHINE_VIDEOPAC_PLUS ? ~0ull : O2_KEYS_MASK_O2;

    m->keys = m->keys_live & mask;
    m->joy[0] = m->joy_live[0] & 0x1f;
    m->joy[1] = m->joy_live[1] & 0x1f;
    if (m->reset_live != m->reset_held) {
        m->reset_held = m->reset_live;
        if (m->reset_held)
            mcs48_reset(&m->cpu);       /* the RESET key: the 8048 only */
    }
}

/* ---- running --------------------------------------------------------------------- */

unsigned o2_machine_missing(const o2_machine_t *m)
{
    unsigned miss = 0;

    if (m->stub_parked)
        miss |= O2_MISSING_BIOS;
    if (!m->vdc.font || (m->have_ef && !m->ef.font))
        miss |= O2_MISSING_FONT;
    if (m->voice_wanted && !m->have_voice)
        miss |= O2_MISSING_VOICE;
    return miss;
}

/* A game came in over the FujiNet cartridge while the stand-in is the BIOS: it
 * would call BIOS routines that are not there. Park the console; the session
 * says "Import BIOS". */
static void check_swap(o2_machine_t *m)
{
    o2_cart_status_t st;

    if (!m->fujinet || m->have_bios)
        return;
    o2_fujinet_status(m->fujinet, &st);
    if (st.swaps != m->fj_swaps) {
        m->fj_swaps = st.swaps;
        if (!st.mailbox)
            m->stub_parked = true;
    }
}

static int step_inner(o2_machine_t *m)
{
    int cycles;

    /* a reset key held (or a game the stand-in cannot run): the CPU does not
     * run, time still passes */
    if (m->reset_held || m->stub_parked) {
        m->cpu.cycles += 4;
        i8244_run_until(&m->vdc, now_of(m));
        return 4;
    }
    m->insn_start = now_of(m);
    /* the 8048 samples INT one machine cycle before an instruction starts */
    /* The 8048 takes the vblank interrupt about 0.9 of a machine cycle before
     * the VDC's own event time: MAME's CPU accepts it at the boundary whose
     * start is within 15-19 pixel clocks (NTSC) of the event, measured against
     * a 60-frame MAME access log (any value from 15 to 19 matches; the CPU
     * port's notes put the NTSC range at 17-21). The lead is a property of
     * how MAME interleaves its CPU and timers, not of the hardware. */
    i8244_run_until(&m->vdc, m->insn_start + (uint64_t)(m->cycle_clocks * 9 / 10));
    if (m->have_ef)
        ef9340_run_until(&m->ef, m->insn_start);
    cycles = mcs48_step(&m->cpu);
    return cycles;
}

int o2_machine_step_quiet(o2_machine_t *m)
{
    return step_inner(m);
}

int o2_machine_step(o2_machine_t *m)
{
    if (m->instr_hook)
        m->instr_hook(m, m->instr_hook_user);
    return step_inner(m);
}

/* Mix the console's sound for [audio_t, t) into m->out (stereo float). */
static void mix_audio(o2_machine_t *m, uint64_t t)
{
    float mono[O2_AUDIO_MAX];
    double exact;
    int n, i;

    if (t <= m->audio_t) {
        m->out_frames = 0;
        return;
    }
    exact = (double)(t - m->audio_t) * O2_AUDIO_RATE / m->pixel_hz + m->audio_frac;
    n = (int)exact;
    m->audio_frac = exact - n;
    if (n > O2_AUDIO_MAX)
        n = O2_AUDIO_MAX;
    i8244_run_until(&m->vdc, t);
    i8244_sound_mix(&m->vdc, m->audio_t, t, mono, n);
    for (i = 0; i < n; i++)
        mono[i] *= m->audio_gain;
    if (m->have_voice) {
        float speech[O2_AUDIO_MAX];

        /* the chip's own clock, brought up to the frame's end; MAME routes it at 1.0 */
        sp0256_sync_to(&m->speech, speech_clock(m));
        sp0256_render_f32(&m->speech, speech, n);
        for (i = 0; i < n; i++) {
            float s = mono[i] + speech[i];

            mono[i] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
        }
    }
    for (i = 0; i < n; i++) {
        m->out[2 * i] = mono[i];
        m->out[2 * i + 1] = mono[i];
    }
    m->out_frames = n;
    m->audio_samples += (uint64_t)n;
    m->audio_t = t;
}

void o2_machine_run_frame(o2_machine_t *m)
{
    uint64_t target = m->vdc.frames + 1;

    o2_machine_latch_inputs(m);
    while (m->vdc.frames < target)
        o2_machine_step(m);
    check_swap(m);
    o2_fujinet_frame(m->fujinet);
    memcpy(m->fb, i8244_frame(&m->vdc), sizeof m->fb);
    m->frame_count++;
    mix_audio(m, now_of(m));
}

void o2_machine_snapshot(o2_machine_t *m, uint32_t *dst)
{
    i8244_catch_up(&m->vdc, now_of(m));
    i8244_frame_in_progress(&m->vdc, now_of(m), 0, dst, NULL, NULL);
}

/* ---- the debugger's views ---------------------------------------------------------- */

uint8_t o2_machine_peek(o2_machine_t *m, int space, uint16_t addr)
{
    uint8_t data;

    switch (space) {
    case 0:     /* O2_SPACE_PROG */
        addr &= 0xfff;
        return addr < O2_BIOS_SIZE ? m->rom[addr] : o2_cart_read(m->cart, addr, 1);
    case 1:     /* O2_SPACE_XDATA: what a MOVX read would see, with no side effects */
        addr &= 0xff;
        data = o2_cart_io_read(m->cart, (uint8_t)addr, 1);
        if (!(m->p1 & 0x10) && !(addr & 0x80))
            data &= m->ram[addr];
        if ((m->p1 & 0x48) == 0)
            data &= i8244_peek(&m->vdc, now_of(m), addr);
        return data;
    case 2:     /* O2_SPACE_IRAM */
        return m->cpu.ram[addr & 0x3f];
    case 3:     /* O2_SPACE_VDC */
        return i8244_peek(&m->vdc, now_of(m), addr & 0xff);
    default:
        return 0xff;
    }
}

void o2_machine_poke(o2_machine_t *m, int space, uint16_t addr, uint8_t data)
{
    switch (space) {
    case 0:     /* the BIOS ROM and the cartridge are read-only; an edit of a
                 * FujiNet app's image goes through the cart's own buffers */
        break;
    case 1:
        addr &= 0xff;
        if (!(addr & 0x80))
            m->ram[addr] = data;
        break;
    case 2:
        m->cpu.ram[addr & 0x3f] = data;
        break;
    case 3:
        i8244_poke(&m->vdc, now_of(m), addr & 0xff, data);
        break;
    }
}
