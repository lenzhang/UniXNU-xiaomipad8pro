// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the FS19xx core (Fs19xx.c) against a register model of piano's four amplifiers on one bus:
//   - probe: the late first answer after a cold reset, an absent amplifier, a foreign device;
//   - the preset container: a synthetic fs19xx.fsm (made-up register data, the documented structure: no vendor data in this file),
//     every header and block error, block selection by address, the units (whole, field, delay, wait, unknown);
//   - speaker safety: at no point does a write power an amplifier up, unmute it or set a gain above 192 except fs19xx_start (power,
//     capped gain) and fs19xx_mute(.., 0); a hostile preset is filtered; a failing start leaves it shut down;
//   - the power sequences in order, the status reads and the group check that stops a stream.
//   - the preset's identity: SHA-256 against the FIPS 180-4 vectors, the stock size and digest.
//   cc -std=c11 -Wall -Wextra -Werror fs19xx_host.c ../Fs19xx.c ../AudioSha256.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Fs19xx.h"
#include "../AudioSha256.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

// ------------------------------------------------------------------------------------------------------------ the bus model
struct Amp {uint8_t address; int present; uint16_t devid; uint16_t reg[256]; int devid_nacks;};
struct Write {uint8_t address, reg; uint16_t value; int acked;};
struct Model {
    struct Amp amp[4];
    struct Write log[4096];
    unsigned writes, transfers;
    uint64_t slept_us;
    int fail_at;          // the transfer (by count) that fails, -1 none
    int fail_reg;         // or every write to this register fails, -1 none
    int allow_power;      // the test is inside fs19xx_start / fs19xx_mute(0)
    unsigned violations;
};
static struct Model m;

static void reset_amp(struct Amp *a)
{
    memset(a->reg, 0, sizeof a->reg);
    a->reg[FS19XX_REG_DEVID] = a->devid; a->reg[FS19XX_REG_REVID] = 0x00b1; a->reg[FS19XX_REG_CHIPINI] = 0x0003;
    a->reg[FS19XX_REG_PWRCTRL] = FS19XX_PWRCTRL_PWDN; a->reg[FS19XX_REG_VOLCTRL] = 0xffc0;   // 0 dB after reset: the dangerous default
    a->reg[FS19XX_REG_DACCTRL] = FS19XX_DACCTRL_MUTE; a->reg[FS19XX_REG_I2SCTRL] = 0x0003; a->reg[0xbd] = 0xa50f; a->reg[0xc0] = 0x005a;
}
static void model_init(void)
{
    memset(&m, 0, sizeof m);
    const uint8_t addresses[4] = {0x35, 0x37, 0x34, 0x36};   // slot order: TL TR BL BR
    for (int i = 0; i < 4; ++i) {m.amp[i].address = addresses[i]; m.amp[i].present = 1; m.amp[i].devid = FS19XX_DEVID; reset_amp(&m.amp[i]);}
    m.fail_at = -1; m.fail_reg = -1;
}
static struct Amp *find(uint8_t address) {for (int i = 0; i < 4; ++i) if (m.amp[i].address == address && m.amp[i].present) return &m.amp[i]; return 0;}
static void derive(struct Amp *a)
{
    const int on = !(a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && a->reg[FS19XX_REG_SYSCTRL] == FS19XX_SYSCTRL_ON;
    a->reg[FS19XX_REG_DIGSTAT] = on && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE ? FS19XX_DIGSTAT_DACRUN : 0;
    a->reg[FS19XX_REG_STATUS] = on ? 0x0042 : 0x0000;   // the model's "PLL and clock" bits while running
}
static int transfer(void *ctx, uint8_t address, const uint8_t *out, uint32_t out_len, uint8_t *in, uint32_t in_len)
{
    (void)ctx;
    const int k = (int)m.transfers++;
    struct Amp *a = find(address);
    if (!a || k == m.fail_at) return -1;
    if (out_len == 3 && !in_len) {
        const uint8_t reg = out[0];
        const uint16_t v = (uint16_t)(out[1] << 8 | out[2]);
        const int acked = !(reg == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) && reg != m.fail_reg;
        if (m.writes < 4096) m.log[m.writes++] = (struct Write){address, reg, v, acked};
        // the safety invariants, checked at every write
        if (reg == FS19XX_REG_VOLCTRL && (v >> 8) > FS19XX_GAIN_CAP) m.violations++;
        if (!m.allow_power && reg == FS19XX_REG_PWRCTRL && !(v & (FS19XX_PWRCTRL_PWDN | FS19XX_PWRCTRL_I2CR))) m.violations++;
        if (!m.allow_power && reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT)) m.violations++;
        if (reg == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) {reset_amp(a); return -1;}   // soft reset: no acknowledge
        if (reg == m.fail_reg) return -1;
        a->reg[reg] = v;
        derive(a);
        return 0;
    }
    if (out_len == 1 && in_len == 2) {
        if (out[0] == FS19XX_REG_DEVID && a->devid_nacks > 0) {a->devid_nacks--; return -1;}
        in[0] = (uint8_t)(a->reg[out[0]] >> 8); in[1] = (uint8_t)a->reg[out[0]];
        return 0;
    }
    return -1;
}
static void sleep_us(void *ctx, uint32_t us) {(void)ctx; m.slept_us += us;}
static const Fs19xxBus bus = {0, transfer, sleep_us};
static unsigned find_write(unsigned from, uint8_t address, uint8_t reg)
{
    for (unsigned i = from; i < m.writes; ++i) if (m.log[i].address == address && m.log[i].reg == reg) return i;
    return ~0u;
}

