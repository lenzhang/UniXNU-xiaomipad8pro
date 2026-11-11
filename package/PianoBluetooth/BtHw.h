// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano (Xiaomi Pad 8 Pro, SM8750) facts for the WCN7860 Bluetooth path, as PianoBluetooth reads them. Facts only; no code, comment or
// table is copied from any source. Sources:
//   DT    = out/inputs/piano-boot.dtb (the running Ubuntu DT, dtc -I dtb -O dts): geniqup@8c0000 (the QUP2 wrapper: clocks m-ahb
//           gcc 129 / s-ahb gcc 130, iommus <apps_smmu 0x423 0>), serial@898000 (qcom,geni-uart: interrupts <0 461 4>, clock "se"
//           gcc 121 = GCC_QUPV3_WRAP2_S6_CLK, pinctrl qup-uart14-default + qup-uart14-cts-rts, no dmas property) and its child
//           bluetooth (qcom,wcn7860-bt: six outputs of the PMU as supplies, max-speed 3200000), wcn7860-pmu (bt-enable-gpios
//           <pm8550ve-f gpio 3>, wlan-enable-gpios <tlmm 16>, swctrl-gpios <tlmm 18>), pmic@5 gpio@8800 bt-en-default-state
//   KDTS  = the Ubuntu piano kernel's sm8750.dtsi / sm8750-xiaomi-piano.dts (piano/dev 7f8d6bed): the pin roles ("TX, RX" gpio26/27,
//           "CTS, RTS" gpio24/25), "Stock bt_uwb_en: VIN1, no bias, output", "Baud index 0x11 (3.2 Mbaud) in every stock
//           gngbtnv20 NVM", the PMU's supply map
//   GCC   = drivers/clk/qcom/gcc-sm8750.c and dt-bindings qcom,sm8750-gcc.h of that kernel: the wrapper/SE6 branch offsets, vote
//           bits, the SE6 RCG and its frequency table (rates, sources, dividers, M/N), parent map 5, its DFS registration
//   RCG   = drivers/clk/qcom/clk-rcg2.c: the CMD/CFG/M/N layout and the DFS registers of a QUP RCG
//   PINS  = drivers/pinctrl/qcom/pinctrl-sm8750.c: gpio24..27 function 1 = qup2_se6; gpio18 function 1 = wcn_sw_ctrl
//   PGPIO = drivers/pinctrl/qcom/pinctrl-spmi-gpio.c: the register layout of a PMIC GPIO (LV/MV subtypes), the order of its
//           configuration writes
//   ARB   = drivers/spmi/spmi-pmic-arb.c (v7) and this repository's k27 0057 / PianoTorch: the owner channel of an APID
//   FW    = xiaomipad-8pro-mainline firmware/piano-bluetooth-rom20.json: "OS3.0.303.0.WPYCNXM bluetooth_a FAT image, image directory"
//   SHENG = UniXNU-sheng porting.toml and reports (BSD-2-Clause-Patent; SM8550 + WCN7850, hardware-verified): the same QUP2 SE6 at
//           0x898000 with INTID 493 and stream 0x423 on SM8550; GENI firmware revision 0x204 (protocol 2) and QUP 0x30090000 read there
// docs/drivers/bluetooth.md section 2 has the same facts with their state (verified / inferred / needs-boot).
#pragma once
#include <stdint.h>

