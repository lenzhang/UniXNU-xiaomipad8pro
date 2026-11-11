// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano (Xiaomi Pad 8 Pro, SM8750) facts for the WCN7860 Wi-Fi path, as PianoWifi's level 1 survey reads them. Sources (facts only,
// no code, comment or table copied from any of them):
//   DT   = out/inputs/piano-boot.dtb, the running Ubuntu DT (dtc -I dtb -O dts): pcie@1c00000 and its pcie@0/wifi@0, phy@1c06000,
//          wcn7860-pmu, pinctrl@f100000 (wlan-en-state, pcie0-default-state), spmi@c400000 pmic@5/gpio@8800 (bt-en-default-state),
//          rsc@16500000 regulators-1..5, aop-cmd-db@81c60000, iommu@15000000
//   GCC  = mainline drivers/clk/qcom/gcc-sm8750.c and include/dt-bindings/clock/qcom,sm8750-gcc.h (register offsets, vote bits)
//   TCSR = mainline drivers/clk/qcom/tcsrcc-sm8750.c (tcsr_pcie_0_clkref_en)
//   PCI  = mainline drivers/pci/controller/dwc/pcie-qcom.c (cfg_1_9_0, the SM8550 data SM8750 falls back to), pcie-designware.h
//   PHY  = mainline drivers/phy/qualcomm/phy-qcom-qmp-pcie.c (sm8750_qmp_gen3x2_pciephy_cfg: qmp_pcie_offsets_v7), phy-qcom-qmp-pcs-v7.h
//   SHENG = UniXNU-sheng package/WifiProbe.cpp (BSD-2-Clause-Patent; SM8550 + WCN7850, hardware-verified there): which reads are
//          safe when (a block whose power domain or interface clock is off stalls the bus), the TLMM pin layout, the SMPS page rule
//   PAGANI = UniXNU-Pagani-Drivers drivers/PaganiWifi/PaganiWifiTables.h (BSD-2-Clause-Patent, 3d570b7; SM8750 + the same 17cb:110e
//          chip on the OnePlus 13T, host-tested only): the same GCC/PARF/PHY/TCSR offsets as mainline, the PMIC LDO page rule, hwcg
// docs/drivers/wifi.md section 2 has the same facts with their state (verified / inferred / needs-boot).
#pragma once
#include <stdint.h>

