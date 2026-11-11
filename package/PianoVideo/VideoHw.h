// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo: the hardware facts of piano's video core (SM8750 Iris, VPU 3.5, "qcom,sm8750-iris"), each with its source. Every
// number here is a fact read from a source, never code: the piano boot DT (out/inputs/piano-boot.dtb, "DT"), the Linux iris driver,
// SM8750 clock controllers and SCM driver in the Ubuntu piano kernel (unixnu-research/ubuntu-ref/kernel, GPL-2.0, read as the
// specification only: "iris", "videocc", "gcc", "scm"), the Venus driver of the same tree for the wrapper's version word ("venus",
// marked inferred), and xiaomipad-8pro-mainline's firmware pin ("pin"). docs/drivers/video.md has the same facts with their state
// (verified / inferred / needs-boot). Offsets are from the block's base; the absolute address is base + offset.
#pragma once
#include <stdint.h>

namespace pianovideo {

// ----------------------------------------------------------------------------------------------------------------- blocks
constexpr uint64_t kIris = 0x0aa00000;        // DT video-codec@aa00000 (len 0xf0000), compatible qcom,sm8750-iris, status okay
constexpr uint32_t kIrisBytes = 0xf0000;
constexpr unsigned kIrisSpi = 174;            // DT interrupts <0 0xae 4>: SPI 174 level high = GIC INTID 206
constexpr uint64_t kVideoCc = 0x0aaf0000;     // DT clock-controller@aaf0000 (qcom,sm8750-videocc, len 0x10000); its own power
constexpr uint32_t kVideoCcBytes = 0x10000;   // domains are MMCX and MXC (DT power-domains, required-opps low_svs)
constexpr uint64_t kGcc = 0x00100000;         // GCC (DpuHw.h, the same SoC block)
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;   // apps SMMU (qcom,sm8750-smmu-500, dma-coherent in the DT)
// The firmware carveout: DT reserved-memory video@9c300000, 8 MiB, no-map; the node's memory-region. Outside XNU's DRAM window
// (0x8c0000000..0xc00000000, dts/piano.dts): XNU never allocates it.
constexpr uint64_t kCarveout = 0x9c300000, kCarveoutBytes = 0x800000;

// The video core's SMMU streams: DT iommus = <&apps_smmu 0x1940 0>, <&apps_smmu 0x1947 0> (one Linux domain for both). The survey
// lists every stream-match group covering 0x1940..0x194f, to see what else the boot stages or the hypervisor keep there.
constexpr uint16_t kStreamFirst = 0x1940, kStreamSecond = 0x1947, kStreamScanLast = 0x194f;

// --------------------------------------------------------------------------------------------------------------- GCC gate
// gcc: gcc_video_ahb_clk 0x32004 and gcc_video_xo_clk 0x32038 are among the branches Linux keeps on at probe (critical, never gated
// by Linux); gcc_video_axi0_clk 0x32018 (the DT's "iface", the codec's AXI) and gcc_video_axi1_clk 0x32028 ("iface1", the
// controller's AXI), both halt-skip, hardware clock gating at the same register; GCC_VIDEO_BCR 0x32000 (not read: a reset).
constexpr uint32_t kGccVideoAhb = 0x32004, kGccVideoXo = 0x32038, kGccVideoAxi0 = 0x32018, kGccVideoAxi1 = 0x32028;

// ------------------------------------------------------------------------------------------------------------- VIDEO_CC
// videocc (videocc-sm8750.c), offsets from kVideoCc:
constexpr uint32_t kCcPll0 = 0x0000;          // video_cc_pll0, Taycan ELU: MODE +0 (bit 31 LOCK_DET, 2 RESET_N, 1 BYPASSNL, 0
constexpr uint32_t kPllOpmode = 0x04, kPllState = 0x08, kPllL = 0x10, kPllAlpha = 0x14;   // OUTCTRL), OPMODE (1 run), L, 16-bit alpha
constexpr uint32_t kPllConfigL = 0x25, kPllConfigAlpha = 0x8000;   // Linux's configuration: 19.2 MHz x (37 + 0.5) = 720 MHz
constexpr uint32_t kCcMvs0Rcg = 0x8000;       // video_cc_mvs0_clk_src (CMD +0, CFG +4: source 10:8 (0 XO, 1 PLL0), divider 4:0);
                                              // its table: 720, 1014, 1260, 1332, 1600, 1710, 1890 MHz, all from PLL0
constexpr uint32_t kCcAhbRcg = 0x8018;        // video_cc_ahb_clk_src (19.2 MHz from XO)
constexpr uint32_t kCcMvs0cGdsc = 0x8034;     // video_cc_mvs0c_gdsc: the controller ("venus" power domain); GDSCR, CFG at +4
constexpr uint32_t kCcMvs0cClk = 0x804c;      // video_cc_mvs0c_clk (the DT's "core"), voted branch
constexpr uint32_t kCcMvs0cFreerun = 0x805c;  // video_cc_mvs0c_freerun_clk ("core_freerun")
constexpr uint32_t kCcMvs0cDiv2 = 0x8060;     // video_cc_mvs0c_div2_div_clk_src (read-only divider, 4 bits: divide by value + 1)
constexpr uint32_t kCcMvs0Gdsc = 0x8068;      // video_cc_mvs0_gdsc: the codec ("vcodec0"), child of mvs0c, hardware-controlled
constexpr uint32_t kCcMvs0Clk = 0x807c;       // video_cc_mvs0_clk ("vcodec0_core"; the OPP clock), voted, hw clock gating bit 1
constexpr uint32_t kCcMvs0Freerun = 0x808c;   // video_cc_mvs0_freerun_clk ("vcodec0_core_freerun"); its memory enable at 0x8090
constexpr uint32_t kCcMvs0FreerunMem = 0x8090;
constexpr uint32_t kCcMvs0Div = 0x809c;       // video_cc_mvs0_div_clk_src (read-only, 4 bits): the OPP rate = mvs0_clk_src / (v+1)
constexpr uint32_t kCcAhbClk = 0x80a4, kCcXoClk = 0x80d4, kCcSleepClk = 0x80f8;   // the three branches Linux keeps on (critical)
constexpr uint32_t kCcMvs0Shift = 0x80d8, kCcMvs0cShift = 0x80dc;
constexpr uint32_t kGdscCfg = 0x4;            // GDSC CFG register: bit 16 POWER_UP_COMPLETE, bit 15 POWER_DOWN_COMPLETE
constexpr uint64_t kXoHz = 19200000;          // bi_tcxo (PLL0's and the RCGs' XO parent)

// The operating points (DT iris_opp_table, "vcodec0_core" rate) and the rails each needs (required-opps: MXC, MMCX):
//   240 MHz svs / low_svs_d1, 338 svs / low_svs, 420 svs_l1 / svs, 444 svs_l1 / svs_l1, 533.333 svs_l1 / nom, 570 nom / nom_l1,
//   630 nom / turbo. 240 MHz = PLL0's 720 MHz through the mvs0 divider of 3 (inferred from the two tables).
constexpr uint32_t kOppMHz[7] = {240, 338, 420, 444, 533, 570, 630};

// ------------------------------------------------------------------------------------------------------------- the core
// iris_vpu_register_defines.h / iris_vpu_common.c / iris_vpu3x.c, offsets from kIris. Regions: VCODEC 0x00000 (the codec, MVS0
// domain), CPU (CPU_CS / CPU_IC) 0xa0000, WRAPPER 0xb0000, WRAPPER_TZ 0xc0000, AON 0xe0000 (these four: the controller, MVS0C).
constexpr uint32_t kVcodecIdleStatus = 0x70;  // VCODEC_SS_IDLE_STATUSN + 4 x pipe (4 VPP pipes on SM8750); bit 22 idle
constexpr unsigned kVppPipes = 4;
constexpr uint32_t kAonNocResetReq = 0x1f000, kAonNocResetAck = 0x1f004;   // AON_WRAPPER_MVP_NOC_RESET_REQ/ACK (codec space)
constexpr uint32_t kCpuCsA2hSoftIntClr = 0xa001c;   // write-to-clear: never loaded by the survey
constexpr uint32_t kCtrlInit = 0xa0048;       // CTRL_INIT: bit 0 written by the host to start the firmware's HFI
constexpr uint32_t kCtrlStatus = 0xa004c;     // CTRL_STATUS: bit 0 init done, 7:1 error (0x4: bad UC region), bit 8 PC_READY, bit 30
                                              // idle message
constexpr uint32_t kQtblInfo = 0xa0050, kQtblAddr = 0xa0054;   // queue table: enable bit 0; its device (IOVA) address
constexpr uint32_t kSciaCmdArg3 = 0xa0058;    // CPU_CS_SCIACMDARG3 (the host writes 1 at boot)
constexpr uint32_t kSfrAddr = 0xa005c;        // the subsystem-failure-reason buffer's IOVA (+ the core arch)
constexpr uint32_t kUcRegionAddr = 0xa0064, kUcRegionSize = 0xa0068;   // the uncached region (queues + SFR), 1 MiB aligned
constexpr uint32_t kH2xSoftIntEn = 0xa0148;   // CPU_CS_H2XSOFTINTEN: bit 0 host-to-firmware interrupt enable
constexpr uint32_t kCpuIcSoftInt = 0xa0150;   // CPU_IC_SOFTINT: a write rings the firmware: never loaded by the survey
constexpr uint32_t kAhbBridgeSyncReset = 0xa0160;
constexpr uint32_t kX2Rpmh = 0xa0168;         // CPU_CS_X2RPMH: bit 0 mask the firmware's signal, bit 1 mask core power-on
constexpr uint32_t kWrapperHwVersion = 0xb0000;   // venus WRAPPER_HW_VERSION at the v6 wrapper base (inferred for VPU 3.5):
                                                  // major 30:28 (mask 0x78000000), minor 27:16, step 15:0
constexpr uint32_t kWrapperIntrStatus = 0xb000c;  // bit 2 A2H (a message), bit 3 A2H watchdog
constexpr uint32_t kWrapperIntrMask = 0xb0010;    // bit 2 / bit 3 masked when set
constexpr uint32_t kDebugBridgeLpiCtl = 0xb0054, kDebugBridgeLpiStatus = 0xb0058;
constexpr uint32_t kCpuNocLpiCtl = 0xb005c, kCpuNocLpiStatus = 0xb0060;   // status: bit 0 done, 1 deny, 2 active
constexpr uint32_t kWrapperSpare0 = 0xb0078;      // VPU 3.5 boot register (the host writes 1 before CTRL_INIT)
constexpr uint32_t kCorePowerStatus = 0xb0080;    // bit 1: the codec (MVS0) is powered
constexpr uint32_t kCoreClockConfig = 0xb0088;    // 0 = core clock runs (the host writes 0 at power-on)
constexpr uint32_t kTzCpuStatus = 0xc0010;        // bit 0: the firmware's CPU is in WFI
constexpr uint32_t kTzAxiClockConfig = 0xc0014, kTzQns4pdxFifoReset = 0xc0018;
constexpr uint32_t kAonNocLpiCtl = 0xe0000, kAonNocLpiStatus = 0xe0004;
constexpr uint32_t kAonNocCoreSwReset = 0xe0018, kAonNocCoreClkCtl = 0xe0020, kAonSpare = 0xe0028;
constexpr uint32_t kAonVideoCtlNocLpiCtl = 0xe002c, kAonVideoCtlNocLpiStatus = 0xe0030;

// ------------------------------------------------------------------------------------------------- firmware and the secure world
// iris_firmware.c: PAS ID 9; the image goes into the memory-region carveout, then auth_and_reset(9), then for each tz_cp_config
// entry (iris_platform_vpu3x.c, one for VPU 3.x) the SCM call MP_VIDEO_VAR (service 0x0c, command 0x08: SMC 0x42000c08, the SiP
// owner and the SMC64 yielding convention of Sm8750Scm's other calls) with the content-protection ranges below; suspend/resume is
// SET_REMOTE_STATE (service 1, command 0x0a) with (0|1, 0); unload is PAS shutdown(9).
// Every one of these calls (from level 3; level 1 makes none) holds the kernel-wide secure-call lock (package/include/PianoScmLock.h,
// docs/scm-lock.md, tag PianoScmTagVideo): around each request through the Sm8750Scm ops, and once from init_image through
// auth_and_reset and MP_VIDEO_VAR. PianoVideo.json names PianoA830 (the word's owner) as a library from then on.
constexpr uint32_t kPasId = 9;
constexpr uint32_t kCpStart = 0, kCpSize = 0x25800000, kCpNonPixelStart = 0x01000000, kCpNonPixelSize = 0x24800000;
constexpr uint32_t kScmMpVideoVar = 0x42000c08;
// The device address limit (iris_platform_vpu3x.c sm8750_data.dma_mask = 0xe0000000 - 1): every IOVA the core is given lies below.
constexpr uint64_t kIovaLimit = 0xe0000000;
// The firmware file (DT firmware-name; pin: 2409160 bytes, signed ELF32, from the stock odm_a firmware/vpu35_4v.mbn of
// OS3.0.303.0.WPYCNXM, sha256 95c5882f...). Read at run time from the owner's tablet; never in this repository.
constexpr const char *kFirmwareName = "qcom/sm8750/vpu35_4v.mbn";
constexpr uint32_t kFirmwareBytes = 2409160;

// --------------------------------------------------------------------------------------------------------- HFI (level 4 = V3)
// iris_hfi_queue.h: one queue table (a 288-byte table header, then three 56-byte queue headers: command, message, debug; 456
// bytes), then three queues of 1024 x 64 x 16 bytes (1 MiB each, variable-size packets in 32-bit words, read/write indices in words); the SFR is
// 4 KiB. iris_hfi_gen2_packet.h: a 32-byte header {size, session_id, header_id, reserved[4], num_packets} followed by packets of
// a 32-byte {size, type, flags, payload_info, port, packet_id, reserved[2]} plus the payload.
constexpr uint32_t kHfiQueueBytes = 1024u * 64u * 16u;
constexpr uint32_t kHfiCmdInit = 0x01000001, kHfiPropImageVersion = 0x03000001;

// ============================================================================================================================
// Levels 2-4 of piano-video (the ladder's V1-V3, docs/drivers/video.md section 8). The facts the writes take, each with its source.
// ============================================================================================================================

// ------------------------------------------------------------------------------------------------- GCC: the video branches (V1)
// gcc: gcc_video_axi0_clk / gcc_video_axi1_clk are BRANCH_HALT_SKIP with hardware clock gating at bit 1 of the same register; their
// resets "bus0" / "bus1" (DT resets <&gcc 0x1d>, <&gcc 0x1e> = GCC_VIDEO_AXI0_CLK_ARES / _AXI1_CLK_ARES) are bit 2 of those registers.
// gcc_video_ahb_clk / gcc_video_xo_clk get CLK_EN at Linux's probe (qcom_branch_set_clk_en) and are never gated by Linux.
constexpr uint32_t kCbcrEnable = 1u << 0, kCbcrHwcg = 1u << 1, kCbcrAres = 1u << 2, kCbcrOff = 1u << 31;
constexpr uint32_t kCbcrNocFsmShift = 28, kCbcrNocFsmOn = 2;   // clk-branch.h: CBCR_NOC_FSM_STATUS 30:28, FSM_STATUS_ON
constexpr uint32_t kBranchHaltUs = 200;                        // clk_branch_wait: 200 x 1 us
constexpr uint32_t kBranchVotedOffUs = 10;                     // a voted branch's disable: a 10 us delay instead of a poll

// ------------------------------------------------------------------------------------------------------- VIDEO_CC (V1)
// PLL0, Taycan ELU (clk-alpha-pll.c clk_alpha_pll_regs[TAYCAN_ELU], clk_lucid_evo_pll_configure, alpha_pll_lucid_evo_enable /
// _disable / _prepare; clk_alpha_pll_taycan_elu_ops = clk_alpha_pll_lucid_evo_ops):
constexpr uint32_t kPllStatus = 0x0c, kPllUserCtl = 0x18, kPllUserCtlU = 0x1c, kPllConfigCtl = 0x20, kPllConfigCtlU = 0x24,
                   kPllConfigCtlU1 = 0x28, kPllTestCtl = 0x2c, kPllTestCtlU = 0x30;
constexpr uint32_t kPllOutctrl = 1u << 0, kPllResetN = 1u << 2, kPllPcalNotDone = 1u << 8, kPllLockDet = 1u << 31;
constexpr uint32_t kPllStandby = 0, kPllRun = 1;
constexpr uint32_t kPllOutMask = 7, kPllEnableVoteRun = 1u << 25;   // USER_CTL: the outputs; FSM (voted) mode
constexpr uint32_t kPllCalL = 0x44u << 16;                          // TRION_PLL_CAL_VAL in L_VAL 31:16 (no cal_l in the config)
// videocc-sm8750.c video_cc_pll0_config (written only when non-zero, as clk_alpha_pll_write_config does; skipped altogether when the
// PLL already runs: "If the bootloader left the PLL enabled it's likely that there are RCGs that will lock up if we disable it").
constexpr uint32_t kPll0ConfigCtl = 0x19660387, kPll0ConfigCtlU = 0x098060a0, kPll0ConfigCtlU1 = 0xb416cb20, kPll0UserCtlU = 0x2;
constexpr uint32_t kPllLockUs = 1500;                               // wait_for_pll: 1500 x 1 us for LOCK_DET
// RCG2 (clk-rcg2.c): CMD UPDATE 0, ROOT_EN 1, ROOT_OFF 31; CFG source 10:8, half-divider 4:0 (hid_width 5), mode 13:12, HW_CLK_CTRL 20.
constexpr uint32_t kRcgUpdate = 1u << 0, kRcgRootEn = 1u << 1, kRcgRootOff = 1u << 31;
constexpr uint32_t kRcgCfgMask = 0x1fu | 7u << 8 | 3u << 12 | 1u << 20;
// video_cc_mvs0_clk_src F(720000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0): source 1 (parent_map_1), pre_div 2 * 1 - 1 = 1. Shared
// RCG (clk_rcg2_shared_ops): enable = force ROOT_EN, CFG, UPDATE, release; disable = park on the safe source (XO, CFG 0) the same way.
constexpr uint32_t kMvs0Cfg720 = 1u << 8 | 1u, kRcgCfgXo = 0;
constexpr uint32_t kRcgUpdateUs = 500;                              // update_config / clk_rcg2_set_force_enable: 500 x 1 us
constexpr uint32_t kMvs0DivFor240 = 2;                              // video_cc_mvs0_div_clk_src (read only): 720 MHz / (2 + 1)
// video_cc_mvs0_freerun_clk is a clk_mem_branch: mem_enable_reg 0x8090 bit 3, inverted (cleared to enable), ack 11:10.
constexpr uint32_t kMemEnable = 1u << 3, kMemAck = 3u << 10;
// GDSC (gdsc.c), both video GDSCs POLL_CFG_GDSCR | RETAIN_FF_ENABLE, en_rest_wait 2, en_few_wait 2, clk_dis_wait 6; MVS0 also
// HW_CTRL_TRIGGER (hardware mode switched by the iris driver: on after the firmware boots, off before the power-off).
constexpr uint32_t kGdscCollapse = 1u << 0, kGdscHwControl = 1u << 1, kGdscSwOverride = 1u << 2, kGdscRetainFf = 1u << 11,
                   kGdscPwrOn = 1u << 31;
constexpr uint32_t kGdscWaitMask = 0xfu << 20 | 0xfu << 16 | 0xfu << 12, kGdscWaits = 2u << 20 | 2u << 16 | 6u << 12;
constexpr uint32_t kGdscPowerUpComplete = 1u << 16, kGdscPowerDownComplete = 1u << 15;   // CFG_GDSCR (GDSCR + 4)
constexpr uint32_t kGdscPollUs = 2000;                              // STATUS_POLL_TIMEOUT_US
// qcom_cc_really_probe with video_cc_sm8750_driver_data: PLL0 configured, the critical branches enabled, clk_sm8750_regs_configure.
constexpr uint32_t kCcShifterDoneMvs0 = 0x8074, kCcShifterDoneMvs0c = 0x8040, kCcShifterDoneBits = 0x1fu << 21;   // 25:21 = 0xf..
constexpr uint32_t kCcSpare9f24 = 0x9f24;                           // bit 0 set at probe
// DT resets "core" <&videocc 5> = VIDEO_CC_MVS0C_FREERUN_CLK_ARES {0x805c, 2}, "vcodec0_core" <&videocc 4> = MVS0_FREERUN_CLK_ARES
// {0x808c, 2}; reset_control_bulk_reset = assert, 1 us, deassert (qcom_reset).

// ------------------------------------------------------------------------------------------------------- the core (V1, V3)
// iris_vpu3x.c iris_vpu33_power_off_hardware (VPU 3.5's codec power-off), iris_vpu_common.c iris_vpu35_vpu4x_power_off_controller,
// iris_vpu_set_preset_registers, iris_vpu_boot_firmware, iris_vpu_prepare_pc.
constexpr uint32_t kVcodecIdle = 1u << 22;                          // VCODEC_SS_IDLE_STATUSN: readl_poll_timeout(.., 2000, 20000)
constexpr uint32_t kIdlePollUs = 20000, kIdleStepUs = 2000;
constexpr uint32_t kLpiReq = 1u << 0;                               // REQ_POWER_DOWN_PREP
constexpr uint32_t kLpiDone = 1u << 0, kLpiDeny = 1u << 1, kLpiActive = 1u << 2;
constexpr uint32_t kLpiPollUs = 2000, kLpiStepUs = 200;             // readl_poll_timeout(.., 200, 2000)
constexpr unsigned kLpiRetries = 1000;                              // "Retry up to 1000 times as recommended by hardware documentation"
constexpr uint32_t kLpiRetryUsCodec = 15, kLpiRetryUsCtl = 10;      // udelay(15) (codec MVP NoC), usleep_range(10, 20) (video-ctl NoC)
constexpr uint32_t kBridgeSwReset = 1u << 0, kBridgeHwResetDisable = 1u << 1;
constexpr uint32_t kX2RpmhMaskAll = 3;                              // MSK_SIGNAL_FROM_TENSILICA | MSK_CORE_POWER_ON
constexpr uint32_t kCorePowered = 1u << 1;                          // WRAPPER_CORE_POWER_STATUS bit 1
constexpr uint32_t kCtrlStatusInitDone = 1u << 0, kCtrlStatusErrorMask = 0xfe, kCtrlStatusErrorUcRegion = 0x4,
                   kCtrlStatusPcReady = 0x100, kCtrlStatusIdle = 1u << 30;
constexpr unsigned kBootPolls = 1000;                               // iris_vpu_boot_firmware: 1000 x usleep_range(50, 100)
constexpr uint32_t kBootPollUs = 100;
constexpr uint32_t kPcPollUs = 2500, kPcStepUs = 250;               // iris_vpu_prepare_pc: readl_poll_timeout(.., 250, 2500)
constexpr uint32_t kUcRegionAlign = 1u << 20, kSfrBytes = 4096, kSfrCoreArch = 1;   // SFR_ADDR = SFR IOVA + VIDEO_ARCH_LX (1)
constexpr uint32_t kH2xEnable = 1, kSoftIntH2a = 1;                 // HOST2XTENSA_INTR_ENABLE; 1 << CPU_IC_SOFTINT_H2A_SHFT (0)
constexpr uint32_t kHfiResponseMs = 1000;                           // HW_RESPONSE_TIMEOUT_VALUE
constexpr uint32_t kIdleCollapseMs = 1500;                          // AUTOSUSPEND_DELAY_VALUE = HW_RESPONSE_TIMEOUT_VALUE + 500

// ------------------------------------------------------------------------------------------------------------- RPMh (V1)
// The AOP command database (DT aop-cmd-db@81c60000, 0x20000, no-map) and the apps RSC's DRV-2 (DT rsc@16500000 "drv-2" at 0x16520000,
// tcs-offset 0xd00, tcs-distance 0x2a0, three ACTIVE TCSes first): the same constants as PianoA830's Sm8750Rpmh.h and PianoWifi.
constexpr uint64_t kCmdDb = 0x81c60000, kCmdDbBytes = 0x20000;
constexpr uint64_t kRscDrv2 = 0x16520000, kRscDrv2Bytes = 0x10000;
constexpr uint32_t kRscTcsBlock = 0xd00, kRscTcsStride = 0x2a0;
constexpr unsigned kRscActiveTcs = 3;
// The resources (rpmhpd.c sm8750: mxc "mxc.lvl", mmcx_w_cx_parent "mmcx.lvl"; interconnect sm8750.c: BCM "MM1", enable_mask BIT(0)).
constexpr const char *kArcMxc = "mxc.lvl", *kArcMmcx = "mmcx.lvl", *kBcmMm1 = "MM1";
// Corners (RPMH_REGULATOR_LEVEL_*; DT rpmhpd opp table): what V1-V3 need. VIDEO_CC itself: MMCX and MXC at low_svs (its
// required-opps); the 240 MHz OPP: MXC svs (0x80), MMCX low_svs_d1 (0x38). So MXC >= 0x80, MMCX >= 0x40.
constexpr uint32_t kVlvlLowSvsD1 = 0x38, kVlvlLowSvs = 0x40, kVlvlSvs = 0x80, kVlvlNomL1 = 0x140;
constexpr uint32_t kMxcNeed = kVlvlSvs, kMmcxNeed = kVlvlLowSvs;
// MMCX also feeds the boot framebuffer's display (DT mdss: power-domain MMCX; the MDP's OPP at 575 MHz requires NOM_L1, which
// ABL leaves it at; docs/drivers/gpu.md). Nothing natively votes MMCX, and a DRV-2 vote replaces the DRV's earlier one: a vote below
// NOM_L1 could take the display's rail away. So the MMCX vote is never below this floor and is never lowered under it.
constexpr uint32_t kMmcxDisplayFloor = kVlvlNomL1;
// BCM MM1 (qnm_video_mvp, qns_mem_noc_sf; bcm-voter.c bcm_aggregate_mask: x 0, y enable_mask when any of its nodes has bandwidth):
// on = commit | valid | y 1, off = commit only (valid 0). The display rides MM0; the path's SH1 (qnm_mnoc_sf, also chm_apps: the
// CPUs' own traffic, so necessarily on), SH0/MC0/ACV (DDR: PianoA830's host vote or the boot's, both above anything V1-V3 move) and
// CN0 (cpu-cfg: keepalive) are not voted here (docs/drivers/video.md section 12.1).
constexpr uint32_t kBcmMm1On = 0x60000001, kBcmMm1Off = 0x40000000;
constexpr uint32_t kDrv2Tag = 0x56494445;                           // "VIDE": PianoVideo in sm8750_rpmh_drv2_owner

// ------------------------------------------------------------------------------------------------- the firmware file (V2)
// Read at run time from the ESP (GPT "esp", partition 37 on LUN 0, FAT32 with 4 KiB sectors: docs/persistent-boot.md), where
// tooling/persist/build-esp.py --video-firmware puts the owner's copy after checking it against the pin below. Never in this
// repository or a build output; never disassembled.
constexpr const char *kEspPartition = "esp";
constexpr const char *kEspFirmwarePath = "EFI/UniXNU/firmware/qcom/sm8750/vpu35_4v.mbn";
constexpr uint8_t kFirmwareSha256[32] = {   // xiaomipad-8pro-mainline firmware/piano-iris.json
    0x95, 0xc5, 0x88, 0x2f, 0x9b, 0x66, 0xb9, 0x3a, 0xd2, 0x63, 0x56, 0x04, 0xb7, 0xdb, 0x56, 0x3c,
    0xc3, 0x10, 0x48, 0x1e, 0x01, 0x7f, 0xa6, 0xb8, 0x2e, 0x0c, 0xe6, 0xcb, 0xb9, 0xea, 0xe0, 0x1b};
constexpr uint32_t kScmPilShutdownCmd = 0x06;                       // QCOM_SCM_PIL_PAS_SHUTDOWN
constexpr uint32_t kScmMpVideoVarArgs = 4;                          // QCOM_SCM_ARGS(4, VAL, VAL, VAL, VAL)
constexpr uint32_t kRemoteStateSuspend = 0, kRemoteStateResume = 1; // iris_set_hw_state: qcom_scm_set_remote_state(resume, 0)

// ------------------------------------------------------------------------------------------------------ the SMMU bank (V3)
// The streams' own stage-1 bank: IOVAs from 0x80000000 (above the content-protection range MP_VIDEO_VAR declares, below the core's
// 0xe0000000 limit); the UC region (queue table + three queues + the SFR) at its start, 1 MiB aligned, declared 4 MiB
// (ALIGN(SFR + queues, 1 MiB) as iris_vpu_setup_ucregion_memory_map computes it).
constexpr uint64_t kXnuDramBase = 0x8c0000000ull, kXnuDramEnd = 0xc00000000ull;   // dts/piano.dts memory@8c0000000 (DpuHw.h)
constexpr uint32_t kIovaBase = 0x80000000u;
constexpr uint32_t kQueueTableBytes = 456;                          // 288 + 3 x 56
constexpr uint32_t kQueueAreaBytes = (kQueueTableBytes + 3 * kHfiQueueBytes + 4095) & ~4095u;   // 0x301000
constexpr uint32_t kSfrOffset = kQueueAreaBytes;                    // the SFR right after the queues, inside the UC region
constexpr uint32_t kUcBytes = kQueueAreaBytes + kSfrBytes;          // 0x302000 bytes backed by memory
constexpr uint32_t kUcRegionBytes = (kUcBytes + kUcRegionAlign - 1) & ~(kUcRegionAlign - 1);   // 0x400000 declared
static_assert(kUcRegionBytes == 0x400000, "the UC region the firmware is told about");
static_assert(kIovaBase % kUcRegionAlign == 0 && kIovaBase + kUcRegionBytes <= kIovaLimit, "the window");

// ---------------------------------------------------------------------------------------------------------- HFI gen2 (V3)
// iris_hfi_common.h / iris_hfi_gen2_defines.h / iris_hfi_gen2_packet.c / iris_hfi_queue.c / ubwc_config.c (sm8750_data).
constexpr uint32_t kHfiHeaderBytes = 32, kHfiPacketBytes = 32, kHfiCorePacketBytes = 4096;   // IFACEQ_CORE_PKT_SIZE
constexpr uint32_t kHfiCmdPowerCollapse = 0x01000002, kHfiCmdBegin = 0x01000000, kHfiCmdEnd = 0x01ffffff;
constexpr uint32_t kHfiPropIntraFramePc = 0x03000002, kHfiPropUbwcMaxChannels = 0x03000003, kHfiPropUbwcMalLength = 0x03000004,
                   kHfiPropUbwcHbb = 0x03000005, kHfiPropUbwcSwzl1 = 0x03000006, kHfiPropUbwcSwzl2 = 0x03000007,
                   kHfiPropUbwcSwzl3 = 0x03000008, kHfiPropUbwcBankSpreading = 0x03000009;
constexpr uint32_t kHfiPropBegin = 0x03000000, kHfiPropEnd = 0x03ffffff, kHfiSysErrorBegin = 0x05000000, kHfiSysErrorEnd = 0x05ffffff;
constexpr uint32_t kHfiHostIntr = 1, kHfiHostResponse = 2, kHfiHostNonDiscardable = 4, kHfiHostGetProperty = 8;
constexpr uint32_t kHfiFwSuccess = 1, kHfiFwInformation = 2, kHfiFwSessionError = 4, kHfiFwSystemError = 8;
constexpr uint32_t kHfiPayloadNone = 0, kHfiPayloadU32 = 1, kHfiPortNone = 0, kHfiArchLx = 1;
constexpr uint32_t kHfiVersionChars = 128;                          // IRIS_FW_VERSION_LENGTH
// SM8750 UBWC (ubwc_config.c sm8750_data: UBWC 5.0, swizzle 6, bank spread, HBB 16, macrotile): max channels 8 (macrotile), MAL 32
// (not 64-byte min access: that is UBWC 1.0 only), HBB 16, swizzle level 1 off, 2 on, 3 on, bank spreading on.
constexpr uint32_t kUbwcMaxChannels = 8, kUbwcMalLength = 32, kUbwcHbb = 16, kUbwcSwzl1 = 0, kUbwcSwzl2 = 1, kUbwcSwzl3 = 1,
                   kUbwcBankSpreading = 1;
constexpr uint32_t kIfaceqDefaultHeader = 0x0101;                   // IFACEQ_DFLT_QHDR

}  // namespace pianovideo
