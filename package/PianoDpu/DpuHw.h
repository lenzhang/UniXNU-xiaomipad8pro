// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu: the hardware facts of piano's display path (SM8750 MDSS / DPU 12.0, two DSI 6G controllers driving one NT36532 panel),
// each with its source. Every number here is a fact read from a source, never code: the piano boot DT (out/inputs/piano-boot.dtb,
// "DT"), the Linux msm DPU/DSI driver and its SM8750 catalog in the Ubuntu piano kernel (GPL-2.0, read as the specification only:
// "msm"), the piano panel driver (GPL-2.0, specification only: "panel"), Project-SunUEFI's piano notes ("SunUEFI"), and the stock
// panel XML in the piano UEFI trees ("XML"). docs/drivers/dpu.md has the same facts with their state (verified / inferred /
// needs-boot). Offsets are from the block's base; the absolute address is base + offset.
#pragma once
#include <stdint.h>

namespace pianodpu {

// ----------------------------------------------------------------------------------------------------------------- blocks
constexpr uint64_t kMdss = 0x0ae00000;        // DT display-subsystem@ae00000 (len 0x1000); SPI 83 level; its own interrupt
                                              // controller: DPU hwirq 0, DSI0 4, DSI1 5, DP 12 (DT interrupts-extended)
constexpr uint64_t kMdp = 0x0ae01000;         // DT display-controller@ae01000 "mdp" (len 0x93000); DPU core 12.0 (msm catalog)
constexpr uint32_t kMdpBytes = 0x93000;
constexpr uint64_t kVbif = 0x0aeb0000;        // DT "vbif" (len 0x3000): not read
constexpr uint64_t kRegDma = 0x0af80000;      // DT "regdma" (len 0x7000): the LUT DMA engine (GC/IGC programming); not read
constexpr uint64_t kDsi[2] = {0x0ae94000, 0x0ae96000};   // DT dsi@ae94000 (master, qcom,master-dsi) and dsi@ae96000; len 0x400
constexpr uint32_t kDsiBytes = 0x400;
constexpr uint64_t kDsiPhy[2] = {0x0ae95000, 0x0ae97000}; // DT phy@ae95000/ae97000 (qcom,sm8750-dsi-phy-3nm): not read
constexpr uint64_t kDispCc = 0x0af00000;      // DT clock-controller@af00000 (qcom,sm8750-dispcc, len 0x20000)
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;   // apps SMMU (qcom,sm8750-smmu-500), 1 MiB
constexpr uint64_t kTlmm = 0x0f100000;        // main TLMM, pin n at + n * 0x1000 (CFG +0, IO +4)
constexpr uint32_t kTlmmStride = 0x1000;
constexpr unsigned kPanelIdGpio = 100;        // DT panel id-gpios: low = p81 42-02-0a, high = p81 35-02-0b (panel nt36532_variants)

// The GOP / splash framebuffer the boot loader left scanning out (DT splash_region@fc800000 no-map, chosen/framebuffer
// x8r8g8b8 3200x2136 stride 0x3200; NativeGopDisplay's PE_state.video).
constexpr uint64_t kSplash = 0xfc800000, kSplashBytes = 0x2b00000;
constexpr uint32_t kPanelWidth = 3200, kPanelHeight = 2136, kSplashStride = 12800;
// XNU's whole DRAM window (dts/piano.dts memory@8c0000000, 13 GiB): every IOSurface lives above 4 GiB, so a 32-bit DPU fetch
// address (below) can reach a surface only through an SMMU translation.
constexpr uint64_t kXnuDramBase = 0x8c0000000ull, kXnuDramEnd = 0xc00000000ull;

// The DPU's SMMU streams: DT iommus = <&apps_smmu 0x800 0x2> (so 0x800 and 0x802); the stock overlay adds 0x801 (secure bank).
constexpr uint16_t kStreamDpu = 0x800, kStreamDpuMask = 0x2, kStreamDpuSecure = 0x801;

// ------------------------------------------------------------------------------------------------------------ DISPCC gate
// Read before any MDSS/DPU/DSI load: a load from an unpowered or unclocked block can stall the bus. GDSC bit 31 = power on;
// branch CBCR bit 31 = CLK_OFF (msm dispcc-sm8750: mdss_gdsc 0x9000, mdss_ahb 0x80b0, mdss_mdp 0x8010, mdss_vsync 0x8030,
// pclk0/1 0x8004/0x8008, byte0/1 0x8034/0x803c, esc0 0x8044; the MDP RCG at 0x8150, CFG +4: source 10:8, half-integer divider 4:0).
constexpr uint32_t kCcMdssGdsc = 0x9000, kCcAhb = 0x80b0, kCcMdp = 0x8010, kCcVsync = 0x8030, kCcPclk0 = 0x8004, kCcPclk1 = 0x8008,
                   kCcByte0 = 0x8034, kCcByte1 = 0x803c, kCcEsc0 = 0x8044, kCcMdpRcg = 0x8150;

// ------------------------------------------------------------------------------------------------------------- MDSS top
constexpr uint32_t kMdssHwVersion = 0x00, kMdssIntrStatus = 0x10;   // msm msm_mdss: HW_INTR_STATUS, one bit per sub-block

// ---------------------------------------------------------------------------------------------------------------- DPU top
// (msm dpu_hwio.h, the 7xxx interrupt set of dpu_hw_interrupts.c)
constexpr uint32_t kTopIntfSel = 0x004, kTopIntr2En = 0x008, kTopIntr2Status = 0x00c, kTopIntrEn = 0x010, kTopIntrStatus = 0x014,
                   kTopSplitEn = 0x2f4, kTopSplitUpper = 0x2f8, kTopSplitLower = 0x3f0, kTopDanger = 0x360, kTopSafe = 0x364,
                   kTopVsyncSel = 0x414, kTopDceSel = 0x450, kTopDpPhyIntfSel = 0x460;
// TOP0 INTR bits (msm catalog): INTF0 underrun 24 / vsync 25, INTF1 26/27, INTF2 28/29, INTF3 30/31; PINGPONG0..3 done 8..11.
constexpr unsigned kIntrIntfUnderrun[4] = {24, 26, 28, 30}, kIntrIntfVsync[4] = {25, 27, 29, 31};

// ------------------------------------------------------------------------------------------------------------------ SSPP
// 4 VIG (QSEED3 v3.4 scaler, CSC) and 6 DMA pipes (DMA4/5 cursor-capable), each 0x344 bytes (msm catalog sm8750_sspp).
struct Pipe {
  const char *name;   // msm's name
  uint32_t base;      // from kMdp
  bool vig;
  unsigned id;        // index within its type: the LM source-select pipe id and the CTL fetch-active bit order
  unsigned flushBit;  // CTL_FLUSH bit of the pipe (msm dpu_hw_ctl update_pending_flush_sspp)
  unsigned activeBit; // CTL_FETCH_PIPE_ACTIVE / CTL_PIPE_ACTIVE bit (msm fetch_tbl: DMA0..5 0..5, VIG0..3 16..19)
};
constexpr Pipe kPipes[] = {
  {"VIG0", 0x04000, true, 0, 0, 16},  {"VIG1", 0x06000, true, 1, 1, 17},  {"VIG2", 0x08000, true, 2, 2, 18},
  {"VIG3", 0x0a000, true, 3, 18, 19}, {"DMA0", 0x24000, false, 0, 11, 0}, {"DMA1", 0x26000, false, 1, 12, 1},
  {"DMA2", 0x28000, false, 2, 24, 2}, {"DMA3", 0x2a000, false, 3, 25, 3}, {"DMA4", 0x2c000, false, 4, 13, 4},
  {"DMA5", 0x2e000, false, 5, 14, 5},
};
constexpr unsigned kPipeCount = sizeof kPipes / sizeof kPipes[0];
// Registers (msm dpu_hw_sspp.c). Sizes and positions are height << 16 | width, y << 16 | x. Rect 1 (multirect) has its own copies.
constexpr uint32_t kSrcSize = 0x00, kSrcXy = 0x08, kOutSize = 0x0c, kOutXy = 0x10, kSrc0Addr = 0x14, kSrc1Addr = 0x18,
                   kSrc2Addr = 0x1c, kSrc3Addr = 0x20, kYStride0 = 0x24, kYStride1 = 0x28, kSrcFormat = 0x30, kUnpack = 0x34,
                   kOpMode = 0x38, kConstColor = 0x3c, kUbwcStatic = 0x44, kFetchConfig = 0x48, kQosCtrl = 0x6c,
                   kAddrSwStatus = 0x70, kUbwcError = 0x138, kOutSizeRec1 = 0x160, kOutXyRec1 = 0x164, kSrcXyRec1 = 0x168,
                   kSrcSizeRec1 = 0x16c, kMultirect = 0x170, kSrcFormatRec1 = 0x174, kUnpackRec1 = 0x178, kOpModeRec1 = 0x17c;
// Multirect (MULTIRECT_OPMODE): bit 0 rect 1 enabled (as written by msm: the rect index ORed in), bit 2 time-multiplexed (else
// parallel). Rect 0 fetches from SRC0 (plane 0) / SRC2, rect 1 from SRC1 / SRC3; YSTRIDE0 = rect 1 pitch << 16 | rect 0 pitch.

// -------------------------------------------------------------------------------------------------------------------- CTL
// CTL0..5 at 0x15000 + 0x1000 n (msm catalog). DPU 12 has no CTL_LAYER blend registers: the blend stages are in the LMs, the CTL
// only names the active pipes and LMs (msm dpu_hw_ctl.c, core_major_ver >= 12).
constexpr uint32_t kCtlBase = 0x15000, kCtlStride = 0x1000;
constexpr unsigned kCtlCount = 6;
constexpr uint32_t kCtlTop = 0x014, kCtlFlush = 0x018, kCtlStart = 0x01c, kCtlPrepare = 0x0d0, kCtlMerge3dActive = 0x0e4,
                   kCtlDscActive = 0x0e8, kCtlWbActive = 0x0ec, kCtlCwbActive = 0x0f0, kCtlIntfActive = 0x0f4,
                   kCtlCdmActive = 0x0f8, kCtlFetchActive = 0x0fc, kCtlMerge3dFlush = 0x100, kCtlDscFlush = 0x104,
                   kCtlIntfFlush = 0x110, kCtlPeriphFlush = 0x128, kCtlPipeActive = 0x12c, kCtlLayerActive = 0x130,
                   kCtlIntfMaster = 0x134, kCtlDspp0Flush = 0x13c;
// CTL_TOP: bit 17 command mode (else video), bits 31:28 the VM group id (msm writes 0xf). CTL_FLUSH: LM0..7 bits 6,7,8,9,10,20,21,27;
// bit 17 the CTL itself; 22 DSC, 23 MERGE_3D, 29 DSPP, 31 INTF (the sub-flush registers say which instance).
constexpr unsigned kLmFlushBit[8] = {6, 7, 8, 9, 10, 20, 21, 27};

// --------------------------------------------------------------------------------------------------------------------- LM
// LM0..7 at 0x44000 + 0x1000 n, pairs 0/1 2/3 4/5 6/7; LM n feeds PINGPONG n; LM0..3 have DSPP0..3 (msm catalog).
constexpr uint32_t kLmBase = 0x44000, kLmStride = 0x1000;
constexpr unsigned kLmCount = 8;
constexpr uint32_t kLmOpMode = 0x00, kLmOutSize = 0x04, kLmBgSrcSel = 0x14, kLmBorder0 = 0x1c, kLmBorder1 = 0x20;
// 11 blend stages above the base at 0x40 + 0x30 s (msm sm8750_lm_sblk): BLEND_OP +0, FG_SRC_SEL +4, CONST_ALPHA +8 (v12).
// A source select byte: 7:6 pipe type (0 DMA, 1 VIG, 3 none), 5:4 rect, 3:0 pipe id; two bytes per stage (source split);
// 0xc0c0 = nothing staged. LM_OP_MODE bit 31: this is the right mixer of a split pair.
constexpr unsigned kLmStages = 11;
constexpr uint32_t lmStage(unsigned s) { return 0x40 + 0x30 * s; }
constexpr uint32_t kLmBlendOp = 0x0, kLmFgSrcSel = 0x4, kLmConstAlpha = 0x8;

// ------------------------------------------------------------------------------------------------------------------- DSPP
// DSPP0..3 at 0x54000 + 0x2000 n (len 0x1800). PCC (v6) at +0x1700: enable +0 bit 0; coefficients, S3.15 (0x8000 = 1.0) (msm
// CONVERT_S3_15): output R from inputs (R,G,B) at +0x10/+0x14/+0x18, G +0x1c/+0x20/+0x24, B +0x28/+0x2c/+0x30 (msm dpu_hw_dspp.c's
// PCC_<input>_<output>_OFF names with dpu_crtc.c's CTM mapping: cfg->g.r = matrix[1], the output-R/input-G term, goes to
// PCC_GREEN_R_OFF 0x14). GC (v2) at +0x17c0: enable +0 bit 0, LUT swap +0x1c; its LUT is written through REGDMA's hardware-indexed
// port (msm dpu_hw_gcv2.c), not read here.
constexpr uint32_t kDsppBase = 0x54000, kDsppStride = 0x2000;
constexpr unsigned kDsppCount = 4;
constexpr uint32_t kPcc = 0x1700, kGc = 0x17c0;
constexpr uint32_t kPccCoef[3][3] = {{0x10, 0x14, 0x18}, {0x1c, 0x20, 0x24}, {0x28, 0x2c, 0x30}};   // [out][in]

// ---------------------------------------------------------------------------------------------------------- MERGE_3D, DSC
constexpr uint32_t kMerge3dBase[4] = {0x4e000, 0x4f000, 0x50000, 0x51000};   // MUX +0, MODE +4 (msm dpu_hw_merge3d.c)
// DSC 1.2 cores dce_0..3 at 0x80000 + 0x1000 n, two encoders each (msm dpu_hw_dsc_1_2.c): common MAIN_CNF +0 (bit 0 split panel,
// bit 1 multiplex, 8:7 active slices per encoder); encoder n at +0x100 / +0x200: DF_CTRL +0 (bit 9 video), MAIN_CONF +0x30 (bpc 3:0,
// bpp<<4 at 19:10, block prediction bit 20, minor version 31:28), PICTURE_SIZE +0x34 (h<<16|w), SLICE_SIZE +0x38 (h<<16|w);
// encoder ctl at +0xf00 / +0xf80: DSC_CTL +0 (input PINGPONG index 2:0, 0xf disabled), DSC_CFG +4.
constexpr uint32_t kDscBase = 0x80000, kDscStride = 0x1000;
constexpr unsigned kDscCount = 4;
constexpr uint32_t kDscEnc[2] = {0x100, 0x200}, kDscCtl[2] = {0xf00, 0xf80};

// ------------------------------------------------------------------------------------------------------------------- INTF
// INTF0 DP, INTF1 DSI0, INTF2 DSI1, INTF3 DP (MST) at 0x34000 + 0x1000 n (msm catalog). Timing in INTF pixel clocks (for DSC the
// compressed clock): HSYNC_CTL = htotal << 16 | hsync width; VSYNC_PERIOD_F0 = vtotal * htotal; VSYNC_PULSE_WIDTH_F0 = vsync lines *
// htotal; DISPLAY_V_START_F0 = (vsync + vbp) * htotal + skew; DISPLAY_V_END_F0 = (vtotal - vfp) * htotal + skew - 1; DISPLAY_HCTL =
// (htotal - hfp - 1) << 16 | (hsync + hbp) (msm dpu_hw_intf_setup_timing_engine). FRAME_LINE_COUNT_EN bits 0/1 enable the frame and
// line counters at +0xac/+0xb0. Interrupts: EN/STATUS/CLEAR at +0x1c0/+0x1c4/+0x1c8; tear block at 0x34800 + 0x1000 n.
constexpr uint32_t kIntfBase = 0x34000, kIntfStride = 0x1000;
constexpr unsigned kIntfCount = 4;
constexpr uint32_t kIntfEn = 0x000, kIntfConfig = 0x004, kIntfHsyncCtl = 0x008, kIntfVsyncPeriod = 0x00c, kIntfVsyncWidth = 0x014,
                   kIntfDispVStart = 0x01c, kIntfDispVEnd = 0x024, kIntfActVStart = 0x02c, kIntfActVEnd = 0x034,
                   kIntfDispHctl = 0x03c, kIntfActHctl = 0x040, kIntfBorder = 0x044, kIntfUnderflowColor = 0x048,
                   kIntfHsyncSkew = 0x04c, kIntfPolarity = 0x050, kIntfConfig2 = 0x060, kIntfDataHctl = 0x064,
                   kIntfPanelFormat = 0x090, kIntfCountEn = 0x0a8, kIntfFrameCount = 0x0ac, kIntfLineCount = 0x0b0,
                   kIntfProgFetch = 0x170, kIntfMux = 0x25c, kIntfStatus = 0x26c, kIntfAvrControl = 0x270, kIntfIntrEn = 0x1c0,
                   kIntfIntrStatus = 0x1c4;
// INTF_CONFIG2 bit 0 wide bus, bit 4 data HCTL, bit 12 DSC data compression.

// -------------------------------------------------------------------------------------------------------------- DSI 6G ctrl
// Offsets from the controller's base (msm dsi.xml, which counts from base + 4: DSI_6G_REG_SHIFT). CTRL: bit 0 enable, 1 video
// mode, 2 command mode, 7:4 data lanes, 8 clock lane. Video timing: ACTIVE_H/V = end << 16 | start; TOTAL = (vtotal - 1) << 16 |
// (htotal - 1); with a bonded pair each controller has half the horizontal timing, with DSC the active part compressed while the porches stay
// (msm dsi_timing_setup): so front porch = TOTAL.h + 1 - ACTIVE_H.end, and vfp = TOTAL.v + 1 - ACTIVE_V.end.
constexpr uint32_t kDsiHwVersion = 0x000, kDsiCtrl = 0x004, kDsiStatus0 = 0x008, kDsiFifoStatus = 0x00c, kDsiVidCfg0 = 0x010,
                   kDsiVidCfg1 = 0x020, kDsiActiveH = 0x024, kDsiActiveV = 0x028, kDsiTotal = 0x02c, kDsiActiveHsync = 0x030,
                   kDsiVsyncHpos = 0x034, kDsiVsyncVpos = 0x038, kDsiLaneStatus = 0x0a8, kDsiLaneCtrl = 0x0ac,
                   kDsiErrIntMask0 = 0x10c, kDsiIntrCtrl = 0x110, kDsiClkCtrl = 0x11c, kDsiClkStatus = 0x120,
                   kDsiVersion = 0x1f4, kDsiVidCompress = 0x2a0;
// The video timing shadow of SM8750 (SunUEFI's working seamless-VFP patch, after the downstream timing double-buffer: DB_MODE = 1,
// TOTAL, TIMING_FLUSH = 1, DB_MODE = 0 on both controllers): Linux-relative 0x1e4 (flush) / 0x1e8 (DB mode), i.e. base + 0x1e8 /
// base + 0x1ec. The survey reads base + 0x1e4..0x1ec as raw words so a boot settles the exact place.
constexpr uint32_t kDsiTimingWords[3] = {0x1e4, 0x1e8, 0x1ec};

// ------------------------------------------------------------------------------------------------------- the panel's modes
// The Ubuntu piano panel driver's modes (panel piano_display_modes; XML: per-DSI HFP 117, HSYNC 16, HBP 16 at 120 Hz): the link
// runs at a fixed 1197.5 Mbit/s per lane (XML DSIBitClockFrequency, 4 lanes), the horizontal front porch selects 144/120/90 Hz,
// and lower rates of the 120 Hz group (60, also 50/48/30 in SunUEFI) only lengthen the vertical front porch. Full-panel numbers.
struct Mode {
  unsigned hz, hfp, vfp;   // hsync 32, hbp 32, vsync 2, vbp 104 in every mode
  uint8_t b2, b3;          // the panel's rate-group registers (DCS 0xB2/0xB3 on page 0x10), sent before sleep-out
};
constexpr Mode kModes[] = {
  {144, 42, 68, 0x00, 0x00}, {120, 234, 68, 0x91, 0x40}, {90, 618, 68, 0x00, 0x80}, {60, 234, 68 + 2310, 0x91, 0x40},
};
constexpr unsigned kHsync = 32, kHbp = 32, kVsync = 2, kVbp = 104;
constexpr unsigned modeHtotal(const Mode &m) { return kPanelWidth + m.hfp + kHsync + kHbp; }
constexpr unsigned modeVtotal(const Mode &m) { return kPanelHeight + m.vfp + kVsync + kVbp; }

// The DDR vote of the boot framebuffer at 120 Hz (DT chosen/framebuffer opp: avg 3 280 896 kB/s, peak 4 257 792 kB/s).
constexpr uint64_t kSplashAvgKBps = 3280896, kSplashPeakKBps = 4257792;

}  // namespace pianodpu
