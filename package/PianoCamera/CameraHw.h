// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera: piano's camera facts (SM8750 CAMSS, the camera clock controllers, CCI0, the two sensors' rails, pins and buses), each
// with its source. Facts only; no code, comment or table of a source is copied. Sources:
//   DT    = out/inputs/piano-boot.dtb (the running Ubuntu DT, dtc -I dtb -O dts): clock-controller@ade0000 (camcc: power-domains
//           MMCX + MXC), clock-controller@1760000 (cambistmclkcc: MMCX + MX), cci@ac7b000, isp@ad27000 (camss), camera@10 (both),
//           the camera rails (apps_rsc regulators), pinctrl@f100000 (gpio-reserved-ranges 36-39, 48-51, 74: none of ours)
//   KDTS  = the Ubuntu piano kernel's sm8750-xiaomi-piano-camera.dtsi (yzddmr6/linux-sm8750-piano piano/dev 7f8d6bed, GPL-2.0): the
//           sensors' supplies, pins and their default/sleep states, the power-on order is the drivers'
//   CAMCC = drivers/clk/qcom/camcc-sm8750.c (branches, RCGs, GDSCs, the critical branches, PLL types)
//   MCLKCC= drivers/clk/qcom/cambistmclkcc-sm8750.c (PLL0 Rivian ELU and its 960 MHz configuration, MCLK0..7, their table)
//   GCC   = drivers/clk/qcom/gcc-sm8750.c (the camera branches, the ones Linux keeps on at probe; QUP2 SE1's)
//   PLL   = drivers/clk/qcom/clk-alpha-pll.c (Taycan ELU and Rivian ELU register layouts)
//   GDSC  = drivers/clk/qcom/gdsc.c (GDSCR/CFG bits, the wait values, the on/off order and bounds)
//   PINS  = drivers/pinctrl/qcom/pinctrl-sm8750.c + pinctrl-msm.c (functions of our pins, the egpio ownership bit)
//   CCI   = drivers/i2c/busses/i2c-qcom-cci.c (registers, commands, the reset/queue protocol, the timing sets)
//   S5K   = drivers/media/i2c/s5kjn1.c; OV = drivers/media/i2c/ov32d40.c (chip IDs, power sequences, controls)
//   CAMSS = drivers/media/platform/qcom/camss/camss-csid.c, camss-vfe.c (HW_VERSION at +0 of CSID and VFE/TFE)
// docs/drivers/camera.md section 2 has the same facts with their state (verified / inferred / needs-boot).
#pragma once
#include <stdint.h>

