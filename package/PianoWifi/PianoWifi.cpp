// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi: the Qualcomm WCN7860 Wi-Fi of piano (SM8750, PCIe0) for macOS 27, native at EL1. Design, ladder and test plan:
// docs/drivers/wifi.md. Facts and their sources: WifiHw.h. This is level 1 only:
//   piano-wifi=1   a read-only survey of what the boot stages left of the Wi-Fi path, logged as "PianoWifi: ..." lines: the PCIe0 and
//                  PHY power domains, block resets, branch clocks and their votes (GCC), the PHY's reference clock gate (TCSR), the
//                  WLAN_EN / SW_CTRL / PERST# / CLKREQ# / WAKE# pins (TLMM), the controller (PARF, DBI, iATU) and the PHY (PCS) only
//                  when they are powered and clocked, the SMMU stream-match entries of streams 0x1400/0x1401, the RPMh resources
//                  of the PMU's and the PHY's supplies (the AOP command database, memory). Nothing is written: every MMIO access of
//                  this level is a load (WifiSurvey.h has no store at all); the endpoint's configuration space is never read.
//   piano-wifi-spmi=1  adds the supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI arbiter's observer channel:
//                  the only stores are the observer channel's read commands (as PianoPower's and PianoInput's read-only levels),
//                  never a PMIC register. Off by default, so piano-wifi=1 alone stores nothing at all.
//   piano-wifi=2   power: the six PMU rails by RPMh (mode NPM, the voltage piano's DT asks for, enable) with the PMIC read back
//                  before and after, PERST# low, CLKREQ# to its PCIe function, WLAN_EN low 150 ms then high, the PMU's 50 ms,
//                  SW_CTRL read. The PHY's two LDOs (one of them the UFS PHY's PLL) are read, never voted.
//   piano-wifi=3   the link: the GDSCs, the RCG sources, the branch votes (under PianoA830's GCC vote lock), the TCSR reference
//                  gate, the block resets, the PARF set-up, the QMP v7 PHY sequence, the BDF-to-SID entry of the endpoint, the root
//                  port, PERST# released, the LTSSM, L0, the configuration window, `17cb:110e`, the memory window, BAR0, and the
//                  SMMU stream 0x1401 through a bypass context bank.
//   piano-wifi=4   MHI: the DMA window, the MHI registers in BAR0, `image/peach/amss20.bin` read at run time from the tablet's own
//                  modem_a, BHI (the boot loader), the MHI contexts, BHIe (the rest of the image), mission mode.
//   piano-wifi=5   QMI: the chip's two IPC router channels over MHI, the router itself, and the WLAN firmware's QMI service — the
//                  radio's and the chip's capability, the indication registration, the host capability, the firmware's memory
//                  regions answered out of the same window, the regulatory database and the China-region board file in 6 KiB
//                  segments, the PHY and auxiliary microcodes by address, the trace configuration, and the firmware's ready
//                  indication. Every file is read at run time from the tablet's own modem_a.
//   piano-wifi=6   the station's first half: the copy engine's rings over BAR0 (the pipes level 5 described to the firmware),
//                  the host transport's handshake (its ready message, the firmware's message interface connected, set-up
//                  complete), that interface's service-ready and ready events, the host's initialisation with this chip's
//                  resource configuration, a station virtual device, and then a **passive scan**: the networks it finds are
//                  logged (the address masked unless piano-wifi-bssid=1) and published as the property piano-wifi-scan, which
//                  PianoWifiEthernet's en1 and tooling/native/piano-wifi read. The scan is stopped and the virtual device
//                  deleted before the level ends. No association, no keys and no data path: those are levels 7 and 8.
//                  piano-wifi=6 implies the mode message of level 5 (the firmware's radio must be running), so it switches
//                  piano-wifi-mode on by itself.
// Levels are cumulative and each one stops the ladder where it fails, leaving WLAN_EN low and PERST# asserted. Other arguments:
// piano-wifi-delay=S (wait S seconds first, default 0), piano-wifi-repeat=S with piano-wifi-samples=N (resample the MMIO part every
// S s, N times, default 3, one summary line each), piano-wifi-hwcg=1 (count a hardware-gated, voted branch under HW_CTL that reads
// CLK_OFF as running), piano-wifi-smmu=0 / piano-wifi-cmddb=0 (skip those reads), piano-wifi-gen=N (the target link speed, default
// 2), piano-wifi-physeq=0 (only the PHY sequence's frame writes), piano-wifi-rcg=0 (leave the aux/rchng sources alone),
// piano-wifi-blind=1 (vote a rail whose PMIC state could not be read), piano-wifi-release=1 (on a failure withdraw the enable of
// the rails this driver enabled), piano-wifi-stop=N (return after step N), piano-wifi-dma=M (the DMA window in MiB, default 24),
// piano-wifi-fwtimeout=MS (the deadline of each MHI phase, default 20000), piano-wifi-qmitimeout=MS (the deadline of each level 5
// step, default 10000), piano-wifi-iova=1 (give the endpoint's stream a translating context bank whose 32-bit window maps the DMA
// window, instead of the bypass bank of level 3: the only way to hand this chip a 32-bit address, since piano's XNU memory is all
// above 4 GiB), piano-wifi-mode=1 (level 5 also sends the firmware's options, the host's copy-engine configuration and the mode,
// which is level 6's doorway), piano-wifi-shadow=1 (the shadow register list in that configuration),
// piano-wifi-wmitimeout=MS (the deadline of each level 6 step, default 10000), piano-wifi-scanwait=MS (how long the scan itself
// may take, default 12000), piano-wifi-l6pre=N (the radio's set-up between the firmware's ready event and level 6's virtual
// device, default 0x3f = 63: bit 0 the hardware mode, 1 the radio's six parameters, 2 the channel list of its regulatory event,
// 3 a device address that is never all zeros, 4 wait for that event first, 5 pace the commands apart, 6 no scan at all — docs
// §10.27) with piano-wifi-l6regwait/l6pace/l6hold (2000, 50, 8000 ms),
// piano-wifi-band=N (1 both bands, 0 only 2.4 GHz), piano-wifi-bssid=1 (print a network's whole
// address in the log instead of its first three bytes), piano-wifi-noc=0 (no PCIe NoC vote), and level 4's four switches of the
// 2026-11-12 fourth round (WifiPbl.h, docs §10.23), each the A/B of a default: piano-wifi-socreset=0 (no chip power-on reset before
// BHI), piano-wifi-msi=0 (MSI left off), piano-wifi-ctxearly=1 (the MHI context registers before the doorbell too),
// piano-wifi-ras=0 (no TLP counters). After wifi-4a (2026-11-12: mission mode at 477 ms, then the firmware's crash dump
// environment 2 s later while one console line held this thread; docs §10.24), three more, each the A/B of a default:
// piano-wifi-rddm=0 (no crash dump: by default the dump's segments are given to the chip before the boot, as the MHI host does,
// and read back and decoded when the firmware dies), piano-wifi-defer=0 (every line printed when it happens: by default the lines
// from the boot loader's doorbell to the end of the ladder are held in memory and printed afterwards, so that the console never
// stalls the host while the firmware waits for it; since the sixth round a helper thread prints them at every phase change and
// within a second), piano-wifi-wake=0 (no device wake doorbell at mission mode), and piano-wifi-deadline=MS (the hard deadline of
// each of levels 4, 5 and 6, default 60000, docs §10.25). Without piano-wifi the driver declines in probe() and maps nothing.
//
// Power (package/include/PianoPowerContract.h): the radio is off unless piano-wifi>=2, since nothing below that level votes a
// rail or releases PERST#. From level 2 on the driver joins the power tree, and its only power state change is to stop the
// radio on a suspend: the scan is stopped, the virtual device deleted, the copy engine's rings disabled, WLAN_EN taken low and
// PERST# asserted. The rails stay voted (Bluetooth shares them) and nothing is brought back on the wake: the Wi-Fi path is off
// until the next boot, which is what an attended bring-up level should do to the battery. The stop waits for the ladder's own
// thread instead of racing it, so setPowerState answers with a deadline and acknowledges when it is done.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "SmmuStream.h"
#include "WifiBringup.h"
#include "WifiChannels.h"
#include "WifiFirmware.h"
#include "WifiLink.h"
#include "WifiMhi.h"
#include "WifiPbl.h"
#include "WifiRddm.h"
#include "WifiSmmuWindow.h"
#include "WifiSurvey.h"
#include "WifiWlfw.h"
#define PIANO_WCN_RAILS_KERNEL
#include <PianoWcnRails.h>   // the PMU's six rails, shared with PianoBluetooth (docs/drivers/bluetooth.md section 7)

using namespace pianowifi;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_wifi_stage = 0;
}

namespace {

struct Args {
  uint32_t level, spmi, smmu, cmddb, hwcg, delay, repeat, samples;
  uint32_t gen, physeq, rcg, blind, release, stop, dmaMib, fwTimeoutMs;
  uint32_t iova, mode, shadow, qmiTimeoutMs;
  uint32_t wmiTimeoutMs, scanWaitMs, band, bssid;
  uint32_t l6pre, l6regwait, l6pace, l6hold;
  uint32_t noc, socreset, msi, ctxearly, ras;
  uint32_t rddm, defer, wake;
  uint32_t deadlineMs;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}

void readArgs()
{
  gArgs.level = bootArg("piano-wifi", 0);
  gArgs.spmi = bootArg("piano-wifi-spmi", 0);
  gArgs.smmu = bootArg("piano-wifi-smmu", 1);
  gArgs.cmddb = bootArg("piano-wifi-cmddb", 1);
  gArgs.hwcg = bootArg("piano-wifi-hwcg", 0);
  gArgs.delay = bootArg("piano-wifi-delay", 0);
  if (gArgs.delay > 600) gArgs.delay = 600;
  gArgs.repeat = bootArg("piano-wifi-repeat", 0);
  if (gArgs.repeat > 3600) gArgs.repeat = 3600;
  gArgs.samples = bootArg("piano-wifi-samples", 3);
  if (gArgs.samples > 100) gArgs.samples = 100;
  gArgs.gen = bootArg("piano-wifi-gen", kDefaultGen);
  if (!gArgs.gen || gArgs.gen > kMaxGen) gArgs.gen = kDefaultGen;
  gArgs.physeq = bootArg("piano-wifi-physeq", 1);
  gArgs.rcg = bootArg("piano-wifi-rcg", 1);
  gArgs.blind = bootArg("piano-wifi-blind", 0);
  gArgs.release = bootArg("piano-wifi-release", 0);
  gArgs.stop = bootArg("piano-wifi-stop", 0);
  // Level 5 needs the firmware's regions, the two microcodes and the router's buffers in the same window as level 4's image.
  const uint32_t dmaDefault = (gArgs.level >= 5 ? kDmaWindowBytesL5 : kDmaWindowBytes) >> 20;
  gArgs.dmaMib = bootArg("piano-wifi-dma", dmaDefault);
  if (gArgs.dmaMib < 12 || gArgs.dmaMib > 96) gArgs.dmaMib = dmaDefault;
  gArgs.dmaMib &= ~1u;   // whole 2 MiB blocks: the window's own mapping (piano-wifi-iova) maps it in blocks of that size
  gArgs.fwTimeoutMs = bootArg("piano-wifi-fwtimeout", 20000);
  if (gArgs.fwTimeoutMs < 1000 || gArgs.fwTimeoutMs > 60000) gArgs.fwTimeoutMs = 20000;
  gArgs.iova = bootArg("piano-wifi-iova", 0);
  // Level 6 needs the firmware's radio running, which is what level 5's last three messages do.
  gArgs.mode = bootArg("piano-wifi-mode", gArgs.level >= 6 ? 1 : 0);
  gArgs.shadow = bootArg("piano-wifi-shadow", 0);
  gArgs.qmiTimeoutMs = bootArg("piano-wifi-qmitimeout", kWlfwTimeoutMs);
  if (gArgs.qmiTimeoutMs < 1000 || gArgs.qmiTimeoutMs > 60000) gArgs.qmiTimeoutMs = kWlfwTimeoutMs;
  gArgs.wmiTimeoutMs = bootArg("piano-wifi-wmitimeout", kWmiTimeoutMs);
  if (gArgs.wmiTimeoutMs < 1000 || gArgs.wmiTimeoutMs > 60000) gArgs.wmiTimeoutMs = kWmiTimeoutMs;
  gArgs.scanWaitMs = bootArg("piano-wifi-scanwait", kScanWaitMsDefault);
  if (gArgs.scanWaitMs < 1000 || gArgs.scanWaitMs > 60000) gArgs.scanWaitMs = kScanWaitMsDefault;
  gArgs.band = bootArg("piano-wifi-band", 1);
  gArgs.bssid = bootArg("piano-wifi-bssid", 0);
  // piano-wifi-l6pre=N: which of the radio's set-up steps level 6 does between the firmware's ready event and its virtual
  // device (P22, docs section 10.27). Bit 0 the hardware mode, 1 the radio's six parameters, 2 the channel list, 3 a usable
  // virtual-device address, 4 wait for the regulatory event first, 5 pace the commands apart, 6 no scan (hold after the
  // virtual device instead, so that a death cannot be blamed on the scan). Default: everything but the last.
  gArgs.l6pre = bootArg("piano-wifi-l6pre", kPreDefault);
  gArgs.l6regwait = bootArg("piano-wifi-l6regwait", kPreRegWaitMsDefault);
  gArgs.l6pace = bootArg("piano-wifi-l6pace", kPrePaceMsDefault);
  gArgs.l6hold = bootArg("piano-wifi-l6hold", kPreHoldMsDefault);
  // piano-wifi-noc=0: no vote for the PCIe NoC's bus clock manager (SN4) before level 3's clocks (the A/B test of docs §10.21)
  gArgs.noc = bootArg("piano-wifi-noc", 1);
  // The fourth round's alignments with the chip's own host (docs §10.23, WifiPbl.h), each on by default and each with its A/B:
  // piano-wifi-socreset=0: no chip-side power-on reset before BHI (the whole recipe of ath12k's power-up and its BHIe vector words;
  //   since docs §10.24 the MHI host's own receive-vector clear runs either way, as the crash dump's table follows it);
  gArgs.socreset = bootArg("piano-wifi-socreset", 1);
  // piano-wifi-msi=0: the endpoint's MSI left off (by default one vector into a sink page of the DMA window, as the host boots it);
  gArgs.msi = bootArg("piano-wifi-msi", 1);
  // piano-wifi-ctxearly=1: the MHI context registers written before the doorbell as well (the 2026-11-12 boots did; the MHI host
  //   writes them only once the device reports READY, which level 4 does in any case);
  gArgs.ctxearly = bootArg("piano-wifi-ctxearly", 0);
  // piano-wifi-ras=0: no TLP counters on the two ends of the link (diagnostic only)
  gArgs.ras = bootArg("piano-wifi-ras", 1);
  // After wifi-4a (docs §10.24), each on by default with its A/B:
  // piano-wifi-rddm=0: no crash dump segments for the chip (WifiRddm.h);
  gArgs.rddm = bootArg("piano-wifi-rddm", 1);
  // piano-wifi-defer=0: every line printed at once, even while the firmware starts (see note() below); =2: held, and printed only
  //   when the ladder stops, with no helper thread (the fifth round's way);
  gArgs.defer = bootArg("piano-wifi-defer", 1);
  // piano-wifi-wake=0: no device wake doorbell at mission mode.
  gArgs.wake = bootArg("piano-wifi-wake", 1);
  // piano-wifi-deadline=MS: the hard deadline of each of levels 4, 5 and 6 as a whole (default 60000; level 6 at least its own
  // overall bound and 5 s more). Every wait inside a level has its own shorter bound; this one ends the level whatever it waits for,
  // and the log's helper thread reports a ladder that is past it without coming back (the desk-dev8n boot of 2026-11-12 held its
  // lines from the doorbell on and printed nothing for 30 minutes).
  gArgs.deadlineMs = bootArg("piano-wifi-deadline", 60000);
  if (gArgs.deadlineMs < 10000 || gArgs.deadlineMs > 600000) gArgs.deadlineMs = 60000;
}

uint64_t nowMs();

// ---- the log, and why part of it waits (wifi-4a, 2026-11-12) ----
// IOLog goes to the console, and the console can hold the calling thread for seconds while the rest of the boot prints (wifi-4a:
// the line after "changed +477 ms" took 2027 ms to come out, while the root volume mounted and launchd started; the firmware
// reached mission mode at 477 ms and was in its crash dump environment when the next register read came, at 2503 ms). The host is
// the only one who can answer the firmware, so from the boot loader's doorbell to the end of the ladder this thread's lines are
// kept in a heap buffer, each with how long after the doorbell it was written, and never printed by this thread while it runs.
// A helper thread prints them (deferPrinter): at every phase change (a stage, a level 4 phase, a level 5 or 6 step), and any line
// held longer than kDeferAgeMs, so the console's stalls hold the helper and never the ladder, and a ladder that hangs still has
// its lines out within a second (desk-dev8n, 2026-11-12: the lines were printed only at the ladder's end, the ladder never ended,
// and nothing came for 30 minutes). The same helper watches each level's hard deadline (piano-wifi-deadline) and says so when the
// ladder is past it. Other threads (the power hook) print at once. piano-wifi-defer=0 prints everything as it happens.
// 480 lines: level 5 now has a line per answer (the board file alone is 21 segments) before level 6's lines.
constexpr unsigned kDeferLines = 480, kDeferBytes = 300;   // 144 000 bytes of heap while the ladder runs
constexpr unsigned kDeferTickMs = 50, kDeferAgeMs = 1000, kWatchGraceMs = 2000, kWatchRepeatMs = 30000, kWatchLines = 20;
char *gDeferBuf;
unsigned gDeferCount, gDeferDropped;   // gDeferCount: lines written (the ladder's thread only, published with a release store)
unsigned gDeferPrinted;                // lines printed (claimed one at a time, so the helper and the last flush never repeat one)
uint64_t gDeferSince;
thread_t gDeferThread;
uint32_t gDeferPhase;                  // bumped at every phase change: the helper prints what is held
uint32_t gPrinterRun, gPrinterExited;  // the helper runs / has returned (atomics)
// The current level's hard deadline (ms since boot; 0 none), for the ladder's own checks and the helper's watch.
uint64_t gLevelDeadline;
uint32_t gLevelNow;

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (gDeferBuf && current_thread() == gDeferThread) {
    const unsigned n = gDeferCount;
    if (n < kDeferLines) {
      snprintf(gDeferBuf + n * kDeferBytes, kDeferBytes, "%s [+%llu ms]", line, (unsigned long long)(nowMs() - gDeferSince));
      __atomic_store_n(&gDeferCount, n + 1, __ATOMIC_RELEASE);   // the helper reads the line only after this
    } else {
      ++gDeferDropped;
    }
    return;
  }
  IOLog("PianoWifi: %s\n", line);
}

// Prints held lines from `buffer` that nobody has printed yet, each claimed first; returns how many this call printed.
unsigned deferPrintHeld(const char *buffer)
{
  unsigned printed = 0;
  for (;;) {
    unsigned at = __atomic_load_n(&gDeferPrinted, __ATOMIC_ACQUIRE);
    const unsigned count = __atomic_load_n(&gDeferCount, __ATOMIC_ACQUIRE);
    if (at >= count) return printed;
    if (!__atomic_compare_exchange_n(&gDeferPrinted, &at, at + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
    IOLog("PianoWifi: %s\n", buffer + at * kDeferBytes);
    ++printed;
  }
}

// The helper: held lines out at every phase change and when one has waited kDeferAgeMs; the level's hard deadline watched.
void deferPrinter(void *arg, wait_result_t)
{
  const char *buffer = static_cast<const char *>(arg);
  uint32_t phase = __atomic_load_n(&gDeferPhase, __ATOMIC_ACQUIRE);
  uint64_t pendingSince = 0, lastWatch = 0;
  unsigned watches = 0;
  while (__atomic_load_n(&gPrinterRun, __ATOMIC_ACQUIRE)) {
    IOSleep(lastWatch ? 1000 : kDeferTickMs);   // a ladder past its deadline is only watched, once a second
    const uint64_t now = nowMs();
    const uint32_t nowPhase = __atomic_load_n(&gDeferPhase, __ATOMIC_ACQUIRE);
    const bool pending = __atomic_load_n(&gDeferPrinted, __ATOMIC_ACQUIRE) < __atomic_load_n(&gDeferCount, __ATOMIC_ACQUIRE);
    if (!pending) pendingSince = 0;
    else if (!pendingSince) pendingSince = now;
    if (pending && (nowPhase != phase || now - pendingSince >= kDeferAgeMs)) {
      phase = nowPhase;
      deferPrintHeld(buffer);
      pendingSince = 0;
    }
    const uint64_t deadline = __atomic_load_n(&gLevelDeadline, __ATOMIC_ACQUIRE);
    const bool due = !lastWatch || now - lastWatch >= kWatchRepeatMs;
    if (deadline && now > deadline + kWatchGraceMs && watches < kWatchLines && due) {
      lastWatch = now;
      ++watches;
      deferPrintHeld(buffer);
      IOLog("PianoWifi: watchdog: level %u is %llu ms past its hard deadline (piano-wifi-deadline) and the ladder's thread has "
            "not come back; last stage %d, %u line(s) held so far (all printed above)\n",
            __atomic_load_n(&gLevelNow, __ATOMIC_ACQUIRE), (unsigned long long)(now - deadline), piano_wifi_stage,
            __atomic_load_n(&gDeferCount, __ATOMIC_ACQUIRE));
    }
  }
  __atomic_store_n(&gPrinterExited, 1u, __ATOMIC_RELEASE);
}

void deferStart()
{
  if (!gArgs.defer || gDeferBuf) return;
  char *buffer = static_cast<char *>(IOMallocData(kDeferLines * kDeferBytes));
  if (!buffer) {
    note("log: no buffer to hold the lines while the firmware starts: they are printed as they come");
    return;
  }
  gDeferCount = gDeferDropped = gDeferPrinted = 0;
  gDeferSince = nowMs();
  gDeferThread = current_thread();
  __atomic_store_n(&gPrinterRun, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&gPrinterExited, 0u, __ATOMIC_RELEASE);
  // piano-wifi-defer=2: no helper (the fifth round's way: everything at the ladder's end, and no watch of the deadlines), the
  // A/B should the helper's console traffic ever be suspected of disturbing the firmware's start.
  thread_t printer = nullptr;
  const bool helper = gArgs.defer != 2 && kernel_thread_start(&deferPrinter, buffer, &printer) == KERN_SUCCESS;
  if (helper) thread_deallocate(printer);
  else __atomic_store_n(&gPrinterExited, 1u, __ATOMIC_RELEASE);
  note("log: from here until the ladder stops, this thread's lines are held with their time since now and printed by %s "
       "(piano-wifi-defer=0 prints them at once)",
       helper              ? "a helper thread at every phase change and within a second"
       : gArgs.defer == 2 ? "this thread when the ladder stops (piano-wifi-defer=2: no helper thread, no deadline watch)"
                          : "this thread when the ladder stops (no helper thread)");
  gDeferBuf = buffer;
}

void deferFlush()
{
  char *buffer = gDeferBuf;
  if (!buffer) return;
  gDeferBuf = nullptr;   // from here note() prints again
  __atomic_store_n(&gPrinterRun, 0u, __ATOMIC_RELEASE);
  // The helper may be inside IOLog (the console stalls it, not us); what it has not claimed yet is printed here.
  const uint64_t until = nowMs() + 3000;
  while (!__atomic_load_n(&gPrinterExited, __ATOMIC_ACQUIRE) && nowMs() < until) IOSleep(kDeferTickMs);
  const unsigned before = __atomic_load_n(&gDeferPrinted, __ATOMIC_ACQUIRE);
  const unsigned here = deferPrintHeld(buffer);
  IOLog("PianoWifi: log: %u line(s) held for %llu ms, %u printed by the helper thread and %u now%s\n", gDeferCount,
        (unsigned long long)(nowMs() - gDeferSince), before, here,
        __atomic_load_n(&gPrinterExited, __ATOMIC_ACQUIRE) ? "" : " (the helper is still inside the console: its buffer is kept)");
  if (gDeferDropped) IOLog("PianoWifi: log: %u later line(s) did not fit and were not kept\n", gDeferDropped);
  if (__atomic_load_n(&gPrinterExited, __ATOMIC_ACQUIRE)) IOFreeData(buffer, kDeferLines * kDeferBytes);
}

// A phase change: the helper prints what is held (a stage, a level 4 phase, a level 5 or 6 step).
void deferPhase() { __atomic_add_fetch(&gDeferPhase, 1u, __ATOMIC_ACQ_REL); }

void stage(int32_t value, const char *what)
{
  piano_wifi_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
  deferPhase();
}

// Each of levels 4, 5 and 6 begins with its hard deadline (piano-wifi-deadline); every wait of the level ends once it is passed
// (levelLate), and the helper thread reports a ladder that does not come back from it.
void levelBegin(uint32_t level, uint32_t ms)
{
  __atomic_store_n(&gLevelNow, level, __ATOMIC_RELEASE);
  __atomic_store_n(&gLevelDeadline, nowMs() + ms, __ATOMIC_RELEASE);
}
void levelEnd() { __atomic_store_n(&gLevelDeadline, uint64_t(0), __ATOMIC_RELEASE); }
bool levelLate()
{
  const uint64_t deadline = __atomic_load_n(&gLevelDeadline, __ATOMIC_ACQUIRE);
  return deadline && nowMs() > deadline;
}

// Explicit ldr, as PianoDwc3 and PianoPower do. The only store helper of this file is spmiWr (the observer channel's command).
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

// ------------------------------------------------------------------------------------------------ MMIO pages, mapped on demand
// The survey decides which addresses it loads (WifiSurvey.h, host-tested); a page is mapped the first time one of its words is
// loaded, so a block the gates keep the survey off is not even mapped.
struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 64;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
uint64_t gLoads;

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

uint32_t load(uint64_t pa)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return 0xffffffffu;
  }
  ++gLoads;
  return rd32(va + uintptr_t(pa & 0xfff));
}

