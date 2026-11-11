// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoKeyboard: the official Xiaomi Pad 8 Pro keyboard cover (pogo pins; keyboard, consumer keys, touchpad) for macOS 27 on piano
// (SM8750), native at EL1. The cover talks to a Nanosic WN8030 MCU on the tablet (Wn8030Piano.h): I2C client 0x4c on i2c@a98000
// (QUP1 SE6, pins gpio56 SDA / gpio57 SCL), interrupt/data gpio97 (rising edge per frame), reset gpio188 and sleep gpio3 (both active
// low), status gpio95, supplies vreg_l11b_1p8 (vddio) and vreg_l14b_3p3 (dvdd) (boot DT keyboard@4c, nanosic,wn8030-piano). The MCU
// runs from RAM: its boot ROM takes MCU_Upgrade.bin (the stock 303 image, staged by tooling/k27/build-native-piano-keyboard.sh, never
// committed) at every power-up.
//
// One kernel thread does everything, in steps (each logs "PianoKeyboard: step N ..." before it touches anything):
//   1 survey     read only: the six pins' TLMM registers and levels, the hall sensor (gpio80), the two rails' cmd-db addresses,
//                the apps RSC id
//   2 pins       reset and sleep driven low (asserted); the bus pins, gpio97 and gpio95 GPIO inputs
//   3 rails      RPMh active votes through the apps RSC (DRV 2, an idle active TCS): L11B 1.8 V, then L14B 3.3 V (mode, mV, enable),
//                each with PianoA830's DRV-2 owner word held (tag 'KBRD'; a wait for another owner is logged)
//   4 power-on   the bus released to its pull-ups, reset released after 2 ms, 500 ms for the boot ROM, sleep released, 50 ms;
//                boot ROM id 0xc8 in state 0
//   5 firmware   the image's header (state 1), its 101 data blocks (3 ms after each write), verify (state 3), boot
//   6 serving    gpio97's rising edges latched in TLMM (no GIC interrupt: polled every piano-keyboard-poll ms), each frame read
//                from register 0x4c and split into reports; attach messages answered as Linux does (keyboard power, Caps LED,
//                touchpad on, mic LED, backlight: Wn8030Piano.h attachCommands), the keyboard power repeated every 12 s while
//                attached; the hall sensor polled every 100 ms. A bring-up that stops early (a failed step, piano-keyboard-stop)
//                still polls the hall sensor.
// The I2C bus is driven in software (BitI2c.h); the QUP engine is not touched.
//
// The hall sensor: the folio's lid switch on TLMM gpio80 (boot DT gpio-keys switch-lid, SW_LID, active low: low = cover closed),
// separate from the MCU's attach bit. Read only (its CTL is never written): logged at start and on each change ("hall: cover
// closed/open"), two equal 100 ms samples make a change. macOS is told nothing unless piano-keyboard-lid=1.
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-keyboard=1            steps 1-6; every report is logged raw ("frame"/"key"/"touch" lines), nothing goes to macOS
//   piano-keyboard=2            the same, and two HID devices (HidDescriptors.h): the keyboard (+ consumer keys) and the touchpad as
//                               a relative pointer; key contents are then NOT logged (the login password), only counted
//   piano-keyboard-stop=N       stop before step N (2: the read-only survey alone)
//   piano-keyboard-trace=1      level 2 logs report contents too
//   piano-keyboard-halfbit=US   I2C half-bit time (default 2 us; 1..50)        piano-keyboard-poll=MS  edge polling (default 4)
//   piano-keyboard-rails=0      no RPMh votes (the rails as the boot stages left them)
//   piano-keyboard-touchpad=V   the touchpad command's value on attach (default 1; 2: not sent)
//   piano-keyboard-backlight=P  backlight 0..100 sent on attach (default 50: a visible sign that commands reach the cover; >100 none)
//   piano-keyboard-auth=1       answer the cover's authentication as Linux does with its helper open (init, offline s3t1; the
//                               final token needs the helper's secret: not sent). Default 0, as piano's Ubuntu runs
//   piano-keyboard-raw=0        level 1 without the raw dump of every frame that is not an accelerometer sample
//   piano-keyboard-tap=0        no tap-to-click        piano-keyboard-invert=B  bit 0 X, bit 1 Y        piano-keyboard-divide=D (2)
//   piano-keyboard-mouse=0      ignore the MCU's own mouse reports (ID 2; used only while no touchpad report has been seen)
//   piano-keyboard-fnrow=1      Right Ctrl + 1..0 - = are F1..F12 (Wn8030Piano.h Keys; default 0: keys as the cover sends them)
//   piano-keyboard-hall=0       do not read gpio80 at all
//   piano-keyboard-lid=1        tell IOPMrootDomain the hall state (kIOPMClamshellClosed/Opened), with clamshell sleep disabled
//                               first (kIOPMDisableClamshell); =2 the same with clamshell sleep allowed (kIOPMEnableClamshell).
//                               Default 0: nothing is sent (native sleep is untested and could hang)
// Not done: the hall sensor's effect on input (Linux turns the touchpad, LEDs and reports off while the cover is closed), the
// authentication token, the backlight/mic LED beyond the boot arguments, suspend.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "Wn8030Piano.h"
#include "BitI2c.h"
#include "HidDescriptors.h"
#include "CmdDb.h"

using namespace pianokeyboard;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last step reached, negative when it failed there.
volatile int32_t piano_keyboard_stage = 0;
// The MCU image, from out/native-kexts/PianoKeyboard/gen/PianoKeyboardFirmware.c (size 0 when built without it).
extern const uint8_t piano_keyboard_firmware[];
extern const uint64_t piano_keyboard_firmware_size;
}

