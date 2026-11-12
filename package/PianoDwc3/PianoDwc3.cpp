// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDwc3: a USB device-mode debug channel for native macOS on piano (SM8750) over the single USB-C port.
//
// The DWC3 core that the UEFI's debug fastboot used is taken over in device mode (no cold PHY bring-up: the firmware's
// power, clocks and PHY are assumed to still be on, and checked first) and enumerates as a composite device with two CDC-ACM
// functions, which the Mac's own CDC-ACM driver turns into /dev/cu.usbmodem* nodes:
//   port 0: the kernel console (the pstore-console ring of k27 0055/0058, else the message buffer) streamed live, with the
//           whole retained boot log replayed when the Mac opens the port, and a line-based kernel monitor (help, ioreg,
//           peek, reset, ...);
//   port 1: a byte pipe to user space, /dev/pianousb (piano-usb-shell=1), for a shell or `log stream` (docs);
//   with piano-usb-hid=1 a third port, input from the Mac into two HID devices; with piano-usb-net=1 a CDC-NCM network function
//   after the ports, whose macOS side is PianoUsbNet.cpp's IOEthernetController (an Ethernet link to the Mac).
// Apple's own device stack (IOUSBDeviceFamily, AppleUSBDeviceNCM) is not used: the macOS 27.0.1 VM kernel collection has no
// XDCI controller class to put under it (docs/drivers/usb-device-debug.md section 3).
//
// Default off. piano-usb=1 only reads (power, clocks, core, PHY, SMMU, repeater state into the console log); 2 takes the
// controller over; 3 also switches on GDSC/branch clocks it finds off. Every step is logged as "PianoDwc3: ...".
// The USB thread's wait between passes follows the link (UsbIdle.h, docs/power-irq.md): 1 ms only while something moves, up to
// 250 ms with no host; piano-usb-irq=1 lets the core's interrupt (SPI 133) end a wait; piano-usb-idle=0 is the old 1 ms loop.
// System sleep (docs/power.md 4, UsbSleep.h): at piano-usb=2 the service is in the power tree. With a host that has configured the
// device and piano-sleep-usb=1 (the default) the gadget stays as it is across a suspend (the console survives; the core's interrupt
// masked, polled until the freeze); otherwise off is a soft disconnect and on, once the glue's clocks read on, connects again (the
// Mac enumerates afresh). Every QSCRATCH write (the VBUS/session override, the UTMI clock select, a monitor poke into it) goes to the
// glue only while awake with its clocks on, else it is recorded and written after the wake (Ubuntu's K:7dffd78b342d: such a write
// with the glue unclocked never completes and resets the SoC).
// piano-usb-handoff=1 (default 0): PianoUsbHost may borrow the controller for host mode and give it back (UsbHandoff.h,
// docs/drivers/usb-host.md H3): released between passes on its ask (only while awake), the takeover run again on its reclaim.
// The cold start (UsbCold.h, docs/drivers/usb-device-debug.md section 17; piano-usb-cold=0|1|auto, default auto): before the takeover
// the USB thread reads the inherited state and, when it is not a running device controller (the normal power-on path, no ABL
// fastboot), brings the GDSC, clocks, rails (RPMh, under PianoA830's DRV-2 lock word), the eUSB2 repeater and the M31 PHY up from
// this driver's own tables; after RUN_STOP a link watch runs the cold start when no host resets the device within
// piano-usb-cold-wait seconds (at most 3 cold starts a boot). piano-usb-cold-sim=MASK first leaves the hardware as the normal path
// may (a test from ABL fastboot); the monitor's `usb cold [sim MASK]` does the same live.
// The role rule (C6, docs/forensics-2026-11-11.md section 7.6): no device register while GCTL reads another role, no xHCI register
// outside the host role. c2 and the takeover's gate leave the core in the device role (the xHCI halted, PRTCAPDIR, then the device
// soft reset; the block reset when that is not shown done); the gadget refuses outside the device role; coreRd/coreWr refuse too.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/select.h>
#include <sys/msgbuf.h>
#include <miscfs/devfs/devfs.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <stdarg.h>
#include "PianoUsbHw.h"
#include "Dwc3Gadget.h"
#include "SmmuStream.h"
#include "HidInput.h"
#include "HidDescriptors.h"   // package/PianoKeyboard: the keyboard (+ consumer) and pointer report descriptors, shared
#include "UsbNet.h"           // the network function's macOS side (PianoUsbNet.cpp)
#include "PianoBacklightClient.h"   // package/PianoBacklight: brightness keys to the backlight (piano-backlight-keys=1)
#include "UsbIdle.h"          // the USB thread's wait tiers
#include "UsbSleep.h"         // a system sleep's steps and the QSCRATCH guard
#include "UsbHandoff.h"       // the controller hand-off to PianoUsbHost (piano-usb-handoff=1)
#include "UsbCold.h"          // the cold start (piano-usb-cold)
#include "LogCursor.h"        // the console's reader of the kernel log ring
#include "BtRpmh.h"           // package/PianoBluetooth: one VRM transaction on apps DRV-2 (and PianoKeyboard's CmdDb.h lookup)
#include "ChargerPmic.h"      // package/PianoPower: the PMIC's Type-C block, found read only (only where no other kext reads it)
#include "../common/PianoIrq.h"
#include <PianoPowerContract.h>

using namespace pianousb;

extern "C" {
// Post-mortem breadcrumbs (also in the console log): the last takeover stage reached, negative when it failed there; the last cold
// start step (1..5 = c1..c5, 6 done), negative where its hard part failed.
volatile int32_t piano_dwc3_stage = 0;
volatile int32_t piano_dwc3_cold = 0;
// PianoA830's lock words (Sm8750GpuPower.c: the GCC branch/PLL vote words; Sm8750Rpmh.h: apps DRV-2's TCSs): PianoA830 links before
// PianoDwc3 (configs/k27-native-kexts.txt) and is a library of PianoDwc3.json, so the KC resolves both to A830's single copy, as for
// PianoTouch, PianoKeyboard, PianoWifi and PianoBluetooth. The protocol is A830's: 0 free, else the holder's tag, compare-exchange.
extern volatile uint32_t sm8750_gcc_vote_owner;
extern volatile uint32_t sm8750_rpmh_drv2_owner;
// sys/conf.h's KERNEL_PRIVATE declaration (bsd_stubs.c, exported by the kernel): /dev/pianousb's cdevsw marked kqueue-capable.
int cdevsw_setkqueueok(int, const struct cdevsw *, int);
}

class PianoUsbHID;   // the HID input devices (piano-usb-hid=1), below

namespace {

// ---------------------------------------------------------------------------------------------------------------- boot args
struct Args {
  uint32_t level;     // piano-usb
  uint32_t phy;       // piano-usb-phy=1: rerun the M31 eUSB2 PHY init before the core reset
  uint32_t smmu;      // piano-usb-smmu: 1 (default) stream 0x40 through a VA64 bypass bank, 0 leave the SMMU alone
  uint32_t utmi;      // piano-usb-utmi: 1 (default) the UTMI clock as PIPE clock (high-speed-only operation)
  uint32_t delay;     // piano-usb-delay=<s> before the takeover
  uint32_t shell;     // piano-usb-shell=1: /dev/pianousb on port 1
  uint32_t poke;      // piano-usb-poke=1: the monitor may write physical addresses
  uint32_t replay;    // piano-usb-replay: 1 (default) the first open replays the retained log, a later one resumes where the last
                      // session stopped; 2 every open replays it; 0 every session starts from now
  uint32_t phyread;   // piano-usb-phyread: 1 (default) read the M31 PHY in the probe
  uint32_t spmi;      // piano-usb-spmi: 1 (default) read the repeater over SPMI in the probe
  uint32_t pid;       // piano-usb-pid: USB product ID
  uint32_t trace;     // piano-usb-trace: events logged one by one after each bus reset (default 400, 0 off)
  uint32_t hid;       // piano-usb-hid=1: a keyboard and a pointer for macOS, driven from the Mac over a third port (piano input)
  uint32_t hidHold;   // piano-usb-hid-hold=MS: keys/buttons still down this long after the last command are released (default 3000)
  uint32_t net;       // piano-usb-net=1: a CDC-NCM network function after the ports, an Ethernet interface for macOS (PianoUsbNet)
  uint32_t netFifo;   // piano-usb-net-fifo: 1 (default) the TX FIFO plan for the endpoints in use, 0 keep the sizes the reset left
  uint32_t irq;       // piano-usb-irq=1: the core's interrupt (SPI 133) wakes the USB thread (default 0: polled, adaptive)
  uint32_t idle;      // piano-usb-idle: 1 (default) the adaptive wait (UsbIdle.h), 0 the old loop (1 ms after every quiet pass)
  uint32_t sleepUsb;  // piano-sleep-usb: 1 (default) a configured host keeps the gadget across a system sleep, 0 a soft disconnect
  uint32_t handoff;   // piano-usb-handoff=1: PianoUsbHost may borrow the controller for host mode (UsbHandoff.h; default 0)
  cold::Mode cold;    // piano-usb-cold=0|1|auto: the inherited state only, always the cold start, or by the inherited state (default)
  bool coldKnown;     // false: piano-usb-cold had a value other than 0, 1, auto (then auto)
  uint32_t coldSteps; // piano-usb-cold-steps=MASK: the cold start's optional parts (UsbCold.h kStep*, default 0x17)
  uint32_t coldSim;   // piano-usb-cold-sim=MASK: the test hook, before the decision (UsbCold.h kSim*, default 0)
  uint32_t coldWait;  // piano-usb-cold-wait=S: no bus reset this long after RUN_STOP: a cold start (default 15, 0: no link watch)
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
  gArgs.level = bootArg("piano-usb", 0);
  gArgs.phy = bootArg("piano-usb-phy", 0);
  gArgs.smmu = bootArg("piano-usb-smmu", 1);
  gArgs.utmi = bootArg("piano-usb-utmi", 1);
  gArgs.delay = bootArg("piano-usb-delay", 0);
  gArgs.shell = bootArg("piano-usb-shell", 0);
  gArgs.poke = bootArg("piano-usb-poke", 0);
  gArgs.replay = bootArg("piano-usb-replay", 1);
  gArgs.phyread = bootArg("piano-usb-phyread", 1);
  gArgs.spmi = bootArg("piano-usb-spmi", 1);
  gArgs.pid = bootArg("piano-usb-pid", kProduct) & 0xffff;
  gArgs.trace = bootArg("piano-usb-trace", 400);
  gArgs.hid = bootArg("piano-usb-hid", 0);
  gArgs.hidHold = bootArg("piano-usb-hid-hold", 3000);
  gArgs.net = bootArg("piano-usb-net", 0);
  gArgs.netFifo = bootArg("piano-usb-net-fifo", 1);
  gArgs.irq = bootArg("piano-usb-irq", 0);
  gArgs.idle = bootArg("piano-usb-idle", 1);
  gArgs.sleepUsb = bootArg("piano-sleep-usb", 1);
  gArgs.handoff = bootArg("piano-usb-handoff", 0);
  if (!gArgs.idle) gArgs.irq = 0;   // the old loop is the old loop
  char text[16] = {};
  const bool given = PE_parse_boot_arg_str("piano-usb-cold", text, sizeof text);
  gArgs.cold = cold::parseMode(text, given, &gArgs.coldKnown);
  gArgs.coldSteps = bootArg("piano-usb-cold-steps", cold::kStepsDefault) & cold::kStepsAll;
  gArgs.coldSim = bootArg("piano-usb-cold-sim", 0) & cold::kSimAll;
  gArgs.coldWait = bootArg("piano-usb-cold-wait", cold::kWatchDefaultS);
  if (gArgs.coldWait > 3600) gArgs.coldWait = 3600;
}

// -------------------------------------------------------------------------------------------------------------------- MMIO
// Explicit ldr/str: every store comes from a general register, never wzr (Qualcomm's trapped SMMU registers need that).
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }

struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes, IOOptionBits cache = kIOMapInhibitCache)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | cache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDwc3: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_dwc3_stage = value;
  note("%s %s", value < 0 ? "FAILED at" : "stage", what);
}

uint64_t nowNs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}

// ------------------------------------------------------------------------------------------------------------------- state
Region gGccUsb, gGccQusb, gGccPhy, gGccPll, gGccVote, gTcsrUsb2, gTcsrUsb3, gDwc, gQs, gM31, gSmmu, gSpmiCore, gSpmiCnfg, gSpmiObs, gTlmm61;
bool gCoreClocked;
uint32_t gSnpsId;
IOBufferMemoryDescriptor *gDma;
IOMemoryMap *gDmaMap;
uint8_t *gDmaVa;
uint64_t gDmaPa;
IOLock *gLock;

// The role rule at the lowest level (UsbCold.h; C6, docs/forensics-2026-11-11.md section 7.6): GCTL.PRTCAPDIR as this driver last
// read or wrote it (0xff: not known, as at the start and after a hand-off's release). A device register (dwc3::isDeviceReg) is
// touched only while that is the device role, an xHCI register only while it is the host role: otherwise a read answers 0 and a
// write is dropped, both counted (the first offset kept for `usb`). Every path reads GCTL before its first device register (the
// probe, the inspection, c2, the takeover's gate), so a stale value can only refuse, never let an access through. Every core access
// of this driver goes through coreRd/coreWr (the gadget's Hal and the cold start's) except the interrupt's GEVNTSIZ/GEVNTCOUNT
// (global registers).
volatile uint32_t gRoleSeen = 0xffu, gRoleRefused, gRoleRefusedFirst;
bool roleAllows(uint32_t off)
{
  const uint32_t role = __atomic_load_n(&gRoleSeen, __ATOMIC_RELAXED);
  if ((dwc3::isDeviceReg(off) && role != dwc3::ROLE_DEVICE) || (dwc3::isXhciReg(off) && role != dwc3::ROLE_HOST)) {
    if (__atomic_fetch_add(&gRoleRefused, 1u, __ATOMIC_RELAXED) == 0) gRoleRefusedFirst = off;
    return false;
  }
  return true;
}
uint32_t coreRd(uint32_t off)
{
  if (!roleAllows(off)) return 0;
  const uint32_t v = rd32(gDwc.va + off);
  if (off == dwc3::GCTL) __atomic_store_n(&gRoleSeen, dwc3::roleOf(v), __ATOMIC_RELAXED);
  return v;
}
void coreWr(uint32_t off, uint32_t v)
{
  if (!roleAllows(off)) return;
  wr32(gDwc.va + off, v);
  if (off == dwc3::GCTL) __atomic_store_n(&gRoleSeen, dwc3::roleOf(v), __ATOMIC_RELAXED);
}

// The USB thread's wake-up word (docs/power-irq.md): it waits on it between passes; the core's interrupt (piano-usb-irq=1) and every
// producer (console and pipe writers, the pipe and HID readers freeing room, the network queues) kick it.
pianoirq::Waiter gWake;
void usbKick() { gWake.kick(); }
// The core's interrupt (below, at the USB thread) and the wait's state, for the monitor's `usb` command too.
pianoirq::SpiLine gIrqLine;
pianoirq::MissGuard gIrqMiss = pianoirq::missGuard(3);
volatile uint32_t gIrqTaken;   // entries that masked events: the thread tells an interrupt from a kick by it
bool gIrqMode;
UsbTier gTier = UsbTier::busy;

// System sleep (UsbSleep.h): setPowerState asks, the USB thread carries it out (the gadget is the thread's); gSleepLock guards the
// state machine and the request. QSCRATCH writes ask it too (qsWrite).
IOLock *gSleepLock;
UsbSleep gSleep;
enum : uint32_t { kAskNone = 0, kAskOff = 1, kAskOn = 2 };
uint32_t gSleepAsk = kAskNone, gSleepSeq, gSleepDone;
bool qsWrite(uint32_t offset, uint32_t value);
// The monitor's `usb cold [sim MASK]` for the USB thread (UsbCold.h): bit 31 pending, bits 4:0 the test hook's mask; due at gColdAskAtMs.
volatile uint32_t gColdAsk;
uint64_t gColdAskAtMs;

struct Ring {
  uint8_t *buf;
  uint32_t size, head, tail;   // size: a power of two
  uint64_t dropped;
  uint32_t used() const { return head - tail; }
  uint32_t space() const { return size - used(); }
  uint32_t put(const uint8_t *s, uint32_t n)
  {
    if (n > space()) { dropped += n - space(); n = space(); }
    for (uint32_t i = 0; i < n; ++i) buf[(head + i) & (size - 1)] = s[i];
    head += n;
    return n;
  }
  uint32_t get(uint8_t *d, uint32_t n)
  {
    if (n > used()) n = used();
    for (uint32_t i = 0; i < n; ++i) d[i] = buf[(tail + i) & (size - 1)];
    tail += n;
    return n;
  }
};
Ring gTx[kPorts], gRx1, gRx2, gNotes;   // gRx2: the input port's command bytes (piano-usb-hid=1)
volatile bool gHidReleaseWanted;        // the input port closed or the bus reset: the HID thread releases everything
PianoUsbHID *gHidDevice[2];             // keyboard, pointer (piano-usb-hid=1)
struct HidCounters {                    // counts only: what was typed is never kept or logged
  uint32_t commands, keyboard, consumer, pointer, releases, autoReleases, errors, sendErrors;
};
HidCounters gHid;
uint16_t gLines[kPorts];
bool gLive = true, gEcho = true;
volatile bool gAbort;