void unmapAll()
{
  for (unsigned i = 0; i < gPageCount; ++i) gPages[i].map->release();
  gPageCount = 0;
}

// ------------------------------------------------------------------------------------------------------------- SPMI (observer)
// The arbiter's windows (SpmiObserver.h): the core and owner tables read through load(); EE 0's observer channels through one
// mapping of the whole window, made before the first command (spmiObserverMapped) and kept for the kext's life: every observer
// command is a PianoSpmi.h transaction under the SPMI lock every kext shares, with interrupts masked, where nothing may map a page
// (load()'s pages are mapped on first use). The one kind of store, the observer channel's read command, is refused for any other
// window or offset. The counts change inside the lock's hold.
unsigned gSpmiStores, gSpmiRefused;
IOMemoryMap *gObsvMap;
uintptr_t gObsvVa;
bool spmiObserverMapped()
{
  if (__atomic_load_n(&gObsvVa, __ATOMIC_ACQUIRE)) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kSpmiObsv, kSpmiObsvBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    ++gMapFailures;
    return false;
  }
  IOMemoryMap *expected = nullptr;
  if (!__atomic_compare_exchange_n(&gObsvMap, &expected, map, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    map->release();   // another thread's mapping won: wait for its address
    while (!__atomic_load_n(&gObsvVa, __ATOMIC_ACQUIRE)) IODelay(1);
    return true;
  }
  __atomic_store_n(&gObsvVa, uintptr_t(map->getVirtualAddress()), __ATOMIC_RELEASE);
  return true;
}
uint32_t spmiRd(void *, Window w, uint32_t off)
{
  if (w == Window::obsv) {
    const uintptr_t va = __atomic_load_n(&gObsvVa, __ATOMIC_ACQUIRE);
    if (!va || off >= kSpmiObsvBytes || (off & 3)) return 0xffffffffu;   // a status of "done, failed": the read ends at once
    ++gLoads;
    return rd32(va + off);
  }
  return load((w == Window::core ? kSpmiCore : kSpmiCnfg) + off);
}
void spmiWr(void *, Window w, uint32_t off, uint32_t value)
{
  if (w != Window::obsv || off % kObsvStride != kObsvCmd || off >= kSpmiObsvBytes || (value >> 27) != kOpExtReadL) {
    ++gSpmiRefused;
    return;
  }
  const uintptr_t va = __atomic_load_n(&gObsvVa, __ATOMIC_ACQUIRE);
  if (!va) {
    ++gSpmiRefused;
    return;
  }
  ++gSpmiStores;
  __asm__ volatile("str %w0, [%1]" ::"r"(value), "r"(va + uintptr_t(off)) : "memory");
}
// The SPMI lock's counts (PianoSpmi.h), one line a report.
void reportSpmiLock(const char *when)
{
  char line[360];
  pianospmi::format(line, sizeof line);
  note("%s: %s", when, line);
}
void spmiDelay(void *, unsigned us) { IODelay(us); }
const Io gSpmi = {nullptr, spmiRd, spmiWr, spmiDelay};
// The arbiter as the survey read it, kept for levels 2 and up (one regulator read at a time through the same channel).
Arbiter gArbiter;
bool gArbiterReady;

// ------------------------------------------------------------------------------------------------------------- the report
const char *onOff(bool on) { return on ? "on" : "off"; }

void reportClocks(const ClockState &c)
{
  note("GCC pcie0 gdsc %08x cfg %08x (%s) phy gdsc %08x cfg %08x (%s) collapse vote %08x (pcie0 %s, phy %s)", c.gdsc, c.gdscCfg,
       onOff(gdscOn(c.gdsc)), c.phyGdsc, c.phyGdscCfg, onOff(gdscOn(c.phyGdsc)), c.collapse,
       (c.collapse & kCollapsePcie0) ? "collapsed" : "released", (c.collapse & kCollapsePcie0Phy) ? "collapsed" : "released");
  char line[360];
  size_t n = 0;
  for (unsigned i = 0; i < kResetCount && n < sizeof line; ++i)
    n += size_t(snprintf(line + n, sizeof line - n, "%s%s %08x%s", i ? ", " : "", kResets[i].name, c.bcr[i],
                         resetAsserted(c.bcr[i]) ? " (asserted)" : ""));
  note("GCC resets: %s", line);
  note("GCC votes 0x52008 %08x 0x52000 %08x", c.vote8, c.vote0);
  n = 0;
  for (unsigned i = 0; i < kBranchCount && n < sizeof line; ++i)
    n += size_t(snprintf(line + n, sizeof line - n, "%s%s %08x %s%s%s", i ? ", " : "", kBranches[i].name, c.cbcr[i],
                         branchOff(c.cbcr[i]) ? "off" : "on", (c.voteMask >> i) & 1 ? " voted" : "",
                         (c.idleMask >> i) & 1 ? " hw-idle" : ""));
  note("GCC branches: %s", line);
  note("GCC aux rcg %08x/%08x rchng rcg %08x/%08x pipe mux %08x (%s); TCSR pcie0 clkref %08x (%s)", c.auxRcg, c.auxCfg, c.rchngRcg,
       c.rchngCfg, c.pipeMux, (c.pipeMux & 3) == 0 ? "PHY" : (c.pipeMux & 3) == 2 ? "TCXO" : "?", c.clkref,
       (c.clkref & 1) ? "enabled" : "disabled");
}

void reportPins(const Survey &s)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const PinDecode d = decodePin(s.pins[i].cfg, s.pins[i].io);
    note("pin %s gpio%u: cfg %08x io %08x: function %u pull %s %u mA output %s, out %u in %u", kPins[i].name, kPins[i].gpio,
         s.pins[i].cfg, s.pins[i].io, d.function, pullName(d.pull), d.driveMa, d.outputEnable ? "enabled" : "disabled", d.out, d.in);
  }
}

void reportController(const ControllerState &r)
{
  if (!r.parfRead) {
    note("controller not read (%s): PARF, DBI and iATU not touched", r.skipped ? r.skipped : "?");
    return;
  }
  note("PARF sys %08x pm %08x pm_stts %08x phy_ctrl %08x ltssm %08x (enable %u, state %#x) int_all %08x status %08x", r.sysCtrl,
       r.pmCtrl, r.pmStts, r.phyCtrl, r.ltssm, (r.ltssm & kParfLtssmEnable) ? 1u : 0u, r.ltssm & kLtssmMask, r.intAll, r.status);
  note("PARF dbi base %08x%08x slave space %08x%08x atu base %08x%08x device type %08x bdf-to-sid %08x (%s)", r.dbiBaseHi,
       r.dbiBase, r.slvSpaceHi, r.slvSpace, r.atuBaseHi, r.atuBase, r.deviceType, r.bdfToSid, (r.bdfToSid & 1) ? "bypass" : "table");
  if (!r.dbiRead) {
    note("DBI not read (%s)", r.skipped ? r.skipped : "?");
    return;
  }
  note("DBI id %08x command %08x class %08x buses %08x; port debug0 %08x (LTSSM %#x %s) debug1 %08x (link %s)", r.id, r.command,
       r.classRev, r.busNumbers, r.debug0, r.debug0 & kLtssmMask, ltssmName(r.debug0), r.debug1,
       (r.debug1 & kPortDebug1LinkUp) ? "up" : "down");
  note("DBI pcie cap at %#x: link cap %08x control/status %08x (Gen%u x%u)", r.expCap, r.linkCap, r.linkCtlSta, linkSpeed(r),
       linkWidth(r));
  for (unsigned i = 0; i < 2; ++i)
    note("iATU outbound %u: ctrl1 %08x ctrl2 %08x (%s) base %08x limit %08x target %08x", i, r.atuCtrl1[i], r.atuCtrl2[i],
         (r.atuCtrl2[i] & kAtuCtrl2Enable) ? "enabled" : "disabled", r.atuBase0[i], r.atuLimit[i], r.atuTarget[i]);
}

void reportPhy(const PhyState &p)
{
  if (!p.read) {
    note("PHY not read (%s)", p.skipped ? p.skipped : "?");
    return;
  }
  note("PHY pcs sw_reset %08x status1 %08x (%s) power_down %08x start %08x", p.swReset, p.status1,
       (p.status1 & kPcsPhyStatus) ? "not ready" : "ready", p.powerDown, p.start);
}

void reportSmmu()
{
  const uint16_t streams[2] = {kStreamRoot, kStreamEndpoint};
  const SmmuStreams s = surveySmmu(streams, kSmmuBytes, [](uint32_t off) { return load(kSmmu + off); });
  note("SMMU idr0 %08x idr1 %08x: %u stream-match groups, %u banks, %u pages%s%s", s.idr0, s.idr1, s.groups, s.banks, s.pages,
       s.error ? ": " : "", s.error ? s.error : "");
  for (unsigned k = 0; k < 2 && !s.error; ++k) {
    if (!s.count[k]) note("SMMU stream %#x: no stream-match entry (the firmware left none)", streams[k]);
    for (unsigned i = 0; i < s.count[k]; ++i) {
      const SmmuMatch &m = s.found[k][i];
      note("SMMU stream %#x: group %u smr %08x s2cr %08x (%s, bank %u, sctlr %08x)", streams[k], m.group, m.smr, m.s2cr,
           s2crType(m.s2cr), m.s2cr & 0xffu, m.sctlr);
    }
  }
}

// What levels 2 and up need from level 1: each rail's RPMh address (cmd-db) and the PPID its regulator answered on (the SPMI
// survey, which also settles the LDO page rule). 0 means "not found", and a rail without both is never voted.
uint32_t gRailAddress[kRailCount];
uint16_t gRailPpid[kRailCount];
// The PCIe NoC's bus clock manager (WifiHw.h, kNocBcmName): its RPMh address and aux data from cmd-db, and whether our vote stands.
struct NocBcm {
  uint32_t address = 0, vote = 0;
  BcmAux aux;
  int rc = -2;
  bool voted = false;
};
NocBcm gNoc;

void reportCmdDb()
{
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbBytes));
  if (!copy) {
    note("cmd-db: no memory for its copy");
    return;
  }
  // memory, not MMIO: one mapping of the whole region (PianoKeyboard's way), copied out word by word and released
  IODeviceMemory *m = IODeviceMemory::withRange(kCmdDb, kCmdDbBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    IOFreeData(copy, kCmdDbBytes);
    note("cmd-db: cannot map %#llx", (unsigned long long)kCmdDb);
    return;
  }
  const uintptr_t va = uintptr_t(map->getVirtualAddress());
  for (uint32_t i = 0; i < kCmdDbBytes; i += 4) {
    const uint32_t w = rd32(va + i);
    memcpy(copy + i, &w, 4);
  }
  map->release();
  char line[360];
  size_t n = 0;
  static const char *const kExtra[] = {"rfclka1", "SN4"};   // the RF reference clock of mainline's MTP, the PCIe NoC BCM (PAGANI)
  for (unsigned i = 0; i < kRailCount + 2 && n < sizeof line; ++i) {
    const char *name = i < kRailCount ? kRails[i].cmdDb : kExtra[i - kRailCount];
    uint32_t address = 0;
    const int rc = cmdDbFind(copy, kCmdDbBytes, name, &address);
    if (i < kRailCount) gRailAddress[i] = rc ? 0 : address;
    n += size_t(rc ? snprintf(line + n, sizeof line - n, "%s%s none (%d)", i ? ", " : "", name, rc)
                   : snprintf(line + n, sizeof line - n, "%s%s %#x", i ? ", " : "", name, address));
  }
  gNoc.rc = cmdDbBcm(copy, kCmdDbBytes, kNocBcmName, &gNoc.address, gNoc.aux);
  IOFreeData(copy, kCmdDbBytes);
  note("cmd-db: %s", line);
  if (!gNoc.rc)
    note("cmd-db: %s (the PCIe NoC's bus clock manager) %#x: unit %u, width %u, clock domain %u", kNocBcmName, gNoc.address,
         gNoc.aux.unit, gNoc.aux.width, gNoc.aux.domain);
  else
    note("cmd-db: %s (the PCIe NoC's bus clock manager): %s (%d)", kNocBcmName,
         gNoc.rc == -4 ? "no aux data (unit, width): a vote would be the smallest one" : "not found", gNoc.rc);
}

// Also keeps the arbiter for levels 2 and up (hwPmic reads one regulator through the same observer channel).
// A rail with no regulator although the arbiter maps one of its pages: its reads failed, or its identity read as another kind (the
// 01:19 probe, v34p: ldof3's TYPE 0x00 and smpi7's 0x04 with an "ok" status), so the survey found no regulator for that rail.
static bool railUnread(const PmicSurvey &s)
{
  for (unsigned i = 0; i < kRailCount; ++i) {
    const RailState &st = s.rails[i];
    if (st.reg()) continue;
    if ((st.primary.present && st.primary.result != ReadResult::ok) || (st.alternate.present && st.alternate.result != ReadResult::ok) ||
        st.unexpected())
      return true;
  }
  return false;
}

void reportPmic()
{
  PmicSurvey s;
  if (!spmiObserverMapped()) {
    note("SPMI: the observer window %#llx does not map: no regulator read", (unsigned long long)kSpmiObsv);
    return;
  }
  surveyPmic(gSpmi, s);
  // The 23:03 and 23:18 probes (charging level 2, the DSP started at once, PianoThermal's ADC rounds): a regulator's reads kept
  // failing for longer than spmiRead's retries, and level 2 then had no PPID for that rail (step -201); at 01:19 two identities read
  // as the wrong kind with an "ok" status. The survey again, 100 ms apart, for up to 3 s while any mapped regulator stays unread
  // or of another kind than its rail's.
  unsigned again = 0;
  while (again < 30 && s.arbiter.v7 && railUnread(s)) {
    IOSleep(100);
    ++again;
    s = PmicSurvey();
    surveyPmic(gSpmi, s);
  }
  if (again)
    note("SPMI: the regulator survey taken %u more time(s), 100 ms apart: %s", again,
         railUnread(s) ? "a mapped regulator is still unreadable or of another kind" : "every mapped regulator read");
  gArbiter = s.arbiter;
  gArbiterReady = s.arbiter.v7;
  if (!s.arbiter.v7) {
    note("SPMI: arbiter version %08x, %u peripherals: not v7, not read", s.arbiter.version, s.arbiter.count);
    return;
  }
  for (unsigned i = 0; i < kRailCount; ++i) {
    const Rail &r = kRails[i];
    const RailState &st = s.rails[i];
    const PmicPeripheral *p = st.reg();
    if (!p) {
      note("rail %s %s (%s): no regulator found (ppid %03x %s type %02x read %s%s)", r.supply, r.regulator, r.cmdDb, st.primary.ppid,
           st.primary.present ? "present" : "absent", st.primary.type, resultName(st.primary.result),
           r.ldo ? (st.alternate.present ? ", alternate present" : ", alternate absent") : "");
      continue;
    }
    gRailPpid[i] = p->ppid;
    note("rail %s %s (%s): ppid %03x apid %d owner %u type %02x/%02x status %02x (%s) %u mV mode %02x en %02x (%s), DT %u mV%s",
         r.supply, r.regulator, r.cmdDb, p->ppid, p->apid, p->owner, p->type, p->subtype, p->status, p->ready() ? "ready" : "not ready",
         p->millivolts(), p->regs40[5], p->regs40[6], p->enabled() ? "enabled" : "disabled", r.millivolts,
         st.chosen == 1 ? " (SM8550 page rule)" : "");
  }
  for (unsigned i = 0; i < kPmicPinCount; ++i) {
    const PmicPeripheral &p = s.pins[i];
    if (!p.read) {
      note("pmic pin %s: ppid %03x %s", kPmicPins[i].name, p.ppid, p.present ? "not readable" : "absent");
      continue;
    }
    note("pmic pin %s: ppid %03x type %02x/%02x status %02x mode %02x vin %02x pull %02x src %02x out %02x en %02x", kPmicPins[i].name,
         p.ppid, p.type, p.subtype, p.status, p.regs40[0], p.regs40[1], p.regs40[2], p.regs40[4], p.regs40[5], p.regs40[6]);
  }
  note("SPMI: %u observer read commands, %u refused stores; TYPE reads: %u tried again for a regulator of the expected kind, %u "
       "read two different values", s.commands, gSpmiRefused, s.typeRetries, s.typeUnstable);
  reportSpmiLock("SPMI");
}

// One line for ioreg and for the repeated samples.
void summary(const Survey &s, char *line, size_t size)
{
  const ClockState &c = s.clocks;
  const PinDecode en = decodePin(s.pins[kPinWlanEn].cfg, s.pins[kPinWlanEn].io),
                  perst = decodePin(s.pins[kPinPerst].cfg, s.pins[kPinPerst].io),
                  sw = decodePin(s.pins[kPinSwCtrl].cfg, s.pins[kPinSwCtrl].io);
  snprintf(line, size, "pcie0 %s phy %s branches-off %03x wlan_en %u/%u perst %u/%u sw_ctrl %u link %s ltssm %#x phystatus %s",
           onOff(gdscOn(c.gdsc)), onOff(gdscOn(c.phyGdsc)), c.offMask, en.out, en.in, perst.out, perst.in, sw.in,
           s.controller.dbiRead ? (linkUp(s.controller) ? "up" : "down") : "unread",
           s.controller.parfRead ? s.controller.ltssm & kLtssmMask : 0xffu,
           s.phy.read ? ((s.phy.status1 & kPcsPhyStatus) ? "not-ready" : "ready") : "unread");
}

// ================================================================================= levels 2 and up: the windows and the writes
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Levels above 1 write, so each block is mapped as one window (level 1's page-at-a-time loads stay for the survey). hwRead and
// hwWrite refuse an offset outside the window: an offset that came from the hardware (all ones while the endpoint does not answer)
// must never reach Device memory unaligned or out of range, which is a panic.
struct WindowMap {
  uint64_t pa;
  uint32_t bytes;
  const char *name;
  IOMemoryMap *map;
  uintptr_t va;
};
WindowMap gWin[kWinCount] = {
  {kGcc, 0x80000, "gcc", nullptr, 0},
  {kTlmm, 0x70000, "tlmm", nullptr, 0},
  {kTcsrPcie0ClkRef & ~0xfffull, 0x1000, "tcsr", nullptr, 0},
  {kParf, uint32_t(kParfBytes), "parf", nullptr, 0},
  {kPhy, uint32_t(kPhyBytes), "phy", nullptr, 0},
  {kDbi, uint32_t(kDbiBytes), "dbi", nullptr, 0},
  {kAtu, uint32_t(kAtuBytes), "atu", nullptr, 0},
  {kConfig, 0x1000, "cfg", nullptr, 0},
};

bool mapWindow(WindowMap &w)
{
  if (w.va) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(w.pa, w.bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  w.map = map;
  w.va = uintptr_t(map->getVirtualAddress());
  return true;
}

bool mapWindows()
{
  for (unsigned i = 0; i < kWinCount; ++i)
    if (!mapWindow(gWin[i])) {
      note("cannot map the %s window (%#llx + %#x)", gWin[i].name, (unsigned long long)gWin[i].pa, gWin[i].bytes);
      return false;
    }
  return true;
}

void unmapWindows()
{
  for (unsigned i = 0; i < kWinCount; ++i)
    if (gWin[i].map) {
      gWin[i].map->release();
      gWin[i].map = nullptr;
      gWin[i].va = 0;
    }
}

// Every write is logged once with what it is and why; a repeat of the same kind (the 126 PHY sequence writes, the 256 cleared
// BDF-to-SID words) is counted instead of logged again.
unsigned gWrites, gBadAccess;
const char *gSeen[64];
unsigned gSeenCount, gSeenRepeats[64];

uint32_t hwRead(void *, unsigned win, uint32_t off)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > gWin[win].bytes) {
    ++gBadAccess;
    return 0xffffffffu;
  }
  return rd32(gWin[win].va + off);
}

void hwWrite(void *, unsigned win, uint32_t off, uint32_t value, const char *what)
{
  if (win >= kWinCount || !gWin[win].va || (off & 3) || off + 4 > gWin[win].bytes) {
    ++gBadAccess;
    note("refused a write outside the %s window: +%#x = %08x (%s)", win < kWinCount ? gWin[win].name : "?", off, value, what);
    return;
  }
  wr32(gWin[win].va + off, value);
  dsb();
  ++gWrites;
  for (unsigned i = 0; i < gSeenCount; ++i)
    if (gSeen[i] == what) {
      ++gSeenRepeats[i];
      return;
    }
  if (gSeenCount < 64) {
    gSeen[gSeenCount] = what;
    gSeenRepeats[gSeenCount++] = 0;
  }
  note("write %s+%#x = %08x: %s", gWin[win].name, off, value, what);
}