namespace {

struct Args {
  uint32_t level, stop, trace, halfUs, pollMs, rails, touchpad, backlight, tap, invert, divide, mouse, auth, raw, fnrow, hall, lid;
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
  gArgs.level = bootArg("piano-keyboard", 0);
  gArgs.stop = bootArg("piano-keyboard-stop", 0);
  gArgs.trace = bootArg("piano-keyboard-trace", 0);
  gArgs.halfUs = clampArg(bootArg("piano-keyboard-halfbit", 2), 1, 50);
  gArgs.pollMs = clampArg(bootArg("piano-keyboard-poll", 4), 1, 50);
  gArgs.rails = bootArg("piano-keyboard-rails", 1);
  gArgs.touchpad = bootArg("piano-keyboard-touchpad", 1);
  gArgs.backlight = bootArg("piano-keyboard-backlight", 50);
  gArgs.tap = bootArg("piano-keyboard-tap", 1);
  gArgs.invert = bootArg("piano-keyboard-invert", 0);
  gArgs.divide = clampArg(bootArg("piano-keyboard-divide", 2), 1, 16);
  gArgs.mouse = bootArg("piano-keyboard-mouse", 1);
  gArgs.auth = bootArg("piano-keyboard-auth", 0);
  gArgs.raw = bootArg("piano-keyboard-raw", 1);
  gArgs.fnrow = bootArg("piano-keyboard-fnrow", 0);
  gArgs.hall = bootArg("piano-keyboard-hall", 1);
  gArgs.lid = bootArg("piano-keyboard-lid", 0);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoKeyboard: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_keyboard_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at step" : "step", value < 0 ? -value : value, what);
}

const char *hex(const uint8_t *p, unsigned n, char *out, size_t size)
{
  size_t used = 0;
  out[0] = 0;
  for (unsigned i = 0; i < n && used + 3 < size; ++i) used += size_t(snprintf(out + used, size - used, "%s%02x", i ? " " : "", p[i]));
  return out;
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr/str, as PianoDwc3 does; a barrier after each store, so a bus line has moved before the half-bit wait starts.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n dsb sy" ::"r"(v), "r"(a) : "memory"); }

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
void unmapRegion(Region &r)
{
  OSSafeReleaseNULL(r.map);
  r.va = 0;
}

// ------------------------------------------------------------------------------------------------------------- the pins (TLMM)
// TLMM (DT pinctrl@f100000, qcom,sm8750-tlmm): pin n at n * 0x1000: CTL (pull 1:0 (1 down, 3 up), function 5:2, drive 8:6, output
// enable 9, eGPIO present 11 / enable 12), IO (bit 0 in, bit 1 out), INTR_CFG (bit 0 enable, 1 polarity, 3:2 detection (1 edge),
// 4 raw status, 10:8 target), INTR_STATUS (bit 0, cleared with 0). An eGPIO-capable pin is claimed for this processor (bit 12), as
// Linux does when it selects a function (Project-SunUEFI PianoPogoBoard.c, pinctrl-sm8750 as its spec).
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kCtl = 0x0, kIo = 0x4, kIntrCfg = 0x8, kIntrStatus = 0xc;
constexpr uint32_t kPullDown = 1, kPullUp = 3, kOe = 1u << 9, kEgpioPresent = 1u << 11, kEgpioEnable = 1u << 12;

enum PinIndex : unsigned { kSda, kScl, kReset, kSleep, kIrq, kStatus, kPins };
struct Pin {
  const char *name;
  unsigned gpio;
  Region page;
  uint32_t ctl, io, intrCfg;   // as found
};
Pin gPin[kPins] = {
  {"SDA", 56, {}, 0, 0, 0}, {"SCL", 57, {}, 0, 0, 0}, {"reset", 188, {}, 0, 0, 0},
  {"sleep", 3, {}, 0, 0, 0}, {"irq", 97, {}, 0, 0, 0}, {"status", 95, {}, 0, 0, 0},
};
// The hall sensor, read only: never configured, only its CTL/IO/INTR_CFG read.
Pin gHall = {"hall", 80, {}, 0, 0, 0};
uintptr_t pinVa(PinIndex p) { return gPin[p].page.va; }
uint32_t pinCfg(PinIndex p, uint32_t pull, bool output)
{
  const uint32_t found = gPin[p].ctl;
  return (found & ~(0x3ffu | kEgpioEnable)) | (found & kEgpioPresent ? kEgpioEnable : 0) | pull | (output ? kOe : 0);
}
bool level(PinIndex p) { return rd32(pinVa(p) + kIo) & 1; }
void drive(PinIndex p, bool high) { wr32(pinVa(p) + kIo, high ? 2u : 0u); }

// The bus lines: released = input with the pull-up, low = output enabled with the output value 0 (set once in step 2).
void lineSda(void *, bool release) { wr32(pinVa(kSda) + kCtl, pinCfg(kSda, kPullUp, !release)); }
void lineScl(void *, bool release) { wr32(pinVa(kScl) + kCtl, pinCfg(kScl, kPullUp, !release)); }
bool readSda(void *) { return level(kSda); }
bool readScl(void *) { return level(kScl); }
void delayUs(void *, unsigned us) { IODelay(us); }

// --------------------------------------------------------------------------------------------------------------- RPMh (rails)
// apps_rsc (DT rsc@16500000, qcom,drv-id 2): this processor's DRV at 0x16520000, TCS block +0xd00, active TCSs 0-2; cmd-db at
// 0x81c60000 + 0x20000 (package/PianoPower/PianoAdsp.cpp, hardware-verified there). One active-only request as PianoAdsp's vote():
// an idle active TCS (DRV_STATUS non-zero; TCS 2 first, since PianoAdsp starts at 1 and PianoA830 at 0), its commands with the
// response bit, WAIT_FOR_CMPL, CMD_ENABLE, the TCS's IRQ_ENABLE bit, CONTROL trigger; completion polled for at most 100 ms (IRQ_STATUS
// and each command's status bit 16), the TCS reclaimed, IRQ_ENABLE as it was. A TCS that did not complete is never used again.
constexpr uint64_t kRscDrv = 0x16520000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24, kMsgidWriteResp = 8u | 1u << 8 | 1u << 16, kCmdCompleted = 1u << 16;
Region gRsc;
RscLayout gLayout;
bool gTcsStuck[3];
struct Rail {
  const char *name, *what;
  uint32_t mv, address;
};
Rail gRails[2] = {{"ldob11", "vreg_l11b_1p8 (vddio)", 1800, 0}, {"ldob14", "vreg_l14b_3p3 (dvdd)", 3296, 0}};

uint32_t rscRd(uint32_t at) { return rd32(gRsc.va + kRscTcs + at); }
void rscWr(uint32_t at, uint32_t v) { wr32(gRsc.va + kRscTcs + at, v); }

// The DRV-2 owner word of PianoA830 (package/PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h, "DRV-2 lock"): one 32-bit word,
// 0 = free, any other value = its owner's tag. PianoA830 holds it around its own TCS transactions (tag 'A830'), PianoPower around
// its votes ('POWR'), so piano-gpu=2, piano-power>=2 and this driver never pick and program a TCS of DRV-2 (or its shared
// IRQ_ENABLE) at once. PianoA830 is in configs/k27-native-kexts.txt and PianoKeyboard comes after the list (PIANO_EXTRA_KEXTS), so
// the KC linker (no weak imports) resolves this to A830's single copy; PianoKeyboard.json names PianoA830 as a library. The protocol
// is A830's: compare-exchange 0 for our tag, 10 us between tries, at most 2 s, store 0 to release.
extern "C" volatile uint32_t sm8750_rpmh_drv2_owner;
constexpr uint32_t kDrv2Tag = 0x4b425244;   // "KBRD"
constexpr unsigned kDrv2Steps = 200000, kDrv2StepUs = 10;

// The owner tag as four characters when it is printable ('A830', 'POWR'), else "?".
const char *tagName(uint32_t tag, char out[5])
{
  for (unsigned i = 0; i < 4; ++i) {
    const char c = char(tag >> (24 - 8 * i));
    if (c < 0x20 || c > 0x7e) return "?";
    out[i] = c;
  }
  out[4] = 0;
  return out;
}

// Takes the lock; *waits gets the 10 us waits. false: someone held it for 2 s (then nothing is programmed).
bool drv2Acquire(uint32_t *waits)
{
  for (uint32_t n = 0;; ++n) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&sm8750_rpmh_drv2_owner, &expected, kDrv2Tag, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *waits = n;
      return true;
    }
    if (n == kDrv2Steps) {
      *waits = n;
      return false;
    }
    IODelay(kDrv2StepUs);
  }
}
void drv2Release() { __atomic_store_n(&sm8750_rpmh_drv2_owner, 0, __ATOMIC_RELEASE); }

bool readRails()
{
  Region db = {};
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
  bool ok = copy && mapRegion(db, kCmdDb, kCmdDbSize);
  if (ok) {
    for (uint32_t i = 0; i < kCmdDbSize; i += 4) {
      const uint32_t w = rd32(db.va + i);
      memcpy(copy + i, &w, 4);
    }
    for (Rail &r : gRails) {
      const int rc = cmdDbFind(copy, kCmdDbSize, r.name, &r.address);
      note("cmd-db: %s (%s) %s %#x", r.name, r.what, rc ? "NOT FOUND, rc" : "at", rc ? unsigned(-rc) : r.address);
      ok = ok && !rc;
    }
  } else {
    note("cmd-db: cannot map %#llx or allocate its copy", (unsigned long long)kCmdDb);
  }
  unmapRegion(db);
  if (copy) IOFreeData(copy, kCmdDbSize);
  return ok;
}

