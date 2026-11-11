// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano (Xiaomi Pad 8 Pro, SM8750) facts for USB host mode and the Type-C port, as PianoUsbHost's level 1 survey reads them. Names
// and layout are ours; no code or table is copied from any source. Each block says where its facts come from and how sure they are
// (docs/drivers/usb-host.md section 2 has the same facts with their state):
//   DT    = out/inputs/piano-boot.dtb (dtc -I dtb -O dts): usb@a600000, phy@88e8000, i2c@a8c000/typec-mux@42, pmic-glink
//           (orientation-gpios), pinctrl@f100000 (gpio-reserved-ranges, usb-orientation-default-state, qup-i2c3-data-clk-state)
//   LNX   = the Ubuntu piano kernel (GPL-2.0, read as a specification only): drivers/usb/dwc3/{core.h,core.c,drd.c,host.c,
//           dwc3-qcom.c}, drivers/phy/qualcomm/phy-qcom-qmp-combo.c (+ phy-qcom-qmp-dp-com-v3.h, phy-qcom-qmp-pcs-v8.h),
//           drivers/usb/typec/tcpm/qcom/qcom_pmic_typec_port.c, drivers/usb/typec/ucsi/{ucsi.h,ucsi_glink.c},
//           drivers/soc/qcom/pmic_glink_altmode.c, drivers/usb/typec/mux/fsa4480.c, phy-qcom-eusb2-repeater.c
//   XHCI  = the xHCI 1.2 specification's register layout (capability, operational, port and extended-capability registers)
//   SUN   = Project-SunUEFI (BSD-2-Clause-Patent) PianoUsbTypec.c and docs/archive/storage-usb/usb-host-port-plan.md (piano-measured)
// The shared USB facts (GCC clocks, QSCRATCH, SPMI windows) are PianoDwc3's (package/PianoDwc3/PianoUsbHw.h, included).
#pragma once
#include <stdint.h>
#include "PianoUsbHw.h"