// The writes of the level just finished (the counts are deltas, so each level's line stands on its own).
unsigned gWritesBefore, gKindsBefore;
void writeSummary(const char *level)
{
  char line[360];
  size_t n = 0;
  for (unsigned i = gKindsBefore; i < gSeenCount && n + 40 < sizeof line; ++i)
    if (gSeenRepeats[i]) n += size_t(snprintf(line + n, sizeof line - n, "%s%s x%u", n ? ", " : "", gSeen[i], gSeenRepeats[i] + 1));
  note("%s: %u writes (%u kinds, %u refused accesses in all)%s%s", level, gWrites - gWritesBefore, gSeenCount - gKindsBefore,
       gBadAccess, n ? "; repeated: " : "", n ? line : "");
  gWritesBefore = gWrites;
  gKindsBefore = gSeenCount;
}

void hwUdelay(void *, unsigned us) { IODelay(us); }
void hwMsleep(void *, unsigned ms) { IOSleep(ms); }
uint64_t hwNow(void *) { return nowMs(); }
void hwStep(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
}

// ---- PianoA830's lock words (package/PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h and Sm8750GpuPower.c): one 32-bit word
// each, 0 = free, else the holder's tag. PianoA830 is listed before PianoWifi in configs/k27-native-kexts.txt and is named as a
// library in PianoWifi.json, so the KC linker (no weak imports) resolves both to A830's single copy, the way PianoPower,
// PianoKeyboard (DRV-2) and PianoTouch (the GCC vote words) do.
extern "C" {
extern volatile uint32_t sm8750_gcc_vote_owner;
extern volatile uint32_t sm8750_rpmh_drv2_owner;
}
constexpr uint32_t kTag = 0x57494649;   // "WIFI"
constexpr unsigned kLockSteps = 200000, kLockStepUs = 10;

bool lockAcquire(volatile uint32_t *word, uint32_t *waits, unsigned steps)
{
  for (uint32_t n = 0;; ++n) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, kTag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *waits = n;
      return true;
    }
    if (n >= steps) {
      *waits = n;
      return false;
    }
    IODelay(kLockStepUs);
  }
}
void lockRelease(volatile uint32_t *word)
{
  uint32_t expected = kTag;
  __atomic_compare_exchange_n(word, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// The GCC vote words and the collapse word are read-modify-written under A830's lock word (PianoTouch's protocol: 10 us steps, at
// most 50 ms; a holder that keeps it longer than that is reported and the update goes ahead, as the bits are only ever set).
int hwVoteLock(void *, int take)
{
  if (!take) {
    lockRelease(&sm8750_gcc_vote_owner);
    return 0;
  }
  uint32_t waits = 0;
  if (lockAcquire(&sm8750_gcc_vote_owner, &waits, 5000)) {
    if (waits) note("GCC vote lock taken after %u waits of %u us", waits, kLockStepUs);
    return 0;
  }
  note("GCC vote lock held by %#x for 50 ms: the votes are set anyway (bits are only ever set)",
       __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
  return 0;
}

// ---- one RPMh transaction on apps DRV-2, under the DRV-2 lock (the protocol of PianoA830's Sm8750Rpmh.h, as PianoPower and
// PianoKeyboard run it): an idle ACTIVE TCS, its commands, the trigger, completion polled for at most 100 ms, the TCS reclaimed.
// A TCS that did not complete is left alone and never used again.
// Nothing under the lock sleeps or logs (P17, wifi-l5ib 2026-11-12: one of our rail transactions, which slept 1 ms per poll and
// printed its line before letting the lock go, spanned PianoA830's whole 2 s wait for it, and the GPU's bring-up failed with
// "the DRV-2 lock was not free"): completion is polled every 10 us against a 100 ms clock, the reclaim clears WAIT_FOR_CMPL with
// the rest (PianoA830 takes a TCS only when it reads 0), and the line comes after the release, with how long the lock was held.
struct RscLayout {
  uint32_t tcsStride, cmdStride, irqStatus, irqClear, waitForCmpl, control, status, cmdEnable, cmdMsgid, cmdAddr, cmdData;
};
constexpr RscLayout rscLayoutFor(uint32_t drvId)
{
  return ((drvId >> 16) & 0xff) >= 3 ? RscLayout{672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c}
                                     : RscLayout{672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
}
constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24, kMsgidWriteResp = 8u | 1u << 8 | 1u << 16,
                   kCmdCompleted = 1u << 16;
IOMemoryMap *gRscMap;
uintptr_t gRscVa;
RscLayout gRsc;
uint32_t gRscId;
bool gTcsStuck[3];

uint32_t rscRd(uint32_t at) { return rd32(gRscVa + kRscTcsBlock + at); }
void rscWr(uint32_t at, uint32_t v)
{
  wr32(gRscVa + kRscTcsBlock + at, v);
  dsb();
}

bool mapRsc()
{
  if (gRscVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kRscDrv2, kRscDrv2Bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    note("rsc: cannot map apps DRV-2 (%#llx)", (unsigned long long)kRscDrv2);
    return false;
  }
  gRscMap = map;
  gRscVa = uintptr_t(map->getVirtualAddress());
  gRscId = rd32(gRscVa);
  gRsc = rscLayoutFor(gRscId);
  note("rsc: apps DRV-2 id %#x (version %u.%u: %s layout), TCS block +%#x", gRscId, (gRscId >> 16) & 0xff, (gRscId >> 8) & 0xff,
       ((gRscId >> 16) & 0xff) >= 3 ? "v3" : "v2", kRscTcsBlock);
  return true;
}

struct TcsOutcome {
  int tcs = -1;
  bool done = false;
  uint32_t waitedUs = 0, status[3] = {0, 0, 0};
};
constexpr unsigned kTcsPollUs = 10, kTcsDeadlineUs = 100000;

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

// The caller holds the DRV-2 lock; nothing here sleeps or logs.
int tcsLocked(uint32_t base, const VrmCmd *cmds, unsigned count, TcsOutcome &out)
{
  const RscLayout &l = gRsc;
  static const unsigned kOrder[3] = {2, 1, 0};
  out = TcsOutcome();
  for (unsigned k = 0; k < 3 && out.tcs < 0; ++k)
    if (!gTcsStuck[kOrder[k]] && rscRd(l.tcsStride * kOrder[k] + l.status)) out.tcs = int(kOrder[k]);
  if (out.tcs < 0) return -16;
  const uint32_t at = l.tcsStride * unsigned(out.tcs), bit = 1u << out.tcs, irqEnable = rscRd(0), mask = (1u << count) - 1;
  if (rscRd(l.irqStatus) & bit) rscWr(l.irqClear, bit);
  for (unsigned c = 0; c < count; ++c) {
    const uint32_t cmd = at + l.cmdStride * c;
    rscWr(cmd + l.cmdMsgid, kMsgidWriteResp);
    rscWr(cmd + l.cmdAddr, base + cmds[c].reg);
    rscWr(cmd + l.cmdData, cmds[c].value);
  }
  rscWr(at + l.waitForCmpl, mask);
  rscWr(at + l.cmdEnable, mask);
  rscWr(0, irqEnable | bit);
  const uint32_t control = rscRd(at + l.control);
  rscWr(at + l.control, control & ~kAmcTrigger);
  rscWr(at + l.control, control & ~(kAmcTrigger | kAmcEnable));
  rscWr(at + l.control, kAmcEnable);
  rscWr(at + l.control, kAmcEnable | kAmcTrigger);
  const uint64_t start = nowUs();
  for (;;) {
    uint32_t all = kCmdCompleted;
    for (unsigned c = 0; c < count; ++c) {
      out.status[c] = rscRd(at + l.cmdStride * c + l.cmdData + 4);
      all &= out.status[c];
    }
    out.done = (rscRd(l.irqStatus) & bit) || all == kCmdCompleted;
    const uint64_t spent = nowUs() - start;
    out.waitedUs = spent > 0xffffffffu ? 0xffffffffu : uint32_t(spent);
    if (out.done || spent >= kTcsDeadlineUs) break;
    IODelay(kTcsPollUs);
  }
  if (out.done) {
    rscWr(at + l.cmdEnable, 0);
    rscWr(at + l.waitForCmpl, 0);
    rscWr(l.irqClear, bit);
    rscWr(at + l.control, rscRd(at + l.control) & ~kAmcTrigger);
    rscWr(at + l.control, 0);
  } else {
    gTcsStuck[out.tcs] = true;
  }
  rscWr(0, irqEnable);
  return out.done ? 0 : -60;
}

// One transaction of `count` commands of the resource at `base`, the DRV-2 lock around it and the line after it.
int rpmhSend(const char *resource, uint32_t base, const VrmCmd *cmds, unsigned count)
{
  if (!gRscVa || !base || !cmds || !count || count > 3) return -22;
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("rsc: the DRV-2 lock is held by %#x; waiting (at most 2 s)", held);
  if (!lockAcquire(&sm8750_rpmh_drv2_owner, &waits, kLockSteps)) {
    note("rsc: the DRV-2 lock stayed held by %#x for 2 s: %s not voted",
         __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE), resource);
    return -16;
  }
  const uint64_t taken = nowUs();
  TcsOutcome out;
  const int rc = tcsLocked(base, cmds, count, out);
  const uint64_t heldUs = nowUs() - taken;
  lockRelease(&sm8750_rpmh_drv2_owner);
  if (out.tcs < 0) {
    note("rsc: %s (%#x): no idle ACTIVE TCS, nothing sent (the DRV-2 lock held %llu us)", resource, base,
         (unsigned long long)heldUs);
    return rc;
  }
  note("rsc: %s (%#x): %u command(s) through TCS %d %s after %u us (status %#x %#x %#x); the DRV-2 lock held %llu us, taken "
       "after %u waits",
       resource, base, count, out.tcs, out.done ? "completed" : "NOT completed (left in flight, not used again)", out.waitedUs,
       out.status[0], out.status[1], out.status[2], (unsigned long long)heldUs, waits);
  return rc;
}

int hwVrm(void *, const char *resource, const VrmCmd *cmds, unsigned count, uint32_t *address)
{
  if (!gRscVa || !cmds || !count || count > 3) return -22;
  uint32_t base = 0;
  for (unsigned i = 0; i < kRailCount; ++i) {
    const char *a = kRails[i].cmdDb, *b = resource;
    unsigned k = 0;
    while (k < 8 && a[k] && a[k] == b[k]) ++k;
    if (k == 8 || a[k] == b[k]) base = gRailAddress[i];   // equal, or equal in the eight characters RPMh names have
  }
  if (address) *address = base;
  if (!base) {
    note("rsc: %s: no RPMh resource of that name in cmd-db, nothing sent", resource);
    return -19;
  }
  return rpmhSend(resource, base, cmds, count);
}

// The PCIe NoC's bus clock manager (WifiHw.h): voted before level 3's clocks, as pcie-qcom votes its "pcie-mem" path, at the
// peak of the link level 3 trains (Gen x 2 lanes; average 0, as pcie-qcom asks); withdrawn (commit, not valid) wherever the chip
// is given up. A cmd-db entry without aux data gets the smallest vote that keeps the NoC on.
void nocVote(bool on)
{
  if (!gArgs.noc || (!on && !gNoc.voted)) return;
  if (!gNoc.address || (gNoc.rc && gNoc.rc != -4)) {
    if (on) note("PCIe NoC: %s is not in cmd-db (%d): not voted; the chip's DMA may not reach memory", kNocBcmName, gNoc.rc);
    return;
  }
  const uint64_t kbps = on ? uint64_t(kPcieLanes) * pcieLaneKbps(gArgs.gen) : 0;
  uint32_t peak = on ? bcmVote(kbps, gNoc.aux.unit, gNoc.aux.width, kNocBusWidth) : 0;
  const bool smallest = on && !peak;
  if (smallest) peak = 1;
  const VrmCmd cmd = {0, bcmCommand(0, peak)};
  const int rc = rpmhSend(kNocBcmName, gNoc.address, &cmd, 1);
  note("PCIe NoC: %s %s: peak %llu kB/s (Gen%u x%u) -> vote %u%s (unit %u, width %u, bus width %u), command %08x: %s (%d)",
       kNocBcmName, on ? "voted" : "withdrawn", (unsigned long long)kbps, gArgs.gen, kPcieLanes, peak,
       smallest ? " (the smallest: no aux data)" : "", gNoc.aux.unit, gNoc.aux.width, kNocBusWidth, cmd.value,
       rc ? "NOT taken" : "taken", rc);
  if (!rc) {
    gNoc.voted = on;
    gNoc.vote = peak;
  }
}

// ---- one regulator over the SPMI observer channel (reads only, the same channel level 1 uses)
int hwPmic(void *, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts)
{
  if (!gArbiterReady || !ppid || !spmiObserverMapped()) return -19;
  const Apid found = spmiFindApid(gSpmi, gArbiter, ppid);
  if (found.apid < 0) return -19;
  uint8_t regs[7] = {};
  // As reportPmic: reads that keep failing through spmiRead's retries are tried again 50 ms apart, for up to 1 s.
  bool ok = false;
  for (unsigned attempt = 0; attempt < 20 && !ok; ++attempt) {
    if (attempt) IOSleep(50);
    ok = spmiRead(gSpmi, found.apid, 0x08, status, 1) == ReadResult::ok && spmiRead(gSpmi, found.apid, 0x40, regs, 7) == ReadResult::ok;
  }
  if (!ok) return -5;
  if (millivolts) *millivolts = uint16_t(uint16_t(regs[0]) | uint16_t(regs[1]) << 8);
  if (mode) *mode = regs[5];
  if (enable) *enable = regs[6];
  return 0;
}

const Hw gHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwVrm, hwPmic, hwStep};

// The rails helper both radios run (PianoWcnRails.h; its words are PianoA830's): this driver's own VRM transaction (hwVrm, under
// the DRV-2 lock) and regulator read (hwPmic, the observer channel); the PPIDs from level 1's survey. The PMU lock also keeps
// PianoBluetooth's SPMI reads of the same peripherals out of ours.
int wcnVrm(void *, const char *resource, const PianoWcnVrmCmd *cmds, unsigned count)
{
  VrmCmd c[3];
  if (!cmds || !count || count > 3) return -22;
  for (unsigned i = 0; i < count; ++i) c[i] = {cmds[i].reg, cmds[i].value};
  return hwVrm(nullptr, resource, c, count, nullptr);
}
PIANO_WCN_KERNEL_RAILS(gWcn, PianoWcnTagWifi, "PianoWifi", wcnVrm, hwPmic, nullptr);
// Wi-Fi no longer runs on the rails: its owner bit back (with piano-wifi-release=1 the last owner withdraws the helper's enables).
unsigned wcnPutOwners() { return piano_wcn_rails_put(&gWcn.rails, PIANO_WCN_WLAN | (gArgs.release ? unsigned(PIANO_WCN_RELEASE) : 0u)); }
void wcnPut() { (void)wcnPutOwners(); }
// A level from 3 up gave the chip up: the PCIe NoC's vote goes (PCIe0 is Wi-Fi's alone), the rails' owner bit with it.
void chipPut()
{
  nocVote(false);
  wcnPut();
}

Options options()
{
  Options o;
  o.level = gArgs.level;
  o.gen = gArgs.gen;
  o.physeq = gArgs.physeq;
  o.rcg = gArgs.rcg;
  o.blind = gArgs.blind;
  o.release = gArgs.release;
  o.stopAfter = int(gArgs.stop);
  return o;
}

// ------------------------------------------------------------------------------------------------------ the levels' reports
void reportPower(const PowerState &p)
{
  for (unsigned i = 0; i < kRailCount; ++i) {
    const Rail &rail = kRails[i];
    const RailVote &r = p.rails[i];
    if (!rail.order) {
      note("rail %s %s (%s): %u mV en %02x -> %u mV en %02x; never voted (%s)", rail.supply, rail.regulator, rail.cmdDb,
           r.millivolts, r.enable, r.millivoltsAfter, r.enableAfter, rail.shared);
      continue;
    }
    note("rail %s %s (%s) %#x: before %s %u mV mode %02x en %02x -> sent mode %u%s%s -> after %s %u mV mode %02x en %02x (rc %d)",
         rail.supply, rail.regulator, rail.cmdDb, r.address, r.read ? "read" : "unread", r.millivolts, r.mode, r.enable,
         kVrmModeNpm, r.voted ? ", voltage" : " (voltage kept)", r.sentEnable ? ", enable" : "",
         r.readAfter ? "read" : "unread", r.millivoltsAfter, r.modeAfter, r.enableAfter, r.rc);
  }
  note("pins: WLAN_EN cfg %08x io %08x, PERST# cfg %08x io %08x, CLKREQ# cfg %08x io %08x, SW_CTRL io %08x (in %u)", p.wlanEnCfg,
       p.wlanEnIo, p.perstCfg, p.perstIo, p.clkreqCfg, p.clkreqIo, p.swCtrlIo, p.swCtrlIo & 1u);
  note("level 2: %u rails with a voltage, %u left at theirs; step %d (%s)", p.voted, p.kept, p.step,
       p.error ? errorName(p.error) : "done");
}

void reportLink(const LinkState &l)
{
  note("GCC: collapse %08x -> %08x, pcie0 gdsc %08x cfg %08x, phy gdsc %08x cfg %08x after %u polls", l.collapseBefore,
       l.collapseAfter, l.gdsc, l.gdscCfg, l.phyGdsc, l.phyGdscCfg, l.gdscPolls);
  note("GCC: votes 0x52008 %08x 0x52000 %08x, aux rcg %08x/%08x rchng rcg %08x/%08x pipe mux %08x, clkref %08x, branches-off %03x",
       l.vote8, l.vote0, l.auxRcg, l.auxCfg, l.rchngRcg, l.rchngCfg, l.pipeMux, l.clkref, l.branchOff);
  note("GCC: resets pcie0 %08x pcie0_phy %08x; pipe cbcr %08x", l.bcr[0], l.bcr[1], l.pipeCbcr);
  note("PARF: device type %08x phy_ctrl %08x dbi base %08x atu base %08x slave space hi %08x sys %08x ltssm %08x", l.parfDeviceType,
       l.parfPhyCtrl, l.parfDbiBase, l.parfAtuBase, l.parfSlvSpaceHi, l.parfSysCtrl, l.parfLtssm);
  note("PHY: %u writes, PCS status1 %08x (%s) after %u polls of 100 us", l.phyWrites, l.pcsStatus,
       (l.pcsStatus & kPcsPhyStatus) ? "not ready" : "ready", l.phyPolls);
  note("PARF: BDF-to-SID hash of %#x is %u, entry %08x, cfg %08x (%s)", kEndpointRid, l.sidHash, l.sidEntry, l.sidCfg,
       (l.sidCfg & kParfBdfToSidBypass) ? "bypass" : "table");
  note("root port: id %08x buses %08x command %08x pcie cap %#x link ctl2 %08x; PERST# low %u ms", l.rootId, l.busNumbers,
       l.rootCommand, l.expCap, l.linkCtl2, l.perstLowMs);
  note("root port: windows as found: I/O %04x upper %08x, prefetchable %08x upper %08x:%08x; closed: I/O %04x, prefetchable %08x",
       l.ioBefore, l.ioUpperBefore, l.prefBefore, l.prefBaseHiBefore, l.prefLimitHiBefore, l.io, l.pref);
  note("link: debug0 %08x (LTSSM %#x %s) debug1 %08x (%s) status %08x (Gen%u x%u) after %u ms", l.debug0, l.debug0 & kLtssmMask,
       ltssmName(l.debug0), l.debug1, (l.debug1 & kPortDebug1LinkUp) ? "up" : "down", l.linkStatus, (l.linkStatus >> 16) & 0xfu,
       (l.linkStatus >> 20) & 0x3fu, l.linkMs);
  note("endpoint: id %08x (%s) class %08x subsystem %08x pcie cap %#x msi cap %#x after %u polls", l.endpointId,
       idIsEndpoint(l.endpointId) ? "17cb:110e, the WCN7860" : "not the WCN7860", l.endpointClass, l.endpointSubsys,
       l.endpointCap, l.endpointMsiCap, l.endpointPolls);
  note("BAR0: mask %08x -> %llu KiB at %#llx (value %08x), command %08x; iATU ctrl2 %08x/%08x, memory base/limit %08x", l.barMask,
       (unsigned long long)(l.barBytes / 1024), (unsigned long long)l.barAddress, l.barValue, l.command, l.atuCtrl2[0],
       l.atuCtrl2[1], l.memBaseLimit);
  note("level 3: step %d (%s)", l.step, l.error ? errorName(l.error) : "done");
}

// =========================================================================================== level 3's tail: the SMMU stream
// The endpoint's stream (0x1401) through a bypass context bank, exactly as PianoDwc3 gives stream 0x40 and NativeUfs the UFS one
// (SmmuStream.h): a free group and a free bank, nothing of an existing UFS / GPU / USB entry touched, and an entry the firmware
// already left that bypasses is kept as it is. piano-wifi-smmu=2 does the root port's stream (0x1400) as well; 0 skips both.
IOMemoryMap *gSmmuMap;
uintptr_t gSmmuVa;
unsigned gEndpointBank = ~0u, gSmmuPages = 0;   // stream 0x1401's bypass bank (level 3's tail), for the fault report

uint32_t smmuRead(uint32_t off) { return rd32(gSmmuVa + off); }

// Mapped on demand, by level 3's tail and by level 5's window (which needs it even with piano-wifi-smmu=0).
bool mapSmmu()
{
  if (gSmmuVa) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kSmmu, kSmmuBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    note("SMMU: cannot map %#llx", (unsigned long long)kSmmu);
    return false;
  }
  gSmmuMap = map;
  gSmmuVa = uintptr_t(map->getVirtualAddress());
  return true;
}

bool smmuStream(uint16_t stream)
{
  if (!mapSmmu()) return false;
  const SmmuPlan p = planSmmuStream(stream, kSmmuBytes, smmuRead);
  note("SMMU stream %#x: groups %u banks %u pages %u; %s slot %u (smr %08x s2cr %08x sctlr %08x cbar %08x cba2r %08x) -> %s bank %u%s%s",
       stream, p.groups, p.banks, p.pages, p.reuse ? "existing entry" : "free", p.slot, p.smr, p.s2cr, p.sctlr, p.cbar, p.cba2r,
       p.keep ? "keep" : "bypass", p.bank, p.error ? ": " : "", p.error ? p.error : "");
  if (p.error) return false;
  const uint64_t deadline = nowMs() + 100;
  const char *failure = applySmmuStream(p, stream, smmuRead, [](uint32_t off, uint32_t v) { wr32(gSmmuVa + off, v); dsb(); },
                                        [deadline] { return nowMs() >= deadline; });
  note("SMMU stream %#x: %s (slot %u: smr %08x s2cr %08x)", stream, failure ? failure : p.keep ? "left as it was" : "bypass set",
       p.slot, smmuRead(0x800 + 4 * p.slot), smmuRead(0xc00 + 4 * p.slot));
  if (!failure && stream == kStreamEndpoint) {
    gEndpointBank = p.bank;
    gSmmuPages = p.pages;
  }
  return failure == nullptr;
}

// ================================================================================================== level 4: DMA, BAR0, MHI
// The DMA window: one physically contiguous range below 2^36 (what ath12k and the 13T's cnss2 both allow this chip), the cacheable
// alias cleaned out of the caches once and then only ever reached through a write-combined mapping, as PianoDwc3's gadget memory
// is: the endpoint's stream bypasses the SMMU, so a bus address is a physical address and nothing keeps the two views coherent.
IOBufferMemoryDescriptor *gDmaBuffer;
IOMemoryMap *gDmaMap;
Arena gArena;
uint64_t gDmaPa;

void cleanToPoc(const void *start, size_t length)
{
  uint64_t ctr = 0;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
  for (uintptr_t a = uintptr_t(start) & ~(line - 1); a < uintptr_t(start) + length; a += line)
    __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  dsb();
}

bool dmaWindow()
{
  if (gArena.va) return true;
  const uint64_t bytes = uint64_t(gArgs.dmaMib) << 20;
  IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), bytes,
      0x0000000fffe00000ull);   // 36 bits, 2 MiB aligned
  if (!buffer) {
    note("dma: no %llu MiB contiguous below 2^36", (unsigned long long)gArgs.dmaMib);
    return false;
  }
  if (buffer->prepare() != kIOReturnSuccess) {
    buffer->release();
    note("dma: the window could not be prepared");
    return false;
  }
  IOByteCount contiguous = 0;
  const uint64_t pa = buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  uint8_t *cached = static_cast<uint8_t *>(buffer->getBytesNoCopy());
  if (!pa || !cached || contiguous < bytes || pa + bytes > (1ull << 36)) {
    buffer->complete();
    buffer->release();
    note("dma: the window is not usable (pa %#llx contiguous %llu)", (unsigned long long)pa, (unsigned long long)contiguous);
    return false;
  }
  bzero(cached, size_t(bytes));
  cleanToPoc(cached, size_t(bytes));
  gDmaMap = buffer->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
  uint8_t *va = gDmaMap ? reinterpret_cast<uint8_t *>(gDmaMap->getVirtualAddress()) : nullptr;
  if (!va) {
    buffer->complete();
    buffer->release();
    note("dma: no write-combined mapping of the window");
    return false;
  }
  gDmaBuffer = buffer;
  gDmaPa = pa;
  // piano-wifi-iova=1: the chip's addresses are the 32-bit window's, which the SMMU translates to this range (iovaWindow()
  // below, called once stream 0x1401 has its bank); otherwise they are physical, which only works while the stream bypasses.
  arenaInit(gArena, va, gArgs.iova ? uint64_t(kIovaWindowBase) : pa, uint32_t(bytes));
  note("dma: %llu MiB at %#llx (write-combined mapping %p), the endpoint's addresses are %s",
       (unsigned long long)gArgs.dmaMib, (unsigned long long)pa, va,
       gArgs.iova ? "this window's 32-bit addresses (stream 0x1401 translates)" : "physical (stream 0x1401 bypasses)");
  return true;
}

