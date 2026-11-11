/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The sensor pack on the ESP: see SensorsLoad.h. */
#include "SensorsLoad.h"

int sl_load(FatVolume *v, const char *path, uint8_t *buf, uint32_t cap, uint32_t *size, SspPack *pack)
{
    FatFile f;
    *size = 0;
    const int rc = fat_open(v, path, &f);
    if (rc == FatNotFound || rc == FatNotDirectory || rc == FatIsDirectory || (!rc && f.directory)) return SL_NOT_FOUND;
    if (rc) return rc == FatReadError ? SL_READ : SL_NOT_FAT;
    if (f.size > cap || f.size > SL_MAX_BYTES) return SL_TOO_BIG;
    if (f.size < SSP_HEADER) return SL_INVALID;
    if (fat_read(v, &f, 0, buf, f.size)) return SL_READ;
    const int checked = sl_check(buf, f.size, pack);
    if (checked == SL_OK) *size = f.size;
    return checked;
}

int sl_check(const uint8_t *buf, uint32_t size, SspPack *pack)
{
    SspHeader h;
    if (size > SL_MAX_BYTES) return SL_TOO_BIG;
    if (!buf || size < SSP_HEADER || ssp_header(buf, size, &h) || h.total != size) return SL_INVALID;
    const uint32_t table = ssp_table_bytes(&h);
    if (table > size - SSP_HEADER || ssp_open(pack, &h, buf + SSP_HEADER)) return SL_INVALID;
    return SL_OK;
}

const char *sl_error_name(int error)
{
    switch (error) {
    case SL_OK: return "ok";
    case SL_NOT_FAT: return "not a FAT volume";
    case SL_NOT_FOUND: return "no pack";
    case SL_TOO_BIG: return "too big";
    case SL_READ: return "read error";
    case SL_INVALID: return "not a valid pack";
    }
    return "?";
}

static int has_prefix(const SspPack *pack, const char *prefix)
{
    for (uint32_t i = 0; i < pack->count; ++i) {
        SspEntry e;
        ssp_entry(pack, i, &e);
        const char *a = prefix, *b = e.name;
        while (*a && *a == *b) {++a; ++b;}
        if (!*a && !(e.flags & SSP_DIRECTORY)) return 1;
    }
    return 0;
}

uint32_t sl_contents(const SspPack *pack)
{
    uint32_t bits = 0;
    if (ssp_find(pack, "/vendor/etc/sensors/sns_reg_config") >= 0) bits |= SL_HAS_REG_CONFIG;
    if (ssp_find(pack, "/odm/etc/sensors/config/json.lst") >= 0) bits |= SL_HAS_JSON_LIST;
    if (has_prefix(pack, "/mnt/vendor/persist/sensors/registry/registry/")) bits |= SL_HAS_REGISTRY;
    if (ssp_find(pack, "/sys/devices/soc0/soc_id") >= 0) bits |= SL_HAS_SOC0;
    return bits;
}
