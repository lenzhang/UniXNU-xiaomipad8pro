// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch: the Xiaomi Pad 8 Pro touch screen (Novatek NT36532E, a TDDI with host-processed "THP" touch) for macOS 27 on piano
// (SM8750), native at EL1. The controller sits on QUP1 serial engine 2 (spi@a88000, chip select 0, GPIO40-43), raises GPIO162 for
// every frame, and is powered with the panel (no rail of its own: nothing is voted). It has no flash for its touch firmware: the host
// downloads the panel's image (BOE or CSOT by GPIO100, staged by tooling/k27/build-native-piano-touch.sh from an ignored out/ copy,
// never committed) into its SRAM at every start; it then streams raw 40 x 60 matrices that TouchMatrix.h turns into contacts,
// TouchTrack.h follows them (stable IDs, palms rejected) and TouchGesture.h turns them into what macOS understands from a HID pointer
// (tap = click, hold = right click, drag, two-finger scroll, two-finger tap = right click; TouchHid.h's absolute pointer with a wheel).
//
// Steps (each logs "PianoTouch: step N ..." before it touches anything; "FAILED at step N" when it stops there):
//   1 survey     read only: the six pins, the GCC votes/branches/rate of SE2, SE2's registers when its clocks run, the SMMU entry
//                of QUP1's stream 0xa3, the built-in firmware images
//   2 smmu       (start(), the matching thread, as PianoDwc3 and NativeUfs do theirs) stream 0xa3 through a bypass context bank,
//                for SE2's DMA engine (only when the GENI bus may be used)
//   3 bus        GENI: the SE2 clock votes (0x52008 under PianoA830's lock word), the rate, the engine (SE DMA when its FIFO is
//                disabled, FIFO otherwise), polled, mode 0; a loopback self-test; the pins to qup1_se2. Or SPI by hand on the same
//                pins (piano-touch-bus=2, or automatically when the GENI bus fails at this step or the next)
//   4 identify   engine reset, the trim-ID loop, the memory map (Nt36532.h)
//   5 firmware   GPIO100 picks the image; the host download with the hardware CRC check (three attempts)
//   6 start      payload length, firmware info, calibration wait, stylus scanning off, idle-baseline update, operating mode
//   7 frames     GPIO162's rising edges latched in TLMM (raw status, from step 5 on), and a frame read only when the controller
//                announced it (an edge, or the line still high; never by a timer, TouchPace.h): the latch polled every
//                piano-touch-poll ms while the panel is busy, once per piano-touch-idle ms at rest (or, with piano-touch-irq=1, the
//                TLMM summary interrupt, package/common/PianoIrq.h); an idling controller is not read at all; checksum,
//                transport kind, counter, type; the matrix decoder, the contacts (TouchTrack.h) and the gestures (TouchGesture.h);
//                at level 2 their reports to the HID pointer (TouchHid.h). While the panel is busy a CPU latency hold (k27 0061's
//                piano_cpuidle_latency_hold, compiled in when the kernel the build links has it). Eleven watchdog frames in a row:
//                the download again, at once and then every 10 s while it comes back, never given up (TouchPace.h)
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-touch=1            steps 1-7; frame statistics, contacts and the gestures they make in the log; nothing goes to macOS
//   piano-touch=2            the same and the HID pointer (docs/drivers/touch.md §14: tap = click, hold = right click, drag = drag,
//                            two fingers scroll, two-finger tap = right click); positions are then not logged (taps at the login
//                            window could spell a password on an on-screen keyboard); piano-touch-trace=1 logs them
//   piano-touch-gestures=0   level 2 as before the gestures: the first contact is the tip (down presses, up releases)
//   piano-touch-hold=MS      still this long: a right click (default 500; 0: never; 200..3000)
//   piano-touch-slop=PX      movement (panel pixels) that makes a drag, a scroll or a pinch (default 64; 4..200; 32 before
//                            2026-11-12, when attend1's short taps became drags)
//   piano-touch-drag-delay=MS past the slop this long before a drag starts; lifted before, the touch is a tap (default 80;
//                            0: at once, as before 2026-11-12; at most 500)
//   piano-touch-scroll=PX    finger travel per wheel count (default 4; 1..64)
//   piano-touch-scroll-res=N the scroll resolution the descriptor declares, counts per inch (default 45; 10..900)
//   piano-touch-scroll-invert=1  scroll against macOS's natural direction setting (both axes)
//   piano-touch-pinch=N      0 a pinch sends nothing (default); 1 Command + wheel; 2 Control + wheel (Accessibility Zoom); 1 and 2
//                            add the keyboard modifiers to the descriptor (report IDs)
//   piano-touch-palm=N       a blob of N nodes or more is a palm (default 48; 0: no palm rejection by size)
//   piano-touch-confirm=N    frames a new contact needs (default 2; 1..5); piano-touch-fast=P: a first peak of P counts at once
//                            (default twice the threshold; 0: never)
//   piano-touch-cpu-hold=US  the CPU latency hold while the panel is busy (default 2000; 0: none)
//   piano-touch-stop=N       stop before step N (2: the read-only survey alone; 4: the bus up and tested, the controller untouched)
//   piano-touch-bus=N        0 GENI, falling back to SPI by hand (default); 1 GENI only; 2 SPI by hand only
//   piano-touch-khz=K        the GENI SPI rate (default 9600; 1000..19200, the DT's maximum)
//   piano-touch-seirq=N      the engine's interrupt outputs while polled: 2 on (default: the 13T's SE DMA code and K90's driver on
//                            this SE keep them on, their status bits follow them; ArmCpu leaves every unused SPI disabled at the
//                            GIC); 1 off; 0 off, then on if the DMA loopback times out
//   piano-touch-smmu=0       leave the SMMU alone (SE DMA then needs stream 0xa3 to bypass already)
//   piano-touch-loopback=0   no loopback self-test of the engine
//   piano-touch-mode=V       the operating-mode command's value (default 2, the stock driver's idle; 65536: not sent)
//   piano-touch-baseline=0   no idle-baseline update command (default: sent with 1, as the stock driver and Ubuntu; 0 before
//                            2026-11-12)
//   piano-touch-stylus=1     leave stylus scanning on (default: off with host command 0x04 = 0 before the others, as Ubuntu)
//   piano-touch-threshold=T  the contact threshold of the decoder (default 200)
//   piano-touch-flip=B       auto (default: by the panel, BOE as processor Y, CSOT Y reversed; the calibration's panel wins over
//                            gpio100 for this, and its invert_y over both); 0..3 explicit: bit 0 X, bit 1 Y flipped
//   piano-touch-poll=MS      edge polling while busy (default 1)
//   piano-touch-idle=MS      at rest the latch looked at once per MS (default 50: an announced frame waits at most that long; 0:
//                            every frame at once, never at rest; 20..200)
//   piano-touch-irq=1        at rest the edge by the TLMM interrupt (gpio162 on the summary SPI 208) instead of a look per idle period
//   piano-touch-noedge=MS    A/B only: the reads without an announced frame of before 2026-11-12 (busy MS after the last read, at rest
//                            the idle period + 50 ms, the display off every 200 ms; 25..200). Default 0: never (TouchPace.h)
//   piano-wake-log=S         the "idle: N wakeups" line every S seconds (default 60, 0 none; shared with the other drivers)
//   piano-touch-trace=1      more frame headers, and contact positions at level 2
//   piano-touch-rotate=1     follow the display's rotation (package/PianoAuto/TouchRotation.h, docs/drivers/sensors.md 5a.3): the
//                            platform function "PianoTouchRotation" (param1 = degrees) and the property "piano-touch-rotation"
//                            (0/90/180/270; root or the console user: piano-rotated) are taken, and each contact is mapped into the
//                            rotated desktop from its touch-down on. Without it both are refused and the reports are as before
// Never: a flash write (the controller has none for the touch firmware), a panel command, a regulator vote, a GPIO other than
// 40-43, 100 and 162.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/hid/IOHIDDevice.h>
#ifndef kIOClientPrivilegeConsoleUser
#define kIOClientPrivilegeConsoleUser "console"   // xnu IOKitKeysPrivate.h: the caller's user has the session on the console
#endif
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "Nt36532.h"
#include "TouchMatrix.h"
#include "TouchTrack.h"
#include "TouchGesture.h"
#include "TouchHid.h"
#include "Se2Bus.h"
#include "PianoTouchCalibration.h"   // generated (tooling/k27/gen-touch-calibration.py) into the ignored out/ tree
#include "SmmuStream.h"
#include "TouchPace.h"
#include "TouchPower.h"
#include "PianoTouchClient.h"   // "PianoTouchContact": the last frame with a finger, for PianoKeyboard's tap veto
#include "../common/PianoIrq.h"
// The power contract (docs/power.md §4): the two power states and pianopower::join. PIANO_TOUCH_LATENCY_HOLD:
// tooling/k27/build-native-piano-touch.sh sets it to 1 when the kernel the KC links (K27_KERNEL) exports
// piano_cpuidle_latency_hold (k27 0061); the KC linker takes no weak imports, so without it the call is not compiled in.
#include <PianoPowerContract.h>
#ifndef PIANO_TOUCH_LATENCY_HOLD
#define PIANO_TOUCH_LATENCY_HOLD 0
#endif
extern "C" {
#include "GeniSpi.h"
}

using namespace pianotouch;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last step reached, negative when it failed there.
volatile int32_t piano_touch_stage = 0;
// The firmware images, from out/native-kexts/PianoTouch/gen/PianoTouchFirmware.c (size 0 when built without one).
extern const uint8_t piano_touch_firmware_boe[], piano_touch_firmware_csot[];
extern const uint64_t piano_touch_firmware_boe_size, piano_touch_firmware_csot_size;
extern const char piano_touch_firmware_boe_sha[], piano_touch_firmware_csot_sha[];
// PianoA830's lock word of the GCC branch vote registers (Se2Bus.h); PianoA830 links first and is a library of PianoTouch.json.
extern volatile uint32_t sm8750_gcc_vote_owner;
}

namespace {

struct Args {
  uint32_t level, stop, bus, khz, seirq, smmu, loopback, mode, baseline, stylus, threshold, flip, pollMs, trace, idleMs, irq, rotate;
  uint32_t gestures, holdMs, slopPx, dragDelayMs, scrollPx, scrollRes, scrollInvert, pinch, palmNodes, confirm, fastPeak, cpuHoldUs,
      noEdgeMs;
  uint32_t pm, display;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = bootArg("piano-touch", 0);
  gArgs.stop = bootArg("piano-touch-stop", 0);
  gArgs.bus = clampArg(bootArg("piano-touch-bus", 0), 0, 2);
  gArgs.khz = clampArg(bootArg("piano-touch-khz", 9600), 1000, 19200);
  gArgs.seirq = clampArg(bootArg("piano-touch-seirq", 2), 0, 2);
  gArgs.smmu = bootArg("piano-touch-smmu", 1);
  gArgs.loopback = bootArg("piano-touch-loopback", 1);
  gArgs.mode = bootArg("piano-touch-mode", 2);
  gArgs.baseline = bootArg("piano-touch-baseline", 1);
  gArgs.stylus = bootArg("piano-touch-stylus", 0);
  gArgs.threshold = clampArg(bootArg("piano-touch-threshold", 200), 20, 5000);
  // piano-touch-flip=auto (or absent, or anything but 0..3) is 4. A string value lands in the buffer as text, a number as an int64.
  char flip[16] = {};
  gArgs.flip = 4;
  if (PE_parse_boot_argn("piano-touch-flip", flip, sizeof flip - 1) && strncmp(flip, "auto", 4) != 0) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(uint8_t(flip[i])) << (8 * i);
    if (v <= 3) gArgs.flip = uint32_t(v);
  }
  gArgs.pollMs = clampArg(bootArg("piano-touch-poll", 1), 1, 50);
  gArgs.trace = bootArg("piano-touch-trace", 0);
  gArgs.idleMs = TouchPace::clampIdle(bootArg("piano-touch-idle", kIdleReadMs));
  gArgs.irq = bootArg("piano-touch-irq", 0);
  gArgs.noEdgeMs = TouchPace::clampBlind(bootArg("piano-touch-noedge", 0));
  gArgs.rotate = bootArg("piano-touch-rotate", 0);
  // The gestures (TouchGesture.h) and the contacts (TouchTrack.h); 0 turns off what has an "off".
  gArgs.gestures = bootArg("piano-touch-gestures", 1);
  const uint32_t hold = bootArg("piano-touch-hold", 500);
  gArgs.holdMs = hold ? clampArg(hold, 200, 3000) : 0;
  gArgs.slopPx = clampArg(bootArg("piano-touch-slop", 64), 4, 200);
  gArgs.dragDelayMs = clampArg(bootArg("piano-touch-drag-delay", 80), 0, 500);
  gArgs.scrollPx = clampArg(bootArg("piano-touch-scroll", 4), 1, 64);
  gArgs.scrollRes = touchScrollResolution(bootArg("piano-touch-scroll-res", kTouchScrollResolution));
  gArgs.scrollInvert = bootArg("piano-touch-scroll-invert", 0);
  gArgs.pinch = clampArg(bootArg("piano-touch-pinch", 0), 0, 2);
  const uint32_t palm = bootArg("piano-touch-palm", 48);
  gArgs.palmNodes = palm ? clampArg(palm, 8, 2400) : 0;
  gArgs.confirm = clampArg(bootArg("piano-touch-confirm", 2), 1, 5);
  gArgs.fastPeak = clampArg(bootArg("piano-touch-fast", 2 * gArgs.threshold), 0, 30000);
  const uint32_t cpuHold = bootArg("piano-touch-cpu-hold", 2000);
  gArgs.cpuHoldUs = cpuHold ? clampArg(cpuHold, 100, 10000) : 0;
  // The power contract (TouchPower.h): the power tree (system sleep, restart), and the display's state.
  gArgs.pm = bootArg("piano-touch-pm", 1);
  gArgs.display = bootArg("piano-touch-display", 1);
}