namespace pianowifi {

// ---- PCIe0 root complex (DT pcie@1c00000, qcom,pcie-sm8750 + qcom,pcie-sm8550, num-lanes 2, linux,pci-domain 0) ----
// reg: parf 0x1c00000+0x3000, dbi 0x40000000+0xf1d, elbi 0x40000f20+0xa8, atu 0x40001000+0x1000, config 0x40100000+0x100000,
// mhi 0x1c03000+0x1000. ranges: I/O 0x40200000+1 MiB, 32-bit memory 0x40300000+0x3d00000 (PCI = CPU address), 64-bit memory
// 0x4_0000_0000 + 0x3_0000_0000. The endpoint wifi@0 (compatible pci17cb,110e) is bus 1 device 0 below the root port (pcie@0).
constexpr uint64_t kParf = 0x01c00000, kParfBytes = 0x3000;
constexpr uint64_t kDbi = 0x40000000, kDbiBytes = 0x1000;
constexpr uint64_t kAtu = 0x40001000, kAtuBytes = 0x1000;
constexpr uint64_t kConfig = 0x40100000;   // never read by level 1 (an access through an iATU nobody programmed can abort)
constexpr uint64_t kMem32 = 0x40300000, kMem32Bytes = 0x3d00000;
constexpr uint16_t kEndpointVendor = 0x17cb, kEndpointDevice = 0x110e;
// PARF (PCI): system control, PM control, PHY control (bit 0 test power-down), LTSSM (bit 8 enable, state 5:0), interrupt status
// (bit 1 link down), status, DBI base (v2, 64-bit), slave address space size (v2), ATU base, device type (4 = root complex),
// BDF-to-SID config (bit 0 bypass). PM_STTS 0x24 is SHENG's/PAGANI's name (mainline does not define it): read for the record only.
constexpr uint32_t kParfSysCtrl = 0x00, kParfPmCtrl = 0x20, kParfPmStts = 0x24, kParfPhyCtrl = 0x40, kParfLtssm = 0x1b0,
                   kParfIntAllStatus = 0x224, kParfStatus = 0x230, kParfDbiBase = 0x350, kParfSlvSpace = 0x358,
                   kParfAtuBase = 0x634, kParfDeviceType = 0x1000, kParfBdfToSidCfg = 0x2c00;
constexpr uint32_t kParfLtssmEnable = 1u << 8;
// DBI, the root port's own configuration space (type 1 header) and the DesignWare port logic
constexpr uint32_t kDbiId = 0x00, kDbiCommand = 0x04, kDbiClass = 0x08, kDbiBusNumbers = 0x18, kDbiCapPtr = 0x34,
                   kPortDebug0 = 0x728, kPortDebug1 = 0x72c;
constexpr uint32_t kPortDebug1LinkUp = 1u << 4, kLtssmMask = 0x3f, kLtssmL0 = 0x11;
// iATU (unrolled, 0x200 per outbound region at the ATU base): control 1 at +0, control 2 at +4 (bit 31 enable), base +8/+0xc,
// limit +0x10, target +0x14/+0x18
constexpr uint32_t kAtuRegion = 0x200, kAtuCtrl2Enable = 1u << 31;

// ---- PCIe0 QMP PHY (DT phy@1c06000, qcom,sm8750-qmp-gen3x2-pcie-phy, 0x2000 bytes; power-domains gcc 1 = PCIE_0_PHY_GDSC;
// resets gcc 7 = PCIE_0_PHY_BCR; clocks aux, cfg_ahb, TCSR ref, rchng 100 MHz, pipe) ----
// The v7 layout (serdes 0x0, pcs 0x400, pcs_misc 0x800, tx 0x1000, rx 0x1200, tx2 0x1800, rx2 0x1a00; SHENG's SM8550 PHY is v6 with
// pcs at 0x200, not this one): PCS SW_RESET +0x00, PCS_STATUS1 +0x14 (bit 6 PHYSTATUS: set while the PHY is not ready),
// POWER_DOWN_CONTROL +0x40 (3 = powered), START_CONTROL +0x44 (3 = started).
constexpr uint64_t kPhy = 0x01c06000, kPhyBytes = 0x2000;
constexpr uint32_t kPcs = 0x400, kPcsSwReset = 0x00, kPcsStatus1 = 0x14, kPcsPowerDown = 0x40, kPcsStart = 0x44;
constexpr uint32_t kPcsPhyStatus = 1u << 6;

// ---- GCC (DT clock-controller@100000; mainline gcc-sm8750.c, the same values as PAGANI's table) ----
constexpr uint64_t kGcc = 0x00100000;
// Power domains: GDSCR (bit 31 PWR_ON, bit 0 SW_COLLAPSE) and CFG_GDSCR at +4 (bit 16 power-up complete, bit 15 power-down complete,
// which Linux polls: POLL_CFG_GDSCR). Both are votable through the collapse register (a set bit keeps the domain collapsed: bit 0
// PCIe0, bit 2 PCIe0 PHY). DT power-domains gcc 0 = PCIE_0_GDSC (controller), gcc 1 = PCIE_0_PHY_GDSC (phy@1c06000).
constexpr uint32_t kGccPcie0Gdsc = 0x6b004, kGccPcie0PhyGdsc = 0x6c000, kGccCollapse = 0x5214c;
constexpr uint32_t kCollapsePcie0 = 1u << 0, kCollapsePcie0Phy = 1u << 2;
// Block resets (BCR, bit 0 asserts): DT resets gcc 4 = PCIE_0_BCR (controller), gcc 7 = PCIE_0_PHY_BCR (PHY); the others for the record
struct Reset { const char *name; uint32_t bcr; };
constexpr Reset kResets[] = {{"pcie0", 0x6b000}, {"pcie0_phy", 0x6c01c}, {"link_down", 0x6c014}, {"nocsr_com_phy", 0x6c020},
                             {"phy_nocsr_com_phy", 0x6c028}, {"pcie_phy", 0x6f000}, {"phy_cfg_ahb", 0x6f00c}, {"phy_com", 0x6f010}};
constexpr unsigned kResetCount = sizeof kResets / sizeof kResets[0];
enum : unsigned { kResetPcie0 = 0, kResetPcie0Phy = 1 };
// Branch clocks (CBCR: bit 31 CLK_OFF, bit 1 HW_CTL, bit 0 CLK_ENABLE), each voted through one bit of a vote register. hwcg: the
// branch is gated by hardware when idle, so it may read CLK_OFF although it is voted (PAGANI); level 1 counts such a branch as
// running only with piano-wifi-hwcg=1 and only when its vote bit and HW_CTL are set.
struct Branch { const char *name; uint32_t cbcr; uint32_t vote; uint8_t bit; bool hwcg; };
constexpr uint32_t kGccVote8 = 0x52008, kGccVote0 = 0x52000;
constexpr Branch kBranches[] = {
  {"slv_axi", 0x6b020, kGccVote8, 0, true},           {"mstr_axi", 0x6b030, kGccVote8, 1, true},
  {"cfg_ahb", 0x6b040, kGccVote8, 2, true},           {"aux", 0x6b044, kGccVote8, 3, false},
  {"pipe", 0x6b054, kGccVote8, 4, false},             {"slv_q2a", 0x6b01c, kGccVote8, 5, false},
  {"cnoc_sf_axi", 0x10058, kGccVote8, 6, true},       {"qmip_ahb", 0x6b018, kGccVote0, 11, true},
  {"aggre_noc_axi", 0x10068, kGccVote0, 12, true},    {"ddrss_sf_qtb", 0x1007c, kGccVote0, 19, true},
  {"cfg_noc_anoc_ahb", 0x10050, kGccVote0, 20, true}, {"phy_rchng", 0x6b064, kGccVote0, 22, false},
};
constexpr unsigned kBranchCount = sizeof kBranches / sizeof kBranches[0];
enum : unsigned { kSlvAxi = 0, kMstrAxi, kCfgAhb, kAux, kPipe, kSlvQ2a, kCnocSfAxi, kQmipAhb, kAggreNoc, kDdrssSf, kCfgNocAnoc, kPhyRchng };
// RCGs (CMD_RCGR at +0: bit 0 update, bit 31 root off; CFG_RCGR at +4) and the pipe clock mux (0 = the PHY's pipe clock, 2 = TCXO)
constexpr uint32_t kGccPcie0AuxRcg = 0x6b084, kGccPcie0RchngRcg = 0x6b068, kGccPcie0PipeMux = 0x6b080;
constexpr uint32_t kGdscPwrOn = 1u << 31, kGdscSwCollapse = 1u << 0, kCbcrClkOff = 1u << 31, kCbcrHwCtl = 1u << 1, kBcrAssert = 1u << 0;

// ---- TCSR clock references (DT clock-controller@f204008; the PHY's "ref" is TCSR clock 0, tcsr_pcie_0_clkref_en, bit 0) ----
constexpr uint64_t kTcsrPcie0ClkRef = 0x0f204008;

// ---- TLMM (DT pinctrl@f100000, 0x102000 bytes, 216 pins; reserved 36-39, 48-51, 74, none of them ours) ----
// Pin n: configuration at n * 0x1000 (pull 1:0 (0 none, 1 down, 2 keeper, 3 up), function 5:2, drive 8:6 ((v + 1) * 2 mA), output
// enable 9), input (bit 0) and output (bit 1) at +4 (SHENG; the same layout PianoPower and PianoDwc3 use).
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000;
struct Pin { const char *name; unsigned gpio; };
// DT: wcn7860-pmu wlan-enable-gpios <tlmm 16 0>, swctrl-gpios <tlmm 18 0>; pcie@0 reset-gpios <tlmm 102 active-low>, wake-gpios
// <tlmm 104 active-low>; pcie0-default-state: gpio102 "gpio" pull-down (PERST#), gpio103 "pcie0_clk_req_n" pull-up, gpio104
// "gpio" pull-up (WAKE#); wlan-en-state: gpio16 "gpio" pull-down output-low. Also read, for the record (SM8750 pinctrl / PAGANI,
// not in piano's DT): gpio19 (function wcn_sw), gpio202 and gpio203 (eGPIO host/device sign of life on the 13T).
constexpr Pin kPins[] = {{"WLAN_EN", 16}, {"SW_CTRL", 18}, {"PERST#", 102}, {"CLKREQ#", 103}, {"WAKE#", 104},
                         {"gpio19", 19},  {"gpio202", 202}, {"gpio203", 203}};
constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];
enum : unsigned { kPinWlanEn = 0, kPinSwCtrl, kPinPerst, kPinClkReq, kPinWake };