// ---- the 32-bit window for the endpoint's stream (WifiSmmuWindow.h), with piano-wifi-iova=1 ----
// The page table is a buffer of its own, not part of the window: the chip can reach every byte of the window, and its own
// translation tables must not be among them. The table is cacheable and cleaned to the point of coherency once, as the SMMU's
// walker is not coherent with the CPU's caches (the bank walks non-cacheable).
IOBufferMemoryDescriptor *gIovaTable;
iova::Window gIovaWindow;
iova::Plan gIovaPlan;

bool iovaWindow()
{
  if (!mapSmmu()) return false;
  IOBufferMemoryDescriptor *table = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), iova::kTableBytes,
      0x0000000ffffff000ull);   // 36 bits, 4 KiB aligned
  if (!table || table->prepare() != kIOReturnSuccess) {
    if (table) table->release();
    note("iova: no page table of %llu bytes", (unsigned long long)iova::kTableBytes);
    return false;
  }
  IOByteCount contiguous = 0;
  const uint64_t tablePa = table->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  uint64_t *words = static_cast<uint64_t *>(table->getBytesNoCopy());
  if (!tablePa || !words || contiguous < iova::kTableBytes) {
    table->complete();
    table->release();
    note("iova: the page table is not usable (pa %#llx)", (unsigned long long)tablePa);
    return false;
  }
  if (!iova::buildWindow(gIovaWindow, words, words + 512, tablePa, gDmaPa, uint32_t(uint64_t(gArgs.dmaMib) << 20),
                         kIovaWindowBase)) {
    table->complete();
    table->release();
    note("iova: the window (%#x + %u MiB over %#llx) does not fit the rule", kIovaWindowBase, gArgs.dmaMib,
         (unsigned long long)gDmaPa);
    return false;
  }
  cleanToPoc(words, size_t(iova::kTableBytes));
  gIovaTable = table;
  gIovaPlan = iova::plan(kStreamEndpoint, kSmmuBytes, [](uint32_t off) { return smmuRead(off); });
  note("iova: window %#x + %u MiB -> %#llx in %u blocks of 2 MiB, table at %#llx; SMMU groups %u banks %u pages %u: %s slot %u "
       "bank %u (output %u bits)",
       gIovaWindow.base, gIovaWindow.bytes >> 20, (unsigned long long)gDmaPa, gIovaWindow.blocks,
       (unsigned long long)tablePa, gIovaPlan.groups, gIovaPlan.banks, gIovaPlan.pages,
       gIovaPlan.reuse ? "the stream's own" : "free", gIovaPlan.slot, gIovaPlan.bank, iova::kPaBits[gIovaPlan.pasize]);
  if (gIovaPlan.error) {
    note("iova: no context bank for stream %#x: %s", kStreamEndpoint, gIovaPlan.error);
    return false;
  }
  // The self-check before the chip is given any of these addresses: every block translates to where the window really is.
  for (uint32_t block = 0; block < gIovaWindow.blocks; ++block) {
    const uint64_t at = uint64_t(gIovaWindow.base) + uint64_t(block) * kIovaBlockBytes;
    if (iova::translate(gIovaWindow, at) == gDmaPa + uint64_t(block) * kIovaBlockBytes) continue;
    note("iova: the table does not translate %#llx: the window is not usable", (unsigned long long)at);
    return false;
  }
  const uint64_t deadline = nowMs() + 100;
  const char *failure = iova::apply(gIovaPlan, kStreamEndpoint, tablePa, [](uint32_t off) { return smmuRead(off); },
                                    [](uint32_t off, uint32_t v) { wr32(gSmmuVa + off, v); dsb(); },
                                    [deadline] { return nowMs() >= deadline; });
  if (failure) {
    note("iova: stream %#x did not take the window: %s", kStreamEndpoint, failure);
    // wifi-l5i (2026-11-12) stopped at the bank's readback: what each register holds against what was written.
    const uint32_t cb = gIovaPlan.cb;
    note("iova: bank %u read back: sctlr %08x (wrote %08x) cba2r %08x (1) cbar %08x (%08x) ttbr0 %08x:%08x (%08x:%08x) tcr %08x (%08x) "
         "tcr2 %08x fsr %08x",
         gIovaPlan.bank, smmuRead(cb + iova::kCbSctlr), iova::kSctlrWindow, smmuRead(iova::cba2rOffset(gIovaPlan.bank)),
         smmuRead(iova::cbarOffset(gIovaPlan.bank)), iova::kCbarStage1, smmuRead(cb + iova::kCbTtbr0Hi),
         smmuRead(cb + iova::kCbTtbr0Lo), iova::ttbr0Hi(tablePa, gIovaPlan.bank), uint32_t(tablePa), smmuRead(cb + iova::kCbTcr), iova::kTcrWindow,
         smmuRead(cb + iova::kCbTcr2), smmuRead(cb + iova::kCbFsr));
    wr32(gSmmuVa + cb + iova::kCbSctlr, 0);   // the bank off again: the stream never pointed at it
    dsb();
    return false;
  }
  note("iova: stream %#x translates through bank %u (smr %08x s2cr %08x sctlr %08x)", kStreamEndpoint, gIovaPlan.bank,
       smmuRead(0x800 + 4 * gIovaPlan.slot), smmuRead(0xc00 + 4 * gIovaPlan.slot),
       smmuRead(gIovaPlan.cb + iova::kCbSctlr));
  return true;
}

IOMemoryMap *gBarMap;
uintptr_t gBarVa;
uint32_t gBarBytes;

bool mapBar(uint64_t pa, uint32_t bytes)
{
  if (gBarVa) return true;
  if (!pa || !bytes || bytes > 0x400000) return false;
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return false;
  gBarMap = map;
  gBarVa = uintptr_t(map->getVirtualAddress());
  gBarBytes = bytes;
  return true;
}

uint32_t barRd(void *, uint32_t off)
{
  if ((off & 3) || off + 4 > gBarBytes) {
    ++gBadAccess;
    return 0xffffffffu;
  }
  return rd32(gBarVa + off);
}
void barWr(void *, uint32_t off, uint32_t value)
{
  if ((off & 3) || off + 4 > gBarBytes) {
    ++gBadAccess;
    return;
  }
  wr32(gBarVa + off, value);
  dsb();
}
const MhiIo gMhiIo = {nullptr, barRd, barWr};

// The chip's own registers above 0x80000 through its remap window; level 4 reads only the SoC version, for the log, and level 6
// reaches the copy engine's rings the same way. One thread owns the window (this driver polls and never takes an interrupt), so
// the two accesses of a windowed read or write are never interleaved with another's.
uint32_t chipRead(uint32_t address)
{
  if (address < kChipWindowStart) return barRd(nullptr, address);
  barWr(nullptr, kChipWindowReg, kChipWindowEnable | ((address >> kChipWindowShift) & kChipWindowMask));
  (void)barRd(nullptr, kChipWindowReg);
  return barRd(nullptr, kChipWindowStart + (address & kChipWindowRange));
}
void chipWrite(uint32_t address, uint32_t value)
{
  if (address < kChipWindowStart) {
    barWr(nullptr, address, value);
    return;
  }
  barWr(nullptr, kChipWindowReg, kChipWindowEnable | ((address >> kChipWindowShift) & kChipWindowMask));
  (void)barRd(nullptr, kChipWindowReg);
  barWr(nullptr, kChipWindowStart + (address & kChipWindowRange), value);
}
uint32_t chipIoRead(void *, uint32_t address) { return chipRead(address); }
void chipIoWrite(void *, uint32_t address, uint32_t value) { chipWrite(address, value); }
const ChipIo gChipIo = {nullptr, chipIoRead, chipIoWrite};

// ---- level 4's diagnostics (P16, 2026-11-12: the boot loader never took the image, BHI status ffffffff, error ef0e0003) ----
// The MHI register block as the chip has it, eight words a line: the core registers, BHI (the serial number at +0x40 masked: it is
// per unit) and BHIe. Reads only, of registers both MHI hosts read.
void dumpRegs(const char *when, const char *what, uint32_t base, uint32_t bytes, uint32_t masked)
{
  for (uint32_t off = 0; off < bytes; off += 32) {
    char line[120];
    size_t n = 0;
    for (uint32_t k = off; k < off + 32 && k < bytes && n + 10 < sizeof line; k += 4)
      n += size_t(k == masked ? snprintf(line + n, sizeof line - n, " --------")
                              : snprintf(line + n, sizeof line - n, " %08x", barRd(nullptr, base + k)));
    note("level 4: %s: %s +%02x:%s", when, what, off, line);
  }
}
void dumpMhi(const char *when, const MhiBoot &b)
{
  dumpRegs(when, "MHI", 0, kMhiCoreDumpBytes, ~0u);
  dumpRegs(when, "BHI", b.bhi, kBhiDumpBytes, kBhiSerialNumber);
  dumpRegs(when, "BHIe", b.bhie, kBhieDumpBytes, ~0u);
}

// What level 4's loop watches besides the state machine, one line whenever any of it changes (at most kSnapLines lines).
struct MhiSnap {
  uint32_t status = 0, ee = 0, bhi = 0, err = 0, bhie = 0;
  bool operator!=(const MhiSnap &o) const { return status != o.status || ee != o.ee || bhi != o.bhi || err != o.err || bhie != o.bhie; }
};
constexpr unsigned kSnapLines = 48;
MhiSnap mhiSnap(const MhiBoot &b)
{
  MhiSnap s;
  s.status = barRd(nullptr, kMhiStatus);
  s.ee = barRd(nullptr, b.bhi + kBhiExecEnv);
  s.bhi = barRd(nullptr, b.bhi + kBhiStatus);
  s.err = barRd(nullptr, b.bhi + kBhiErrCode);
  s.bhie = barRd(nullptr, b.bhie + kBhieTxVecStatus);
  return s;
}
void noteSnap(const char *when, uint64_t ms, const MhiSnap &s)
{
  note("level 4: %s +%llu ms: MHISTATUS %08x (state %#x%s%s), EE %08x, BHI status %08x, error code %08x, BHIe status %08x", when,
       (unsigned long long)ms, s.status, (s.status >> kMhiStateShift) & 0xffu, (s.status & kMhiStatusReady) ? ", ready" : "",
       (s.status & kMhiStatusSysErr) ? ", SYSERR" : "", s.ee, s.bhi, s.err, s.bhie);
}

// The chip's power-on reset before BHI (WifiPbl.h chipPowerOnReset: ath12k's whole power-up recipe), then the MHI host's clear of
// the receive vector block. chipRead/chipWrite write the window register before every access, so the window the global reset
// clears needs nothing restored here.
void chipPowerOn(MhiBoot &b)
{
  ChipReset r;
  chipPowerOnReset(gChipIo, [](unsigned ms) { IOSleep(ms); }, b.bhi, r);
  const unsigned rx = mhiClearRxVectors(b);
  note("level 4: chip power-on reset: chip LTSSM %08x -> %08x (%u writes), GCC hot reset %08x -> %08x, interrupts cleared, QFPROM "
       "power %08x -> %08x (VDD4BLOW %s), BHIe vector words cleared, Q6 cookie %08x",
       r.ltssmBefore, r.ltssm, r.ltssmWrites, r.hotResetBefore, r.hotReset, r.qfpromBefore, r.qfprom,
       (r.qfpromBefore & kChipQfpromVdd4Blow) ? (r.qfprom & kChipQfpromVdd4Blow ? "was on, still on" : "was on, now off")
                                              : "was off",
       r.cookie);
  note("level 4: chip power-on reset: warm entry %08x -> %08x, reset cause %08x, global reset %08x -> %08x%s, MHISTATUS %08x -> "
       "%08x after MHICTRL's reset, EE %08x; %u receive vector words cleared",
       r.warm, r.warmAfter, r.cause, r.globalBefore, r.globalAfter, r.linkLost ? " (the link went down)" : "", r.mhiStatusBefore,
       r.mhiStatusAfter, r.ee, rx);
}

// What the boot loader reports it read from the chip's fuses (WifiPbl.h bhiIdentity): counts and a digest, never the values.
void noteIdentity(const char *when, const MhiBoot &b)
{
  if (b.bhi + kBhiOemPkHash + 4 * kBhiOemPkHashWords > b.reglen) return;
  const BhiIdentity id = bhiIdentity([](uint32_t off) { return barRd(nullptr, off); }, b.bhi);
  note("level 4: BHI identity %s: MSM hardware ID %u of %u words non-zero, OEM public key hash %u of %u non-zero (%u all ones), "
       "digest %08x",
       when, id.hwIdNonZero, kBhiMsmHwIdWords, id.pkHashNonZero, kBhiOemPkHashWords, id.pkHashAllOnes, id.digest);
}

// ---- the fourth round's diagnostics (docs §10.23): the MSI sink, the TLP counters of both ends, the image as the chip reads it ----
// The sink: one page of the DMA window, filled with a pattern; the endpoint's MSI points at it (one vector, data kMsiSinkData),
// so any word that changed is a write the chip made into the window.
constexpr uint32_t kMsiSinkFill = 0xa5a5a5a5u;
constexpr uint16_t kMsiSinkData = 0x4d53;
uint8_t *gMsiSinkVa;
uint64_t gMsiSinkBus;
MsiSetup gMsi;
RasDes gRasRoot, gRasEndpoint;

uint32_t cfgRd(uint32_t off) { return hwRead(nullptr, kWinCfg, off); }
void msiWr(uint32_t off, uint32_t v) { hwWrite(nullptr, kWinCfg, off, v, "endpoint MSI capability / command"); }
void cfgWr(uint32_t off, uint32_t v) { hwWrite(nullptr, kWinCfg, off, v, "endpoint TLP counter control"); }
uint32_t dbiRd(uint32_t off) { return hwRead(nullptr, kWinDbi, off); }
void dbiWr(uint32_t off, uint32_t v) { hwWrite(nullptr, kWinDbi, off, v, "root port TLP counter control"); }

void msiSink(const LinkState &link)
{
  uint64_t bus = 0;
  uint8_t *va = arenaAlloc(gArena, 4096, &bus);
  if (!va) {
    note("level 4: msi: no room for the sink page");
    return;
  }
  for (uint32_t i = 0; i < 4096; i += 4) *reinterpret_cast<volatile uint32_t *>(va + i) = kMsiSinkFill;
  dsb();
  gMsiSinkVa = va;
  gMsiSinkBus = bus;
  const bool on = msiEnable(gMsi, cfgRd, msiWr, link.endpointMsiCap, bus, kMsiSinkData);
  note("level 4: msi: cap %#x control %04x -> %04x (%s addresses), address %08x:%08x data %04x, command %08x: %s%s",
       gMsi.cap, gMsi.ctlBefore, gMsi.ctl, gMsi.is64 ? "64-bit" : "32-bit", gMsi.addressHi, gMsi.addressLo, gMsi.data, gMsi.command,
       on ? "one vector into the sink page" : "not enabled: ", on ? "" : gMsi.why);
  if (on) note("level 4: msi: the sink page is at bus %#llx", (unsigned long long)bus);
}

// What the chip wrote into the sink: the first changed words (an MSI write is one word, the data in its low half).
void msiSinkReport(const char *when)
{
  if (!gMsiSinkVa) return;
  unsigned changed = 0, first = ~0u;
  uint32_t firstValue = 0;
  for (uint32_t i = 0; i < 4096; i += 4) {
    const uint32_t v = *reinterpret_cast<volatile uint32_t *>(gMsiSinkVa + i);
    if (v == kMsiSinkFill) continue;
    if (!changed++) {
      first = i;
      firstValue = v;
    }
  }
  if (!changed)
    note("dma: msi sink %s: untouched (no write from the chip reached the window%s)", when,
         gMsi.enabled ? "" : "; MSI was not enabled");
  else
    note("dma: msi sink %s: %u word(s) changed, the first at +%#x = %08x (%s): the chip's writes reach the window", when, changed,
         first, firstValue, (firstValue & 0xffffu) == kMsiSinkData ? "an MSI" : "not the MSI data");
}

void rasArm()
{
  if (!gArgs.ras) return;
  gRasRoot.at = rasDesFind(dbiRd);
  gRasEndpoint.at = rasDesFind(cfgRd);
  rasDesArm(gRasRoot, dbiRd, dbiWr);
  rasDesArm(gRasEndpoint, cfgRd, cfgWr);
  note("level 4: TLP counters: root port %s%#x (%u of %u on), endpoint %s%#x (%u of %u on)", gRasRoot.at ? "at " : "none ",
       gRasRoot.at, __builtin_popcount(gRasRoot.on), kRasEventCount, gRasEndpoint.at ? "at " : "none ", gRasEndpoint.at,
       __builtin_popcount(gRasEndpoint.on), kRasEventCount);
}

void rasLine(const char *who, const RasDes &r)
{
  if (!r.at) return;
  char line[300];
  size_t n = 0;
  for (unsigned i = 0; i < kRasEventCount && n + 24 < sizeof line; ++i)
    n += size_t(((r.on >> i) & 1u) ? snprintf(line + n, sizeof line - n, " %s %u", kRasEvents[i].name, r.delta[i])
                                   : snprintf(line + n, sizeof line - n, " %s -", kRasEvents[i].name));
  note("dma: TLPs since the doorbell, %s:%s", who, line);
}

void rasReport()
{
  if (!gArgs.ras) return;
  rasDesRead(gRasRoot, dbiRd, dbiWr);
  rasDesRead(gRasEndpoint, cfgRd, cfgWr);
  rasLine("root port", gRasRoot);
  rasLine("endpoint", gRasEndpoint);
  if (!gRasRoot.at) return;
  // The reading (§8.4): reads from the chip, and what went back. A 512 KiB image is about a thousand reads of 512 bytes.
  const uint32_t reads = rasDelta(gRasRoot, 7, 0x0c), data = rasDelta(gRasRoot, 7, 0x07), bare = rasDelta(gRasRoot, 7, 0x06);
  if (!rasOn(gRasRoot, 7, 0x0c)) return;
  if (!reads)
    note("dma: the root port received no read request from the chip: the boot loader refused before it fetched anything");
  else
    note("dma: the chip asked for data %u time(s); the root port answered with %u completion(s) with data and %u without (%s)",
         reads, data, bare, bare ? "errors: a read the SMMU or the NoC refused" : data ? "the reads were served" : "no answer");
}

// The image as the chip reads it: the boot loader's buffer and the file's parts in the window, read through its write-combined
// mapping (DRAM), against the stock file's identity.
struct ImageParts {
  Span spans[1 + kMhiMaxSegments];
  unsigned count = 0, fileFrom = 0;   // the file is spans[fileFrom .. count)
  uint32_t bytes = 0;
};
ImageParts gImage;
constexpr uint32_t kBounceBytes = 16384;

void imageHash(const char *when, bool whole)
{
  if (!gImage.count) return;
  uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
  if (!bounce) return note("firmware: %s: no buffer to hash the image through", when);
  auto copy = [](uint8_t *to, const uint8_t *from, uint32_t n) { memcpy(to, from, n); };
  uint8_t sbl[32], file[32];
  hashSpans(gImage.spans, 1, bounce, kBounceBytes, copy, sbl);
  const bool sblStock = sha256Same(sbl, kAmssStockSblSha256);
  if (!whole) {
    IOFreeData(bounce, kBounceBytes);
    note("firmware: %s: the boot loader in the window %s (sha256 %02x%02x%02x%02x...)", when,
         sblStock ? "is still the stock file's first 512 KiB" : "is NOT the stock file's first 512 KiB", sbl[0], sbl[1], sbl[2],
         sbl[3]);
    return;
  }
  // The whole file: the boot loader's buffer is its first 512 KiB, the segments the rest (peach's split; WCN7850's layout would
  // carry the whole file in the segments, and the line says so).
  hashSpans(gImage.spans + gImage.fileFrom, gImage.count - gImage.fileFrom, bounce, kBounceBytes, copy, file);
  IOFreeData(bounce, kBounceBytes);
  const bool fileStock = gImage.bytes == kAmssStockBytes && sha256Same(file, kAmssStockSha256);
  note("firmware: %s: boot loader sha256 %02x%02x%02x%02x... (%s), the file as the window holds it, %u bytes, sha256 "
       "%02x%02x%02x%02x... (%s)",
       when, sbl[0], sbl[1], sbl[2], sbl[3], sblStock ? "the stock file's first 512 KiB" : "NOT the stock boot loader", gImage.bytes,
       file[0], file[1], file[2], file[3], fileStock ? "the stock amss20.bin Ubuntu runs" : "NOT the stock amss20.bin");
}