bool ringAlloc(Ring &r, uint32_t size)
{
  r.buf = static_cast<uint8_t *>(IOMallocData(size));
  r.size = size;
  r.head = r.tail = 0;
  r.dropped = 0;
  return r.buf != nullptr;
}

// Notes of the USB thread go through a ring to their own thread: with -v, IOLog draws on the 3200x2136 console and can take
// milliseconds, longer than the host allows for parts of a control transfer (SET_ADDRESS: 50 ms).
void noteLater(const char *line)
{
  if (!gNotes.buf) { IOLog("PianoDwc3: %s\n", line); return; }
  const uint32_t n = uint32_t(strlen(line));
  IOLockLock(gLock);
  if (gNotes.space() > n) {
    gNotes.put(reinterpret_cast<const uint8_t *>(line), n);
    gNotes.put(reinterpret_cast<const uint8_t *>("\n"), 1);
  } else {
    gNotes.dropped += n + 1;
  }
  IOLockWakeup(gLock, &gNotes, false);
  IOLockUnlock(gLock);
}

void noteThread(void *, wait_result_t)
{
  for (;;) {
    char line[640];
    uint32_t n = 0;
    IOLockLock(gLock);
    while (!gNotes.used()) IOLockSleep(gLock, &gNotes, THREAD_UNINT);
    uint8_t c = 0;
    while (gNotes.used() && gNotes.get(&c, 1) && c != '\n')
      if (n + 1 < sizeof line) line[n++] = char(c);
    IOLockUnlock(gLock);
    line[n] = 0;
    IOLog("PianoDwc3: %s\n", line);
  }
}

// Console output (port 0): '\n' -> "\r\n". wait: block up to 2 s for room while the Mac has the port open.
void consoleWrite(const char *s, size_t n, bool wait)
{
  uint8_t chunk[256];
  size_t i = 0;
  const uint64_t deadline = nowNs() + 2000000000ull;
  while (i < n) {
    uint32_t c = 0;
    while (i < n && c + 2 <= sizeof chunk) {
      if (s[i] == '\n') chunk[c++] = '\r';
      chunk[c++] = uint8_t(s[i++]);
    }
    IOLockLock(gLock);
    while (wait && gTx[0].space() < c && (gLines[0] & 1) && nowNs() < deadline) {
      IOLockUnlock(gLock);
      IOSleep(2);
      IOLockLock(gLock);
    }
    const bool open = gLines[0] & 1;
    if (open) gTx[0].put(chunk, c);
    IOLockUnlock(gLock);
    if (open) usbKick();
  }
}

void cvprintf(bool wait, const char *fmt, va_list ap)
{
  char line[512];
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  if (n > 0) consoleWrite(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1, wait);
}
// cprintf: from the monitor thread, may wait for room. cnotice: from the USB thread, never waits (it drains the ring).
void cprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  cvprintf(true, fmt, ap);
  va_end(ap);
}
void cnotice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cnotice(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  cvprintf(false, fmt, ap);
  va_end(ap);
}

// ------------------------------------------------------------------------------------------------------ the kernel log tail
// k27 0055/0058 pstore-console: header then the ring stored 11 times; copy 0 is read live. "DBGR" (0058): {start, size} at
// words 4/5, data at +256, capacity (bytes - 256) / 11. "DBGB" (0055 alone): words 1/2, data at +12, (bytes - 16) / 11.
// Without the boot argument: the kernel's message buffer. The header's start can step back (0055's writers are lock-free, each
// stores it from its own end): LogCursor.h keeps the reader's and the writer's positions (desk1, usb-device-debug.md section 18).
struct LogTail {
  Region map;
  const volatile uint32_t *hdr;
  const volatile uint8_t *data;
  uint32_t cap, startWord, sizeWord;
  bool ok, msgbuf;
  LogCursor cur;
  uint64_t lostShown;   // cur.lost as last reported on the console

  uint32_t start() const
  {
    if (msgbuf) return msgbufp ? uint32_t(msgbufp->msg_bufx) % cap : 0;
    return hdr[startWord] % cap;
  }
  uint32_t size() const
  {
    if (msgbuf) return cap;
    const uint32_t s = hdr[sizeWord];
    return s > cap ? cap : s;
  }
  uint8_t at(uint32_t i) const { return msgbuf ? uint8_t(msgbufp->msg_bufc[i]) : data[i]; }
};
LogTail gLog;
volatile int gRewindRequest;   // 1 replay, 2 from now: set by the monitor, applied by the USB thread (the tail's only user)

void logInit()
{
  uint64_t base = 0, bytes = 0x200000;
  if (PE_parse_boot_argn("pstore-console", &base, sizeof base) && base) {
    (void)PE_parse_boot_argn("pstore-console-size", &bytes, sizeof bytes);
    if (bytes >= 0x10000 && bytes <= 0x1000000 && mapRegion(gLog.map, base, bytes, kIOMapWriteCombineCache)) {
      gLog.hdr = reinterpret_cast<const volatile uint32_t *>(gLog.map.va);
      const uint32_t sig = gLog.hdr[0];
      if (sig == 0x52474244u) {   // "DBGR"
        gLog.data = reinterpret_cast<const volatile uint8_t *>(gLog.map.va + 256);
        gLog.cap = uint32_t((bytes - 256) / 11);
        gLog.startWord = 4;
        gLog.sizeWord = 5;
        gLog.ok = true;
      } else if (sig == 0x42474244u) {   // "DBGB"
        gLog.data = reinterpret_cast<const volatile uint8_t *>(gLog.map.va + 12);
        gLog.cap = uint32_t((bytes - 16) / 11);
        gLog.startWord = 1;
        gLog.sizeWord = 2;
        gLog.ok = true;
      }
      note("log source: pstore-console %#llx (+%#llx) signature %08x capacity %u: %s", base, bytes, sig, gLog.cap,
           gLog.ok ? "used" : "unknown layout");
    }
  }
  if (!gLog.ok && msgbufp && msgbufp->msg_size > 0 && msgbufp->msg_bufc) {
    gLog.msgbuf = true;
    gLog.cap = uint32_t(msgbufp->msg_size);
    gLog.ok = true;
    note("log source: the kernel message buffer (%u bytes)", gLog.cap);
  }
  if (gLog.ok) gLog.cur.init(gLog.cap, gLog.start(), gLog.size(), nowNs() / 1000);
}

void consoleClosed();   // below the gadget

// The writer's position taken (LogCursor::sync): every pass, and once a second through piano-usb-delay.
void logSync()
{
  if (gLog.ok) gLog.cur.sync(gLog.start(), nowNs() / 1000);
}

// Every pass of the USB thread, the port open or not (the writer's position stays known); sends while the port is open and live.
// True when it moved bytes (the USB thread's activity), or when bytes wait for their settle time (the next pass comes soon).
bool logPump()
{
  if (!gLog.ok) return false;
  logSync();
  if (const int request = gRewindRequest) {
    gRewindRequest = 0;
    gLog.cur.rewind(request == 1);
  }
  if (!(gLines[0] & 1) || !gLive) return false;
  bool moved = false;
  char chunk[512];
  for (unsigned round = 0; round < 32; ++round) {
    const uint32_t avail = gLog.cur.readable();   // a reader the writer overtook skips to the oldest byte first (counted)
    if (gLog.cur.lost > gLog.lostShown) {
      cnotice("\n[PianoDwc3: %llu bytes of the log were overwritten before the console could send them]\n",
              (unsigned long long)(gLog.cur.lost - gLog.lostShown));
      gLog.lostShown = gLog.cur.lost;
    }
    if (!avail) break;
    IOLockLock(gLock);
    const uint32_t room = gTx[0].space();
    IOLockUnlock(gLock);
    if (room < 2 * sizeof chunk) return moved;   // '\n' may double; the host is behind (the wait tiers see the full queue)
    uint32_t n = 0, taken = 0;
    while (taken < avail && n < sizeof chunk) {
      const uint8_t c = gLog.at(gLog.cur.index(gLog.cur.sent + taken));
      ++taken;
      if (c) chunk[n++] = char(c);   // the message buffer starts zero-filled
    }
    gLog.cur.consume(taken);
    consoleWrite(chunk, n, false);
    moved = true;
  }
  return moved || gLog.cur.unsettled();
}

// ------------------------------------------------------------------------------------------------------------ the monitor
char gLine[256], gEdit[256];
uint32_t gEditLen;
bool gLineReady;

bool parseNumber(const char *&s, uint64_t *out)
{
  while (*s == ' ') ++s;
  uint64_t v = 0;
  unsigned base = 10, digits = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
  for (;; ++s) {
    unsigned d;
    if (*s >= '0' && *s <= '9') d = unsigned(*s - '0');
    else if (base == 16 && *s >= 'a' && *s <= 'f') d = unsigned(*s - 'a' + 10);
    else if (base == 16 && *s >= 'A' && *s <= 'F') d = unsigned(*s - 'A' + 10);
    else break;
    v = v * base + d;
    ++digits;
  }
  *out = v;
  return digits > 0;
}

bool word(const char *&s, const char *w)
{
  while (*s == ' ') ++s;
  size_t n = strlen(w);
  if (strncmp(s, w, n) || (s[n] && s[n] != ' ')) return false;
  s += n;
  return true;
}

void dumpEntry(IORegistryEntry *e, unsigned depth, unsigned max)
{
  if (gAbort) return;
  char indent[66];
  unsigned n = depth * 2 < sizeof indent - 2 ? depth * 2 : sizeof indent - 2;
  for (unsigned i = 0; i < n; ++i) indent[i] = ' ';
  indent[n] = 0;
  IOService *service = OSDynamicCast(IOService, e);
  cprintf("%s+-o %s  <class %s%s>\n", indent, e->getName(gIOServicePlane), e->getMetaClass()->getClassName(),
          service && service->getBusyState() ? ", busy" : "");
  if (depth >= max) return;
  OSIterator *children = e->getChildIterator(gIOServicePlane);
  if (!children) return;
  while (OSObject *o = children->getNextObject()) {
    if (IORegistryEntry *c = OSDynamicCast(IORegistryEntry, o)) dumpEntry(c, depth + 1, max);
    if (gAbort) break;
  }
  children->release();
}

void peek(uint64_t pa, uint64_t count)
{
  if (pa & 3) { cprintf("peek: address must be 4-byte aligned\n"); return; }
  if (!count) count = 4;
  if (count > 256) count = 256;
  Region r = {};
  if (!mapRegion(r, pa, count * 4)) { cprintf("peek: cannot map %#llx\n", pa); return; }
  for (uint64_t i = 0; i < count; ++i) {
    if (!(i % 4)) cprintf("%s%010llx:", i ? "\n" : "", pa + 4 * i);
    cprintf(" %08x", rd32(r.va + uintptr_t(4 * i)));
  }
  cprintf("\n");
  r.map->release();
}

// ABL boots the installed OS after a reset unless the PMIC restart reason says otherwise: byte 0x48 of the SID 0 SDAM
// (PPID 0x071), bits 1-7 (2 = bootloader), bit 0 kept. The same SPMI arbiter v7 access as k27 0057's piano-reset-to, here
// for one reset on request, so an installed system (no piano-reset-to) can be sent to ABL fastboot from the Mac.
bool spmiDone(uintptr_t status)
{
  for (int i = 0; i < 100000; i++) {
    const uint32_t st = rd32(status);
    if (st & 1) return (st & 0xe) == 0;
  }
  return false;
}

int setRestartReason(uint8_t reason)
{
  constexpr uint64_t kCore = 0x0C400000, kCnfg = 0x0C42D000, kObsv = 0x0C440000, kChnl = 0x0C500000;
  constexpr uint32_t kPpid = 0x071, kOffset = 0x48;
  Region core = {}, cnfg = {}, obsv = {}, chnl = {};
  int err = 0;
  uint32_t apid = 0, count = 0;
  if (!mapRegion(core, kCore, 0x3000) || !mapRegion(cnfg, kCnfg, 0x1000)) {
    err = -1;
  } else if (count = rd32(core.va + 4) & 0x7ff, (rd32(core.va) >> 28) != 7 || !count || count > 1024) {
    err = -2;
  } else {
    for (apid = 0; apid < count; apid++) {
      const uint32_t map = rd32(core.va + 0x2000 + 4 * apid);
      if (map && ((map >> 8) & 0xfff) == kPpid && (rd32(cnfg.va + 4 * apid) & 7) == 0) break;
    }
    if (apid == count) err = -3;
    else if (!mapRegion(obsv, kObsv + 0x20ull * apid, 0x20) || !mapRegion(chnl, kChnl + 0x1000ull * apid, 0x20)) err = -4;
  }
  if (!err) {
    wr32(obsv.va, (1u << 27) | (kOffset << 4));                 // read 1 byte
    if (!spmiDone(obsv.va + 8)) err = -5;
  }
  if (!err) {
    const uint8_t old = uint8_t(rd32(obsv.va + 24));
    wr32(chnl.va + 16, uint32_t((old & 1) | uint8_t(reason << 1)));
    wr32(chnl.va, (0u << 27) | (kOffset << 4));                 // write 1 byte
    if (!spmiDone(chnl.va + 8)) err = -6;
    wr32(obsv.va, (1u << 27) | (kOffset << 4));
    (void)spmiDone(obsv.va + 8);
    cprintf("SDAM restart reason %#04x -> %#04x (APID %u)\n", old, uint8_t(rd32(obsv.va + 24)), apid);
  }
  Region *all[] = {&core, &cnfg, &obsv, &chnl};
  for (Region *r : all)
    if (r->map) r->map->release();
  return err;
}

void usbStatus();

void execute(const char *line)
{
  const char *s = line;
  gAbort = false;
  uint64_t a = 0, b = 0;
  if (word(s, "help") || word(s, "?")) {
    cprintf("PianoDwc3 kernel monitor. Commands:\n"
            "  log            replay the retained console log from its oldest byte\n"
            "  live on|off    the live log stream on this port (output of commands is always sent)\n"
            "  echo on|off    echo typed characters\n"
            "  usb            controller and channel state\n"
            "  usb cold [sim M]  a cold start of the controller now (piano-usb-cold), after the test hook M (piano-usb-cold-sim)\n"
            "  ioreg [depth]  the IOService plane (default and limit: depth 32); Ctrl-C stops it\n"
            "  peek PA [n]    n 32-bit words at physical address PA (uncached mapping)\n"
            "  poke PA V      write V at PA (needs piano-usb-poke=1)\n"
            "  args           kernel boot arguments\n"
            "  uptime         time since boot\n"
            "  reset          restart through PSCI (with piano-reset-to, back to ABL fastboot)\n"
            "  reset fastboot restart into ABL fastboot (sets the PMIC restart reason first)\n"
            "  panic          panic on purpose\n");
  } else if (word(s, "log")) {
    gRewindRequest = 1;
    gLive = true;
  } else if (word(s, "live")) {
    gLive = !word(s, "off");
    cprintf("live log %s\n", gLive ? "on" : "off");
    if (gLive) gRewindRequest = 2;
  } else if (word(s, "echo")) {
    gEcho = !word(s, "off");
  } else if (word(s, "usb")) {
    if (word(s, "cold")) {   // the USB thread does it in 0.5 s, after this reply went out; the link drops and the Mac enumerates again
      uint64_t mask = 0;
      if (word(s, "sim") && (!parseNumber(s, &mask) || mask > cold::kSimAll)) { cprintf("usb cold [sim MASK]: MASK 0..%#x\n", cold::kSimAll); return; }
      gColdAskAtMs = nowNs() / 1000000 + 500;
      __atomic_store_n(&gColdAsk, 0x80000000u | uint32_t(mask), __ATOMIC_RELEASE);
      usbKick();
      cprintf("cold restart in 0.5 s (test hook %#llx): the link drops now; usb-console.py reconnects once the Mac enumerates again\n",
              (unsigned long long)mask);
      return;
    }
    usbStatus();
  } else if (word(s, "ioreg")) {
    if (!parseNumber(s, &a) || a > 32) a = 32;
    dumpEntry(IORegistryEntry::getRegistryRoot(), 0, unsigned(a));
  } else if (word(s, "peek")) {
    if (!parseNumber(s, &a)) { cprintf("peek PA [count]\n"); return; }
    if (!parseNumber(s, &b)) b = 4;
    peek(a, b);
  } else if (word(s, "poke")) {
    if (!gArgs.poke) { cprintf("poke: boot with piano-usb-poke=1\n"); return; }
    if (!parseNumber(s, &a) || !parseNumber(s, &b) || (a & 3)) { cprintf("poke PA VALUE (PA 4-byte aligned)\n"); return; }
    if (a >= pianohw::kQscratch && a < pianohw::kQscratch + 0x100 && gQs.va) {   // the glue: through the sleep guard
      const bool now = qsWrite(uint32_t(a - pianohw::kQscratch), uint32_t(b));
      cprintf("%010llx <- %08x %s, reads %08x\n", a, uint32_t(b), now ? "written" : "recorded for the wake (asleep or unclocked)",
              rd32(gQs.va + uintptr_t(a - pianohw::kQscratch)));
      return;
    }
    Region r = {};
    if (!mapRegion(r, a, 4)) { cprintf("poke: cannot map %#llx\n", a); return; }
    wr32(r.va, uint32_t(b));
    dsb();
    cprintf("%010llx <- %08x, reads %08x\n", a, uint32_t(b), rd32(r.va));
    r.map->release();
  } else if (word(s, "args")) {
    cprintf("%s\n", PE_boot_args());
  } else if (word(s, "uptime")) {
    const uint64_t ns = nowNs();
    cprintf("%llu.%03llu s\n", ns / 1000000000ull, ns / 1000000ull % 1000);
  } else if (word(s, "reset")) {
    if (word(s, "fastboot")) {
      const int err = setRestartReason(2);
      if (err) { cprintf("reset fastboot: SDAM write failed (%d); not restarting\n", err); return; }
      cprintf("restart reason: bootloader (ABL fastboot after this reset)\n");
    }
    cprintf("restarting\n");
    IOSleep(200);
    PEHaltRestart(kPERestartCPU);
  } else if (word(s, "panic")) {
    panic("PianoDwc3: panic requested on the USB console");
  } else if (*s) {
    cprintf("unknown command: %s (help lists them)\n", s);
  }
}