namespace pianousbhost {

// ------------------------------------------------------------------------------------------------------------------- TLMM (DT)
// GPIO n: configuration at n * 0x1000 (pull 1:0, function 5:2, drive 8:6, output enable 9), level at +4 (in bit 0, out bit 1).
// Reserved for other execution environments (never loaded): 36-39, 48-51, 74 (gpio-reserved-ranges).
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000, kTlmmCfg = 0x0, kTlmmIo = 0x4;
constexpr unsigned kGpioOrientation = 61;   // pmic-glink orientation-gpios: input, pull-down, 2 mA; LNX ucsi_glink: high = reverse
constexpr unsigned kGpioMuxSda = 44, kGpioMuxScl = 45;   // i2c@a8c000 (QUP1 SE3, function qup1_se3, pull-up): the DIO4485's bus
constexpr bool tlmmReserved(unsigned gpio) { return (gpio >= 36 && gpio <= 39) || (gpio >= 48 && gpio <= 51) || gpio == 74; }
constexpr uint64_t tlmmCfg(unsigned gpio) { return kTlmm + uint64_t(gpio) * kTlmmStride + kTlmmCfg; }
constexpr uint64_t tlmmIo(unsigned gpio) { return kTlmm + uint64_t(gpio) * kTlmmStride + kTlmmIo; }

// DIO4485 (DT typec-mux@42 "dioo,dio4485" on i2c@a8c000; LNX fsa4480.c): not read at level 1 (a bus transaction needs the pins and
// the QUP engine set up: writes). Facts for later levels: ID register 0x00 reads 0xf6; 0x04 switch enable, 0x05 switch select,
// 0x12 function enable; Linux's USB-only state 0x04 = 0x98, 0x05 = 0x18; DP-only (pin assignment C/E) turns the USB switch off.
constexpr uint8_t kMuxAddress = 0x42, kMuxIdReg = 0x00, kMuxIdDio4485 = 0xf6, kMuxEnableReg = 0x04, kMuxSelectReg = 0x05;

// ------------------------------------------------------------------------------------------------------ DWC3 core (DT, LNX)
// usb@a600000: the xHCI registers are the first 32 KiB (DWC3_XHCI_REGS_END 0x7fff), the global registers start at +0xc100.
constexpr uint64_t kXhci = pianohw::kDwc3;
constexpr uint32_t kXhciBytes = 0x8000;
constexpr uint32_t kGctlPrtcapShift = 12, kGctlPrtcapMask = 3u << 12;   // GCTL[13:12]: 1 host, 2 device, 3 OTG (DRD)
enum Prtcap : uint32_t { kPrtcapNone = 0, kPrtcapHost = 1, kPrtcapDevice = 2, kPrtcapOtg = 3 };
constexpr uint32_t kGctlCoreSoftReset = 1u << 11, kGctlDsblClkGtng = 1u << 0;
constexpr uint32_t kGstsCurmodMask = 3u;                                // GSTS[1:0]: 0 device, 1 host
constexpr uint32_t kGusb2PhySusphy = 1u << 6, kGusb2PhyEnblslpm = 1u << 8;
constexpr uint32_t kGusb3PipeSusphy = 1u << 17, kGusb3PipeUxExitPx = 1u << 27, kGusb3PipeDisRxDetInP3 = 1u << 28;

// ------------------------------------------------------------------------------------------------------------- xHCI (XHCI)
constexpr uint32_t kCapLength = 0x00, kHcsParams1 = 0x04, kHcsParams2 = 0x08, kHcsParams3 = 0x0c, kHccParams1 = 0x10, kDbOff = 0x14,
                   kRtsOff = 0x18, kHccParams2 = 0x1c;
// Operational registers (from CAPLENGTH)
constexpr uint32_t kOpUsbCmd = 0x00, kOpUsbSts = 0x04, kOpPageSize = 0x08, kOpConfig = 0x38, kOpPortBase = 0x400, kOpPortStride = 0x10;
constexpr uint32_t kUsbCmdRun = 1u << 0, kUsbStsHalted = 1u << 0, kUsbStsCnr = 1u << 11, kUsbStsHce = 1u << 12;
// PORTSC
constexpr uint32_t kPortCcs = 1u << 0, kPortPed = 1u << 1, kPortOca = 1u << 3, kPortPr = 1u << 4, kPortPp = 1u << 9;
constexpr uint32_t portLinkState(uint32_t portsc) { return (portsc >> 5) & 0xfu; }
constexpr uint32_t portSpeed(uint32_t portsc) { return (portsc >> 10) & 0xfu; }
// Extended capabilities: ID 7:0, next pointer 15:8 (in dwords); 1 legacy support, 2 supported protocol, 10 debug capability
constexpr uint32_t kXcapLegacy = 1, kXcapProtocol = 2, kXcapPower = 3, kXcapMsi = 5, kXcapDebug = 10, kXcapMax = 16;

// ------------------------------------------------------------------------------------ QMP USB3/DP combo PHY (DT, LNX v8 layout)
// phy@88e8000 (qcom,sm8750-qmp-usb3-dp-phy), 16 KiB, power domain USB3_PHY_GDSC (GCC 0x50018), clocks aux 0x39064, com_aux 0x39068,
// usb3_pipe 0x3906c, ref = TCSR USB3 clkref. Linux's sm8750 configuration uses the v8 offsets: DP_COM at +0 (the Type-C mux
// registers), USB3 serdes +0x1000, PCS +0x1e00, PCS_USB +0x2100, DP PHY +0x3c00. No reset values are in the sources.
constexpr uint64_t kQmp = 0x088e8000;
constexpr uint32_t kQmpBytes = 0x4000;
constexpr uint32_t kDpComPhyMode = 0x00, kDpComSwReset = 0x04, kDpComPowerDown = 0x08, kDpComSwi = 0x0c, kDpComTypec = 0x10,
                   kDpComTypecPwrdn = 0x14, kDpComResetOvrd = 0x1c;
constexpr uint32_t kPhyModeUsb3 = 1u << 0, kPhyModeDp = 1u << 1;            // 1 USB3 only, 2 DP only (4 lanes), 3 USB3 + DP 2 lanes
constexpr uint32_t kTypecPortSelectVal = 1u << 0, kTypecPortSelectMux = 1u << 1, kTypecInvertCc = 1u << 2;   // VAL 1 = reverse
constexpr uint32_t kPcs = 0x1e00, kPcsSwReset = 0x00, kPcsStatus1 = 0x14, kPcsPowerDown = 0x40, kPcsStart = 0x44;
constexpr uint32_t kPcsPhyStatus = 1u << 6;                                 // PCS_STATUS1 bit 6: the PHY is not (yet) ready
constexpr uint32_t kDpPhy = 0x3c00, kDpPhyPdCtl = 0x18, kDpPhyMode = 0x1c;   // DP_PHY_MODE 0x5c normal, 0x4c reverse (LNX)

// --------------------------------------------------------------------------------------------------- PMIC Type-C block (LNX)
// PMIH010x (SID 7): the Type-C peripheral is the charger block + 0x600 (+0x300 more for SUBTYPE 0xba..0xc4), found by
// package/PianoPower/ChargerPmic.h (SUN's layout). Bit meanings from qcom_pmic_typec_port.c (same register offsets on piano).
constexpr uint8_t kTcSnkStatus = 0x06, kTcSrcStatus = 0x08, kTcSmState = 0x09, kTcSmStatus = 0x0a, kTcMisc = 0x0b,
                  kTcLegacy = 0x0d, kTcUusb = 0x0f, kTcRtStatus = 0x10, kTcModeCfg = 0x44, kTcVconn = 0x46, kTcCcout = 0x48;
// SNK_STATUS (we are the sink, a source's Rp seen): 3 default USB power, 2 1.5 A, 1 3.0 A, 0 short; 6:4 debug accessory currents
constexpr uint8_t kSnkRpStd = 1u << 3, kSnkRp1p5 = 1u << 2, kSnkRp3p0 = 1u << 1, kSnkRpShort = 1u << 0, kSnkDamMask = 0x70;
// SRC_STATUS (we are the source, a sink's termination seen): 3 Rd/open (a USB device), 2 Rd/Ra (a device behind a powered cable,
// VCONN), 1 Ra/open, 0 Ra/Ra (audio accessory), 4 debug accessory, 5 high battery
constexpr uint8_t kSrcRdOpen = 1u << 3, kSrcRdRa = 1u << 2, kSrcRaOpen = 1u << 1, kSrcRaRa = 1u << 0, kSrcDebug = 1u << 4;
// MISC_STATUS: 0 attached, 1 orientation (1 = CC2), 3 debounce done, 4 VBUS error, 5 VBUS detected, 6 source mode (we source),
// 7 water detected. SM_STATUS: 5 VBUS at vSafe5V, 6 at vSafe0V, 7 USBIN below LV. MODE_CFG: 2 source only, 1 sink only, 0 disabled,
// 4 try-sink, 3 try-source (0 in 2:0 = dual role). SUN: piano entering UEFI reads MODE_CFG 0x02 (sink only).
constexpr uint8_t kMiscAttached = 1u << 0, kMiscCc2 = 1u << 1, kMiscDebounced = 1u << 3, kMiscVbusError = 1u << 4,
                  kMiscVbus = 1u << 5, kMiscSourceMode = 1u << 6, kMiscWater = 1u << 7;
constexpr uint8_t kSmVsafe5v = 1u << 5, kSmVsafe0v = 1u << 6;
constexpr uint8_t kModeDisabled = 1u << 0, kModeSinkOnly = 1u << 1, kModeSourceOnly = 1u << 2, kModeTrySrc = 1u << 3,
                  kModeTrySnk = 1u << 4;

// ------------------------------------------------------------------------------------------- eUSB2 repeater (DT, LNX, SUN)
// pmih0108 SID 7 @0xfd00 (PPID 0x7fd, pianohw::kRepeaterPpid): status 0x08 / enable 0x46 (SUN: 0x80 / 0x80); host mode forces the
// 19.2 MHz clock: FORCE_EN_5 0xe8 bit 6 and FORCE_VAL_5 0xed bit 6 (0 in device mode, LNX); tuning 0x51/0x54/0x56/0x57 (SUN: the
// stock host overrides 0f/01/03/05 vs the device values 0a/03/03 seen after the firmware).
constexpr uint8_t kRepStatus = 0x08, kRepEnable = 0x46, kRepForceEn5 = 0xe8, kRepForceVal5 = 0xed, kRepForce19p2 = 1u << 6;
constexpr uint8_t kRepTune[] = {0x51, 0x54, 0x56, 0x57};

}  // namespace pianousbhost