TrackConfig trackConfig()
{
  TrackConfig c;
  c.confirm = gArgs.confirm;
  c.fastPeak = int32_t(gArgs.fastPeak);
  c.palmNodes = gArgs.palmNodes;
  c.palmPeaks = gArgs.palmNodes ? 3 : 0;
  return c;
}
GestureConfig gestureConfig()
{
  GestureConfig c;
  c.gestures = gArgs.gestures != 0;
  c.holdMs = gArgs.holdMs;
  c.slopPx = int32_t(gArgs.slopPx);
  c.dragDelayMs = gArgs.dragDelayMs;
  c.scrollPx = int32_t(gArgs.scrollPx);
  c.scrollInvert = gArgs.scrollInvert != 0;
  c.pinch = uint8_t(gArgs.pinch);
  return c;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoTouch: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_touch_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at step" : "step", value < 0 ? -value : value, what);
}

const char *hex(const uint8_t *p, unsigned n, char *out, size_t size)
{
  size_t used = 0;
  out[0] = 0;
  for (unsigned i = 0; i < n && used + 3 < size; ++i) used += size_t(snprintf(out + used, size - used, "%s%02x", i ? " " : "", p[i]));
  return out;
}

// snprintf into line[used..], never past its end (used stays < size).
void append(char *line, size_t size, size_t &used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void append(char *line, size_t size, size_t &used, const char *fmt, ...)
{
  if (used + 1 >= size) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line + used, size - used, fmt, ap);
  va_end(ap);
  if (n > 0) used = used + size_t(n) < size ? used + size_t(n) : size - 1;
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}
uint64_t nowMs() { return nowUs() / 1000; }
const char *const kFlipNames[5] = {"0 (none)", "1 (X)", "2 (Y)", "3 (X and Y)", "auto"};
const char *const kPinchNames[3] = {"recognised, nothing sent", "Command + wheel", "Control + wheel"};

// Explicit ldr/str, as PianoDwc3 does: every store from a general register (Qualcomm's trapped SMMU registers need that).
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
bool mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}

// ------------------------------------------------------------------------------------------------------------- the pins (TLMM)
// TLMM (pinctrl@f100000): pin n at n * 0x1000: CTL (pull 1:0 (1 down, 3 up), function 5:2, drive 8:6, output enable 9, eGPIO
// present 11 / ours 12), IO (bit 0 in, bit 1 out), INTR_CFG (bit 0 enable, 1 polarity, 3:2 detection (1 edge), 4 raw status,
// 10:8 target), INTR_STATUS (bit 0, cleared with 0). As package/PianoKeyboard: an eGPIO-capable pin is claimed for this processor.
constexpr uint32_t kCtl = 0x0, kIo = 0x4, kIntrCfg = 0x8, kIntrStatus = 0xc;
constexpr uint32_t kPullDown = 1, kPullUp = 3, kOe = 1u << 9, kEgpioPresent = 1u << 11, kEgpioEnable = 1u << 12;
struct Pin {
  const char *name;
  Region page;
  uint32_t ctl, io, intrCfg;   // as found
};
Pin gPin[kPins] = {{"MISO", {}, 0, 0, 0}, {"MOSI", {}, 0, 0, 0}, {"CLK", {}, 0, 0, 0},
                   {"CS", {}, 0, 0, 0},   {"irq", {}, 0, 0, 0},  {"lcd-id", {}, 0, 0, 0}};
uintptr_t pinVa(unsigned p) { return gPin[p].page.va; }
uint32_t pinCfg(unsigned p, uint32_t function, uint32_t strength, uint32_t pull, bool output)
{
  const uint32_t found = gPin[p].ctl;
  return (found & ~(0x3ffu | kEgpioEnable)) | (found & kEgpioPresent ? kEgpioEnable : 0) | pull | function << 2 | strength << 6 |
         (output ? kOe : 0);
}
void setCtl(unsigned p, uint32_t v)
{
  wr32(pinVa(p) + kCtl, v);
  dsb();
}
bool level(unsigned p) { return rd32(pinVa(p) + kIo) & 1; }
void drive(unsigned p, bool high)
{
  wr32(pinVa(p) + kIo, high ? 2u : 0u);
  dsb();
}

// ------------------------------------------------------------------------------------------------------------------- the rest
Region gGcc, gSe, gQup, gSmmu;
uint32_t gcc(uint32_t off) { return rd32(gGcc.va + off); }
bool gSmmuReady = false;   // stream 0xa3 bypasses (step 2, or as found)

// CPU caches around the engine's DMA (the stream bypasses the SMMU, so the engine is not coherent): as the 13T's touch code.
void dmaCache(void *bytes, size_t size, unsigned op)
{
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = 4ull << ((ctr >> 16) & 15), end = uintptr_t(bytes) + size;
  for (uintptr_t at = uintptr_t(bytes) & ~(line - 1); at < end; at += line) {
    if (op == 0) __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    else if (op == 1) __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    else __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
  }
  dsb();
}
void dmaClean(void *, void *p, size_t n) { dmaCache(p, n, 0); }
void dmaCleanInval(void *, void *p, size_t n) { dmaCache(p, n, 1); }
void dmaInval(void *, void *p, size_t n) { dmaCache(p, n, 2); }

// The engine's register callbacks. The delay: with SE DMA the first polls of a transfer spin (1 us), later ones sleep (1 ms), so a
// frame read (2-5 ms) does not hold a CPU. In FIFO mode every poll spins (the FIFO is 64 bytes: the engine waits for each refill).
// poll_limit is set per transfer from its length and the rate (PianoTouch::pollLimit): twice the time on the wire, plus a margin.
constexpr unsigned kSpinPolls = 300;
unsigned gDelayCalls;
bool gDelaySleeps;
uint32_t seRd(void *ctx, uint32_t reg) { return rd32(static_cast<Region *>(ctx)->va + reg); }
void seWr(void *ctx, uint32_t reg, uint32_t v)
{
  wr32(static_cast<Region *>(ctx)->va + reg, v);
  dsb();
}
void seDelay(void *, unsigned us)
{
  if (!gDelaySleeps || ++gDelayCalls <= kSpinPolls) IODelay(us);
  else IOSleep(1);
}

uint32_t smmuRead(uint32_t off) { return rd32(gSmmu.va + off); }

}  // namespace

// --------------------------------------------------------------------------------------------------------------- the HID device
// The IOHIDDevice subclass of package/PianoInput's PianoHIDDevice and PianoKeyboard's PianoKeyboardHID (UPSTREAM) with the
// pointer descriptor of TouchHid.h (made for the scroll resolution and the pinch's modifiers before start).
class PianoTouchHID : public IOHIDDevice {
  OSDeclareDefaultStructors(PianoTouchHID)

public:
  TouchDescriptor fDescriptor = kTouchDescriptorDefault;
  IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
  OSString *newTransportString() const override { return OSString::withCString("SPI"); }
  OSString *newManufacturerString() const override { return OSString::withCString("UniXNU"); }
  OSString *newProductString() const override { return OSString::withCString("Piano Touch Screen"); }
  OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(kVendorId, 32); }
  OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(kProductId, 32); }
  OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x50540000u, 32); }
  OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(0x01, 32); }
  OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(0x02, 32); }
};

OSDefineMetaClassAndStructors(PianoTouchHID, IOHIDDevice)

IOReturn PianoTouchHID::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
  *descriptor = IOBufferMemoryDescriptor::withBytes(fDescriptor.bytes, fDescriptor.length, kIODirectionNone);
  return *descriptor ? kIOReturnSuccess : kIOReturnNoMemory;
}

// --------------------------------------------------------------------------------------------------------------------- the service
class PianoTouch : public IOService {
  OSDeclareDefaultStructors(PianoTouch)

  enum class BusKind : uint8_t { none, geni, gpio };
  PianoTouchHID *fHid = nullptr;
  IONotifier *fPublished = nullptr;
  BusKind fBus = BusKind::none;
  geni_spi fSpi = {};
  IOBufferMemoryDescriptor *fDma = nullptr;
  bool fPinsQup = false, fPinsGpio = false, fIrqArmed = false;
  Chip fChip;
  Image fImage;
  const uint8_t *fFw = nullptr;
  uint32_t fFwBytes = 0;
  const char *fFwName = "none";
  uint8_t *fFrame = nullptr;
  Matrix *fMatrix = nullptr;
  // The contacts and their gestures (TouchTrack.h, TouchGesture.h); at level 2 the reports go to fHid.
  Tracker fTracker;
  Gesture fGesture;
  bool fReportIds = false;              // the pinch's modifiers are in the descriptor
  uint8_t fSentModifiers = 0;
  uint32_t fContactLines = 0, fGestureLines = 0, fLastGestureStats = 0;
  // The CPU latency hold of a read burst (TouchPace.h BurstHold; piano_cpuidle_latency_hold when compiled in).
  BurstHold fBurst;
  int fCpuHold = -1;
  uint32_t fCpuHoldFailures = 0;
  // The controller's watchdog: the download again with a back-off (TouchPace.h WatchdogRecovery); the reads no edge started.
  WatchdogRecovery fWatchdog;
  bool fWdtWaitNoted = false;
  uint32_t fLevelReads = 0, fBlindReads = 0, fClockLosses = 0;
  // The power states (TouchPower.h): setPowerState writes fSystemOff and kicks fWake; the frame thread acts on it, parks (fParked)
  // while off, restarts on its own. The display's state from NativeGopDisplay's piano-display-power (fDisplay, retained once found).
  TouchPower fPower;
  pianoirq::Waiter fWake = {};
  volatile bool fSystemOff = false, fParked = false, fServing = false, fFramesStopped = false;
  bool fJoined = false;
  IOService *fDisplay = nullptr;
  uint64_t fDisplayReadMs = 0, fDisplayLookupMs = 0;
  int fDisplayState = -1;
  // transfer statistics
  uint32_t fXfers = 0, fXferErrors = 0;
  // serving statistics
  uint64_t fStartMs = 0, fLastReadMs = 0, fLastStatsMs = 0;
  uint32_t fEdges = 0, fReads = 0, fData = 0, fGood = 0, fBad = 0, fInconsistent = 0, fHand = 0, fWdt = 0, fReadErrors = 0;
  uint32_t fConsecutive = 0, fWdtRun = 0, fDownloads = 0, fGaps = 0, fShown = 0, fSent = 0, fSendErrors = 0;
  uint32_t fTypes[8] = {}, fTypeCount[8] = {};
  uint64_t fReadUsSum = 0;
  uint32_t fReadUsMax = 0, fReadsSinceStats = 0, fEdgesSinceStats = 0;
  uint16_t fLastCounter = 0;
  uint8_t fWdtState = 0;
  bool fHaveCounter = false, fEdgeSeen = false, fEdgePending = false;
  // The pace and the interrupt (TouchPace.h, PianoIrq.h): the edge after an idle sleep by the TLMM interrupt (piano-touch-irq=1).
  pianoirq::tlmm::Pin fPin = {};
  pianoirq::MissGuard fMiss = pianoirq::missGuard(3);
  bool fIrqLive = false, fEdgeByIrq = false, fPollNextEdge = false;
  uint32_t fIdleSleeps = 0, fIrqEdges = 0, fIrqReads = 0, fIrqBadReads = 0;
  uint32_t fRateKhz = 0;   // the SPI rate the GENI divider gives
  int fPanel = kPanelUnknown;   // gpio100: the firmware image's panel
  // The display's rotation (piano-touch-rotate=1): requested by callPlatformFunction / setProperties, taken by the frame thread at a
  // touch-down (TouchRotation.h). Without the argument it stays 0 and every report is as before.
  PatRotation fRotation = {0, 0, 0};
  const char *volatile fRotationSource = "none";
  // The uptime (ms) of the last frame with a finger on the screen (not a palm), for "PianoTouchContact" (PianoTouchClient.h).
  volatile uint64_t fLastContactMs = 0;
  // After a download and start: its first edge (TouchPace.h StartGrace; under piano-touch-noedge no read without one before it).
  StartGrace fStart;
  // The announced-frame rule's evidence (TouchPace.h): what the read in progress started on, the one before it and the gap; the
  // first watchdog frame of a run and the read before it; the controller gone quiet (no edge for kHoldMs) and stalled contacts.
  ReadOn fReadOn = ReadOn::none, fPrevReadOn = ReadOn::none, fWdtOn = ReadOn::none, fWdtPrevOn = ReadOn::none;
  uint64_t fPrevReadMs = 0, fStartedMs = 0;
  uint32_t fWdtGapMs = 0, fFramesSinceStart = 0, fStalls = 0;
  Quiet fQuiet;