void monitorThread(void *, wait_result_t)
{
  for (;;) {
    IOLockLock(gLock);
    while (!gLineReady) IOLockSleep(gLock, &gLineReady, THREAD_UNINT);
    char line[sizeof gLine];
    memcpy(line, gLine, sizeof line);
    gLineReady = false;
    IOLockUnlock(gLock);
    execute(line);
    consoleWrite("piano> ", 7, true);
  }
}

// Bytes typed on port 0 (the USB thread): line editing, echo, Ctrl-C.
void consoleInput(const uint8_t *s, unsigned n)
{
  for (unsigned i = 0; i < n; ++i) {
    const char c = char(s[i]);
    if (c == 3) { gAbort = true; gEditLen = 0; consoleWrite("^C\n", 3, false); continue; }
    if (c == '\r' || c == '\n') {
      if (gEcho) consoleWrite("\n", 1, false);
      gEdit[gEditLen] = 0;
      IOLockLock(gLock);
      if (!gLineReady) {
        memcpy(gLine, gEdit, gEditLen + 1);
        gLineReady = true;
        IOLockWakeup(gLock, &gLineReady, false);
      }
      IOLockUnlock(gLock);
      gEditLen = 0;
      continue;
    }
    if (c == 0x7f || c == 0x08) {
      if (gEditLen) { --gEditLen; if (gEcho) consoleWrite("\b \b", 3, false); }
      continue;
    }
    if (uint8_t(c) < 0x20 || gEditLen + 1 >= sizeof gEdit) continue;
    gEdit[gEditLen++] = c;
    if (gEcho) consoleWrite(&c, 1, false);
  }
}

// ----------------------------------------------------------------------------------------------- /dev/pianousb (port 1)
struct selinfo gSelRead, gSelWrite;
bool gUserOpen;
int gMajor = -1;
void *gDevNode;

int puOpen(dev_t, int, int, struct proc *)
{
  IOLockLock(gLock);
  const bool busy = gUserOpen;
  gUserOpen = true;
  IOLockUnlock(gLock);
  return busy ? EBUSY : 0;
}

int puClose(dev_t, int, int, struct proc *)
{
  IOLockLock(gLock);
  gUserOpen = false;
  IOLockUnlock(gLock);
  return 0;
}

int puRead(dev_t, struct uio *uio, int ioflag)
{
  int error = 0;
  IOLockLock(gLock);
  while (!gRx1.used()) {
    if (ioflag & IO_NDELAY) { IOLockUnlock(gLock); return EWOULDBLOCK; }
    error = msleep(&gRx1, IOLockGetMachLock(gLock), PZERO | PCATCH, "pianousb", nullptr);
    if (error) { IOLockUnlock(gLock); return error; }
  }
  while (!error && uio_resid(uio) > 0 && gRx1.used()) {
    uint8_t tmp[512];
    const uint32_t want = uio_resid(uio) < user_ssize_t(sizeof tmp) ? uint32_t(uio_resid(uio)) : uint32_t(sizeof tmp);
    const uint32_t n = gRx1.get(tmp, want);
    IOLockUnlock(gLock);
    error = uiomove(reinterpret_cast<char *>(tmp), int(n), uio);
    IOLockLock(gLock);
  }
  IOLockUnlock(gLock);
  usbKick();   // room for the next bulk OUT transfer
  return error;
}

// Data written while the Mac does not have port 1 open is dropped (a `log stream` never blocks).
int puWrite(dev_t, struct uio *uio, int ioflag)
{
  int error = 0;
  while (!error && uio_resid(uio) > 0) {
    uint8_t tmp[512];
    const uint32_t n = uio_resid(uio) < user_ssize_t(sizeof tmp) ? uint32_t(uio_resid(uio)) : uint32_t(sizeof tmp);
    error = uiomove(reinterpret_cast<char *>(tmp), int(n), uio);
    if (error) break;
    IOLockLock(gLock);
    uint32_t done = 0;
    while (done < n) {
      if (!(gLines[1] & 1)) { gTx[1].dropped += n - done; break; }
      const uint32_t room = gTx[1].space(), k = room < n - done ? room : n - done;
      if (k) { gTx[1].put(tmp + done, k); done += k; usbKick(); continue; }
      if (ioflag & IO_NDELAY) { gTx[1].dropped += n - done; break; }
      error = msleep(&gTx[1], IOLockGetMachLock(gLock), PZERO | PCATCH, "pianousb", nullptr);
      if (error) break;
    }
    IOLockUnlock(gLock);
  }
  return error;
}

// select() and (the cdevsw being kqueue-capable, shellThread) poll() and kevent: the bytes ready (to read; room to write, a full ring
// when the Mac's end is closed, since writes are then dropped), or 0 with the selinfo recorded. kevent's data is this count.
int puSelect(dev_t, int which, void *wql, struct proc *p)
{
  int ready = 0;
  IOLockLock(gLock);
  if (which == FREAD) {
    if (gRx1.used()) ready = int(gRx1.used()); else selrecord(p, &gSelRead, wql);
  } else if (which == FWRITE) {
    if (!(gLines[1] & 1)) ready = gTx[1].size ? int(gTx[1].size) : 1;
    else if (gTx[1].space()) ready = int(gTx[1].space());
    else selrecord(p, &gSelWrite, wql);
  }
  IOLockUnlock(gLock);
  return ready;
}

int puIoctl(dev_t, u_long, caddr_t, int, struct proc *) { return ENOTTY; }

const struct cdevsw gCdev = {
  puOpen, puClose, puRead, puWrite, puIoctl, eno_stop, eno_reset, nullptr, puSelect, eno_mmap, eno_strat, eno_getc, eno_putc, 0,
};

void shellThread(void *, wait_result_t)
{
  if (OSDictionary *matching = IOService::resourceMatching(kIOBSDKey)) {
    if (IOService *bsd = IOService::waitForMatchingService(matching)) bsd->release();
    matching->release();
  }
  gMajor = cdevsw_add(-1, &gCdev);
  if (gMajor < 0) { note("/dev/pianousb: cdevsw_add failed"); return; }
  // poll() is kqueue in XNU, and a character device's knote attaches only when its cdevsw is marked kqueue-capable (spec_kqfilter,
  // filt_specattach: EINVAL, so poll() answers POLLNVAL at once): unmarked, pianousb-shell's poll() loop spun and never read
  // (desk-dev, 2026-11-12). puSelect keeps the contract (a count when ready, selrecord otherwise). The helper uses select() anyway.
  const int kq = cdevsw_setkqueueok(gMajor, &gCdev, 0);
  gDevNode = devfs_make_node(makedev(gMajor, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "pianousb");
  note("/dev/pianousb %s (major %d): port 1 is the user pipe; select%s", gDevNode ? "created" : "NOT created", gMajor,
       kq == 0 ? ", poll and kqueue" : " only (cdevsw_setkqueueok refused)");
}

// ------------------------------------------------------------------------------------------------------- the gadget's Hal
struct KernelHal {
  uint32_t rd(uint32_t off) { return coreRd(off); }
  void wr(uint32_t off, uint32_t v) { coreWr(off, v); }
  void barrier() { dsb(); }
  void udelay(unsigned us)
  {
    if (us >= 1000) IOSleep(us / 1000);
    else IODelay(us);
  }
  uint64_t nowUs() { return nowNs() / 1000; }
  uint8_t *dma() { return gDmaVa; }
  uint64_t dmaPhys() { return gDmaPa; }
  unsigned txFill(unsigned port, uint8_t *dst, unsigned cap)
  {
    uint8_t tmp[512];
    unsigned total = 0;
    IOLockLock(gLock);
    while (total < cap && gTx[port].used()) {
      const uint32_t n = gTx[port].get(tmp, cap - total < sizeof tmp ? cap - total : uint32_t(sizeof tmp));
      for (uint32_t i = 0; i < n; ++i) dst[total + i] = tmp[i];
      total += n;
    }
    IOLockUnlock(gLock);
    if (port == 1 && total) {
      wakeup(&gTx[1]);
      selwakeup(&gSelWrite);
    }
    return total;
  }
  unsigned rxSpace(unsigned port)
  {
    if (port == kPortConsole) return 1u << 20;
    IOLockLock(gLock);
    const unsigned n = port == kPortInput ? gRx2.space() : gRx1.space();
    IOLockUnlock(gLock);
    return n;
  }
  void rxData(unsigned port, const uint8_t *src, unsigned len)
  {
    for (unsigned at = 0; at < len;) {
      uint8_t tmp[512];
      const unsigned n = len - at < sizeof tmp ? len - at : unsigned(sizeof tmp);
      for (unsigned i = 0; i < n; ++i) tmp[i] = src[at + i];   // out of the DMA buffer first
      at += n;
      if (port == kPortConsole) { consoleInput(tmp, n); continue; }
      IOLockLock(gLock);
      if (port == kPortInput) {   // commands for the HID thread; never logged
        gRx2.put(tmp, n);
        IOLockWakeup(gLock, &gRx2, false);
      } else if (gUserOpen) {
        gRx1.put(tmp, n);
      }
      IOLockUnlock(gLock);
    }
    if (port == kPortPipe) {
      wakeup(&gRx1);
      selwakeup(&gSelRead);
    }
  }
  void lineState(unsigned port, uint16_t state)
  {
    const bool opened = (state & 1) && !(gLines[port] & 1), closed = !(state & 1) && (gLines[port] & 1);
    if (port == kPortConsole && closed) consoleClosed();   // before the queue is dropped: its bytes are sent again next time
    IOLockLock(gLock);
    gLines[port] = state;
    if (opened) gTx[port].head = gTx[port].tail;   // nothing stale from an earlier session
    IOLockUnlock(gLock);
    note("port %u %s (DTR/RTS %x)", port, state & 1 ? "opened" : "closed", state);
    if (port == kPortConsole && opened) {
      // The boot's first session replays the retained log (piano-usb-replay=1, the default; 2: every session does); a later one
      // resumes where the last stopped, so a host that reconnects gets each line once (desk1, usb-device-debug.md section 18).
      const LogCursor::Start how = gLog.ok ? gLog.cur.open(gArgs.replay != 0, gArgs.replay == 2) : LogCursor::Start::now;
      if (how == LogCursor::Start::resume)
        cnotice("\nPianoDwc3: piano kernel console on USB (piano-usb=%u), session %u: the log resumes where the last session stopped "
                "(%llu bytes; a few lines may repeat). Type 'help'.\n", gArgs.level, gLog.cur.sessions,
                (unsigned long long)gLog.cur.behind());
      else
        cnotice("\nPianoDwc3: piano kernel console on USB (piano-usb=%u)%s. Type 'help'.\n", gArgs.level,
                how == LogCursor::Start::replay ? "; the retained boot log follows" : "");
      gLive = true;
    }
    if (port == kPortInput && !(state & 1)) {   // nothing stays pressed once the Mac's end is gone
      IOLockLock(gLock);
      gHidReleaseWanted = true;
      IOLockWakeup(gLock, &gRx2, false);
      IOLockUnlock(gLock);
    }
    if (port == kPortPipe) {
      wakeup(&gTx[1]);
      selwakeup(&gSelWrite);
      if (opened && !gArgs.shell) {
        static const char msg[] = "PianoDwc3: port 1 is the user pipe; boot with piano-usb-shell=1 to get /dev/pianousb\r\n";
        IOLockLock(gLock);
        gTx[1].put(reinterpret_cast<const uint8_t *>(msg), sizeof msg - 1);
        IOLockUnlock(gLock);
      }
    }
  }
  // The network function (piano-usb-net=1): PianoUsbNet.cpp's queues and link.
  unsigned netTxNext(uint8_t *dst, unsigned room) { return pianonet::txNext(dst, room); }
  unsigned netRxSpace() { return pianonet::rxSpace(); }
  bool netRxFrame(const uint8_t *src, unsigned len) { return pianonet::rxFrame(src, len); }
  void netRxDone() { pianonet::rxDone(); }
  void netLink(bool up, uint32_t bitsPerSecond)
  {
    pianonet::link(up, bitsPerSecond);
    note("net: the Mac %s the NCM data interface", up ? "selected" : "left");
  }
  __attribute__((format(printf, 2, 3))) void note(const char *fmt, ...)
  {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    noteLater(line);
  }
};
KernelHal gHal;
Gadget<KernelHal> gGadget;
bool gRunning;

// Port 0 closed (the host closed it, a bus reset, a cold restart): what had not reached the host (queued for the port, or in its
// bulk IN) is sent again when it opens next; '\r's and the monitor's own lines in the queue make it a little more, never less.
void consoleClosed()
{
  if (!gLog.ok) return;
  IOLockLock(gLock);
  const uint32_t queued = gTx[0].used();
  IOLockUnlock(gLock);
  gLog.cur.close(uint64_t(queued) + gGadget.takeInUnconfirmed(kPortConsole));
}

// ------------------------------------------------------------------------------------------------------- hardware: probe
// The GCC pages this driver reads: the USB block 0x39000, the USB3 PHY's 0x50000, the M31's BCR 0x12000, GPLL0 at 0 and the vote
// words at 0x52000 (the cold start; the vote word only under PianoA830's lock, UsbCold.h).
uint32_t gcc(uint32_t off)
{
  if (off >= 0x39000 && off < 0x3a000) return rd32(gGccUsb.va + (off - 0x39000));
  if (off >= 0x50000 && off < 0x51000) return rd32(gGccPhy.va + (off - 0x50000));
  if (off >= 0x12000 && off < 0x13000) return rd32(gGccQusb.va + (off - 0x12000));
  if (off < 0x1000 && gGccPll.va) return rd32(gGccPll.va + off);
  if (off >= 0x52000 && off < 0x53000 && gGccVote.va) return rd32(gGccVote.va + (off - 0x52000));
  return 0xdeadbeef;
}
void gccWrite(uint32_t off, uint32_t v)
{
  if (off >= 0x39000 && off < 0x3a000) wr32(gGccUsb.va + (off - 0x39000), v);
  else if (off >= 0x50000 && off < 0x51000) wr32(gGccPhy.va + (off - 0x50000), v);
  else if (off >= 0x12000 && off < 0x13000) wr32(gGccQusb.va + (off - 0x12000), v);
  else if (off >= 0x52000 && off < 0x53000 && gGccVote.va) wr32(gGccVote.va + (off - 0x52000), v);
  dsb();
}

bool clocksOn(uint32_t gdsc, uint32_t master, uint32_t cfgNoc, uint32_t aggre)
{
  using namespace pianohw;
  return (gdsc & kGdscPwrOn) && !(master & kCbcrClkOff) && !(cfgNoc & kCbcrClkOff) && !(aggre & kCbcrClkOff);
}

// The glue's clocks as GCC reads them now (GCC is always readable): the GDSC on and the master and AXI branches running.
bool glueClocked()
{
  using namespace pianohw;
  return clocksOn(gcc(kGccUsb30Gdsc), gcc(kGccUsb30Master), gcc(kGccCfgNocUsb3Axi), gcc(kGccAggreUsb3Axi));
}

// Every QSCRATCH write: to the glue only while awake with its clocks on (UsbSleep.h), else recorded (the last value per register)
// and written after the wake. True when written now.
bool qsWrite(uint32_t offset, uint32_t value)
{
  const bool clocked = glueClocked();
  IOLockLock(gSleepLock);
  const bool now = gSleep.qscratchWrite(offset, value, clocked);
  IOLockUnlock(gSleepLock);
  if (now) {
    wr32(gQs.va + offset, value);
    dsb();
  } else {
    note("QSCRATCH %#x = %08x not written now (%s, glue %s): recorded for the wake", offset, value, sleepStateName(gSleep.state()),
         clocked ? "clocked" : "NOT clocked");
  }
  return now;
}

bool probeClocks()
{
  using namespace pianohw;
  if (!mapRegion(gGccUsb, kGcc + 0x39000, 0x1000) || !mapRegion(gGccPhy, kGcc + 0x50000, 0x1000) ||
      !mapRegion(gGccQusb, kGcc + 0x12000, 0x1000) || !mapRegion(gGccPll, kGcc, 0x1000) || !mapRegion(gGccVote, kGcc + 0x52000, 0x1000) ||
      !mapRegion(gTcsrUsb2, kTcsrUsb2ClkRef, 4) || !mapRegion(gTcsrUsb3, kTcsrUsb3ClkRef, 4)) {
    note("cannot map GCC/TCSR");
    return false;
  }
  const uint32_t gdsc = gcc(kGccUsb30Gdsc), master = gcc(kGccUsb30Master), cfgNoc = gcc(kGccCfgNocUsb3Axi), aggre = gcc(kGccAggreUsb3Axi);
  note("GCC usb30 gdsc %08x bcr %08x master %08x (rcg %08x %08x) sleep %08x mock_utmi %08x (rcg %08x %08x) cfg_noc_axi %08x aggre_axi %08x",
       gdsc, gcc(kGccUsb30Bcr), master, gcc(kGccUsb30MasterRcg), gcc(kGccUsb30MasterRcg + 4), gcc(kGccUsb30Sleep), gcc(kGccUsb30MockUtmi),
       gcc(kGccUsb30MockUtmiRcg), gcc(kGccUsb30MockUtmiRcg + 4), cfgNoc, aggre);
  note("GCC usb3 phy gdsc %08x aux %08x com_aux %08x pipe %08x qusb2phy bcr %08x; TCSR clkref usb2 %08x usb3 %08x",
       gcc(kGccUsb3PhyGdsc), gcc(kGccUsb3PhyAux), gcc(kGccUsb3PhyComAux), gcc(kGccUsb3PhyPipe), gcc(kGccQusb2PhyBcr), rd32(gTcsrUsb2.va),
       rd32(gTcsrUsb3.va));
  gCoreClocked = clocksOn(gdsc, master, cfgNoc, aggre);
  note("USB core %s", gCoreClocked ? "powered and clocked: registers readable" : "NOT powered/clocked: its registers are not touched");
  return true;
}

void probeCore()
{
  using namespace dwc3;
  using namespace pianohw;
  if (!mapRegion(gDwc, kDwc3, kDwc3Bytes) || !mapRegion(gQs, kQscratch, 0x100)) { note("cannot map the core"); return; }
  auto r = [](uint32_t off) { return coreRd(off); };
  gSnpsId = r(GSNPSID);
  const uint32_t gctl = r(GCTL);   // first: the role rule (coreRd) needs the role before any device register
  note("core GSNPSID %08x GVERSION %08x GVERTYPE %08x GCTL %08x GSTS %08x GUCTL %08x GUCTL1 %08x GSBUSCFG0 %08x GSBUSCFG1 %08x",
       gSnpsId, r(GVERSION), r(GVERTYPE), gctl, r(GSTS), r(GUCTL), r(GUCTL1), r(GSBUSCFG0), r(GSBUSCFG1));
  note("core GHWPARAMS0 %08x 1 %08x 3 %08x 6 %08x 7 %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x GDBGLTSSM %08x", r(GHWPARAMS0),
       r(GHWPARAMS1), r(GHWPARAMS3), r(GHWPARAMS6), r(GHWPARAMS7), r(GUSB2PHYCFG), r(GUSB3PIPECTL), r(GDBGLTSSM));
  if (roleOf(gctl) == ROLE_DEVICE)
    note("device DCFG %08x DCTL %08x DSTS %08x (link state %u) DEVTEN %08x DALEPENA %08x GEVNTADR %08x%08x GEVNTSIZ %08x GEVNTCOUNT %08x",
         r(DCFG), r(DCTL), r(DSTS), dstsLinkState(r(DSTS)), r(DEVTEN), r(DALEPENA), r(GEVNTADRHI), r(GEVNTADRLO), r(GEVNTSIZ), r(GEVNTCOUNT));
  else
    note("device registers not read: the core is in the %s role (GCTL %08x); GEVNTADR %08x%08x GEVNTSIZ %08x GEVNTCOUNT %08x",
         cold::roleName(roleOf(gctl)), gctl, r(GEVNTADRHI), r(GEVNTADRLO), r(GEVNTSIZ), r(GEVNTCOUNT));
  note("TX FIFOs %08x %08x %08x %08x %08x, RX FIFO %08x; QSCRATCH general %08x hs_phy %08x ss_phy %08x", r(GTXFIFOSIZ0),
       r(GTXFIFOSIZ0 + 4), r(GTXFIFOSIZ0 + 8), r(GTXFIFOSIZ0 + 12), r(GTXFIFOSIZ0 + 16), r(GRXFIFOSIZ0), rd32(gQs.va + kQsGeneralCfg),
       rd32(gQs.va + kQsHsPhyCtrl), rd32(gQs.va + kQsSsPhyCtrl));
}

void probePhy()
{
  using namespace pianohw;
  if (!mapRegion(gM31, kM31, 0x1000)) return;
  note("M31 eUSB2 PHY [3c] %08x [50 por] %08x [54 common0] %08x [64 hs_ctrl2] %08x [94 override] %08x", rd32(gM31.va + kM31UtmiCtrl0),
       rd32(gM31.va + kM31Por), rd32(gM31.va + kM31Common0), rd32(gM31.va + kM31HsCtrl2), rd32(gM31.va + kM31Override));
}

int spmiApid(uint32_t ppid)
{
  const uint32_t version = rd32(gSpmiCore.va), count = rd32(gSpmiCore.va + 4) & 0x7ffu;
  if (version < 0x70000000u || version >= 0x80000000u || !count || count > 1024) {
    note("SPMI arbiter version %08x, %u peripherals: not the v7 layout, not read", version, count);
    return -1;
  }
  int found = -1;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t map = rd32(gSpmiCore.va + 0x2000 + 4 * i);
    if (((map >> 8) & 0xfffu) == ppid && (found < 0 || (rd32(gSpmiCnfg.va + 4 * i) & 7u) == 0)) found = int(i);
  }
  return found;
}