// ---- apps SMMU (DT iommu@15000000, qcom,sm8750-smmu-500): pcie@1c00000 iommu-map <0 &apps_smmu 0x1400 1>, <0x100 &apps_smmu 0x1401 1>:
// requester 0 (the root port) is stream 0x1400, requester 0x100 (bus 1, the WCN7860) is stream 0x1401 ----
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kStreamRoot = 0x1400, kStreamEndpoint = 0x1401;
// MSIs: msi-map <0 &its 0x1400 1>, <0x100 &its 0x1401 1> (GIC ITS 0x16040000, which XNU does not drive); the controller's own
// "msi0".."msi7" lines are GIC SPI 141-148, "global" SPI 140 (DT interrupts). Not used by level 1.

// ---- the WCN7860 PMU's supplies and the PHY's (DT), as RPMh VRM resources (cmd-db name) and the PMIC peripheral behind them ----
// The PMIC is pmic-id letter - 'a' = SPMI SID (b 1, d 3, f 5, g 6, i 8; PAGANI's PPIDs agree). SMPS n is peripheral page
// 0x9b + 3 * (n - 1) (SHENG, PAGANI); LDO n is page 0xc0 + n on PAGANI's SM8750 PMICs (from its running DT's OCP notifier names;
// piano's DT has none), 0x40 + n - 1 in SHENG's guess for SM8550: level 1 reads TYPE at both and reports which one is an LDO.
// Registers: 0x04 TYPE (0x04 LDO; 0x03, 0x1c, 0x21, 0x22 SMPS kinds), 0x05 SUBTYPE, 0x08 STATUS (bit 7 ready), 0x40/0x41 the set
// point in mV, 0x45 MODE, 0x46 EN_CTL (bit 7 enabled). Observer reads only (piano-wifi-spmi=1).
// What the chip needs (Project-SunUEFI patches/linux/7.3/0001-platform.patch, its WCN7861 votes): S5F 876-1000 mV, S4D 876-1036 mV,
// S7I 1312-1340 mV, S3G 1860-2000 mV; the stock RPMh minimums are lower (500 / 500 / 1224 mV), so level 2 votes the voltage too.
// order: the DT supply order, which is the order the rails are brought up in (level 2); 0 = never voted by this driver.
// minMv / maxMv: piano's own DT range of that regulator (regulator-min/max-microvolt of rsc@16500000 regulators-*), so the voted
// voltage is a fact of this board, not the 13T's: level 2 asks for minMv (what the chip needs) and never for more than maxMv.
struct Rail {
  const char *supply; const char *regulator; const char *cmdDb; uint8_t sid; uint8_t smps; uint8_t ldo; uint16_t millivolts;
  uint16_t minMv; uint16_t maxMv; uint8_t order; const char *shared;
};
constexpr Rail kRails[] = {   // smps / ldo: the regulator's number in its kind (0: not that kind)
  {"pmu vddio", "vreg_l3f_1p8", "ldof3", 5, 0, 3, 1800, 1800, 1800, 1, "BT vdd18-aon"},
  {"pmu vddio1p2", "vreg_l2f_1p2", "ldof2", 5, 0, 2, 1200, 1200, 1200, 2, "BT vdd12-io"},
  {"pmu vddaon", "vreg_s5f_0p88", "smpf5", 5, 5, 0, 880, 876, 1000, 3, "BT rfa0p75/aon"},
  {"pmu vdddig", "vreg_s4d_0p88", "smpd4", 3, 4, 0, 880, 876, 1036, 4, "BT vdd-dig"},
  {"pmu vddrfa1p2", "vreg_s7i_1p31", "smpi7", 8, 7, 0, 1310, 1312, 1340, 5, "BT rfa1p3 (on the 13T also the UFS vccq parent)"},
  {"pmu vddrfa1p8", "vreg_s3g_1p86", "smpg3", 6, 3, 0, 1860, 1864, 2000, 6, "BT rfa1p8"},
  {"phy vdda-phy", "vreg_l1f_0p88", "ldof1", 5, 0, 1, 880, 880, 920, 0, "always-on; UFS refgen, USB, CSI: never voted"},
  {"phy vdda-pll", "vreg_l3g_1p2", "ldog3", 6, 0, 3, 1200, 1200, 1256, 0, "the UFS PHY's and the DSI PHY's PLL: never voted, never lowered"},
};
constexpr unsigned kRailCount = sizeof kRails / sizeof kRails[0];
constexpr unsigned kVotedRails = 6;   // kRails[0..5]; the two PHY LDOs are read only (NativeUfs runs on one of them)
constexpr uint8_t smpsPage(unsigned n) { return uint8_t(0x9b + 3 * (n - 1)); }
constexpr uint8_t ldoPage(unsigned n) { return uint8_t(0xc0 + n); }
constexpr uint8_t ldoPageAlt(unsigned n) { return uint8_t(0x40 + n - 1); }
// Two PMIC GPIOs: BT_EN of the PMU (DT bt-enable-gpios <&pmic@5 gpio 3>; PM8550VE GPIO n at page 0x88 + n - 1, gpio@8800: SID 5 page
// 0x8a) and the 32 kHz sleep clock pin PAGANI and SHENG drive for the PMU (PMK8550 gpio3 "alt_sleep_clk", SID 0 page 0xba; piano's
// DT names no clock for wcn7860-pmu, mainline's MTP names RPMH_RF_CLK1). Registers 0x04 TYPE, 0x05 SUBTYPE, 0x08 STATUS (bit 0 the
// input level), 0x40 MODE_CTL, 0x41 DIG_VIN_CTL, 0x42 DIG_PULL_CTL, 0x44 DIG_OUT_SOURCE_CTL, 0x45 DIG_OUT_CTL, 0x46 EN_CTL.
struct PmicPin { const char *name; uint8_t sid; uint8_t page; };
constexpr PmicPin kPmicPins[] = {{"BT_EN (pm8550ve-f gpio3)", 5, 0x8a}, {"sleep clock (pmk8550 gpio3)", 0, 0xba}};
constexpr unsigned kPmicPinCount = sizeof kPmicPins / sizeof kPmicPins[0];
// SPMI arbiter v7 (DT spmi@c400000, qcom,ee = 0): the windows of SpmiObserver.h
// AOP command database (DT aop-cmd-db@81c60000, 0x20000 bytes, no-map): read only, copied out word by word (PianoKeyboard's way)
constexpr uint64_t kCmdDb = 0x81c60000, kCmdDbBytes = 0x20000;