bool voteLocked(const Rail &r)
{
  const RscLayout &l = gLayout;
  static const unsigned kOrder[3] = {2, 1, 0};
  int tcs = -1;
  for (unsigned k = 0; k < 3 && tcs < 0; ++k)
    if (!gTcsStuck[kOrder[k]] && rscRd(l.tcsStride * kOrder[k] + l.status)) tcs = int(kOrder[k]);
  if (tcs < 0) {
    note("rails: %s: no idle active TCS, no vote", r.name);
    return false;
  }
  const uint32_t base = l.tcsStride * unsigned(tcs), bit = 1u << tcs, irqEnable = rscRd(0);
  if (rscRd(l.irqStatus) & bit) rscWr(l.irqClear, bit);
  const uint32_t addr[3] = {r.address + 8, r.address, r.address + 4}, data[3] = {7, r.mv, 1};   // mode HPM, mV, enable
  for (unsigned c = 0; c < 3; ++c) {
    const uint32_t cmd = base + l.cmdStride * c;
    rscWr(cmd + l.cmdMsgid, kMsgidWriteResp);
    rscWr(cmd + l.cmdAddr, addr[c]);
    rscWr(cmd + l.cmdData, data[c]);
  }
  rscWr(base + l.waitForCmpl, 7);
  rscWr(base + l.cmdEnable, 7);
  rscWr(0, irqEnable | bit);
  const uint32_t control = rscRd(base + l.control);
  rscWr(base + l.control, control & ~kAmcTrigger);
  rscWr(base + l.control, control & ~(kAmcTrigger | kAmcEnable));
  rscWr(base + l.control, kAmcEnable);
  rscWr(base + l.control, kAmcEnable | kAmcTrigger);
  bool done = false;
  unsigned waited = 0;
  uint32_t s[3] = {};
  for (;; ++waited) {
    for (unsigned c = 0; c < 3; ++c) s[c] = rscRd(base + l.cmdStride * c + l.cmdData + 4);
    done = (rscRd(l.irqStatus) & bit) || ((s[0] & s[1] & s[2]) & kCmdCompleted);
    if (done || waited >= 100) break;
    IOSleep(1);
  }
  if (done) {
    rscWr(base + l.cmdEnable, 0);
    rscWr(l.irqClear, bit);
    rscWr(base + l.control, rscRd(base + l.control) & ~kAmcTrigger);
    rscWr(base + l.control, 0);
  } else {
    gTcsStuck[tcs] = true;
  }
  rscWr(0, irqEnable);
  note("rails: %s (%s) %u mV, mode 7, enable: TCS %d %s after %u ms (cmd status %#x %#x %#x)", r.name, r.what, r.mv, tcs,
       done ? "completed" : "NOT completed (left in flight, not used again)", waited, s[0], s[1], s[2]);
  return done;
}

// Every vote runs with the DRV-2 owner word held: the idle TCS is picked, programmed, triggered and reclaimed inside it.
bool vote(const Rail &r)
{
  char tag[5];
  uint32_t waits = 0;
  const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
  if (held) note("rails: %s: the DRV-2 lock is held by %#x ('%s'); waiting (at most 2 s)", r.name, held, tagName(held, tag));
  if (!drv2Acquire(&waits)) {
    const uint32_t owner = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
    note("rails: %s: the DRV-2 lock stayed held by %#x ('%s') for 2 s: no vote sent", r.name, owner, tagName(owner, tag));
    return false;
  }
  note("rails: %s: DRV-2 lock taken (tag 'KBRD') after %u waits of %u us", r.name, waits, kDrv2StepUs);
  const bool done = voteLocked(r);
  drv2Release();
  return done;
}

}  // namespace

// --------------------------------------------------------------------------------------------------------------- the HID devices
// DroidVMInput's DroidVMHIDDevice (as package/PianoInput's PianoHIDDevice, UPSTREAM) with what a device says about itself taken from
// HidDescriptors.h; setReport takes the keyboard's LED output report (Caps Lock goes to the MCU).
class PianoKeyboardHID : public IOHIDDevice {
  OSDeclareDefaultStructors(PianoKeyboardHID)

public:
  Kind fKind = Kind::keyboard;
  void (*fLeds)(void *, uint8_t) = nullptr;
  void *fLedsContext = nullptr;

  IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
  OSString *newTransportString() const override { return OSString::withCString("I2C"); }
  OSString *newManufacturerString() const override { return OSString::withCString("UniXNU"); }
  OSString *newProductString() const override { return OSString::withCString(deviceInfo(fKind).product); }
  OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(kVendorId, 32); }
  OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(kProductId, 32); }
  OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x504b0000u | unsigned(fKind), 32); }
  OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(deviceInfo(fKind).usagePage, 32); }
  OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(deviceInfo(fKind).usage, 32); }
  IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType reportType, IOOptionBits options) override;
};

OSDefineMetaClassAndStructors(PianoKeyboardHID, IOHIDDevice)

IOReturn PianoKeyboardHID::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
  const DeviceInfo &info = deviceInfo(fKind);
  *descriptor = IOBufferMemoryDescriptor::withBytes(info.descriptor, info.descriptorBytes, kIODirectionNone);
  return *descriptor ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn PianoKeyboardHID::setReport(IOMemoryDescriptor *report, IOHIDReportType reportType, IOOptionBits options)
{
  if (!report || reportType != kIOHIDReportTypeOutput || fKind != Kind::keyboard) return kIOReturnUnsupported;
  uint8_t bytes[2] = {0, 0};
  const IOByteCount length = report->readBytes(0, bytes, sizeof bytes);
  uint8_t leds;
  if (length >= 2 && bytes[0] == kIdKeyboard) leds = bytes[1];                         // the report with its ID
  else if (length == 1 && (options & 0xff) == kIdKeyboard) leds = bytes[0];            // the LED byte alone
  else return kIOReturnUnsupported;
  if (fLeds) fLeds(fLedsContext, leds);
  return kIOReturnSuccess;
}

// --------------------------------------------------------------------------------------------------------------------- the service
class PianoKeyboard : public IOService {
  OSDeclareDefaultStructors(PianoKeyboard)

  PianoKeyboardHID *fHid[2] = {nullptr, nullptr};
  IONotifier *fPublished = nullptr;
  BitI2c fBus;
  Pointer fPointer;
  Keys fKeyFilter;
  uint8_t fFrame[kFrameBytes];
  uint8_t fWire[kDataWire];
  // serving state (the thread's own)
  bool fAttached = false, fTouchSeen = false, fEdgeSeen = false, fReadAgain = false, fCaps = false, fStatusLevel = false;
  uint64_t fStartMs = 0, fLastReadMs = 0, fKeepaliveMs = 0;
  uint32_t fReads = 0, fFrames = 0, fEmpty = 0, fReadErrors = 0, fBadFrames = 0, fEdges = 0, fCommands = 0, fCommandErrors = 0;
  uint32_t fKeys = 0, fConsumer = 0, fTouch = 0, fMouse = 0, fVendor = 0, fAccel = 0, fAuth = 0, fAttaches = 0, fSent = 0, fSendErrors = 0;
  uint32_t fConsecutiveErrors = 0, fRawLogged = 0, fEmptyLogged = 0, fLevelReads = 0, fIrqHigh = 0, fPolls = 0;
  uint32_t fAcks = 0, fStates = 0, fPointerSent = 0, fSumBad = 0;
  bool fIrqWasLow = false;
  // the hall sensor (gpio80): the accepted level (1 open, 0 closed; -1 not read), the last sample, changes, the next sample time
  int fHall = -1, fHallSample = -1;
  uint32_t fHallChanges = 0;
  uint64_t fHallNextMs = 0;
  bool fLidSetup = false;
  // the LED request from IOHIDFamily's side
  volatile uint32_t fLedRequests = 0, fLedWanted = 0;
  uint32_t fLedSeen = 0;