namespace pianocamera {

// ------------------------------------------------------------------------------------------------------------ the blocks
constexpr uint64_t kGcc = 0x00100000;
constexpr uint64_t kCamCc = 0x0ade0000, kCamCcBytes = 0x20000;      // DT clock-controller@ade0000
constexpr uint64_t kMclkCc = 0x01760000, kMclkCcBytes = 0x6000;     // DT clock-controller@1760000 (cambistmclkcc)
constexpr uint64_t kTlmm = 0x0f100000;                              // DT pinctrl@f100000
constexpr uint64_t kCci0 = 0x0ac7b000, kCci0Bytes = 0x1000;         // DT cci@ac7b000 (SPI 426 edge; polled here)
constexpr uint64_t kQup2Se1 = 0x00884000;                           // DT i2c@884000 ("i2c9", the front sensor's bus in Ubuntu)
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;       // apps SMMU (MMU-500), read only here
constexpr uint16_t kCamStream = 0x1c00;                             // DT camss iommus = <&apps_smmu 0x1c00 0x0>
constexpr uint16_t kCamStreamScanLast = 0x1c1f;                     // the survey lists every group covering 0x1c00..0x1c1f
constexpr uint64_t kCmdDb = 0x81c60000, kCmdDbBytes = 0x20000;      // as PianoWifi/PianoBluetooth (reserved memory, read only)
constexpr uint64_t kRscDrv2 = 0x16520000, kRscDrv2Bytes = 0x10000;  // apps RSC DRV-2 (the TCS block at +0xd00)
constexpr uint32_t kRscTcsBlock = 0xd00;

// The CAMSS register blocks (DT camss reg, reg-names). HW_VERSION at +0 for CSID and TFE (CAMSS: generation 31:28, revision 27:16,
// step 15:0). A block is read only when TITAN_TOP is on, the camera AHB branch runs, and (when not 0) its GDSC is on and its own
// branch runs: an unclocked CAMSS register is not read (it can stall the bus). CSIPHY's version needs a write (CTRL6 SHOW_REV_ID)
// first, so the read-only survey does not read it.
enum class BlockKind : uint8_t { csid, csidLite, tfe, tfeLite };
struct Block {
  const char *name;
  uint64_t pa;
  BlockKind kind;
  uint32_t gdsc;     // its camcc GDSCR (0: under TITAN_TOP only)
  uint32_t branch;   // the camcc branch that clocks its registers
};
constexpr Block kBlocks[] = {
  {"csid0", 0x0ad27000, BlockKind::csid, 0, 0x112d8},          {"csid1", 0x0ad2a000, BlockKind::csid, 0, 0x112d8},
  {"csid2", 0x0ad2d000, BlockKind::csid, 0, 0x112d8},          {"csid-lite0", 0x0ad6d000, BlockKind::csidLite, 0, 0x11194},
  {"csid-lite1", 0x0ad72000, BlockKind::csidLite, 0, 0x11194}, {"tfe0", 0x0ac86000, BlockKind::tfe, 0x11004, 0x11030},
  {"tfe1", 0x0ac96000, BlockKind::tfe, 0x11084, 0x110b0},      {"tfe2", 0x0aca6000, BlockKind::tfe, 0x110ec, 0x11118},
  {"tfe-lite0", 0x0ad6e000, BlockKind::tfeLite, 0, 0x11168},   {"tfe-lite1", 0x0ad73000, BlockKind::tfeLite, 0, 0x11168},
};
constexpr unsigned kBlockCount = sizeof kBlocks / sizeof kBlocks[0];
// CSIPHY n at 0x0ada9000 + 0x2000 n (DT csiphy0..5); the rear's is 1, the front's 4 (KDTS ports 1 and 4). Not read before level 3.
constexpr uint64_t csiphyBase(unsigned n) { return 0x0ada9000ull + 0x2000ull * n; }

// --------------------------------------------------------------------------------------------------------------- GCC
// GCC: the camera branches. 0x26004 camera AHB, 0x26034 camera XO and 0xa0004 the MCLK controller's AHB are among the branches Linux
// turns on at probe and never off ("critical"); the HF/SF AXI branches (0x26014/0x26024, halt-skip, hardware gating) carry the write
// masters' traffic (level 3); the QMIP AHB branches (0x26008/0x2600c/0x26010) are voted, read only. CBCR: bit 31 CLK_OFF, bit 0
// CLK_ENABLE, bit 1 HW_CTL.
constexpr uint32_t kGccCameraAhb = 0x26004, kGccCameraXo = 0x26034, kGccMclkAhb = 0xa0004;
constexpr uint32_t kGccCameraHfAxi = 0x26014, kGccCameraSfAxi = 0x26024;
constexpr uint32_t kGccQmipNrt = 0x26008, kGccQmipRt = 0x2600c, kGccQmipCmd = 0x26010;
// QUP2 (the front sensor's GENI I2C in Ubuntu, SE1): the shared vote word (bit 0 core, 1 s-ahb, 2 m-ahb, 3 core2x, 5 s1) and the
// branches it votes; the SE1 clock source. Read only: this driver reaches the front sensor with its own software I2C (BitI2c.h).
constexpr uint32_t kGccQup2Vote = 0x52010, kVoteS1 = 1u << 5, kVoteQup2Base = 0xfu;
constexpr uint32_t kGccQup2MAhb = 0x23298, kGccQup2SAhb = 0x2329c, kGccQup2Core = 0x232a0, kGccQup2Core2x = 0x232b4,
                   kGccQup2S1 = 0x1e140, kGccQup2S1Rcg = 0x1e150;
constexpr uint32_t kCbcrClkOff = 1u << 31, kCbcrEnable = 1u << 0, kCbcrHwCtl = 1u << 1;
constexpr uint32_t kGccWindowBytes = 0xa1000;   // the GCC pages this driver loads (0x23000..0x26fff, 0x1e000, 0x52000, 0xa0000)

// -------------------------------------------------------------------------------------------------------------- CAMCC
// GDSCs (CAMCC): TITAN_TOP is the parent of every other one (POLL_CFG_GDSCR, RETAIN_FF_ENABLE); TFE0/1/2 its children.
constexpr uint32_t kGdscTitanTop = 0x1134c, kGdscTfe0 = 0x11004, kGdscTfe1 = 0x11084, kGdscTfe2 = 0x110ec, kGdscIpe0 = 0x1017c,
                   kGdscOfe = 0x100c8;
// GDSCR (GDSC): bit 31 PWR_ON, 23:20 EN_REST_WAIT, 19:16 EN_FEW_WAIT, 15:12 CLK_DIS_WAIT, bit 11 RETAIN_FF_ENABLE, bit 2 SW_OVERRIDE,
// bit 1 HW_CONTROL, bit 0 SW_COLLAPSE; CFG at +4: bit 16 POWER_UP_COMPLETE, bit 15 POWER_DOWN_COMPLETE. Linux's set-up writes the
// wait values 2/8/2 with HW_CONTROL and SW_OVERRIDE clear; on: SW_COLLAPSE clear, CFG POWER_UP_COMPLETE within 2 ms, 1 us, then
// RETAIN_FF_ENABLE; off: SW_COLLAPSE set, CFG POWER_DOWN_COMPLETE within 2 ms.
constexpr uint32_t kGdscPwrOn = 1u << 31, kGdscRetainFf = 1u << 11, kGdscSwOverride = 1u << 2, kGdscHwControl = 1u << 1,
                   kGdscCollapse = 1u << 0;
constexpr uint32_t kGdscWaitMask = 0xfffu << 12, kGdscWaitValues = 2u << 20 | 8u << 16 | 2u << 12;
constexpr uint32_t kGdscCfg = 4, kGdscCfgUpDone = 1u << 16, kGdscCfgDownDone = 1u << 15;
constexpr unsigned kGdscPollUs = 2000;
// The branches CAMCC keeps on at probe (critical): DRV AHB, DRV XO, the GDSC clock, the sleep clock.
constexpr uint32_t kCcDrvAhb = 0x113c4, kCcDrvXo = 0x113c0, kCcGdscClk = 0x1137c, kCcSleepClk = 0x11398;
// Branches (CBCR offsets; all BRANCH_HALT: CLK_OFF follows the enable within Linux's bound).
constexpr uint32_t kCcCamTopAhb = 0x113ac, kCcCamTopFastAhb = 0x1139c, kCcCoreAhb = 0x11360, kCcCci0 = 0x11284, kCcCci1 = 0x112a0,
                   kCcCsid = 0x112d8, kCcCsidCsiphyRx = 0x10020, kCcCsiphy1 = 0x10040, kCcCsi1Timer = 0x1003c,
                   kCcCsiphy4 = 0x100a0, kCcCsi4Timer = 0x1009c, kCcCamnocRtAxi = 0x11300, kCcIfeLite = 0x11168,
                   kCcIfeLiteAhb = 0x111a8, kCcIfeLiteCsid = 0x11194, kCcIfeLiteCphyRx = 0x111a4, kCcTfe0Main = 0x11030;
constexpr unsigned kBranchPollUs = 500;
// RCGs (CMD at +0: bit 31 ROOT_OFF, bit 1 ROOT_EN, bit 0 UPDATE; CFG at +4: 4:0 half divider (2 d - 1; 0 and 1 divide by 1), 10:8
// source, 13:12 M/N mode; M +8, N +0xc, D +0x10). Source 0 is the XO (19.2 MHz) in every camcc parent map and in cambistmclkcc's;
// the reset value of CFG is 0: XO, undivided.
constexpr uint32_t kCcSlowAhbRcg = 0x10100, kCcFastAhbRcg = 0x100dc, kCcCci0Rcg = 0x1126c, kCcCphyRxRcg = 0x11068, kCcCsidRcg = 0x112c0,
                   kCcCsi1TimerRcg = 0x10024, kCcCsi4TimerRcg = 0x10084, kCcCamnocRtAxiRcg = 0x112e8, kCcIfeLiteRcg = 0x11150,
                   kCcIfeLiteCsidRcg = 0x1117c, kCcXoRcg = 0x11364;
constexpr uint32_t kRcgRootOff = 1u << 31, kRcgRootEn = 1u << 1, kRcgUpdate = 1u << 0;
constexpr uint32_t kRcgCfg = 4, kRcgM = 8, kRcgN = 0xc, kRcgD = 0x10;
constexpr uint32_t kRcgCfgMask = 0x1fu | 7u << 8 | 3u << 12, kRcgCfgXo = 0;
constexpr unsigned kRcgUpdatePolls = 500;   // 1 us each
inline uint32_t rcgSource(uint32_t cfg) { return (cfg >> 8) & 7u; }
inline uint32_t rcgHalfDiv(uint32_t cfg) { return cfg & 0x1fu; }
inline uint32_t rcgMode(uint32_t cfg) { return (cfg >> 12) & 3u; }
// CAMCC PLL0..6 (Taycan ELU) at 0x1000 n: MODE +0 (bit 31 LOCK_DET, 2 RESET_N, 1 BYPASSNL, 0 OUTCTRL), OPMODE +4 (1 run, 0
// standby), STATE +8, STATUS +0xc, L_VAL +0x10, ALPHA +0x14, USER_CTL +0x18. The survey reads MODE, OPMODE and L_VAL only.
constexpr unsigned kCcPllCount = 7;
constexpr uint32_t ccPll(unsigned n) { return 0x1000u * n; }
constexpr uint32_t kPllMode = 0, kPllOpmode = 4, kPllTaycanL = 0x10, kPllLockDet = 1u << 31, kPllOutctrl = 1u << 0;
// What the survey dumps of CAMCC (name, offset), in its log order.
struct Reg {
  const char *name;
  uint32_t off;
};
constexpr Reg kCcBranches[] = {
  {"drv-ahb", kCcDrvAhb},     {"drv-xo", kCcDrvXo},          {"gdsc-clk", kCcGdscClk},       {"sleep", kCcSleepClk},
  {"top-ahb", kCcCamTopAhb},  {"top-fast-ahb", kCcCamTopFastAhb}, {"core-ahb", kCcCoreAhb},  {"cci0", kCcCci0},
  {"cci1", kCcCci1},          {"csid", kCcCsid},             {"csid-rx", kCcCsidCsiphyRx},   {"csiphy1", kCcCsiphy1},
  {"csi1-timer", kCcCsi1Timer}, {"csiphy4", kCcCsiphy4},     {"csi4-timer", kCcCsi4Timer},   {"camnoc-rt-axi", kCcCamnocRtAxi},
  {"ife-lite", kCcIfeLite},   {"ife-lite-ahb", kCcIfeLiteAhb}, {"ife-lite-csid", kCcIfeLiteCsid}, {"ife-lite-rx", kCcIfeLiteCphyRx},
  {"tfe0", kCcTfe0Main},
};
constexpr Reg kCcRcgs[] = {
  {"slow-ahb", kCcSlowAhbRcg}, {"fast-ahb", kCcFastAhbRcg}, {"cci0", kCcCci0Rcg},        {"cphy-rx", kCcCphyRxRcg},
  {"csid", kCcCsidRcg},        {"csi1-timer", kCcCsi1TimerRcg}, {"csi4-timer", kCcCsi4TimerRcg}, {"camnoc-rt-axi", kCcCamnocRtAxiRcg},
  {"ife-lite", kCcIfeLiteRcg}, {"ife-lite-csid", kCcIfeLiteCsidRcg}, {"xo", kCcXoRcg},
};
constexpr Reg kCcGdscs[] = {{"titan-top", kGdscTitanTop}, {"tfe0", kGdscTfe0}, {"tfe1", kGdscTfe1}, {"tfe2", kGdscTfe2},
                            {"ipe0", kGdscIpe0},          {"ofe", kGdscOfe}};
constexpr unsigned kCcBranchCount = sizeof kCcBranches / sizeof kCcBranches[0], kCcRcgCount = sizeof kCcRcgs / sizeof kCcRcgs[0],
                   kCcGdscCount = sizeof kCcGdscs / sizeof kCcGdscs[0];

// ----------------------------------------------------------------------------------------------------- the MCLK controller
// cambistmclkcc (MCLKCC): PLL0 Rivian ELU at +0 (MODE +0 as above, OPMODE +4, STATUS +0xc, L_VAL +0x10, USER_CTL +0x14, CONFIG_CTL
// +0x1c, _U +0x20, _U1 +0x24, _U2 +0x28, TEST_CTL +0x2c, _U +0x30); Linux's configuration 19.2 MHz x 0x32 = 960 MHz (VCO 833..1125
// MHz; CONFIG_CTL 0x12000000, _U 0x00890263, _U1 0x1af04237, _U2 0). MCLK n: RCG at 0x4000 + 0x1c n (8-bit M/N, 5-bit half
// divider; parent map: 0 XO, 3 PLL0 "even", 5 PLL0 "main", both the 960 MHz output), branch at 0x4018 + 0x1c n; the sleep clock
// branch 0x40f8 is the one Linux keeps on. Its table: 12 MHz (PLL0 / 10 x 1/8), 19.2 MHz (XO / 1), 24 MHz (PLL0 / 10 x 1/4),
// 68.571 MHz (PLL0 / 14). KDTS: the rear sensor's MCLK1 at 24 MHz, the front's MCLK4 at 19.2 MHz.
constexpr uint32_t kMclkPll = 0x0, kPllRivianL = 0x10, kPllRivianUserCtl = 0x14;
constexpr uint32_t mclkRcg(unsigned n) { return 0x4000u + 0x1cu * n; }
constexpr uint32_t mclkBranch(unsigned n) { return 0x4018u + 0x1cu * n; }
constexpr uint32_t kMclkSleep = 0x40f8;
constexpr unsigned kMclkCount = 8;
constexpr uint64_t kXoHz = 19200000;

// ------------------------------------------------------------------------------------------------------------------ TLMM
// Pin n: CFG at 0x1000 n (1:0 pull: 0 none, 1 down, 2 keeper, 3 up; 5:2 function; 8:6 drive (v + 1) x 2 mA; bit 9 output enable;
// bit 11 EGPIO_PRESENT; bit 12 EGPIO_ENABLE: set, the application processor's TLMM owns the pin (PINS: Linux sets it whenever it
// selects a function of a pin that has the bit present); bit 13 I2C pull), IO at +4 (bit 0 input level, bit 1 output value).
constexpr uint32_t kTlmmStride = 0x1000;
constexpr uint32_t kTlmmPullMask = 3u, kTlmmFuncShift = 2, kTlmmFuncMask = 0xfu << 2, kTlmmDrvShift = 6, kTlmmDrvMask = 7u << 6,
                   kTlmmOe = 1u << 9, kTlmmEgpioPresent = 1u << 11, kTlmmEgpioEnable = 1u << 12;
constexpr uint32_t kTlmmIn = 1u << 0, kTlmmOut = 1u << 1;
constexpr uint64_t tlmmCfg(unsigned gpio) { return kTlmm + uint64_t(gpio) * kTlmmStride; }
constexpr uint64_t tlmmIo(unsigned gpio) { return tlmmCfg(gpio) + 4; }
enum class PinRole : uint8_t { i2cSda, i2cScl, mclk, reset, supplyEnable };
struct Pin {
  const char *name;
  unsigned gpio;
  PinRole role;
  uint8_t function;   // its function when in use (PINS: index 1 of each list; 0 = GPIO)
  uint8_t driveMa;    // KDTS default state
};
// KDTS: cam-cci0-1 (gpio115 cci_i2c_sda, gpio116 cci_i2c_scl, 4 mA, no bias), cam-wide (gpio90 cam_mclk 4 mA; gpio6 reset 2 mA),
// cam-front (gpio93 cam_aon_mclk4 2 mA; gpio7 reset 2 mA), cam-wide/front-avdd (gpio175/176, 2 mA), cam-i2c9 (gpio4/5 qup2_se1,
// 2 mA). All "bias-disable" when active (external pull-ups on the buses); sleep states pull down and drive reset/enable low.
constexpr Pin kPins[] = {
  {"cci-sda", 115, PinRole::i2cSda, 1, 4},     {"cci-scl", 116, PinRole::i2cScl, 1, 4},     {"rear-mclk", 90, PinRole::mclk, 1, 4},
  {"rear-reset", 6, PinRole::reset, 0, 2},     {"rear-avdd-en", 175, PinRole::supplyEnable, 0, 2},
  {"front-sda", 4, PinRole::i2cSda, 1, 2},     {"front-scl", 5, PinRole::i2cScl, 1, 2},     {"front-mclk", 93, PinRole::mclk, 1, 2},
  {"front-reset", 7, PinRole::reset, 0, 2},    {"front-avdd-en", 176, PinRole::supplyEnable, 0, 2},
};
constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];
enum : unsigned { kPinCciSda = 0, kPinCciScl, kPinRearMclk, kPinRearReset, kPinRearAvdd, kPinFrontSda, kPinFrontScl, kPinFrontMclk,
                  kPinFrontReset, kPinFrontAvdd };
