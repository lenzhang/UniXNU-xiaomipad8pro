// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano (Xiaomi Pad 8 Pro, SM8750) USB facts for PianoDwc3. Sources (no code copied from any of them):
//   DT  = out/inputs/piano-boot.dtb, the running Ubuntu DT (dtc -I dtb -O dts): usb@a600000, phy@88e3000, phy@88e8000,
//         pmic@7/phy@fd00, iommu@15000000, spmi@c400000, clock-controller@100000 / @f204008
//   GCC = mainline drivers/clk/qcom/gcc-sm8750.c, TCSR = tcsrcc-sm8750.c, QS = drivers/usb/dwc3/dwc3-qcom.c (register facts)
//   SUN = Project-SunUEFI uefi/core/PianoDwc3Device.c, PianoUsbPhy.c (BSD-2-Clause-Patent; measured on piano, 2026-10)
// docs/drivers/usb-device-debug.md section 2 has the same table with each fact's state.
#pragma once
#include <stdint.h>

namespace pianohw {

// DWC3 core (DT usb@a600000 reg 0xa600000 + 0xfc100, flattened qcom binding: QSCRATCH inside it)
constexpr uint64_t kDwc3 = 0x0a600000, kDwc3Bytes = 0x10000;   // global/device registers at +0xc100..+0xcfff
constexpr uint64_t kQscratch = 0x0a6f8800;                      // QS SDM845_QSCRATCH_BASE_OFFSET 0xf8800; SUN: 0x0A6F8810/30 measured
constexpr uint32_t kQsGeneralCfg = 0x08, kQsHsPhyCtrl = 0x10, kQsSsPhyCtrl = 0x30;
constexpr uint32_t kQsPipeUtmiClkSel = 1u << 0, kQsPipe3PhystatusSw = 1u << 3, kQsPipeUtmiClkDis = 1u << 8;   // GENERAL_CFG
constexpr uint32_t kQsUtmiOtgVbusValid = 1u << 20, kQsSwSessvldSel = 1u << 28;                              // HS_PHY_CTRL
constexpr uint32_t kQsLane0PwrPresent = 1u << 24;                                                           // SS_PHY_CTRL
// Interrupts (DT): dwc_usb3 GIC SPI 133 level-high; not used (PianoDwc3 polls; GEVNTSIZ.INTMASK keeps the line quiet)
constexpr uint32_t kDwc3Spi = 133;
// SMMU (DT iommus = <&apps_smmu 0x40 0x0>; apps SMMU 0x15000000 + 1 MiB, qcom,sm8750-smmu-500)
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kUsbStream = 0x40;

// GCC (DT clock-controller@100000; GCC offsets)
constexpr uint64_t kGcc = 0x00100000;
constexpr uint32_t kGccUsb30Bcr = 0x39000, kGccUsb30Gdsc = 0x39004, kGccUsb30Master = 0x39018, kGccUsb30Sleep = 0x39028,
                   kGccUsb30MockUtmi = 0x3902c, kGccUsb30MasterRcg = 0x39030, kGccUsb30MockUtmiRcg = 0x39048,
                   kGccUsb3PhyAux = 0x39064, kGccUsb3PhyComAux = 0x39068, kGccUsb3PhyPipe = 0x3906c,
                   kGccCfgNocUsb3Axi = 0x3908c, kGccAggreUsb3Axi = 0x39090,   // BRANCH_HALT_VOTED, enable bit in the same word
                   kGccQusb2PhyBcr = 0x12000, kGccUsb3PhyGdsc = 0x50018;
constexpr uint32_t kGdscPwrOn = 1u << 31, kGdscSwCollapse = 1u << 0, kCbcrClkOff = 1u << 31, kCbcrClkEnable = 1u << 0;
// TCSR clock references (DT clock-controller@f204008; TCSR offsets are from that base): eUSB2 PHY "ref" = USB2, QMP = USB3
constexpr uint64_t kTcsrUsb2ClkRef = 0x0f204008 + 0x2000, kTcsrUsb3ClkRef = 0x0f204008 + 0x3000;

// M31 eUSB2 PHY (DT phy@88e3000, qcom,sm8750-m31-eusb2-phy, reg 0x29c; resets = GCC_QUSB2PHY_PRIM_BCR). Registers are byte-wide,
// replicated across a 32-bit read (SUN). Offsets and the init writes are SUN's PianoUsbPhyStart (from Qualcomm's UsbConfigDxe).
constexpr uint64_t kM31 = 0x088e3000;
constexpr uint32_t kM31UtmiCtrl0 = 0x3c, kM31Por = 0x50, kM31Common0 = 0x54, kM31HsCtrl2 = 0x64, kM31Override = 0x94;
struct RegWrite { uint32_t offset, value; };
constexpr uint32_t kDelay = 0xffffffffu;   // {kDelay, n}: wait n microseconds
// SUN PianoUsbPhyStart after the PHY reset: override and POR asserted, the 38.4 MHz reference selected, factory tuning kept.
// Which word is which (UTMI_CTRL0, COMMON0 = PHY_ENABLE/RETENABLEN/SIDDQ/FSEL, HS_CTRL2 = VBUSVLDEXT) is SUN's reading: inferred.
constexpr RegWrite kM31Init[] = {
  {0x94, 2}, {0x50, 2}, {kDelay, 10},
  {0x154, 1}, {0xb8, 1}, {0x54, 0x69}, {0x16c, 0}, {0x17c, 7}, {0x168, 0}, {0x15c, 0}, {0x64, 0xf}, {0x64, 0xf}, {0x3c, 1},
  {0x54, 0x6b}, {0x54, 0x6b}, {0x50, 0}, {0x64, 7}, {0x94, 0}, {kDelay, 20},
};

// eUSB2 repeater (DT pmic@7 = pmih0108 SPMI SID 7, phy@fd00 qcom,pm8550b-eusb2-repeater); SUN read FD08 = FD46 = 0x80
constexpr uint32_t kRepeaterPpid = 0x7fd;
constexpr uint8_t kRepeaterStatus = 0x08, kRepeaterEnable = 0x46;
// SPMI arbiter v7 (DT spmi@c400000: core 0xc400000+0x3000, obsrvr 0xc440000+0x80000, cnfg 0xc42d000+0x4000, qcom,ee = 0)
constexpr uint64_t kSpmiCore = 0x0c400000, kSpmiObs = 0x0c440000, kSpmiCnfg = 0x0c42d000;

}  // namespace pianohw