  static void threadMain(void *self, wait_result_t);
  static bool eventServicePublished(void *target, void *refCon, IOService *service, IONotifier *notifier);
  static void ledRequest(void *self, uint8_t leds);
  bool step(int32_t n, const char *what);
  void run();
  void survey();
  bool setupPins();
  bool setupRails();
  bool powerOn();
  bool loadFirmware();
  void armIrq();
  bool publish();
  void serve();
  bool readFrame();
  void dispatch(const Report &r);
  void coverAttached(bool on, const char *why);
  bool sendCommand(uint8_t cmd, uint8_t value, const char *what);
  void authAnswer(const Report &r);
  IOReturn send(Kind kind, const uint8_t *bytes, unsigned length);
  void surveyHall();
  void pollHall(uint64_t now);
  void hallOnly();
  void lidNotify(bool closed);
  void releaseAll();
  void describe(uint64_t seconds);
  bool romWrite(uint8_t value);
  int romRead();

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PianoKeyboard, IOService)

IOService *PianoKeyboard::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoKeyboard::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-keyboard=%u (%s), stop %u, I2C half-bit %u us, poll %u ms, rails %u, touchpad cmd %u, backlight %u, auth %u, "
       "raw %u, fnrow %u, hall %u, lid %u, firmware %llu bytes",
       gArgs.level, gArgs.level >= 2 ? "HID devices for macOS" : "raw reports logged, nothing for macOS", gArgs.stop, gArgs.halfUs,
       gArgs.pollMs, gArgs.rails, gArgs.touchpad, gArgs.backlight, gArgs.auth, gArgs.raw, gArgs.fnrow, gArgs.hall, gArgs.lid,
       (unsigned long long)piano_keyboard_firmware_size);
  setProperty("piano-keyboard", gArgs.level, 32);
  PointerConfig pc;
  pc.divide = int32_t(gArgs.divide);
  pc.tap = gArgs.tap != 0;
  pc.invertX = gArgs.invert & 1;
  pc.invertY = gArgs.invert & 2;
  fPointer.configure(pc);
  fKeyFilter.fnRow = gArgs.fnrow != 0;
  thread_t thread = nullptr;
  if (kernel_thread_start(threadMain, this, &thread) != KERN_SUCCESS) {
    stage(-1, "kernel_thread_start");
    return true;   // stays attached and does nothing more
  }
  thread_deallocate(thread);
  registerService();
  return true;
}

void PianoKeyboard::threadMain(void *self, wait_result_t)
{
  static_cast<PianoKeyboard *>(self)->run();
}

bool PianoKeyboard::step(int32_t n, const char *what)
{
  if (gArgs.stop && uint32_t(n) >= gArgs.stop) {
    note("stopped before step %d (%s): piano-keyboard-stop=%u", n, what, gArgs.stop);
    return false;
  }
  stage(n, what);
  return true;
}

void PianoKeyboard::run()
{
  if (!step(1, "survey (read only): pins, hall sensor, rails, RSC")) return;
  survey();
  const bool served = step(2, "pins: reset/sleep low (asserted), gpio56/57/97/95 GPIO inputs") && setupPins() &&
                      step(3, "rails: RPMh votes L11B 1.8 V then L14B 3.3 V") && setupRails() &&
                      step(4, "power-on: reset released, boot ROM 500 ms, sleep released 50 ms; boot ROM id and state") && powerOn() &&
                      step(5, "firmware: header, data blocks, verify, boot") && loadFirmware() &&
                      step(6, "serving: gpio97 edges latched and polled, frames read from register 0x4c");
  setProperty("piano-keyboard-stage", uint64_t(uint32_t(piano_keyboard_stage)), 32);
  if (!served) {
    hallOnly();
    return;
  }
  armIrq();
  if (gArgs.level >= 2 && !publish()) {
    hallOnly();
    return;
  }
  setProperty("piano-keyboard-stage", uint64_t(uint32_t(piano_keyboard_stage)), 32);
  serve();
}

// ------------------------------------------------------------------------------------------------------------- steps 1-5
void PianoKeyboard::survey()
{
  for (Pin &p : gPin) {
    if (!mapRegion(p.page, kTlmm + uint64_t(p.gpio) * 0x1000, 0x10)) {
      note("pins: gpio%u (%s) does not map", p.gpio, p.name);
      continue;
    }
    p.ctl = rd32(p.page.va + kCtl);
    p.io = rd32(p.page.va + kIo);
    p.intrCfg = rd32(p.page.va + kIntrCfg);
    note("pins: gpio%-3u %-6s CTL %08x (function %u, pull %u, drive %u mA, %s%s) IO %08x (in %u, out %u) INTR_CFG %08x STATUS %08x",
         p.gpio, p.name, p.ctl, (p.ctl >> 2) & 15, p.ctl & 3, 2 * (((p.ctl >> 6) & 7) + 1), p.ctl & kOe ? "output" : "input",
         p.ctl & kEgpioPresent ? (p.ctl & kEgpioEnable ? ", eGPIO ours" : ", eGPIO not ours") : "", p.io, p.io & 1, (p.io >> 1) & 1,
         p.intrCfg, rd32(p.page.va + kIntrStatus));
  }
  surveyHall();
  if (gArgs.rails && readRails()) {
    if (mapRegion(gRsc, kRscDrv, 0x10000)) {
      const uint32_t id = rd32(gRsc.va);
      gLayout = rscLayout(id);
      char line[160];
      size_t used = 0;
      for (unsigned t = 0; t < 3; ++t)
        used += size_t(snprintf(line + used, sizeof line - used, "%sTCS %u %s", t ? ", " : "", t,
                                rscRd(gLayout.tcsStride * t + gLayout.status) ? "idle" : "busy"));
      note("rsc: DRV 2 id %#x (version %u.%u), %s", id, (id >> 16) & 0xff, (id >> 8) & 0xff, line);
    } else {
      note("rsc: cannot map %#llx", (unsigned long long)kRscDrv);
    }
  }
  uint32_t code = 0;
  const int image = imageCheck(piano_keyboard_firmware, uint32_t(piano_keyboard_firmware_size), &code);
  note("firmware: %llu bytes, %s (code %u bytes, %u blocks)", (unsigned long long)piano_keyboard_firmware_size,
       image ? "NOT usable" : "image checks pass", code, (code + kDataChunk - 1) / kDataChunk);
}