// -------------------------------------------------------------------------------------- a synthetic preset container
static void put16(uint8_t *p, unsigned v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
struct Unit {uint16_t mask, unit, value;};
// One block: head (40 bytes), three list entries, speaker info, common units, scene units. Returns its size.
static unsigned block(uint8_t *b, unsigned address, uint16_t version, uint16_t type, const struct Unit *common, unsigned nc,
                      const struct Unit *scenes, unsigned ns)
{
    memset(b, 0, 40);
    put16(b + 0, version); put16(b + 26, 3); put16(b + 30, address); put16(b + 32, type);
    unsigned at = 12;   // the lists follow the 3 x 4-byte entries
    put16(b + 40, at); put16(b + 42, 1);
    put16(b + 40 + at, 2); put16(b + 42 + at, 0x1234); put16(b + 44 + at, 0x0055);   // speaker info: word 1 = tcoef
    at += 6;
    put16(b + 44, at); put16(b + 46, 2);
    put16(b + 40 + at, nc);
    for (unsigned i = 0; i < nc; ++i) {put16(b + 42 + at + 4 * i, common[i].unit); put16(b + 44 + at + 4 * i, common[i].value);}
    at += 2 + 4 * nc;
    put16(b + 48, at); put16(b + 50, 3);
    put16(b + 40 + at, ns);
    for (unsigned i = 0; i < ns; ++i) {
        put16(b + 42 + at + 6 * i, scenes[i].mask); put16(b + 44 + at + 6 * i, scenes[i].unit); put16(b + 46 + at + 6 * i, scenes[i].value);
    }
    at += 2 + 6 * ns;
    return 40 + at;
}
static uint8_t fsm[4096];
static unsigned fsm_size;
static void finish_fsm(void)
{
    put16(fsm + 22, fsm_size);
    put16(fsm + 24, fsm_crc16(fsm + 26, fsm_size - 26));
}
// Four blocks (one per address) and a descriptor of another type first. hostile: units that try to power up, unmute, raise the gain.
static void build_fsm(int hostile)
{
    memset(fsm, 0, sizeof fsm);
    memcpy(fsm, "UNIXNU-PIANO-TEST-FSM", 21);
    put16(fsm + 26, 5);
    unsigned at = 28 + 5 * 4;
    put16(fsm + 28, 0); put16(fsm + 30, 7);   // a descriptor of another kind: skipped
    const uint8_t addresses[4] = {0x34, 0x35, 0x36, 0x37};
    for (unsigned i = 0; i < 4; ++i) {
        const uint16_t slot = (uint16_t)(addresses[i] - 0x34);
        struct Unit common[12] = {
            {0, 0xf01a, slot},             // the TDM slot register, whole
            {0, 0x34bd, 0x5},              // register 0xbd bits 7:4 = 5
            {0, 0x1f00, 2},                // a 2 ms delay
            {0, 0x2fc0, 0x705a},           // wait until register 0xc0 bits 7:0 read 0x5a
            {0, 0xf00b, FS19XX_OTPACC_KEY},
            {0, 0xf00b, 0},
        };
        unsigned nc = 6;
        if (hostile) {
            common[nc++] = (struct Unit){0, 0xf010, 0x0000};   // PWRCTRL: power up
            common[nc++] = (struct Unit){0, 0xf030, 0x0210};   // DACCTRL: unmute
            common[nc++] = (struct Unit){0, 0xf016, 0xffc0};   // VOLCTRL: 0 dB
            common[nc++] = (struct Unit){0, 0x7816, 0x00ff};   // VOLCTRL bits 15:8 as a field: 0 dB
            common[nc++] = (struct Unit){0, 0x1f00, 60000};    // a 60 s delay
            common[nc++] = (struct Unit){0, 0x5f00, 1};        // an unknown special unit
        }
        const struct Unit scenes[2] = {{FSM_SCENE_MUSIC, 0xf01c, (uint16_t)(slot + 8)}, {0x0002, 0xf01c, 0x7777}};
        put16(fsm + 32 + 4 * i, at); put16(fsm + 34 + 4 * i, 0);
        at += block(fsm + at, addresses[i], 0x8001, 0x2901, common, nc, scenes, 2);
    }
    fsm_size = at;
    finish_fsm();
}

// --------------------------------------------------------------------------------------------------------------- tests
static void testBasics(void)
{
    CHECK(fsm_crc16((const uint8_t *)"123456789", 9) == 0xbb3d);   // CRC-16/ARC check value
    CHECK(fs19xx_gain_clamp(255) == 192 && fs19xx_gain_clamp(193) == 192 && fs19xx_gain_clamp(192) == 192 && fs19xx_gain_clamp(160) == 160);
    CHECK(fs19xx_gain_db10(192) == -236 && fs19xx_gain_db10(160) == -356 && fs19xx_gain_db10(255) == 0 && fs19xx_gain_db10(215) == -150);
    int changed;
    CHECK(fs19xx_filter(FS19XX_REG_PWRCTRL, 0, &changed) == 1 && changed);
    CHECK(fs19xx_filter(FS19XX_REG_PWRCTRL, 3, &changed) == 3 && !changed);
    CHECK(fs19xx_filter(FS19XX_REG_DACCTRL, 0x0210, &changed) == 0x0310 && changed);
    CHECK(fs19xx_filter(FS19XX_REG_VOLCTRL, 0xffc0, &changed) == 0xc0c0 && changed);
    CHECK(fs19xx_filter(FS19XX_REG_VOLCTRL, 0xa0c0, &changed) == 0xa0c0 && !changed);
    CHECK(fs19xx_filter(0x1a, 0xffff, &changed) == 0xffff && !changed);
    char names[96];
    fs19xx_status_names(0x0042, names, sizeof names);
    CHECK(!strcmp(names, "PLLS CLKS"));
    fs19xx_status_names(0xffff, names, sizeof names);
    CHECK(!strcmp(names, "BOVDS PLLS OTPDS OVDS UVDS OCDS CLKS OTWDS BOPS BOLDS INDS VDDPS"));
    fs19xx_status_names(0xffff, names, 6);
    CHECK(!strcmp(names, "BOVDS"));
    CHECK(fs19xx_status_bit(7) == 0 && !strcmp(fs19xx_status_bit(14), "VDDPS"));
}

static void testProbe(void)
{
    model_init();
    Fs19xx a;
    m.amp[0].devid_nacks = 3;   // the first power-up after a cold boot answers late
    fs19xx_init_amp(&a, &bus, 0x35);
    CHECK(fs19xx_probe(&a) == Fs19xxOk && a.state == Fs19xxIdentified && a.devid == 0x2900 && a.revid == 0xb1 && m.slept_us == 15000);
    CHECK(m.writes == 0);   // a probe writes nothing
    m.amp[1].present = 0;
    m.slept_us = 0;
    fs19xx_init_amp(&a, &bus, 0x37);
    CHECK(fs19xx_probe(&a) == Fs19xxErrTimeout && a.state == Fs19xxAbsent && m.slept_us == 100000);   // 20 polls, 5 ms apart
    m.amp[2].devid = 0x1700; reset_amp(&m.amp[2]);
    fs19xx_init_amp(&a, &bus, 0x34);
    CHECK(fs19xx_probe(&a) == Fs19xxErrId && a.state == Fs19xxAbsent && a.devid == 0x1700);
    CHECK(fs19xx_configure(&a, 0) == Fs19xxErrState && fs19xx_start(&a, 48000, 6144000, 160, 0) == Fs19xxErrState);
}

static void testContainer(void)
{
    build_fsm(0);
    uint32_t n = 0;
    CHECK(fsm_check(fsm, fsm_size, &n) == FsmOk && n == 5);
    Fs19xxPreset p;
    const uint8_t addresses[4] = {0x34, 0x35, 0x36, 0x37};
    for (int i = 0; i < 4; ++i) {
        CHECK(fsm_find(fsm, fsm_size, addresses[i], FS19XX_DEVID, &p) == FsmOk);
        CHECK(p.version == 0x8001 && p.type == 0x2901 && p.tcoef == 0x55 && p.common_count == 6 && p.scene_count == 2);
        CHECK(p.common[0] == 0x1a && p.common[1] == 0xf0 && p.common[2] == i);   // the slot register of this address
    }
    CHECK(fsm_find(fsm, fsm_size, 0x38, FS19XX_DEVID, &p) == FsmNoBlock);
    CHECK(fsm_find(fsm, fsm_size, 0x34, 0x1700, &p) == FsmWrongDevice);
    CHECK(fsm_check(fsm, 27, &n) == FsmShort && fsm_check(0, 100, &n) == FsmShort);
    CHECK(fsm_check(fsm, fsm_size - 1, &n) == FsmBadSize);
    fsm[100] ^= 0x40;
    CHECK(fsm_check(fsm, fsm_size, &n) == FsmBadCrc && fsm_find(fsm, fsm_size, 0x34, FS19XX_DEVID, &p) == FsmBadCrc);
    fsm[100] ^= 0x40;
    CHECK(fsm_check(fsm, fsm_size, &n) == FsmOk);
    // descriptors past the end
    put16(fsm + 26, 2000); finish_fsm();
    CHECK(fsm_check(fsm, fsm_size, &n) == FsmBadIndex);
    // a block not marked valid, a list past the end
    build_fsm(0);
    const unsigned first = fsm[36] | fsm[37] << 8;   // descriptor 2: the 0x35 block
    put16(fsm + first, 0x0001); finish_fsm();
    CHECK(fsm_find(fsm, fsm_size, 0x35, FS19XX_DEVID, &p) == FsmInvalid && fsm_find(fsm, fsm_size, 0x34, FS19XX_DEVID, &p) == FsmOk);
    build_fsm(0);
    const unsigned last = fsm[44] | fsm[45] << 8;   // descriptor 4: the 0x37 block, the file's last
    put16(fsm + last + 40 + 18, 999); finish_fsm();   // its common list's count
    CHECK(fsm_find(fsm, fsm_size, 0x37, FS19XX_DEVID, &p) == FsmBadList);
    build_fsm(0);
    put16(fsm + last + 26, 400); finish_fsm();   // its list entries past the end
    CHECK(fsm_find(fsm, fsm_size, 0x37, FS19XX_DEVID, &p) == FsmBadList);
    CHECK(!strcmp(fsm_error(FsmBadCrc), "CRC16 mismatch") && !strcmp(fsm_error(42), "?"));
}

static Fs19xx amps[4];
static int configure_all(int hostile)
{
    model_init();
    build_fsm(hostile);
    int r = 0;
    for (int i = 0; i < 4; ++i) {
        Fs19xxPreset p;
        fs19xx_init_amp(&amps[i], &bus, m.amp[i].address);
        CHECK(fs19xx_probe(&amps[i]) == Fs19xxOk);
        CHECK(fsm_find(fsm, fsm_size, m.amp[i].address, amps[i].devid, &p) == FsmOk);
        r |= fs19xx_configure(&amps[i], &p);
    }
    return r;
}

static void testConfigure(void)
{
    CHECK(configure_all(0) == 0);
    CHECK(m.violations == 0);
    for (int i = 0; i < 4; ++i) {
        const struct Amp *a = &m.amp[i];
        const uint16_t slot = (uint16_t)(a->address - 0x34);
        CHECK(amps[i].state == Fs19xxReady && amps[i].filtered == 0 && amps[i].waits_missed == 0 && amps[i].otp_unlocks == 1);
        CHECK(a->reg[0x1a] == slot && a->reg[0x1c] == slot + 8);       // its own block, the music scene only
        CHECK(a->reg[0xbd] == 0xa55f);                                  // bits 7:4 replaced, the rest kept
        CHECK(a->reg[FS19XX_REG_SPKCOEF] == 0x55 << 1);
        CHECK(a->reg[FS19XX_REG_PWRCTRL] == FS19XX_PWRCTRL_PWDN && a->reg[FS19XX_REG_SYSCTRL] == 0 && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);
    }
    // the first amplifier's order: soft reset (unacknowledged), power down, coefficient, units, then mute, power down, blocks off
    const uint8_t want[] = {FS19XX_REG_PWRCTRL, FS19XX_REG_PWRCTRL, FS19XX_REG_SPKCOEF, 0x1a, 0xbd, 0x0b, 0x0b, 0x1c,
                            FS19XX_REG_DACCTRL, FS19XX_REG_PWRCTRL, FS19XX_REG_SYSCTRL};
    unsigned k = 0;
    for (unsigned i = 0; i < m.writes && m.log[i].address == 0x35; ++i, ++k) CHECK(k < sizeof want && m.log[i].reg == want[k]);
    CHECK(k == sizeof want && !m.log[0].acked && m.log[0].value == FS19XX_PWRCTRL_I2CR && m.log[1].value == FS19XX_PWRCTRL_PWDN);
}

static void testHostilePreset(void)
{
    CHECK(configure_all(1) == 0);
    CHECK(m.violations == 0);
    for (int i = 0; i < 4; ++i) {
        CHECK(amps[i].filtered == 5 && amps[i].skipped == 1);   // PWRCTRL, DACCTRL, two VOLCTRL, the 60 s delay
        CHECK((m.amp[i].reg[FS19XX_REG_VOLCTRL] >> 8) <= FS19XX_GAIN_CAP && m.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN);
    }
    CHECK(m.slept_us < 4 * 200000ull);   // the 60 s delay was capped (100 ms per amplifier, plus the sequences' own waits)
    // a wait that never matches is counted, not fatal
    model_init();
    build_fsm(0);
    Fs19xxPreset p;
    fs19xx_init_amp(&amps[0], &bus, 0x35);
    CHECK(fs19xx_probe(&amps[0]) == 0 && fsm_find(fsm, fsm_size, 0x35, FS19XX_DEVID, &p) == 0);
    m.amp[0].reg[0xc0] = 0;
    CHECK(fs19xx_unit(&amps[0], 0x2fc0, 0x705a) == Fs19xxOk && amps[0].waits_missed == 1);
}

static void testStartStop(void)
{
    CHECK(configure_all(0) == 0);
    const unsigned before = m.writes;
    // a rate or clock piano never runs: refused before any write
    CHECK(fs19xx_start(&amps[0], 44100, 6144000, 160, 0) == Fs19xxErrArg && fs19xx_start(&amps[0], 48000, 3072000, 160, 0) == Fs19xxErrArg);
    CHECK(m.writes == before && amps[0].state == Fs19xxReady);
    m.allow_power = 1;
    for (int i = 0; i < 4; ++i) CHECK(fs19xx_start(&amps[i], 48000, 6144000, 255, 0) == Fs19xxOk && amps[i].state == Fs19xxPlaying);
    m.allow_power = 0;
    CHECK(m.violations == 0);
    for (int i = 0; i < 4; ++i) {
        const struct Amp *a = &m.amp[i];
        CHECK(amps[i].gain == FS19XX_GAIN_CAP && a->reg[FS19XX_REG_VOLCTRL] == 0xc0c0);   // 255 asked, 192 written
        CHECK(a->reg[FS19XX_REG_I2SCTRL] == (0x9000 | 0x0800 | 0x0003));                    // 48 kHz, data out, other bits kept
        CHECK(a->reg[FS19XX_REG_PLLCTRL1] == 0x0260 && a->reg[FS19XX_REG_PLLCTRL2] == 0x0540 && a->reg[FS19XX_REG_PLLCTRL3] == 0x0018);
        CHECK(a->reg[FS19XX_REG_OTPACC] == 0 && a->reg[FS19XX_REG_TSCTRL] == FS19XX_TSCTRL_TSEN && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE);
    }
    // the order of the first amplifier's start: the PLL under the OTP key, blocks on, power up, the capped gain, then unmute
    const unsigned s = find_write(before, 0x35, FS19XX_REG_I2SCTRL);
    const uint8_t want[] = {FS19XX_REG_I2SCTRL, FS19XX_REG_OTPACC, FS19XX_REG_PLLCTRL1, FS19XX_REG_PLLCTRL2, FS19XX_REG_PLLCTRL3,
                            FS19XX_REG_OTPACC, FS19XX_REG_SYSCTRL, FS19XX_REG_PWRCTRL, FS19XX_REG_VOLCTRL, FS19XX_REG_DACCTRL, FS19XX_REG_TSCTRL};
    for (unsigned k = 0; k < sizeof want; ++k) CHECK(m.log[s + k].address == 0x35 && m.log[s + k].reg == want[k]);
    CHECK(find_write(before, 0x35, FS19XX_REG_VOLCTRL) < find_write(before, 0x35, FS19XX_REG_DACCTRL));   // capped before unmuted

    // status: all four running and equal; then one differs, one stops, a read fails
    Fs19xxStatus st[4], first[4];
    for (int i = 0; i < 4; ++i) CHECK(fs19xx_read_status(&amps[i], &first[i]) == 0 && fs19xx_status_running(&first[i]));
    CHECK(fs19xx_group_check(first, 0, 4) == 0);
    for (int i = 0; i < 4; ++i) fs19xx_read_status(&amps[i], &st[i]);
    CHECK(fs19xx_group_check(st, first, 4) == 0);
    st[2].status ^= 0x0004;   // OTPDS changed on bottom left
    CHECK(fs19xx_group_check(st, first, 4) == 0x4);
    st[2].status ^= 0x0004;
    st[3].digstat = 0;        // bottom right's DAC stopped
    CHECK(fs19xx_group_check(st, first, 4) == 0x8);
    st[3].digstat = FS19XX_DIGSTAT_DACRUN;
    st[1].ok = 0;
    CHECK(fs19xx_group_check(st, first, 4) == 0x2);
    st[1].ok = 1;
    first[0].status = 0x0040;  // top left's own first reading differs from now
    CHECK(fs19xx_group_check(st, first, 4) == 0x1);
    st[0].volctrl = 0xd0c0;    // a gain above the cap read back
    CHECK(fs19xx_group_check(st, 0, 4) & 0x1);

    // shut down: mute, power down, 35 ms, blocks off; then the status says down
    const uint64_t slept = m.slept_us;
    for (int i = 0; i < 4; ++i) CHECK(fs19xx_shutdown(&amps[i]) == 0 && amps[i].state == Fs19xxReady);
    CHECK(m.slept_us - slept == 4 * 35000ull);
    for (int i = 0; i < 4; ++i) {CHECK(fs19xx_read_status(&amps[i], &st[i]) == 0 && fs19xx_status_down(&st[i]) && !fs19xx_status_running(&st[i]));}
    CHECK(fs19xx_mute(&amps[0], 0) == Fs19xxErrState && m.amp[0].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);   // not playing

    // started muted: no unmute until fs19xx_mute(.., 0)
    m.allow_power = 1;
    CHECK(fs19xx_start(&amps[0], 48000, 6144000, 160, 1) == 0 && amps[0].muted && m.amp[0].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);
    CHECK(m.amp[0].reg[FS19XX_REG_VOLCTRL] == 0xa0c0 && fs19xx_mute(&amps[0], 0) == 0 && m.amp[0].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE);
    m.allow_power = 0;
    CHECK(fs19xx_mute(&amps[0], 1) == 0 && m.amp[0].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE);
    CHECK(fs19xx_shutdown(&amps[0]) == 0 && m.violations == 0);
}

static void testFailures(void)
{
    // a bus failure in the middle of a start: the amplifier is shut down again and marked failed
    CHECK(configure_all(0) == 0);
    m.allow_power = 1;
    m.fail_at = (int)m.transfers + 8;   // the PWRCTRL power-up write of the start
    CHECK(fs19xx_start(&amps[0], 48000, 6144000, 160, 0) == Fs19xxErrBus && amps[0].state == Fs19xxFailed);
    m.allow_power = 0;
    CHECK(m.amp[0].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN);
    CHECK(m.amp[0].reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_MUTE && m.amp[0].reg[FS19XX_REG_SYSCTRL] == 0);
    // a shutdown whose mute write fails still powers down and switches the blocks off
    CHECK(configure_all(0) == 0);
    m.allow_power = 1;
    CHECK(fs19xx_start(&amps[1], 48000, 6144000, 160, 0) == 0);
    m.allow_power = 0;
    m.fail_reg = FS19XX_REG_DACCTRL;
    CHECK(fs19xx_shutdown(&amps[1]) == Fs19xxErrBus && amps[1].state == Fs19xxFailed);
    CHECK(m.amp[1].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN && m.amp[1].reg[FS19XX_REG_SYSCTRL] == 0);
    m.fail_reg = -1;
    // the soft reset never completes (CHIPINI stays 0): configure fails, the amplifier stays down
    model_init();
    build_fsm(0);
    Fs19xxPreset p;
    fs19xx_init_amp(&amps[2], &bus, 0x34);
    CHECK(fs19xx_probe(&amps[2]) == 0 && fsm_find(fsm, fsm_size, 0x34, FS19XX_DEVID, &p) == 0);
    m.fail_reg = FS19XX_REG_PWRCTRL;   // the power-down write after each reset request fails
    CHECK(fs19xx_configure(&amps[2], &p) == Fs19xxErrTimeout && amps[2].state == Fs19xxFailed);
    m.fail_reg = -1;
    CHECK(m.violations == 0);
}


static int hex_is(const uint8_t d[32], const char *hex)
{
    for (unsigned i = 0; i < 32; ++i) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1 || v != d[i]) return 0;
    }
    return 1;
}

