// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AOP command database (cmd-db) lookup of an RPMh resource's address, and the apps RSC register layout: the two parts of
// package/PianoPower/QcomRpmh.c (same repository) this driver needs, in its own namespace (no symbol shared with PianoPower's).
// The keyboard's supplies are piano's vreg_l11b_1p8 (vddio, "ldob11", 1800 mV) and vreg_l14b_3p3 (dvdd, "ldob14", 3296 mV) (boot DT
// keyboard@4c; Project-SunUEFI PianoPogoBoard.c), a VRM resource each: +0 voltage in mV, +4 enable, +8 mode (7: high power).
#ifndef PIANO_KEYBOARD_CMD_DB_H
#define PIANO_KEYBOARD_CMD_DB_H
#include <stddef.h>
#include <stdint.h>

namespace pianokeyboard {

constexpr uint32_t kCmdDbMagic = 0x0c0330db;

// Header: u32 version, u32 magic, 8 slave records of 16 bytes (u16 id, header offset, data offset, entry count, ...), u32 checksum,
// u32 reserved, then the data area the offsets count from (byte 144). Entries are 24 bytes: an 8-byte id, two u32 priorities, u32
// address, u16 aux length, u16 aux offset. Returns 0 with the address, -1 not a cmd-db, -2 no such resource, -3 corrupt.
inline int cmdDbFind(const uint8_t *db, size_t bytes, const char *name, uint32_t *address)
{
  auto u16 = [db](size_t at) { return uint32_t(db[at] | db[at + 1] << 8); };
  auto u32 = [&u16](size_t at) { return u16(at) | u16(at + 2) << 16; };
  if (bytes < 144 || u32(4) != kCmdDbMagic) return -1;
  char id[8] = {0};
  for (unsigned i = 0; i < 8 && name[i]; ++i) id[i] = name[i];
  for (unsigned s = 0; s < 8; ++s) {
    const size_t slave = 8 + 16 * s;
    if (!u16(slave)) break;
    const size_t heads = 144 + u16(slave + 2), count = u16(slave + 6);
    if (heads > bytes || count * 24 > bytes - heads) return -3;
    for (size_t e = 0; e < count; ++e) {
      const size_t entry = heads + 24 * e;
      bool same = true;
      for (unsigned i = 0; i < 8 && same; ++i) same = db[entry + i] == uint8_t(id[i]);
      if (!same) continue;
      *address = u32(entry + 16);
      return *address ? 0 : -3;
    }
  }
  return -2;
}

// An RSC DRV's TCS registers relative to its TCS block (DRV base + tcs-offset), by the DRV id register's major version (bits 23:16).
struct RscLayout {
  uint32_t tcsStride, cmdStride, irqStatus, irqClear, waitForCmpl, control, status, cmdEnable, cmdMsgid, cmdAddr, cmdData;
};
inline RscLayout rscLayout(uint32_t drvId)
{
  if (((drvId >> 16) & 0xff) >= 3) return {672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c};
  return {672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
}

}  // namespace pianokeyboard
#endif
