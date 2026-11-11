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
constexpr uint32_t kPasId = 9;
constexpr uint32_t kCpStart = 0, kCpSize = 0x25800000, kCpNonPixelStart = 0x01000000, kCpNonPixelSize = 0x24800000;
constexpr uint32_t kScmMpVideoVar = 0x42000c08;
// The device address limit (iris_platform_vpu3x.c sm8750_data.dma_mask = 0xe0000000 - 1): every IOVA the core is given lies below.
constexpr uint64_t kIovaLimit = 0xe0000000;
// The firmware file (DT firmware-name; pin: 2409160 bytes, signed ELF32, from the stock odm_a firmware/vpu35_4v.mbn of
// OS3.0.303.0.WPYCNXM, sha256 95c5882f...). Read at run time from the owner's tablet; never in this repository.
constexpr const char *kFirmwareName = "qcom/sm8750/vpu35_4v.mbn";
constexpr uint32_t kFirmwareBytes = 2409160;

// --------------------------------------------------------------------------------------------------- HFI (levels 4+, not used here)
// iris_hfi_queue.h: one queue table (a 288-byte table header, then three 56-byte queue headers: command, message, debug; 456
// bytes), then three queues of 1024 x 64 x 16 bytes (1 MiB each, variable-size packets in 32-bit words, read/write indices in words); the SFR is
// 4 KiB. iris_hfi_gen2_packet.h: a 32-byte header {size, session_id, header_id, reserved[4], num_packets} followed by packets of
// a 32-byte {size, type, flags, payload_info, port, packet_id, reserved[2]} plus the payload.
constexpr uint32_t kHfiQueueBytes = 1024u * 64u * 16u;
constexpr uint32_t kHfiCmdInit = 0x01000001, kHfiPropImageVersion = 0x03000001;

}  // namespace pianovideo