// ------------------------------------------------------------------------------------------------------------ the hall sensor
// gpio80, read only. Its CTL tells whether it is a GPIO input (function 0) as the boot stages leave it; the level is read whatever
// the function (TLMM's IO bit 0 is the pad).
void PianoKeyboard::surveyHall()
{
  if (!gArgs.hall) {
    note("hall: gpio80 not read (piano-keyboard-hall=0)");
    return;
  }
  Pin &p = gHall;
  if (!mapRegion(p.page, kTlmm + uint64_t(p.gpio) * 0x1000, 0x10)) {
    note("hall: gpio80 does not map: no hall sensor");
    return;
  }
  p.ctl = rd32(p.page.va + kCtl);
  p.io = rd32(p.page.va + kIo);
  p.intrCfg = rd32(p.page.va + kIntrCfg);
  fHall = fHallSample = int(p.io & 1);
  note("hall: gpio80 CTL %08x (function %u, pull %u, %s) IO %08x INTR_CFG %08x: cover %s at start (level %u; the DT's switch-lid "
       "is active low: low = closed)%s",
       p.ctl, (p.ctl >> 2) & 15, p.ctl & 3, p.ctl & kOe ? "output" : "input", p.io, p.intrCfg, fHall ? "open" : "closed", p.io & 1,
       gArgs.lid ? "" : "; macOS is not told (piano-keyboard-lid=0)");
  setProperty("piano-keyboard-hall", uint64_t(fHall), 32);
}

// Every 100 ms: a level that two samples in a row agree on and differs from the accepted one is a change.
void PianoKeyboard::pollHall(uint64_t now)
{
  if (fHall < 0 || now < fHallNextMs) return;
  fHallNextMs = now + 100;
  const int sample = int(rd32(gHall.page.va + kIo) & 1);
  const bool agreed = sample == fHallSample;
  fHallSample = sample;
  if (!agreed || sample == fHall) return;
  fHall = sample;
  ++fHallChanges;
  note("hall: cover %s (gpio80 %d, change %u, %llu s after start)", fHall ? "open" : "closed", fHall, fHallChanges,
       (unsigned long long)(fStartMs ? (now - fStartMs) / 1000 : 0));
  setProperty("piano-keyboard-hall", uint64_t(fHall), 32);
  lidNotify(!fHall);
}

// piano-keyboard-lid=1/2 only: the hall state to IOPMrootDomain as a clamshell (an asynchronous power event there). =1 first
// disables clamshell sleep, so a close reports the state without putting the system to sleep; =2 allows it.
void PianoKeyboard::lidNotify(bool closed)
{
  if (!gArgs.lid || fHall < 0) return;
  IOPMrootDomain *root = getPMRootDomain();
  if (!root) {
    note("hall: no IOPMrootDomain: clamshell %s not sent", closed ? "closed" : "opened");
    return;
  }
  if (!fLidSetup) {
    fLidSetup = true;
    root->receivePowerNotification(gArgs.lid >= 2 ? kIOPMEnableClamshell : kIOPMDisableClamshell);
    note("hall: piano-keyboard-lid=%u: clamshell sleep %s (%s sent)", gArgs.lid, gArgs.lid >= 2 ? "ALLOWED" : "disabled",
         gArgs.lid >= 2 ? "kIOPMEnableClamshell" : "kIOPMDisableClamshell");
  }
  root->receivePowerNotification(closed ? kIOPMClamshellClosed : kIOPMClamshellOpened);
  note("hall: %s sent to IOPMrootDomain", closed ? "kIOPMClamshellClosed" : "kIOPMClamshellOpened");
}

// The bring-up stopped (a failed step, piano-keyboard-stop): the hall sensor is still polled and logged.
void PianoKeyboard::hallOnly()
{
  if (fHall < 0) return;
  note("hall: polled every 100 ms without the keyboard (the bring-up stopped at step %d)", piano_keyboard_stage);
  fStartMs = nowMs();
  lidNotify(!fHall);
  for (;;) {
    pollHall(nowMs());
    IOSleep(100);
  }
}

bool PianoKeyboard::setupPins()
{
  for (const Pin &p : gPin)
    if (!p.page.va) {
      stage(-2, "a pin does not map");
      return false;
    }
  // Reset and sleep: output value low first, then output enabled (reset no pull, sleep pull-down: the DT's keyboard-default-state).
  drive(kReset, false);
  drive(kSleep, false);
  wr32(pinVa(kReset) + kCtl, pinCfg(kReset, 0, true));
  wr32(pinVa(kSleep) + kCtl, pinCfg(kSleep, kPullDown, true));
  wr32(pinVa(kStatus) + kCtl, pinCfg(kStatus, 0, false));
  wr32(pinVa(kIrq) + kCtl, pinCfg(kIrq, gPin[kIrq].ctl & 3, false));
  wr32(pinVa(kIrq) + kIntrCfg, gPin[kIrq].intrCfg & ~1u);   // no interrupt to the GIC
  // The bus pins as GPIO inputs without a pull until the rails are up (no current into an unpowered MCU); their output value 0 is
  // what "low" drives later.
  drive(kSda, false);
  drive(kScl, false);
  wr32(pinVa(kScl) + kCtl, pinCfg(kScl, 0, false));
  wr32(pinVa(kSda) + kCtl, pinCfg(kSda, 0, false));
  IODelay(50);
  note("pins: reset %u sleep %u (driven low), SDA %u SCL %u (inputs, no pull yet), irq %u, status %u", level(kReset), level(kSleep),
       level(kSda), level(kScl), level(kIrq), level(kStatus));
  static const LineIo io = {nullptr, lineSda, lineScl, readSda, readScl, delayUs};
  fBus.configure(io, gArgs.halfUs, 20000);
  return true;
}

bool PianoKeyboard::setupRails()
{
  if (!gArgs.rails) {
    note("rails: not voted (piano-keyboard-rails=0)");
    return true;
  }
  if (!gRsc.va || !gRails[0].address || !gRails[1].address) {
    stage(-3, "rails: no cmd-db address or RSC (see the survey)");
    return false;
  }
  for (const Rail &r : gRails)
    if (!vote(r)) {
      stage(-3, "rails: a vote did not complete");
      return false;
    }
  IOSleep(5);
  note("rails: up; bus SDA %u SCL %u, irq %u, status %u", level(kSda), level(kScl), level(kIrq), level(kStatus));
  return true;
}

bool PianoKeyboard::romWrite(uint8_t value)
{
  const uint8_t bytes[2] = {kRegBoot, value};
  const I2c r = fBus.write(kAddress, bytes, 2);
  IOSleep(3);
  if (r != I2c::ok) note("boot ROM: command %#04x: %s", value, i2cName(r));
  return r == I2c::ok;
}
int PianoKeyboard::romRead()
{
  const uint8_t reg = kRegBoot;
  uint8_t v = 0;
  const I2c r = fBus.writeRead(kAddress, &reg, 1, &v, 1);
  if (r != I2c::ok) {
    note("boot ROM: read of register 0x5c: %s", i2cName(r));
    return -1;
  }
  return v;
}

bool PianoKeyboard::powerOn()
{
  lineScl(nullptr, true);   // released: the internal pull-ups on, beside the board's
  lineSda(nullptr, true);
  IOSleep(2);
  drive(kReset, true);
  IOSleep(500);
  drive(kSleep, true);
  IOSleep(50);
  note("power-on: reset %u sleep %u; bus SDA %u SCL %u, irq %u, status %u", level(kReset), level(kSleep), level(kSda), level(kScl),
       level(kIrq), level(kStatus));
  if (!fBus.recover()) note("power-on: the bus does not come free (SDA %u SCL %u)", level(kSda), level(kScl));
  int id = -1, state = -1;
  if (romWrite(kRomId)) id = romRead();
  if (romWrite(kRomState)) state = romRead();
  note("boot ROM: id %#x (want 0xc8), state %d (want 0: idle)", id, state);
  if (id != kBootId) {
    stage(-4, "boot ROM: no id 0xc8 at I2C 0x4c (MCU not powered, not there, or the bus is wrong)");
    return false;
  }
  if (state != kStateIdle) {
    stage(-4, "boot ROM: not idle");
    return false;
  }
  return true;
}

