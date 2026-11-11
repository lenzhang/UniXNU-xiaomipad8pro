// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the four-amplifier group (AudioAmps.c) against a register model of piano's amplifiers with their reset lines:
// the reset sequence, all four or none, the preset verdicts, the test gain cap at start, the mute-first way down, the group check.
//   cc -std=c11 -Wall -Wextra -Werror amps_host.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioAmps.h"
#include "synthetic_fsm.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

struct Amp {uint8_t address; int present, released; uint16_t devid; uint16_t reg[256];};
struct Event {char kind; uint8_t slot, reg; uint16_t value; uint64_t at;};   // 'W' write, 'R' reset line
static struct {
    struct Amp amp[4];
    struct Event ev[8192];
    unsigned events, dones;
    uint64_t now_us;
    int fail_after;   // transfers after this many fail (-1 none)
    unsigned transfers;
} m;

static void defaults(struct Amp *a)
{
    memset(a->reg, 0, sizeof a->reg);
    a->reg[FS19XX_REG_DEVID] = a->devid; a->reg[FS19XX_REG_CHIPINI] = 0x0300; a->reg[FS19XX_REG_PWRCTRL] = FS19XX_PWRCTRL_PWDN;
    a->reg[FS19XX_REG_VOLCTRL] = 0xffc0; a->reg[FS19XX_REG_DACCTRL] = FS19XX_DACCTRL_MUTE;
}
static void derive(struct Amp *a)
{
    const int on = !(a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && a->reg[FS19XX_REG_SYSCTRL] == FS19XX_SYSCTRL_ON;
    a->reg[FS19XX_REG_DIGSTAT] = on && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE ? FS19XX_DIGSTAT_DACRUN : 0;
    a->reg[FS19XX_REG_STATUS] = (uint16_t)((a->reg[FS19XX_REG_STATUS] & 0x0100) | (on ? 0x0042 : 0));
}
static int slot_of(uint8_t address) {for (int i = 0; i < 4; ++i) if (m.amp[i].address == address) return i; return -1;}
static void event(char kind, int slot, uint8_t reg, uint16_t value)
{
    if (m.events < 8192) m.ev[m.events++] = (struct Event){kind, (uint8_t)slot, reg, value, m.now_us};
}
static int transfer(void *ctx, uint8_t address, const uint8_t *out, uint32_t out_len, uint8_t *in, uint32_t in_len)
{
    (void)ctx;
    const int s = slot_of(address);
    if (s < 0 || !m.amp[s].present || !m.amp[s].released) return -1;
    if (m.fail_after >= 0 && m.transfers++ >= (unsigned)m.fail_after) return -1;
    struct Amp *a = &m.amp[s];
    if (out_len == 3 && !in_len) {
        const uint16_t v = (uint16_t)(out[1] << 8 | out[2]);
        event('W', s, out[0], v);
        if (out[0] == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) {defaults(a); return -1;}
        a->reg[out[0]] = v;
        derive(a);
        return 0;
    }
    if (out_len == 1 && in_len == 2) {in[0] = (uint8_t)(a->reg[out[0]] >> 8); in[1] = (uint8_t)a->reg[out[0]]; return 0;}
    return -1;
}
static void sleep_us(void *ctx, uint32_t us) {(void)ctx; m.now_us += us;}
static const Fs19xxBus bus = {0, transfer, sleep_us};
static int reset(void *owner, unsigned slot, int release)
{
    (void)owner;
    event('R', (int)slot, 0, (uint16_t)release);
    if (!release || !m.amp[slot].released) defaults(&m.amp[slot]);
    m.amp[slot].released = release;
    return 0;
}
static void done(void *owner) {(void)owner; m.dones++;}
static const AudioAmpsIo io = {0, &bus, reset, done};

static void model(void)
{
    memset(&m, 0, sizeof m);
    const uint8_t addresses[4] = AUDIO_AMP_ADDRESSES;
    for (int i = 0; i < 4; ++i) {m.amp[i].address = addresses[i]; m.amp[i].present = 1; m.amp[i].devid = FS19XX_DEVID; defaults(&m.amp[i]);}
    m.fail_after = -1;
}

int main(void)
{
    static uint8_t fsm[4096];
    const unsigned fsm_bytes = sfsm_build(fsm, sizeof fsm, 0);
    AudioAmps g;
    CHECK(audio_amp_address(0) == 0x35 && audio_amp_address(1) == 0x37 && audio_amp_address(2) == 0x34 && audio_amp_address(3) == 0x36);
    CHECK(!strcmp(audio_amp_name(3), "Bottom Right") && audio_amp_address(4) == 0);

    // the reset sequence: all four held low, 10 ms, all four released, 5 ms, then the probes
    model();
    audio_amps_init(&g, &io);
    CHECK(audio_amps_probe(&g) == 0xf && g.released && m.dones == 1);
    for (int i = 0; i < 4; ++i) CHECK(m.ev[i].kind == 'R' && m.ev[i].slot == i && m.ev[i].value == 0 && m.ev[i].at == 0);
    for (int i = 4; i < 8; ++i) CHECK(m.ev[i].kind == 'R' && m.ev[i].value == 1 && m.ev[i].at == 10000);
    CHECK(g.amp[0].devid == FS19XX_DEVID && g.amp[3].state == Fs19xxIdentified);
    // starting before configuring is refused, nothing written
    const unsigned before = m.events;
    CHECK(audio_amps_start(&g, 160) == AudioAmpsErrState && m.events == before);

    // all four or none: one silent, one foreign
    model();
    m.amp[1].present = 0;
    m.amp[3].devid = 0x1700;
    audio_amps_init(&g, &io);
    CHECK(audio_amps_probe(&g) == 0x5 && g.probe[1] == Fs19xxErrTimeout && g.probe[3] == Fs19xxErrId);
    CHECK(audio_amps_configure(&g, fsm, fsm_bytes, 1) == AudioAmpsErrMissing && !g.present && !g.released);
    for (int i = 0; i < 4; ++i) CHECK(!m.amp[i].released);   // held in reset again

    // the preset refused: not the stock file without "any"; nothing configured, all held
    model();
    audio_amps_init(&g, &io);
    audio_amps_probe(&g);
    CHECK(audio_amps_configure(&g, fsm, fsm_bytes, 0) == AudioAmpsErrPreset && g.preset == AudioPresetNotStock && !g.configured);
    for (int i = 0; i < 4; ++i) CHECK(!m.amp[i].released);
    // no block for an amplifier: an address the container lacks
    model();
    m.amp[2].address = 0x34;
    audio_amps_init(&g, &io);
    audio_amps_probe(&g);
    static uint8_t three[4096];
    memcpy(three, fsm, sizeof three);
    three[28 + 4 * 0 + 2] = 7;   // the first descriptor (0x34's block) of another kind
    sfsm_put16(three + 24, fsm_crc16(three + 26, fsm_bytes - 26));
    CHECK(audio_amps_configure(&g, three, fsm_bytes, 1) == AudioAmpsErrBlock && g.find[2] == FsmNoBlock);
    // a bus error while configuring the third: all four shut down and held
    model();
    audio_amps_init(&g, &io);
    audio_amps_probe(&g);
    m.fail_after = 40;
    CHECK(audio_amps_configure(&g, fsm, fsm_bytes, 1) == AudioAmpsErrConfigure && !g.configured && !g.released);

    // the happy path: configured, all down, started muted at a gain capped to 160, unmuted, checked, off (mutes first), held
    model();
    audio_amps_init(&g, &io);
    audio_amps_probe(&g);
    CHECK(audio_amps_configure(&g, fsm, fsm_bytes, 1) == AudioAmpsOk && g.configured == 0xf);
    CHECK(audio_amps_read(&g) == 0 && audio_amps_all_down(&g));
    for (int i = 0; i < 4; ++i) CHECK(m.amp[i].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE && (m.amp[i].reg[FS19XX_REG_PWRCTRL] & 1));
    CHECK(audio_amps_start(&g, 200) == AudioAmpsOk && g.started == 0xf);
    for (int i = 0; i < 4; ++i) {
        CHECK(m.amp[i].reg[FS19XX_REG_VOLCTRL] >> 8 == AUDIO_AMPS_TEST_GAIN && m.amp[i].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);
        CHECK(!(m.amp[i].reg[FS19XX_REG_PWRCTRL] & 1));
    }
    CHECK(audio_amps_read(&g) == 0 && !audio_amps_all_down(&g));
    CHECK(audio_amps_check(&g) == 0xf);   // muted, the DACs do not run: not "playing" (the session checks only after the unmute)
    CHECK(audio_amps_unmute(&g) == AudioAmpsOk);
    for (int i = 0; i < 4; ++i) CHECK(m.amp[i].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE);
    CHECK(audio_amps_read(&g) == 0 && g.have_first && audio_amps_check(&g) == 0);
    m.amp[1].reg[FS19XX_REG_STATUS] |= 0x0100;   // OTWDS on the top right one
    CHECK(audio_amps_read(&g) == 0 && audio_amps_check(&g) == 0x2);
    const unsigned at = m.events;
    CHECK(audio_amps_off(&g) == 0 && !g.started);
    for (unsigned i = 0; i < 4; ++i) CHECK(m.ev[at + i].kind == 'W' && m.ev[at + i].reg == FS19XX_REG_DACCTRL && m.ev[at + i].value == FS19XX_DACCTRL_MUTE);
    for (unsigned i = at + 4; i < m.events; ++i) CHECK(m.ev[i].reg != FS19XX_REG_DACCTRL || m.ev[i].value == FS19XX_DACCTRL_MUTE);
    for (int i = 0; i < 4; ++i) CHECK((m.amp[i].reg[FS19XX_REG_PWRCTRL] & 1) && m.amp[i].reg[FS19XX_REG_SYSCTRL] == 0);
    audio_amps_hold(&g);
    for (int i = 0; i < 4; ++i) CHECK(!m.amp[i].released);
    CHECK(!g.present && !g.configured && audio_amps_start(&g, 160) == AudioAmpsErrState);
    // a failed start (the bus gone mid-way): every amplifier shut down again
    model();
    audio_amps_init(&g, &io);
    audio_amps_probe(&g);
    CHECK(audio_amps_configure(&g, fsm, fsm_bytes, 1) == AudioAmpsOk);
    m.fail_after = 20;
    m.transfers = 0;
    CHECK(audio_amps_start(&g, 160) == AudioAmpsErrStart && !g.started);
    m.fail_after = -1;
    for (int i = 0; i < 4; ++i) CHECK(m.amp[i].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);
    printf("PASS amps: %u checks\n", checks);
    return 0;
}
