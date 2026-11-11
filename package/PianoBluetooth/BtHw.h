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
//           bits, the SE6 RCG and its frequency table, parent map 5
//   RCG   = drivers/clk/qcom/clk-rcg2.c: the CMD/CFG/M/N layout and the DFS registers of a QUP RCG
//   PINS  = drivers/pinctrl/qcom/pinctrl-sm8750.c: gpio24..27 function 1 = qup2_se6; gpio18 function 1 = wcn_sw_ctrl
//   PGPIO = drivers/pinctrl/qcom/pinctrl-spmi-gpio.c: the register layout of a PMIC GPIO (LV/MV subtypes)
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

}  // namespace pianobt