bool spmiRead(int apid, uint8_t offset, uint8_t *value)
{
  const uintptr_t ch = gSpmiObs.va + 0x20u * uint32_t(apid);
  wr32(ch, 1u << 27 | uint32_t(offset) << 4);   // observer channel of EE 0: extended read, one byte
  for (unsigned i = 0; i < 1000; ++i) {
    const uint32_t status = rd32(ch + 8);
    if (status & 0xeu) return false;
    if (status & 1u) { *value = uint8_t(rd32(ch + 0x18)); return true; }
    IODelay(1);
  }
  return false;
}

void probeRepeater()
{
  using namespace pianohw;
  if (!mapRegion(gSpmiCore, kSpmiCore, 0x3000) || !mapRegion(gSpmiCnfg, kSpmiCnfg, 0x1000) || !mapRegion(gSpmiObs, kSpmiObs, 0x8000)) return;
  const int apid = spmiApid(kRepeaterPpid);
  uint8_t status = 0, enable = 0;
  if (apid < 0 || !spmiRead(apid, kRepeaterStatus, &status) || !spmiRead(apid, kRepeaterEnable, &enable)) {
    note("eUSB2 repeater (SID 7, 0xfd00): not read (APID %d)", apid);
    return;
  }
  note("eUSB2 repeater (SID 7 APID %d): status [fd08] %02x enable [fd46] %02x (%s)", apid, status, enable,
       (status & 0x80) && (enable & 0x80) ? "ready and enabled" : "NOT ready/enabled");
}

uint32_t smmuRead(uint32_t off) { return rd32(gSmmu.va + off); }

SmmuPlan probeSmmu()
{
  using namespace pianohw;
  SmmuPlan p;
  if (!mapRegion(gSmmu, kSmmu, kSmmuBytes)) { p.error = "cannot map the SMMU"; return p; }
  p = planSmmuStream(kUsbStream, kSmmuBytes, smmuRead);
  note("SMMU stream %#x: groups %u banks %u pages %u; %s slot %u (smr %08x s2cr %08x; bank sctlr %08x cbar %08x cba2r %08x) -> %s bank %u%s%s",
       kUsbStream, p.groups, p.banks, p.pages, p.reuse ? "existing entry" : "free", p.slot, p.smr, p.s2cr, p.sctlr, p.cbar, p.cba2r,
       p.keep ? "keep" : "bypass", p.bank, p.error ? ": " : "", p.error ? p.error : "");
  return p;
}

bool smmuSetup()
{
  const SmmuPlan p = probeSmmu();
  if (p.error) return false;
  const uint64_t deadline = nowNs() + 100000000ull;
  const char *failure = applySmmuStream(p, pianohw::kUsbStream, smmuRead,
                                        [](uint32_t off, uint32_t v) { wr32(gSmmu.va + off, v); dsb(); },
                                        [deadline] { return nowNs() >= deadline; });
  note("SMMU stream %#x: %s (slot %u: smr %08x s2cr %08x)", pianohw::kUsbStream, failure ? failure : p.keep ? "left as it was" : "bypass set",
       p.slot, smmuRead(0x800 + 4 * p.slot), smmuRead(0xc00 + 4 * p.slot));
  return failure == nullptr;
}

// ---------------------------------------------------------------------------------------------------- hardware: takeover
bool waitGcc(uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
  for (unsigned i = 0; i < us / 10; ++i) {
    if ((gcc(off) & mask) == want) return true;
    IODelay(10);
  }
  return (gcc(off) & mask) == want;
}

// piano-usb=3: the controller's power domain and branch clocks, as the firmware leaves them for its own use (no RCG change).
bool enableClocks()
{
  using namespace pianohw;
  uint32_t v = gcc(kGccUsb30Gdsc);
  if (!(v & kGdscPwrOn)) {
    gccWrite(kGccUsb30Gdsc, (v & ~kGdscSwCollapse) | 1u << 11);   // SW_COLLAPSE off, RETAIN_FF on
    if (!waitGcc(kGccUsb30Gdsc, kGdscPwrOn, kGdscPwrOn, 100000)) { note("usb30 GDSC does not power on (%08x)", gcc(kGccUsb30Gdsc)); return false; }
  }
  static const uint32_t branches[] = {kGccCfgNocUsb3Axi, kGccAggreUsb3Axi, kGccUsb30Master, kGccUsb30Sleep, kGccUsb30MockUtmi};
  for (uint32_t b : branches) {
    v = gcc(b);
    if (!(v & kCbcrClkOff)) continue;
    gccWrite(b, v | kCbcrClkEnable);
    if (!waitGcc(b, kCbcrClkOff, 0, 10000)) { note("GCC branch %#x stays off (%08x)", b, gcc(b)); return false; }
  }
  if (!(rd32(gTcsrUsb2.va) & 1u)) { wr32(gTcsrUsb2.va, rd32(gTcsrUsb2.va) | 1u); dsb(); }
  note("clocks: GDSC %08x master %08x cfg_noc %08x aggre %08x tcsr usb2 %08x", gcc(kGccUsb30Gdsc), gcc(kGccUsb30Master),
       gcc(kGccCfgNocUsb3Axi), gcc(kGccAggreUsb3Axi), rd32(gTcsrUsb2.va));
  return clocksOn(gcc(kGccUsb30Gdsc), gcc(kGccUsb30Master), gcc(kGccCfgNocUsb3Axi), gcc(kGccAggreUsb3Axi));
}

// piano-usb-phy=1: the eUSB2 PHY reset and Project-SunUEFI's piano init sequence (the controller is halted).
void phyInit()
{
  using namespace pianohw;
  if (!mapRegion(gM31, kM31, 0x1000)) return;
  gccWrite(kGccQusb2PhyBcr, gcc(kGccQusb2PhyBcr) | 1u);
  IODelay(10);
  gccWrite(kGccQusb2PhyBcr, gcc(kGccQusb2PhyBcr) & ~1u);
  IODelay(10);
  for (const RegWrite &w : kM31Init) {
    if (w.offset == kDelay) { IODelay(w.value); continue; }
    wr32(gM31.va + w.offset, w.value);
    dsb();
  }
  probePhy();
}

// dwc3-qcom's UTMI-as-PIPE clock selection (QSCRATCH GENERAL_CFG): the core no longer needs the QMP PHY's PIPE clock.
void selectUtmi()
{
  using namespace pianohw;
  const uintptr_t cfg = gQs.va + kQsGeneralCfg;
  qsWrite(kQsGeneralCfg, rd32(cfg) | kQsPipeUtmiClkDis);
  IODelay(100);
  qsWrite(kQsGeneralCfg, rd32(cfg) | kQsPipeUtmiClkSel | kQsPipe3PhystatusSw);
  IODelay(100);
  qsWrite(kQsGeneralCfg, rd32(cfg) & ~kQsPipeUtmiClkDis);
  note("QSCRATCH general %08x: UTMI clock as PIPE clock", rd32(cfg));
}

// The Qualcomm glue's VBUS/session override: without it the core sees no attach (Project-SunUEFI test 65 on piano).
bool sessionValid()
{
  using namespace pianohw;
  const uintptr_t hs = gQs.va + kQsHsPhyCtrl, ss = gQs.va + kQsSsPhyCtrl;
  const uint32_t hsWant = rd32(hs) | kQsUtmiOtgVbusValid | kQsSwSessvldSel, ssWant = rd32(ss) | kQsLane0PwrPresent;
  qsWrite(kQsSsPhyCtrl, ssWant);
  qsWrite(kQsHsPhyCtrl, hsWant);
  note("QSCRATCH session: hs_phy %08x ss_phy %08x", rd32(hs), rd32(ss));
  return rd32(hs) == hsWant && rd32(ss) == ssWant;
}

bool dmaAlloc()
{
  if (gDmaVa) return true;
  gDma = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                                     IOOptionBits(kIOMemoryMapperNone), Layout::kBytes, 0x1000);
  if (!gDma) return false;
  IOByteCount contiguous = 0;
  gDmaPa = gDma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  uint8_t *cached = static_cast<uint8_t *>(gDma->getBytesNoCopy());
  if (!gDmaPa || (gDmaPa & 0xfff) || contiguous < Layout::kBytes || !cached) return false;
  bzero(cached, Layout::kBytes);
  // The core reaches this memory without cache coherence (SMMU bypass): the CPU uses a second, non-cacheable mapping only,
  // after the cacheable one is cleaned and dropped from the caches.
  for (uint32_t off = 0; off < Layout::kBytes; off += 64) __asm__ volatile("dc civac, %0" ::"r"(cached + off) : "memory");
  dsb();
  gDmaMap = gDma->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
  gDmaVa = gDmaMap ? reinterpret_cast<uint8_t *>(gDmaMap->getVirtualAddress()) : nullptr;
  note("DMA region %#llx (+%#x), non-cacheable CPU mapping %p", gDmaPa, Layout::kBytes, gDmaVa);
  return gDmaVa != nullptr;
}

// ------------------------------------------------------------------------------------ the cold start's kernel side (UsbCold.h)
// The lock words are PianoA830's (above); this kext's tag in both is "DWC3". The GCC vote lock: 10 us steps, at most 50 ms, a holder
// that keeps it longer does not stop the vote (the bit is only ever set; PianoTouch's and PianoWifi's rule). The DRV-2 lock: 10 us
// steps, at most 2 s, then nothing is sent (PianoKeyboard's and PianoBluetooth's rule).
constexpr uint32_t kLockTag = 0x44574333;   // "DWC3"
bool lockTake(volatile uint32_t *word, uint32_t steps, uint32_t *waits)
{
  for (uint32_t n = 0;; ++n) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, kLockTag, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *waits = n;
      return true;
    }
    if (n >= steps) {
      *waits = n;
      return false;
    }
    IODelay(10);
  }
}
void lockGive(volatile uint32_t *word)
{
  uint32_t expected = kLockTag;
  __atomic_compare_exchange_n(word, &expected, 0u, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// The rails' RPMh addresses (cmd-db, read once) and apps DRV-2 (mapped once); one VRM transaction a vote through PianoBluetooth's
// vrmSend (BtRpmh.h: an idle ACTIVE TCS 2, 1, 0; completion polled for at most 100 ms; a TCS that does not complete is never used
// again), with the DRV-2 lock word held around it.
uint32_t gRailAddr[cold::kRails];
bool gCmdDbTried, gCmdDbOk;
Region gRsc;
pianobt::Rsc gRscState;
bool gRscMapped;

bool coldCmdDb()
{
  using namespace pianohw;
  if (gCmdDbTried) return gCmdDbOk;
  gCmdDbTried = true;
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbBytes));
  Region db = {};
  if (!copy || !mapRegion(db, kCmdDb, kCmdDbBytes)) {
    if (copy) IOFreeData(copy, kCmdDbBytes);
    note("cold: cmd-db: cannot map %#llx or allocate its copy: no rail can be voted", (unsigned long long)kCmdDb);
    return false;
  }
  for (uint32_t i = 0; i < kCmdDbBytes; i += 4) {
    const uint32_t w = rd32(db.va + i);
    memcpy(copy + i, &w, 4);
  }
  db.map->release();
  gCmdDbOk = true;
  char line[200];
  size_t n = 0;
  for (unsigned i = 0; i < cold::kRails && n < sizeof line; ++i) {
    const int rc = pianokeyboard::cmdDbFind(copy, kCmdDbBytes, cold::kRailTable[i].cmdDb, &gRailAddr[i]);
    if (rc) gRailAddr[i] = 0;
    gCmdDbOk = gCmdDbOk && !rc;
    n += size_t(rc ? snprintf(line + n, sizeof line - n, "%s%s none (%d)", i ? ", " : "", cold::kRailTable[i].cmdDb, rc)
                   : snprintf(line + n, sizeof line - n, "%s%s %#x", i ? ", " : "", cold::kRailTable[i].cmdDb, gRailAddr[i]));
  }
  IOFreeData(copy, kCmdDbBytes);
  note("cold: cmd-db: %s", line);
  return gCmdDbOk;
}

