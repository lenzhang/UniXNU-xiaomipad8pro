/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Piano's sensor policies: see SensorsPolicy.h. */
#include "SensorsPolicy.h"

/* The users of low DRAM: every reserved-memory region of the live DT below 4 GiB but the CDSP carveout
 * (docs/hyp-evidence/memory-map-running-dt.txt), the firmware's holes in it, the UEFI regions natively still in use (pianoPkg
 * MemoryMapLib.c: the firmware volume with its stacks and heaps, the boot image's "Kernel" area, the DXE heap, Piano_Log,
 * Display_Demura), and the CDSP carveout's slices that are not the sensors' (PianoDspMemory.h). Ranges may overlap; the first match
 * names the user. */
const PsRange ps_reserved[] = {
    {0x80000000ull, 0x1200000ull, "gunyah-hyp"},
    {0x81200000ull, 0x200000ull, "cpucp"},
    {0x81400000ull, 0x560000ull, "a firmware hole"},
    {0x81a00000ull, 0x40000ull, "xbl-dtlog"},
    {0x81c00000ull, 0x60000ull, "aop-image"},
    {0x81c60000ull, 0x20000ull, "aop-cmd-db"},
    {0x81c80000ull, 0x74000ull, "aop-tme-uefi-merged"},
    {0x81cf4000ull, 0x1000ull, "display-dump"},
    {0x81d00000ull, 0x200000ull, "smem"},
    {0x81f00000ull, 0x100000ull, "pdp-ns-shared"},
    {0x82000000ull, 0x380000ull, "cpucp-scandump"},
    {0x82380000ull, 0x20000ull, "adsp-mhi (the DSP's)"},
    {0x823a0000ull, 0x40000ull, "soccp-sdi"},
    {0x823e0000ull, 0x80000ull, "pmic-minii-dump"},
    {0x824a0000ull, 0x100000ull, "pvmfw"},
    {0x825a0000ull, 0x60000ull, "a firmware hole"},
    {0x82600000ull, 0x100000ull, "global-sync"},
    {0x82700000ull, 0x100000ull, "tz-stat"},
    {0x82800000ull, 0x2000000ull, "qdss"},
    {0x84a00000ull, 0x4900000ull, "dsm-partition-1"},
    {0x89300000ull, 0xa80000ull, "dsm-partition-2"},
    {0x8ba00000ull, 0xf600000ull, "mpss (disabled, but the UEFI's PIL_Reserved)"},
    {0x9b000000ull, 0x80000ull, "q6-mpss-dtb"},
    {0x9b080000ull, 0x10000ull, "ipa-fw"},
    {0x9b090000ull, 0xa000ull, "ipa-gsi"},
    {0x9b09a000ull, 0x2000ull, "gpu-micro-code (the GPU's ZAP)"},
    {0x9b09c000ull, 0x4000ull, "reserved@9b09c000"},
    {0x9b0a0000ull, 0x1e0000ull, "spss"},
    {0x9b280000ull, 0x40000ull, "spu-tz-shared"},
    {0x9b2c0000ull, 0x40000ull, "spu-modem-shared"},
    {0x9b300000ull, 0x800000ull, "camera"},
    {0x9bb00000ull, 0x800000ull, "camera-2"},
    {0x9c300000ull, 0x800000ull, "video (PianoVideo's firmware)"},
    {0x9cb00000ull, 0x700000ull, "cvp (the native console mailbox)"},
    {PIANO_DSP_MEMORY_AUDIO, PIANO_DSP_MEMORY_AUDIO_BYTES, "the CDSP carveout's audio slice (PianoDspMemory.h)"},
    {PIANO_DSP_MEMORY_SPARE, PIANO_DSP_MEMORY_SPARE_BYTES, "the CDSP carveout's spare slice (PianoDspMemory.h)"},
    {0x9eb00000ull, 0x80000ull, "q6-cdsp-dtb"},
    {0x9eb80000ull, 0x80000ull, "reserved@9eb80000"},
    {0x9ec00000ull, 0x180000ull, "soccp"},
    {0x9ed80000ull, 0x80000ull, "q6-adsp-dtb (the DSP's)"},
    {0xa3500000ull, 0x2c80000ull, "Display_Demura / ramoops"},
    {0xa7000000ull, 0x1000000ull, "the UEFI firmware volume, its stacks and heaps"},
    {0xa8000000ull, 0x10000000ull, "the UEFI's Kernel area (the boot image)"},
    {0xb8000000ull, 0x200000ull, "xbl-ramdump"},
    {0xb8200000ull, 0x5780000ull, "adspslpi (the DSP's image)"},
    {0xbd930000ull, 0x1a5d0000ull, "the UEFI's DXE heap"},
    {0xd4e23000ull, 0x2dd000ull, "hwfence-shbuf"},
    {0xd7f00000ull, 0x100000ull, "Piano_Log"},
    {0xd8000000ull, 0x600000ull, "tz-merged"},
    {0xd8600000ull, 0xb120000ull, "a firmware hole"},
    {0xf3800000ull, 0x5000000ull, "trust-ui-vm"},
    {0xf8800000ull, 0x3800000ull, "oem-vm"},
    {0xfc000000ull, 0x400000ull, "vm-shared"},
    {0xfc800000ull, 0x2b00000ull, "the splash framebuffer"},
    {0xff800000ull, 0x800000ull, "llcc-lpi"},
};
const unsigned ps_reserved_count = sizeof ps_reserved / sizeof ps_reserved[0];

