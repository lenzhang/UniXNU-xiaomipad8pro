/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Fs19xx.h"

#define FSM_HEADER 28u
#define FSM_SIZE_AT 22u
#define FSM_CRC_AT 24u
#define FSM_COUNT_AT 26u
#define FSM_BLOCK_HEAD 40u
#define FSM_BLOCK_VERSION_AT 0u
#define FSM_BLOCK_LISTS_AT 26u
#define FSM_BLOCK_ADDRESS_AT 30u
#define FSM_BLOCK_TYPE_AT 32u
#define FSM_DESC_BLOCK 0u
#define FSM_LIST_SPEAKER 1u
#define FSM_LIST_COMMON 2u
#define FSM_LIST_SCENES 3u
#define UNIT_SPECIAL 0xfu
#define UNIT_DELAY 1u
#define UNIT_WAIT 2u
#define DELAY_CAP_MS 100u
#define WAIT_TRIES 35u

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}

unsigned fs19xx_gain_clamp(unsigned requested) {return requested > FS19XX_GAIN_CAP ? FS19XX_GAIN_CAP : requested;}
int fs19xx_gain_db10(unsigned gain) {return gain > 255 ? 0 : -((int)(255 - gain) * 375 + 50) / 100;}

const char *fs19xx_status_bit(unsigned bit)
{
    static const char *const names[16] = {"BOVDS", "PLLS", "OTPDS", "OVDS", "UVDS", "OCDS", "CLKS", 0, "OTWDS", "BOPS", "BOLDS", "INDS",
                                          0, 0, "VDDPS", 0};
    return bit < 16 ? names[bit] : 0;
}