  static void threadMain(void *self, wait_result_t);
  static bool eventServicePublished(void *target, void *refCon, IOService *service, IONotifier *notifier);
  static int busXfer(void *self, const uint8_t *tx, uint8_t *rx, uint32_t len);
  static void busSleep(void *, unsigned ms) { IOSleep(ms); }
  bool step(int32_t n, const char *what);
  void survey();
  void surveySmmu(bool apply);
  void run();
  bool busUp();
  bool geniUp();
  bool geniClocks();
  bool se2Clocked();
  bool geniLoopback();
  bool engineIdle();
  unsigned pollLimit(uint32_t len) const;
  bool gpioUp();
  void pinsToGpio();
  bool identify();
  bool firmware();
  bool startScanning();
  void latchIrq();
  void clearLatch();
  void armIrq();
  bool publish();
  void serve();
  void frame(uint32_t readUs);
  void contacts(uint64_t now);
  void gestures(uint64_t now);
  void lost(uint64_t now);
  void deliver(const GestureOut &out);
  void send(const PointerReport &r);
  void sendBytes(const uint8_t *bytes, unsigned length);
  void burst(uint64_t now, bool busy, bool end = false);
  int displayState(uint64_t now);
  bool powerStep(PowerStep step, const MatrixConfig &mc, uint64_t now);
  bool restart(bool fromSuspend, const MatrixConfig &mc);
  void park();
  void stats(uint64_t now);

public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoTouch, IOService)

IOService *PianoTouch::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoTouch::step(int32_t n, const char *what)
{
  if (gArgs.stop && uint32_t(n) >= gArgs.stop) {
    note("stopped before step %d (%s): piano-touch-stop=%u", n, what, gArgs.stop);
    return false;
  }
  stage(n, what);
  return true;
}

bool PianoTouch::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-touch=%u (%s), stop %u, bus %u (%s), %u kHz, seirq %u, smmu %u, loopback %u, mode %u, baseline %u, stylus %s, "
       "threshold %u, flip %s, poll %u ms, idle %u ms (%s), irq %u, noedge %u ms; firmware BOE %llu bytes, CSOT %llu bytes",
       gArgs.level, gArgs.level >= 2 ? "HID tablet pointer for macOS" : "log only, nothing for macOS", gArgs.stop, gArgs.bus,
       gArgs.bus == 0 ? "GENI, else by hand" : gArgs.bus == 1 ? "GENI only" : "by hand only", gArgs.khz, gArgs.seirq, gArgs.smmu,
       gArgs.loopback, gArgs.mode, gArgs.baseline, gArgs.stylus ? "on" : "off", gArgs.threshold, kFlipNames[gArgs.flip], gArgs.pollMs,
       gArgs.idleMs, gArgs.idleMs ? "the latch once per idle period at rest" : "every frame at once", gArgs.irq, gArgs.noEdgeMs,
       (unsigned long long)piano_touch_firmware_boe_size, (unsigned long long)piano_touch_firmware_csot_size);
  setProperty("piano-touch", gArgs.level, 32);
  if (gArgs.gestures)
    note("gestures: tap = click, held %u ms = right click%s, moved past %u px for %u ms = drag, two-finger tap = right click, "
         "two-finger scroll %u px a count (declared %u/inch, %s), pinch %s", gArgs.holdMs, gArgs.holdMs ? "" : " (never)",
         gArgs.slopPx, gArgs.dragDelayMs, gArgs.scrollPx, gArgs.scrollRes,
         gArgs.scrollInvert ? "inverted" : "the content follows the fingers under natural scrolling", kPinchNames[gArgs.pinch]);
  else
    note("gestures: off (piano-touch-gestures=0): the first contact is the tip, down presses, up releases");
  char fast[48] = "", palm[96] = "off (piano-touch-palm=0)";
  if (gArgs.fastPeak) snprintf(fast, sizeof fast, ", at once from a peak of %u", gArgs.fastPeak);
  if (gArgs.palmNodes) snprintf(palm, sizeof palm, "a blob of %u nodes or 3 peaks, and where one was for 500 ms", gArgs.palmNodes);
  note("contacts: up to %u followed; one counts after %u frames%s, ends after 3 without; palm: %s", kMaxTracks, gArgs.confirm, fast, palm);
  if (PIANO_TOUCH_LATENCY_HOLD && gArgs.cpuHoldUs)
    note("latency: while the panel is busy no CPU idle state slower than %u us to leave (piano_cpuidle_latency_hold)", gArgs.cpuHoldUs);
  else
    note("latency: no CPU latency hold (%s)", PIANO_TOUCH_LATENCY_HOLD ? "piano-touch-cpu-hold=0" :
         "built for a kernel without piano_cpuidle_latency_hold");
  if (gArgs.rotate) {
    setProperty(PAT_PROPERTY, 0ull, 32);
    note("rotation: following the display's (piano-touch-rotate=1): %s and the property %s taken (0/90/180/270); 0 until told",
         PAT_FUNCTION, PAT_PROPERTY);
  }
  // The unit's calibration (built in from the private Ubuntu file; its arrays are never logged), checked again here.
  if (kUnitCalibrationPresent && calibrationValid(kUnitCalibration))
    note("calibration: unit file (panel %s, sha256 %s), electrode maps, edge %d px%s",
         kUnitCalibration.projectVendorId == kVendorBoe ? "BOE" : kUnitCalibration.projectVendorId == kVendorCsot ? "CSOT" : "unknown",
         kUnitCalibrationSha, kUnitCalibration.edgeSize / kUnits,
         kUnitCalibration.invertY < 0 ? "" : kUnitCalibration.invertY ? ", invert_y true" : ", invert_y false");
  else if (kUnitCalibrationPresent)
    note("calibration: unit file (sha256 %s) INVALID: none (linear map)", kUnitCalibrationSha);
  else
    note("calibration: none (linear map)");
  if (kUnitInvertYOverride >= 0)
    note("orientation: this unit's Y %s by the build (PIANO_TOUCH_INVERT_Y=%d): piano-touch-flip=auto takes it; an explicit "
         "piano-touch-flip still wins", kUnitInvertYOverride ? "reversed" : "as the processor gives it", kUnitInvertYOverride);
  if (!step(1, "survey (read only): pins, SE2 clocks and registers, SMMU stream 0xa3, firmware images")) return true;
  survey();
  // Step 2 here, on the matching thread: the SMMU's stream-match registers are not safe to touch later from another thread
  // (NativeUfs: a read never returned). SPI by hand needs no SMMU: then it is not even read.
  const bool smmuStep = gArgs.bus != 2 && gArgs.smmu;
  if (smmuStep && step(2, "SMMU: stream 0xa3 (QUP1) through a bypass context bank, for SE2's DMA")) surveySmmu(true);
  else if (gArgs.bus != 2) surveySmmu(false);   // read only
  if (gArgs.stop && gArgs.stop <= 3) {
    if (gArgs.stop == 3 || !smmuStep) note("stopped before step 3 (bus): piano-touch-stop=%u", gArgs.stop);
    return true;
  }
  // The HID device from start, as PianoInput and PianoDwc3 publish theirs; it sends nothing until a contact comes.
  if (gArgs.level >= 2 && !publish()) return true;
  fChip.txBytes = 63 * 1024 + 8;
  fChip.rxBytes = kMaxFrame + 8;
  fChip.tx = static_cast<uint8_t *>(IOMallocData(fChip.txBytes));
  fChip.rx = static_cast<uint8_t *>(IOMallocData(fChip.rxBytes));
  fFrame = static_cast<uint8_t *>(IOMallocData(kMaxFrame + 8));
  fMatrix = static_cast<Matrix *>(IOMallocData(sizeof(Matrix)));
  if (!fChip.tx || !fChip.rx || !fFrame || !fMatrix) {
    stage(-3, "buffers");
    return true;
  }
  thread_t thread = nullptr;
  if (kernel_thread_start(threadMain, this, &thread) != KERN_SUCCESS) {
    stage(-3, "kernel_thread_start");
    return true;   // stays attached and does nothing more
  }
  thread_deallocate(thread);
  registerService();
  // The power tree (docs/power.md §4): setPowerState(off) before a system sleep, a restart or a shutdown stops the frames, on
  // restarts them (piano-touch-pm=0: not joined, as before).
  if (gArgs.pm) {
    fJoined = pianopower::join(this) == kIOReturnSuccess;
    note("power: %s the power tree: system off stops the frames and masks gpio162's interrupt, on restarts them; the display %s",
         fJoined ? "joined" : "NOT joined", gArgs.display ? "followed (piano-display-power: off mutes touch)" : "ignored (piano-touch-display=0)");
  }
  return true;
}

void PianoTouch::threadMain(void *self, wait_result_t)
{
  static_cast<PianoTouch *>(self)->run();
}

// ------------------------------------------------------------------------------------------------------------- step 1, 2
void PianoTouch::survey()
{
  for (unsigned p = 0; p < kPins; ++p) {
    Pin &pin = gPin[p];
    if (!mapRegion(pin.page, kTlmm + uint64_t(kGpio[p]) * 0x1000, 0x10)) {
      note("pins: gpio%u (%s) does not map", kGpio[p], pin.name);
      continue;
    }
    pin.ctl = rd32(pin.page.va + kCtl);
    pin.io = rd32(pin.page.va + kIo);
    pin.intrCfg = rd32(pin.page.va + kIntrCfg);
    note("pins: gpio%-3u %-6s CTL %08x (function %u, pull %u, drive %u mA, %s%s) IO %08x (in %u) INTR_CFG %08x STATUS %08x", kGpio[p],
         pin.name, pin.ctl, (pin.ctl >> 2) & 15, pin.ctl & 3, 2 * (((pin.ctl >> 6) & 7) + 1), pin.ctl & kOe ? "output" : "input",
         pin.ctl & kEgpioPresent ? (pin.ctl & kEgpioEnable ? ", eGPIO ours" : ", eGPIO not ours") : "", pin.io, pin.io & 1,
         pin.intrCfg, rd32(pin.page.va + kIntrStatus));
  }
  if (!mapRegion(gGcc, kGcc, kGccBytes)) {
    note("gcc: cannot map %#llx", (unsigned long long)kGcc);
  } else {
    char line[200];
    size_t used = 0;
    bool on = true;
    for (const Branch &b : kBranches) {
      const uint32_t v = gcc(b.cbcr);
      on = on && branchOn(v);
      append(line, sizeof line, used, "%s%s %08x (%s)", used ? ", " : "", b.name, v, !branchOn(v) ? "off" : v & 2 ? "hw-gated" : "on");
    }
    const uint32_t vote = gcc(kGccVote);
    note("gcc: vote %#x = %08x (wanted bits %08x %s); %s", kGccVote, vote, kVoteBits, (vote & kVoteBits) == kVoteBits ? "set" : "NOT all set",
         line);
    const Se2Clock c = se2Clock([](uint32_t off) { return gcc(off); });
    note("gcc: SE2 clock: QSPI ref RCG cmd %08x cfg %08x m %08x n %08x dfs %08x, divider %08x -> source %u Hz, SE2 %u Hz", c.cmd, c.cfg,
         c.m, c.n, c.dfs, c.divider, c.sourceHz, c.seHz);
    if (on && mapRegion(gSe, kSe2, kSe2Bytes) && mapRegion(gQup, kQup1, kQup1Bytes)) {
      note("se2: FW_REVISION %08x (protocol %u%s) IF_DISABLE %08x (FIFO %s) STATUS %08x HW_PARAM %08x %08x DMA_MODE %08x M_CLK_CFG %08x "
           "CLK_SEL %08x SE_IRQ_EN %08x DELAY %08x; QUP1 version %08x",
           seRd(&gSe, GENI_FW_REVISION_RO), (seRd(&gSe, GENI_FW_REVISION_RO) >> 8) & 0xff,
           ((seRd(&gSe, GENI_FW_REVISION_RO) >> 8) & 0xff) == GENI_PROTO_SPI ? ": SPI" : ": NOT SPI", seRd(&gSe, GENI_IF_DISABLE_RO),
           seRd(&gSe, GENI_IF_DISABLE_RO) & 1 ? "disabled: SE DMA" : "enabled", seRd(&gSe, SE_GENI_STATUS), seRd(&gSe, SE_HW_PARAM_0),
           seRd(&gSe, 0xe28), seRd(&gSe, SE_GENI_DMA_MODE_EN), seRd(&gSe, GENI_SER_M_CLK_CFG), seRd(&gSe, SE_GENI_CLK_SEL),
           seRd(&gSe, SE_IRQ_EN), seRd(&gSe, 0x278), seRd(&gQup, QUPV3_HW_VER_REG));
    } else {
      note("se2: registers not read (%s)", on ? "the engine does not map" : "a QUP1/SE2 clock is off");
    }
  }
  static const struct {
    const char *name;
    const uint8_t *data;
    const uint64_t *size;
    const char *sha;
  } kImages[2] = {{"CSOT", piano_touch_firmware_csot, &piano_touch_firmware_csot_size, piano_touch_firmware_csot_sha},
                  {"BOE", piano_touch_firmware_boe, &piano_touch_firmware_boe_size, piano_touch_firmware_boe_sha}};
  for (const auto &i : kImages) {
    if (!*i.size) {
      note("firmware %s: not built in", i.name);
      continue;
    }
    Image im;
    const int rc = parseImage(i.data, uint32_t(*i.size), im);
    note("firmware %s: %llu bytes (sha256 %.16s...), %s: version %#x, %u partitions, ILM %u bytes, DLM %u bytes%s", i.name,
         (unsigned long long)*i.size, i.sha, rc ? "NOT usable" : "parsed", im.version, im.count, im.part[0].size, im.part[1].size,
         im.cascade2nd ? ", cascaded second header (auto copy)" : "");
  }
}