namespace pianobt {

// ---- the serial engine: QUP2 (wrapper geniqup@8c0000) SE6 = "UART14" (serial@898000) ----
constexpr uint64_t kSe6 = 0x00898000, kSe6Bytes = 0x4000;
constexpr uint64_t kQup2 = 0x008c0000, kQup2Bytes = 0x2000;
constexpr uint32_t kQupHwVersion = 0x4;        // the wrapper's QUPV3_HW_VER (major 31:28, minor 27:16, step 15:0)
constexpr uint32_t kSpiUart14 = 461;           // DT interrupts <0 461 IRQ_TYPE_LEVEL_HIGH>: GIC INTID 493
constexpr uint16_t kStreamQup2 = 0x423;        // the wrapper's SMMU stream (SE DMA); the GPI DMA engine is 0x436 (not used)

// ---- GCC (clock-controller@100000) ----
constexpr uint64_t kGcc = 0x00100000;
// The QUP2 votes: one word, one bit per branch (BRANCH_HALT_VOTED). The word is shared with every QUP2 user.
constexpr uint32_t kGccQup2Vote = 0x52010;
enum : uint32_t { kVoteCore = 1u << 0, kVoteSAhb = 1u << 1, kVoteMAhb = 1u << 2, kVoteCore2x = 1u << 3, kVoteS6 = 1u << 10 };
constexpr uint32_t kVoteBits = kVoteCore | kVoteSAhb | kVoteMAhb | kVoteCore2x | kVoteS6;
// Branch CBCRs: bit 31 CLK_OFF, bit 1 HW_CTL (hardware gating: may read CLK_OFF while idle although voted), bit 0 CLK_ENABLE.
struct Branch {
  const char *name;
  uint32_t cbcr;
  uint32_t vote;
  bool hwcg;   // GCC's hwcg_reg/hwcg_bit: the AHB branches are gated by hardware when idle
};
constexpr Branch kBranches[] = {
  {"m-ahb", 0x23298, kVoteMAhb, true}, {"s-ahb", 0x2329c, kVoteSAhb, true}, {"core", 0x232a0, kVoteCore, false},
  {"core2x", 0x232b4, kVoteCore2x, false}, {"s6", 0x1e76c, kVoteS6, false},
};
constexpr unsigned kBranchCount = sizeof kBranches / sizeof kBranches[0];
enum : unsigned { kBrMAhb = 0, kBrSAhb, kBrCore, kBrCore2x, kBrS6 };
constexpr uint32_t kCbcrClkOff = 1u << 31, kCbcrHwCtl = 1u << 1;
// The SE6 clock source: an RCG (gcc_qupv3_wrap2_s6_clk_src, DFS-capable) at 0x1e77c: CMD +0 (bit 0 update, bit 31 root off),
// CFG +4 (4:0 half divider 2d-1, 10:8 source, 13:12 M/N mode), M +8, N +0xc (~(N - M)), D +0x10, 16-bit M/N; DFS (RCG): CMD_DFSR
// +0x14 (bit 0 enabled, 4:1 the current level), PERF_DFSR(l) +0x1c + 4l (CFG's layout), PERF_M_DFSR(l) +0x5c + 4l,
// PERF_N_DFSR(l) +0x9c + 4l, eight levels.
constexpr uint32_t kGccS6Rcg = 0x1e77c;
constexpr uint32_t kRcgCfg = 0x4, kRcgM = 0x8, kRcgN = 0xc, kRcgD = 0x10, kRcgDfsCmd = 0x14, kRcgDfsPerf = 0x1c, kRcgDfsM = 0x5c,
                   kRcgDfsN = 0x9c, kRcgDfsLevels = 8;
// Parent map 5 of gcc-sm8750.c: 0 TCXO 19.2 MHz, 1 GPLL0 600 MHz, 2 GPLL7 (its rate is not in the source: 0 = unknown), 6 GPLL0
// even 300 MHz (GPLL0 / 2).
constexpr uint32_t kRcgParentHz[8] = {19200000, 600000000, 0, 0, 0, 0, 300000000, 0};
// The RCG's frequency table (what Linux may program when DFS is off). 115200 x 16 = 1.8432 MHz is 7.3728 MHz / 4; 3200000 x 16
// = 51.2 MHz is the 51.2 MHz entry itself (GPLL0 even x 64 / 375).
constexpr uint32_t kS6Rates[] = {7372800,  14745600, 19200000, 29491200,  32000000,  48000000,  51200000,  64000000,
                                 75000000, 80000000, 96000000, 100000000, 102400000, 112000000, 117964800, 128000000};
constexpr unsigned kS6RateCount = sizeof kS6Rates / sizeof kS6Rates[0];
// The same table as GCC has it (ftbl_gcc_qupv3_wrap2_s6_clk_src): rate, parent map 5 source, the half divider field (2 x divider
// - 1) and M/N (0/0: no M/N). Level 3 programs one of these when the boot chain left DFS off (clk-rcg2's configure: M, N as
// ~(N - M), D as ~clamp(N, M, 2 (N - M)) in 16 bits, then CFG, then CMD bit 0 and a wait of at most 500 us for it to clear).
struct RcgEntry {
  uint32_t hz;
  uint8_t source, halfDivider;
  uint16_t m, n;
};
constexpr RcgEntry kS6Table[] = {
  {7372800, 6, 1, 384, 15625},  {14745600, 6, 1, 768, 15625},  {19200000, 0, 1, 0, 0},       {29491200, 6, 1, 1536, 15625},
  {32000000, 6, 1, 8, 75},      {48000000, 6, 1, 4, 25},       {51200000, 6, 1, 64, 375},    {64000000, 6, 1, 16, 75},
  {75000000, 6, 7, 0, 0},       {80000000, 6, 1, 4, 15},       {96000000, 6, 1, 8, 25},      {100000000, 1, 11, 0, 0},
  {102400000, 6, 1, 128, 375},  {112000000, 6, 1, 28, 75},     {117964800, 6, 1, 6144, 15625}, {128000000, 1, 1, 16, 75},
};
static_assert(sizeof kS6Table / sizeof kS6Table[0] == kS6RateCount, "one table entry per rate");
constexpr uint32_t kRcgCmdUpdate = 1u << 0, kRcgCmdRootOff = 1u << 31, kRcgUpdatePolls = 500;
constexpr uint32_t kRcgCfgMask = 0x1fu | 7u << 8 | 3u << 12 | 1u << 20;   // half divider, source, M/N mode, hardware control
// The GCC window levels 2 and up map: the vote word, the five CBCRs and the RCG (0x100000 + 0x60000, PianoTouch's window).
constexpr uint64_t kGccBytes = 0x60000;

// ---- TLMM (pinctrl@f100000): the UART's pins and the PMU's two TLMM lines ----
// Pin n: configuration at n * 0x1000 (pull 1:0 (0 none, 1 down, 2 keeper, 3 up), function 5:2, drive 8:6 ((v + 1) * 2 mA),
// output enable 9), input (bit 0) and output (bit 1) at +4. Same layout as PianoWifi/PianoPower/PianoDwc3.
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000;
struct Pin {
  const char *name;
  unsigned gpio;
  uint8_t function;   // the function the DT's state selects (0 = plain GPIO)
  int8_t pull;        // -1 down, 0 none, 1 up (the DT's state)
  bool output;        // a GPIO the PMU's driver drives (WLAN_EN)
};
// DT: qup-uart14-default gpio26/27 qup2_se6 2 mA pull-up (KDTS: TX, RX); qup-uart14-cts-rts gpio24/25 qup2_se6 2 mA pull-down
// (KDTS: CTS, RTS); wlan-en-state gpio16 GPIO pull-down output low; SW_CTRL gpio18 (an input from the chip; PINS function 1 is
// wcn_sw_ctrl, the DT uses it as a GPIO). Which pad carries which signal is the DT's comment (verified as a source fact; the pads'
// levels at hand-over are needs-boot). The SE itself reports the RX line (SE_GENI_IOS bit 0) and CTS (bit 1, low = asserted).
constexpr Pin kPins[] = {{"CTS", 24, 1, -1, false}, {"RTS", 25, 1, -1, false}, {"TX", 26, 1, 1, false},
                         {"RX", 27, 1, 1, false},   {"WLAN_EN", 16, 0, -1, true}, {"SW_CTRL", 18, 0, 0, false}};
constexpr unsigned kPinCount = sizeof kPins / sizeof kPins[0];
enum : unsigned { kPinCts = 0, kPinRts, kPinTx, kPinRx, kPinWlanEn, kPinSwCtrl };
constexpr uint64_t tlmmCfg(unsigned gpio) { return kTlmm + uint64_t(gpio) * kTlmmStride; }
constexpr uint64_t tlmmIo(unsigned gpio) { return tlmmCfg(gpio) + 4; }
// Level 3's window: gpio0 .. gpio28 (WLAN_EN 16, SW_CTRL 18, the UART 24..27), never written but for the four UART pins.
constexpr uint64_t kTlmmBytes = 0x1d000;
constexpr unsigned kUartDriveMa = 2;   // the DT's drive-strength for all four

// ---- BT_EN: pm8550ve "f" (SPMI SID 5) GPIO 3 ----
// A PMIC GPIO n is peripheral 0x88 + n - 1 (gpio@8800, DT gpio numbers are 1-based: PGPIO's PMIC_GPIO_PHYSICAL_OFFSET), so
// PPID 0x58a. The stock state (KDTS "bt_uwb_en", DT bt-en-default-state): function normal, VIN1, no bias, output low.
constexpr uint8_t kBtEnSid = 5, kBtEnPage = 0x8a;
constexpr uint16_t kBtEnPpid = uint16_t(kBtEnSid << 8 | kBtEnPage);
// PGPIO registers: TYPE 0x04 (0x10 = GPIO), SUBTYPE 0x05 (0x10..0x15 the LV/MV kinds), STATUS1 0x08, RT_STS 0x10 (bit 0 the pin's
// level while its input is enabled), MODE_CTL 0x40 (LV/MV: 1:0 direction 0 in, 1 out, 2 in+out, 3 analog), DIG_VIN_CTL 0x41
// (2:0 power source), DIG_PULL_CTL 0x42 (2:0: 0..3 pull-up strengths, 4 down, 5 none), DIG_IN_CTL 0x43, DIG_OUT_SOURCE_CTL 0x44
// (LV/MV: bit 7 the output level, 3:0 the function, 0 = normal), DIG_OUT_CTL 0x45 (1:0 strength, 5:4 buffer type), EN_CTL 0x46
// (bit 7 enabled).
constexpr uint8_t kPgType = 0x04, kPgSubtype = 0x05, kPgStatus1 = 0x08, kPgRtSts = 0x10, kPgMode = 0x40, kPgVin = 0x41,
                  kPgPull = 0x42, kPgDigIn = 0x43, kPgOutSrc = 0x44, kPgDigOut = 0x45, kPgEn = 0x46;
constexpr uint8_t kPgTypeGpio = 0x10, kPgOutLevel = 0x80, kPgEnabled = 0x80, kPgPullNone = 5;
inline bool pgLvMv(uint8_t subtype) { return subtype >= 0x10 && subtype <= 0x15; }
// Level 2 drives BT_EN to the DT's state: MODE_CTL 1 (digital output), DIG_VIN_CTL 1 (VIN1), DIG_PULL_CTL 5 (no bias),
// DIG_OUT_SOURCE_CTL bit 7 = the level with function 0 (normal), EN_CTL 0x80; written in PGPIO's order (VIN, PULL, MODE, OUT_SOURCE,
// EN), each read back, and only these five registers of this one peripheral (DIG_IN_CTL, DIG_OUT_CTL and the analog pass-through
// are kept as they read: the DT names no strength). When the four configuration registers already read as the DT's state (the
// stock "bt_uwb_en" state level 1 expects), only DIG_OUT_SOURCE_CTL is written.
constexpr uint8_t kPgModeOutput = 1, kPgVin1 = 1;
// The SPMI arbiter v7's owner channels (DT spmi@c400000 chnls 0xc500000): 0x1000 per APID, command +0x00 (opcode 0 = extended
// write, register << 4, bytes - 1), status +0x08 (bit 0 done, bits 1..3 failure, denied, dropped), write data +0x10. The arbiter
// takes a write only from the APID's owner EE (the cnfg table, SpmiObserver.h), so level 2 maps the channel only for an APID that
// level 1 found owned by EE 0; PianoTorch's TorchCore.h and PianoDwc3's restart-reason write are the same channel.
constexpr uint64_t kSpmiChnls = 0x0c500000;
constexpr uint32_t kChnlStride = 0x1000, kChnlCmd = 0x00, kChnlStatus = 0x08, kChnlWdata0 = 0x10, kChnlBytes = 0x20;
constexpr uint32_t kOpExtWrite = 0;
// BT_EN low before it rises when it was found high (an earlier start stopped first; PianoWifi holds WLAN_EN low 150 ms for the same).
constexpr unsigned kBtEnLowMs = 100;

// ---- apps SMMU (iommu@15000000, ARM SMMUv2 / MMU-500): read only here ----
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;

// ---- the link and the controller ----
// DT max-speed 3200000; the controller starts at 115200 (Linux's init speed for every QCA UART part); KDTS: the stock NVMs carry
// baud index 0x11 (3.2 Mbit/s). Hardware flow control (CTS/RTS) on both sides.
constexpr uint32_t kInitBaud = 115200, kOperBaud = 3200000;
// pwrseq-qcom-wcn's wcn7860 entry: the six rails (shared with Wi-Fi), then the enable pin high, then 50 ms before the chip
// answers; no gap between the two enable pins (its gpio_enable_delay is 0 for this part). Ubuntu runs Bluetooth with no RF
// clock vote on the PMU (K:f46f8386ed88).
constexpr unsigned kPmuPowerUpMs = 50;

// ---- firmware (never in this repository: read at run time from the tablet's own partition) ----
// FW: the stock bluetooth_a FAT image, directory image/, names unchanged: gngbtfw20.mbn (232436 bytes, the ROM 0x20 rampatch, an
// ELF), gngbtnv20.bin (13399 bytes) and 25 board-specific gngbtnv20.bXX (13399 bytes; b2e and b3e 13291). btqca's names:
// qca/gngbtfw%02x.mbn by ROM version, qca/gngbtnv%02x.b%02x by board id (else .bin).
constexpr const char *const kFirmwarePartitions[] = {"bluetooth_a", "bluetooth_b"};
constexpr const char *const kFirmwareDirs[] = {"image"};
constexpr uint32_t kStockRampatchBytes = 232436, kStockNvmBytes = 13399;
constexpr uint8_t kStockRom = 0x20;
// The largest file level 4 holds in memory at once (the rampatch is read whole once, then sent from memory).
constexpr uint32_t kFirmwareMaxBytes = 512 * 1024, kNvmMaxBytes = 16 * 1024;

// ---- the unit's Bluetooth address (private: never in this repository, never logged) ----
// The locally built ADT carries it when the builder was given one (tooling/adt/make-piano-adt.py --bt-address-file, by default
// private/bluetooth-address when that file exists): /arm-io/piano-bt, property bt-address, six bytes in the order it is written
// (most significant first). No driver matches that node; level 4 reads the property at run time, writes it with the vendor
// command and checks it by Read BD_ADDR. Without it the controller keeps its NVM's default address (reported as such).
constexpr const char *kAddressNode = "/arm-io/piano-bt";
constexpr const char *kAddressProperty = "bt-address";

// ---- RPMh for the rails (PianoWcnRails.h): the AOP command database and apps DRV-2, as PianoWifi's WifiHw.h has them ----
constexpr uint64_t kCmdDb = 0x81c60000, kCmdDbBytes = 0x20000;
constexpr uint64_t kRscDrv2 = 0x16520000, kRscDrv2Bytes = 0x10000;
constexpr uint32_t kRscTcsBlock = 0xd00;

}  // namespace pianobt