// After a failed level 4: where the chip's DMA went. The PCI status and AER words of both ends (a request the root complex
// refused leaves "received master/target abort" or an AER bit in the endpoint; a completion that never came, a completion
// timeout), the controller's interrupt status, the SMMU's global fault (an unmatched stream: sGFSYNR1 holds the stream it came
// with) and the endpoint bank's fault registers. Reads only.
uint32_t aerAt(unsigned win)
{
  uint32_t at = kExtCapStart;
  for (unsigned guard = 0; at >= kExtCapStart && at + 0x14 <= 0x1000 && guard < 32; ++guard) {
    const uint32_t h = hwRead(nullptr, win, at);
    if (!h || h == 0xffffffffu) return 0;
    if ((h & 0xffffu) == kExtCapAer) return at;
    at = (h >> 20) & 0xffcu;
  }
  return 0;
}
void dmaReport(const LinkState &link)
{
  const uint32_t epAer = aerAt(kWinCfg), rpAer = aerAt(kWinDbi), epStatus = hwRead(nullptr, kWinCfg, 0x04);
  note("dma: endpoint command/status %08x%s, device control/status %08x; AER%s uncorrectable %08x correctable %08x", epStatus,
       (epStatus & kStatusErrorBits) ? " (error bits set)" : "", link.endpointCap ? hwRead(nullptr, kWinCfg, link.endpointCap + 0x08) : 0,
       epAer ? "" : " (no capability)", epAer ? hwRead(nullptr, kWinCfg, epAer + kAerUncorrectable) : 0,
       epAer ? hwRead(nullptr, kWinCfg, epAer + kAerCorrectable) : 0);
  note("dma: root port command/status %08x, secondary status %04x, device control/status %08x, BAR0 %08x BAR1 %08x; AER%s "
       "uncorrectable %08x correctable %08x; PARF interrupt status %08x",
       hwRead(nullptr, kWinDbi, kDbiCommand), hwRead(nullptr, kWinDbi, 0x1c) >> 16,
       link.expCap ? hwRead(nullptr, kWinDbi, link.expCap + 0x08) : 0, hwRead(nullptr, kWinDbi, 0x10), hwRead(nullptr, kWinDbi, 0x14),
       rpAer ? "" : " (no capability)", rpAer ? hwRead(nullptr, kWinDbi, rpAer + kAerUncorrectable) : 0,
       rpAer ? hwRead(nullptr, kWinDbi, rpAer + kAerCorrectable) : 0, hwRead(nullptr, kWinParf, kParfIntAllStatus));
  if (!gSmmuVa) return;
  note("dma: SMMU sCR0 %08x, global fault sGFSR %08x sGFSYNR0 %08x sGFSYNR1 %08x (stream %#x) sGFSYNR2 %08x sGFAR %08x:%08x",
       smmuRead(0x000), smmuRead(0x048), smmuRead(0x050), smmuRead(0x054), smmuRead(0x054) & 0xffffu, smmuRead(0x058),
       smmuRead(0x044), smmuRead(0x040));
  const bool window = gArgs.iova && !gIovaPlan.error && gIovaPlan.cb;
  const unsigned bank = window ? gIovaPlan.bank : gEndpointBank;
  if (bank == ~0u) return;
  const uint32_t cb = window ? gIovaPlan.cb : (gSmmuPages + bank) * 4096u;
  note("dma: SMMU bank %u (%s): sctlr %08x fsr %08x far %08x:%08x fsynr0 %08x", bank, window ? "the 32-bit window" : "bypass",
       smmuRead(cb + iova::kCbSctlr), smmuRead(cb + iova::kCbFsr), smmuRead(cb + 0x064), smmuRead(cb + 0x060), smmuRead(cb + 0x068));
}

MhiContexts gContexts;
Ipcr gIpcr;                       // level 5's two channels, planned by level 4 before the boot when level 5 follows
bool gIpcrPlanned;
uint32_t gMhiRegLen;
MhiBoot gBoot;                   // level 4's register layout (BHI, BHIe), for the crash report of any later level
const LinkState *gLinkState;      // for dmaReport after a crash

// ---- the crash dump (WifiRddm.h, docs §10.24) ----
// Given to the chip before the boot loader's doorbell, right after the receive block's clear, as the MHI host gives its table in
// the power-up's preparation; read back once the firmware reports its crash dump environment or a system error.
Rddm gRddm;
bool gRddmArmed, gCrashReported;
constexpr uint32_t kRddmEnterMs = 1000, kRddmWaitMs = 3000, kRddmRearmMs = 1500;

void rddmSetup(const MhiBoot &b)
{
  if (!gArgs.rddm) {
    note("level 4: crash dump: not given to the chip (piano-wifi-rddm=0): a firmware that dies cannot say why");
    return;
  }
  if (!rddmPlan(gRddm, gArena, kRddmBytes)) {
    note("level 4: crash dump: the DMA window has no room for %u KiB of segments: not given to the chip", kRddmBytes / 1024);
    return;
  }
  rddmArm(gRddm, gMhiIo, b.bhie, uint32_t(nowMs() * 2654435761u) | 1u);
  gRddmArmed = true;
  note("level 4: crash dump: %u segments of %u KiB from bus %#llx, the vector table at %#llx (%u bytes), given to the chip "
       "before the boot with session %#x (receive status %08x)",
       gRddm.segments, gRddm.segBytes / 1024, (unsigned long long)gRddm.segBus[0], (unsigned long long)gRddm.tableBus,
       gRddm.tableBytes, gRddm.seq, barRd(nullptr, b.bhie + kBhieRxVecStatus));
}

// Whether the device has written anything into the first segment's first page yet (the re-arm below only happens to a dump that
// never started).
bool rddmTouched()
{
  if (!gRddm.segments) return false;
  for (uint32_t i = 0; i < 4096; i += 8)
    if (*reinterpret_cast<volatile uint64_t *>(gRddm.seg[0] + i)) return true;
  return false;
}

// The firmware died (`when` says where): what MHI and BHI say, the dump once the chip has written it, its decode, and where the
// chip's DMA went. Once per boot; bounded by kRddmEnterMs + kRddmWaitMs.
void crashReport(const char *when)
{
  if (gCrashReported) return;
  gCrashReported = true;
  // The firmware is dead: nothing waits on this host any more, and the crash's own lines must never be the ones a full buffer
  // drops, so the held lines come out first and these are printed as they are written.
  deferFlush();
  const MhiBoot &b = gBoot;
  MhiHealth h = mhiHealth(gMhiIo, b.bhi);
  note("crash: %s: MHISTATUS %08x (state %#x%s), EE %s, BHI error code %08x, debug %08x %08x %08x, receive status %08x", when,
       h.status, h.state, h.sysErr ? ", SYSERR" : "", mhiEeName(h.ee), barRd(nullptr, b.bhi + kBhiErrCode),
       barRd(nullptr, b.bhi + kBhiErrDbg1), barRd(nullptr, b.bhi + kBhiErrDbg2), barRd(nullptr, b.bhi + kBhiErrDbg3),
       barRd(nullptr, b.bhie + kBhieRxVecStatus));
  const uint64_t start = nowMs();
  while (!h.rddm && !h.gone && nowMs() - start < kRddmEnterMs) {   // a system error comes first; the dump environment follows
    IOSleep(5);
    h = mhiHealth(gMhiIo, b.bhi);
  }
  if (!h.rddm) note("crash: the chip is not in its crash dump environment after %llu ms (EE %s%s)",
                    (unsigned long long)(nowMs() - start), mhiEeName(h.ee), h.gone ? ", MHISTATUS all ones" : "");
  if (!gRddmArmed) {
    note("crash: no dump segments were given to the chip: the reason cannot be read back");
  } else {
    uint32_t poll = kRddmWaiting;
    bool rearmed = false;
    while (nowMs() - start < kRddmEnterMs + kRddmWaitMs) {
      poll = rddmPoll(gRddm, gMhiIo, b.bhie);
      if (poll != kRddmWaiting) break;
      // A dump that never started by now: the table given once more with a new session (the device may only look at the doorbell
      // in its dump environment). Never while data is arriving.
      if (!rearmed && h.rddm && nowMs() - start >= kRddmRearmMs && !rddmTouched()) {
        rddmArm(gRddm, gMhiIo, b.bhie, gRddm.seq + 0x101u);
        rearmed = true;
      }
      IOSleep(5);
    }
    note("crash: dump %s after %llu ms: receive status %08x (session %s%s)", rddmPollName(poll),
         (unsigned long long)(nowMs() - start), gRddm.status, rddmSessionMatches(gRddm) ? "ours" : "not ours",
         rearmed ? "; the table was given once more" : "");
    uint8_t *scratch = static_cast<uint8_t *>(IOMallocData(kRddmScratchBytes));
    RddmReport *rep = static_cast<RddmReport *>(IOMallocData(sizeof(RddmReport)));
    if (!scratch || !rep) {
      note("crash: no memory to decode the dump");
    } else {
      const uint32_t serial = barRd(nullptr, b.bhi + kBhiSerialNumber);
      rddmDecode(gRddm, scratch, [](uint8_t *to, const uint8_t *from, uint32_t n) { memcpy(to, from, n); }, serial, *rep);
      note("crash: dump: %u of %u segment(s) hold data, the data ends at +%#llx", rep->segmentsWithData, gRddm.segments,
           (unsigned long long)rep->dataEnd);
      if (rep->header) {
        note("crash: dump header: version %#x, %u bytes (%s), %u entr%s", rep->version, rep->headerBytes,
             rep->sizeFirst ? "the size first" : "the version first", rep->entries, rep->entries == 1 ? "y" : "ies");
        for (uint32_t i = 0; i < rep->files; ++i) {
          const RddmFile &f = rep->file[i];
          note("crash: file %u: \"%s\" (\"%s\"), %llu bytes at +%#llx%s, device address %#llx", i, f.name, f.description,
               (unsigned long long)f.size, (unsigned long long)f.offset, f.inDump ? "" : " (not inside the segments)",
               (unsigned long long)f.base);
        }
      } else if (rep->why) {
        note("crash: dump header not decoded (%s): first words %08x %08x", rep->why, rep->word0, rep->word1);
      }
      if (rep->reason[0])
        note("crash: the firmware's failure reason (file \"%s\"): \"%s\"", rep->file[rep->reasonFile].name, rep->reason);
      else if (rep->reasonFile >= 0)
        note("crash: the failure reason file \"%s\" is empty", rep->file[rep->reasonFile].name);
      for (unsigned i = 0; i < rep->strings; ++i)
        note("crash: error-like text in the first segment at +%#llx: \"%s\"", (unsigned long long)rep->string[i].at,
             rep->string[i].text);
      if (!rep->reason[0] && rep->dataEnd)
        note("crash: %u error-like string(s) in the first segment, %u shown; nothing else of the dump is printed",
             rep->candidates, rep->strings);
    }
    if (scratch) IOFreeData(scratch, kRddmScratchBytes);
    if (rep) IOFreeData(rep, sizeof(RddmReport));
  }
  dumpMhi("at the crash", b);
  if (gLinkState) dmaReport(*gLinkState);   // the SMMU's fault registers say whether the chip's DMA left the window
  msiSinkReport("at the crash");
}

struct FirmwarePlan {
  FwLayout layout;
  uint32_t bytes = 0, segments = 0, tableEntries = 0;
  uint64_t sblBus = 0, tableBus = 0, contextBus = 0, segmentBus[kMhiMaxSegments] = {};
  const char *partition = nullptr;
  const char *directory = nullptr;
  int rc = 0;
};

// The image straight from the tablet's own modem_a into the DMA window: the boot loader into its buffer, each 512 KiB segment into
// its own, then the BHIe vector table. The file is never held whole in the heap and nothing of it is logged.
bool loadFirmware(FirmwarePlan &plan)
{
  plan.rc = piano_wifi_firmware_open(&plan.partition);
  if (plan.rc) {
    note("firmware: no partition (%d: %s)", plan.rc,
         plan.rc == PianoWifiFwNotReady ? "NativeUfs has not published it yet" : "not mounted");
    return false;
  }
  plan.rc = piano_wifi_firmware_stat(kAmssName, &plan.bytes, &plan.directory);
  if (plan.rc) {
    note("firmware: %s not found on %s (%d)", kAmssName, plan.partition, plan.rc);
    return false;
  }
  uint8_t magic[kElfMagicBytes] = {};
  const bool haveMagic = plan.bytes > kMhiSblBytes &&
                         !piano_wifi_firmware_read(kAmssName, kMhiSblBytes, magic, sizeof magic);
  if (!firmwareLayoutFromMagic(haveMagic ? magic : nullptr, plan.bytes, kMhiSblBytes, plan.layout)) {
    note("firmware: %s (%u bytes) has no usable layout", kAmssName, plan.bytes);
    return false;
  }
  note("firmware: %s/%s on %s, %u bytes: boot loader %u bytes, %s image %u bytes at %u", plan.directory, kAmssName,
       plan.partition, plan.bytes, plan.layout.sblBytes, plan.layout.split ? "a second ELF" : "the whole file",
       plan.layout.imageBytes, plan.layout.imageOffset);
  plan.segments = (plan.layout.imageBytes + kMhiSegBytes - 1) / kMhiSegBytes;
  if (plan.segments > kMhiMaxSegments) {
    note("firmware: %u segments of 512 KiB is more than the %u this driver plans for", plan.segments, kMhiMaxSegments);
    return false;
  }
  uint8_t *sbl = arenaAlloc(gArena, kMhiSblBytes, &plan.sblBus);
  if (!sbl) return note("firmware: the DMA window has no room for the boot loader"), false;
  if (piano_wifi_firmware_read(kAmssName, plan.layout.sblOffset, sbl, plan.layout.sblBytes))
    return note("firmware: the boot loader could not be read"), false;
  // The parts as the chip will read them, for the identity check below and the one after a failure (§10.23): the boot loader's
  // buffer, then the segments. With peach's split the file is the buffer and the segments; with one image it is the segments.
  gImage = ImageParts{};
  gImage.spans[gImage.count++] = Span{sbl, plan.layout.sblBytes};
  uint64_t addresses[kMhiMaxSegments] = {};
  for (uint32_t i = 0; i < plan.segments; ++i) {
    uint8_t *segment = arenaAlloc(gArena, kMhiSegBytes, &plan.segmentBus[i]);
    if (!segment) return note("firmware: the DMA window has no room for segment %u", i), false;
    const uint32_t done = i * kMhiSegBytes, left = plan.layout.imageBytes - done;
    if (piano_wifi_firmware_read(kAmssName, plan.layout.imageOffset + done, segment, left < kMhiSegBytes ? left : kMhiSegBytes))
      return note("firmware: segment %u could not be read", i), false;
    addresses[i] = plan.segmentBus[i];
    gImage.spans[gImage.count++] = Span{segment, left < kMhiSegBytes ? left : kMhiSegBytes};
  }
  gImage.fileFrom = plan.layout.split ? 0 : 1;
  gImage.bytes = plan.bytes;
  BhieEntry *table = reinterpret_cast<BhieEntry *>(arenaAlloc(gArena, 4096, &plan.tableBus));
  if (!table) return note("firmware: the DMA window has no room for the vector table"), false;
  plan.tableEntries = bhieTable(table, 4096 / sizeof(BhieEntry), addresses, plan.layout.imageBytes, kMhiSegBytes);
  if (!plan.tableEntries) return note("firmware: the vector table does not fit"), false;
  dsb();
  note("firmware: in the window: boot loader at %#llx, %u segments, vector table at %#llx (%u entries)",
       (unsigned long long)plan.sblBus, plan.segments, (unsigned long long)plan.tableBus, plan.tableEntries);
  imageHash("in the window", true);
  return true;
}

int level4(const LinkState &link, PowerState &power, const Options &opt)
{
  if (!mapBar(link.barAddress, link.barBytes > kBar0BytesExpected ? kBar0BytesExpected : link.barBytes)) {
    note("level 4: BAR0 (%#llx + %u) could not be mapped", (unsigned long long)link.barAddress, link.barBytes);
    return -14;
  }
  note("level 4: BAR0 mapped (%#llx + %#x); MHIREGLEN %08x, BHIOFF %08x, BHIEOFF %08x, MHISTATUS %08x",
       (unsigned long long)link.barAddress, gBarBytes, barRd(nullptr, kMhiRegLen), barRd(nullptr, kMhiBhiOff),
       barRd(nullptr, kMhiBhieOff), barRd(nullptr, kMhiStatus));
  MhiBoot boot;
  if (!mhiOpen(boot, gMhiIo, gBarBytes)) {
    note("level 4: the MHI register layout is not usable (reglen %08x bhi %08x bhie %08x): %s", boot.reglen, boot.bhi, boot.bhie,
         mhiFailName(boot.error));
    safeOff(gHw, power, opt);
    return -5;
  }
  gMhiRegLen = boot.reglen;
  gBoot = boot;
  gLinkState = &link;
  const uint32_t version = chipRead(kChipTcsrSocHwVersion);
  note("level 4: BHI at %#x, BHIe at %#x, EE %s, state %#x; chip TCSR version %08x (major %u)", boot.bhi, boot.bhie,
       mhiEeName(mhiEeRead(boot)), mhiStateRead(boot), version, (version >> 8) & 0xfu);
  noteIdentity("at the start", boot);
  FirmwarePlan plan;
  if (!loadFirmware(plan)) {
    piano_wifi_firmware_close();
    safeOff(gHw, power, opt);
    return -2;
  }
  if (gArgs.level < 5) piano_wifi_firmware_close();   // level 5 reads its own files from the same partition
  MhiContexts &contexts = gContexts;
  uint64_t contextBus = 0;
  uint8_t *contextVa = arenaAlloc(gArena, kMhiContextBytes, &contextBus);
  if (!contextVa || !mhiContextsPlan(contexts, contextVa, contextBus, kMhiContextBytes)) {
    note("level 4: no room for the MHI contexts");
    safeOff(gHw, power, opt);
    return -12;
  }
  // What the chip's own host does before the doorbell (WifiPbl.h, docs §10.23): its power-on reset of the chip, the receive
  // vector block cleared, MSI enabled; the MHI context registers only once the device reports READY (the loop below does that in
  // any case), unless piano-wifi-ctxearly=1 asks for the 2026-11-12 order.
  if (gArgs.socreset) {
    chipPowerOn(boot);
    noteIdentity("after the chip's reset", boot);
  } else {
    // The MHI host's own clear of the receive block (its power-on values are undefined) comes before the crash dump's table in
    // any case; chipPowerOn has it after the chip's reset.
    note("level 4: %u receive vector words cleared (no chip reset: piano-wifi-socreset=0)", mhiClearRxVectors(boot));
  }
  rddmSetup(boot);
  if (gArgs.msi) msiSink(link);
  // Level 5's two channels: their rings and buffers now, out of the same window, so that nothing but doorbells and the start
  // commands is left for after mission mode (the MHI host initialises every context its configuration names before the
  // power-up; mhiContextsPlan wrote the two channels' fixed fields).
  if (gArgs.level >= 5) {
    gIpcrPlanned = ipcrPlan(gIpcr, gArena, contexts);
    note("level 4: channels %u and %u %s", kChanIpcrOut, kChanIpcrIn,
         gIpcrPlanned ? "planned before the boot (state disabled, burst mode disabled, the data event ring)"
                      : "could not be planned: the DMA window has no room (level 5 will stop)");
  }
  dsb();
  if (gArgs.ctxearly) mhiContextsProgram(contexts, gMhiIo, boot.reglen);
  note("level 4: contexts at %#llx (channels %u, event rings %u: control at %#llx, data at %#llx, command ring at %#llx), %s",
       (unsigned long long)contextBus, kMhiChannels, kMhiEventRings, (unsigned long long)contexts.eventBus[0],
       (unsigned long long)contexts.eventBus[1], (unsigned long long)contexts.commandBus,
       gArgs.ctxearly ? "given to the device now and again at READY" : "given to the device at READY, as the MHI host does");
  // The image reaches the chip through the write-combined mapping only: everything written so far is pushed out before the doorbell
  // (the dsb above; barWr ends every register write with another).
  note("level 4: the image: %u bytes at bus %#llx (%s), aligned to %llu KiB; the vector table at %#llx", plan.layout.sblBytes,
       (unsigned long long)plan.sblBus, gArgs.iova ? "the 32-bit window, translated by the SMMU" : "physical, the stream bypasses",
       (unsigned long long)(arenaAlign(kMhiSblBytes) >> 10), (unsigned long long)plan.tableBus);
  dumpMhi("before the doorbell", boot);
  rasArm();
  deferStart();   // from the doorbell on, the console must not hold this thread (wifi-4a: 2 s, and the firmware died meanwhile)
  uint32_t phase = mhiBegin(boot, 0, plan.sblBus, plan.layout.sblBytes, plan.tableBus,
                            plan.tableEntries * uint32_t(sizeof(BhieEntry)), uint32_t(nowMs()) | 1u);
  if (phase == kMhiSblLoading)
    note("level 4: boot begun in phase %s (EE %s); BHI status %08x before, %08x after this host cleared it (%s)",
         mhiPhaseName(phase), mhiEeName(boot.ee), boot.statusBefore, boot.statusCleared,
         boot.clearTook ? "the clear took" : "the clear did not take: that value is the register's own, not an error");
  else
    note("level 4: boot begun in phase %s (EE %s)", mhiPhaseName(phase), mhiEeName(boot.ee));
  const uint64_t started = nowMs();
  uint64_t since = started;
  uint32_t last = phase, steps = 0;
  MhiSnap seen = mhiSnap(boot);
  unsigned snapLines = 1;
  noteSnap("after the doorbell", 0, seen);
  while (phase != kMhiDone && phase != kMhiFailed) {
    if (phase == kMhiNeedContexts) {
      dsb();
      mhiContextsProgram(contexts, gMhiIo, boot.reglen);
      phase = mhiContextsDone(boot);
    } else {
      phase = mhiStep(boot);
      if (phase != kMhiNeedContexts && phase != kMhiDone && phase != kMhiFailed) mhiEventAck(contexts, gMhiIo, boot.reglen);
    }
    ++steps;
    const uint64_t now = nowMs();
    const MhiSnap snap = mhiSnap(boot);
    if (snap != seen) {
      if (snapLines < kSnapLines) noteSnap("changed", now - started, snap);
      else if (snapLines == kSnapLines) note("level 4: further register changes are not logged (%u lines)", kSnapLines);
      ++snapLines;
      seen = snap;
    }
    if (phase != last) {
      note("level 4: %s after %llu ms (EE %s, state %#x)", mhiPhaseName(phase), (unsigned long long)(now - started),
           mhiEeName(boot.ee), boot.state);
      deferPhase();
      last = phase;
      since = now;
      if (!levelLate()) continue;
    }
    if (now - since > gArgs.fwTimeoutMs || levelLate()) {
      if (levelLate()) note("level 4: the level's hard deadline (%u ms) passed in phase %s", gArgs.deadlineMs, mhiPhaseName(phase));
      phase = mhiTimedOut(boot);
      break;
    }
    IOSleep(5);
  }
  note("level 4: %s after %llu ms, %u steps, %u context writes, %u event acknowledgements; EE %s, state %#x, BHI status %08x, "
       "error code %08x (%s)",
       phase == kMhiDone ? "mission mode" : "stopped", (unsigned long long)(nowMs() - started), steps, contexts.writes,
       contexts.acks, mhiEeName(mhiEeRead(boot)), boot.state, boot.lastStatus, boot.errcode, mhiFailName(boot.error));
  if (phase != kMhiDone) {
    note("level 4: failed in phase %s: BHI error code %08x, debug %08x %08x %08x; BHI status %08x before the clear, %08x after "
         "(%s); %u register change line(s)",
         mhiPhaseName(boot.failedPhase), boot.errcode, boot.errdbg[0], boot.errdbg[1], boot.errdbg[2], boot.statusBefore,
         boot.statusCleared, boot.clearTook ? "it took" : "not taken or not reached", snapLines);
    rasReport();   // first: the counters as close to the refusal as the log allows
    noteSnap("at the end", nowMs() - started, mhiSnap(boot));
    dumpMhi("at the end", boot);
    dmaReport(link);
    msiSinkReport("at the end");
    imageHash("after the failure", false);
    noteIdentity("at the end", boot);
    if (boot.error == kMhiErrRddm || boot.error == kMhiErrSysErr) crashReport("level 4, before mission mode");
    safeOff(gHw, power, opt);
    return -1;
  }
  // Mission mode. What the MHI host does first (its mission-mode transition): the device kept awake through the wake doorbell, so
  // that it stays in M0 while the host starts the channels; this host keeps it asserted while its levels run.
  if (gArgs.wake) {
    const bool rung = mhiDevWake(gMhiIo, boot.reglen, true);
    note("level 4: device wake %s", rung ? "asserted (channel 127's doorbell = 1)" : "NOT asserted: the doorbell array is outside BAR0");
  }
  rasReport();
  msiSinkReport("at mission mode");
  const MhiHealth health = mhiHealth(gMhiIo, boot.bhi);
  if (health.dead()) {
    crashReport("level 4, right after mission mode");
    safeOff(gHw, power, opt);
    return -1;
  }
  return 0;
}