void PianoTouch::surveySmmu(bool apply)
{
  if (!mapRegion(gSmmu, kSmmu, kSmmuBytes)) {
    note("smmu: cannot map %#llx", (unsigned long long)kSmmu);
    return;
  }
  const pianousb::SmmuPlan p = pianousb::planSmmuStream(kQup1Stream, kSmmuBytes, smmuRead);
  note("smmu: stream %#x: groups %u banks %u pages %u; %s slot %u (smr %08x s2cr %08x; bank sctlr %08x cbar %08x cba2r %08x) -> %s bank %u%s%s",
       kQup1Stream, p.groups, p.banks, p.pages, p.reuse ? "existing entry" : "free", p.slot, p.smr, p.s2cr, p.sctlr, p.cbar, p.cba2r,
       p.keep ? "keep" : "bypass", p.bank, p.error ? ": " : "", p.error ? p.error : "");
  if (!p.error && p.keep) gSmmuReady = true;
  if (!apply || p.error || p.keep) {
    if (apply && p.error) note("smmu: no bypass for stream 0xa3: SE DMA unusable (FIFO mode or SPI by hand still can run)");
    return;
  }
  const uint64_t deadline = nowUs() + 100000;
  const char *failure = pianousb::applySmmuStream(p, kQup1Stream, smmuRead,
                                                  [](uint32_t off, uint32_t v) { wr32(gSmmu.va + off, v); dsb(); },
                                                  [deadline] { return nowUs() >= deadline; });
  note("smmu: stream %#x: %s (slot %u: smr %08x s2cr %08x)", kQup1Stream, failure ? failure : "bypass set", p.slot,
       smmuRead(0x800 + 4 * p.slot), smmuRead(0xc00 + 4 * p.slot));
  gSmmuReady = failure == nullptr;
  if (failure) note("smmu: the bypass for stream 0xa3 did not take: SE DMA unusable");
}

// ------------------------------------------------------------------------------------------------------------- the thread
void PianoTouch::run()
{
  fStartMs = nowMs();
  fChip.bus = Bus{this, busXfer, busSleep, 0, 0};
  if (!step(3, "bus: SE2 (GENI) or SPI by hand on gpio40-43")) return;
  if (!busUp()) return;
  if (!step(4, "identify: engine reset, trim ID, memory map")) return;
  if (!identify()) return;
  if (!step(5, "firmware: lcd-id, host download with the hardware CRC")) return;
  latchIrq();   // gpio162's edges latched from here: each download clears what came before, the new firmware's first frame is kept
  if (!firmware()) return;
  if (!step(6, "start: payload length, firmware info, calibration, baseline, operating mode")) return;
  if (!startScanning()) return;
  if (!step(7, "frames: gpio162 edges latched and polled, a frame read per edge")) return;
  armIrq();
  setProperty("piano-touch-stage", uint64_t(uint32_t(piano_touch_stage)), 32);
  serve();
  __atomic_store_n(&fServing, false, __ATOMIC_RELEASE);   // setPowerState(off) no longer waits for a frame thread
}

int PianoTouch::busXfer(void *self, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  PianoTouch *t = static_cast<PianoTouch *>(self);
  ++t->fXfers;
  int rc = 0;
  if (t->fBus == BusKind::geni) {
    if (!t->se2Clocked()) return -5;   // never an SE2 access with its clock off (a bus stall)
    // With SE DMA the transfer returns on DMA done, the engine's command can still be shifting out: the next one waits for it.
    if (!t->engineIdle()) {
      if (++t->fXferErrors <= 10 || t->fXferErrors % 100 == 0)
        note("geni: transfer %u: the engine is still busy (STATUS %08x, %u errors)", t->fXfers, seRd(&gSe, SE_GENI_STATUS), t->fXferErrors);
      return -16;
    }
    gDelayCalls = 0;
    t->fSpi.snap_valid = 0;
    t->fSpi.poll_limit = t->pollLimit(len);
    rc = geni_spi_transfer(&t->fSpi, tx, rx, len, 0);
    if (rc && (++t->fXferErrors <= 10 || t->fXferErrors % 100 == 0))
      note("geni: transfer %u (%u bytes) %d (%u errors); M_IRQ %08x STATUS %08x DMA tx %08x rx %08x, moved %u/%u", t->fXfers, len, rc,
           t->fXferErrors, t->fSpi.snap_irq, t->fSpi.snap_status, t->fSpi.snap_dma_tx, t->fSpi.snap_dma_rx, t->fSpi.snap_dma_txin,
           t->fSpi.snap_dma_rxin);
  } else if (t->fBus == BusKind::gpio) {
    static const PinIo io = {
      nullptr, [](void *, bool h) { drive(kMosi, h); }, [](void *, bool h) { drive(kClk, h); }, [](void *, bool h) { drive(kCs, h); },
      [](void *) { return level(kMiso); }, [](void *, unsigned us) { IODelay(us); }};
    bitSpiTransfer(io, tx, rx, len);
  } else {
    rc = -19;
  }
  return rc;
}

// ------------------------------------------------------------------------------------------------------------- step 3
// The engine has no command running (M_ACTIVE clear), within 2 ms.
bool PianoTouch::engineIdle()
{
  for (unsigned n = 0; n < 2000; ++n) {
    if (!(seRd(&gSe, SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE)) return true;
    IODelay(1);
  }
  return false;
}

// Polls for a transfer of len bytes: twice its time on the wire (in ms with SE DMA, where polls past the first kSpinPolls sleep
// 1 ms; in us in FIFO mode, where every poll spins about 1 us), plus a margin.
unsigned PianoTouch::pollLimit(uint32_t len) const
{
  const uint64_t wireUs = uint64_t(len) * 8 * 1000 / (fRateKhz ? fRateKhz : 1000);
  return fSpi.use_dma ? unsigned(kSpinPolls + 100 + 2 * wireUs / 1000) : unsigned(100000 + 2 * wireUs);
}

bool PianoTouch::busUp()
{
  if (gArgs.bus != 2) {
    if (geniUp()) return true;
    if (gArgs.bus == 1) {
      stage(-3, "bus: the GENI engine is not usable (piano-touch-bus=1: no fallback)");
      return false;
    }
    note("bus: GENI not usable: SPI by hand on the same pins instead");
  }
  return gpioUp();
}

// Our votes in 0x52008 (the QUP1 wrapper and SE2 branches) under PianoA830's lock word (a holder that keeps it for 50 ms does not
// stop us: the update goes ahead). Bits are only ever set.
struct GccVoteResult {
  uint32_t before, after, waits;
  bool needed, locked;
};
static GccVoteResult voteSe2Clocks()
{
  GccVoteResult r = {0, 0, 0, (gcc(kGccVote) & kVoteBits) != kVoteBits, false};
  for (; r.needed && r.waits < 5000 && !r.locked; ++r.waits) {
    uint32_t expected = 0;
    r.locked = __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kGccVoteTag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    if (!r.locked) IODelay(10);
  }
  r.before = gcc(kGccVote);
  if ((r.before & kVoteBits) != kVoteBits) {
    wr32(gGcc.va + kGccVote, r.before | kVoteBits);
    dsb();
  }
  r.after = gcc(kGccVote);
  if (r.needed && r.locked) {
    uint32_t expected = kGccVoteTag;
    __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  return r;
}

// Before each GENI transfer: every QUP1/SE2 clock branch still runs (read in GCC, which is always safe). An SE2 register access
// with a branch off stalls the bus, which can hang the CPU until the watchdog resets the tablet; so a branch found off: our votes
// again and the transfer skipped (a transfer error: the frame is dropped, the next one retries).
bool PianoTouch::se2Clocked()
{
  uint32_t off = 0;
  bool on = true;
  for (const Branch &b : kBranches) {
    const uint32_t v = gcc(b.cbcr);
    if (!branchOn(v)) {
      on = false;
      off = v;
    }
  }
  if (on) return true;
  const GccVoteResult r = voteSe2Clocks();
  if (++fClockLosses <= 10 || fClockLosses % 100 == 0)
    note("geni: a QUP1/SE2 clock branch is off (CBCR %08x; vote %08x -> %08x; %u times): voted again, the transfer skipped", off,
         r.before, r.after, fClockLosses);
  return false;
}

bool PianoTouch::geniClocks()
{
  // Our votes even when the clocks already run (on someone else's vote they could stop later and stall the next SE2 access).
  const GccVoteResult r = voteSe2Clocks();
  const uint32_t before = r.before, after = r.after, waits = r.waits;
  const bool needed = r.needed, locked = r.locked;
  char line[200];
  size_t used = 0;
  bool on = true;
  for (const Branch &b : kBranches) {
    uint32_t v = 0;
    for (unsigned n = 0; n < 200; ++n) {
      v = gcc(b.cbcr);
      if (branchOn(v)) break;
      IODelay(10);
    }
    on = on && branchOn(v);
    append(line, sizeof line, used, "%s%s %08x", used ? ", " : "", b.name, v);
  }
  note("geni: votes %08x -> %08x (%s, %u lock waits); branches %s: %s", before, after,
       !needed ? "already set" : locked ? "set under the lock" : "lock held elsewhere: written anyway", waits, on ? "on" : "NOT all on", line);
  return on;
}

// The engine's internal loopback (MOSI to MISO): proves the clocks, the engine's mode and (with SE DMA) the stream's bypass without
// the controller. Its chip select is held high by hand meanwhile (gpio43 as a GPIO output), so the controller never takes the
// pattern for a command even if the pins already belong to the engine; the pattern starts with 0x00 (a read) all the same.
bool PianoTouch::geniLoopback()
{
  uint8_t tx[64], rx[64];
  for (unsigned i = 0; i < sizeof tx; ++i) tx[i] = uint8_t(i ? 0xa5 ^ (i * 37) : 0);
  drive(kCs, true);
  setCtl(kCs, pinCfg(kCs, 0, kDrive6mA, kPullUp, true));
  geni_spi_setup(&fSpi, GENI_SPI_LOOP, 0);
  gDelayCalls = 0;
  fSpi.snap_valid = 0;
  fSpi.poll_limit = pollLimit(sizeof tx);
  const uint64_t t0 = nowUs();
  const int rc = geni_spi_transfer(&fSpi, tx, rx, sizeof tx, 0);
  const uint64_t us = nowUs() - t0;
  geni_spi_setup(&fSpi, 0, 0);
  unsigned same = 0;
  for (unsigned i = 0; i < sizeof tx; ++i) same += rx[i] == tx[i];
  char a[64], b[64];
  note("geni: loopback %s (rc %d, %llu us): %u/64 bytes back; sent %s, got %s", !rc && same == 64 ? "PASSED" : "FAILED", rc,
       (unsigned long long)us, same, hex(tx, 8, a, sizeof a), hex(rx, 8, b, sizeof b));
  if (rc && fSpi.snap_valid)
    note("geni: loopback: M_IRQ %08x STATUS %08x TX_FIFO %08x RX_FIFO %08x DMA tx %08x rx %08x, moved %u/%u", fSpi.snap_irq,
         fSpi.snap_status, fSpi.snap_txfs, fSpi.snap_rxfs, fSpi.snap_dma_tx, fSpi.snap_dma_rx, fSpi.snap_dma_txin, fSpi.snap_dma_rxin);
  return !rc && same == 64;
}

bool PianoTouch::geniUp()
{
  if (!gGcc.va || !gPin[kCs].page.va) {
    note("geni: GCC or the pins did not map");
    return false;
  }
  if (!geniClocks()) return false;
  const Se2Clock c = se2Clock([](uint32_t off) { return gcc(off); });
  const uint32_t want = gArgs.khz * 1000, div = geniDivider(c.seHz, want);
  note("geni: SE2 clock %u Hz (RCG cfg %08x, divider %08x): SPI %u Hz wanted, GENI divider %u -> %u Hz", c.seHz, c.cfg, c.divider, want,
       div, div ? c.seHz / div : 0);
  if (!div) {
    note("geni: the SE2 clock rate is unknown (source not decoded)");
    return false;
  }
  if (!mapRegion(gSe, kSe2, kSe2Bytes) || !mapRegion(gQup, kQup1, kQup1Bytes)) {
    note("geni: SE2 or QUP1 does not map");
    return false;
  }
  fSpi.se = {&gSe, seRd, seWr, seDelay};
  fSpi.wrapper = {&gQup, seRd, seWr, seDelay};
  if (int rc = geni_spi_read_info(&fSpi)) {
    note("geni: read_info %d", rc);
    return false;
  }
  const geni_spi_info &in = fSpi.info;
  note("geni: FW %08x (protocol %u), IF_DISABLE %08x (FIFO %s), STATUS %08x, HW_PARAM_0 %08x (TX FIFO %u words of %u bits), QUP %08x",
       in.fw_revision, in.protocol, in.if_disable, in.fifo_disabled ? "disabled" : "enabled", in.geni_status, in.hw_param0,
       in.tx_fifo_depth, in.fifo_width_bits, in.qup_hw_version);
  if (in.protocol != GENI_PROTO_SPI) {
    note("geni: the engine holds no SPI firmware");
    return false;
  }
  if (in.geni_status & (GENI_STATUS_M_ACTIVE | (1u << 12))) {
    note("geni: a command is running on the engine (not ours): left alone");
    return false;
  }
  if (in.fifo_disabled) {
    if (!gSmmuReady) {
      note("geni: FIFO disabled and stream 0xa3 not bypassing (step 2): no SE DMA");
      return false;
    }
    constexpr uint32_t kHalf = 64 * 1024;
    if (!fDma)
      fDma = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                                         IOOptionBits(kIOMemoryMapperNone), 2 * kHalf, 4096);
    IOByteCount contiguous = 0;
    const uint64_t pa = fDma ? fDma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone) : 0;
    uint8_t *va = fDma ? static_cast<uint8_t *>(fDma->getBytesNoCopy()) : nullptr;
    if (!va || !pa || (pa & 0xfff) || contiguous < 2 * kHalf) {
      note("geni: no contiguous DMA buffer (pa %#llx, %llu contiguous)", (unsigned long long)pa, (unsigned long long)contiguous);
      return false;
    }
    bzero(va, 2 * kHalf);
    dmaCache(va, 2 * kHalf, 1);
    // Kept to the end: once the engine has seen these addresses they are never given back (as the 13T's touch code).
    fSpi.use_dma = 1;
    fSpi.dma = {nullptr, dmaClean, dmaCleanInval, dmaInval, va, va + kHalf, pa, pa + kHalf, kHalf};
    note("geni: SE DMA through %#llx (+%#x TX, +%#x RX)", (unsigned long long)pa, 0u, kHalf);
  }
  gDelaySleeps = fSpi.use_dma;
  fRateKhz = (c.seHz / div) / 1000;
  for (unsigned pass = 0; pass < 2; ++pass) {
    const bool irqOn = gArgs.seirq == 2 || (gArgs.seirq == 0 && pass == 1);
    fSpi.poll_only = irqOn ? 2 : 1;
    if (pass && !engineIdle()) {
      note("geni: the engine stays busy after the failed loopback (STATUS %08x): not retried", seRd(&gSe, SE_GENI_STATUS));
      return false;
    }
    int rc = geni_spi_init(&fSpi);
    if (!rc) rc = geni_spi_setup(&fSpi, 0, 0);
    if (!rc) rc = geni_spi_set_clock(&fSpi, want, c.seHz, 0);
    // No chip-select or inter-word delays (K90's driver on this SE writes 0; what the boot chain left would slow every byte).
    if (!rc) seWr(&gSe, 0x278, 0);
    note("geni: init (%s, engine interrupt outputs %s): %d; M_CLK_CFG %08x DMA_MODE %08x SE_IRQ_EN %08x DELAY %08x",
         fSpi.use_dma ? "SE DMA" : "FIFO", irqOn ? "on" : "off", rc, seRd(&gSe, GENI_SER_M_CLK_CFG), seRd(&gSe, SE_GENI_DMA_MODE_EN),
         seRd(&gSe, SE_IRQ_EN), seRd(&gSe, 0x278));
    if (rc) return false;
    if (!gArgs.loopback) break;
    if (geniLoopback()) break;
    if (pass == 1 || gArgs.seirq != 0) return false;
    note("geni: retrying with the engine's interrupt outputs on (piano-touch-seirq=0)");
  }
  // The pins to the engine: function 1 (qup1_se2), 6 mA, no bias, as the DT's qup-spi2 states.
  for (unsigned p = kMiso; p <= kCs; ++p) setCtl(p, pinCfg(p, kFunctionQup, kDrive6mA, 0, false));
  fPinsQup = true;
  note("geni: pins qup1_se2: gpio40 %08x gpio41 %08x gpio42 %08x gpio43 %08x", rd32(pinVa(kMiso)), rd32(pinVa(kMosi)), rd32(pinVa(kClk)),
       rd32(pinVa(kCs)));
  fBus = BusKind::geni;
  fChip.bus.maxWrite = 63 * 1024;
  fChip.bus.maxRead = kMaxFrame - 2;
  return true;
}

