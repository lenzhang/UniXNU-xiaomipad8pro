// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch: the Xiaomi Pad 8 Pro touch screen (Novatek NT36532E, a TDDI with host-processed "THP" touch) for macOS 27 on piano
// (SM8750), native at EL1. The controller sits on QUP1 serial engine 2 (spi@a88000, chip select 0, GPIO40-43), raises GPIO162 for
// every frame, and is powered with the panel (no rail of its own: nothing is voted). It has no flash for its touch firmware: the host
// downloads the panel's image (BOE or CSOT by GPIO100, staged by tooling/k27/build-native-piano-touch.sh from an ignored out/ copy,
// never committed) into its SRAM at every start; it then streams raw 40 x 60 matrices that TouchMatrix.h turns into one contact.
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
//   6 start      payload length, firmware info, calibration wait, idle-baseline update, operating mode
//   7 frames     GPIO162's rising edges latched in TLMM (raw status), polled every piano-touch-poll ms while the panel is busy
//                and a frame read per edge; at rest one frame per piano-touch-idle ms (TouchPace.h), its edge waited for by
//                polling or, with piano-touch-irq=1, by the TLMM summary interrupt (package/common/PianoIrq.h); checksum,
//                transport kind, counter, type; the matrix decoder and the tip; at level 2 the tablet pointer (TouchHid.h). Ten
//                watchdog frames in a row: the download again (at most three times)
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-touch=1            steps 1-7; frame statistics and contacts in the log; nothing goes to macOS
//   piano-touch=2            the same and the HID tablet pointer (a tap clicks, a drag drags); contact positions are then only
//                            counted (piano-touch-trace=1 logs them)
//   piano-touch-stop=N       stop before step N (2: the read-only survey alone; 4: the bus up and tested, the controller untouched)
//   piano-touch-bus=N        0 GENI, falling back to SPI by hand (default); 1 GENI only; 2 SPI by hand only
//   piano-touch-khz=K        the GENI SPI rate (default 9600; 1000..19200, the DT's maximum)
//   piano-touch-seirq=N      the engine's interrupt outputs while polled: 2 on (default: the 13T's SE DMA code and K90's driver on
//                            this SE keep them on, their status bits follow them; ArmCpu leaves every unused SPI disabled at the
//                            GIC); 1 off; 0 off, then on if the DMA loopback times out
//   piano-touch-smmu=0       leave the SMMU alone (SE DMA then needs stream 0xa3 to bypass already)
//   piano-touch-loopback=0   no loopback self-test of the engine
//   piano-touch-mode=V       the operating-mode command's value (default 2; 65536: not sent)
//   piano-touch-baseline=0   no idle-baseline update command
//   piano-touch-threshold=T  the contact threshold of the decoder (default 200)
//   piano-touch-flip=B       auto (default: by the panel, BOE as processor Y, CSOT Y reversed; the calibration's panel wins over
//                            gpio100 for this, and its invert_y over both); 0..3 explicit: bit 0 X, bit 1 Y flipped
//   piano-touch-poll=MS      edge polling while busy (default 1)
//   piano-touch-idle=MS      one frame per MS at rest (default 50; 0: every frame, the old loop; 20..500)
//   piano-touch-irq=1        the idle frames' edge by the TLMM interrupt (gpio162 on the summary SPI 208) instead of 1 ms polls
//   piano-wake-log=S         the "idle: N wakeups" line every S seconds (default 60, 0 none; shared with the other drivers)
//   piano-touch-trace=1      more frame headers, and contact positions at level 2
// Never: a flash write (the controller has none for the touch firmware), a panel command, a regulator vote, a GPIO other than
// 40-43, 100 and 162.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "Nt36532.h"
#include "TouchMatrix.h"
#include "TouchHid.h"
#include "Se2Bus.h"
#include "PianoTouchCalibration.h"   // generated (tooling/k27/gen-touch-calibration.py) into the ignored out/ tree
#include "SmmuStream.h"
#include "TouchPace.h"
#include "../common/PianoIrq.h"
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
  uint32_t level, stop, bus, khz, seirq, smmu, loopback, mode, baseline, threshold, flip, pollMs, trace, idleMs, irq;
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
// DroidVMInput's DroidVMHIDDevice (as package/PianoInput's PianoHIDDevice and PianoKeyboard's PianoKeyboardHID, UPSTREAM) with the
// tablet descriptor of TouchHid.h.
class PianoTouchHID : public IOHIDDevice {
  OSDeclareDefaultStructors(PianoTouchHID)

public:
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
  *descriptor = IOBufferMemoryDescriptor::withBytes(kTabletDescriptor, sizeof kTabletDescriptor, kIODirectionNone);
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
  Tip fTip;
  // transfer statistics
  uint32_t fXfers = 0, fXferErrors = 0;
  // serving statistics
  uint64_t fStartMs = 0, fLastReadMs = 0, fLastStatsMs = 0, fTipDownMs = 0, fLastHeldLogMs = 0;
  uint32_t fEdges = 0, fReads = 0, fData = 0, fGood = 0, fBad = 0, fInconsistent = 0, fHand = 0, fWdt = 0, fReadErrors = 0;
  uint32_t fConsecutive = 0, fWdtRun = 0, fDownloads = 0, fGaps = 0, fShown = 0, fContactsLogged = 0, fSent = 0, fSendErrors = 0;
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
  bool geniLoopback();
  bool engineIdle();
  unsigned pollLimit(uint32_t len) const;
  bool gpioUp();
  void pinsToGpio();
  bool identify();
  bool firmware();
  bool startScanning();
  void armIrq();
  bool publish();
  void serve();
  void frame(uint32_t readUs);
  void contact(TipEvent e);
  void send(bool tip, int32_t x, int32_t y);
  void stats(uint64_t now);

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
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
  note("start: piano-touch=%u (%s), stop %u, bus %u (%s), %u kHz, seirq %u, smmu %u, loopback %u, mode %u, baseline %u, "
       "threshold %u, flip %s, poll %u ms, idle %u ms (%s), irq %u; firmware BOE %llu bytes, CSOT %llu bytes",
       gArgs.level, gArgs.level >= 2 ? "HID tablet pointer for macOS" : "log only, nothing for macOS", gArgs.stop, gArgs.bus,
       gArgs.bus == 0 ? "GENI, else by hand" : gArgs.bus == 1 ? "GENI only" : "by hand only", gArgs.khz, gArgs.seirq, gArgs.smmu,
       gArgs.loopback, gArgs.mode, gArgs.baseline, gArgs.threshold, kFlipNames[gArgs.flip], gArgs.pollMs, gArgs.idleMs,
       gArgs.idleMs ? "one frame per idle period at rest" : "every frame", gArgs.irq,
       (unsigned long long)piano_touch_firmware_boe_size, (unsigned long long)piano_touch_firmware_csot_size);
  setProperty("piano-touch", gArgs.level, 32);
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
  // The HID device from start, as PianoInput and PianoDwc3 publish theirs; it sends nothing until a tip comes.
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
  if (!firmware()) return;
  if (!step(6, "start: payload length, firmware info, calibration, baseline, operating mode")) return;
  if (!startScanning()) return;
  if (!step(7, "frames: gpio162 edges latched and polled, a frame read per edge")) return;
  armIrq();
  setProperty("piano-touch-stage", uint64_t(uint32_t(piano_touch_stage)), 32);
  serve();
}