// =============================================== level 5: the IPC router's channels, QRTR, and the firmware's QMI service
// The transport (WifiChannels.h) over the contexts level 4 gave the chip, the copied router (WifiQrtr.c) on top of it, and the
// WLFW client (WifiWlfw.h) on top of that. Everything is polled from this thread: no MSI, no interrupt, no allocation after the
// window, and every wait is bounded by piano-wifi-qmitimeout.
QcomQrtr gRouter;
WlfwClient gClient;
unsigned gQrtrUnrouted;
// The firmware's service as it was announced: the instance word this family uses (WifiHw.h), or, when only another instance of
// service 69 is announced, that one (logged; the chip's own host looks the service up by its instance, which this driver has from
// the documents, not from a boot).
uint32_t gWlfwInstance = kWlfwInstanceWord;
bool wlfwFind(uint32_t *node, uint32_t *port)
{
  if (!qcom_qrtr_find_service(&gRouter, kWlfwService, kWlfwInstanceWord, node, port)) {
    gWlfwInstance = kWlfwInstanceWord;
    return true;
  }
  for (uint32_t i = 0; i < gRouter.remote_count; ++i)
    if (gRouter.remote[i].service == kWlfwService) {
      gWlfwInstance = gRouter.remote[i].instance;
      if (node) *node = gRouter.remote[i].node;
      if (port) *port = gRouter.remote[i].port;
      return true;
    }
  return false;
}

// The link's send: a packet that already sits in one of our outbound buffers is submitted as it is; a control packet the router
// built on its stack (hello, resume) is copied into a free buffer first.
int qrtrSend(void *, const uint8_t *packet, uint32_t length)
{
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
    if (gIpcr.tx[i].va == packet) return ipcrSubmitTx(gIpcr, gMhiIo, i, length) ? 0 : -1;
  const int index = ipcrClaimTx(gIpcr);
  if (index < 0 || length > gIpcr.tx[index].bytes) return -1;
  memcpy(gIpcr.tx[index].va, packet, length);
  return ipcrSubmitTx(gIpcr, gMhiIo, unsigned(index), length) ? 0 : -1;
}

// A data packet for our port: the client's next message.
void qrtrReceive(void *, QcomQrtr *, uint32_t, uint32_t, uint32_t, const uint8_t *data, uint32_t length)
{
  wlfwReceive(gClient, data, length);
}

// What the client needs: a buffer with room for the router's header in front of the message, the send, the firmware files of the
// stock partition, memory of the window, and one line per step.
uint8_t *wlfwClaim(void *, uint32_t *capacity)
{
  const int index = ipcrClaimTx(gIpcr);
  if (index < 0) return nullptr;
  *capacity = gIpcr.tx[index].bytes - QCOM_QRTR_HEADER;
  return gIpcr.tx[index].va + QCOM_QRTR_HEADER;
}
uint64_t gWlfwSentAt;   // when the request outstanding went out (its answer's line says how long it took)
int wlfwSendMessage(void *, uint8_t *buffer, uint32_t length)
{
  uint8_t *base = buffer - QCOM_QRTR_HEADER;
  uint32_t node = 0, port = 0;
  if (!wlfwFind(&node, &port)) return -1;
  gWlfwSentAt = nowMs();
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
    if (gIpcr.tx[i].va == base)
      return qcom_qrtr_send_buffer(&gRouter, base, gIpcr.tx[i].bytes, kQrtrClientPort, node, port, length);
  return -1;
}
uint32_t wlfwFileSize(void *, const char *name)
{
  uint32_t bytes = 0;
  return piano_wifi_firmware_stat(name, &bytes, nullptr) ? 0 : bytes;
}
int wlfwFileRead(void *, const char *name, uint64_t offset, void *to, uint32_t bytes)
{
  return piano_wifi_firmware_read(name, offset, to, bytes);
}
uint64_t wlfwMemory(void *, uint32_t bytes, uint8_t **va)
{
  uint64_t bus = 0;
  uint8_t *p = arenaAlloc(gArena, bytes, &bus);
  if (!p) return 0;
  if (va) *va = p;
  return bus;
}
void wlfwStepNote(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
  // What each download step sends (names and sizes only: nothing of a file is ever printed).
  auto file = [&](const char *name, const char *kind) {
    const uint32_t bytes = wlfwFileSize(nullptr, name);
    if (!bytes) return note("step %d: %s: %s is not in %s of the firmware partition", step, kind, name, "image/peach or image");
    note("step %d: %s: %s, %u bytes, %u segment(s) of at most %u, file identifier %#x", step, kind, name, bytes,
         (bytes + kWlfwMaxDataBytes - 1) / kWlfwMaxDataBytes, kWlfwMaxDataBytes, gClient.boardId);
  };
  if (step == kWlfwStepRegdb) {
    note("step %d: board identifiers: the chip's capability %s%#x, the radio's capability %s%#x; the downloads carry %#x", step,
         gClient.haveCapBoard ? "" : "gave none: ", gClient.haveCapBoard ? gClient.capBoardId : kBoardIdDefault,
         gClient.havePhyBoard ? "" : "gave none: ", gClient.havePhyBoard ? gClient.phyBoardId : kBoardIdDefault, gClient.boardId);
    file(kRegdbName, "the regulatory database (kind 4)");
  } else if (step == kWlfwStepBoard) {
    const char *why = nullptr;
    const char *name = wlfwBoardFile(gClient.boardId, &why);
    note("step %d: board file %s for board %#x: %s", step, name, gClient.boardId, why);
    file(name, "the board file (kind 1, at most 256 KiB sent)");
  } else if (step == kWlfwStepM3 || step == kWlfwStepAuxUc) {
    const char *name = step == kWlfwStepM3 ? kM3Name : kAuxUcName;
    note("step %d: %s, %u bytes, by address", step, name, wlfwFileSize(nullptr, name));
  } else if (step == kWlfwStepQdss) {
    file(kQdssName, "the trace configuration");
  }
  deferPhase();
}

// Every answer and indication of the firmware's service, with its codes (WlfwIo::event).
const char *wlfwIndicationName(uint16_t message)
{
  switch (message) {
  case kWlfwRequestMemInd: return "the firmware asks for memory";
  case kWlfwFwMemReadyInd: return "the firmware's memory is ready";
  case kWlfwFwReadyInd: return "the firmware is ready";
  default: return "one this driver does not act on";
  }
}
void wlfwEventNote(void *, const WlfwEvent &e)
{
  if (e.kind == kWlfwEventIndication) {
    note("step %d: indication %#06x: %s", e.step, e.message, wlfwIndicationName(e.message));
    return;
  }
  if (e.kind == kWlfwEventUnexpected) {
    note("step %d: unexpected message %#06x transaction %u%s%s (counted, not acted on)", e.step, e.message, e.transaction,
         e.hasResult ? ", a response or a refusal" : "", e.message == kWlfwFwReadyInd ? ": a ready indication before its time" : "");
    return;
  }
  char what[120] = "";
  if (e.segment >= 0)
    snprintf(what, sizeof what, ", %s segment %d of %u, %u bytes%s", e.file ? e.file : "?", e.segment + 1, e.segments, e.bytes,
             e.last ? ", the last" : "");
  note("step %d: answer to %#06x (transaction %u%s) after %llu ms: %s result %u error %#x%s", e.step, e.message, e.transaction,
       what, (unsigned long long)(nowMs() - gWlfwSentAt), !e.hasResult ? "no result TLV," : "", e.result, e.error,
       !e.hasResult ? "" : e.result == kQmiResultSuccess ? " (accepted)" : " (refused)");
}

// One turn: the event rings walked, every received packet given to the router, the buffers given back to the chip, and the
// chip's health read (MHISTATUS and the execution environment: two register reads). Returns how many events were seen; `stop` is
// set when the chip reported an environment this driver cannot go on from (its crash dump environment, a system error), and
// gChipDead says that it died rather than refused.
bool gChipDead;
unsigned gM0Events, gM1Answers;
unsigned pump(bool &stop)
{
  bool fatal = false;
  const unsigned seen = ipcrPoll(gIpcr, gContexts, gMhiIo, [&](uint32_t kind, uint32_t, uint32_t code, uint32_t length,
                                                               uint8_t *va) {
    switch (kind) {
    case kIpcrPacket:
      if (qcom_qrtr_receive(&gRouter, va, length)) ++gQrtrUnrouted;
      break;
    case kIpcrState:
      note("level 5: the chip reports state %#x", code);
      if (code == kStateM0) {
        // The MHI host's M0 in mission mode: every event ring, the command ring with a command waiting, every running channel
        // rung again, so that a device that came back from a low-power state sees where the host is.
        ++gM0Events;
        ipcrRingAll(gIpcr, gContexts, gMhiIo);
      } else if (code == kStateM1) {
        // The device idled although the wake doorbell is asserted: the MHI host's answer is M2, and a host that still needs the
        // device then toggles the wake doorbell, which brings it back to M0.
        ++gM1Answers;
        mhiSetState(gMhiIo, kStateM2);
        mhiDevWake(gMhiIo, gMhiRegLen, false);
        mhiDevWake(gMhiIo, gMhiRegLen, true);
      } else if (code == kStateSysErr) {
        fatal = true;
      }
      break;
    case kIpcrEe:
      note("level 5: the chip reports environment %s", mhiEeName(code));
      if (code == kEeRddm) fatal = true;
      break;
    default:
      break;
    }
  });
  // Every buffer a packet came back in goes onto channel 21's ring again (from the walk after 21's start on: before it the
  // transport posts nothing, since 21's preparation puts its ring back at the base).
  if (seen) ipcrPostRx(gIpcr, gMhiIo);
  // A flow-control answer the link refused when it was due goes out now (the firmware sends nothing more to our port without it).
  qcom_qrtr_poll(&gRouter);
  const MhiHealth h = mhiHealth(gMhiIo, gBoot.bhi);
  if (h.dead()) fatal = true;
  if (fatal) gChipDead = true;
  stop = fatal;
  return seen;
}

// A channel started: its ring into its context, one command on the primary ring, then its completion event.
bool startChannel(uint32_t channel)
{
  if (!ipcrStart(gIpcr, gMhiIo, channel)) {
    note("level 5: the start command for channel %u could not be queued (%s)", channel,
         gIpcr.error ? gIpcr.error : "a command is in flight");
    return false;
  }
  const uint64_t deadline = nowMs() + kMhiCommandTimeoutMs;
  while (nowMs() < deadline && !levelLate()) {
    bool stop = false;
    pump(stop);
    if (stop) {
      note("level 5: the chip died while channel %u was being started", channel);
      return false;
    }
    if ((channel == kChanIpcrOut && gIpcr.started[0]) || (channel == kChanIpcrIn && gIpcr.started[1])) return true;
    if (gIpcr.commandErrors) {
      note("level 5: the chip refused to start channel %u (completion code %#x)", channel, gIpcr.lastCode);
      return false;
    }
    IOSleep(2);
  }
  note("level 5: no completion for the start of channel %u within %u ms", channel, kMhiCommandTimeoutMs);
  return false;
}

// Where the two channels and the data event ring are, as this host and as the chip's contexts have them: the line that says
// whether the chip took what was posted (its read pointers moved), and what came back.
int ipcrElementOf(const ChanRing &r, uint64_t ptr)
{
  return ptr == r.bus + uint64_t(r.elements) * kMhiElement ? int(r.elements) : ipcrSlotOf(r, ptr);
}
void ipcrReport(const char *when)
{
  using namespace ipcrdetail;
  const ChanRing &o = gIpcr.out, &i = gIpcr.in;
  if (!o.ctx || !i.ctx) return;
  uint8_t *ev = mhiEventCtx(gContexts, kIpcrEventRing);
  const uint64_t evBase = get64(ev + 12), evRp = get64(ev + 28), acked = gContexts.acked[kIpcrEventRing];
  // Three lines: a held line has room for 300 bytes (kDeferBytes), and one line cut the 2026-11-12 boots' report in half.
  note("level 5: %s: channel 20 state %#x, the chip's read pointer at element %d, ours %u, written to %u (%u owed), %u "
       "completion(s) (last code %#x), %u doorbell(s)",
       when, ipcrChanState(o), ipcrElementOf(o, get64(o.ctx + kChanRp)), o.rp, o.wp, ipcrOutstanding(o), o.events, o.lastCode,
       o.doorbells);
  note("level 5: %s: channel 21 state %#x, the chip's read pointer at element %d, ours %u, written to %u (%u buffers posted), "
       "%u completion(s) (last code %#x, %u bytes), %u doorbell(s)",
       when, ipcrChanState(i), ipcrElementOf(i, get64(i.ctx + kChanRp)), i.rp, i.wp, ipcrRxPosted(gIpcr), i.events, i.lastCode,
       i.lastLength, i.doorbells);
  note("level 5: %s: data event ring: the chip at element %lld, this host at %lld; %u event(s) dropped, %u element(s) given "
       "back unread, %u error code(s), %u doorbell-mode event(s)",
       when, (long long)(evRp - evBase) / int(kMhiElement), (long long)(acked - evBase) / int(kMhiElement), gIpcr.dropped,
       gIpcr.skipped, gIpcr.transferErrors, gIpcr.dbModes);
}

int level5(PowerState &power, const Options &opt)
{
  stage(500, "level 5: the IPC router's channels, QRTR, and the WLAN firmware's QMI service");
  const uint64_t entered = nowMs();
  if (!gIpcrPlanned && !(gIpcrPlanned = ipcrPlan(gIpcr, gArena, gContexts))) {
    stage(-500, "level 5: the DMA window has no room for the channels' rings and buffers");
    safeOff(gHw, power, opt);
    return -1;
  }
  if (!ipcrOpen(gIpcr, gMhiIo, gMhiRegLen)) {
    stage(-500, "level 5: the chip's doorbell arrays are not where the register space says");
    safeOff(gHw, power, opt);
    return -2;
  }
  note("level 5: channels %u and %u planned (%u elements each, %u KiB of buffers), channel doorbells at %#x, event doorbells "
       "at %#x",
       kChanIpcrOut, kChanIpcrIn, kIpcrElements, (kIpcrRxBuffers + kIpcrOutBuffers) * kIpcrBufferBytes / 1024, gIpcr.chdb,
       gIpcr.erdb);
  if (!startChannel(kChanIpcrOut) || !startChannel(kChanIpcrIn)) {
    if (gChipDead) crashReport("level 5, starting the IPC router's channels");
    stage(-501, "level 5: a channel did not start");
    safeOff(gHw, power, opt);
    return -3;
  }
  // The inbound channel's buffers, right after its start (the walk that saw the start's completion has normally posted them
  // already; this call posts whatever is still free).
  ipcrPostRx(gIpcr, gMhiIo);
  note("level 5: both channels run (states %#x / %#x) %llu ms after level 5 began, %u receive buffers posted (channel 21's "
       "ring written to element %u, %u doorbell(s))",
       ipcrChanState(gIpcr.out), ipcrChanState(gIpcr.in), (unsigned long long)(nowMs() - entered), ipcrRxPosted(gIpcr),
       gIpcr.in.wp, gIpcr.in.doorbells);
  if (!ipcrRxPosted(gIpcr)) {
    ipcrReport("no receive buffer on channel 21");
    stage(-501, "level 5: channel 21 runs but has no receive buffer: the chip could not answer");
    safeOff(gHw, power, opt);
    return -3;
  }
  // The router. The chip's router speaks first on this link (the Linux host only ever answers a hello), and the copied router
  // answers it; ours goes out only if the chip has said nothing within kHelloWaitMs.
  qcom_qrtr_init(&gRouter, kQrtrLocalNode, qrtrSend, nullptr);
  if (qcom_qrtr_add_client(&gRouter, kQrtrClientPort, qrtrReceive, nullptr)) {
    stage(-502, "level 5: the router's client port could not be added");
    safeOff(gHw, power, opt);
    return -4;
  }
  constexpr uint32_t kHelloWaitMs = 500;
  bool died = false;
  const uint64_t helloFrom = nowMs();
  while (!gRouter.hello_received && nowMs() - helloFrom < kHelloWaitMs && !levelLate()) {
    bool stop = false;
    pump(stop);
    if (stop) {
      died = true;
      break;
    }
    IOSleep(2);
  }
  const bool chipFirst = gRouter.hello_received;
  if (!gRouter.hello_sent && !died) qcom_qrtr_start(&gRouter);
  note("level 5: router: %s", chipFirst ? "the chip said hello first and was answered"
                              : died   ? "the chip died before it said hello"
                                       : "no hello from the chip within 500 ms: ours went first");
  // Our hello (or our answer to the chip's) must complete on channel 20: its transfer completion names the element it took.
  uint32_t node = 0, port = 0;
  uint64_t deadline = nowMs() + gArgs.qmiTimeoutMs, helloDone = 0;
  auto helloSeen = [&]() {
    if (!helloDone && gIpcr.txDone) helloDone = nowMs();
  };
  helloSeen();
  while (!died && nowMs() < deadline && !levelLate()) {
    bool stop = false;
    pump(stop);
    helloSeen();
    if (stop) {
      died = true;
      break;
    }
    if (wlfwFind(&node, &port)) break;
    IOSleep(5);
  }
  if (helloDone)
    note("level 5: router: our %s completed on channel 20 %llu ms after the router began (code %#x, %u packet(s) out, %u "
         "completed)",
         chipFirst ? "answer to the chip's hello" : "hello", (unsigned long long)(helloDone - helloFrom), gIpcr.out.lastCode,
         gIpcr.sent, gIpcr.txDone);
  else
    note("level 5: router: our %s was %s (%u packet(s) out, none completed by the chip)",
         chipFirst ? "answer to the chip's hello" : "hello", gRouter.hello_sent ? "sent and never completed" : "not sent",
         gIpcr.sent);
  if (died || !gRouter.hello_received || !wlfwFind(&node, &port)) {
    note("level 5: hello %s, %u services announced, %u packets in (%u unrouted), %u received that were not router packets (the "
         "transport delivered %u)%s",
         gRouter.hello_received ? "answered" : "unanswered", gRouter.remote_count, gRouter.rx_packets, gRouter.rx_unrouted,
         gRouter.rx_bad, gIpcr.received, levelLate() ? "; the level's hard deadline passed" : "");
    ipcrReport("waiting for the firmware's service");
    for (uint32_t r = 0; r < gRouter.remote_count && r < 8; ++r)
      note("level 5: the chip announced service %u instance %#x on node %u port %#x", gRouter.remote[r].service,
           gRouter.remote[r].instance, gRouter.remote[r].node, gRouter.remote[r].port);
    if (gChipDead) crashReport("level 5, waiting for the firmware's QMI service");
    stage(-502, "level 5: the firmware's QMI service was not announced on the router");
    safeOff(gHw, power, opt);
    return -5;
  }
  note("level 5: the router is up (our node %u port %#x, the chip's node %u; the firmware's service %u instance %#x on port %u%s)",
       kQrtrLocalNode, kQrtrClientPort, gRouter.remote_node, kWlfwService, gWlfwInstance, port,
       gWlfwInstance == kWlfwInstanceWord ? "" : ": not the instance word this family was expected to announce, used anyway");
  // The service's exchange.
  WlfwIo io;
  io.ctx = nullptr;
  io.claim = wlfwClaim;
  io.send = wlfwSendMessage;
  io.file_size = wlfwFileSize;
  io.file_read = wlfwFileRead;
  io.memory = wlfwMemory;
  io.step = wlfwStepNote;
  io.event = wlfwEventNote;
  const uint64_t started = nowMs();
  if (!wlfwStart(gClient, io, gArgs.mode, gArgs.shadow)) {
    note("level 5: the first request was not sent: %s", wlfwErrorName(gClient.error));
    stage(-503, "level 5: the QMI exchange did not start");
    safeOff(gHw, power, opt);
    return -6;
  }
  int last = gClient.step;
  unsigned heard = 0;   // responses and indications so far: each one is progress, and the next request gets its own deadline
  deadline = nowMs() + gArgs.qmiTimeoutMs;
  while (gClient.step != kWlfwStepDone && gClient.step != kWlfwStepStopped) {
    bool stop = false;
    pump(stop);
    if (stop) {
      wlfwTimedOut(gClient);
      note("level 5: the chip reported a system error or its crash dump environment during %s", wlfwStepName(last));
      break;
    }
    const unsigned now = gClient.responses + gClient.indications;
    if ((gClient.step != last || now != heard) && !levelLate()) {   // a step moved or an answer came: a deadline of its own
      last = gClient.step;
      heard = now;
      deadline = nowMs() + gArgs.qmiTimeoutMs;
      continue;
    }
    if (nowMs() > deadline || levelLate()) {
      if (levelLate()) note("level 5: the level's hard deadline (%u ms) passed during %s", gArgs.deadlineMs, wlfwStepName(last));
      else if (wlfwQdssGiveUp(gClient)) {   // the trace configuration unanswered: given up, the ready indication still awaited
        note("level 5: the trace configuration's request %#06x (transaction %u) unanswered after %u ms: given up, waiting for "
             "the firmware's ready", gClient.pendingMessage, gClient.pending, gArgs.qmiTimeoutMs);
        last = gClient.step;
        deadline = nowMs() + gArgs.qmiTimeoutMs;
        continue;
      }
      wlfwTimedOut(gClient);
      break;
    }
    IOSleep(2);
  }
  // What the firmware said, and what was sent to it.
  if (gClient.error == kWlfwErrTimeout && gClient.timedOutWaiting) {
    char what[120] = "";
    if (gClient.outSegment >= 0)
      snprintf(what, sizeof what, ", %s segment %d of %u, %u bytes%s", gClient.outFile ? gClient.outFile : "?",
               gClient.outSegment + 1, gClient.outSegments, gClient.outBytes, gClient.outLast ? ", the last" : "");
    note("level 5: unanswered: request %#06x (transaction %u%s), sent %llu ms before the stop", gClient.pendingMessage,
         gClient.pending, what, (unsigned long long)(nowMs() - gWlfwSentAt));
  }
  note("level 5: the chip: chip %#x family %#x system-on-chip %#x, board %#x, %u radio(s)%s", gClient.chipId,
       gClient.chipFamily, gClient.socId, gClient.boardId, gClient.radios, gClient.singleChipMlo ? ", single-chip multi-link" : "");
  note("level 5: board identifiers: the chip's capability %s%#x, the radio's capability %s%#x (only noted); board file %s",
       gClient.haveCapBoard ? "" : "gave none, so ", gClient.haveCapBoard ? gClient.capBoardId : kBoardIdDefault,
       gClient.havePhyBoard ? "" : "gave none: ", gClient.havePhyBoard ? gClient.phyBoardId : kBoardIdDefault,
       wlfwBoardFile(gClient.boardId, nullptr));
  if (gClient.qdssRefused || gClient.qdssSkipped || gClient.qdssTimedOut)
    note("level 5: the trace configuration %s; the exchange went on without it",
         gClient.qdssSkipped ? "is not on the partition" : gClient.qdssRefused ? "was refused" : "was not answered");
  if (gClient.qdssRefused) note("level 5: its refusal: result %u error %#x", gClient.qdssResult, gClient.qdssError);
  note("level 5: the firmware: version %#x built %s, capabilities %#llx, status %#llx, %u MAC(s), %u mV", gClient.fwVersion,
       gClient.buildTimestamp[0] ? gClient.buildTimestamp : "?", (unsigned long long)gClient.fwCaps,
       (unsigned long long)gClient.fwStatus, gClient.macs, gClient.voltageMv);
  note("level 5: memory: %u request(s), %u region(s) of %u KiB asked for, %u KiB given%s", gClient.memRequests,
       gClient.regionCount, gClient.regionBytes / 1024, gClient.regionGiven / 1024,
       gClient.memDelayed ? " (the last answer asked the firmware to come back with smaller pieces)" : "");
  for (uint32_t i = 0; i < gClient.regionCount && i < 8; ++i)
    note("level 5: region %u: %u KiB of %s at %#llx", i, gClient.regions[i].size / 1024,
         wlfwMemTypeName(gClient.regions[i].type), (unsigned long long)gClient.regions[i].address);
  note("level 5: %u requests, %u responses, %u indications, %u download segments, %u shadow registers, %u unexpected messages",
       gClient.requests, gClient.responses, gClient.indications, gClient.segments, gClient.shadowCount, gClient.unexpected);
  note("level 5: the transport: %u packets out, %u in, %u completions, %u commands, %u events dropped, %u refused claims; "
       "the router: %u packets in, %u data, %u unrouted, %u refused sends",
       gIpcr.sent, gIpcr.received, gIpcr.txDone, gIpcr.commands, gIpcr.dropped, gIpcr.refused, gRouter.rx_packets,
       gRouter.rx_data, gRouter.rx_unrouted, gRouter.tx_refused);
  // The firmware's router asks for a RESUME_TX on its fifth packet to our port and sends nothing more from its tenth without one
  // (P21): every request answered, none owed, is the healthy reading.
  note("level 5: flow control: %u data packet(s) asked for an answer, %u RESUME_TX sent (from our port %#x to the sender, naming "
       "node %u port %#x), %s",
       gRouter.rx_confirms, gRouter.resumes_sent, kQrtrClientPort, kQrtrLocalNode, kQrtrClientPort,
       gRouter.resume_owed ? "one still owed (the link refused it)" : "none owed");
  const bool done = gClient.step == kWlfwStepDone;
  note("level 5: %s after %llu ms (step %d: %s%s%s)", done ? "the firmware is ready" : "stopped",
       (unsigned long long)(nowMs() - started), done ? gClient.step : gClient.failedStep,
       wlfwStepName(done ? gClient.step : gClient.failedStep), done ? "" : ", ", done ? "" : wlfwErrorName(gClient.error));
  note("level 5: MHI after mission mode: %u M0 event(s) answered by ringing every doorbell, %u M1 answered with M2", gM0Events,
       gM1Answers);
  ipcrReport(done ? "at the firmware's ready" : "where it stopped");
  if (!done) {
    if (gChipDead) {
      crashReport("level 5, during the QMI exchange");
    } else {   // the chip lives: whether its DMA left the window or a request was refused on the link is in these
      rasReport();
      if (gLinkState) dmaReport(*gLinkState);
      msiSinkReport("where level 5 stopped");
    }
    safeOff(gHw, power, opt);
    return -7;
  }
  return 0;
}