constexpr uint32_t drvField(unsigned ma) { return uint32_t(ma / 2 - 1) << kTlmmDrvShift; }
constexpr unsigned drvMa(uint32_t cfg) { return (((cfg & kTlmmDrvMask) >> kTlmmDrvShift) + 1) * 2; }
constexpr unsigned pinFunction(uint32_t cfg) { return (cfg & kTlmmFuncMask) >> kTlmmFuncShift; }

// ------------------------------------------------------------------------------------------------------------------ rails
// DT: every one of these LDOs supplies only the cameras (VCM vcm@c, disabled, shares L6B/L7B with the rear sensor). RPMh VRM
// resources (cmd-db names): +0 the set point in mV, +4 enable, +8 mode (7: high power; the DT's initial mode is HPM). The PMIC side
// over the SPMI observer: LDO n at page 0xc0 + n, else 0x40 + n - 1 (PianoWcnRails.h's rule, decided by TYPE 0x04); STATUS1 0x08
// (bit 7 ready), VSET 0x40/0x41 (mV), MODE 0x45, EN_CTL 0x46 (bit 7). PMIC letters are SPMI SIDs: b 1, f 5, g 6, i 8.
// The CSIPHY rails L1F (0.88 V) and L3I (0.88 V) are always-on in the DT; L3G (1.2 V) is also the UFS and DSI PHYs' PLL: read only,
// never voted (as PianoWifi treats it).
struct Rail {
  const char *supply, *regulator, *cmdDb;
  uint8_t sid, ldo;
  uint16_t mv;
  bool voted;   // false: read only
};
constexpr Rail kRails[] = {
  {"rear vddd", "vreg_l1i_1p1", "ldoi1", 8, 1, 1096, true},    // KDTS: "Stock L1I VSET readback is 1096 mV"
  {"rear vddio", "vreg_l6b_1p8", "ldob6", 1, 6, 1800, true},   // also the (disabled) VCM's vin
  {"rear afvdd", "vreg_l7b_2p8", "ldob7", 1, 7, 2800, true},   // the VCM's vdd; the sensor driver powers it with the sensor
  {"front vddio", "vreg_l10b_1p8", "ldob10", 1, 10, 1800, true},
  {"front dvdd", "vreg_l2g_1p2", "ldog2", 6, 2, 1200, true},
  {"csiphy 1p2", "vreg_l3g_1p2", "ldog3", 6, 3, 1200, false},  // read only: also the UFS/DSI PHY PLL
  {"csiphy0/1/3/5 0p9", "vreg_l1f_0p88", "ldof1", 5, 1, 880, false},
  {"csiphy2/4 0p9", "vreg_l3i_0p88", "ldoi3", 8, 3, 880, false},
};
constexpr unsigned kRailCount = sizeof kRails / sizeof kRails[0];
enum : unsigned { kRailRearVddd = 0, kRailRearVddio, kRailRearAfvdd, kRailFrontVddio, kRailFrontDvdd, kRailCsiphy1p2, kRailL1f, kRailL3i };
constexpr uint8_t ldoPage(unsigned n) { return uint8_t(0xc0 + n); }
constexpr uint8_t ldoPageAlt(unsigned n) { return uint8_t(0x40 + n - 1); }
constexpr uint8_t kPmicType = 0x04, kPmicSubtype = 0x05, kPmicStatus1 = 0x08, kPmicVset = 0x40, kPmicMode = 0x45, kPmicEnCtl = 0x46;
constexpr uint8_t kPmicTypeLdo = 0x04, kPmicReady = 0x80, kPmicEnabled = 0x80;
constexpr uint32_t kVrmVoltage = 0, kVrmEnable = 4, kVrmMode = 8, kVrmModeHpm = 7;
// The fixed 2.8 V analogue supplies (KDTS regulator-fixed, enable-active-high, startup-delay-us 100): rear vdda gpio175, front avdd
// gpio176.
constexpr unsigned kFixedStartupUs = 100;

