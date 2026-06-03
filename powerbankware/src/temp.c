#include "powerbankware.h"

/*
 * ntc_temp_read — OEM FUN_0800e1d4.
 *
 * Converts the most recent FEDL5236 TS (thermistor) reading in the RX buffer
 * (0x20000614, bytes [2..3] = 16-bit ADC) into a temperature index via the
 * same rational transfer curve batteryware uses for its cell score:
 *
 *     result = (adc * 610 * 10000) / (2500000 - adc * 610)
 *
 * computed in 64-bit (the OEM uses __aeabi_lmul / __aeabi_ldivmod, restored
 * here to plain C arithmetic), then walked down the 146-entry descending LUT
 * `s_temp_lut`: the returned index is the highest slot whose value is >= the
 * computed result. Per the OEM, index 0 is never tested — if the walk reaches
 * it the function returns 0x91 (the top of the table).
 *
 * The init's temperature gate treats the index as °C-ish: valid is
 * [0x15, 0x6e).
 */

/* Resistance/temperature LUT, OEM flash @ 0x0801e820 (146 x u32, descending
 * 0x0002F2FE .. 0x00000358), extracted verbatim from the image. */
static const uint32_t s_temp_lut[146] = {
    0x0002f2fe, 0x0002caa0, 0x0002a49d, 0x000280d3, 0x00025f21, 0x00023f66,
    0x00022184, 0x0002055c, 0x0001ead2, 0x0001d1cd, 0x0001ba32, 0x0001a3ed,
    0x00018ee6, 0x00017b09, 0x00016845, 0x00015689, 0x000145c5, 0x000135e9,
    0x000126e9, 0x000118b7, 0x00010b48, 0x0000fe90, 0x0000f285, 0x0000e71d,
    0x0000dc4f, 0x0000d212, 0x0000c85f, 0x0000bf2d, 0x0000b675, 0x0000ae31,
    0x0000a659, 0x00009ee8, 0x000097d8, 0x00009124, 0x00008ac5, 0x000084b9,
    0x00007ef9, 0x00007982, 0x0000744f, 0x00006f5d, 0x00006aa9, 0x0000662e,
    0x000061ea, 0x00005dda, 0x000059fa, 0x00005649, 0x000052c4, 0x00004f68,
    0x00004c34, 0x00004925, 0x00004639, 0x0000436f, 0x000040c5, 0x00003e39,
    0x00003bca, 0x00003976, 0x0000373c, 0x0000351c, 0x00003312, 0x00003120,
    0x00002f42, 0x00002d79, 0x00002bc4, 0x00002a21, 0x00002890, 0x00002710,
    0x0000259f, 0x0000243f, 0x000022ec, 0x000021a8, 0x00002072, 0x00001f48,
    0x00001e2a, 0x00001d18, 0x00001c11, 0x00001b15, 0x00001a23, 0x0000193b,
    0x0000185b, 0x00001785, 0x000016b8, 0x000015f2, 0x00001534, 0x0000147e,
    0x000013cf, 0x00001326, 0x00001284, 0x000011e9, 0x00001153, 0x000010c3,
    0x00001038, 0x00000fb2, 0x00000f32, 0x00000eb6, 0x00000e3f, 0x00000dcc,
    0x00000d5e, 0x00000cf3, 0x00000c8c, 0x00000c29, 0x00000bca, 0x00000b6e,
    0x00000b15, 0x00000abf, 0x00000a6c, 0x00000a1c, 0x000009cf, 0x00000985,
    0x0000093d, 0x000008f7, 0x000008b4, 0x00000873, 0x00000835, 0x000007f8,
    0x000007be, 0x00000785, 0x0000074e, 0x00000719, 0x000006e6, 0x000006b4,
    0x00000685, 0x00000656, 0x00000629, 0x000005fe, 0x000005d4, 0x000005ab,
    0x00000583, 0x0000055d, 0x00000538, 0x00000514, 0x000004f2, 0x000004d0,
    0x000004af, 0x00000490, 0x00000471, 0x00000454, 0x00000437, 0x0000041b,
    0x00000400, 0x000003e6, 0x000003cc, 0x000003b4, 0x0000039c, 0x00000385,
    0x0000036e, 0x00000358,
};

/* FEDL5236 RX frame buffer (shared with fedl5236.c). */
#define FEDL_RX_BUF  ((volatile uint8_t *)0x20000614)

uint8_t ntc_temp_read(void)
{
    volatile uint8_t * const rx = FEDL_RX_BUF;

    uint32_t adc = (uint32_t)(rx[2] | (rx[3] << 8));
    int64_t  p1  = (int64_t)adc * 610;
    int64_t  result = (p1 * 10000) / (2500000 - p1);

    uint8_t idx = 0x91;
    for (;;) {
        if (idx == 0) {
            return 0x91;
        }
        if ((uint32_t)result <= s_temp_lut[idx]) {
            break;
        }
        idx--;
    }
    return idx;
}