// ================================================================================================= level 2: the rails by RPMh
// The application processor's RSC (DT rsc@16500000, qcom,drv-id 2): the HLOS's DRV is DRV-2 at 0x16520000, its TCS block at +0xd00,
// three ACTIVE TCSes first (qcom,tcs-config). The protocol and the register layout are PianoA830's Sm8750Rpmh.h / PianoPower's
// QcomRpmh.c (same repository): a VRM resource takes the set point in mV at +0, the enable at +4 and the mode at +8 (7 = NPM/HPM).
constexpr uint64_t kRscDrv2 = 0x16520000, kRscDrv2Bytes = 0x10000;
constexpr uint32_t kRscTcsBlock = 0xd00;
constexpr uint32_t kVrmVoltage = 0, kVrmEnable = 4, kVrmMode = 8, kVrmModeNpm = 7;
// pwrseq-qcom-wcn's wcn7850/peach entry (behaviour, not code): the rails in DT order, then WLAN_EN high and 50 ms before the chip
// answers. SHENG held WLAN_EN low 150 ms first so that an earlier start has stopped; PAGANI does the same.
constexpr unsigned kWlanEnLowMs = 150, kPmuPowerUpMs = 50;

// ================================================================================================= level 3: the root complex
// PARF, the SM8550 programming model (PCI = mainline pcie-qcom cfg_1_9_0; PAGANI has the same offsets from the 13T's vendor driver)
constexpr uint32_t kParfMhiClockResetCtrl = 0x174, kParfAxiMstrWrAddrHalt = 0x1a8, kParfIntAllClear = 0x228,
                   kParfIntAllMask = 0x22c, kParfDbiBaseHi = 0x354, kParfSlvSpaceHi = 0x35c, kParfAtuBaseHi = 0x638,
                   kParfBdfToSidTable = 0x2000;