// ------------------------------------------------------------------------------------------------------------------ CCI
// CCI0 (CCI; DT compatible qcom,sm8750-cci + qcom,msm8996-cci: two masters, queues of 64 and 16 words). The rear sensor is on master
// 1 (KDTS i2c-bus@1, 1 MHz Fast-mode Plus in Linux, with the CCI clock at 37.5 MHz from CAMCC PLL0). Level 2 runs the CCI clock from
// the XO (19.2 MHz: the RCG's reset source, no PLL) and so takes the timing set Linux has for a 19.2 MHz CCI clock (the msm8916/
// msm8974 parts' "fast" set: 400 kHz) or its "standard" set (100 kHz).
constexpr uint32_t kCciHwVersion = 0x000, kCciResetCmd = 0x004, kCciQueueStart = 0x008, kCciHaltReq = 0x034;
constexpr uint32_t kCciResetAll = 0x0f73f3f7, kCciResetM1 = 0x0003f001;
constexpr uint32_t cciSclCtl(unsigned m) { return 0x100u + 0x100u * m; }
constexpr uint32_t cciSdaCtl0(unsigned m) { return 0x104u + 0x100u * m; }
constexpr uint32_t cciSdaCtl1(unsigned m) { return 0x108u + 0x100u * m; }
constexpr uint32_t cciSdaCtl2(unsigned m) { return 0x10cu + 0x100u * m; }
constexpr uint32_t cciMiscCtl(unsigned m) { return 0x110u + 0x100u * m; }
constexpr uint32_t cciReadData(unsigned m) { return 0x118u + 0x100u * m; }
constexpr uint32_t cciReadLevel(unsigned m) { return 0x11cu + 0x100u * m; }
constexpr uint32_t cciExecCount(unsigned m, unsigned q) { return 0x300u + 0x200u * m + 0x100u * q; }
constexpr uint32_t cciCurCount(unsigned m, unsigned q) { return 0x304u + 0x200u * m + 0x100u * q; }
constexpr uint32_t cciCurCmd(unsigned m, unsigned q) { return 0x308u + 0x200u * m + 0x100u * q; }
constexpr uint32_t cciLoad(unsigned m, unsigned q) { return 0x310u + 0x200u * m + 0x100u * q; }
constexpr uint32_t kCciIrqGlobalClear = 0xc00, kCciIrqMask = 0xc04, kCciIrqClear = 0xc08, kCciIrqStatus = 0xc0c;
// IRQ bits: master m at 12 m: RD_DONE +0, Q0_REPORT +4, Q1_REPORT +8; RST_DONE 24, HALT_ACK 25 + m, Q0/Q1 NACK 27 + 2 m / 28 + 2 m;
// error masks 0x18000ee6 (m0) and 0x60ee6000 (m1).
constexpr uint32_t cciRdDone(unsigned m) { return 1u << (12 * m); }
constexpr uint32_t cciQReport(unsigned m, unsigned q) { return 1u << (12 * m + 4 + 4 * q); }
constexpr uint32_t cciHaltAck(unsigned m) { return 1u << (25 + m); }
constexpr uint32_t cciNack(unsigned m, unsigned q) { return 1u << (27 + 2 * m + q); }
constexpr uint32_t cciErrors(unsigned m) { return m ? 0x60ee6000u : 0x18000ee6u; }
constexpr uint32_t kCciRstDone = 1u << 24;
// Queue commands (the low nibble of a word): SET_PARAM 1 (7-bit address << 4), REPORT 8 (bit 8: raise the queue's report bit),
// WRITE 9 (byte count << 4, then the bytes packed little-endian after the command byte), READ 10 (byte count << 4). A read's data
// words hold the address byte first, then the bytes read: (count / 4 + 1) words.
constexpr uint32_t kCciSetParam = 1, kCciReport = 8, kCciWrite = 9, kCciRead = 10, kCciReportIrq = 1u << 8;
constexpr unsigned kCciQueueWords[2] = {64, 16}, kCciMaxWrite = 11, kCciMaxRead = 12, kCciTimeoutMs = 100;
struct CciTiming {
  uint16_t thigh, tlow, tsuSto, tsuSta, thdDat, thdSta, tbuf;
  uint8_t stretch;
  uint16_t trdhld, tsp;
};
constexpr CciTiming kCciXo100k = {78, 114, 28, 28, 10, 77, 118, 0, 6, 1};   // 19.2 MHz CCI clock, 100 kHz
constexpr CciTiming kCciXo400k = {20, 28, 21, 21, 13, 18, 32, 0, 6, 3};     // 19.2 MHz CCI clock, 400 kHz
constexpr unsigned kCciMaster = 1;