bool PianoKeyboard::loadFirmware()
{
  const uint64_t t0 = nowMs();
  uint32_t code = 0;
  const int checked = imageCheck(piano_keyboard_firmware, uint32_t(piano_keyboard_firmware_size), &code);
  if (checked) {
    note("firmware: image check %d (-1 size, -2 magic, -3 code size, -4 code sum); built without MCU_Upgrade.bin?", checked);
    stage(-5, "firmware: no usable image");
    return false;
  }
  headerCommand(fWire, piano_keyboard_firmware, code);
  I2c r = fBus.write(kAddress, fWire, kHeaderWire);
  IOSleep(3);
  const int header = r == I2c::ok && romWrite(kRomState) ? romRead() : -1;
  note("firmware: header %s, state %d (want 1)", i2cName(r), header);
  if (header != kStateHeader) {
    stage(-5, "firmware: header not taken");
    return false;
  }
  unsigned blocks = 0;
  for (uint32_t offset = 0; offset < code; ++blocks) {
    const unsigned n = dataCommand(fWire, piano_keyboard_firmware, code, offset);
    r = fBus.write(kAddress, fWire, kDataWire);
    IOSleep(3);
    if (r != I2c::ok) {
      note("firmware: block %u (offset %u): %s at byte %u", blocks, offset, i2cName(r), fBus.failedByte);
      stage(-5, "firmware: a data block failed");
      return false;
    }
    offset += n;
  }
  const bool verify = romWrite(kRomVerify);
  IOSleep(10);
  const int verified = verify && romWrite(kRomState) ? romRead() : -1;
  note("firmware: %u blocks (%u bytes) in %llu ms; verify %s, state %d (want 3)", blocks, code, (unsigned long long)(nowMs() - t0),
       verify ? "sent" : "FAILED", verified);
  if (verified != kStateVerified) {
    stage(-5, "firmware: not verified");
    return false;
  }
  if (!romWrite(kRomBoot)) {
    stage(-5, "firmware: boot command failed");
    return false;
  }
  IOSleep(50);
  note("firmware: booted (%llu ms in all); irq %u, status %u", (unsigned long long)(nowMs() - t0), level(kIrq), level(kStatus));
  return true;
}

// Rising edges of gpio97 latched in its INTR_STATUS (raw status on, interrupt enable off: nothing reaches the GIC).
void PianoKeyboard::armIrq()
{
  const uint32_t cfg = (gPin[kIrq].intrCfg & ~(0x1fu | (7u << 8))) | (1u << 4) | (1u << 2) | (1u << 1) | (3u << 8);
  wr32(pinVa(kIrq) + kIntrCfg, cfg);
  wr32(pinVa(kIrq) + kIntrStatus, 0);
  note("serving: gpio97 INTR_CFG %08x (rising edge latched, not enabled), level %u; status gpio95 %u", rd32(pinVa(kIrq) + kIntrCfg),
       level(kIrq), level(kStatus));
  fStatusLevel = level(kStatus);
}

// --------------------------------------------------------------------------------------------------------------- HID (level 2)
bool PianoKeyboard::publish()
{
  for (unsigned k = 0; k < 2; ++k) {
    const Kind kind = Kind(k);
    PianoKeyboardHID *hid = OSTypeAlloc(PianoKeyboardHID);
    if (hid) {
      hid->fKind = kind;
      hid->fLeds = ledRequest;
      hid->fLedsContext = this;
    }
    if (!hid || !hid->init(nullptr)) {
      OSSafeReleaseNULL(hid);
      stage(-6, "HID device: init");
      return false;
    }
    hid->setProperty(kIOHIDBuiltInKey, true);
    if (!hid->attach(this)) {
      OSSafeReleaseNULL(hid);
      stage(-6, "HID device: attach");
      return false;
    }
    if (!hid->start(this)) {
      hid->detach(this);
      OSSafeReleaseNULL(hid);
      stage(-6, "HID device: IOHIDDevice::start");
      return false;
    }
    fHid[k] = hid;
    note("HID device %s: vendor %#x product %#x, usage %#x/%#x, %u-byte descriptor, registry ID %#llx", deviceInfo(kind).product,
         kVendorId, kProductId, deviceInfo(kind).usagePage, deviceInfo(kind).usage, deviceInfo(kind).descriptorBytes,
         hid->getRegistryEntryID());
  }
  if (OSDictionary *matching = IOService::serviceMatching("IOHIDEventService")) {
    fPublished = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, eventServicePublished, this, nullptr);
    matching->release();
  }
  return true;
}

bool PianoKeyboard::eventServicePublished(void *, void *, IOService *service, IONotifier *)
{
  for (IOService *provider = service->getProvider(); provider; provider = provider->getProvider())
    if (PianoKeyboardHID *hid = OSDynamicCast(PianoKeyboardHID, provider)) {
      note("event service %s published on %s: macOS has it", service->getMetaClass()->getClassName(), deviceInfo(hid->fKind).product);
      break;
    }
  return true;
}

void PianoKeyboard::ledRequest(void *self, uint8_t leds)
{
  PianoKeyboard *k = static_cast<PianoKeyboard *>(self);
  __atomic_store_n(&k->fLedWanted, uint32_t(leds), __ATOMIC_RELEASE);
  __atomic_fetch_add(&k->fLedRequests, 1, __ATOMIC_RELEASE);
}

IOReturn PianoKeyboard::send(Kind kind, const uint8_t *bytes, unsigned length)
{
  PianoKeyboardHID *hid = fHid[unsigned(kind)];
  if (!hid) return kIOReturnNotReady;
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, length, kIODirectionNone);
  const IOReturn rc = report ? hid->handleReport(report) : kIOReturnNoMemory;
  OSSafeReleaseNULL(report);
  ++fSent;
  if (rc != kIOReturnSuccess && (++fSendErrors <= 5 || fSendErrors % 1000 == 0))
    note("%s: handleReport %#x (%u failures)", deviceInfo(kind).product, rc, fSendErrors);
  return rc;
}

void PianoKeyboard::releaseAll()
{
  const uint8_t keyboard[kKeyboardBytes] = {kIdKeyboard}, consumer[kConsumerBytes] = {kIdConsumer};
  uint8_t pointer[kPointerBytes];
  Pointer::release(pointer);
  send(Kind::keyboard, keyboard, sizeof keyboard);
  send(Kind::keyboard, consumer, sizeof consumer);
  send(Kind::pointer, pointer, sizeof pointer);
  fPointer.reset();
  fKeyFilter.reset();
}

// ------------------------------------------------------------------------------------------------------------------ step 6
bool PianoKeyboard::sendCommand(uint8_t cmd, uint8_t value, const char *what)
{
  uint8_t wire[kCommandWire];
  command(wire, cmd, value);
  const I2c r = fBus.write(kAddress, wire, kCommandWire);
  ++fCommands;
  if (r != I2c::ok) ++fCommandErrors;
  if (r != I2c::ok || fCommands <= 20) note("command %#04x %s = %u: %s", cmd, what, value, i2cName(r));
  return r == I2c::ok;
}