constexpr uint32_t kParfDeviceTypeRc = 4, kParfPhyTestPwrDown = 1u << 0, kParfBdfToSidBypass = 1u << 0,
                   kParfSysCtrlMstrAxiClkEnable = 1u << 29, kParfMhiClockResetCtrlBypass = 1u << 4, kParfPmCtrlReqNotEntr = 1u << 5,
                   kParfAxiMstrWrHaltEnable = 1u << 31;
// The BDF-to-SID hash table: 256 words of (BDF << 16 | SID offset << 8 | next), indexed by a CRC-8 (polynomial 0x07, MSB first,
// the requester ID's high byte then its low byte) of the requester ID; used once the bypass bit is clear.
constexpr uint8_t kBdfHashPoly = 0x07;
constexpr uint16_t kRootRid = 0x0000, kEndpointRid = 0x0100;
// DBI: the type 1 header's memory window, and the PCI Express capability's link control 2 (capability + 0x30)
constexpr uint32_t kDbiMemBaseLimit = 0x20, kExpLinkCap = 0x0c, kExpLinkCtlSta = 0x10, kExpLinkCtl2 = 0x30;
constexpr uint32_t kCommandMemory = 1u << 1, kCommandMaster = 1u << 2;
// iATU, unrolled: control 1 (+0, the TLP type), control 2 (+4, bit 31 enable), base (+8/+0xc), limit (+0x10), target (+0x14/+0x18),
// upper limit (+0x20)
constexpr uint32_t kAtuCtrl1 = 0x00, kAtuCtrl2 = 0x04, kAtuBaseLo = 0x08, kAtuBaseHi = 0x0c, kAtuLimit = 0x10, kAtuTargetLo = 0x14,
                   kAtuTargetHi = 0x18, kAtuUpperLimit = 0x20;