// ------------------------------------------------------------------------------------------------------------------ sensors
// S5K: chip ID 0x38e1 at 0x0000 (16 bits); MCLK 24 MHz only (the driver refuses another rate: its PLL words assume it); 4 lanes at
// 700 MHz; reset active low. Power on: vddd, 1-2 ms, vdda, vddio, afvdd, MCLK, reset released, 10-15 ms; off: reset, MCLK, afvdd,
// vddio, vdda, 1-2 ms, vddd. Controls: exposure 0x0202 (16), analogue gain 0x0204 (16; 32 = 1x, 32..2048), frame length 0x0340
// (16), line length 0x0342, orientation 0x0101 (bit 1 vflip, bit 0 hflip; Ubuntu's daemon sets vflip for the rear's mounting:
// BGGR then), test pattern 0x0600 (16: 0 off, 1 solid, 2 bars, 3 fade bars, 4 PN9), streaming 0x0100 bit 0.
// OV: chip ID 0x5632 at 0x300a (16 bits), up to five reads 10-12 ms apart; MCLK 19.2 MHz; 4 lanes at 842.4 MHz; reset active low.
// Power on: reset asserted, vddio + dvdd + avdd, 5-6 ms, MCLK, 10-12 ms, reset released, 20-25 ms; off: reset, MCLK, supplies.
// Controls: exposure 0x3500 (24), gain 0x3508 (16; 256 = 1x... min 288, max 15872), frame length 0x380e (16), test pattern 0x4308
// (8; "most likely not" the real one, the driver says), streaming 0x0100 (in the on/off tables, data/camera/ov32d40.tbl).
// Both sensors answer at 7-bit address 0x10 on their own buses: the rear on CCI0 master 1 (SDA gpio115, SCL gpio116), the front on
// QUP2 SE1's pins (SDA gpio4, SCL gpio5; Ubuntu ran it as GENI I2C at 1 MHz).
enum class Sensor : uint8_t { rear, front };
struct SensorFacts {
  const char *name, *part;
  uint8_t address;
  uint16_t idReg, idValue;
  unsigned idTries, idRetryMs;
  unsigned mclk;                 // MCLK index in cambistmclkcc
  uint32_t mclkHz;               // what the sensor's driver programs
  unsigned csiphy, lanes;
  uint32_t linkHz;
};
constexpr SensorFacts kSensors[2] = {
  {"rear", "S5KJN1", 0x10, 0x0000, 0x38e1, 3, 10, 1, 24000000, 1, 4, 700000000},
  {"front", "OV32D40", 0x10, 0x300a, 0x5632, 5, 12, 4, 19200000, 4, 4, 842400000},
};
// The power sequences' waits (the drivers' usleep_range upper ends).
constexpr unsigned kRearVdddToVddaUs = 2000, kRearResetToIdUs = 15000, kRearOffVdddUs = 2000;
constexpr unsigned kFrontSupplyToMclkUs = 6000, kFrontMclkToResetUs = 12000, kFrontResetToIdUs = 25000;

}  // namespace pianocamera