int PianoTouch::busXfer(void *self, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  PianoTouch *t = static_cast<PianoTouch *>(self);
  ++t->fXfers;
  int rc = 0;
  if (t->fBus == BusKind::geni) {
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

bool PianoTouch::geniClocks()
{
  // Our votes even when the clocks already run (on someone else's vote they could stop later and stall the next SE2 access), under
  // PianoA830's lock word (a holder that keeps it for 50 ms does not stop us: the update goes ahead). Bits are only ever set.
  uint32_t waits = 0;
  const bool needed = (gcc(kGccVote) & kVoteBits) != kVoteBits;
  bool locked = false;
  for (; needed && waits < 5000 && !locked; ++waits) {
    uint32_t expected = 0;
    locked = __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kGccVoteTag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    if (!locked) IODelay(10);
  }
  const uint32_t before = gcc(kGccVote);
  if ((before & kVoteBits) != kVoteBits) {
    wr32(gGcc.va + kGccVote, before | kVoteBits);
    dsb();
  }
  const uint32_t after = gcc(kGccVote);
  if (needed && locked) {
    uint32_t expected = kGccVoteTag;
    __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
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
  return true;
}

static const char *command(int rc) { return rc == 1 ? "not sent" : rc == 0 ? "taken" : "NOT taken (continuing)"; }

bool PianoTouch::startScanning()
{
  const int rc = pianotouch::start(fChip, gArgs.baseline != 0, gArgs.mode);
  note("start: %s (rc %d); payload %u bytes (reported %u), frame %u bytes; firmware info version %#x %s, x %u y %u; reset state %#04x "
       "(%s after %u polls); baseline command %s, mode %u %s",
       rc ? "FAILED" : "scanning", rc, fChip.payload, fChip.payloadReported, frameBytes(fChip), fChip.fwVersion,
       fChip.fwInfoValid ? "(valid)" : "(NOT valid)", fChip.xNum, fChip.yNum, fChip.resetState, fChip.calibrated ? "calibrated" : "NOT calibrated",
       fChip.polls, command(fChip.baselineRc), gArgs.mode, command(fChip.modeRc));
  if (rc) {
    stage(-6, "start: the bus failed during the start sequence");
    return false;
  }
  return true;
}

// Rising edges of gpio162 latched in its INTR_STATUS (raw status on, interrupt enable off: nothing reaches the GIC).
void PianoTouch::armIrq()
{
  Pin &irq = gPin[kIrq];
  if (!irq.page.va) {
    note("frames: gpio162 does not map: frames polled without edges");
    return;
  }
  setCtl(kIrq, (irq.ctl & ~(kOe | (0xfu << 2)) & ~kEgpioEnable) | (irq.ctl & kEgpioPresent ? kEgpioEnable : 0));
  const uint32_t cfg = (irq.intrCfg & ~(0x1fu | (7u << 8))) | (1u << 4) | (1u << 2) | (1u << 1) | (3u << 8);
  wr32(pinVa(kIrq) + kIntrCfg, cfg);
  wr32(pinVa(kIrq) + kIntrStatus, 0);
  dsb();
  fIrqArmed = true;
  note("frames: gpio162 CTL %08x INTR_CFG %08x (rising edge latched, not enabled), level %u", rd32(pinVa(kIrq) + kCtl),
       rd32(pinVa(kIrq) + kIntrCfg), level(kIrq));
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
  note("HID device: tablet pointer, vendor %#x product %#x, %u-byte descriptor, registry ID %#llx", kVendorId, kProductId,
       unsigned(sizeof kTabletDescriptor), hid->getRegistryEntryID());
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

void PianoTouch::send(bool tip, int32_t x, int32_t y)
{
  if (!fHid) return;
  uint8_t r[kReportBytes];
  tabletReport(tip, x, y, kPanelW, kPanelH, r);
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(r, sizeof r, kIODirectionNone);
  const IOReturn rc = report ? fHid->handleReport(report) : kIOReturnNoMemory;
  OSSafeReleaseNULL(report);
  ++fSent;
  if (rc != kIOReturnSuccess && (++fSendErrors <= 5 || fSendErrors % 1000 == 0)) note("HID: handleReport %#x (%u failures)", rc, fSendErrors);
}

void PianoTouch::contact(TipEvent e)
{
  if (e == TipEvent::none) return;
  const uint64_t now = nowMs();
  const bool show = gArgs.level < 2 || gArgs.trace;
  if (e == TipEvent::down) {
    fTipDownMs = fLastHeldLogMs = now;
    if (fContactsLogged < 200 || fTip.downs % 20 == 0) {
      ++fContactsLogged;
      if (show) note("touch %u down at (%d, %d) px, peak %d, frame %u", fTip.downs, fTip.x, fTip.y, fMatrix->maxDelta, fMatrix->lastFrameNo);
      else note("touch %u down", fTip.downs);
    }
    send(false, fTip.x, fTip.y);   // the pointer there first, then the button
    send(true, fTip.x, fTip.y);
  } else if (e == TipEvent::move) {
    if (show && fContactsLogged < 200 && now - fLastHeldLogMs >= 250) {
      fLastHeldLogMs = now;
      note("touch %u at (%d, %d) px, peak %d", fTip.downs, fTip.x, fTip.y, fMatrix->maxDelta);
    }
    send(true, fTip.x, fTip.y);
  } else {
    if (fContactsLogged < 200 || fTip.ups % 20 == 0) {
      if (show) note("touch %u up at (%d, %d) px after %llu ms", fTip.ups, fTip.x, fTip.y, (unsigned long long)(now - fTipDownMs));
      else note("touch %u up after %llu ms", fTip.ups, (unsigned long long)(now - fTipDownMs));
    }
    send(false, fTip.x, fTip.y);
  }
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
    if (++fWdtRun == 11) note("frames: %u watchdog frames in a row (state %#04x)", fWdtRun, fWdtState);
    return;
  }
  fWdtRun = 0;
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
    note("matrix: reference of %u frames of type %d (%u x %u) after %u good frames; decoding", kRefFrames, fMatrix->refType, fMatrix->rows,
         fMatrix->cols, fGood);
  contact(tipUpdate(fTip, fMatrix->touching, fMatrix->x, fMatrix->y));
}

void PianoTouch::stats(uint64_t now)
{
  const uint64_t span = now - fLastStatsMs ? now - fLastStatsMs : 1;
  char types[96];
  size_t used = 0;
  types[0] = 0;
  for (unsigned k = 0; k < 8 && fTypeCount[k]; ++k) append(types, sizeof types, used, "%s%u:%u", used ? " " : "", fTypes[k], fTypeCount[k]);
  note("frames at %llu s: %llu reads/s, %llu edges/s; edges %u, reads %u (data %u: checksum ok %u, bad %u, inconsistent %u; handshake %u, "
       "watchdog %u), read errors %u, counter gaps %u; types {%s}; read %llu us avg, %u max; matrix %s, decoded %u, peak %d; touches %u",
       (unsigned long long)((now - fStartMs) / 1000), (unsigned long long)(fReadsSinceStats * 1000ull / span),
       (unsigned long long)(fEdgesSinceStats * 1000ull / span), fEdges, fReads, fData, fGood, fBad, fInconsistent, fHand, fWdt,
       fReadErrors, fGaps, types, (unsigned long long)(fReads ? fReadUsSum / fReads : 0), fReadUsMax,
       fMatrix->haveReference ? "referenced" : fMatrix->refType >= 0 ? "learning the reference" : "learning the type", fMatrix->frames,
       fMatrix->maxDelta, fTip.downs);
  fLastStatsMs = now;
  fReadsSinceStats = fEdgesSinceStats = 0;
  setProperty("piano-touch-frames", uint64_t(fGood), 32);
}

void PianoTouch::serve()
{
  MatrixConfig mc;
  mc.threshold = int32_t(gArgs.threshold);
  const Calibration *cal = kUnitCalibrationPresent && calibrationValid(kUnitCalibration) ? &kUnitCalibration : nullptr;
  mc.cal = cal;
  const Orientation o = chooseOrientation(fPanel, cal, gArgs.flip);
  mc.flipX = o.flipX;
  mc.flipY = o.flipY;
  if (o.mismatch)
    note("WARNING: lcd-id gpio100 says %s but the calibration's project_vendor_id %d is %s: orientation follows the calibration, "
         "the firmware image gpio100", panelName(fPanel), cal->projectVendorId, panelName(o.calibrationPanel));
  note("orientation: flip X %u, Y %u (%s; panel %s by %s), position %s", o.flipX, o.flipY, o.why, panelName(o.panel),
       o.calibrationPanel != kPanelUnknown ? "the calibration" : fPanel != kPanelUnknown ? "gpio100" : "default",
       cal ? "from the unit's electrode maps and edge mapping" : "linear over the matrix");
  matrixReset(*fMatrix, mc);
  fTip = Tip();
  const uint64_t begin = nowMs();
  fLastReadMs = fLastStatsMs = begin;
  // The GPIO transport reads at most 10 frames a second (each takes ~45 ms of a CPU); GENI one per edge.
  const uint64_t minGapMs = fBus == BusKind::gpio ? 100 : 0, noEdgeMs = fBus == BusKind::gpio ? 100 : 200;
  // Paced over GENI only (SPI by hand reads at most 10 frames a second anyway).
  const bool paced = gArgs.idleMs && fBus == BusKind::geni;
  TouchPace pace = touchPace(gArgs.idleMs, begin);
  pianoirq::WakeStats wakes = pianoirq::wakeStats(pianoirq::wakeLogMs());
  uint32_t readsAtWindow = 0, sleepsAtWindow = 0;
  note("frames: serving over %s, %s", fBus == BusKind::geni ? "GENI" : "SPI by hand",
       gArgs.level >= 2 ? "the tip goes to macOS" : "contacts are logged only (piano-touch=2 gives them to macOS)");
  for (;;) {
    const uint64_t now = nowMs();
    if (fIrqArmed && (rd32(pinVa(kIrq) + kIntrStatus) & 1)) {
      wr32(pinVa(kIrq) + kIntrStatus, 0);
      dsb();
      if (!fEdgeSeen) note("frames: first gpio162 edge after %llu ms", (unsigned long long)(now - begin));
      fEdgeSeen = fEdgePending = true;
      ++fEdges;
      ++fEdgesSinceStats;
    }
    const uint64_t since = now - fLastReadMs;
    if (since >= minGapMs && (fEdgePending || since >= noEdgeMs)) {
      fEdgePending = false;
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
        contact(tipCancel(fTip));
        if (++fConsecutive >= 50) {
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
      if (fWdtRun > 10) {
        contact(tipCancel(fTip));
        if (fDownloads >= 4) {
          stage(-7, "frames: the controller's watchdog again after three re-downloads");
          return;
        }
        // As the vendor flow: a controller that reports 0xfe is idled first (swrst <- 0xaa, 15 ms), then the download again.
        if (fWdtState == 0xfe) {
          writeByte(fChip, kSwrst, 0xaa);
          IOSleep(15);
        }
        note("frames: watchdog (state %#04x): the download again", fWdtState);
        fWdtRun = 0;
        if (!firmware() || !startScanning()) return;
        matrixReset(*fMatrix, mc);
      }
    }
    const uint64_t age = now - begin;
    if (now - fLastStatsMs >= (age < 30000 ? 5000 : 60000)) stats(now);
    pianoirq::WakeStats::Window w;
    if (wakes.due(now, &w)) {
      note("idle: %u wakeups in %u s, irq %u (%u frame reads; %u idle sleeps; %u reads on an interrupt's edge, %u of them bad; %u "
           "missed; pace %s)", w.wakeups, w.seconds, w.irqs, fReads - readsAtWindow, fIdleSleeps - sleepsAtWindow, fIrqReads, fIrqBadReads,
           fMiss.total, paced ? (pace.idle(nowMs(), false) ? "idle" : "every frame") : "every frame (not paced)");
      readsAtWindow = fReads;
      sleepsAtWindow = fIdleSleeps;
    }
    // The pace (TouchPace.h): every frame while the panel is busy, one per idle period at rest.
    const bool busy = fMatrix->touching || fTip.down || fTip.seen || !fMatrix->haveReference || fWdtRun;
    if (paced && fEdgeSeen && pace.idle(nowMs(), busy) && !fEdgePending) {
      const uint32_t sleep = pace.sleepMs(nowMs(), fLastReadMs);
      if (sleep) {
        IOSleep(sleep);
        wakes.count(pianoirq::Woke::timeout);
        ++fIdleSleeps;
        wr32(pinVa(kIrq) + kIntrStatus, 0);   // the latched edge came mid-sleep: the next fresh one starts a frame
        dsb();
        continue;
      }
      if (fIrqLive && fPin.live() && !fPollNextEdge) {
        const uint32_t seen = fPin.gen();
        fPin.arm();   // the handler masks it again when the edge comes
        if (fPin.wait(seen, 20)) {
          wakes.count(pianoirq::Woke::irq);
          ++fIrqEdges;
          fMiss.delivered();
          fEdgeByIrq = fPin.live();
        } else {
          wakes.count(pianoirq::Woke::timeout);
          fPin.mask();
          if (fPin.latched() && fMiss.miss()) {
            fIrqLive = false;
            note("frames: gpio162 latched but no interrupt in 3 waits in a row: idle edges by 1 ms polls from now on");
          }
        }
        if (fIrqLive && !fPin.live()) {
          fIrqLive = false;
          note("frames: the TLMM summary line went away (state %u): idle edges by 1 ms polls from now on", fPin.s->state);
        }
        continue;
      }
    }
    IOSleep(gArgs.pollMs);
    wakes.count(pianoirq::Woke::timeout);
  }
}