constexpr uint32_t kAtuTypeMem = 0, kAtuTypeCfg0 = 4;
// The RCG sources: aux from the TCXO (CFG 0), rchng at 100 MHz (DT assigned-clock-rates 0x5f5e100; source 6 = GPLL0_OUT_EVEN with
// the divider field of 3, as mainline's and PAGANI's tables encode it); the pipe mux takes 0 = the PHY's pipe clock, 2 = TCXO.
constexpr uint32_t kAuxCfgTcxo = 0, kRchngCfg100MHz = (6u << 8) | 5u, kRcgUpdate = 1u << 0;
constexpr uint32_t kPipeMuxPhy = 0, kPipeMuxTcxo = 2, kTcsrClkRefEnable = 1u << 0;
// The GCC branch vote bits level 3 sets (the words are shared: PianoA830 votes GPLL0 in 0x52000, PianoTouch the QUP branches)
constexpr uint32_t kVote8Bits = 0x7f, kVote0Bits = (1u << 11) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 22);
// Link speeds: the PHY is gen3x2, but Gen3 needs cx/mx at NOM (DT opp-table: opp-level 0x100 for the two fastest OPPs), which
// nothing here votes, so the target link speed stays Gen2 unless piano-wifi-gen says otherwise (PAGANI's rule).
constexpr unsigned kMaxGen = 3, kDefaultGen = 2;
constexpr uint32_t kLinkTrainMs = 1000, kEndpointAnswerMs = 1000;