uint32_t coldRscRd(void *, uint32_t off) { return rd32(gRsc.va + pianohw::kRscTcsBlock + off); }
void coldRscWr(void *, uint32_t off, uint32_t v)
{
  wr32(gRsc.va + pianohw::kRscTcsBlock + off, v);
  dsb();
}
void coldRscSleep(void *, unsigned ms) { IOSleep(ms); }
void coldRscDelay(void *, unsigned us) { IODelay(us); }   // BtRpmh.h's poll under the DRV-2 lock: busy, never a sleep (P17)

bool coldRsc()
{
  if (gRscMapped) return true;
  if (!mapRegion(gRsc, pianohw::kRscDrv2, pianohw::kRscDrv2Bytes)) {
    note("cold: rsc: cannot map apps DRV-2 (%#llx): no rail can be voted", (unsigned long long)pianohw::kRscDrv2);
    return false;
  }
  const uint32_t id = rd32(gRsc.va);
  gRscState.layout = pianokeyboard::rscLayout(id);
  gRscMapped = true;
  note("cold: rsc: apps DRV-2 id %#x (version %u.%u), TCS block +%#x", id, (id >> 16) & 0xff, (id >> 8) & 0xff, pianohw::kRscTcsBlock);
  return true;
}

// One rail: enable = mode 7 (high power), the set point, enable 1 (PianoKeyboard's order); else enable 0 only (the test hook).
int coldRailVote(unsigned i, uint16_t mv, bool enable)
{
  if (i >= cold::kRails) return -22;
  if (!coldCmdDb() && !gRailAddr[i]) return -19;
  if (!gRailAddr[i] || !coldRsc()) return -19;
  const PianoWcnVrmCmd on[3] = {{PianoWcnVrmMode, PianoWcnVrmModeNpm}, {PianoWcnVrmVoltage, mv}, {PianoWcnVrmEnable, 1}};
  const PianoWcnVrmCmd off[1] = {{PianoWcnVrmEnable, 0}};
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("cold: rsc: the DRV-2 lock is held by %#x: waiting (at most 2 s)", held);
  if (!lockTake(&sm8750_rpmh_drv2_owner, 200000, &waits)) {
    note("cold: rsc: the DRV-2 lock stayed held by %#x for 2 s: %s not voted", __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE),
         cold::kRailTable[i].cmdDb);
    return -16;
  }
  pianobt::VrmResult res;
  const pianobt::RscIo io = {nullptr, coldRscRd, coldRscWr, coldRscSleep, coldRscDelay};
  const int rc = pianobt::vrmSend(gRscState, io, gRailAddr[i], enable ? on : off, enable ? 3 : 1, res);
  lockGive(&sm8750_rpmh_drv2_owner);
  note("cold: rsc: %s (%#x) %s: TCS %d %s after %u us (status %#x %#x %#x; DRV-2 lock after %u waits of 10 us)", cold::kRailTable[i].cmdDb,
       gRailAddr[i], enable ? "mode 7, set point, enable 1" : "enable 0", res.tcs, res.done ? "completed" : rc == -16 ? "NOT sent (no idle TCS)" :
       "NOT completed (left in flight, not used again)", res.waitedUs, res.status[0], res.status[1], res.status[2], waits);
  return rc;
}

// SPMI for the cold start: the APIDs found once (EE 0's entry preferred), reads through the observer channel (spmiRead above), one
// write channel (the repeater's) mapped on first use. piano-usb-spmi=0: nothing (-2).
struct ApidEntry { uint16_t ppid; int apid; };
ApidEntry gApids[12];
unsigned gApidCount;
Region gChnl;
int gChnlApid = -1;

bool coldSpmiMapped()
{
  using namespace pianohw;
  return mapRegion(gSpmiCore, kSpmiCore, 0x3000) && mapRegion(gSpmiCnfg, kSpmiCnfg, 0x1000) && mapRegion(gSpmiObs, kSpmiObs, 0x8000);
}
int coldApid(uint16_t ppid)
{
  for (unsigned i = 0; i < gApidCount; ++i)
    if (gApids[i].ppid == ppid) return gApids[i].apid;
  if (!coldSpmiMapped()) return -19;
  const uint32_t version = rd32(gSpmiCore.va), count = rd32(gSpmiCore.va + 4) & 0x7ffu;
  int found = -1;
  if (version >= 0x70000000u && version < 0x80000000u && count && count <= 1024)
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t map = rd32(gSpmiCore.va + 0x2000 + 4 * i);
      if (map && ((map >> 8) & 0xfffu) == ppid && (found < 0 || (rd32(gSpmiCnfg.va + 4 * i) & 7u) == 0)) found = int(i);
    }
  if (gApidCount < sizeof gApids / sizeof gApids[0]) gApids[gApidCount++] = {ppid, found};
  return found;
}
int coldSpmiRead(uint16_t ppid, uint8_t reg, uint8_t *v)
{
  if (!gArgs.spmi) return -2;
  const int apid = coldApid(ppid);
  if (apid < 0) return -19;
  return spmiRead(apid, reg, v) ? 0 : -5;
}
int coldSpmiOwner(uint16_t ppid)
{
  if (!gArgs.spmi) return -2;
  const int apid = coldApid(ppid);
  return apid < 0 ? -1 : int(rd32(gSpmiCnfg.va + 4 * uint32_t(apid)) & 7u);
}
int coldSpmiWrite(uint16_t ppid, uint8_t reg, uint8_t v)
{
  using namespace pianohw;
  if (!gArgs.spmi) return -2;
  const int apid = coldApid(ppid);
  if (apid < 0) return -19;
  if (coldSpmiOwner(ppid) != 0) return -1;
  if (gChnlApid != apid) {
    if (gChnl.map) gChnl.map->release();
    gChnl = {};
    gChnlApid = -1;
    if (!mapRegion(gChnl, kSpmiChnl + uint64_t(kSpmiChnlStride) * uint32_t(apid), 0x20)) return -12;
    gChnlApid = apid;
  }
  wr32(gChnl.va + 0x10, v);
  wr32(gChnl.va + 0x00, uint32_t(reg) << 4);   // opcode 0: extended write, one byte
  dsb();
  for (unsigned i = 0; i < 1000; ++i) {
    const uint32_t st = rd32(gChnl.va + 0x08);
    if (st & 1u) return (st & 0xeu) ? -5 : 0;
    IODelay(1);
  }
  return -60;
}

// The PMIC's Type-C block (ChargerPmic.h's discovery), read only, and only where no other kext reads it: one observer channel serves
// one reader at a time, and PianoPower (any level) and PianoUsbHost read this block themselves.
uint32_t coldIoRd(void *, pianopower::Window w, uint32_t off)
{
  return rd32((w == pianopower::Window::core ? gSpmiCore.va : w == pianopower::Window::cnfg ? gSpmiCnfg.va : gSpmiObs.va) + off);
}
void coldIoWr(void *, pianopower::Window w, uint32_t off, uint32_t v)
{
  if (w == pianopower::Window::obsv) wr32(gSpmiObs.va + off, v);
}
void coldIoDelay(void *, unsigned us) { IODelay(us); }

cold::TypeC coldTypec()
{
  cold::TypeC t;
  if (!gArgs.spmi || bootArg("piano-power", 0) || bootArg("piano-usbhost", 0) || !coldSpmiMapped()) return t;
  const pianopower::Io io = {nullptr, coldIoRd, coldIoWr, coldIoDelay};
  const pianopower::Arbiter a = pianopower::spmiArbiter(io);
  if (!a.v7) return t;
  const pianopower::ChargerLayout l = pianopower::discoverCharger(io, a);
  uint8_t v[6] = {};
  if (!l.found || pianopower::spmiRead(io, l.typec.apid, 0x06, v, 6) != pianopower::ReadResult::ok) return t;
  t.snk = v[0];
  t.src = v[2];
  t.misc = v[5];
  (void)pianopower::spmiRead(io, l.typec.apid, 0x44, &t.mode, 1);
  t.read = true;
  return t;
}

// UsbCold.h's Hal. Before RUN_STOP its lines go straight to IOLog (as the takeover's); the USB thread is the only caller (and start()
// at piano-usb=1, read only).
using ColdTypeC = cold::TypeC;
struct ColdHal {
  uint32_t gcc(uint32_t off) { return ::gcc(off); }
  void gccWrite(uint32_t off, uint32_t v) { ::gccWrite(off, v); }
  uint32_t tcsr() { return rd32(gTcsrUsb2.va); }
  void tcsrWrite(uint32_t v)
  {
    wr32(gTcsrUsb2.va, v);
    dsb();
  }
  uint32_t core(uint32_t off) { return coreRd(off); }
  void coreWrite(uint32_t off, uint32_t v)
  {
    coreWr(off, v);
    dsb();
  }
  uint32_t m31(uint32_t off) { return rd32(gM31.va + off); }
  void m31Write(uint32_t off, uint32_t v)
  {
    wr32(gM31.va + off, v);
    dsb();
  }
  uint32_t qs(uint32_t off) { return rd32(gQs.va + off); }
  bool qsWrite(uint32_t off, uint32_t v) { return ::qsWrite(off, v); }
  int spmiRead(uint16_t ppid, uint8_t reg, uint8_t *v) { return coldSpmiRead(ppid, reg, v); }
  int spmiWrite(uint16_t ppid, uint8_t reg, uint8_t v) { return coldSpmiWrite(ppid, reg, v); }
  int spmiOwner(uint16_t ppid) { return coldSpmiOwner(ppid); }
  int railVote(unsigned rail, uint16_t mv, bool enable) { return coldRailVote(rail, mv, enable); }
  bool voteLock(bool take)
  {
    if (!take) {
      lockGive(&sm8750_gcc_vote_owner);
      return true;
    }
    uint32_t waits = 0;
    if (lockTake(&sm8750_gcc_vote_owner, 5000, &waits)) return true;
    note("cold: the GCC vote lock stayed held by %#x for 50 ms: GPLL0's vote bit is set anyway (only ever set)",
         __atomic_load_n(&sm8750_gcc_vote_owner, __ATOMIC_ACQUIRE));
    return false;
  }
  bool gpio61(uint32_t *cfg, uint32_t *io)
  {
    if (!mapRegion(gTlmm61, pianohw::kTlmmGpio61, 8)) return false;
    *cfg = rd32(gTlmm61.va);
    *io = rd32(gTlmm61.va + 4);
    return true;
  }
  ColdTypeC typec() { return coldTypec(); }
  void udelay(unsigned us)
  {
    if (us >= 1000) IOSleep(us / 1000);
    else IODelay(us);
  }
  void cold(int step) { piano_dwc3_cold = step; }
  __attribute__((format(printf, 2, 3))) void note(const char *fmt, ...)
  {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoDwc3: %s\n", line);
  }
};

// Every register block the cold start and its inspection may touch, mapped (mapping touches nothing).
bool coldMap()
{
  using namespace pianohw;
  return gGccUsb.va && gTcsrUsb2.va && mapRegion(gDwc, kDwc3, kDwc3Bytes) && mapRegion(gQs, kQscratch, 0x100) && mapRegion(gM31, kM31, 0x1000);
}

// What the cold start saw and did this boot (the monitor's `usb`).
cold::Watch gWatch;
cold::Result gColdLast;
uint32_t gColdReasons;
bool gColdRan;

// The test hook (if any), the inherited state read and reported, the decision: true = the cold start.
bool coldDecide(uint32_t sim)
{
  ColdHal h;
  if (!coldMap()) {
    note("cold: cannot map the USB blocks: the inherited state is taken as it is");
    return false;
  }
  if (sim) cold::simulate(h, sim);
  const cold::Inherited s = cold::inspect(h, gArgs.spmi != 0, (sim & cold::kSimRails) != 0);
  gColdReasons = cold::report(h, s, gArgs.cold, gArgs.spmi != 0);
  return gArgs.cold == cold::Mode::always || (gArgs.cold == cold::Mode::automatic && gColdReasons);
}

uint32_t gLastDsts = ~0u;

// coldStart: UsbCold.h's c1..c5 first (power, stop, rails, repeater, M31 PHY), instead of trusting what the firmware left.
bool takeover(bool coldStart)
{
  using namespace dwc3;
  using namespace pianohw;
  stage(10, "takeover: power and clocks");
  if (coldStart) {
    stage(11, "takeover: cold start (piano-usb-cold: power, stop, rails, repeater, M31 PHY)");
    ColdHal h;
    gColdRan = true;
    ++gWatch.colds;
    gColdLast = coldMap() ? cold::coldStart(h, gArgs.coldSteps) : cold::Result{};
    if (!gColdLast.ok() || !coldMap()) { stage(-11, "takeover: the cold start (its 'cold cN FAILED' line names the step)"); return false; }
    gCoreClocked = true;
    gSnpsId = 0;   // read again below
  } else {
    // The glue's clocks as GCC reads them now (a test hook or a hand-off may have changed them since the probe).
    gCoreClocked = glueClocked();
    if (!gCoreClocked && gArgs.level >= 3) gCoreClocked = enableClocks();
  }
  if (!gCoreClocked) { stage(-10, "takeover: the core is not powered/clocked (piano-usb=3 switches GDSC and clocks on)"); return false; }
  if (!gSnpsId) probeCore();
  const uint32_t ip = gSnpsId >> 16;
  if (ip != 0x5533 && ip != 0x3331 && ip != 0x3332) { stage(-11, "takeover: GSNPSID is not a DWC_usb3x core"); return false; }
  const bool dwc31 = ip != 0x5533;
  // Port p uses IN endpoints 2p+1 and 2p+2 (EP0 IN is the first): two ports need 5, the input port 7. The network function uses 7
  // and 8 whatever the port count: 9 (piano: 16).
  const uint32_t inEps = (gHal.rd(GHWPARAMS3) >> 18) & 0x1fu;
  const unsigned wanted = gArgs.hid ? 3 : 2, ports = inEps >= 2 * wanted + 1 ? wanted : inEps >= 5 ? 2 : 1;
  bool net = gArgs.net != 0 && inEps >= kNetInEndpoints;
  note("core %s, %u IN endpoints: %u ACM port(s)%s%s", dwc31 ? "DWC_usb31/32" : "DWC_usb3", inEps, ports,
       gArgs.hid && ports < 3 ? " (no input port: it needs 7 IN endpoints)" : gArgs.hid ? " (port 2: piano input)" : "",
       net ? " + the CDC-NCM network function (interfaces after the ports, endpoints 7 and 8)" :
       gArgs.net ? " (no network function: it needs 9 IN endpoints)" : "");
  stage(12, "takeover: halt");
  {   // The role rule's gate (UsbCold.h ensureDevice), before the first device register, in every mode: a core that does not read
      // the device role (a boot path that ran it as a host, a hand-off not given back, piano-usb-cold=0) gets the xHCI halted and
      // the switch first; one that still does not is left alone (C6's order hung the system).
    ColdHal h;
    cold::RoleSwitch rs;
    if (!coldMap() || !cold::ensureDevice(h, rs)) {
      stage(-12, "takeover: the device role (the 'takeover: role:' line says why; no device register was touched)");
      return false;
    }
  }
  if (gHal.rd(DCTL) & DCTL_RUN_STOP) note("the firmware left RUN_STOP set (DSTS %08x)", gHal.rd(DSTS));
  gGadget.bind(&gHal, ports, uint16_t(gArgs.pid), gArgs.trace, net);
  if (!gGadget.halt(100)) note("no halt within 100 ms (DSTS %08x): continuing with the soft reset", gHal.rd(DSTS));
  stage(13, "takeover: DMA memory");
  if (!dmaAlloc()) { stage(-13, "takeover: DMA memory"); return false; }
  if (gArgs.phy && !coldStart) {   // the cold start's c5 did it
    stage(14, "takeover: M31 eUSB2 PHY init");
    phyInit();
  }
  if (gArgs.utmi) {
    stage(15, "takeover: UTMI clock select");
    selectUtmi();
  }
  stage(16, "takeover: core soft reset");
  uint32_t waited = 0;
  if (!gGadget.softReset(dwc31, &waited)) {
    note("DCTL %08x after %u us", gHal.rd(DCTL), waited);
    stage(-16, "takeover: core soft reset");
    return false;
  }
  note("soft reset done in %u us: GCTL %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x DSTS %08x", waited, gHal.rd(GCTL), gHal.rd(GUSB2PHYCFG),
       gHal.rd(GUSB3PIPECTL), gHal.rd(DSTS));
  // IN endpoint n sends from TX FIFO n: their sizes after the reset (the input port's endpoints 5 and 6 use FIFOs 5 and 6).
  note("TX FIFOs after the reset: %08x %08x %08x %08x %08x %08x %08x %08x", gHal.rd(GTXFIFOSIZ0), gHal.rd(GTXFIFOSIZ0 + 4),
       gHal.rd(GTXFIFOSIZ0 + 8), gHal.rd(GTXFIFOSIZ0 + 12), gHal.rd(GTXFIFOSIZ0 + 16), gHal.rd(GTXFIFOSIZ0 + 20), gHal.rd(GTXFIFOSIZ0 + 24),
       gHal.rd(GTXFIFOSIZ0 + 28));
  // The network function: its IN endpoints' TX FIFOs (7, 8) and the ports' are laid out after EP0's (TxFifoPlan.h), as the reset
  // values of FIFOs past the firmware's own are not known. Without piano-usb-net no FIFO is written; piano-usb-net-fifo=0 keeps the
  // reset's sizes (a first-run experiment: is it the plan or the function).
  if (net && !gArgs.netFifo) note("net: piano-usb-net-fifo=0: the TX FIFOs stay as the reset left them");
  if (net && gArgs.netFifo) {
    uint8_t packets[TxFifoPlan::kMax] = {};
    for (unsigned p = 0; p < ports; ++p) {
      packets[notifyEp(p)] = 1;
      packets[bulkEp(p)] = 4;
    }
    packets[kNetNotifyEp] = 1;
    packets[kNetBulkEp] = 12;
    const uint32_t fifo0 = gHal.rd(GTXFIFOSIZ0);
    const TxFifoPlan plan = planTxFifos(packets, TxFifoPlan::kMax, fifo0,
                                        txFifoRamWords(gHal.rd(GHWPARAMS1), gHal.rd(GHWPARAMS6), gHal.rd(GHWPARAMS7)),
                                        txFifoWordBytes(gHal.rd(GHWPARAMS0), gHal.rd(GHWPARAMS6), ip == 0x3332), dwc31);
    if (plan.error) {
      note("net: TX FIFO plan: %s (FIFO 0 %08x, RAM %u words of %u bytes): the network function is left out", plan.error, fifo0, plan.ram,
           plan.wordBytes);
      net = false;
      gGadget.bind(&gHal, ports, uint16_t(gArgs.pid), gArgs.trace, false);
    } else {
      gGadget.setTxFifoPlan(plan);
      note("net: TX FIFO plan: words %u..%u of %u (%u bytes each): 1 %08x 2 %08x 3 %08x 4 %08x 5 %08x 6 %08x 7 %08x 8 %08x", plan.begin, plan.end,
           plan.ram, plan.wordBytes, plan.value[1], plan.value[2], plan.value[3], plan.value[4], plan.value[5], plan.value[6], plan.value[7],
           plan.value[8]);
    }
  }
  stage(17, "takeover: device mode, event buffer, EP0");
  if (!gGadget.init()) { stage(-17, "takeover: endpoint commands"); return false; }
  if (net)
    note("net: TX FIFOs now: %08x %08x %08x %08x %08x %08x %08x %08x %08x", gHal.rd(GTXFIFOSIZ0), gHal.rd(GTXFIFOSIZ0 + 4), gHal.rd(GTXFIFOSIZ0 + 8),
         gHal.rd(GTXFIFOSIZ0 + 12), gHal.rd(GTXFIFOSIZ0 + 16), gHal.rd(GTXFIFOSIZ0 + 20), gHal.rd(GTXFIFOSIZ0 + 24), gHal.rd(GTXFIFOSIZ0 + 28),
         gHal.rd(GTXFIFOSIZ0 + 32));
  stage(18, "takeover: VBUS/session override");
  if (!sessionValid()) note("QSCRATCH readback differs (continuing)");
  stage(19, "takeover: run");
  // From here on the host may be enumerating: notes only through the note thread.
  if (!gGadget.run(50)) gHal.note("DSTS %08x: the core did not leave halt within 50 ms", gHal.rd(DSTS));
  piano_dwc3_stage = 20;
  gHal.note("stage 20 running: DCFG %08x DCTL %08x DSTS %08x GEVNTCOUNT %08x; waiting for the Mac (VID:PID %04x:%04x)", gHal.rd(DCFG),
            gHal.rd(DCTL), gHal.rd(DSTS), gHal.rd(GEVNTCOUNT), kVendor, gArgs.pid);
  if (gRoleRefused)
    gHal.note("role rule: %u core accesses refused so far (the first at %#x): a path reached a device register outside the device role "
              "(or the xHCI outside the host role); report this line", gRoleRefused, gRoleRefusedFirst);
  return true;
}