int ps_arena_check(uint64_t pa, uint64_t bytes, const char **why)
{
    const char *reason = 0;
    if (!bytes || (pa & 0xfff) || (bytes & 0xfff)) reason = "not whole 4 KiB pages";
    else if (pa < 0x80000000ull) reason = "below DRAM";
    else if (pa + bytes > 0x100000000ull || pa + bytes < pa) reason = "not below 4 GiB (the DSP takes 32-bit addresses)";
    for (unsigned i = 0; !reason && i < ps_reserved_count; ++i)
        if (pa < ps_reserved[i].base + ps_reserved[i].bytes && ps_reserved[i].base < pa + bytes) reason = ps_reserved[i].owner;
    if (why) *why = reason;
    return reason ? -1 : 0;
}

/* Battery first (the owner's priority, backlight.md): a dark room near the panel's floor, an office (300-500 lux) around 40 %, full
 * scale only in daylight. Owner-tunable later through PianoBacklight's offset (the brightness keys shift the curve). */
const PsCurvePoint ps_als_curve[] = {
    {0, 40},     {5, 60},     {20, 110},   {50, 170},   {100, 240},  {200, 320},
    {400, 420},  {800, 540},  {1500, 660}, {3000, 800}, {6000, 920}, {10000, 1000},
};
const unsigned ps_als_curve_points = sizeof ps_als_curve / sizeof ps_als_curve[0];

uint32_t ps_als_curve_permille(uint32_t lux)
{
    if (lux <= ps_als_curve[0].lux) return ps_als_curve[0].permille;
    for (unsigned i = 1; i < ps_als_curve_points; ++i) {
        const PsCurvePoint a = ps_als_curve[i - 1], b = ps_als_curve[i];
        if (lux <= b.lux) {
            const uint64_t span = b.lux - a.lux, at = lux - a.lux;
            return a.permille + (uint32_t)(((uint64_t)(b.permille - a.permille) * at + span / 2) / span);
        }
    }
    return ps_als_curve[ps_als_curve_points - 1].permille;
}

void ps_als_init(PsAls *a)
{
    a->has = 0;
    a->goal = 0;
    a->smooth_q16 = 0;
    a->published = 0xffffffffu;
    a->last_ms = a->published_ms = 0;
}

static uint32_t distance(uint32_t a, uint32_t b) {return a > b ? a - b : b - a;}

