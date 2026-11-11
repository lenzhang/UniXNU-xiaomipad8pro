/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The portable part of the firmware loader: see PianoFirmwareCore.h. */
#include "PianoFirmwareCore.h"

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;}
static uint64_t pad8(uint64_t n) {return (n + 7) & ~(uint64_t)7;}

static int same(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

static int allowed(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

int pfw_pin_ok(const PianoFirmwarePin *pin)
{
    static const char magic[8] = PIANO_FW_PIN_MAGIC_INIT;
    if (!pin || !same(pin->magic, magic, sizeof magic) || !pin->size || pin->size > PIANO_FW_MAX_BYTES) return 0;
    size_t n = 0, part = 0, dots = 0;
    for (; n < PIANO_FW_PATH_BYTES && pin->path[n]; ++n) {
        const char c = pin->path[n];
        if (c == '/') {
            if (!part || dots == part) return 0;   /* an empty, "." or ".." component */
            part = dots = 0;
            continue;
        }
        if (!allowed(c)) return 0;
        ++part;
        dots += c == '.';
    }
    return n && n < PIANO_FW_PATH_BYTES && part && dots != part;
}

size_t pfw_full_path(const PianoFirmwarePin *pin, char *out, size_t cap)
{
    static const char dir[] = PIANO_FW_DIR "/";
    size_t n = 0;
    for (const char *p = dir; *p; ++p) {
        if (n + 1 >= cap) return 0;
        out[n++] = *p;
    }
    for (size_t i = 0; i < PIANO_FW_PATH_BYTES && pin->path[i]; ++i) {
        if (n + 1 >= cap) return 0;
        out[n++] = pin->path[i];
    }
    out[n] = 0;
    return n;
}

int pfw_check(const PianoFirmwarePin *pin, const uint8_t *data, uint64_t bytes, const PianoFwOps *ops)
{
    uint8_t digest[32];
    if (bytes != pin->size) return PIANO_FW_SIZE;
    ops->sha256(ops->context, data, (size_t)bytes, digest);
    return same(digest, pin->sha256, sizeof digest) ? PIANO_FW_OK : PIANO_FW_HASH;
}

int pfw_from_fat(FatVolume *v, const PianoFirmwarePin *pin, const PianoFwOps *ops, uint8_t **data)
{
    char path[sizeof PIANO_FW_DIR + PIANO_FW_PATH_BYTES + 1];
    FatFile f;
    *data = 0;
    if (!pfw_pin_ok(pin) || !pfw_full_path(pin, path, sizeof path)) return PIANO_FW_BAD_PIN;
    const int rc = fat_open(v, path, &f);
    if (rc == FatNotFound || rc == FatNotDirectory || (!rc && f.directory)) return PIANO_FW_NOT_FOUND;
    if (rc) return rc == FatReadError ? PIANO_FW_READ : PIANO_FW_DAMAGED;
    if (f.size != pin->size) return PIANO_FW_SIZE;
    uint8_t *buf = ops->alloc(ops->context, pin->size);
    if (!buf) return PIANO_FW_NO_MEMORY;
    const int read = fat_read(v, &f, 0, buf, pin->size);
    const int checked = read ? (read == FatReadError ? PIANO_FW_READ : PIANO_FW_DAMAGED) : pfw_check(pin, buf, pin->size, ops);
    if (checked) {
        ops->release(ops->context, buf, pin->size);
        return checked;
    }
    *data = buf;
    return PIANO_FW_OK;
}

int pfw_addon_find(const uint8_t *addon, uint64_t bytes, const char *path, PfwAddonEntry *entry)
{
    if (!addon || bytes < PFW_ADDON_HEADER || bytes > PIANO_FW_ADDON_MAX_BYTES || !same(addon, PFW_ADDON_MAGIC, 8) ||
        le32(addon + 12) != 0 || le64(addon + 16) != bytes)
        return PIANO_FW_DAMAGED;
    const uint32_t count = le32(addon + 8);
    if (count > PFW_ADDON_MAX_ENTRIES) return PIANO_FW_DAMAGED;
    int found = 0;
    uint64_t at = PFW_ADDON_HEADER;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < PFW_ADDON_ENTRY) return PIANO_FW_DAMAGED;
        const uint8_t *e = addon + at;
        const uint32_t name_bytes = le32(e);
        const uint64_t size = le64(e + 8), packed = le64(e + 16);
        if (name_bytes < 2 || name_bytes > PFW_ADDON_PATH_MAX || packed > bytes) return PIANO_FW_DAMAGED;
        const uint64_t name_at = at + PFW_ADDON_ENTRY, data_at = name_at + pad8(name_bytes);
        if (data_at > bytes || pad8(packed) > bytes - data_at) return PIANO_FW_DAMAGED;
        const char *name = (const char *)(addon + name_at);
        for (uint32_t k = 0; k + 1 < name_bytes; ++k) if (!name[k]) return PIANO_FW_DAMAGED;
        if (name[name_bytes - 1]) return PIANO_FW_DAMAGED;
        if (!found) {
            uint32_t k = 0;
            while (k + 1 < name_bytes && path[k] && path[k] == name[k]) ++k;
            if (k + 1 == name_bytes && !path[k]) {
                found = 1;
                entry->path = name;
                entry->mode = le32(e + 4);
                entry->size = size;
                entry->packed = addon + data_at;
                entry->packed_bytes = packed;
            }
        }
        at = data_at + pad8(packed);
    }
    if (at != bytes) return PIANO_FW_DAMAGED;   /* the total covers the entries exactly */
    return found ? PIANO_FW_OK : PIANO_FW_NOT_FOUND;
}