void PianoTouch::pinsToGpio()
{
  // As Project-SunUEFI's GPIO SPI: all four GPIO inputs (6 mA, pull-down) first, CS high / CLK and MOSI low, then the outputs on
  // (CS with its pull-up).
  for (unsigned p = kMiso; p <= kCs; ++p) setCtl(p, pinCfg(p, 0, kDrive6mA, kPullDown, false));
  drive(kCs, true);
  drive(kClk, false);
  drive(kMosi, false);
  for (unsigned p = kMosi; p <= kCs; ++p) setCtl(p, pinCfg(p, 0, kDrive6mA, p == kCs ? kPullUp : kPullDown, true));
  fPinsGpio = true;
  fPinsQup = false;
}

bool PianoTouch::gpioUp()
{
  for (unsigned p = kMiso; p <= kCs; ++p)
    if (!gPin[p].page.va) {
      stage(-3, "bus: a pin does not map");
      return false;
    }
  pinsToGpio();
  note("gpio: SPI by hand: gpio40 %08x gpio41 %08x gpio42 %08x gpio43 %08x; CS %u CLK %u MISO %u", rd32(pinVa(kMiso)), rd32(pinVa(kMosi)),
       rd32(pinVa(kClk)), rd32(pinVa(kCs)), level(kCs), level(kClk), level(kMiso));
  fBus = BusKind::gpio;
  fChip.bus.maxWrite = 512;
  fChip.bus.maxRead = 64;
  return true;
}

// ------------------------------------------------------------------------------------------------------------- step 4-6
bool PianoTouch::identify()
{
  for (unsigned round = 0; round < 2; ++round) {
    const uint64_t t0 = nowMs();
    const int rc = pianotouch::identify(fChip);
    char id[40];
    note("identify (%s): %s after %u tries, %llu ms: id %s, table %d%s%s, event buffer %#x, poll info %#x", fBus == BusKind::geni ? "GENI" : "by hand",
         rc ? "NO NT36532" : "NT36532", fChip.tries, (unsigned long long)(nowMs() - t0), hex(fChip.id, 6, id, sizeof id), fChip.table,
         rc ? "" : fChip.e ? " (E)" : " (not E)", rc ? "" : fChip.cascade ? ", cascade" : ", single", fChip.eventBuf, fChip.pollInfo);
    if (!rc) return true;
    if (round || fBus != BusKind::geni || gArgs.bus != 0) break;
    note("identify: no answer over GENI: SPI by hand on the same pins");
    if (!gpioUp()) return false;
  }
  stage(-4, "identify: no NT36532 trim ID (no controller on this bus, or the bus does not move bits)");
  return false;
}

bool PianoTouch::firmware()
{
  if (!fFw) {
    // lcd-id: GPIO input for one read (function 0, output off, pull and drive kept), then as it was.
    Pin &id = gPin[kLcdId];
    int panel = -1;
    if (id.page.va) {
      setCtl(kLcdId, (id.ctl & ~(kOe | (0xfu << 2)) & ~kEgpioEnable) | (id.ctl & kEgpioPresent ? kEgpioEnable : 0));
      IODelay(20);
      panel = level(kLcdId);
      setCtl(kLcdId, id.ctl);
    }
    fPanel = panel == 1 ? kPanelBoe : panel == 0 ? kPanelCsot : kPanelUnknown;
    const bool boe = panel == 1;
    fFw = boe ? piano_touch_firmware_boe : piano_touch_firmware_csot;
    fFwBytes = uint32_t(boe ? piano_touch_firmware_boe_size : piano_touch_firmware_csot_size);
    fFwName = boe ? "BOE" : "CSOT";
    note("firmware: lcd-id gpio100 = %d: %s panel, image %u bytes%s", panel, fFwName, fFwBytes, panel < 0 ? " (pin did not map: CSOT)" : "");
    if (!fFwBytes || parseImage(fFw, fFwBytes, fImage)) {
      stage(-5, "firmware: no usable image for this panel (built without it? tooling/k27/build-native-piano-touch.sh)");
      return false;
    }
  }
  const uint64_t t0 = nowMs();
  const int rc = download(fChip, fImage, fFw, fFwBytes);
  ++fDownloads;
  note("firmware: download %u %s: attempt %u, stage %u (rc %d), %u bytes written, %llu ms; reset state %#04x after %u polls%s",
       fDownloads, rc ? "FAILED" : "done", fChip.attempt, fChip.stage, fChip.stageRc, fChip.written, (unsigned long long)(nowMs() - t0),
       fChip.resetState, fChip.polls, fImage.cascade2nd ? "" : " (no auto copy)");
  if (rc) {
    stage(-5, "firmware: the controller did not boot the image (reset state never 0xa0..0xaf)");
    return false;
  }
  // The edges latched until now were the old firmware's (or its watchdog state's): the new one's first announced frame comes next.
  clearLatch();
  fStartedMs = nowMs();
  fFramesSinceStart = 0;
  fQuiet.started(fStartedMs);
  return true;
}

static const char *command(int rc) { return rc == 1 ? "not sent" : rc == 0 ? "taken" : "NOT taken (continuing)"; }

bool PianoTouch::startScanning()
{
  const int rc = pianotouch::start(fChip, gArgs.baseline != 0, gArgs.mode, gArgs.stylus == 0);
  note("start: %s (rc %d); payload %u bytes (reported %u), frame %u bytes; firmware info version %#x %s, x %u y %u; reset state %#04x "
       "(%s after %u polls); stylus off %s, baseline command (%u) %s, mode %u %s",
       rc ? "FAILED" : "scanning", rc, fChip.payload, fChip.payloadReported, frameBytes(fChip), fChip.fwVersion,
       fChip.fwInfoValid ? "(valid)" : "(NOT valid)", fChip.xNum, fChip.yNum, fChip.resetState, fChip.calibrated ? "calibrated" : "NOT calibrated",
       fChip.polls, command(fChip.stylusRc), kIdleBaselineUpdate, command(fChip.baselineRc), gArgs.mode, command(fChip.modeRc));
  if (rc) {
    stage(-6, "start: the bus failed during the start sequence");
    return false;
  }
  return true;
}

// Rising edges of gpio162 latched in its INTR_STATUS (raw status on, interrupt enable off: nothing reaches the GIC), from step 5 on
// (TouchPace.h: a frame is read only when the controller announced it, the start's first one included).
void PianoTouch::latchIrq()
{
  Pin &irq = gPin[kIrq];
  if (!irq.page.va || fIrqArmed) return;
  setCtl(kIrq, (irq.ctl & ~(kOe | (0xfu << 2)) & ~kEgpioEnable) | (irq.ctl & kEgpioPresent ? kEgpioEnable : 0));
  const uint32_t cfg = (irq.intrCfg & ~(0x1fu | (7u << 8))) | (1u << 4) | (1u << 2) | (1u << 1) | (3u << 8);
  wr32(pinVa(kIrq) + kIntrCfg, cfg);
  wr32(pinVa(kIrq) + kIntrStatus, 0);
  dsb();
  fIrqArmed = true;
}

// The latch emptied: after a download only (the edges before it were the old firmware's); a latched edge is otherwise never dropped.
void PianoTouch::clearLatch()
{
  if (fIrqArmed) {
    wr32(pinVa(kIrq) + kIntrStatus, 0);
    dsb();
  }
  fEdgePending = false;
}

void PianoTouch::armIrq()
{
  if (!fIrqArmed) {
    note("frames: gpio162 does not map: nothing announces a frame, one read per %u ms without an edge (the level-1 loop)", kNoEdgeOldMs);
    return;
  }
  note("frames: gpio162 CTL %08x INTR_CFG %08x (rising edge latched since step 5, not enabled), level %u, latched %u",
       rd32(pinVa(kIrq) + kCtl), rd32(pinVa(kIrq) + kIntrCfg), level(kIrq), rd32(pinVa(kIrq) + kIntrStatus) & 1);
  if (!gArgs.irq || !gArgs.idleMs) return;
  // The same latch, and its interrupt on the TLMM summary line (enabled only while an idle frame's edge is waited for).
  char why[160] = {};
  fIrqLive = fPin.open("PianoTouch", 162, pinVa(kIrq), pianoirq::tlmm::Trigger::rising, why, sizeof why);
  if (fIrqLive)
    note("frames: gpio162 interrupt on the TLMM summary (SPI %u, INTID %u): slot %u, INTR_CFG armed %08x masked %08x; idle frames' edges by "
         "interrupt", pianoirq::kSpiTlmmSummary, pianoirq::intid(pianoirq::kSpiTlmmSummary), fPin.slot, fPin.armed, fPin.masked);
  else
    note("frames: gpio162 interrupt NOT used (%s): idle frames' edges by 1 ms polls", why);
}