// ============================================== level 6: the copy engine, the host transport and the firmware's messages
// The rings come out of the same window as levels 4 and 5 (the firmware's regions are already in it), the pipes are the ones
// level 5 described to the firmware, and everything is polled from this one thread: no MSI, no interrupt, no allocation after
// the window. Every wait is bounded by piano-wifi-wmitimeout (and piano-wifi-scanwait for the scan).
Ce gCe;
HttRx gHtt;                         // the receive rings and their buffers (P23), out of the same window
bool gHttPlanned;
Link gLink;
volatile bool gLinkRunning;         // the ladder's thread is inside level 6: the power hook asks it to stop instead of racing
volatile uint32_t gStopRequested;   // set by the power hook, read by the ladder each turn

void linkNote(void *, int step, const char *what)
{
  piano_wifi_stage = step;
  note("step %d: %s", step, what);
  deferPhase();
}

// The scan's result in the registry: one entry per network, and a line for the log. The addresses are published whole (the
// macOS side needs them to join later) and masked in the log unless piano-wifi-bssid=1 (docs/drivers/wifi.md section 11).
void publishScan(IOService *owner, const ScanList &list, bool full)
{
  OSArray *array = OSArray::withCapacity(list.count ? list.count : 1);
  if (!array) return;
  for (uint32_t i = 0; i < list.count; ++i) {
    const ScanEntry &entry = list.entries[i];
    OSDictionary *one = OSDictionary::withCapacity(8);
    if (!one) break;
    char bssid[24] = {};
    scanFormatBssid(entry, true, bssid, sizeof bssid);
    OSString *ssid = OSString::withCString(entry.ssidLength ? entry.ssid : "");
    OSString *address = OSString::withCString(bssid);
    OSNumber *channel = OSNumber::withNumber(uint64_t(entry.channel), 32);
    OSNumber *freq = OSNumber::withNumber(uint64_t(entry.freq), 32);
    OSNumber *rssi = OSNumber::withNumber(uint64_t(uint32_t(entry.rssi)), 32);
    OSNumber *beacons = OSNumber::withNumber(uint64_t(entry.beacons), 32);
    OSNumber *probes = OSNumber::withNumber(uint64_t(entry.probes), 32);
    if (ssid) one->setObject("ssid", ssid);
    if (address) one->setObject("bssid", address);
    if (channel) one->setObject("channel", channel);
    if (freq) one->setObject("freq", freq);
    if (rssi) one->setObject("rssi", rssi);
    if (beacons) one->setObject("beacons", beacons);
    if (probes) one->setObject("probes", probes);
    one->setObject("hidden", entry.hidden ? kOSBooleanTrue : kOSBooleanFalse);
    OSSafeReleaseNULL(ssid);
    OSSafeReleaseNULL(address);
    OSSafeReleaseNULL(channel);
    OSSafeReleaseNULL(freq);
    OSSafeReleaseNULL(rssi);
    OSSafeReleaseNULL(beacons);
    OSSafeReleaseNULL(probes);
    array->setObject(one);
    one->release();
  }
  owner->setProperty("piano-wifi-scan", array);
  array->release();
  char line[200];
  snprintf(line, sizeof line, "%u network(s), %u frame(s) (%u beacon, %u probe), %u ignored, %u dropped", list.count,
           list.frames, list.beacons, list.probes, list.ignored, list.dropped);
  owner->setProperty("piano-wifi-scan-summary", line);
  for (uint32_t i = 0; i < list.count; ++i) {
    const ScanEntry &entry = list.entries[i];
    char bssid[24] = {};
    scanFormatBssid(entry, full, bssid, sizeof bssid);
    // A network's name is the owner's environment as much as its address: the log carries its length only, unless the owner
    // asked for whole names and addresses (piano-wifi-bssid=1); the registry property keeps both for the tablet's own tool.
    char name[48] = {};
    if (full)
      snprintf(name, sizeof name, "\"%s\"", entry.ssidLength ? entry.ssid : "");
    else
      snprintf(name, sizeof name, "of %u byte(s)", unsigned(entry.ssidLength));
    note("level 6: network %u: a name %s%s, bssid %s channel %u (%u MHz) signal %d dBm, %u beacon(s) %u probe(s)", i, name,
         entry.hidden ? " (it does not say its name)" : "", bssid, entry.channel, entry.freq, int(entry.rssi), entry.beacons,
         entry.probes);
  }
}

// The teardown: the scan stopped and the virtual device deleted, waited for (bounded), then the rings disabled.
void linkTeardown()
{
  const uint64_t deadline = nowMs() + 1000;
  bool idle = false;
  while (!idle && nowMs() < deadline) {
    idle = linkStopRequest(gLink, nowMs());
    if (!idle) IOSleep(5);
  }
  linkDisable(gLink);
  note("level 6: the teardown: the scan %s, the virtual device %s, the rings disabled%s",
       !gLink.scanStarted ? "was not running" : gLink.scanTimedOut ? "was stopped by this driver" : "ended by itself",
       gLink.deleted ? "deleted" : "was not created",
       idle ? "" : " (the chip had not taken everything: the rings were disabled anyway)");
}

int level6(IOService *owner, PowerState &power, const Options &opt)
{
  stage(600, "level 6: the copy engine, the host transport and the firmware's messages");
  // P23: with the receive buffers asked for, pipe 4 (the data interface's messages out) gets a ring too, and the receive rings
  // and their buffers come out of the window and are filled before anything is announced. A window without room for them
  // only leaves them out.
  const bool wantRx = (gArgs.l6pre & kPreRxRings) != 0;
  if (!cePlan(gCe, gArena, wantRx ? kCePipeMaskWithHtt : kCePipeMaskLevel6)) {
    stage(-600, "level 6: the DMA window has no room for the copy engine's rings and buffers");
    safeOff(gHw, power, opt);
    return -1;
  }
  gHttPlanned = false;
  if (wantRx) {
    gHttPlanned = httPlan(gHtt, gArena, (gArgs.l6pre & kPreRbmSw1) != 0);
    if (gHttPlanned) httFill(gHtt);
    note("level 6: the receive rings: %s (a refill ring of %u buffers of %u bytes for the firmware, %u radio buffer rings of "
         "%u, an error ring of %u, %u status rings of %u buffers; the buffers return to %s)",
         gHttPlanned ? "planned and filled, announced after the first scan" : "not planned: the window has no room for them",
         kRxRefillEntries - 1, kRxBufferBytes, kRxRadios, kRxMacBufEntries, kRxErrDstEntries, kRxRadios,
         kRxMonStatusEntries - 1, (gArgs.l6pre & kPreRbmSw1) ? "software ring 1" : "the firmware's own manager");
  }
  ceProgram(gCe, gChipIo);
  unsigned posted = 0;
  for (unsigned i = 0; i < kCeHostPipeCount; ++i) posted += cePostRx(gCe, gChipIo, i);
  note("level 6: %u pipes: %u, %u, %u and %u; %u receive buffers posted, %u register writes", kCeHostPipeCount,
       kCeHostPipes[0].pipe, kCeHostPipes[1].pipe, kCeHostPipes[2].pipe, kCeHostPipes[3].pipe, posted, gCe.writes);
  LinkOptions options;
  options.timeoutMs = gArgs.wmiTimeoutMs;
  options.scanWaitMs = gArgs.scanWaitMs;
  options.with5GHz = gArgs.band != 0;
  options.passive = true;
  options.pre = gArgs.l6pre;
  options.regWaitMs = gArgs.l6regwait;
  options.paceMs = gArgs.l6pace;
  options.holdMs = gArgs.l6hold;
  options.htt = gHttPlanned ? &gHtt : nullptr;
  note("level 6: before the virtual device (piano-wifi-l6pre=%#x): hardware mode %u, the radio's parameters %u, the channel "
       "list %u (the regulatory event waited for %u, up to %u ms), a usable device address %u, paced %u ms, no scan %u",
       gArgs.l6pre, (gArgs.l6pre & kPreHwMode) ? 1u : 0u, (gArgs.l6pre & kPrePdevParams) ? 1u : 0u,
       (gArgs.l6pre & kPreChanList) ? 1u : 0u, (gArgs.l6pre & kPreRegWait) ? 1u : 0u, gArgs.l6regwait,
       (gArgs.l6pre & kPreVdevMac) ? 1u : 0u, (gArgs.l6pre & kPrePace) ? gArgs.l6pace : 0u,
       (gArgs.l6pre & kPreNoScan) ? 1u : 0u);
  gLink.note = linkNote;
  gLink.noteCtx = nullptr;
  const uint64_t started = nowMs();
  linkBegin(gLink, gCe, gChipIo, options, started);
  gLinkRunning = true;
  // Six message steps, the four of the radio's set-up (P22) and the scan — or the hold instead of it.
  // P23 adds the data interface's connection, two statistics waits, its version, the rings and a second scan.
  const uint64_t overall = started + uint64_t(gArgs.wmiTimeoutMs) * 12 + gArgs.l6regwait +
                           ((gArgs.l6pre & kPreNoScan) ? gArgs.l6hold : gArgs.scanWaitMs) +
                           ((gArgs.l6pre & kPreRxRings) ? gArgs.scanWaitMs + 4000u : 0u) +
                           ((gArgs.l6pre & kPreDiag) ? 2000u : 0u);
  int step = gLink.step;
  while (step != kLinkDone && step != kLinkStopped) {
    if (__atomic_load_n(&gStopRequested, __ATOMIC_ACQUIRE)) {
      note("level 6: a suspend asked the radio to stop during %s", linkStepName(step));
      break;
    }
    step = linkRun(gLink, nowMs());
    if (mhiHealth(gMhiIo, gBoot.bhi).dead()) {   // MHI still carries the firmware's health while the copy engine runs
      note("level 6: the chip reported a system error or its crash dump environment during %s", linkStepName(step));
      gChipDead = true;
      break;
    }
    if (nowMs() > overall || levelLate()) {
      note("level 6: the ladder as a whole ran out of time during %s", linkStepName(step));
      break;
    }
    if (step != kLinkDone && step != kLinkStopped) IOSleep(2);
  }
  // What the firmware said, and what the scan found.
  note("level 6: the transport: %u credits of %u bytes, %u endpoints, version %#x; our endpoint %d (pipes %u out, %u in), "
       "%u credit reports (%u credits), %u control messages",
       gLink.htc.credits, gLink.htc.creditSize, gLink.htc.endpoints, gLink.htc.version, gLink.endpoint,
       gLink.endpoint > 0 ? gLink.htc.ep[gLink.endpoint].ulPipe : 0u,
       gLink.endpoint > 0 ? gLink.htc.ep[gLink.endpoint].dlPipe : 0u, gLink.htc.creditReports, gLink.htc.creditsGiven,
       gLink.controlMessages);
  note("level 6: the chip: %u radio chain(s), board %#x, high-throughput %#x very-high-throughput %#x, system %#x, "
       "%u scan channel(s), %u radio(s), chain mask %#x, %u memory request(s)",
       gLink.caps.rfChains, gLink.caps.boardId, gLink.caps.htCaps, gLink.caps.vhtCaps, gLink.caps.sysCaps,
       gLink.caps.scanChannels, gLink.caps.supportedMacs, gLink.caps.chainMask, gLink.caps.memRequests);
  if (gLink.ready.seen) {
    char mac[24] = {};
    ScanEntry asAddress;
    bool zero = true;
    for (unsigned i = 0; i < 6; ++i) {
      asAddress.bssid[i] = gLink.ready.mac[i];
      zero = zero && gLink.ready.mac[i] == 0;
    }
    scanFormatBssid(asAddress, gArgs.bssid != 0, mac, sizeof mac);
    note("level 6: the address behind the ready event: %s", gLink.ready.extraMacs
             ? (gLink.ready.extraMacSeen ? "usable (it is what the virtual device takes when the firmware's own is zeros)"
                                         : "announced but not usable (zeros or a group address)")
             : "none announced");
    note("level 6: the firmware is ready: status %#x, %u peer(s), %u extra address(es); the chip's own address %s%s",
         gLink.ready.status, gLink.ready.totalPeers, gLink.ready.extraMacs, zero ? "is all zeros" : mac,
         zero ? " (this chip's firmware reports none: the unit's own is in the persist partition, section 13.1)" : "");
    // An address the firmware does report is what the macOS side's en1 takes (PianoWifiEthernet); it is never logged whole
    // unless piano-wifi-bssid=1, and never committed (section 11).
    if (!zero) {
      OSData *bytes = OSData::withBytes(gLink.ready.mac, sizeof gLink.ready.mac);
      if (bytes) {
        owner->setProperty("piano-wifi-mac", bytes);
        bytes->release();
      }
    }
  }
  // The radio's set-up (P22, docs section 10.27): what went out before the virtual device, and what the firmware's own
  // regulatory event said. The device's address is never printed, only where it came from.
  note("level 6: the radio's set-up: hardware mode %s%s, %u of %u parameter(s), the channel list %s; the virtual device's "
       "address comes from %s; the last command went out at step %d",
       gLink.hwModeSent ? "sent" : "not sent",
       gLink.hwModeSent ? (gLink.hwModeAnswered ? gLink.hwModeStatus ? " (refused)" : " (accepted)" : " (not answered)") : "",
       gLink.paramsSent, kPdevParamCount,
       gLink.chanListSent ? (gLink.chanListFromEvent ? "sent from the firmware's regulatory event" : "sent from this driver's "
                                                                                                     "own list")
                          : "not sent",
       gLink.vdevMacFrom, gLink.lastCommandStep);
  if (gLink.chanListSent)
    note("level 6: the channel list: %u channel(s) (%u passive, %u radar), %u left out of the 2.4 GHz band and %u of the "
         "5 GHz band",
         gLink.chanList.count, gLink.chanList.passive, gLink.chanList.radar, gLink.chanList.left24, gLink.chanList.left5);
  if (gLink.reg.seen)
    note("level 6: the regulatory domain: country \"%s\" (%#x), country number %u, domain %#x, radar region %u, status %u, "
         "radios %u, bandwidths 2.4 GHz %u-%u MHz 5 GHz %u-%u MHz, %u rule(s) (%u of the 2.4 GHz band, %u of the 5 GHz band; "
         "%u read, %u not read)%s",
         gLink.reg.alpha2, gLink.reg.country, gLink.reg.countryId, gLink.reg.domainCode, gLink.reg.dfsRegion,
         gLink.reg.status, gLink.reg.numPhy, gLink.reg.minBw24, gLink.reg.maxBw24, gLink.reg.minBw5, gLink.reg.maxBw5,
         gLink.reg.rules24 + gLink.reg.rules5, gLink.reg.rules24, gLink.reg.rules5, gLink.reg.ruleCount,
         gLink.reg.rulesDropped, gLink.reg.extended ? " (the extended event)" : "");
  else
    note("level 6: the regulatory domain: the firmware sent no regulatory event%s",
         (gArgs.l6pre & kPreChanList) ? " (the channel list is this driver's own)" : "");
  for (uint32_t i = 0; i < gLink.reg.ruleCount && i < 8; ++i)
    note("level 6: regulatory rule %u: %u-%u MHz, up to %u MHz wide, %d dBm, flags %#x%s%s%s", i, gLink.reg.rule[i].startKhz,
         gLink.reg.rule[i].endKhz, gLink.reg.rule[i].maxBwMhz, int(gLink.reg.rule[i].powerDbm), gLink.reg.rule[i].flags,
         (gLink.reg.rule[i].flags & kRegRuleFlagDisabled) ? " disabled" : "",
         (gLink.reg.rule[i].flags & kRegRuleFlagNoIr) ? " passive" : "",
         (gLink.reg.rule[i].flags & kRegRuleFlagRadar) ? " radar" : "");
  if (gLink.unknownEvents) {
    char ids[120] = {};
    uint32_t at = 0;
    for (uint32_t i = 0; i < gLink.unknownEvents && i < 8 && at + 12 < sizeof ids; ++i)
      at += uint32_t(snprintf(ids + at, sizeof ids - at, "%s%#x", i ? ", " : "", gLink.unknownEvent[i]));
    note("level 6: %u event(s) this level knows nothing about: %s", gLink.unknownEvents, ids);
  }
  // P23: what each scan round saw (the first without the receive buffers, the second with them), the radio's own counters,
  // the data interface and the rings, and every event identifier with its count.
  if (gArgs.l6pre & kPreRxRings)
    note("level 6: the data interface: %s, version %s%u.%u, %u message(s) out, %u in (types %#x %#x %#x %#x), %u peer map(s); "
         "rings announced %u of %u; refill: %u buffer(s) given, the firmware's tail at %u; status buffers given back %u; error "
         "entries %u; radio buffer rings filled to %u and %u",
         gLink.httRefused ? "refused" : gLink.httEndpoint > 0 ? "connected" : "not connected",
         gHtt.versionSeen ? "" : "not answered, ", gHtt.versionMajor, gHtt.versionMinor, gLink.httMessages, gHtt.t2hMessages,
         gHtt.t2hTypes[0], gHtt.t2hTypes[1], gHtt.t2hTypes[2], gHtt.t2hTypes[3], gHtt.peerMaps, gHtt.announced, kHttRings,
         gHtt.refillPosted, gHtt.refillTaken, gHtt.statusRecycled, gHtt.errorsSeen, gHtt.macBufFilled[0],
         gHtt.macBufFilled[1]);
  for (uint32_t r = 0; r < 2; ++r) {
    const ScanRound &round = gLink.rounds[r];
    if (!round.ran) continue;
    note("level 6: scan round %u (%s): %s, %u scan event(s), %u channel visit(s), %u management frame(s), %u network(s) new "
         "in this round",
         r + 1, r ? "with the receive buffers" : "before the receive buffers",
         round.completed ? wmiScanReasonName(round.reason) : round.started ? "it did not report an end" : "it did not start",
         round.scanEvents, round.channels, round.mgmtFrames, round.networks);
    // The channel visits: frequency, noise floor, and the busy and receiving shares of the visit's cycles (a radio that hears
    // the air has busy and receiving cycles even when it keeps no frame).
    for (uint32_t i = 0; i < round.visits; i += 6) {
      char line[260] = {};
      uint32_t at = 0;
      for (uint32_t k = i; k < round.visits && k < i + 6 && at + 40 < sizeof line; ++k) {
        const ChanVisit &v = round.visit[k];
        const uint32_t busy = v.cycles ? uint32_t(uint64_t(v.busy) * 100 / v.cycles) : 0;
        const uint32_t rx = v.cycles ? uint32_t(uint64_t(v.rxFrames) * 100 / v.cycles) : 0;
        at += uint32_t(snprintf(line + at, sizeof line - at, "%s%u: nf %d busy %u%% rx %u%%", k > i ? "; " : "", v.freq,
                                int(v.noiseFloor), busy, rx));
      }
      note("level 6: round %u visits: %s", r + 1, line);
    }
    if (round.stats.seen)
      note("level 6: round %u radio: nf %d, cycles %u (busy %u, receiving %u, transmitting %u), PHY errors %u; received: "
           "statuses %u, MPDUs to the host %u and to the firmware %u (MSDUs %u and %u), MPDU errors %u, PHY errors %u (%u "
           "dropped)",
           r + 1, int(round.stats.noiseFloor), round.stats.cycles, round.stats.busyCycles, round.stats.rxCycles,
           round.stats.txCycles, round.stats.phyErrCount, round.stats.statusRcvd, round.stats.httMpdus, round.stats.locMpdus,
           round.stats.httMsdus, round.stats.locMsdus, round.stats.mpduErrs, round.stats.phyErrs, round.stats.phyErrDrop);
    else if (gArgs.l6pre & kPreDiag)
      note("level 6: round %u radio: the statistics were not answered", r + 1);
  }
  for (uint32_t i = 0; i < gLink.eventKinds; i += 8) {
    char line[200] = {};
    uint32_t at = 0;
    for (uint32_t k = i; k < gLink.eventKinds && k < i + 8 && at + 20 < sizeof line; ++k)
      at += uint32_t(snprintf(line + at, sizeof line - at, "%s%#x x%u", k > i ? ", " : "", gLink.eventCount[k].id,
                              gLink.eventCount[k].count));
    note("level 6: events by identifier: %s%s", line, gLink.eventKindsDropped && i + 8 >= gLink.eventKinds ? " (and more)" : "");
  }
  note("level 6: the scan: %u channel(s) asked for, started %u, %u scan event(s), %u channel visit(s), %s%s",
       gLink.request.channelCount, gLink.scanStarted ? 1u : 0u, gLink.scanEventsSeen, gLink.scanChannels,
       gLink.scanCompleted ? wmiScanReasonName(gLink.scanReason) : "it did not report an end",
       gLink.scanTimedOut ? " (after this driver stopped waiting for it)" : "");
  publishScan(owner, gLink.scan, gArgs.bssid != 0);
  note("level 6: %u commands, %u events (%u extended capability), %u management frames (%u refused by the firmware's own "
       "checks, %u unreadable), %u unexpected messages, %u messages dropped, %u credit waits; the copy engine: %u out "
       "(%u bytes), %u in (%u bytes), %u completions, %u refused claims, %u dropped",
       gLink.commands, gLink.events, gLink.extEvents, gLink.mgmtFrames, gLink.scan.badFrames, gLink.scan.ignored,
       gLink.unexpected, gLink.badMessages, gLink.creditWaits, gCe.sent, gCe.bytesOut, gCe.received, gCe.bytesIn,
       gCe.completions, gCe.refused, gCe.dropped);
  const bool done = gLink.step == kLinkDone;
  note("level 6: %s after %llu ms (step %d: %s%s%s)", done ? "the scan is done" : "stopped",
       (unsigned long long)(nowMs() - started), done ? gLink.step : gLink.failedStep,
       linkStepName(done ? gLink.step : gLink.failedStep), done ? "" : ", ", done ? "" : linkErrorName(gLink.error));
  linkTeardown();
  gLinkRunning = false;
  char state[200];
  snprintf(state, sizeof state, "step %d %s%s%s; %u network(s), endpoint %d, %u credits", done ? gLink.step : gLink.failedStep,
           linkStepName(done ? gLink.step : gLink.failedStep), done ? "" : ": ", done ? "" : linkErrorName(gLink.error),
           gLink.scan.count, gLink.endpoint, gLink.htc.credits);
  owner->setProperty("piano-wifi-level6", state);
  if (!done) {
    if (gChipDead) crashReport("level 6");
    safeOff(gHw, power, opt);
    return -2;
  }
  return 0;
}