// piano-keyboard-auth=1: the Linux driver's answers (Wn8030Piano.h authReply). The cover's UID and challenge are never logged.
void PianoKeyboard::authAnswer(const Report &r)
{
  uint8_t wire[kCommandWire];
  const AuthStep a = authReply(r, wire);
  const char *what = nullptr;
  switch (a) {
  case AuthStep::init: what = "auth init (MIAUTH)"; break;
  case AuthStep::s3t1:
    note("auth: the cover's init reply carries its 16-byte UID (not logged)");
    what = "auth s3t1 (offline: key meta 00 00 00 02, fixed host challenge)";
    break;
  case AuthStep::challenge:
    note("auth: the cover's challenge (16 bytes, not logged); the token (s5t1) needs the Linux helper's secret: not sent, "
         "the exchange ends here");
    return;
  case AuthStep::tooShort: note("auth: kind %#04x in a %u-byte report: too short to answer", r.bytes[4], r.length); return;
  case AuthStep::none: return;
  }
  char line[60];
  const I2c rc = fBus.write(kAddress, wire, kCommandWire);
  ++fCommands;
  if (rc != I2c::ok) ++fCommandErrors;
  note("auth: sent %s: %s (%s)", what, i2cName(rc), hex(wire + 1, 12, line, sizeof line));
}

void PianoKeyboard::coverAttached(bool on, const char *why)
{
  if (on == fAttached) return;
  fAttached = on;
  note("cover %s (%s)", on ? "ATTACHED" : "detached", why);
  if (!on) {
    releaseAll();   // every key, consumer key and button up; the pointer and key state forgotten
    return;
  }
  ++fAttaches;
  Command list[5];
  const unsigned n = attachCommands(fCaps, gArgs.touchpad, gArgs.backlight, list);
  for (unsigned i = 0; i < n; ++i) sendCommand(list[i].cmd, list[i].value, list[i].what);
  fKeepaliveMs = nowMs() + 12000;
}

bool PianoKeyboard::readFrame()
{
  const uint8_t reg = kRegRuntime;
  ++fReads;
  fLastReadMs = nowMs();
  const I2c r = fBus.writeRead(kAddress, &reg, 1, fFrame, kFrameBytes);
  if (r != I2c::ok) {
    ++fReadErrors;
    if (++fConsecutiveErrors <= 5 || fReadErrors % 100 == 0) note("frame read %u: %s (%u errors)", fReads, i2cName(r), fReadErrors);
    if (r == I2c::busBusy) fBus.recover();
    return false;
  }
  fConsecutiveErrors = 0;
  Report reports[24];
  unsigned count = 0;
  const Frame f = unpackFrame(fFrame, reports, 24, &count);
  // Raw bytes may hold keys: at level 2 (the login password) only with piano-keyboard-trace=1. Authentication material (the
  // cover's UID, a challenge) never.
  const bool show = gArgs.level < 2 || gArgs.trace;
  char line[220];
  if (f == Frame::empty) {
    ++fEmpty;
    if (show && gArgs.raw && fEmptyLogged < 3)
      note("empty frame %u (read %u), raw: %s", ++fEmptyLogged, fReads, hex(fFrame, 24, line, sizeof line));
    return false;
  }
  // Every frame that is more than an accelerometer sample, raw (the first 300, then every 50th): what the MCU sends is not all known.
  bool onlyAccel = f == Frame::reports, secret = false;
  for (unsigned i = 0; i < count; ++i) {
    int attached = -1;
    onlyAccel = onlyAccel && vendorKind(reports[i], &attached) == Vendor::accel;
    secret = secret || authPayload(reports[i]);
  }
  if (show && gArgs.raw && !onlyAccel && (++fRawLogged <= 300 || fRawLogged % 50 == 0)) {
    if (secret) note("frame %u (read %u, %u reports): authentication material, raw not logged", fFrames + 1, fReads, count);
    else note("frame %u (read %u, %u reports), raw: %s", fFrames + 1, fReads, count, hex(fFrame, kFrameBytes, line, sizeof line));
  }
  // The trailer checksum is not a condition (Linux checks none): a mismatch is counted, the first ones logged.
  if (!frameSumOk(fFrame) && (++fSumBad <= 5 || fSumBad % 100 == 0))
    note("frame %u: trailer checksum %#04x, the bytes sum to %#04x (%u mismatches; dispatched anyway)", fReads, fFrame[kFrameBytes - 1],
         sum8(fFrame + 2, kFrameBytes - 3), fSumBad);
  if (f != Frame::reports) {
    if (++fBadFrames <= 20 || fBadFrames % 100 == 0)
      note("frame %u: %s (%u reports before it): %s", fReads, frameName(f), count, hex(fFrame, show && !secret ? 24 : 3, line, sizeof line));
  }
  ++fFrames;
  for (unsigned i = 0; i < count; ++i) dispatch(reports[i]);
  return f == Frame::reports;
}

// "mods 02 keys 04 05" of a 9-byte keyboard report (hex).
static const char *keyList(const uint8_t *k, char *out, size_t size)
{
  size_t used = size_t(snprintf(out, size, "mods %02x keys", k[1]));
  bool any = false;
  for (unsigned i = 3; i < kKeyboardBytes && used + 4 < size; ++i)
    if (k[i]) {
      used += size_t(snprintf(out + used, size - used, " %02x", k[i]));
      any = true;
    }
  if (!any && used + 6 < size) snprintf(out + used, size - used, " none");
  return out;
}

