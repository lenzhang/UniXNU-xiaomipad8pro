// SPDX-License-Identifier: BSD-2-Clause-Patent
// The initialisation sequence of the SM8750 PCIe0 QMP gen3x2 PHY (v7 layout), as a table of (offset, byte value) writes in the order
// they are made. 128 writes: POWER_DOWN_CONTROL = 3 first, then the SerDes, both RX lanes, both TX lanes, the PCS and PCS_MISC, and
// last SW_RESET = 0 and START_CONTROL = 3.
//
// Provenance (UPSTREAM has the same record): the table is PaganiWifi's `phy::kSequence` (UniXNU-Pagani-Drivers
// drivers/PaganiWifi/PaganiWifiTables.h 3d570b7, BSD-2-Clause-Patent), which that driver took from the OnePlus 13T's **running
// device tree** (`qcom,phy-sequence` of its vendor PCIe node), not from Linux. Its own note says where it differs from mainline's
// `sm8750_qmp_gen3x2_pcie_*_tbl`: RX_MODE_01_HIGH4 / RX_MODE_10_HIGH4 on both lanes are 0x8f where mainline writes 0x9c, offset
// 0x5e4 is written 0x0f where mainline writes EQ_CONFIG2 at 0x5e0, and 0x8f8 = 0x1f and 0x1800 = 0 are writes mainline does not
// make. Open question: a vendor device tree's sequence and Qualcomm's own driver tables plainly share an ancestor at Qualcomm, so
// whether this table is "independent" of the GPL-2.0 tables in `phy-qcom-qmp-pcie.c` cannot be settled from here; what is certain is
// that it is not a copy of them (the writes above differ), that it reached us through BSD-licensed code, and that each entry is a
// register value, a hardware fact. docs/drivers/wifi.md section 11 carries the same note; if the owner wants the question closed, the
// sequence can be replaced by one read at run time from piano's own stock device tree, which no repository would then carry.
//
// These are the 13T's values for the same SoC and the same PHY block; piano's DT names the same compatible string
// (`qcom,sm8750-qmp-gen3x2-pcie-phy`) and the same 8 KiB window, and carries no sequence of its own (mainline drives this PHY from
// its tables). `piano-wifi-physeq=0` applies only the three frame writes, so that a boot can tell "the table is wrong for piano"
// from "the PHY does not come up at all".
#pragma once
#include <stdint.h>
#include "WifiHw.h"