void usbStatus()
{
  using namespace dwc3;
  const GadgetStats &s = gGadget.stats;
  const uint32_t gctl = glueClocked() && gDwc.va ? gHal.rd(GCTL) : 0;   // first: the role rule
  if (glueClocked() && gDwc.va && roleOf(gctl) == ROLE_DEVICE)
    cprintf("piano-usb=%u stage %d; core %08x DCTL %08x DSTS %08x (link %u) DCFG %08x GEVNTCOUNT %08x\n", gArgs.level, piano_dwc3_stage,
            gSnpsId, gHal.rd(DCTL), gHal.rd(DSTS), dstsLinkState(gHal.rd(DSTS)), gHal.rd(DCFG), gHal.rd(GEVNTCOUNT));
  else if (glueClocked() && gDwc.va)
    cprintf("piano-usb=%u stage %d; core %08x GCTL %08x: the %s role, the device registers are not read\n", gArgs.level, piano_dwc3_stage,
            gSnpsId, gctl, cold::roleName(roleOf(gctl)));
  else
    cprintf("piano-usb=%u stage %d; the glue is NOT clocked: the core is not read\n", gArgs.level, piano_dwc3_stage);
  const cold::RoleSwitch &rs = gColdLast.role;
  if (gColdRan)
    cprintf("role: the last cold start's c2 found role %u (%s), xHCI %d, soft switch %s, block reset %s; role rule: %u accesses refused "
            "(the first at %#x)\n", rs.found, cold::roleName(rs.found), rs.xhci, !rs.soft ? "not tried" : rs.proven ? "proven" : "NOT proven",
            rs.blockReset ? rs.bcrWhy : "not taken", gRoleRefused, gRoleRefusedFirst);
  else
    cprintf("role: no cold start this boot; role rule: %u accesses refused (the first at %#x)\n", gRoleRefused, gRoleRefusedFirst);
  char why[480];
  cold::whyText(gColdReasons, why, sizeof why);
  cprintf("cold: piano-usb-cold=%s steps %#x sim %#x wait %u s; at the takeover the inherited state had: %s; cold starts %u (at most %u), "
          "breadcrumb %d, last soft failures %#x; link watch %s\n", cold::modeName(gArgs.cold), gArgs.coldSteps, gArgs.coldSim, gArgs.coldWait,
          why, gWatch.colds, cold::kColdTries, piano_dwc3_cold, gColdLast.soft, gWatch.armed ? "armed" : gWatch.hostSeen ? "done (a host came)" :
          "off");
  cprintf("speed %u address %u configuration %u; events %u (device %u): resets %u connects %u disconnects %u erratic %u overflows %u\n",
          s.speed, s.address, s.configuration, s.events, s.deviceEvents, s.resets, s.connects, s.disconnects, s.erratic, s.overflows);
  cprintf("setups %u stalls %u EP0 timeouts %u; command errors %u timeouts %u (last %08x) start errors %u stale %u; notes dropped %llu\n",
          s.setups, s.stalls, s.ep0Timeouts, s.cmdErrors, s.cmdTimeouts, s.lastCmdFailure, s.startErrors, s.stale, gNotes.dropped);
  cprintf("bulk in %u transfers %llu bytes, out %u transfers %llu bytes; ports DTR %x/%x; tx0 %u dropped %llu, tx1 %u dropped %llu\n",
          s.inTransfers, s.inBytes, s.outTransfers, s.outBytes, gLines[0], gLines[1], gTx[0].used(), gTx[0].dropped, gTx[1].used(),
          gTx[1].dropped);
  const LogCursor &lc = gLog.cur;
  cprintf("log: %s capacity %u; written %llu sent %llu (behind %llu), session %u, %llu bytes lost to an overtaking writer (%u times), "
          "%u header steps back ignored; /dev/pianousb %s\n", !gLog.ok ? "none" : gLog.msgbuf ? "msgbuf" : "pstore", gLog.cap,
          (unsigned long long)lc.written, (unsigned long long)lc.sent, (unsigned long long)lc.behind(), lc.sessions,
          (unsigned long long)lc.lost, lc.laps, lc.backSteps, gDevNode ? "present" : "absent");
  cprintf("sleep: %s (piano-sleep-usb=%u); %u sleeps, %u wakes; QSCRATCH writes recorded %u (lost %u)\n", sleepStateName(gSleep.state()),
          gArgs.sleepUsb, gSleep.sleeps(), gSleep.wakes(), gSleep.deferred(), gSleep.lost());
  cprintf("wait: %s, tier %s; SPI 133: %s, %llu handled, %llu strays, %u missed\n",
          !gArgs.idle ? "1 ms loop (piano-usb-idle=0)" : gIrqMode ? "interrupt" : "polled, adaptive", tierName(gTier),
          gIrqLine.up ? (gIrqLine.tripped ? "disabled (storm)" : "attached") : "not attached", (unsigned long long)gIrqLine.count(),
          (unsigned long long)gIrqLine.strays, gIrqMiss.total);
  if (gArgs.hid)
    cprintf("hid: keyboard %s pointer %s; port 2 DTR %x; %u commands (keyboard %u consumer %u pointer %u), %u releases, %u auto-releases, "
            "%u bad lines, %u send errors\n", gHidDevice[0] ? "up" : "absent", gHidDevice[1] ? "up" : "absent", gLines[kPortInput],
            gHid.commands, gHid.keyboard, gHid.consumer, gHid.pointer, gHid.releases, gHid.autoReleases, gHid.errors, gHid.sendErrors);
  if (gGadget.net()) {
    const ncm::NetStats &n = gGadget.netStats;
    char line[400];
    pianonet::status(line, sizeof line);
    cprintf("net: %s\n", line);
    cprintf("net: alternate %u, NTB input size %u, filter %#x, %u notifications, %u zero-length packets, %u alternate changes\n", n.alt,
            n.ntbInSize, n.filter, n.notifications, n.zlps, n.altChanges);
  } else if (gArgs.net) {
    cprintf("net: piano-usb-net=1 but the network function is left out (see the takeover's notes)\n");
  }
}

// ------------------------------------------------------------------------------- the core's interrupt (piano-usb-irq=1)
// dwc_usb3, GIC SPI 133, level-high: up while the event buffer holds events (GEVNTCOUNT) and GEVNTSIZ.INTMASK is clear. The
// primary handler masks it (the line drops) and kicks the thread; the thread takes the events and unmasks before it waits. A storm
// (entries with nothing to mask) disables the line at the GIC; three timed-out waits in a row with events pending and the line
// unmasked mean it never arrives. Either way the thread masks it for good and polls (UsbIdle.h's polled periods).
void coreInterrupt(void *)
{
  using namespace dwc3;
  const uint32_t size = rd32(gDwc.va + GEVNTSIZ), count = rd32(gDwc.va + GEVNTCOUNT);
  if (dwc3IrqAction(count, size) == IrqAction::mask) {
    wr32(gDwc.va + GEVNTSIZ, size | GEVNTSIZ_INTMASK);
    dsb();
    __atomic_add_fetch(&gIrqTaken, 1, __ATOMIC_RELEASE);
    gWake.kick();
    return;
  }
  if (gIrqLine.stray()) {
    wr32(gDwc.va + GEVNTSIZ, size | GEVNTSIZ_INTMASK);
    dsb();
    gWake.kick();
  }
}

bool attachCoreInterrupt()
{
  const uint32_t id = pianoirq::intid(pianoirq::kSpiDwc3);
  const IOReturn r = gIrqLine.attach(id, false, coreInterrupt, nullptr);
  const uint32_t rb = gIrqLine.readback;
  gHal.note("irq: SPI %u (INTID %u, level) %s (%#x): readback %#x (group 1 %u, enabled %u, edge %u, priority %#x), router %#llx; %s",
            pianoirq::kSpiDwc3, id, r == kIOReturnSuccess ? "attached" : "NOT attached", r, rb, rb & 1, (rb >> 1) & 1, (rb >> 2) & 1,
            (rb >> 8) & 0xff, (unsigned long long)gIrqLine.router,
            r == kIOReturnSuccess ? "events wake the USB thread (periods: busy 10 ms, port open 20 ms, configured 200 ms, no host 1 s)"
                                  : "polled instead");
  return r == kIOReturnSuccess;
}

void irqOff(const char *why)
{
  gIrqMode = false;
  gHal.wr(dwc3::GEVNTSIZ, dwc3::GEVNTSIZ_INTMASK | Layout::kEventBytes);
  gHal.barrier();
  gHal.note("irq: off (%s): %llu handled, %llu strays, %u missed; polled from now on", why, (unsigned long long)gIrqLine.count(),
            (unsigned long long)gIrqLine.strays, gIrqMiss.total);
}

// The USB thread's side of a system sleep, at the top of each pass: carries out an off or an on asked by setPowerState (UsbSleep.h)
// and writes what QSCRATCH writes were recorded once awake and clocked. Returns whether the pass may touch the core.
bool sleepStep()
{
  using namespace dwc3;
  IOLockLock(gSleepLock);
  const uint32_t ask = gSleepAsk, seq = gSleepSeq;
  gSleepAsk = kAskNone;
  if (ask == kAskOff) {
    const SleepOff o = gSleep.off(true, gGadget.configured(), gIrqMode);
    const uint32_t n = gSleep.sleeps();
    IOLockUnlock(gSleepLock);
    if (o.maskIrq) {
      gHal.wr(GEVNTSIZ, GEVNTSIZ_INTMASK | Layout::kEventBytes);
      gHal.barrier();
    }
    if (o.halt) {
      const bool halted = gGadget.sleepDisconnect();
      gHal.note("sleep %u: soft disconnect (%s): RUN_STOP off (%s), the endpoints and the address forgotten; the core is left alone "
                "until the wake", n, gArgs.sleepUsb ? "no host has the device configured" : "piano-sleep-usb=0",
                halted ? "halted" : "NOT halted within 100 ms");
    } else if (o.keepPolling) {
      gHal.note("sleep %u: the Mac has the device configured: kept (the console survives the suspend; piano-sleep-usb=0 disconnects "
                "instead)%s", n, o.maskIrq ? "; the core's interrupt masked, polled until the freeze" : "");
    }
    IOLockLock(gSleepLock);
    gSleepDone = seq;
    IOLockWakeup(gSleepLock, &gSleepDone, false);
  } else if (ask == kAskOn) {
    const bool clocked = glueClocked();
    const SleepOn o = gSleep.on(clocked);
    const uint32_t n = gSleep.wakes();
    IOLockUnlock(gSleepLock);
    if (!o.touch) {
      using namespace pianohw;
      gHal.note("wake %u: the glue is NOT clocked (GDSC %08x, master %08x, cfg_noc %08x, aggre %08x): the core and QSCRATCH are left "
                "alone for the rest of the boot", n, gcc(kGccUsb30Gdsc), gcc(kGccUsb30Master), gcc(kGccCfgNocUsb3Axi),
                gcc(kGccAggreUsb3Axi));
    } else if (o.reconnect) {
      const bool ok = gGadget.sleepConnect();
      gHal.note("wake %u: the glue clocked: %s", n, ok ? "connected again (RUN_STOP on): the Mac enumerates afresh"
                                                       : "the core did not restart (DSTS below); left as it is");
      if (!ok) gHal.note("DSTS %08x DCTL %08x", gHal.rd(DSTS), gHal.rd(DCTL));
    } else {
      gHal.note("wake %u: the glue clocked; %s", n, gGadget.configured() ? "kept: the console goes on" : "polling again");
    }
    IOLockLock(gSleepLock);
  }
  // QSCRATCH writes recorded while asleep or unclocked (a takeover cut by a sleep): written once awake with the glue clocked.
  uint32_t offset[UsbSleep::kRecorded], value[UsbSleep::kRecorded];
  unsigned replay = 0;
  if (gSleep.state() == SleepState::awake && gSleep.recorded() && glueClocked()) {
    replay = gSleep.recorded();
    for (unsigned i = 0; i < replay; ++i) {
      offset[i] = gSleep.recordedOffset(i);
      value[i] = gSleep.recordedValue(i);
    }
    gSleep.replayed();
  }
  const bool usable = gSleep.coreUsable();
  IOLockUnlock(gSleepLock);
  for (unsigned i = 0; i < replay; ++i) {
    wr32(gQs.va + offset[i], value[i]);
    dsb();
    gHal.note("QSCRATCH %#x = %08x written (recorded while asleep or unclocked), reads %08x", offset[i], value[i], rd32(gQs.va + offset[i]));
  }
  return usable;
}

// Before the takeover: a system sleep that came first (piano-usb-delay) is waited out.
void waitAwake()
{
  IOLockLock(gSleepLock);
  if (!gSleep.takeoverMayRun()) note("takeover: waiting for the wake (a system sleep came first)");
  while (!gSleep.takeoverMayRun()) IOLockSleep(gSleepLock, &gSleep, THREAD_UNINT);
  IOLockUnlock(gSleepLock);
}

// ------------------------------------------------------------------------------- the hand-off to PianoUsbHost (piano-usb-handoff=1)
// PianoUsbHost asks through callPlatformFunction (UsbHandoff.h); the USB thread acts between passes: a release stops the device
// controller (RUN_STOP 0, the halt awaited), masks its event interrupt and parks the thread until a reclaim; a reclaim re-reads the
// core's power domain and clocks and, only when they read on, runs the whole takeover again (PianoUsbHost has put the core back in
// device mode first). Without piano-usb-handoff nothing of this runs and the platform function answers unsupported.
volatile uint32_t gHandoffState = kHoOff, gHandoffAsk = kAskState;
uint32_t gHandoffReleases, gHandoffReclaims;

void handoffSet(uint32_t state)
{
  __atomic_store_n(&gHandoffState, state, __ATOMIC_RELEASE);
  thread_wakeup((event_t)&gHandoffState);
}
uint32_t handoffTake() { return __atomic_exchange_n(&gHandoffAsk, uint32_t(kAskState), __ATOMIC_ACQ_REL); }

