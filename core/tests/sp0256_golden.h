/*
 * sp0256_golden.h -- goldens for sp0256_test.c. No ROM data in here: only
 * hashes of what the chip outputs and a few output samples.
 *
 * Where each came from:
 *
 *  scn / synth  Produced by MAME's own src/devices/sound/sp0256.cpp,
 *               compiled UNMODIFIED behind a stub of MAME's device classes
 *               (sound_stream, device_t, ...) and driven by
 *               sp0256_scenario.h; core/o2/sp0256.c reproduces every one
 *               (checked over 400 seeds x 60000 samples, not just these; the
 *               one seed on which MAME itself never returns is the
 *               instruction-budget deviation).
 *               The stub zeroes the object (MAME never initialises the FIFO
 *               array) and, between updates, the scratch-ring pointers, to
 *               sidestep the wrap defect described in sp0256.h.
 *
 *  hello/sweep  Intellivoice ROM (sp0256-012, CRC 0DE7579D) played by the
 *               real MAME binary (`mame intvecs -samplerate 10000
 *               -wavwrite`, a Lua script writing $0080 each frame), the
 *               write times taken from the script's log (floor(t * 10000)).
 *               core/o2/sp0256.c's output equals MAME's wav sample for
 *               sample except that some positive samples (0.4% of "hello",
 *               2.4% of the sweep) are 1 LSB low in the wav: MAME's float mix
 *               with the console's PSG DC offset truncates (the chip itself
 *               is exact), and where that DC pushes a loud negative peak into
 *               the wav's clip, the same clip applied to ours matches. The hashes below are of
 *               this core's own 16-bit output, which the unmodified-MAME stub
 *               reproduces too.
 */
#ifndef SP0256_GOLDEN_H
#define SP0256_GOLDEN_H

#include <stdint.h>

#define SP0256_GOLDEN_SCN_N       32
#define SP0256_GOLDEN_SCN_SAMPLES 40000
static const uint32_t sp0256_golden_scn[SP0256_GOLDEN_SCN_N] = {
    0x3e2680cau, 0x283e87bbu, 0x7d6f8a9fu, 0xb3f21d30u, 0x1ca4e41cu, 0xb71f8057u,
    0x8a5d87b3u, 0x3a20743du, 0x64763ea0u, 0x1b64c20cu, 0xc31cb21eu, 0xa5473910u,
    0x109297b0u, 0x736ec0b9u, 0x8aded3edu, 0x6ac2303cu, 0x26c79794u, 0x2e5c20dcu,
    0xe16db977u, 0xab3dde20u, 0xad924b7bu, 0xf8db3aafu, 0x6b55512au, 0x76654275u,
    0x6366c798u, 0x4b9135c0u, 0x2c7c4033u, 0xfd673391u, 0x444b53ffu, 0x2128d9dcu,
    0x1d4c3dacu, 0x9b1fb173u,
};

typedef struct { uint32_t hash, busy, accept; } sp0256_golden_synth_t;
static const sp0256_golden_synth_t sp0256_golden_synth[5] = {
    { 0xb0cb9710u, 254, 59 },
    { 0x448a5dddu, 264, 59 },
    { 0x2d205741u, 187, 59 },
    { 0xb0cb9710u, 254, 59 },
    { 0x448a5dddu, 264, 59 },
};

typedef struct { uint32_t at; uint8_t value; } sp0256_golden_ald_t;

/* "hello": HH1 EH LL AX OW PA4 written as the chip frees up */
#define SP0256_GOLDEN_HELLO_SAMPLES 40000
#define SP0256_GOLDEN_HELLO_HASH    0x462e4ef9
static const sp0256_golden_ald_t sp0256_golden_hello[] = {
    {1001,27}, {1168,7}, {6341,45}, {10847,15}, {11014,53}, {13851,3},
};
/* the 16 samples after the first address load (index 1008..1023), as MAME's wav has them */
static const int16_t sp0256_golden_hello_first[16] = {
    -1024, 2196, 76, -3136, 368, 1944, 1148, -3596, 2400, 932, -3352, 144, 2612, -1372, 204, 1180
};

/* every one of the 64 allophones in order, 60 s */
#define SP0256_GOLDEN_SWEEP_SAMPLES 600000
#define SP0256_GOLDEN_SWEEP_HASH    0xa3af8a3a
static const sp0256_golden_ald_t sp0256_golden_sweep[64] = {
    {1001,0}, {1168,1}, {1335,2}, {11348,3}, {16355,4}, {18357,5}, {19359,6}, {19692,7},
    {38551,8}, {43057,9}, {46895,10}, {50901,11}, {56074,12}, {61582,13}, {67923,14}, {72096,15},
    {76769,16}, {79606,17}, {83945,18}, {87950,19}, {93291,20}, {98464,21}, {106642,22}, {115320,23},
    {123831,24}, {133177,25}, {142189,26}, {149032,27}, {157543,28}, {162883,29}, {168224,30}, {175066,31},
    {181408,32}, {188751,33}, {195927,34}, {200433,35}, {206942,36}, {211448,37}, {218624,38}, {222963,39},
    {225300,40}, {228805,41}, {232643,42}, {236481,43}, {239652,44}, {239819,45}, {239986,46}, {240153,47},
    {240320,48}, {240487,49}, {241154,50}, {243491,51}, {243825,52}, {247329,53}, {247496,54}, {248164,55},
    {248831,56}, {257510,57}, {257676,58}, {270694,59}, {271028,60}, {271361,61}, {272363,62}, {272530,63},
};

#endif /* SP0256_GOLDEN_H */