// ================================================================================================= level 4: MHI and firmware
// The endpoint's MHI registers live at the start of BAR0 (sheng, PAGANI; ath12k maps them the same way). BHI and BHIe are at the
// offsets the device reports in BHIOFF / BHIEOFF.
constexpr uint32_t kMhiRegLen = 0x00, kMhiBhiOff = 0x28, kMhiBhieOff = 0x2c, kMhiCtrl = 0x38, kMhiStatus = 0x48;
constexpr uint32_t kMhiCfg = 0x10, kMhiErdbOff = 0x20, kMhiCcabap = 0x58, kMhiEcabap = 0x60, kMhiCrcbap = 0x68,
                   kMhiCtrlBase = 0x80, kMhiCtrlLimit = 0x88, kMhiDataBase = 0x98, kMhiDataLimit = 0xa0;
constexpr uint32_t kMhiCtrlReset = 1u << 1, kMhiStatusReady = 1u << 0, kMhiStatusSysErr = 1u << 2, kMhiStateShift = 8;
constexpr uint32_t kBhiImgAddrLo = 0x08, kBhiImgAddrHi = 0x0c, kBhiImgSize = 0x10, kBhiImgTxDb = 0x18, kBhiIntVec = 0x20,
                   kBhiExecEnv = 0x28, kBhiStatus = 0x2c, kBhiErrCode = 0x30;
constexpr uint32_t kBhieTxVecAddrLo = 0x2c, kBhieTxVecAddrHi = 0x30, kBhieTxVecSize = 0x34, kBhieTxVecDb = 0x3c,
                   kBhieTxVecStatus = 0x44;
constexpr uint32_t kMhiSeqMask = 0x3fffffffu, kMhiXferSuccess = 2, kMhiXferError = 3;
// The chip's register window in BAR0: offsets below 0x80000 are direct, higher ones go through the window register (PAGANI's choice
// for 0x110e, from the 13T's cnss2: 0x3278; WCN7850 and sheng use 0x310c). Level 4 reads only the SoC version through it.
constexpr uint32_t kChipWindowReg = 0x3278, kChipWindowEnable = 0x40000000, kChipWindowShift = 19, kChipWindowMask = 0x3f,
                   kChipWindowStart = 0x80000, kChipWindowRange = 0x7ffff;
constexpr uint32_t kChipTcsrSocHwVersion = 0x01b00000;
constexpr uint32_t kBar0BytesExpected = 0x200000;   // 2 MiB on WCN7850 (sheng); needs-boot for 0x110e
// The firmware file (never in this repository): the stock modem_a FAT, image/peach/amss20.bin = ath12k's amss.bin, 6930496 bytes.
// BHI carries its first 512 KiB (the secondary boot loader); BHIe carries the second ELF that follows when the bytes at 512 KiB are
// "\177ELF" (peach), else the whole file (WCN7850). Segments of 512 KiB, as the 13T's cnss2 cuts them.
constexpr const char *const kFirmwarePartitions[] = {"modem_a", "modem_b"};
constexpr const char *const kFirmwareDirs[] = {"image/peach", "image"};
constexpr const char *kAmssName = "amss20.bin";
constexpr uint32_t kMhiSblBytes = 512 * 1024, kMhiSegBytes = 512 * 1024, kMhiMaxSegments = 32;
constexpr uint32_t kDmaWindowBytes = 24u << 20;   // the boot loader, the segments, the vector table and the MHI contexts

}  // namespace pianowifi