// The power contract (package/include/PianoPowerContract.h): on a suspend the radio stops. The ladder's own thread owns the
// hardware, so the hook asks it to stop and waits for it (bounded); then the scan and the virtual device are gone, the rings
// are disabled, and WLAN_EN goes low with PERST# asserted. Nothing is brought back on the wake.
PowerState gPowerStorage;       // the rails and pins as level 2 left them: what safeOff needs, from any thread
PowerState *gPowerState;
Options gPowerOptions;
bool gRadioStopped;

void stopRadio(const char *why)
{
  if (gRadioStopped || gArgs.level < 2) return;
  gRadioStopped = true;
  __atomic_store_n(&gStopRequested, 1u, __ATOMIC_RELEASE);
  const uint64_t deadline = nowMs() + 2000;
  while (gLinkRunning && nowMs() < deadline) IOSleep(10);
  note("%s: stopping the radio (the ladder's thread %s)", why, gLinkRunning ? "did not finish in 2 s" : "is done");
  if (!gLinkRunning && gLink.ce) linkTeardown();
  if (gPowerState && gWin[kWinTlmm].va) {
    safeOff(gHw, *gPowerState, gPowerOptions);
    nocVote(false);   // PERST# is asserted: nothing crosses the PCIe NoC any more
    // The chip is given up for good, so the rails are given back: Bluetooth keeps them up if it is running, and with
    // piano-wifi-release=1 the last owner withdraws the enables this driver added.
    const unsigned left = wcnPutOwners();
    note("%s: WLAN_EN is low and PERST# asserted; the rails are given back (owners left %#x) and the Wi-Fi path stays off "
         "until the next boot",
         why, left);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoWifi : public IOService {
  OSDeclareDefaultStructors(PianoWifi)
  void run();
  static void thread(void *self, wait_result_t);

  static void stopThread(void *self, wait_result_t);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
};

OSDefineMetaClassAndStructors(PianoWifi, IOService)

IOService *PianoWifi::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoWifi::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-wifi=%u spmi=%u smmu=%u cmddb=%u hwcg=%u delay=%u repeat=%u samples=%u (WCN7860 %04x:%04x on PCIe0 %#llx)",
       gArgs.level, gArgs.spmi, gArgs.smmu, gArgs.cmddb, gArgs.hwcg, gArgs.delay, gArgs.repeat, gArgs.samples, kEndpointVendor,
       kEndpointDevice, (unsigned long long)kParf);
  if (gArgs.level >= 2)
    note("levels: 1 survey, 2 power, 3 link%s%s%s (gen %u, physeq %u, rcg %u, blind %u, release %u, stop %u, dma %u MiB, "
         "phase timeout %u ms, qmi timeout %u ms, iova %u, mode %u, shadow %u, wmi timeout %u ms, scan wait %u ms, band %u, "
         "bssid %u)",
         gArgs.level >= 4 ? ", 4 MHI firmware" : "", gArgs.level >= 5 ? ", 5 QMI" : "",
         gArgs.level >= 6 ? ", 6 the copy engine, the transport and a scan" : "", gArgs.gen, gArgs.physeq, gArgs.rcg,
         gArgs.blind, gArgs.release, gArgs.stop, gArgs.dmaMib, gArgs.fwTimeoutMs, gArgs.qmiTimeoutMs, gArgs.iova, gArgs.mode,
         gArgs.shadow, gArgs.wmiTimeoutMs, gArgs.scanWaitMs, gArgs.band, gArgs.bssid);
  if (gArgs.level >= 3)
    note("options: noc %u (the PCIe NoC's bus clock manager voted before level 3's clocks)", gArgs.noc);
  if (gArgs.level >= 4)
    note("options: socreset %u (the chip's power-on reset before BHI), msi %u (MSI into a sink page), ctxearly %u (MHI contexts "
         "before the doorbell), ras %u (TLP counters)",
         gArgs.socreset, gArgs.msi, gArgs.ctxearly, gArgs.ras);
  if (gArgs.level >= 4)
    note("options: rddm %u (crash dump), defer %u (lines held from the doorbell, printed by a helper thread), wake %u (device "
         "wake), deadline %u ms (each level's hard deadline)",
         gArgs.rddm, gArgs.defer, gArgs.wake, gArgs.deadlineMs);
  if (gArgs.level > 6) note("piano-wifi=%u: levels above 6 are not implemented; running up to level 6", gArgs.level);
  note("level 1: loads only, nothing written%s", gArgs.spmi || gArgs.level >= 2 ? "; SPMI observer read commands for the PMIC part"
                                                                               : "");
  setProperty("piano-wifi", gArgs.level, 32);
  // From level 2 on something is powered, so the driver joins the power tree: its kOff stops the radio (the power contract,
  // package/include/PianoPowerContract.h). Below level 2 nothing of the chip is on and there is nothing to do.
  if (gArgs.level >= 2) {
    const IOReturn rc = pianopower::join(this);
    if (rc != kIOReturnSuccess) note("the power tree refused this driver (%#x): a suspend will not stop the radio", rc);
  }
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoWifi::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;
  }
  thread_deallocate(t);
  return true;
}

void PianoWifi::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-wifi");
  static_cast<PianoWifi *>(self)->run();
}

// The power contract: kOff stops the radio (and nothing comes back on the wake, §10.14). Stopping it waits for the ladder's own
// thread (at most 2 s) and then for the chip to take the two commands (at most 1 s), which is longer than the one second a
// synchronous answer may take — so the answer is a deadline in microseconds and an acknowledgement when it is done
// (docs/power.md §4), and the waiting happens on a thread of its own.
IOReturn PianoWifi::setPowerState(unsigned long state, IOService *)
{
  if (state != pianopower::kOff || gArgs.level < 2 || gRadioStopped) return pianopower::kAckNow;
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoWifi::stopThread, this, &t) != KERN_SUCCESS) {
    stopRadio("suspend");   // no thread to be had: do it here, late acknowledgement and all
    return pianopower::kAckNow;
  }
  thread_deallocate(t);
  return 4000000;
}

void PianoWifi::stopThread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-wifi-stop");
  PianoWifi *driver = static_cast<PianoWifi *>(self);
  stopRadio("suspend");
  driver->acknowledgeSetPowerState();
}

void PianoWifi::systemWillShutdown(IOOptionBits specifier)
{
  if (specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) stopRadio("shutdown");
  IOService::systemWillShutdown(specifier);
}

void PianoWifi::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "GCC, TCSR and TLMM (read only)");
  Survey s;
  surveyClocks(s, load, gArgs.hwcg != 0);
  surveyPins(s, load);
  reportClocks(s.clocks);
  reportPins(s);
  stage(2, "PCIe0 controller: PARF, DBI, iATU (read only, only when powered and clocked)");
  surveyController(s, load);
  reportController(s.controller);
  stage(3, "PCIe0 PHY: PCS status (read only, only when powered and clocked)");
  surveyPhy(s, load);
  reportPhy(s.phy);
  if (gArgs.smmu) {
    stage(4, "SMMU streams 0x1400/0x1401 (read only)");
    reportSmmu();
  }
  // Levels 2 and up need both: the rails' RPMh addresses and the PPIDs their regulators answer on.
  if (gArgs.cmddb || gArgs.level >= 2) {
    stage(5, "AOP command database: the supplies' RPMh resources (read only)");
    reportCmdDb();
  }
  if (gArgs.spmi || gArgs.level >= 2) {
    stage(6, "the supplies' PMIC regulators and the BT_EN / sleep clock pins over SPMI (observer reads)");
    // Under the PMU lock: PianoBluetooth reads the same peripherals through the same observer channel.
    const bool held = !piano_wcn_pmu_lock(&gWcn.rails, 1);
    reportPmic();
    if (held) piano_wcn_pmu_lock(&gWcn.rails, 0);
  }
  char line[200];
  summary(s, line, sizeof line);
  setProperty("piano-wifi-survey", line);
  const PinDecode en = decodePin(s.pins[kPinWlanEn].cfg, s.pins[kPinWlanEn].io);
  note("summary: %s", line);
  note("verdict: WLAN_EN %s, PCIe0 %s, PHY %s, link %s: %s", en.out ? "driven high" : "low", onOff(gdscOn(s.clocks.gdsc)),
       onOff(gdscOn(s.clocks.phyGdsc)), linkUp(s.controller) ? "up" : "not up",
       linkUp(s.controller) ? "the boot stages left the link trained (level 2 may skip the power-up; check the endpoint first)"
                            : "level 2 must do the whole power-up (rails, WLAN_EN, domains, clocks, PHY, PERST#, LTSSM)");
  for (uint32_t i = 1; gArgs.repeat && i <= gArgs.samples; ++i) {
    IOSleep(gArgs.repeat * 1000);
    Survey again;
    survey(again, load, gArgs.hwcg != 0);
    summary(again, line, sizeof line);
    note("sample %u (+%u s): %s", i, i * gArgs.repeat, line);
  }
  stage(7, "the survey is done");
  note("level 1: %llu MMIO loads, %u SPMI observer commands, 0 other stores; %u pages mapped, %u map failures",
       (unsigned long long)gLoads, gSpmiStores, gPageCount, gMapFailures);
  if (gArgs.level < 2) {
    unmapAll();
    return;
  }

  // ---- level 2 and up: the first writes of this driver -----------------------------------------------------------------
  const Options opt = options();
  PowerState &power = gPowerStorage;   // a file-scope state, so that the power hook's safeOff has it on its own thread
  for (unsigned i = 0; i < kRailCount; ++i) {
    power.rails[i].ppid = gRailPpid[i];
    power.rails[i].address = gRailAddress[i];
  }
  gPowerOptions = opt;
  gPowerState = &power;
  if (!mapWindows() || !mapRsc()) {
    stage(-8, "level 2: the windows or apps DRV-2 could not be mapped; nothing was written");
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 2: the six PMU rails, PERST# low, WLAN_EN; the PHY's two LDOs are read only (one is the UFS PHY's PLL)");
  // The rails through the shared helper first (PianoBluetooth may hold them already: then only a read-back), then level 2's own
  // steps, the PMU lock across both: BT_EN never rises between our rails and WLAN_EN. powerUp's steps 201..203 then find the
  // rails up and send the same votes again (apps DRV-2 holds one vote per resource), and enable nothing of their own.
  for (unsigned i = 0; i < kRailCount && i < PIANO_WCN_RAIL_COUNT; ++i) gWcn.rails.ppid[i] = gRailPpid[i];
  gWcn.rails.blind = int(gArgs.blind);
  int rc = piano_wcn_pmu_lock(&gWcn.rails, 1);
  if (!rc) {
    rc = piano_wcn_rails_get(&gWcn.rails, PIANO_WCN_WLAN);
    if (rc) {
      power.step = gWcn.rails.step ? gWcn.rails.step : -kStepRailsRead;
      power.error = rc == PianoWcnBusy ? kErrRailVote : rc;
    } else {
      rc = powerUp(power, gHw, opt);
      if (rc) wcnPut();
    }
    piano_wcn_pmu_lock(&gWcn.rails, 0);
  } else {
    power.step = -kStepRailsRead;
    power.error = kErrRailVote;
    note("level 2: the PMU lock stayed held by %#x (PianoBluetooth): nothing written", piano_wcn_load(&piano_wcn_pmu_owner));
  }
  reportPower(power);
  reportSpmiLock("level 2");
  writeSummary("level 2");
  if (rc) {
    stage(power.step, "level 2 stopped here");
    note("level 2: FAILED at step %d (%s): WLAN_EN is low and PERST# asserted; nothing above this level ran", power.step,
         errorName(power.error));
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 2: done (rails voted, WLAN_EN high, PERST# low, SW_CTRL in %u)", power.swCtrlIo & 1u);
  if (gArgs.level < 3) {
    unmapAll();
    return;
  }

  // ---- level 3 ---------------------------------------------------------------------------------------------------------
  // The DMA path's bandwidth before the controller's clocks, as pcie-qcom does (WifiHw.h: the PCIe NoC's bus clock manager).
  nocVote(true);
  LinkState link;
  rc = linkUp(link, power, gHw, opt);
  reportLink(link);
  writeSummary("level 3");
  char state[200];
  snprintf(state, sizeof state, "level %u step %d%s%s link %s gen %u bar %llu KiB at %#llx", gArgs.level, link.step,
           link.error ? " error " : "", link.error ? errorName(link.error) : "",
           (link.debug1 & kPortDebug1LinkUp) ? "up" : "down", (link.linkStatus >> 16) & 0xfu,
           (unsigned long long)(link.barBytes / 1024), (unsigned long long)link.barAddress);
  setProperty("piano-wifi-link", state);
  if (rc) {
    stage(link.step, "level 3 stopped here");
    note("level 3: FAILED at step %d (%s): WLAN_EN is low and PERST# asserted", link.step, errorName(link.error));
    chipPut();
    unmapWindows();
    unmapAll();
    return;
  }
  note("level 3: done (L0 at Gen%u x%u, the endpoint answers 17cb:110e, BAR0 %llu KiB at %#llx)", (link.linkStatus >> 16) & 0xfu,
       (link.linkStatus >> 20) & 0x3fu, (unsigned long long)(link.barBytes / 1024), (unsigned long long)link.barAddress);
  if (gArgs.smmu) {
    stage(kStepBar + 1, "the SMMU: the endpoint's stream through a bypass context bank");
    if (!smmuStream(kStreamEndpoint)) {
      stage(-(kStepBar + 1), "level 3: the endpoint's SMMU stream was not set up; no DMA is started");
      safeOff(gHw, power, opt);
      chipPut();
      unmapWindows();
      unmapAll();
      return;
    }
    if (gArgs.smmu >= 2) (void)smmuStream(kStreamRoot);
  } else {
    note("level 3: piano-wifi-smmu=0: the SMMU is untouched (level 4 then needs stream 0x1401 to bypass already)");
  }
  if (gArgs.level < 4) {
    unmapAll();
    return;
  }

  // ---- level 4 ---------------------------------------------------------------------------------------------------------
  // The lines level 4 starts holding at the boot loader's doorbell (note(), deferStart) come out on every way out of the ladder
  // from here; and a ladder that ended well lets the device idle again (the wake doorbell back to 0, as the MHI host's last
  // release does), since nothing answers it after this thread is gone.
  struct LadderExit {
    bool released = false;
    ~LadderExit()
    {
      levelEnd();
      deferFlush();
    }
  } ladderExit;
  auto releaseWake = [&]() {
    if (!gArgs.wake || ladderExit.released || gChipDead) return;
    ladderExit.released = true;
    note("level %u: device wake %s", gArgs.level,
         mhiDevWake(gMhiIo, gMhiRegLen, false) ? "released (the chip may idle)" : "not released");
  };
  stage(400, "level 4: the DMA window, BAR0's MHI registers, the firmware over BHI/BHIe");
  if (!dmaWindow()) {
    stage(-400, "level 4: no DMA window; nothing was given to the endpoint");
    safeOff(gHw, power, opt);
    chipPut();
    unmapWindows();
    unmapAll();
    return;
  }
  if (gArgs.iova && !iovaWindow()) {
    stage(-400, "level 4: the window's 32-bit addresses were not set up; nothing was given to the endpoint");
    safeOff(gHw, power, opt);
    chipPut();
    unmapWindows();
    unmapAll();
    return;
  }
  levelBegin(4, gArgs.deadlineMs);
  rc = level4(link, power, opt);
  levelEnd();
  writeSummary("level 4");
  stage(rc ? -401 : 401, rc ? "level 4 stopped before mission mode" : "level 4: the firmware runs (mission mode)");
  note("level 4: %s%s", rc ? "the chip did not reach mission mode" : "the chip is in mission mode",
       rc ? "" : gArgs.level < 5 ? "; the ladder ends here (QMI is level 5, WMI and the station levels 6 and up)" : "");
  if (rc) chipPut();   // level 4's failures end in safeOff: WLAN_EN low
  if (rc || gArgs.level < 5) {
    if (!rc) releaseWake();
    unmapAll();
    return;
  }

  // ---- level 5 ---------------------------------------------------------------------------------------------------------
  levelBegin(5, gArgs.deadlineMs);
  rc = level5(power, opt);
  levelEnd();
  piano_wifi_firmware_close();
  if (rc) chipPut();   // level 5's failures end in safeOff too
  writeSummary("level 5");
  stage(rc ? -504 : 504, rc ? "level 5 stopped before the firmware was ready" : "level 5: the firmware is ready");
  note("level 5: %s; %s", rc ? "the firmware did not become ready" : "the firmware is ready",
       rc ? "the ladder ends here"
          : gArgs.level < 6 ? "the copy engine, the transport and the scan are level 6 (docs/drivers/wifi.md section 13)"
                            : "level 6 follows: the copy engine, the transport and a passive scan");
  if (rc || gArgs.level < 6) {
    if (!rc) releaseWake();
    unmapAll();
    return;
  }
  if (!gArgs.mode) {
    note("level 6: not run: the firmware's radio was not started (piano-wifi-mode=0); piano-wifi=6 needs it");
    releaseWake();
    unmapAll();
    return;
  }

  // ---- level 6 ---------------------------------------------------------------------------------------------------------
  // level 6's own overall bound (six message steps and the scan) is inside its hard deadline
  {
    const uint32_t own = gArgs.wmiTimeoutMs * 12 + gArgs.l6regwait +
                         ((gArgs.l6pre & kPreNoScan) ? gArgs.l6hold : gArgs.scanWaitMs) +
                         ((gArgs.l6pre & kPreRxRings) ? gArgs.scanWaitMs + 4000u : 0u) +
                         ((gArgs.l6pre & kPreDiag) ? 2000u : 0u) + 5000;
    levelBegin(6, own > gArgs.deadlineMs ? own : gArgs.deadlineMs);
  }
  rc = level6(this, power, opt);
  levelEnd();
  if (rc) chipPut();   // level 6's failures end in safeOff as well
  writeSummary("level 6");
  stage(rc ? -613 : 613, rc ? "level 6 stopped before the scan was done" : "level 6: the scan is done");
  note("level 6: %s; %s", rc ? "the scan did not finish" : "the scan is done",
       rc ? "the ladder ends here" : "association, keys and the data path are levels 7 and 8 (docs/drivers/wifi.md section 13)");
  if (!rc) {
    releaseWake();
    // The gate PianoWifiEthernet waits for: en1 appears (link down, with the scan list) only when level 6 came up.
    publishResource("PianoWifiLink");
    note("level 6: the resource PianoWifiLink is published: PianoWifiEthernet's en1 may come up (link down, the scan list in "
         "the property piano-wifi-scan; tooling/native/piano-wifi prints it)");
  }
  unmapAll();
}
