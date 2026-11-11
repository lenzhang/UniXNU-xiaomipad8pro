// SPDX-License-Identifier: BSD-2-Clause-Patent
// A synthetic fs19xx.fsm for the host tests: the container's documented structure (Fs19xx.h) with made-up register data, one block
// per piano amplifier address. No vendor data: the stock preset is never read by any test.
#pragma once
#include <stdint.h>
#include <string.h>
#include "../Fs19xx.h"

static void sfsm_put16(uint8_t *p, unsigned v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}

// One block: the 40-byte head, three list entries, speaker info, common units (unit, value), music scene units (mask, unit, value).
static unsigned sfsm_block(uint8_t *b, unsigned address, const uint16_t (*common)[2], unsigned nc, const uint16_t (*scenes)[3], unsigned ns)
{
    memset(b, 0, 40);
    sfsm_put16(b + 0, 0x8001); sfsm_put16(b + 26, 3); sfsm_put16(b + 30, address); sfsm_put16(b + 32, 0x2901);
    unsigned at = 12;
    sfsm_put16(b + 40, at); sfsm_put16(b + 42, 1);
    sfsm_put16(b + 40 + at, 2); sfsm_put16(b + 42 + at, 0x1234); sfsm_put16(b + 44 + at, 0x0055);
    at += 6;
    sfsm_put16(b + 44, at); sfsm_put16(b + 46, 2);
    sfsm_put16(b + 40 + at, nc);
    for (unsigned i = 0; i < nc; ++i) {sfsm_put16(b + 42 + at + 4 * i, common[i][0]); sfsm_put16(b + 44 + at + 4 * i, common[i][1]);}
    at += 2 + 4 * nc;
    sfsm_put16(b + 48, at); sfsm_put16(b + 50, 3);
    sfsm_put16(b + 40 + at, ns);
    for (unsigned i = 0; i < ns; ++i) {
        sfsm_put16(b + 42 + at + 6 * i, scenes[i][0]); sfsm_put16(b + 44 + at + 6 * i, scenes[i][1]); sfsm_put16(b + 46 + at + 6 * i, scenes[i][2]);
    }
    at += 2 + 6 * ns;
    return 40 + at;
}

// The container: four blocks (0x34..0x37), each setting a made-up slot register; hostile adds units that try to power the
// amplifier up, unmute it and raise the gain (the safety filter must change them). Returns the size (at most cap).
static unsigned sfsm_build(uint8_t *out, unsigned cap, int hostile)
{
    if (cap < 1024) return 0;
    memset(out, 0, cap);
    memcpy(out, "UNIXNU-PIANO-TEST-FSM", 21);
    sfsm_put16(out + 26, 4);
    unsigned at = 28 + 4 * 4;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned address = 0x34 + i;
        uint16_t common[8][2] = {{0xf01a, (uint16_t)i}, {0x34bd, 0x5}, {0x1f00, 2}};
        unsigned nc = 3;
        if (hostile) {
            common[nc][0] = 0xf010; common[nc++][1] = 0x0000;   // PWRCTRL: power up
            common[nc][0] = 0xf030; common[nc++][1] = 0x0210;   // DACCTRL: unmute
            common[nc][0] = 0xf016; common[nc++][1] = 0xffc0;   // VOLCTRL: 0 dB
        }
        const uint16_t scenes[1][3] = {{FSM_SCENE_MUSIC, 0xf01c, (uint16_t)(i + 8)}};
        sfsm_put16(out + 28 + 4 * i, at); sfsm_put16(out + 30 + 4 * i, 0);
        at += sfsm_block(out + at, address, (const uint16_t (*)[2])common, nc, scenes, 1);
    }
    sfsm_put16(out + 22, at);
    sfsm_put16(out + 24, fsm_crc16(out + 26, at - 26));
    return at;
}