void handoffRelease()
{
  using namespace dwc3;
  gRunning = false;
  const bool halted = gGadget.halt(100);
  gHal.wr(GEVNTSIZ, GEVNTSIZ_INTMASK | Layout::kEventBytes);
  gHal.barrier();
  if (gIrqMode) irqOff("the controller is handed to PianoUsbHost");
  gHandoffReleases++;
  gHal.note("hand-off: released to PianoUsbHost (%u): DCTL %08x DSTS %08x (%s); the console keeps its log for the replay", gHandoffReleases,
            gHal.rd(DCTL), gHal.rd(DSTS), halted ? "halted" : "NOT halted within 100 ms");
  __atomic_store_n(&gRoleSeen, 0xffu, __ATOMIC_RELAXED);   // PianoUsbHost changes the role: not known until GCTL is read again
  handoffSet(kHoReleased);
}

// Parked (released, or a takeover that failed): waits for a reclaim; returns once a takeover after one went through. gRunning is
// false meanwhile, so a system sleep is answered by setPowerState itself without touching the core (UsbSleep.h's early state).
void handoffPark()
{
  for (;;) {
    const uint32_t seen = gWake.now();
    const uint32_t ask = handoffTake();
    if (handoffDecide(__atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE), ask) == HandoffDo::reclaim) {
      gHandoffReclaims++;
      waitAwake();   // a system sleep while the controller was lent: the takeover waits for the wake
      if (!glueClocked()) {
        note("hand-off: reclaim %u: the core's power domain or clocks read off: no takeover (idle until asked again)", gHandoffReclaims);
        handoffSet(kHoFailed);
        continue;
      }
      note("hand-off: reclaim %u: the core is back from PianoUsbHost: the takeover again", gHandoffReclaims);
      gCoreClocked = true;
      gWatch.disarm();   // the controller has two users now: no cold start behind PianoUsbHost's back
      if (takeover(false)) {
        gRunning = true;
        handoffSet(kHoDevice);
        return;
      }
      note("hand-off: the takeover after the reclaim failed: idle until asked again");
      handoffSet(kHoFailed);
      continue;
    }
    gWake.wait(seen, 60000);   // a kick (the ask) ends it
  }
}

// A takeover that failed: more cold starts (piano-usb-cold=1|auto), kRetryMs apart, while the boot's budget (kColdTries) lasts. True
// once one went through.
bool takeoverRetry(bool coldStart)
{
  bool ok = takeover(coldStart);
  while (!ok && gArgs.cold != cold::Mode::inherited && gWatch.colds < cold::kColdTries) {
    note("takeover failed: a cold start in %u ms (cold start %u of at most %u this boot)", cold::kRetryMs, gWatch.colds + 1, cold::kColdTries);
    IOSleep(cold::kRetryMs);
    waitAwake();
    ok = takeover(true);
  }
  return ok;
}

// The link watch's or the monitor's cold start, on the USB thread between passes (awake, not lent): the device controller stopped (a
// host sees a detach), the test hook if asked, the state read and reported, the cold start and the takeover. False: no takeover went
// through (the caller stops touching the core).
bool coldRestart(uint32_t sim, const char *why)
{
  note("cold restart (%s): the device controller stopped, then%s the cold start and the takeover", why, sim ? " the test hook," : "");
  if (gIrqMode) irqOff("a cold restart: polled from now on");
  if (glueClocked()) {
    gGadget.halt(100);
    gHal.wr(dwc3::GEVNTSIZ, dwc3::GEVNTSIZ_INTMASK | Layout::kEventBytes);
    gHal.barrier();
  }
  if (gLines[kPortConsole] & 1) consoleClosed();   // the next session resumes behind what had not reached the Mac
  IOLockLock(gLock);
  for (uint16_t &line : gLines) line = 0;   // every port closed: the Mac opens them again after it enumerates
  IOLockUnlock(gLock);
  (void)coldDecide(sim);
  const bool ok = takeoverRetry(true);
  gWatch.restarted(nowNs() / 1000000);
  return ok;
}

// The monitor's ask, once its reply had time to go out (0.5 s). True with the mask when it is due.
bool coldAskDue(uint64_t nowMs, uint32_t *sim)
{
  const uint32_t ask = __atomic_load_n(&gColdAsk, __ATOMIC_ACQUIRE);
  if (!(ask & 0x80000000u) || nowMs < gColdAskAtMs) return false;
  __atomic_store_n(&gColdAsk, 0u, __ATOMIC_RELEASE);
  *sim = ask & cold::kSimAll;
  return true;
}

void usbThread(void *, wait_result_t)
{
  if (gArgs.handoff) handoffSet(kHoStarting);
  if (gArgs.delay) {
    note("waiting %u s (piano-usb-delay)", gArgs.delay);
    for (uint32_t s = 0; s < gArgs.delay; ++s) {   // the log cursor follows the writer meanwhile (a step < cap - 64 KiB a sync)
      IOSleep(1000);
      logSync();
    }
  }
  waitAwake();
  // The inherited state read (after the test hook, piano-usb-cold-sim) and the decision (piano-usb-cold), then the takeover.
  if (!takeoverRetry(coldDecide(gArgs.coldSim))) {
    if (!gArgs.handoff) {
      note("takeover failed; the controller is left as it is. Rebuild the RAM image without piano-usb to drop this driver.");
      return;
    }
    note("takeover failed; the controller is left as it is; parked for a hand-off (piano-usb-handoff=1)");
    handoffSet(kHoFailed);
    handoffPark();
  }
  if (gArgs.handoff) handoffSet(kHoDevice);
  gRunning = true;
  if (gArgs.cold == cold::Mode::inherited || !gArgs.coldWait) {
    gHal.note("cold: no link watch (%s)", gArgs.cold == cold::Mode::inherited ? "piano-usb-cold=0" : "piano-usb-cold-wait=0");
  } else {
    gWatch.arm(gArgs.coldWait * 1000, nowNs() / 1000000);
    gHal.note("cold: link watch: no bus reset from a host within %u s: a cold start (the wait doubles; at most %u cold starts this boot, %u "
              "so far)", gArgs.coldWait, cold::kColdTries, gWatch.colds);
  }
  gIrqMode = gArgs.irq && attachCoreInterrupt();
  if (!gArgs.idle) gHal.note("wait: 1 ms after every quiet pass (piano-usb-idle=0, the old loop)");
  else if (!gIrqMode)
    gHal.note("wait: polled, adaptive (busy %u ms, port open %u ms, configured %u ms, no host %u ms)%s", kPolledPeriods.busy, kPolledPeriods.open,
              kPolledPeriods.attached, kPolledPeriods.idle, gArgs.irq ? "" : "; piano-usb-irq=1 for the core's interrupt");
  uint64_t lastReport = nowNs();
  const uint64_t begin = lastReport / 1000000;
  uint64_t lastActivity = begin, lastEvent = begin;
  pianoirq::WakeStats wakes = pianoirq::wakeStats(pianoirq::wakeLogMs());
  uint32_t passes[4] = {};
  bool kicked = false;
  for (;;) {
    const uint32_t seen = gWake.now();   // before the pass: a kick during it ends the wait at once
    if (!sleepStep()) {   // a system sleep without a host kept, or the glue unclocked at the wake: the core is not touched
      gWake.wait(seen, 1000);
      continue;
    }
    // The monitor's `usb cold` and the link watch (piano-usb-cold=1|auto): a cold start, only while awake. A host seen once ends the
    // watch for the boot; a cold start that does not go through leaves the core alone from then on (parked, as a failed takeover).
    if (gSleep.state() == SleepState::awake) {
      const uint64_t ms = nowNs() / 1000000;
      uint32_t sim = 0;
      const char *why = nullptr;
      if (coldAskDue(ms, &sim)) {
        why = "asked on the console";
      } else if (gWatch.armed) {
        const cold::WatchDo d = gWatch.step(ms, gGadget.stats.resets != 0 || gGadget.stats.connects != 0);
        if (d == cold::WatchDo::coldStart) why = "no bus reset from a host since the last run (the link watch)";
        if (d == cold::WatchDo::giveUp)
          gHal.note("cold: link watch: still no host after %u cold starts: given up for this boot (DSTS %08x)", gWatch.colds, gHal.rd(dwc3::DSTS));
        if (!gWatch.armed && gWatch.hostSeen && d == cold::WatchDo::nothing)
          gHal.note("cold: link watch: a host reset the device: the watch ends (%u cold starts this boot)", gWatch.colds);
      }
      if (why) {
        const bool ok = coldRestart(sim, why);
        lastReport = nowNs();
        lastActivity = lastEvent = lastReport / 1000000;
        if (!ok) {
          gRunning = false;
          if (!gArgs.handoff) {
            note("cold restart: no takeover went through; the controller is left as it is for the rest of the boot");
            return;
          }
          note("cold restart: no takeover went through; parked for a hand-off (piano-usb-handoff=1)");
          handoffSet(kHoFailed);
          handoffPark();
        }
        continue;
      }
    }
    // The hand-off (piano-usb-handoff=1): a release only while awake (not in a sleep's kept state); an ask it cannot act on is dropped.
    if (gArgs.handoff && __atomic_load_n(&gHandoffAsk, __ATOMIC_ACQUIRE) != kAskState) {
      const uint32_t ask = handoffTake();
      if (gSleep.state() == SleepState::awake && handoffDecide(__atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE), ask) == HandoffDo::release) {
        handoffRelease();
        handoffPark();
        gIrqMode = false;   // after a hand-off the thread polls (the line may be Apple's xHCI's by then)
        lastReport = nowNs();
        lastActivity = lastEvent = lastReport / 1000000;
        continue;
      }
    }
    const bool kept = gSleep.state() == SleepState::kept;   // until the freeze: polled, the interrupt left masked
    const unsigned events = gGadget.poll();
    const bool moved = logPump();
    gGadget.pump();
    gGadget.tick();
    const uint64_t ns = nowNs();
    if (ns - lastReport > 1000000000ull) {   // DSTS once a second while it changes (link state before the first reset)
      lastReport = ns;
      const uint32_t dsts = gHal.rd(dwc3::DSTS);
      // Ignore the frame number and COREIDLE: COREIDLE follows the console's own traffic, so it made a line a second, and with it
      // more traffic, for as long as the console streamed (desk1: 5000 of these lines in 20 minutes).
      constexpr uint32_t kNoise = 0x3fff8u | dwc3::DSTS_COREIDLE;
      if ((dsts & ~kNoise) != (gLastDsts & ~kNoise))
        gHal.note("DSTS %08x (link state %u, connected speed %u), %u events so far", dsts, dwc3::dstsLinkState(dsts), dsts & 7u,
             gGadget.stats.events);
      gLastDsts = dsts;
    }
    if (!gArgs.idle) {   // the old loop, for the A/B
      if (!events) IOSleep(1);
      continue;
    }
    const uint64_t now = ns / 1000000;
    if (events) lastEvent = now;
    if (events || moved || kicked) lastActivity = now;
    kicked = false;
    if (events) continue;   // as before: more may have come meanwhile
    if (gIrqMode && !gIrqLine.live()) irqOff("storm: the line stayed up with nothing to mask; disabled at the GIC");
    UsbActivity a{};
    a.configured = gGadget.configured();
    a.netUp = gGadget.netLinkUp();
    a.ep0Active = gGadget.ep0Active();
    for (unsigned p = 0; p < gGadget.ports(); ++p)
      if (gLines[p] & 1) {
        a.portOpen = true;
        if (gTx[p].used() && !gGadget.inBusy(p)) a.txPending = true;
      }
    a.linkState = dwc3::dstsLinkState(gHal.rd(dwc3::DSTS));
    a.sinceActivityMs = now - lastActivity;
    a.sinceEventMs = now - lastEvent;
    gTier = usbTier(a);
    ++passes[unsigned(gTier)];
    const uint32_t period = periodMs(gTier, gIrqMode && !kept ? kIrqPeriods : kPolledPeriods);
    if (gIrqMode && !kept) {   // unmask: events already pending raise the line at once
      gHal.wr(dwc3::GEVNTSIZ, Layout::kEventBytes);
      gHal.barrier();
    }
    const uint32_t taken = __atomic_load_n(&gIrqTaken, __ATOMIC_ACQUIRE);
    const bool woke = gWake.wait(seen, period);
    const bool byIrq = __atomic_load_n(&gIrqTaken, __ATOMIC_ACQUIRE) != taken;
    kicked = woke && !byIrq;
    wakes.count(byIrq ? pianoirq::Woke::irq : woke ? pianoirq::Woke::kick : pianoirq::Woke::timeout);
    if (gIrqMode) {
      if (byIrq) {
        gIrqMiss.delivered();
      } else if (!woke && (gHal.rd(dwc3::GEVNTCOUNT) & 0xffffu) && !(gHal.rd(dwc3::GEVNTSIZ) & dwc3::GEVNTSIZ_INTMASK) &&
                 gIrqMiss.miss()) {
        irqOff("events pending with the line unmasked after 3 waits in a row: no interrupt arrives");
      }
    }
    pianoirq::WakeStats::Window w;
    if (wakes.due(nowNs() / 1000000, &w)) {
      gHal.note("idle: %u wakeups in %u s, irq %u (kick %u, timer %u; passes busy %u open %u configured %u no-host %u; %s; link state %u, "
                "configuration %u; SPI 133 count %llu)", w.wakeups, w.seconds, w.irqs, w.kicks, w.timeouts, passes[0], passes[1], passes[2],
                passes[3], gIrqMode ? "interrupt" : "polled", a.linkState, gGadget.stats.configuration, (unsigned long long)gIrqLine.count());
      for (uint32_t &n : passes) n = 0;
    }
  }
}

}  // namespace

// UsbNet.h: PianoUsbNet.cpp's queues wake the USB thread.
void pianonet::usbKick() { gWake.kick(); }

// ---------------------------------------------------------------------------------------------- HID input (piano-usb-hid=1)
// A keyboard (+ consumer keys) and a relative pointer for macOS, fed by the Mac's commands on port 2 (HidInput.h). The class is the
// pattern of PianoInput's PianoHIDDevice and PianoKeyboard's PianoKeyboardHID (the IOHIDDevice subclass proven on this kernel
// collection, vtable checked at link time; UPSTREAM); the report descriptors are PianoKeyboard's (HidDescriptors.h: kind 0 the
// keyboard, 1 the pointer), so the reports are the cover's. Own product ID and names; nothing typed is ever logged, only counts.
namespace {
constexpr uint32_t kHidProductId = 0x5055;   // 'PU'; vendor 0x1AF4 as PianoInput/PianoKeyboard (tooling/k27/kext/PianoDwc3.json)
const char *const kHidProducts[2] = {"Piano USB Keyboard", "Piano USB Pointer"};
inline const pianokeyboard::DeviceInfo &hidShape(uint8_t kind) { return pianokeyboard::deviceInfo(pianokeyboard::Kind(kind)); }
}  // namespace

class PianoUsbHID : public IOHIDDevice {
  OSDeclareDefaultStructors(PianoUsbHID)

public:
  uint8_t fKind = 0;

  IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override
  {
    *descriptor = IOBufferMemoryDescriptor::withBytes(hidShape(fKind).descriptor, hidShape(fKind).descriptorBytes, kIODirectionNone);
    return *descriptor ? kIOReturnSuccess : kIOReturnNoMemory;
  }
  OSString *newTransportString() const override { return OSString::withCString("USB"); }
  OSString *newManufacturerString() const override { return OSString::withCString("UniXNU"); }
  OSString *newProductString() const override { return OSString::withCString(kHidProducts[fKind]); }
  OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(pianokeyboard::kVendorId, 32); }
  OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(kHidProductId, 32); }
  OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x50550000u | fKind, 32); }
  OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(hidShape(fKind).usagePage, 32); }
  OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(hidShape(fKind).usage, 32); }
  // The keyboard's LED report (Caps Lock and the rest): taken and dropped, so macOS sees no error.
  IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType reportType, IOOptionBits options) override
  {
    return report && reportType == kIOHIDReportTypeOutput && fKind == 0 ? kIOReturnSuccess : kIOReturnUnsupported;
  }
};

OSDefineMetaClassAndStructors(PianoUsbHID, IOHIDDevice)

namespace {
IONotifier *gHidPublished;

bool hidSend(uint8_t kind, const uint8_t *bytes, unsigned length)
{
  PianoUsbHID *hid = gHidDevice[kind];
  if (!hid) return false;
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, length, kIODirectionNone);
  const IOReturn rc = report ? hid->handleReport(report) : kIOReturnNoMemory;
  OSSafeReleaseNULL(report);
  if (rc != kIOReturnSuccess && ++gHid.sendErrors <= 5) {
    char line[96];
    snprintf(line, sizeof line, "hid: %s handleReport %#x", kHidProducts[kind], rc);
    noteLater(line);
  }
  return rc == kIOReturnSuccess;
}

void hidReleaseAll()
{
  const uint8_t keyboard[kKeyboardReportBytes] = {kReportKeyboard}, consumer[kConsumerReportBytes] = {kReportConsumer};
  const uint8_t pointer[kPointerReportBytes] = {};
  hidSend(0, keyboard, sizeof keyboard);
  hidSend(0, consumer, sizeof consumer);
  hidSend(1, pointer, sizeof pointer);
}