int32_t ps_als_step(PsAls *a, int have_sample, int32_t milli_lux, uint64_t now_ms)
{
    if (have_sample) {
        const uint32_t lux = milli_lux <= 0 ? 0 : (uint32_t)((milli_lux + 500) / 1000);
        const uint32_t target = ps_als_curve_permille(lux);
        if (!a->has) {
            a->has = 1;
            a->goal = target;
            a->smooth_q16 = target << 16;
            a->last_ms = a->published_ms = now_ms;
            a->published = target;
            return (int32_t)target;
        }
        if (distance(target, a->goal) >= PS_ALS_HYSTERESIS) a->goal = target;
    }
    if (!a->has) return -1;
    if (now_ms > a->last_ms) {
        const uint64_t dt = now_ms - a->last_ms;
        const uint64_t tau = (a->goal << 16) > a->smooth_q16 ? PS_ALS_UP_MS : PS_ALS_DOWN_MS;
        const int64_t alpha = (int64_t)((dt << 16) / (tau + dt));            /* 0 .. 65536 */
        const int64_t gap = (int64_t)((uint64_t)a->goal << 16) - (int64_t)a->smooth_q16;
        int64_t next = (int64_t)a->smooth_q16 + gap * alpha / 65536;
        const int64_t left = ((int64_t)a->goal << 16) - next;
        if (left > -(2 << 16) && left < (2 << 16)) next = (int64_t)a->goal << 16;   /* within two permille: there */
        a->smooth_q16 = (uint32_t)next;
        a->last_ms = now_ms;
    }
    const uint32_t value = (a->smooth_q16 + 0x8000u) >> 16;
    if (value == a->published || now_ms - a->published_ms < PS_ALS_PUBLISH_MS) return -1;
    if (distance(value, a->published) < PS_ALS_STEP && value != a->goal) return -1;
    a->published = value;
    a->published_ms = now_ms;
    return (int32_t)value;
}

int ps_edge_of(int sno_state)
{
    switch (sno_state) {
    case SNO_PORTRAIT: return PS_EDGE_TOP;            /* in the panel frame: gravity along -y, the camera edge up (docked) */
    case SNO_LANDSCAPE_LEFT: return PS_EDGE_LEFT;
    case SNO_UPSIDE_DOWN: return PS_EDGE_BOTTOM;
    case SNO_LANDSCAPE_RIGHT: return PS_EDGE_RIGHT;
    }
    return PS_EDGE_NONE;
}

const char *ps_edge_name(int edge)
{
    switch (edge) {
    case PS_EDGE_TOP: return "top";
    case PS_EDGE_LEFT: return "left";
    case PS_EDGE_BOTTOM: return "bottom";
    case PS_EDGE_RIGHT: return "right";
    }
    return "none";
}

int ps_accel_sample(SnoState *s, const SfxAxes *axes, const uint32_t bits[3], uint64_t now_ms)
{
    int32_t in[3], out[3];
    for (unsigned i = 0; i < 3; ++i) in[i] = sfx_accel_ios(bits[i]);
    sfx_axes_apply(axes, in, out);
    return sno_update(s, out[0], out[1], out[2], now_ms);
}

void ps_rotation_defaults(PsRotationConfig *c)
{
    c->degrees[PS_EDGE_TOP] = 0;
    c->degrees[PS_EDGE_LEFT] = 90;
    c->degrees[PS_EDGE_BOTTOM] = 180;
    c->degrees[PS_EDGE_RIGHT] = 270;
    c->settle_ms = 600;
    c->min_interval_ms = 1000;
}

void ps_rotation_init(PsRotation *r)
{
    r->rotation = -1;
    r->edge = PS_EDGE_NONE;
    r->edge_since = r->changed_ms = 0;
}

int ps_rotation_step(PsRotation *r, const PsRotationConfig *c, int edge, int attached, int locked, uint64_t now_ms)
{
    if (attached) {
        r->edge = PS_EDGE_NONE;          /* after a detach the edge must settle afresh */
        if (r->rotation == 0) return -1;
        r->rotation = 0;
        r->changed_ms = now_ms;
        return 0;
    }
    if (edge < PS_EDGE_TOP || edge > PS_EDGE_RIGHT) {r->edge = PS_EDGE_NONE; return -1;}
    if (edge != r->edge) {
        r->edge = edge;
        r->edge_since = now_ms;
        return -1;
    }
    if (locked || now_ms - r->edge_since < c->settle_ms) return -1;
    const int want = c->degrees[edge];
    if (want == r->rotation) return -1;
    if (r->rotation >= 0 && now_ms - r->changed_ms < c->min_interval_ms) return -1;
    r->rotation = want;
    r->changed_ms = now_ms;
    return want;
}