void PianoKeyboard::dispatch(const Report &r)
{
  char line[220], decoded[80];
  const bool show = gArgs.level < 2 || gArgs.trace;
  switch (r.id) {
  case kIdKeyboard: {
    ++fKeys;
    if (!fAttached) coverAttached(true, "a key report without an attach message");
    uint8_t out[kKeyboardBytes];
    const uint32_t rollovers = fKeyFilter.rollovers();
    fKeyFilter.filter(r.bytes, out);
    const bool rollover = fKeyFilter.rollovers() != rollovers, changed = memcmp(out, r.bytes, kKeyboardBytes) != 0;
    const IOReturn rc = send(Kind::keyboard, out, kKeyboardBytes);
    if (show)
      note("key %u: %s -> %s%s%s", fKeys, hex(r.bytes, r.length, line, sizeof line), keyList(out, decoded, sizeof decoded),
           rollover ? " (rollover: the previous keys kept)" : changed ? " (Fn row)" : "", fHid[0] ? (rc ? " (macOS refused)" : "") : "");
    else if (fKeys <= 3 || fKeys % 100 == 0)
      note("key report %u to macOS: %s (contents not logged at level 2)", fKeys, rc ? "handleReport FAILED" : "handleReport ok");
    return;
  }
  case kIdConsumer: {
    ++fConsumer;
    const IOReturn rc = send(Kind::keyboard, r.bytes, kConsumerBytes);
    if (show || fConsumer <= 20)
      note("consumer %u: %s (usage %#x)%s", fConsumer, hex(r.bytes, r.length, line, sizeof line), le16(r.bytes + 1),
           fHid[0] ? (rc ? ", macOS refused" : ", to macOS") : "");
    return;
  }
  case kIdTouchpad: {
    if (!fTouchSeen) note("touchpad detected (first report 0x19): a cover with a touchpad");
    fTouchSeen = true;
    ++fTouch;
    if (show && (fTouch <= 120 || fTouch % 25 == 0)) note("touch %u: %s", fTouch, hex(r.bytes, r.length, line, sizeof line));
    uint8_t out[2][kPointerBytes];
    const unsigned n = fPointer.touch(r.bytes, nowMs(), out);
    for (unsigned i = 0; i < n; ++i) {
      const IOReturn rc = send(Kind::pointer, out[i], kPointerBytes);
      if (fHid[1] && ++fPointerSent <= 3) note("pointer report %u to macOS: %s", fPointerSent, rc ? "handleReport FAILED" : "handleReport ok");
    }
    Gesture g;
    if (fPointer.takeGesture(&g) && (fPointer.gestures() <= 100 || fPointer.gestures() % 20 == 0))
      note("touch gesture %u: %u finger%s, %u ms, (%d,%d) -> (%d,%d), travel %u, %u moves, %u scrolls%s%s", fPointer.gestures(), g.most,
           g.most == 1 ? "" : "s", g.ms, g.x0, g.y0, g.x1, g.y1, g.travel, g.moves, g.scrolls, g.pressed ? ", pressed" : "",
           g.tap ? (g.most >= 2 ? ", TAP (right click)" : ", TAP (left click)") : "");
    return;
  }
  case kIdMouse: {
    ++fMouse;
    if (show && (fMouse <= 40 || fMouse % 50 == 0)) note("mouse %u: %s", fMouse, hex(r.bytes, r.length, line, sizeof line));
    if (!gArgs.mouse || fTouchSeen) return;
    uint8_t out[kPointerBytes];
    Pointer::mouse(r.bytes, out);
    send(Kind::pointer, out, kPointerBytes);
    return;
  }
  default: break;
  }
  int attached = -1;
  const Vendor v = vendorKind(r, &attached);
  ++fVendor;
  if (v == Vendor::attach) {
    note("attach message: state byte %#04x (bit 0 %u, bit 1 %u)%s: %s", r.bytes[9], r.bytes[9] & 1, (r.bytes[9] >> 1) & 1,
         fAttached && attached == 1 ? ", already attached" : "", hex(r.bytes, r.length < 16 ? r.length : 16, line, sizeof line));
    coverAttached(attached == 1, "attach message");
  } else if (v == Vendor::accel) {
    if (++fAccel <= 3 || fAccel % 500 == 0) note("accelerometer %u: %s", fAccel, hex(r.bytes, 16, line, sizeof line));
  } else if (v == Vendor::authRequest || v == Vendor::authInit || v == Vendor::authChallenge) {
    // A request's bytes are logged; an init reply's or a challenge's only up to the payload (the UID / challenge is not).
    if (++fAuth <= 20 || gArgs.auth)
      note("%s (%s): %s%s", vendorName(v), gArgs.auth ? "answered as Linux with its helper" : "unanswered: piano-keyboard-auth=0",
           hex(r.bytes, v == Vendor::authRequest ? 12 : 8, line, sizeof line), v == Vendor::authRequest ? "" : " ... (not logged)");
    if (gArgs.auth) authAnswer(r);
  } else if (v == Vendor::ack) {
    if (++fAcks <= 12 || fAcks % 100 == 0) note("command ack %u: command %#04x status %u", fAcks, r.bytes[6], r.bytes[7]);
  } else if (v == Vendor::state) {
    if (++fStates <= 12 || fStates % 100 == 0) note("command state %u: command %#04x = %02x", fStates, r.bytes[4], r.bytes[6]);
  } else if (fVendor <= 30) {
    note("vendor %#04x (%s): %s", r.id, vendorName(v), hex(r.bytes, r.length < 24 ? r.length : 24, line, sizeof line));
  }
}

void PianoKeyboard::serve()
{
  fStartMs = nowMs();
  fLastReadMs = fStartMs;
  uint64_t nextDescribe = 15000;
  note("serving: %s", gArgs.level >= 2 ? "reports go to macOS" : "reports are logged only (piano-keyboard=2 gives them to macOS)");
  lidNotify(!fHall);
  for (;;) {
    const uint64_t now = nowMs();
    const bool edge = rd32(pinVa(kIrq) + kIntrStatus) & 1;
    if (edge) {
      wr32(pinVa(kIrq) + kIntrStatus, 0);
      if (!fEdgeSeen) note("first gpio97 edge after %llu ms", (unsigned long long)(now - fStartMs));
      fEdgeSeen = true;
      ++fEdges;
    }
    // A frame per edge; after a frame with reports one more read (edges coalesce while a read runs). Until an edge is seen, and
    // every second after that, a read anyway (an edge lost before arming leaves the MCU waiting).
    // gpio97 held high without a latched edge (a level the MCU keeps while it has data) also means a frame is waiting.
    ++fPolls;
    const bool high = level(kIrq);
    if (high) ++fIrqHigh;
    else fIrqWasLow = true;
    const bool levelDue = !edge && !fReadAgain && high && fIrqWasLow;
    if (levelDue) ++fLevelReads;
    const bool due = edge || levelDue || fReadAgain || now - fLastReadMs >= (fEdgeSeen ? 1000u : 100u);
    unsigned burst = 0;
    if (due) {
      do fReadAgain = readFrame();
      while (fReadAgain && ++burst < 8);
    }
    const bool status = level(kStatus);
    if (status != fStatusLevel) {
      fStatusLevel = status;
      note("status gpio95 now %u", status);
    }
    const uint32_t requests = __atomic_load_n(&fLedRequests, __ATOMIC_ACQUIRE);
    if (requests != fLedSeen) {
      fLedSeen = requests;
      const bool caps = __atomic_load_n(&fLedWanted, __ATOMIC_ACQUIRE) & kLedCapsLock;
      if (caps != fCaps) {
        fCaps = caps;
        if (fAttached) sendCommand(kCmdIndicator, caps ? kLedCapsOn : kLedCapsOff, "caps lock LED");
      }
    }
    if (fAttached && now >= fKeepaliveMs) {
      sendCommand(kCmdKeyboardPower, 1, "keyboard power (every 12 s)");
      fKeepaliveMs = now + 12000;
    }
    pollHall(now);
    const uint64_t elapsed = now - fStartMs;
    if (elapsed >= nextDescribe) {
      describe(elapsed / 1000);
      nextDescribe = nextDescribe < 60000 ? nextDescribe + 15000 : nextDescribe < 120000 ? nextDescribe + 30000 : nextDescribe + 300000;
    }
    if (!fReadAgain) IOSleep(fConsecutiveErrors >= 50 ? 1000 : gArgs.pollMs);
  }
}

void PianoKeyboard::describe(uint64_t seconds)
{
  note("alive at %llu s: %s, %u reads (%u frames, %u empty, %u errors, %u bad, %u checksum mismatches), %u edges%s, %u level reads, irq high in %u of %u "
       "polls; keys %u (rollovers %u), consumer %u, touch %u (gestures %u, taps %u), mouse %u, vendor %u (accel %u, auth %u, acks "
       "%u), attaches %u; %u commands (%u failed), %u reports to macOS (%u failed); irq %u status %u; hall %s",
       seconds, fAttached ? "attached" : "no cover", fReads, fFrames, fEmpty, fReadErrors, fBadFrames, fSumBad, fEdges,
       fEdgeSeen ? "" : " (none yet: polled every 100 ms)", fLevelReads, fIrqHigh, fPolls, fKeys, fKeyFilter.rollovers(), fConsumer,
       fTouch, fPointer.gestures(), fPointer.taps(), fMouse, fVendor, fAccel, fAuth, fAcks, fAttaches, fCommands, fCommandErrors,
       fSent, fSendErrors, level(kIrq), level(kStatus), fHall < 0 ? "not read" : fHall ? "open" : "closed");
}