// One reply line to the Mac (port 2), dropped while the Mac does not have the port open. Never carries what was sent.
void hidReply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hidReply(const char *fmt, ...)
{
  char line[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > int(sizeof line) - 2) n = int(sizeof line) - 2;
  line[n++] = '\n';
  IOLockLock(gLock);
  if (gLines[kPortInput] & 1) gTx[kPortInput].put(reinterpret_cast<const uint8_t *>(line), uint32_t(n));
  IOLockUnlock(gLock);
  usbKick();
}

void hidCounts(const char *why)
{
  char line[200];
  snprintf(line, sizeof line, "hid: %s: %u commands (keyboard %u, consumer %u, pointer %u), %u releases, %u auto-releases, %u bad lines, "
           "%u send errors", why, gHid.commands, gHid.keyboard, gHid.consumer, gHid.pointer, gHid.releases, gHid.autoReleases, gHid.errors,
           gHid.sendErrors);
  noteLater(line);
}

// The commands, off the USB thread: what handleReport costs never delays EP0 or the console.
void hidThread(void *, wait_result_t)
{
  LineAssembler lines;
  pianobacklight::KeyWatch brightness;
  bool keys = false, consumer = false, buttons = false;
  uint64_t last = nowNs(), reported = 0;
  uint32_t reportedCommands = 0;
  for (;;) {
    uint8_t buf[256];
    IOLockLock(gLock);
    if (!gRx2.used() && !gHidReleaseWanted) {
      // 100 ms only while something is held (the auto-release); commands and a closed port wake the thread themselves, so
      // otherwise it sleeps (it used to wake 10 times a second for nothing; docs/power-irq.md).
      const uint32_t waitMs = keys || consumer || buttons ? 100 : gHid.commands != reportedCommands ? 1000 : 60000;
      uint64_t deadline = 0;
      clock_interval_to_deadline(waitMs, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gLock, &gRx2, deadline, THREAD_UNINT);
    }
    const uint32_t n = gRx2.get(buf, sizeof buf);
    const bool release = gHidReleaseWanted;
    gHidReleaseWanted = false;
    IOLockUnlock(gLock);
    if (n) usbKick();   // room for the input port's next bulk OUT transfer
    if (release) {
      if (keys || consumer || buttons) { hidReleaseAll(); ++gHid.releases; }
      keys = consumer = buttons = false;
      lines = LineAssembler();
      hidCounts("port closed");
    }
    for (uint32_t i = 0; i < n; ++i) {
      const int r = lines.push(char(buf[i]));
      if (r < 0) { ++gHid.errors; hidReply("err long"); }
      if (r <= 0) continue;
      const HidCommand c = parseHidCommand(lines.line());
      last = nowNs();
      bool sent = true;
      switch (c.op) {
      case HidOp::keyboard: sent = hidSend(0, c.report, c.bytes); keys = c.keysDown; ++gHid.keyboard; break;
      case HidOp::consumer: sent = hidSend(0, c.report, c.bytes); consumer = c.keysDown; ++gHid.consumer; break;
      case HidOp::pointer: sent = hidSend(1, c.report, c.bytes); buttons = c.keysDown; ++gHid.pointer; break;
      case HidOp::release: hidReleaseAll(); keys = consumer = buttons = false; ++gHid.releases; break;
      case HidOp::status:
        hidReply("status keyboard %u pointer %u commands %u errors %u", gHidDevice[0] != nullptr, gHidDevice[1] != nullptr, gHid.commands,
                 gHid.errors + gHid.sendErrors);
        continue;
      default:
        ++gHid.errors;
        hidReply("err %s", c.error ? c.error : "?");
        continue;
      }
      // piano-backlight-keys=1: the brightness usages and Right Ctrl + 1/2 also step the panel backlight (PianoBacklightClient.h);
      // the reports went to macOS as always.
      if (pianobacklight::keysEnabled()) {
        if (c.op == HidOp::release) brightness = pianobacklight::KeyWatch();
        pianobacklight::step(c.op == HidOp::consumer ? brightness.consumer(uint16_t(c.report[1] | c.report[2] << 8))
                             : c.op == HidOp::keyboard ? brightness.keyboard(c.report) : 0, "the Mac's keyboard (PianoDwc3 HID)");
      }
      ++gHid.commands;
      if (sent) hidReply("ok %u", gHid.commands);
      else hidReply("err hid");
    }
    const uint64_t now = nowNs();
    if ((keys || consumer || buttons) && now - last > uint64_t(gArgs.hidHold) * 1000000ull) {
      hidReleaseAll();
      keys = consumer = buttons = false;
      ++gHid.autoReleases;
      hidCounts("held past piano-usb-hid-hold, released");
    }
    if (gHid.commands != reportedCommands && now - reported > 10000000000ull) {   // counts only, at most every 10 s
      reported = now;
      reportedCommands = gHid.commands;
      hidCounts("activity");
    }
  }
}

bool hidEventServicePublished(void *, void *, IOService *service, IONotifier *)
{
  for (IOService *provider = service->getProvider(); provider; provider = provider->getProvider())
    if (PianoUsbHID *hid = OSDynamicCast(PianoUsbHID, provider)) {
      note("hid: event service %s published on %s: macOS has it", service->getMetaClass()->getClassName(), kHidProducts[hid->fKind]);
      break;
    }
  return true;
}

// Both devices under the PianoDwc3 service, as PianoInput and PianoKeyboard attach and start theirs.
bool publishHid(IOService *owner)
{
  for (uint8_t k = 0; k < 2; ++k) {
    PianoUsbHID *hid = OSTypeAlloc(PianoUsbHID);
    if (hid) hid->fKind = k;
    if (!hid || !hid->init(nullptr)) {
      OSSafeReleaseNULL(hid);
      note("hid: %s: init failed", kHidProducts[k]);
      return false;
    }
    hid->setProperty(kIOHIDBuiltInKey, true);
    if (!hid->attach(owner)) {
      OSSafeReleaseNULL(hid);
      note("hid: %s: attach failed", kHidProducts[k]);
      return false;
    }
    if (!hid->start(owner)) {
      hid->detach(owner);
      OSSafeReleaseNULL(hid);
      note("hid: %s: IOHIDDevice::start failed", kHidProducts[k]);
      return false;
    }
    gHidDevice[k] = hid;
    note("hid: %s: vendor %#x product %#x, usage %#x/%#x, %u-byte descriptor (PianoKeyboard's), registry ID %#llx", kHidProducts[k],
         pianokeyboard::kVendorId, kHidProductId, hidShape(k).usagePage, hidShape(k).usage, hidShape(k).descriptorBytes,
         hid->getRegistryEntryID());
  }
  if (OSDictionary *matching = IOService::serviceMatching("IOHIDEventService")) {
    gHidPublished = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, hidEventServicePublished, nullptr, nullptr);
    matching->release();
  }
  return true;
}
}  // namespace

// ------------------------------------------------------------------------------------------------------------- the service
class PianoDwc3 : public IOService {
  OSDeclareDefaultStructors(PianoDwc3)
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
};

OSDefineMetaClassAndStructors(PianoDwc3, IOService)

// "piano-usb-handoff" (UsbHandoff.h): param1 the ask, param2 uint32_t * for the state after, param3 the wait in ms (at most 5000).
// Unsupported without piano-usb-handoff=1 (the controller stays PianoDwc3's).
IOReturn PianoDwc3::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                         void *param4)
{
  if (!functionName || !functionName->isEqualTo(kHandoffFunction))
    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
  uint32_t *state = static_cast<uint32_t *>(param2);
  if (!gArgs.handoff || gArgs.level < 2) {
    if (state) *state = kHoOff;
    return kIOReturnUnsupported;
  }
  const uint32_t ask = uint32_t(uintptr_t(param1));
  uint32_t waitMs = uint32_t(uintptr_t(param3));
  if (waitMs > 5000) waitMs = 5000;
  // Only an ask the thread can act on now is left for it: a release asked while the takeover still runs, or during a system sleep,
  // is not kept for later (the caller may have changed its mind by then, and nobody would reclaim). "state" withdraws a pending ask.
  if (ask == kAskState) __atomic_store_n(&gHandoffAsk, uint32_t(kAskState), __ATOMIC_RELEASE);
  const bool awake = gSleep.state() == SleepState::awake;
  if ((ask != kAskRelease || awake) && handoffDecide(__atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE), ask) != HandoffDo::nothing) {
    __atomic_store_n(&gHandoffAsk, ask, __ATOMIC_RELEASE);
    gWake.kick();
  } else {
    if (state) *state = __atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE);
    return kIOReturnSuccess;
  }
  const uint64_t until = nowNs() + uint64_t(waitMs) * 1000000ull;
  for (;;) {
    const uint32_t s = __atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE);
    if (handoffSettled(ask, s) || nowNs() >= until) break;
    assert_wait_timeout((event_t)&gHandoffState, THREAD_UNINT, 10, kMillisecondScale);
    if (__atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE) != s) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
  }
  if (state) *state = __atomic_load_n(&gHandoffState, __ATOMIC_ACQUIRE);
  return kIOReturnSuccess;
}

IOService *PianoDwc3::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoDwc3::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-usb=%u phy=%u smmu=%u utmi=%u delay=%u shell=%u hid=%u net=%u irq=%u idle=%u (DWC3 %#llx, SMMU stream %#x, %s)",
       gArgs.level, gArgs.phy, gArgs.smmu, gArgs.utmi, gArgs.delay, gArgs.shell, gArgs.hid, gArgs.net, gArgs.irq, gArgs.idle,
       pianohw::kDwc3, pianohw::kUsbStream, gArgs.irq ? "interrupt, else polled" : gArgs.idle ? "polled, adaptive" : "polled every 1 ms");
  note("start: piano-usb-cold=%s%s steps=%#x sim=%#x wait=%u s (the cold start, UsbCold.h: %s)", cold::modeName(gArgs.cold),
       gArgs.coldKnown ? "" : " (an unknown value: auto)", gArgs.coldSteps, gArgs.coldSim, gArgs.coldWait,
       gArgs.cold == cold::Mode::inherited ? "never; the inherited state only, as before" : gArgs.cold == cold::Mode::always ? "always" :
       "when the inherited state is not a running device controller, or no host comes");
  if (gArgs.coldSim && gArgs.level < 2) note("piano-usb-cold-sim needs piano-usb=2: no test hook");
  if (gArgs.hid && gArgs.level < 2) note("piano-usb-hid needs piano-usb=2: no HID input");
  if (gArgs.net && gArgs.level < 2) note("piano-usb-net needs piano-usb=2: no network function");
  setProperty("piano-usb", gArgs.level, 32);
  gLock = IOLockAlloc();
  gSleepLock = IOLockAlloc();   // the sleep guard: QSCRATCH writes (the takeover, a monitor poke) and setPowerState
  gSleep = UsbSleep(gArgs.sleepUsb != 0);
  if (!gLock || !gSleepLock) return false;
  stage(1, "probe: GCC and TCSR (read only)");
  if (!probeClocks()) { stage(-1, "probe: GCC map"); return true; }
  if (gCoreClocked) {
    stage(2, "probe: DWC3 core and QSCRATCH (read only)");
    probeCore();
    if (gArgs.phyread) {
      IOSleep(20);
      stage(3, "probe: M31 PHY (read only)");
      probePhy();
    }
  }
  if (gArgs.spmi) {
    IOSleep(20);
    stage(4, "probe: eUSB2 repeater over SPMI (read only)");
    probeRepeater();
  }
  logInit();
  if (gArgs.level < 2) {
    if (gArgs.smmu) {
      IOSleep(20);
      stage(5, "probe: SMMU (read only)");
      probeSmmu();
    }
    // What the cold start would find (read only; at level 2 the USB thread reads it again before its decision).
    if (coldMap()) {
      ColdHal h;
      cold::report(h, cold::inspect(h, gArgs.spmi != 0), gArgs.cold, gArgs.spmi != 0);
    }
    stage(9, "probe done (piano-usb=1: nothing written)");
    setProperty("piano-usb-stage", uint64_t(uint32_t(piano_dwc3_stage)), 32);
    return true;
  }
  // The SMMU write happens here, on the matching thread right after NativeUfs's (piano's other SMMU user), not later from
  // another thread (ShengUfs's note: the stream-match registers were not safe to touch later).
  if (gArgs.smmu) {
    IOSleep(20);
    stage(6, "SMMU: stream 0x40 bypass");
    if (!smmuSetup()) { stage(-6, "SMMU: stream 0x40 bypass (piano-usb-smmu=0 skips it)"); return true; }
  }
  if (!ringAlloc(gTx[0], 256 * 1024) || !ringAlloc(gTx[1], 64 * 1024) || !ringAlloc(gRx1, 64 * 1024)) { stage(-7, "ring buffers"); return true; }
  thread_t thread = nullptr;
  if (ringAlloc(gNotes, 64 * 1024) && kernel_thread_start(noteThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  else gNotes.buf = nullptr;   // notes stay synchronous
  if (kernel_thread_start(monitorThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  if (gArgs.shell && kernel_thread_start(shellThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  // piano-usb-hid=1: the keyboard and the pointer (as PianoInput publishes its device: from start), their command thread, and so the
  // third port. Without the rings or the thread the device keeps its two ports.
  if (gArgs.hid) {
    stage(7, "HID input: keyboard and pointer (piano-usb-hid=1)");
    if (!ringAlloc(gTx[kPortInput], 4096) || !ringAlloc(gRx2, 16 * 1024) ||
        kernel_thread_start(hidThread, nullptr, &thread) != KERN_SUCCESS) {
      note("hid: no rings or thread: the input port is left out");
      gArgs.hid = 0;
    } else {
      thread_deallocate(thread);
      if (!publishHid(this)) note("hid: the devices are not there; the input port answers \"err hid\"");
      registerService();
    }
  }
  // piano-usb-net=1: the queues now (the USB thread uses them from its first enumeration), the Ethernet controller from a thread
  // once IOBSD is up (PianoUsbNet.cpp), under this service, registered as for the HID devices. Without them the device keeps its
  // ACM-only shape.
  if (gArgs.net) {
    if (!pianonet::init(&gGadget.netStats) || !pianonet::startThread(this)) {
      note("net: no queues or thread: the network function is left out");
      gArgs.net = 0;
    } else {
      if (!gArgs.hid) registerService();   // the HID path registered it already
      note("net: CDC-NCM network function requested (piano-usb-net=1): MAC %02x:%02x:%02x:%02x:%02x:%02x here, %02x:%02x:%02x:%02x:%02x:%02x "
           "on the Mac", kNetDeviceMac[0], kNetDeviceMac[1], kNetDeviceMac[2], kNetDeviceMac[3], kNetDeviceMac[4], kNetDeviceMac[5],
           kNetHostMac[0], kNetHostMac[1], kNetHostMac[2], kNetHostMac[3], kNetHostMac[4], kNetHostMac[5]);
    }
  }
  // piano-usb-handoff=1: registered so PianoUsbHost finds this service by class for its platform function (UsbHandoff.h).
  if (gArgs.handoff) {
    note("hand-off: piano-usb-handoff=1: PianoUsbHost may borrow the controller for host mode (released and reclaimed on its ask)");
    if (!gArgs.hid && !gArgs.net) registerService();   // the HID and network paths registered it already
  }
  if (kernel_thread_start(usbThread, nullptr, &thread) != KERN_SUCCESS) { stage(-8, "USB thread"); return true; }
  thread_deallocate(thread);
  note("system sleep: in the power tree (%#x): %s", pianopower::join(this),
       gArgs.sleepUsb ? "a host that has the device configured keeps it across a suspend (the console survives), else a soft "
                        "disconnect; QSCRATCH written only awake with the glue clocked"
                      : "a soft disconnect at every suspend (piano-sleep-usb=0); QSCRATCH written only awake with the glue clocked");
  return true;
}

// docs/power.md 4. off: the USB thread keeps the gadget (a configured host, piano-sleep-usb=1) or disconnects it, and this waits
// for it (at most 500 ms); before the takeover nothing is to do (it waits for the wake). on: asynchronous, the thread reads the
// glue's clocks first and touches nothing when they are off.
IOReturn PianoDwc3::setPowerState(unsigned long state, IOService *)
{
  if (!gSleepLock) return pianopower::kAckNow;
  IOLockLock(gSleepLock);
  if (state == pianopower::kOff) {
    if (!gRunning) {
      gSleep.off(false, false, false);
      IOLockUnlock(gSleepLock);
      note("sleep: before the takeover: it runs after the wake");
      return pianopower::kAckNow;
    }
    gSleepAsk = kAskOff;
    const uint32_t seq = ++gSleepSeq;
    IOLockUnlock(gSleepLock);
    usbKick();
    IOLockLock(gSleepLock);
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    while (gSleepDone != seq && IOLockSleepDeadline(gSleepLock, &gSleepDone, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    const bool done = gSleepDone == seq;
    IOLockUnlock(gSleepLock);
    if (!done) note("sleep: the USB thread did not answer within 500 ms (it acts at its next pass)");
    return pianopower::kAckNow;
  }
  if (gSleep.state() == SleepState::awake) {   // the power tree's first on
    IOLockUnlock(gSleepLock);
    return pianopower::kAckNow;
  }
  if (!gRunning) {   // asleep before the takeover: it may run now
    gSleep.on(true);
    IOLockWakeup(gSleepLock, &gSleep, false);
    IOLockUnlock(gSleepLock);
    return pianopower::kAckNow;
  }
  gSleepAsk = kAskOn;
  ++gSleepSeq;
  IOLockUnlock(gSleepLock);
  usbKick();
  return pianopower::kAckNow;
}