int pfw_from_addon(const uint8_t *addon, uint64_t bytes, const PianoFirmwarePin *pin, const PianoFwOps *ops, uint8_t **data)
{
    char path[sizeof PIANO_FW_DIR + PIANO_FW_PATH_BYTES + 1];
    PfwAddonEntry e;
    *data = 0;
    if (!pfw_pin_ok(pin) || !pfw_full_path(pin, path, sizeof path)) return PIANO_FW_BAD_PIN;
    const int rc = pfw_addon_find(addon, bytes, path, &e);
    if (rc) return rc;
    if (e.size != pin->size) return PIANO_FW_SIZE;
    uint8_t *buf = ops->alloc(ops->context, pin->size);
    if (!buf) return PIANO_FW_NO_MEMORY;
    const int checked = ops->inflate(ops->context, buf, pin->size, e.packed, (size_t)e.packed_bytes) ? PIANO_FW_READ
                                                                                                   : pfw_check(pin, buf, pin->size, ops);
    if (checked) {
        ops->release(ops->context, buf, pin->size);
        return checked;
    }
    *data = buf;
    return PIANO_FW_OK;
}

const char *piano_fw_error(int result)
{
    switch (result) {
    case PIANO_FW_OK: return "ok";
    case PIANO_FW_BAD_PIN: return "malformed pin";
    case PIANO_FW_NOT_FOUND: return "not found";
    case PIANO_FW_SIZE: return "another size than the pin's";
    case PIANO_FW_HASH: return "another SHA-256 than the pin's";
    case PIANO_FW_READ: return "read error";
    case PIANO_FW_NO_MEMORY: return "no memory";
    case PIANO_FW_NO_ESP: return "no ESP";
    case PIANO_FW_DAMAGED: return "damaged container";
    }
    return "?";
}

const char *piano_fw_source(int source)
{
    switch (source) {
    case PIANO_FW_FROM_EMBEDDED: return "built in";
    case PIANO_FW_FROM_ADDON: return "the boot's addon";
    case PIANO_FW_FROM_ESP: return "the ESP";
    }
    return "none";
}