// --------------------------------------------------------------------------------------------------------------- HID (level 2)
bool PianoTouch::publish()
{
  PianoTouchHID *hid = OSTypeAlloc(PianoTouchHID);
  if (!hid || !hid->init(nullptr)) {
    OSSafeReleaseNULL(hid);
    stage(-3, "HID device: init");
    return false;
  }
  fReportIds = gArgs.gestures && gArgs.pinch;
  hid->fDescriptor = makeTouchDescriptor(gArgs.scrollRes, fReportIds);
  hid->setProperty(kIOHIDBuiltInKey, true);
  if (!hid->attach(this)) {
    OSSafeReleaseNULL(hid);
    stage(-3, "HID device: attach");
    return false;
  }
  if (!hid->start(this)) {
    hid->detach(this);
    OSSafeReleaseNULL(hid);
    stage(-3, "HID device: IOHIDDevice::start");
    return false;
  }
  fHid = hid;
  note("HID device: touch screen pointer (absolute X/Y, 3 buttons, wheel and AC Pan at %u counts/inch%s), vendor %#x product %#x, "
       "%u-byte descriptor, registry ID %#llx", gArgs.scrollRes, fReportIds ? "; the keyboard modifiers as report 2" : "", kVendorId,
       kProductId, hid->fDescriptor.length, hid->getRegistryEntryID());
  if (OSDictionary *matching = IOService::serviceMatching("IOHIDEventService")) {
    fPublished = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, eventServicePublished, this, nullptr);
    matching->release();
  }
  return true;
}

bool PianoTouch::eventServicePublished(void *, void *, IOService *service, IONotifier *)
{
  for (IOService *provider = service->getProvider(); provider; provider = provider->getProvider())
    if (OSDynamicCast(PianoTouchHID, provider)) {
      note("event service %s published on the touch screen: macOS has it", service->getMetaClass()->getClassName());
      break;
    }
  return true;
}

// The display's rotation (piano-touch-rotate=1), from a kernel caller: param1 = degrees (uintptr_t), param2 = the caller's name. Only
// recorded; the frame thread takes it at the next touch-down.
IOReturn PianoTouch::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  if (name && name->isEqualTo(pianotouch::kFunctionContact)) {   // PianoTouchClient.h: one word, no lock, no wait
    if (!p1) return kIOReturnBadArgument;
    *static_cast<uint64_t *>(p1) = __atomic_load_n(&fLastContactMs, __ATOMIC_ACQUIRE);
    return kIOReturnSuccess;
  }
  if (!name || !name->isEqualTo(PAT_FUNCTION)) return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
  if (!gArgs.rotate) return kIOReturnUnsupported;
  if (!pat_request(&fRotation, uint32_t(uintptr_t(p1)))) return kIOReturnBadArgument;
  fRotationSource = p2 ? static_cast<const char *>(p2) : "a kernel caller";
  return kIOReturnSuccess;
}

// The same from user space (IORegistryEntrySetCFProperty): "piano-touch-rotation" = 0/90/180/270, from root or the console user (the
// rotation agent runs as the logged-in user; a user process can choose only among the four rotations).
IOReturn PianoTouch::setProperties(OSObject *properties)
{
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  OSNumber *degrees = d ? OSDynamicCast(OSNumber, d->getObject(PAT_PROPERTY)) : nullptr;
  if (!degrees || !gArgs.rotate) return IOService::setProperties(properties);   // as before this override (unsupported)
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess &&
      IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeConsoleUser) != kIOReturnSuccess)
    return kIOReturnNotPrivileged;
  if (degrees->unsigned64BitValue() > 270 || !pat_request(&fRotation, degrees->unsigned32BitValue())) return kIOReturnBadArgument;
  fRotationSource = "setProperties";
  return kIOReturnSuccess;
}

void PianoTouch::sendBytes(const uint8_t *bytes, unsigned length)
{
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, length, kIODirectionNone);
  const IOReturn rc = report ? fHid->handleReport(report) : kIOReturnNoMemory;
  OSSafeReleaseNULL(report);
  ++fSent;
  if (rc != kIOReturnSuccess && (++fSendErrors <= 5 || fSendErrors % 1000 == 0)) note("HID: handleReport %#x (%u failures)", rc, fSendErrors);
}

// One report of the gestures (level 2): its position is the rotated desktop's (gestures() mapped the contacts through the rotation
// in use), scaled over that desktop's size. With the pinch's modifiers in the descriptor a change of them goes first, as report 2.
void PianoTouch::send(const PointerReport &r)
{
  if (!fHid) return;
  uint8_t b[kMaxReportBytes];
  if (fReportIds && r.modifiers != fSentModifiers) {
    const unsigned n = modifierReport(r.modifiers, b);
    sendBytes(b, n);
    fSentModifiers = r.modifiers;
  }
  const PatPoint desktop = pat_rotate(fRotation.shown, 0, 0, kPanelW, kPanelH);
  const unsigned n = pointerReport(r, desktop.width, desktop.height, fReportIds, b);
  sendBytes(b, n);
}

// The tracker's notes of this frame (TouchTrack.h): the first 200 lines, then about every 20th contact. Positions only at level 1
// or with piano-touch-trace=1.
void PianoTouch::contacts(uint64_t now)
{
  const bool show = gArgs.level < 2 || gArgs.trace;
  for (unsigned i = 0; i < fTracker.noteCount; ++i) {
    const TrackNote &n = fTracker.notes[i];
    const Track &t = n.track;
    if (fContactLines >= 200 && t.id % 20) continue;
    ++fContactLines;
    const char *palm = t.palm ? " (a palm's)" : "";
    switch (n.kind) {
    case TrackNoteKind::down:
      if (show) note("contact %u down at (%d, %d) px, peak %d, area %u nodes%s, frame %u", t.id, t.x, t.y, t.peak, t.area, palm, fMatrix->lastFrameNo);
      else note("contact %u down, peak %d, area %u nodes%s", t.id, t.peak, t.area, palm);
      break;
    case TrackNoteKind::up:
      if (show)
        note("contact %u up at (%d, %d) px after %llu ms (%u frames, peak up to %d, area up to %u nodes)%s", t.id, t.x, t.y,
             (unsigned long long)(now - t.downMs), t.frames, t.maxPeak, t.maxArea, palm);
      else
        note("contact %u up after %llu ms (%u frames, peak up to %d, area up to %u nodes)%s", t.id, (unsigned long long)(now - t.downMs),
             t.frames, t.maxPeak, t.maxArea, palm);
      break;
    case TrackNoteKind::palm:
      note("contact %u is a palm (area %u nodes, %u peaks in its blob): no gesture from it", t.id, t.area, t.blobPeaks);
      break;
    }
  }
}

// The gestures of this frame: the active contacts in the desktop (the rotation is latched when a gesture begins, never in the
// middle of one), the engine, then the reports and the notes.
void PianoTouch::gestures(uint64_t now)
{
  if (gArgs.rotate && gestureIdle(fGesture) && trackerActive(fTracker, true)) {
    const uint32_t was = fRotation.shown;
    if (pat_latch(&fRotation, 1) != was) {
      note("rotation: %u degrees from this touch on (was %u; from %s, change %u)", fRotation.shown, was, fRotationSource,
           fRotation.changes);
      setProperty(PAT_PROPERTY, uint64_t(fRotation.shown), 32);
    }
  }
  GestureContact c[kMaxTracks];
  unsigned n = 0;
  bool finger = false;
  for (const Track &t : fTracker.t) {
    if (t.state != TrackState::active) continue;
    const PatPoint p = pat_rotate(fRotation.shown, t.x, t.y, kPanelW, kPanelH);
    c[n++] = GestureContact{t.id, t.palm, p.x, p.y, t.frames};
    finger = finger || !t.palm;
  }
  if (finger) __atomic_store_n(&fLastContactMs, now, __ATOMIC_RELEASE);
  GestureOut out;
  gestureUpdate(fGesture, c, n, now, out);
  deliver(out);
}

// Frames lost (a read error, the watchdog): every contact ends, a held button or modifier is released.
void PianoTouch::lost(uint64_t now)
{
  trackerCancel(fTracker);
  contacts(now);
  GestureOut out;
  gestureCancel(fGesture, out, now);
  deliver(out);
}

void PianoTouch::deliver(const GestureOut &out)
{
  for (unsigned i = 0; i < out.n; ++i) send(out.r[i]);
  const bool show = gArgs.level < 2 || gArgs.trace;
  const uint32_t made = fGesture.taps + fGesture.rights + fGesture.drags + fGesture.scrolls + fGesture.pinches + fGesture.cancels;
  for (unsigned i = 0; i < out.noteCount; ++i) {
    const GestureNote &g = out.notes[i];
    if (fGestureLines >= 200 && made % 20) continue;
    ++fGestureLines;
    char at[40] = "";
    if (show) snprintf(at, sizeof at, " at (%d, %d)", g.x, g.y);
    static const char *const kAxis[3] = {"both axes", "vertical", "horizontal"};
    switch (g.kind) {
    case GestureKind::tap: note("gesture: tap%s -> click (%u ms, %d px at most from the landing%s)", at, g.ms, g.a,
                                g.b ? ": past the slop, lifted before the drag" : ""); break;
    case GestureKind::holdRight: note("gesture: held %u ms%s -> right click", g.ms, at); break;
    case GestureKind::dragStart: note("gesture: drag%s, left button down (%u ms after the touch)", at, g.ms); break;
    case GestureKind::dragEnd: note("gesture: drag ends%s after %u ms, %d px from its start", at, g.ms, g.a); break;
    case GestureKind::twoTap: note("gesture: two-finger tap%s -> right click (%u ms)", at, g.ms); break;
    case GestureKind::scrollStart: note("gesture: two-finger scroll%s, %s", at, kAxis[g.a < 0 || g.a > 2 ? 0 : g.a]); break;
    case GestureKind::scrollEnd: note("gesture: scroll ends after %u ms: wheel %d, AC Pan %d counts", g.ms, g.a, g.b); break;
    case GestureKind::pinchStart: note("gesture: pinch%s: %s", at, kPinchNames[g.a < 0 || g.a > 2 ? 0 : g.a]); break;
    case GestureKind::pinchEnd:
      note("gesture: pinch ends after %u ms: spread %d px, %d zoom counts%s", g.ms, g.a, g.b, gArgs.pinch ? "" : " (not sent)");
      break;
    case GestureKind::cancelPalm: note("gesture: cancelled (a contact turned out a palm); nothing until every finger is up"); break;
    case GestureKind::cancelFingers: note("gesture: three or more fingers: nothing until every finger is up"); break;
    case GestureKind::cancelLost: note("gesture: cancelled (frames lost): released"); break;
    case GestureKind::plainDown: note("touch down%s (the tip)", at); break;
    case GestureKind::plainUp: note("touch up%s after %u ms", at, g.ms); break;
    }
  }
}

// The CPU latency hold of a read burst (TouchPace.h BurstHold): taken when the panel turns busy, released 1.5 s after it was last
// busy, or at once (end) when the frame loop stops.
void PianoTouch::burst(uint64_t now, bool busy, bool end)
{
#if PIANO_TOUCH_LATENCY_HOLD
  if (!gArgs.cpuHoldUs) return;
  const int s = end ? (fBurst.held ? -1 : 0) : fBurst.step(now, busy);
  if (end) fBurst.held = false;
  if (s > 0 && fCpuHold < 0) {
    fCpuHold = piano_cpuidle_latency_hold(gArgs.cpuHoldUs);
    if (fCpuHold < 0 && ++fCpuHoldFailures <= 3) note("latency: no free hold slot (piano_cpuidle_latency_hold -1): this burst without");
  } else if (s < 0 && fCpuHold >= 0) {
    piano_cpuidle_latency_release(fCpuHold);
    fCpuHold = -1;
  }
#else
  (void)now;
  (void)busy;
  (void)end;
#endif
}

// ------------------------------------------------------------------------------------------------------------- power
// The power contract (docs/power.md §4, TouchPower.h): off before a system sleep (and a restart or shutdown), on after it. Here only
// recorded and the frame thread kicked: it ends the contacts, releases the latency hold, masks its interrupt and parks. Off waits
// for that, at most 600 ms (an idle wait is woken at once, a frame read takes ~5 ms), and acknowledges in any case (the kernel's
// freeze stops the thread wherever it is). On returns at once: the thread restarts asynchronously (Ubuntu: a faster wake).
IOReturn PianoTouch::setPowerState(unsigned long state, IOService *)
{
  const bool off = state == pianopower::kOff;
  __atomic_store_n(&fSystemOff, off, __ATOMIC_RELEASE);
  fWake.kick();
  if (!off || !__atomic_load_n(&fServing, __ATOMIC_ACQUIRE)) return pianopower::kAckNow;
  unsigned waited = 0;
  while (!__atomic_load_n(&fParked, __ATOMIC_ACQUIRE) && waited < 600) {
    IOSleep(1);
    ++waited;
  }
  note("power: system off %s after %u ms", __atomic_load_n(&fParked, __ATOMIC_ACQUIRE) ? "taken, frames stopped" :
       "NOT taken yet (the frame thread parks at its next pass)", waited);
  return pianopower::kAckNow;
}