namespace pianowifi {

struct PhyWrite { uint16_t offset; uint8_t value; };

constexpr PhyWrite kPhySequence[] = {
  {0x0440, 0x03}, {0x00c0, 0x01}, {0x00cc, 0x62}, {0x00d0, 0x02}, {0x0060, 0xf8}, {0x0064, 0x01}, {0x0000, 0x93}, {0x0004, 0x01},
  {0x00e0, 0x90}, {0x00e4, 0x82}, {0x00f4, 0x07}, {0x0070, 0x02}, {0x0010, 0x02}, {0x0074, 0x16}, {0x0014, 0x16}, {0x0078, 0x36},
  {0x0018, 0x36}, {0x0110, 0x08}, {0x00bc, 0x0a}, {0x0120, 0x42}, {0x0080, 0x04}, {0x0084, 0x0d}, {0x0020, 0x0a}, {0x0024, 0x1a},
  {0x0088, 0x41}, {0x0028, 0x34}, {0x0090, 0xab}, {0x0094, 0xaa}, {0x0098, 0x01}, {0x0030, 0x55}, {0x0034, 0x55}, {0x0038, 0x01},
  {0x0140, 0x14}, {0x0164, 0x34}, {0x003c, 0x01}, {0x001c, 0x04}, {0x0174, 0x16}, {0x01bc, 0x0f}, {0x0170, 0xa0}, {0x13a4, 0x38},
  {0x12dc, 0x11}, {0x1360, 0xbf}, {0x1364, 0xbf}, {0x1368, 0xb7}, {0x136c, 0xea}, {0x135c, 0x3f}, {0x1374, 0x09}, {0x1378, 0x49},
  {0x137c, 0x1b}, {0x1380, 0x8f}, {0x1370, 0xd1}, {0x1388, 0x09}, {0x138c, 0x49}, {0x1390, 0x1b}, {0x1394, 0x8f}, {0x1384, 0xd1},
  {0x12c4, 0x3e}, {0x12c8, 0x1e}, {0x12cc, 0xd2}, {0x1208, 0x09}, {0x1214, 0x05}, {0x124c, 0x08}, {0x1250, 0x08}, {0x12d8, 0x09},
  {0x1318, 0x1c}, {0x131c, 0x60}, {0x12f8, 0x07}, {0x13f8, 0x08}, {0x1800, 0x00}, {0x1084, 0x35}, {0x108c, 0x10}, {0x1090, 0x31},
  {0x1094, 0x7f}, {0x10e4, 0x02}, {0x1040, 0x08}, {0x103c, 0x14}, {0x1ba4, 0x38}, {0x1adc, 0x11}, {0x1b60, 0xbf}, {0x1b64, 0xbf},
  {0x1b68, 0xb7}, {0x1b6c, 0xea}, {0x1b5c, 0x3f}, {0x1b74, 0x09}, {0x1b78, 0x49}, {0x1b7c, 0x1b}, {0x1b80, 0x8f}, {0x1b70, 0xd1},
  {0x1b88, 0x09}, {0x1b8c, 0x49}, {0x1b90, 0x1b}, {0x1b94, 0x8f}, {0x1b84, 0xd1}, {0x1ac4, 0x3e}, {0x1ac8, 0x1e}, {0x1acc, 0xd2},
  {0x1a08, 0x09}, {0x1a14, 0x05}, {0x1a4c, 0x08}, {0x1a50, 0x08}, {0x1ad8, 0x09}, {0x1b18, 0x1c}, {0x1b1c, 0x60}, {0x1af8, 0x07},
  {0x1bf8, 0x08}, {0x1884, 0x35}, {0x188c, 0x10}, {0x1890, 0x31}, {0x1894, 0x7f}, {0x18e4, 0x02}, {0x1840, 0x08}, {0x183c, 0x14},
  {0x04dc, 0x05}, {0x0588, 0x77}, {0x0598, 0x0b}, {0x08a4, 0x1e}, {0x08f4, 0x27}, {0x05e4, 0x0f}, {0x080c, 0x1d}, {0x0814, 0x07},
  {0x0820, 0xc1}, {0x0894, 0x00}, {0x08f8, 0x1f}, {0x05d0, 0x8c}, {0x0568, 0x17}, {0x0570, 0x2e}, {0x0400, 0x00}, {0x0444, 0x03},
};
constexpr unsigned kPhySequenceCount = sizeof kPhySequence / sizeof kPhySequence[0];

// The sequence's frame, which also is the whole of it with piano-wifi-physeq=0: powered, out of reset, started.
constexpr unsigned kPhyFrameTail = 2;   // the last two writes (SW_RESET, START_CONTROL) go after the pipe clock is switched over
static_assert(kPhySequenceCount == 128, "the donor's sequence has 128 writes");
static_assert(kPhySequence[0].offset == kPcs + kPcsPowerDown && kPhySequence[0].value == 3, "it opens with POWER_DOWN_CONTROL = 3");
static_assert(kPhySequence[kPhySequenceCount - 2].offset == kPcs + kPcsSwReset && kPhySequence[kPhySequenceCount - 2].value == 0,
              "SW_RESET = 0 is the second to last write");
static_assert(kPhySequence[kPhySequenceCount - 1].offset == kPcs + kPcsStart && kPhySequence[kPhySequenceCount - 1].value == 3,
              "START_CONTROL = 3 is the last write");
// Every offset is inside the PHY's window and 4-byte aligned (the tests check it too, so a mistyped entry cannot reach hardware).
constexpr bool phySequenceInRange()
{
  for (unsigned i = 0; i < kPhySequenceCount; ++i)
    if ((kPhySequence[i].offset & 3) || kPhySequence[i].offset + 4 > kPhyBytes) return false;
  return true;
}
static_assert(phySequenceInRange(), "a PHY sequence offset is outside the window or misaligned");

}  // namespace pianowifi