void fs19xx_init_amp(Fs19xx *a, const Fs19xxBus *bus, uint8_t address)
{
    for (unsigned i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    a->bus = bus; a->address = address; a->state = Fs19xxAbsent; a->muted = 1;
}

static void sleep_us(Fs19xx *a, uint32_t us) {if (a->bus->sleep_us) a->bus->sleep_us(a->bus->context, us);}

int fs19xx_read(Fs19xx *a, uint8_t reg, uint16_t *value)
{
    uint8_t in[2] = {0, 0};
    a->reads++;
    if (a->bus->transfer(a->bus->context, a->address, &reg, 1, in, 2)) {a->bus_errors++; a->last_error = Fs19xxErrBus; return Fs19xxErrBus;}
    *value = (uint16_t)(in[0] << 8 | in[1]);
    return Fs19xxOk;
}

int fs19xx_write(Fs19xx *a, uint8_t reg, uint16_t value)
{
    const uint8_t out[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    a->writes++;
    if (a->bus->transfer(a->bus->context, a->address, out, 3, 0, 0)) {a->bus_errors++; a->last_error = Fs19xxErrBus; return Fs19xxErrBus;}
    return Fs19xxOk;
}

int fs19xx_probe(Fs19xx *a)
{
    uint16_t id = 0, rev = 0;
    int r = Fs19xxErrBus;
    for (unsigned i = 0; i < 20 && r; ++i) {
        r = fs19xx_read(a, FS19XX_REG_DEVID, &id);
        if (r) sleep_us(a, 5000);
    }
    if (r) {a->state = Fs19xxAbsent; return a->last_error = Fs19xxErrTimeout;}
    a->devid = id;
    if (id >> 8 != FS19XX_DEVID_FAMILY) {a->state = Fs19xxAbsent; return a->last_error = Fs19xxErrId;}
    if (fs19xx_read(a, FS19XX_REG_REVID, &rev)) {a->state = Fs19xxAbsent; return Fs19xxErrBus;}
    a->revid = rev & 0xffu;
    a->state = Fs19xxIdentified;
    return Fs19xxOk;
}

/* ------------------------------------------------------------------------------------------------- the preset container */
uint16_t fsm_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = crc & 1u ? (uint16_t)(crc >> 1 ^ 0xa001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

int fsm_check(const uint8_t *data, uint32_t size, uint32_t *descriptors)
{
    if (!data || size < FSM_HEADER) return FsmShort;
    if (le16(data + FSM_SIZE_AT) != size) return FsmBadSize;
    if (fsm_crc16(data + FSM_COUNT_AT, size - FSM_COUNT_AT) != le16(data + FSM_CRC_AT)) return FsmBadCrc;
    const uint32_t n = le16(data + FSM_COUNT_AT);
    if (FSM_HEADER + 4u * n > size) return FsmBadIndex;
    if (descriptors) *descriptors = n;
    return FsmOk;
}

/* A list of `type` in a block (room: the bytes from the block to the end of the file): its count and first unit, unit bytes each. */
static int block_list(const uint8_t *block, uint32_t room, uint16_t type, uint32_t unit, const uint8_t **list, uint32_t *count)
{
    const uint32_t n = le16(block + FSM_BLOCK_LISTS_AT);
    if (FSM_BLOCK_HEAD + 4u * n > room) return FsmBadList;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *e = block + FSM_BLOCK_HEAD + 4u * i;
        if (le16(e + 2) != type) continue;
        const uint32_t at = FSM_BLOCK_HEAD + le16(e);
        if (at + 2u > room) return FsmBadList;
        const uint32_t c = le16(block + at);
        if (at + 2u + c * unit > room) return FsmBadList;
        *list = block + at + 2u;
        *count = c;
        return FsmOk;
    }
    *list = 0; *count = 0;
    return FsmOk;   /* absent: an empty list */
}

int fsm_find(const uint8_t *data, uint32_t size, uint8_t address, uint16_t devid, Fs19xxPreset *out)
{
    uint32_t n = 0;
    int r = fsm_check(data, size, &n);
    if (r) return r;
    for (unsigned i = 0; i < sizeof *out; ++i) ((uint8_t *)out)[i] = 0;
    const uint8_t *block = 0;
    for (uint32_t i = 0; i < n && !block; ++i) {
        const uint32_t at = le16(data + FSM_HEADER + 4u * i);
        if (le16(data + FSM_HEADER + 4u * i + 2u) != FSM_DESC_BLOCK || at + FSM_BLOCK_HEAD > size) continue;
        if (le16(data + at + FSM_BLOCK_ADDRESS_AT) == address) block = data + at;
    }
    if (!block) return FsmNoBlock;
    const uint32_t room = (uint32_t)(data + size - block);
    out->version = le16(block + FSM_BLOCK_VERSION_AT);
    out->type = le16(block + FSM_BLOCK_TYPE_AT);
    if (!(out->version & 0x8000u)) return FsmInvalid;
    if (out->type >> 8 != devid >> 8) return FsmWrongDevice;
    const uint8_t *list;
    uint32_t count;
    if ((r = block_list(block, room, FSM_LIST_SPEAKER, 2, &list, &count))) return r;
    if (list && count > 1) out->tcoef = le16(list + 2);
    if ((r = block_list(block, room, FSM_LIST_COMMON, 4, &out->common, &out->common_count))) return r;
    if ((r = block_list(block, room, FSM_LIST_SCENES, 6, &out->scenes, &out->scene_count))) return r;
    return FsmOk;
}

const char *fsm_error(int error)
{
    switch (error) {
    case FsmOk: return "ok";
    case FsmShort: return "shorter than its header";
    case FsmBadSize: return "size field differs from the file's size";
    case FsmBadCrc: return "CRC16 mismatch";
    case FsmBadIndex: return "descriptors past the end";
    case FsmNoBlock: return "no block for this address";
    case FsmInvalid: return "block not marked valid";
    case FsmWrongDevice: return "block for another device type";
    case FsmBadList: return "list past the end";
    default: return "?";
    }
}

/* --------------------------------------------------------------------------------------------------- units and the filter */
uint16_t fs19xx_filter(uint8_t reg, uint16_t value, int *changed)
{
    uint16_t v = value;
    if (reg == FS19XX_REG_PWRCTRL) v |= FS19XX_PWRCTRL_PWDN;
    else if (reg == FS19XX_REG_DACCTRL) v |= FS19XX_DACCTRL_MUTE_BIT;
    else if (reg == FS19XX_REG_VOLCTRL && (unsigned)(v >> 8) > FS19XX_GAIN_CAP) v = (uint16_t)(FS19XX_GAIN_CAP << 8 | (v & 0xffu));
    if (changed) *changed = v != value;
    return v;
}

static uint16_t field_mask(unsigned first, unsigned width_minus_1)
{
    unsigned last = first + width_minus_1;
    if (last > 15) last = 15;
    return (uint16_t)((0xffffu >> (15 - last)) & (0xffffu << first));
}

int fs19xx_unit(Fs19xx *a, uint16_t unit, uint16_t value)
{
    const uint8_t reg = (uint8_t)unit;
    const unsigned first = (unit >> 8) & 0xfu, width = (unit >> 12) & 0xfu;
    if (first == UNIT_SPECIAL && width) {
        if (width == UNIT_DELAY) {
            uint32_t ms = value;
            if (ms > DELAY_CAP_MS) {ms = DELAY_CAP_MS; a->filtered++;}
            sleep_us(a, ms * 1000u);
            return Fs19xxOk;
        }
        if (width == UNIT_WAIT) {
            const unsigned at = (value >> 8) & 0xfu;
            const uint16_t mask = field_mask(0, (value >> 12) & 0xfu);
            for (unsigned i = 0; i < WAIT_TRIES; ++i) {
                uint16_t v;
                sleep_us(a, 1000);
                if (fs19xx_read(a, reg, &v)) return Fs19xxErrBus;
                if (((v >> at) & mask) == (value & 0xffu)) return Fs19xxOk;
            }
            a->waits_missed++;
            return Fs19xxOk;
        }
        a->skipped++;
        return Fs19xxOk;
    }
    uint16_t v = value;
    if (!(first == 0 && width == 15)) {
        const uint16_t mask = field_mask(first, width);
        uint16_t old;
        if (fs19xx_read(a, reg, &old)) return Fs19xxErrBus;
        v = (uint16_t)((old & ~mask) | ((uint32_t)value << first & mask));
    }
    int changed = 0;
    v = fs19xx_filter(reg, v, &changed);
    if (changed) a->filtered++;
    if (reg == FS19XX_REG_OTPACC && v == FS19XX_OTPACC_KEY) a->otp_unlocks++;
    return fs19xx_write(a, reg, v);
}

/* ------------------------------------------------------------------------------------------------------- the sequences */
static int soft_reset(Fs19xx *a)
{
    for (unsigned i = 0; i < 5; ++i) {
        uint16_t v;
        (void)fs19xx_write(a, FS19XX_REG_PWRCTRL, FS19XX_PWRCTRL_I2CR);   /* not acknowledged: its result means nothing */
        (void)fs19xx_read(a, FS19XX_REG_PWRCTRL, &v);
        sleep_us(a, 15000);
        if (fs19xx_write(a, FS19XX_REG_PWRCTRL, FS19XX_PWRCTRL_PWDN) || fs19xx_read(a, FS19XX_REG_CHIPINI, &v)) continue;
        if (v == 0x0003u || v == 0x0300u) return Fs19xxOk;
    }
    return Fs19xxErrTimeout;
}

int fs19xx_shutdown(Fs19xx *a)
{
    int r = 0;
    r |= fs19xx_write(a, FS19XX_REG_DACCTRL, FS19XX_DACCTRL_MUTE);
    r |= fs19xx_write(a, FS19XX_REG_PWRCTRL, FS19XX_PWRCTRL_PWDN);
    sleep_us(a, 35000);
    r |= fs19xx_write(a, FS19XX_REG_SYSCTRL, 0);
    a->muted = 1;
    if (a->state == Fs19xxPlaying || a->state == Fs19xxReady) a->state = r ? Fs19xxFailed : Fs19xxReady;
    return r ? Fs19xxErrBus : Fs19xxOk;
}

int fs19xx_configure(Fs19xx *a, const Fs19xxPreset *p)
{
    if (a->state != Fs19xxIdentified && a->state != Fs19xxReady) return Fs19xxErrState;
    if (!p || (p->common_count && !p->common) || (p->scene_count && !p->scenes)) return Fs19xxErrArg;
    int r = soft_reset(a);
    if (!r) r = fs19xx_write(a, FS19XX_REG_SPKCOEF, (uint16_t)(p->tcoef << 1));
    for (uint32_t i = 0; !r && i < p->common_count; ++i)
        r = fs19xx_unit(a, le16(p->common + 4 * i), le16(p->common + 4 * i + 2));
    for (uint32_t i = 0; !r && i < p->scene_count; ++i)
        if (le16(p->scenes + 6 * i) & FSM_SCENE_MUSIC) r = fs19xx_unit(a, le16(p->scenes + 6 * i + 2), le16(p->scenes + 6 * i + 4));
    a->state = Fs19xxReady;
    const int s = fs19xx_shutdown(a);
    if (r || s) {a->state = Fs19xxFailed; return a->last_error = r ? r : s;}
    return Fs19xxOk;
}

int fs19xx_start(Fs19xx *a, uint32_t rate, uint32_t bit_clock, unsigned gain, int muted)
{
    if (a->state != Fs19xxReady) return Fs19xxErrState;
    if (rate != 48000 || bit_clock != 6144000) return Fs19xxErrArg;
    const unsigned g = fs19xx_gain_clamp(gain);
    uint16_t i2s;
    int r = fs19xx_read(a, FS19XX_REG_I2SCTRL, &i2s);
    if (!r) r = fs19xx_write(a, FS19XX_REG_I2SCTRL, (uint16_t)((i2s & ~(FS19XX_I2SCTRL_SR_MASK | FS19XX_I2SCTRL_DOE)) |
                                                               FS19XX_SR_48K << FS19XX_I2SCTRL_SR_SHIFT | FS19XX_I2SCTRL_DOE));
    if (!r) r = fs19xx_write(a, FS19XX_REG_OTPACC, FS19XX_OTPACC_KEY);
    if (!r) r = fs19xx_write(a, FS19XX_REG_PLLCTRL1, FS19XX_PLL1_6M144);
    if (!r) r = fs19xx_write(a, FS19XX_REG_PLLCTRL2, FS19XX_PLL2_6M144);
    if (!r) r = fs19xx_write(a, FS19XX_REG_PLLCTRL3, FS19XX_PLL3_6M144);
    if (!r) r = fs19xx_write(a, FS19XX_REG_OTPACC, 0);
    if (!r) r = fs19xx_write(a, FS19XX_REG_SYSCTRL, FS19XX_SYSCTRL_ON);
    if (!r) r = fs19xx_write(a, FS19XX_REG_PWRCTRL, 0);
    if (!r) r = fs19xx_write(a, FS19XX_REG_VOLCTRL, (uint16_t)(g << 8 | 0x00c0u));
    if (!r && !muted) r = fs19xx_write(a, FS19XX_REG_DACCTRL, FS19XX_DACCTRL_UNMUTE);
    sleep_us(a, 10000);
    uint16_t ts;
    if (!r) r = fs19xx_read(a, FS19XX_REG_TSCTRL, &ts);
    if (!r) r = fs19xx_write(a, FS19XX_REG_TSCTRL, (uint16_t)(ts | FS19XX_TSCTRL_TSEN));
    a->state = Fs19xxPlaying;
    if (r) {
        fs19xx_shutdown(a);
        a->state = Fs19xxFailed;
        return a->last_error = r;
    }
    a->gain = (uint8_t)g;
    a->muted = muted ? 1 : 0;
    return Fs19xxOk;
}

int fs19xx_mute(Fs19xx *a, int mute)
{
    if (!mute && a->state != Fs19xxPlaying) return Fs19xxErrState;
    const int r = fs19xx_write(a, FS19XX_REG_DACCTRL, mute ? FS19XX_DACCTRL_MUTE : FS19XX_DACCTRL_UNMUTE);
    if (!r) a->muted = mute ? 1 : 0;
    return r;
}

/* --------------------------------------------------------------------------------------------------------------- status */
int fs19xx_read_status(Fs19xx *a, Fs19xxStatus *s)
{
    int r = 0;
    r |= fs19xx_read(a, FS19XX_REG_STATUS, &s->status);
    r |= fs19xx_read(a, FS19XX_REG_DIGSTAT, &s->digstat);
    r |= fs19xx_read(a, FS19XX_REG_PWRCTRL, &s->pwrctrl);
    r |= fs19xx_read(a, FS19XX_REG_SYSCTRL, &s->sysctrl);
    r |= fs19xx_read(a, FS19XX_REG_VOLCTRL, &s->volctrl);
    r |= fs19xx_read(a, FS19XX_REG_DACCTRL, &s->dacctrl);
    s->ok = !r;
    return r ? Fs19xxErrBus : Fs19xxOk;
}

int fs19xx_status_running(const Fs19xxStatus *s)
{
    return s->ok && !(s->pwrctrl & FS19XX_PWRCTRL_PWDN) && s->sysctrl == FS19XX_SYSCTRL_ON && (s->digstat & FS19XX_DIGSTAT_DACRUN);
}

int fs19xx_status_down(const Fs19xxStatus *s) {return s->ok && (s->pwrctrl & FS19XX_PWRCTRL_PWDN);}

unsigned fs19xx_group_check(const Fs19xxStatus *now, const Fs19xxStatus *first, unsigned count)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < count && i < 32; ++i) {
        const Fs19xxStatus *s = &now[i];
        if (!fs19xx_status_running(s) || (s->volctrl >> 8) > FS19XX_GAIN_CAP ||
            (now[0].ok && (s->status & FS19XX_STATUS_BITS) != (now[0].status & FS19XX_STATUS_BITS)) ||
            (first && first[i].ok && (s->status & FS19XX_STATUS_BITS) != (first[i].status & FS19XX_STATUS_BITS)))
            bad |= 1u << i;
    }
    return bad;
}

void fs19xx_status_names(uint16_t status, char *out, unsigned size)
{
    unsigned n = 0;
    if (!size) return;
    for (unsigned bit = 0; bit < 16; ++bit) {
        const char *name = fs19xx_status_bit(bit);
        if (!name || !(status >> bit & 1u)) continue;
        if (n && n + 1 < size) out[n++] = ' ';
        for (const char *c = name; *c && n + 1 < size; ++c) out[n++] = *c;
    }
    out[n] = 0;
}