// NativeGopDisplay's piano-display-power (1 on, 0 off; -1 not published: it publishes it with piano-backlight=2), read every
// kDisplayPollMs; the service is found by class (no link to it), looked up at most every 5 s until found, retained.
int PianoTouch::displayState(uint64_t now)
{
  if (!gArgs.display) return -1;
  if (fDisplayReadMs && now - fDisplayReadMs < kDisplayPollMs) return fDisplayState;
  fDisplayReadMs = now;
  if (!fDisplay && (!fDisplayLookupMs || now - fDisplayLookupMs >= 5000)) {
    fDisplayLookupMs = now;
    if (OSDictionary *match = IOService::serviceMatching("NativeGopDisplay")) {
      fDisplay = IOService::copyMatchingService(match);
      match->release();
    }
  }
  int state = -1;
  if (fDisplay) {
    OSObject *v = fDisplay->copyProperty("piano-display-power");
    if (OSBoolean *b = OSDynamicCast(OSBoolean, v)) state = b->isTrue() ? 1 : 0;
    else if (OSNumber *n = OSDynamicCast(OSNumber, v)) state = n->unsigned64BitValue() ? 1 : 0;
    OSSafeReleaseNULL(v);
  }
  if (state >= 0 && fDisplayState < 0) note("display: following NativeGopDisplay's piano-display-power (%s now)", state ? "on" : "off");
  fDisplayState = state;
  return state;
}

// The frame thread while the system is off: until setPowerState(on) (the kernel freezes it in between).
void PianoTouch::park()
{
  __atomic_store_n(&fParked, true, __ATOMIC_RELEASE);
  for (;;) {
    const uint32_t seen = fWake.now();
    if (!__atomic_load_n(&fSystemOff, __ATOMIC_ACQUIRE)) break;
    fWake.wait(seen, 1000);
  }
  __atomic_store_n(&fParked, false, __ATOMIC_RELEASE);
}

// After a suspend or the display's return: the controller probed by reads; kept, the frames go on (after a suspend the reference is
// learned again: the time asleep is unknown); lost, the download again, at most kResumeDownloads times, then the reference. The
// latch, the contacts and the gestures start afresh. False: the controller did not come back (frames stopped until the next
// resume or display return).
bool PianoTouch::restart(bool fromSuspend, const MatrixConfig &mc)
{
  bool kept = false;
  int rc = probeState(fChip, &kept);
  if (rc && fBus == BusKind::geni) {   // the engine itself may have lost its setup: once more from its clocks and loopback
    note("%s: the probe's read failed (rc %d): the GENI engine set up again", fromSuspend ? "power" : "display", rc);
    if (geniUp()) rc = probeState(fChip, &kept);
  }
  // Reads of the firmware info and the reset state only, no frame (TouchPace.h: a frame is read when the controller announces it).
  // Both survive the watchdog state (attend1: "kept its state", then 0xfd frames): a controller in it announces its 0xfd blocks all
  // the time, and the frame loop downloads again after eleven of them.
  note("%s: the controller %s: firmware info %#04x/%#04x (started with %#04x), reset state %#04x%s", fromSuspend ? "power" : "display",
       rc == kOk && kept ? "kept its state" : "lost its state", fChip.probeVersion, fChip.probeComplement, fChip.fwVersion,
       fChip.probeReset, rc ? " (the probe's read failed)" : "");
  unsigned tried = 0;
  Restart plan = restartPlan(fromSuspend, rc == kOk && kept, tried);
  while (plan == Restart::download) {
    ++tried;
    plan = firmware() && startScanning() ? Restart::relearn : restartPlan(fromSuspend, false, tried);
  }
  fFramesStopped = plan == Restart::giveUp;
  if (plan == Restart::relearn) matrixReset(*fMatrix, mc);
  if (tried && plan == Restart::relearn) fStart.started(nowMs());   // a download and start: the first frame's edge first
  trackerReset(fTracker, trackConfig());
  gestureReset(fGesture, gestureConfig());
  fWdtRun = fConsecutive = 0;
  // The latch is kept: an edge latched meanwhile is a frame the controller announced and waits with (a download cleared it).
  return !fFramesStopped;
}

bool PianoTouch::powerStep(PowerStep s, const MatrixConfig &mc, uint64_t now)
{
  switch (s) {
  case PowerStep::none:
    return true;
  case PowerStep::suspend:
    lost(now);
    burst(now, false, true);
    if (fIrqLive) fPin.mask();
    note("power: system off (suspend %u): frames stopped, contacts released, CPU latency hold released, gpio162 interrupt masked",
         fPower.suspends);
    park();
    return true;
  case PowerStep::mute:
    lost(now);
    burst(now, false, true);
    note("display off: touch muted (nothing reported), the latch looked at once per %u ms (piano-touch-display=0 ignores the display)",
         kMutedReadMs);
    return true;
  case PowerStep::resume:
  case PowerStep::unmute: {
    const uint64_t t0 = nowMs();
    const bool back = restart(s == PowerStep::resume, mc);
    note("%s: %s after %llu ms%s", s == PowerStep::resume ? "power: system on" : "display on",
         back ? "touch back" : "the controller did not come back: frames stopped until the next resume", (unsigned long long)(nowMs() - t0),
         fPower.muted() ? " (the display is off: muted)" : "");
    fLastReadMs = nowMs();
    return true;
  }
  }
  return true;
}

// ------------------------------------------------------------------------------------------------------------- step 7
void PianoTouch::frame(uint32_t readUs)
{
  const uint32_t len = frameBytes(fChip);
  const uint8_t *packet = fFrame + kTransportBytes;
  const FrameKind kind = frameKind(fFrame);
  if (kind == FrameKind::handshake) {
    ++fHand;
    fWdtRun = 0;
    return;
  }
  if (kind == FrameKind::watchdog) {
    ++fWdt;
    fWdtState = fFrame[1];
    if (!fWdtRun) {   // the run's first: what this read and the one before it started on (TouchPace.h)
      fWdtOn = fReadOn;
      fWdtPrevOn = fPrevReadOn;
      fWdtGapMs = uint32_t(fLastReadMs - fPrevReadMs);
    }
    if (++fWdtRun == 11)
      note("frames: %u watchdog frames in a row (state %#04x); the first read %s, %u ms after the read before it (%s); %u frames since "
           "the last start, %llu ms ago", fWdtRun, fWdtState, readOnName(fWdtOn), fWdtGapMs, readOnName(fWdtPrevOn), fFramesSinceStart,
           (unsigned long long)(fLastReadMs - fStartedMs));
    return;
  }
  fWdtRun = 0;
  ++fFramesSinceStart;
  ++fData;
  const int sum = packetChecksum(packet, fChip.payload);
  const PacketFields f = packetFields(packet);
  if (sum > 0) {
    ++fGood;
    if (fHaveCounter && uint16_t(fLastCounter + 1) != f.counter) ++fGaps;
    fLastCounter = f.counter;
    fHaveCounter = true;
    unsigned k = 0;
    while (k < 8 && fTypeCount[k] && fTypes[k] != f.type) ++k;
    if (k < 8) {
      fTypes[k] = f.type;
      ++fTypeCount[k];
    }
  } else if (sum == 0) {
    ++fBad;
  } else {
    ++fInconsistent;
  }
  if (fShown < 3 || (gArgs.trace && fShown < 40) || (sum <= 0 && fBad + fInconsistent <= 5)) {
    ++fShown;
    char t[64], p[64];
    note("frame %u (%u bytes, read %u us): transport %s; packet %s: marker %#04x counter %u frame %u scan %u type %u rows %u cols %u "
         "words %u checksum %s",
         fReads, len, readUs, hex(fFrame + 1, 8, t, sizeof t), hex(packet, 8, p, sizeof p), f.marker, f.counter, f.frameNo, f.scanMode,
         f.type, f.rows, f.cols, le32(packet + 8), sum > 0 ? "ok" : sum == 0 ? "BAD" : "INCONSISTENT");
  }
  if (sum <= 0) return;
  if (matrixFeed(*fMatrix, packet, fChip.payload) != Feed::decoded) return;
  if (fMatrix->frames == 1)
    note("matrix: reference of %u frames of type %d (%u x %u) after %u good frames; decoding", fMatrix->refCount, fMatrix->refType,
         fMatrix->rows, fMatrix->cols, fGood);
  if (!fPower.reporting()) return;   // the display off: the reference follows the drift, nothing is tracked or reported
  const uint64_t now = nowMs();
  trackerUpdate(fTracker, fMatrix->points, fMatrix->pointCount, now);
  contacts(now);
  gestures(now);
}

void PianoTouch::stats(uint64_t now)
{
  const uint64_t span = now - fLastStatsMs ? now - fLastStatsMs : 1;
  char types[96];
  size_t used = 0;
  types[0] = 0;
  for (unsigned k = 0; k < 8 && fTypeCount[k]; ++k) append(types, sizeof types, used, "%s%u:%u", used ? " " : "", fTypes[k], fTypeCount[k]);
  note("frames at %llu s: %llu reads/s, %llu edges/s; edges %u, reads %u (data %u: checksum ok %u, bad %u, inconsistent %u; handshake %u, "
       "watchdog %u), read errors %u, counter gaps %u; types {%s}; read %llu us avg, %u max; matrix %s, decoded %u, peak %d; contacts %u",
       (unsigned long long)((now - fStartMs) / 1000), (unsigned long long)(fReadsSinceStats * 1000ull / span),
       (unsigned long long)(fEdgesSinceStats * 1000ull / span), fEdges, fReads, fData, fGood, fBad, fInconsistent, fHand, fWdt,
       fReadErrors, fGaps, types, (unsigned long long)(fReads ? fReadUsSum / fReads : 0), fReadUsMax,
       fMatrix->haveReference ? "referenced" : fMatrix->refType >= 0 ? "learning the reference" : "learning the type", fMatrix->frames,
       fMatrix->maxDelta, fTracker.downs);
  // The pace's evidence (TouchPace.h): reads no edge started (by the line's level, by the timeout), the line now, the watchdog.
  note("frames: reads %u: %u on an edge, %u on the line still high, %u without either; gpio162 now %u; watchdog frames %u, downloads "
       "for it %u", fReads, fReads - fLevelReads - fBlindReads, fLevelReads, fBlindReads, fIrqArmed ? unsigned(level(kIrq)) : 9u, fWdt,
       fWatchdog.downloads);
  fLastStatsMs = now;
  fReadsSinceStats = fEdgesSinceStats = 0;
  // The gestures, when there were new ones.
  const Gesture &g = fGesture;
  const uint32_t made = g.taps + g.rights + g.drags + g.scrolls + g.pinches + g.cancels + fTracker.palms;
  if (made != fLastGestureStats) {
    fLastGestureStats = made;
    note("gestures at %llu s: %u taps, %u right clicks, %u drags, %u scrolls, %u pinches, %u cancelled; contacts %u (%u palms, %u palm "
         "points, %u without a slot), %u merged peaks; %u reports sent, %u failed; CPU latency holds %u",
         (unsigned long long)((now - fStartMs) / 1000), g.taps, g.rights, g.drags, g.scrolls, g.pinches, g.cancels, fTracker.downs,
         fTracker.palms, fTracker.palmPoints, fTracker.noSlot, fMatrix->merged, fSent, fSendErrors, fBurst.takes);
  }
  setProperty("piano-touch-frames", uint64_t(fGood), 32);
}