static void testIdentity(void)
{
    uint8_t d[32];
    audio_sha256("", 0, d);
    CHECK(hex_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    audio_sha256("abc", 3, d);
    CHECK(hex_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    audio_sha256(two, (uint32_t)strlen(two), d);
    CHECK(hex_is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    AudioSha256 s;
    static uint8_t a[1000];
    memset(a, 'a', sizeof a);
    audio_sha256_init(&s);
    for (int i = 0; i < 1000; ++i) audio_sha256_update(&s, a, sizeof a);
    audio_sha256_final(&s, d);
    CHECK(hex_is(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    // byte-at-a-time and in one go agree across the 55/56/64-byte padding edges
    for (uint32_t n = 50; n < 130; ++n) {
        uint8_t one[32];
        audio_sha256(a, n, one);
        audio_sha256_init(&s);
        for (uint32_t i = 0; i < n; ++i) audio_sha256_update(&s, a + i, 1);
        audio_sha256_final(&s, d);
        CHECK(!memcmp(one, d, 32));
    }
    CHECK(hex_is(audio_preset_sha256, "ab060b8476065bd789f327f7f8e724a2e17d94e93a85e2e8aa4b4954ce88b90e") && AUDIO_PRESET_BYTES == 4348);
    static uint8_t fake[AUDIO_PRESET_BYTES];
    CHECK(!audio_preset_is_stock(fake, sizeof fake) && !audio_preset_is_stock(fake, 4347) && !audio_preset_is_stock(0, 4348));
}

int main(void)
{
    testBasics();
    testIdentity();
    testProbe();
    testContainer();
    testConfigure();
    testHostilePreset();
    testStartStop();
    testFailures();
    printf("PASS fs19xx: %u checks\n", checks);
    return 0;
}