void PianoTouch::serve()
{
  MatrixConfig mc;
  mc.threshold = int32_t(gArgs.threshold);
  const Calibration *cal = kUnitCalibrationPresent && calibrationValid(kUnitCalibration) ? &kUnitCalibration : nullptr;
  mc.cal = cal;
  const Orientation o = chooseOrientation(fPanel, cal, gArgs.flip, kUnitInvertYOverride);
  mc.flipX = o.flipX;
  mc.flipY = o.flipY;
  if (o.mismatch)
    note("WARNING: lcd-id gpio100 says %s but the calibration's project_vendor_id %d is %s: orientation follows the calibration, "
         "the firmware image gpio100", panelName(fPanel), cal->projectVendorId, panelName(o.calibrationPanel));
  note("orientation: flip X %u, Y %u (%s; panel %s by %s), position %s", o.flipX, o.flipY, o.why, panelName(o.panel),
       o.calibrationPanel != kPanelUnknown ? "the calibration" : fPanel != kPanelUnknown ? "gpio100" : "default",
       cal ? "from the unit's electrode maps and edge mapping" : "linear over the matrix");
  matrixReset(*fMatrix, mc);
  trackerReset(fTracker, trackConfig());
  gestureReset(fGesture, gestureConfig());
  const uint64_t begin = nowMs();
  fLastReadMs = fLastStatsMs = fPrevReadMs = begin;
  if (!fStartedMs) fQuiet.started(fStartedMs = begin);
  // The GPIO transport reads at most 10 frames a second (each takes ~45 ms of a CPU); GENI one per announced frame.
  const uint64_t minGapMs = fBus == BusKind::gpio ? 100 : 0;
  // Paced over GENI only (SPI by hand reads at most 10 frames a second anyway).
  const bool paced = gArgs.idleMs && fBus == BusKind::geni;
  TouchPace pace = touchPace(paced ? gArgs.idleMs : 0, begin, gArgs.noEdgeMs);
  bool rest = false;   // the pace's state after the last pass
  pianoirq::WakeStats wakes = pianoirq::wakeStats(pianoirq::wakeLogMs());
  uint32_t readsAtWindow = 0, sleepsAtWindow = 0;
  note("frames: serving over %s, %s", fBus == BusKind::geni ? "GENI" : "SPI by hand",
       gArgs.level >= 2 ? "the gestures go to macOS" : "contacts and gestures are logged only (piano-touch=2 gives them to macOS)");
  if (!pace.blindMs)
    note("frames: a frame read only when the controller announces it (a gpio162 edge, or the line still high after %u ms), none without "
         "one: busy at once (the latch every %u ms), at rest within %u ms, with the display off within %u ms; a contact without frames "
         "for %u ms released", kNoEdgeMinMs, gArgs.pollMs, pace.idleMs ? pace.idleMs : gArgs.pollMs, kMutedReadMs, kStallMs);
  else
    note("frames: piano-touch-noedge=%u: also read without an announced frame (busy %u ms after the last read, at rest %u ms, with the "
         "display off %u ms), the rules before 2026-11-12 (A/B only)", pace.blindMs, pace.noEdgeMs(false), pace.noEdgeMs(true), kMutedReadMs);
  fPower.followDisplay = gArgs.display != 0;
  __atomic_store_n(&fServing, true, __ATOMIC_RELEASE);
  for (;;) {
    const uint64_t now = nowMs();
    // The power states (TouchPower.h): the system's (setPowerState; a suspend parks the thread in powerStep) and the display's.
    powerStep(fPower.step(__atomic_load_n(&fSystemOff, __ATOMIC_ACQUIRE), displayState(now)), mc, now);
    if (fFramesStopped) {   // a controller that did not come back after a resume: no reads until the next one
      const uint32_t seen = fWake.now();
      if (!__atomic_load_n(&fSystemOff, __ATOMIC_ACQUIRE)) fWake.wait(seen, 1000);
      continue;
    }
    // gpio162's latch (TouchPace.h): an edge is a frame the controller announced and waits with; it is taken here, never dropped.
    if (fIrqArmed && (rd32(pinVa(kIrq) + kIntrStatus) & 1)) {
      wr32(pinVa(kIrq) + kIntrStatus, 0);
      dsb();
      if (!fEdgeSeen) note("frames: first gpio162 edge after %llu ms", (unsigned long long)(now - begin));
      if (fStart.edge() && (fStart.edges <= 10 || fStart.edges % 20 == 0))
        note("frames: the first edge %llu ms after start %u", (unsigned long long)(now - fStart.sinceMs), fStart.starts);
      fEdgeSeen = fEdgePending = true;
      fQuiet.edge(now);
      ++fEdges;
      ++fEdgesSinceStats;
    }
    const bool muted = fPower.muted();
    const uint64_t since = now - fLastReadMs;
    // A frame announced without an edge we saw: the line still high two cycles after the last read started.
    const bool levelPending = !fEdgePending && fIrqArmed && since >= kNoEdgeMinMs && level(kIrq);
    ReadCue cue{fEdgePending, levelPending, fIrqArmed, muted, rest, false, since};
    const uint32_t expiredBefore = fStart.expired;
    cue.grace = fStart.holds(now);
    ReadOn on;
    if (fBus == BusKind::gpio)
      on = since < minGapMs ? ReadOn::none : fEdgePending ? ReadOn::edge : levelPending ? ReadOn::level
           : (!fIrqArmed || pace.blindMs) && since >= 100 ? ReadOn::blind : ReadOn::none;
    else
      on = readOn(pace, cue);
    if (pace.blindMs && fStart.expired != expiredBefore && (fStart.expired <= 5 || fStart.expired % 20 == 0))
      note("frames: no edge within %u ms of start %u: reading without one again (%u times; piano-touch-noedge)", kStartGraceMs,
           fStart.starts, fStart.expired);
    if (on != ReadOn::none) {
      if (on == ReadOn::level) ++fLevelReads;
      else if (on == ReadOn::blind) ++fBlindReads;
      fEdgePending = false;
      fPrevReadOn = fReadOn;
      fReadOn = on;
      fPrevReadMs = fLastReadMs;
      fLastReadMs = now;
      ++fReads;
      ++fReadsSinceStats;
      const uint64_t t0 = nowUs();
      const uint32_t badBefore = fBad + fInconsistent;
      const int rc = readFrame(fChip, fFrame);
      const uint32_t us = uint32_t(nowUs() - t0);
      fReadUsSum += us;
      if (us > fReadUsMax) fReadUsMax = us;
      if (rc) {
        ++fReadErrors;
        lost(nowMs());
        if (++fConsecutive >= 50) {
          burst(nowMs(), false, true);
          stage(-7, "frames: 50 frame reads in a row failed");
          return;
        }
      } else {
        fConsecutive = 0;
        frame(us);
      }
      // A read on an edge the interrupt reported: a CPU that left a deep idle state late may have started it mid-frame (bad
      // checksum); then the next idle edge is polled for.
      if (fEdgeByIrq) {
        ++fIrqReads;
        fPollNextEdge = rc || fBad + fInconsistent != badBefore;
        if (fPollNextEdge) ++fIrqBadReads;
        fEdgeByIrq = false;
      } else {
        fPollNextEdge = false;
      }
      // The controller's watchdog (TouchPace.h WatchdogRecovery): the download again, at once or every 10 s while it comes back,
      // never given up; the display off only counts it (its blocks are read as it announces them; the return downloads if needed).
      if (fWdtRun >= kWatchdogRun && !muted) {
        if (fWatchdog.due(nowMs())) {
          lost(nowMs());
          // As the vendor flow: a controller that reports 0xfe is idled first (swrst <- 0xaa, 15 ms), then the download again.
          if (fWdtState == 0xfe) {
            writeByte(fChip, kSwrst, 0xaa);
            IOSleep(15);
          }
          const bool soon = fWatchdog.downloads && nowMs() - fWatchdog.lastMs < 2 * kWatchdogRetryMs;
          if (fWatchdog.downloads < 10 || fWatchdog.downloads % 10 == 0)
            note("frames: watchdog (state %#04x, %u frames in a row): the download again (%u for the watchdog%s)", fWdtState, fWdtRun,
                 fWatchdog.downloads + 1, soon ? "; it came back: every 10 s from now on, never given up" : "");
          fWatchdog.downloaded(nowMs());
          fWdtRun = 0;
          fWdtWaitNoted = false;
          if (firmware() && startScanning()) {   // firmware() empties the latch: the new firmware's first frame comes next
            matrixReset(*fMatrix, mc);
            fStart.started(nowMs());
          } else {
            note("frames: the download for the watchdog failed: again in %u s", kWatchdogRetryMs / 1000);
          }
          fLastReadMs = nowMs();
        } else if (!fWdtWaitNoted) {
          fWdtWaitNoted = true;
          note("frames: watchdog (state %#04x) again %llu ms after its download: the next download in %llu ms", fWdtState,
               (unsigned long long)(nowMs() - fWatchdog.lastMs), (unsigned long long)(fWatchdog.lastMs + kWatchdogRetryMs - nowMs()));
        }
      }
    }
    const uint64_t pass = nowMs();
    // The controller quiet (TouchPace.h Quiet): it idles. Under the default rules it is then read only when it announces a frame;
    // a reference still being learned is formed from the frames so far (TouchMatrix.h matrixSettle).
    if (fIrqArmed && fQuiet.step(pass) && (fQuiet.quiets <= 10 || fQuiet.quiets % 50 == 0))
      note("frames: the controller quiet (no edge for %u ms) %u: %u frames since the last start %llu ms ago%s", kQuietMs, fQuiet.quiets,
           fFramesSinceStart, (unsigned long long)(pass - fStartedMs), pace.blindMs ? "" : "; read only when it announces a frame");
    if (fQuiet.quiet && !fMatrix->haveReference && matrixSettle(*fMatrix))
      note("matrix: reference of %u frames of type %d (%u x %u), formed once the controller went quiet (TouchMatrix.h matrixSettle)",
           fMatrix->refCount, fMatrix->refType, fMatrix->rows, fMatrix->cols);
    // A contact or a gesture without frames (TouchPace.h kStallMs): the controller idles only when nobody touches it, so its
    // silence is taken as the lift (an empty frame: a short touch still clicks); what is still going on after it is released.
    if (stalled(trackerLive(fTracker) || !gestureIdle(fGesture), pass, fLastReadMs)) {
      ++fStalls;
      if (fStalls <= 20 || fStalls % 20 == 0)
        note("frames: no frame for %llu ms with a contact or a gesture on (%u times): taken as the lift", (unsigned long long)(pass - fLastReadMs),
             fStalls);
      trackerUpdate(fTracker, nullptr, 0, pass);
      contacts(pass);
      gestures(pass);
      if (trackerLive(fTracker) || !gestureIdle(fGesture)) lost(pass);
    }
    const uint64_t age = now - begin;
    if (now - fLastStatsMs >= (age < 30000 ? 5000 : 60000)) stats(now);
    pianoirq::WakeStats::Window w;
    if (wakes.due(now, &w)) {
      note("idle: %u wakeups in %u s, irq %u (%u frame reads; %u idle sleeps; %u reads on an interrupt's edge, %u of them bad; %u "
           "missed; pace %s)", w.wakeups, w.seconds, w.irqs, fReads - readsAtWindow, fIdleSleeps - sleepsAtWindow, fIrqReads, fIrqBadReads,
           fMiss.total, muted ? "display off" : paced ? (pace.idle(nowMs(), false) ? "idle" : "every frame") : "every frame (not paced)");
      readsAtWindow = fReads;
      sleepsAtWindow = fIdleSleeps;
    }
    if (muted) {   // the latch looked at once per kMutedReadMs; the display's state is read every 250 ms
      const uint64_t gone = nowMs() - fLastReadMs;
      const uint32_t wait = gone >= kMutedReadMs ? kMutedReadMs : uint32_t(kMutedReadMs - gone);
      const uint32_t seen = fWake.now();
      if (!__atomic_load_n(&fSystemOff, __ATOMIC_ACQUIRE)) fWake.wait(seen, wait);
      wakes.count(pianoirq::Woke::timeout);
      continue;
    }
    // The pace (TouchPace.h): every announced frame at once while the panel is busy, within an idle period at rest. The decoder
    // learning counts as busy only until the controller is quiet: then it announces a frame every ~1.5 s.
    const bool learning = !fMatrix->haveReference && !(fIrqArmed && fQuiet.quiet);
    const bool busy = trackerLive(fTracker) || !gestureIdle(fGesture) || learning || fWdtRun;
    burst(nowMs(), busy);
    rest = paced && pace.idle(nowMs(), busy);
    if (rest && !fEdgePending) {
      const bool irqWait = fEdgeSeen && fIrqLive && fPin.live() && !fPollNextEdge;
      const uint32_t sleep = pace.sleepMs(nowMs(), fLastReadMs);
      if (sleep || (!pace.blindMs && !irqWait)) {
        // Before the idle period's end; or after it without the interrupt: the latch looked at again in one idle period.
        const uint32_t seen = fWake.now();   // a power change (setPowerState) ends the sleep at once
        if (!__atomic_load_n(&fSystemOff, __ATOMIC_ACQUIRE)) fWake.wait(seen, sleep ? sleep : pace.idleMs);
        wakes.count(pianoirq::Woke::timeout);
        ++fIdleSleeps;
        continue;   // the latch, set during the sleep (the frame waits), is taken at the top
      }
      if (irqWait) {
        const uint32_t seen = fPin.gen();
        fPin.arm();   // the handler masks it again when the edge comes
        if (fPin.wait(seen, pace.blindMs ? 20 : pace.idleMs)) {
          wakes.count(pianoirq::Woke::irq);
          ++fIrqEdges;
          fMiss.delivered();
          fEdgeByIrq = fPin.live();
        } else {
          wakes.count(pianoirq::Woke::timeout);
          fPin.mask();
          if (fPin.latched() && fMiss.miss()) {
            fIrqLive = false;
            note("frames: gpio162 latched but no interrupt in 3 waits in a row: idle edges by a look per idle period from now on");
          }
        }
        if (fIrqLive && !fPin.live()) {
          fIrqLive = false;
          note("frames: the TLMM summary line went away (state %u): idle edges by a look per idle period from now on", fPin.s->state);
        }
        continue;
      }
    }
    IOSleep(gArgs.pollMs);
    wakes.count(pianoirq::Woke::timeout);
  }
}
