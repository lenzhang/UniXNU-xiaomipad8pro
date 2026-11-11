// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoPolicy's portable part: the laptop-class policy of piano (cover, keyboard, power key, power profile) as a pure state machine,
// host-tested (tests/policy_host.cpp). PianoPolicy.cpp gathers the inputs from the other drivers' published state and carries out
// the actions; docs/policy.md is the page (signals, the table, what each driver adds).
//
// Inputs: the cover's hall sensor and attach bit (PianoKeyboard), the power source (PianoPower's AppleSmartBattery), the display
// power (NativeGopDisplay, a message per change: no polling), macOS Low Power Mode (machdep.piano_policy_lpm, which a userland agent mirrors), whether system sleep
// exists (k27 0062: machdep.piano_sleep "on") with its wake mask, PianoThermal's protection state.
// Outputs: IOPMrootDomain power notifications (the clamshell, clamshell sleep allowed or not, the AC adaptor), user-active
// requests (display wake), sleep now (the power key), PianoThermal's profile, PianoA830's GPU ceiling (client 1 of its slots).
//
// The rules (Ubuntu's, docs/ubuntu-history/device.md 2.2-2.3 and kernel.md 2.3/2.9, as behaviour):
//   cover closed on battery          -> system sleep, only with system sleep on, the cover-open wake armed (piano-sleep-wake bit 2)
//                                       and the cover seen open since boot; else, and on external power, lock only (no sleep)
//   cover opened                     -> clamshell open and a user-active request (the display wakes; from a suspend 0062 wakes)
//   keyboard status (gpio95) rising  -> display wake, only while the display is off, the cover open and attached, and the display
//                                       has been off for the hold-off (1 s); never gpio97 (not an input here at all)
//   power key short press            -> display on (or not known): sleep now; display off: display wake; no system sleep, a long
//                                       press, level 1: macOS's own power key (PianoInput's HID usage, as today)
//   Low Power Mode or not on AC      -> Power Saver (PianoThermal's battery caps, the GPU ceiling); on AC without it -> Balanced
#ifndef PIANO_POLICY_POLICY_H
#define PIANO_POLICY_POLICY_H

#include <stdint.h>

namespace pianopolicy {

enum class Lid : uint8_t { unknown, open, closed };
enum class Power : uint8_t { unknown, battery, external };
enum class Screen : uint8_t { unknown, on, off };
enum class Profile : uint8_t { saver, balanced };
enum class Close : uint8_t { none, sleep, lock };        // what closing the cover does (none: the cover part is off)
enum class Thermal : int8_t { unknown = -1, normal = 0, warm = 1, passive = 2, critical = 3 };   // PianoThermal's Policy.h State
enum class Key : uint8_t { passThrough, sleep, wakeDisplay };   // the power key's short press

// k27 0062's wake sources (piano-sleep-wake).
constexpr uint32_t kWakePowerKey = 1, kWakeLid = 2, kWakeUsb = 4, kWakeVolumeDown = 8, kWakeKeyboard = 16;

// IOPMrootDomain::receivePowerNotification messages (IOKit/pwr_mgt/IOPM.h, IOPMPrivate.h; PianoPolicy.cpp checks the values).
constexpr uint32_t kSleepNow = 1u << 0, kClamshellClosed = 1u << 4, kDisableClamshell = 1u << 6, kEnableClamshell = 1u << 7,
                   kClamshellOpened = 1u << 10, kSetValue = 1u << 16, kSetAcAdaptorConnected = 1u << 18;

// PianoThermal's profile call ("piano-thermal-profile", docs/policy.md 5.2): 0 by the power source (as today), 1 Power Saver.
constexpr uint32_t kThermalAuto = 0, kThermalSaver = 1;
// PianoA830's ceiling slots (docs/policy.md 5.3, package/PianoA830/board/PianoGpuCeiling.h): a PianoA830 that publishes
// piano-gpu-ceiling-clients >= 2 keeps one slot per caller and runs the GPU at the lowest; the power profile's is client 1, so
// PianoThermal's passive ceiling (client 0) stays in force whatever is written here: no breadcrumb read, no tick, no put-back.
// Without the property (a PianoA830 from before the slots): the one shared slot and the interim arbitration (Engine::gpu).
constexpr uint32_t kGpuClientShared = 0, kGpuClientPolicy = 1;
inline bool gpuOwnSlot(uint32_t clients) { return clients >= 2; }
// NativeGopDisplay's capability (docs/policy.md 5.6): published true on the panel's service before it registers, it says that each
// change of piano-display-power is messaged (kIOMessageServicePropertyChange, the property's name). Without it (an older KC) a
// message seen proves the same; else the display is polled by the tick (Engine::tickWhy).
constexpr const char *kDisplayMessagesKey = "piano-display-power-messages";
inline bool displayEvents(bool marked, uint32_t messagesSeen) { return marked || messagesSeen > 0; }
// Thresholds of the power key (PianoInput measures the press).
constexpr uint32_t kLongPressMs = 1500;

struct Config {
  bool act = false;              // piano-policy=2 (1: decisions logged, nothing sent)
  bool lid = false;              // piano-policy-lid=1: act on the cover (only once the hall polarity is verified)
  bool lidConflict = false;      // piano-keyboard-lid != 0: PianoKeyboard already tells IOPMrootDomain the clamshell
  bool profile = true;           // piano-policy-profile (0: no PianoThermal profile, no GPU ceiling)
  uint32_t gpuKhz = 342000;      // piano-policy-gpu-mhz: Power Saver's GPU ceiling (Ubuntu's Power Saver: 342 MHz); 0 none
  bool gpuNeeded = false;        // PianoA830's own maximum is above gpuKhz (piano-gpu-dcvs-max / -maxlevel given); else a no-op
  bool thermalOwnsGpu = false;   // PianoThermal at level 2: it writes the same single GPU ceiling slot at its passive trip
  uint32_t thermalGpuKhz = 160000;   // piano-thermal-gpu-mhz: the ceiling PianoThermal sets at passive (put back after a race)
  bool powerKey = true;          // piano-policy-powerkey
  bool keyboard = true;          // piano-policy-kbd: gpio95 rising wakes the display
  uint32_t holdoffMs = 1000;     // piano-policy-kbd-holdoff: the display off at least this long before gpio95 counts
  bool displayTick = false;      // piano-policy-display-tick=1: the 5 s tick of before 2026-11-12 even with display messages
};

struct Inputs {
  Lid lid = Lid::unknown;        // PianoKeyboard piano-keyboard-hall (1 open, 0 closed); unknown without the service or the hall
  bool attached = false;         // piano-keyboard-attached
  Power power = Power::unknown;  // IOPMPowerSource ExternalConnected
  Screen screen = Screen::unknown;   // NativeGopDisplay piano-display-power (always since 2026-11-11; before: piano-backlight=2)
  bool displayEvents = false;    // the display found messages each change of it (its piano-display-power-messages, or one seen)
  bool lpm = false;              // machdep.piano_policy_lpm
  bool sleepOn = false;          // machdep.piano_sleep begins with "on" (k27 0062 and piano-sleep=1)
  uint32_t wakeMask = 0;         // its "wake mask"
  Thermal thermal = Thermal::unknown;   // PianoThermal's piano_thermal_state (with thermalOwnsGpu, only while the slot is shared)
  uint32_t gpuClients = 0;       // PianoA830's piano-gpu-ceiling-clients (0: no PianoA830 seen yet, or one without the slots)
};

// What one set of inputs means (no history but whether the cover was ever seen open).
struct View {
  Close close = Close::none;
  const char *closeWhy = "";
  Profile profile = Profile::saver;
  const char *profileWhy = "";
  uint32_t gpuKhz = 0;           // the ceiling wanted (0: none)
  bool keyboardWake = false;     // gpio95 may wake the display now
  bool keyboardLatch = false;    // PianoKeyboard may arm the gpio95 latch for a system sleep (0062 bit 16)
};

inline View view(const Config &c, const Inputs &in, bool seenOpen)
{
  View v;
  if (!c.lid) {
    v.closeWhy = "piano-policy-lid=0: the cover is not acted on";
  } else if (c.lidConflict) {
    v.closeWhy = "piano-keyboard-lid is set: PianoKeyboard tells IOPMrootDomain the clamshell";
  } else if (in.lid == Lid::unknown) {
    v.closeWhy = "no hall reading (PianoKeyboard absent or piano-keyboard-hall=0)";
  } else {
    v.close = Close::lock;
    if (in.power == Power::external) v.closeWhy = "on external power: lock only";
    else if (in.power == Power::unknown) v.closeWhy = "power source unknown (piano-power-ps=1 publishes it): lock only";
    else if (!in.sleepOn) v.closeWhy = "no system sleep (k27 0062 with piano-sleep=1): lock only";
    else if (!(in.wakeMask & kWakeLid)) v.closeWhy = "the cover-open wake is not armed (piano-sleep-wake lacks 2): lock only";
    else if (!seenOpen) v.closeWhy = "the cover has not been seen open since boot: lock only";
    else {
      v.close = Close::sleep;
      v.closeWhy = "on battery: sleep";
    }
  }
  if (in.lpm) {
    v.profile = Profile::saver;
    v.profileWhy = "Low Power Mode";
  } else if (in.power == Power::external) {
    v.profile = Profile::balanced;
    v.profileWhy = "on external power";
  } else {
    v.profile = Profile::saver;
    v.profileWhy = in.power == Power::battery ? "on battery" : "power source unknown";
  }
  v.gpuKhz = c.profile && v.profile == Profile::saver && c.gpuNeeded ? c.gpuKhz : 0;
  v.keyboardWake = c.keyboard && in.screen == Screen::off && in.lid == Lid::open && in.attached;
  v.keyboardLatch = in.lid == Lid::open && in.attached && in.sleepOn && (in.wakeMask & kWakeKeyboard);
  return v;
}

enum class Kind : uint8_t { root, userActive, thermalProfile, gpuCeiling };
struct Action {
  Kind kind;
  uint32_t value;      // root: the message; thermalProfile: the mode; gpuCeiling: kHz (0 clears); userActive: unused
  const char *why;
  uint32_t client;     // gpuCeiling: PianoA830's slot (kGpuClientShared: param2 null, kGpuClientPolicy: its own)
};
struct Actions {
  static constexpr unsigned kMax = 8;
  Action a[kMax];
  unsigned n = 0;
  void add(Kind k, uint32_t v, const char *why, uint32_t client = kGpuClientShared)
  {
    if (n < kMax) a[n++] = Action{k, v, why, client};
  }
};

// What the shell reports back for a call to another driver.
enum class Result : uint8_t { taken, absent, unsupported };

class Engine {
 public:
  explicit Engine(const Config &c) : c_(c) {}
  const Config &config() const { return c_; }
  bool seenOpen() const { return seenOpen_; }
  uint32_t gpuWritten() const { return gpuWritten_; }
  bool gpuHeld() const { return gpuHeld_; }
  // PianoA830 keeps a slot for us (seen in an input or in the slot a write went to); once seen, for the rest of the boot.
  bool gpuOwnSlot() const { return ownSlot_; }
  bool profileUnsupported() const { return profileUnsupported_; }

  // After any input change (and on the tick): brings what IOPMrootDomain, PianoThermal and PianoA830 were told in line with the
  // inputs. Each action is sent (or, at level 1, logged) in order; then gpuDone / profileDone for the calls to other drivers.
  Actions step(const Inputs &in, uint64_t nowMs)
  {
    Actions out;
    if (in.lid == Lid::open) seenOpen_ = true;
    if (in.screen != screen_) {
      if (in.screen == Screen::off) screenOffMs_ = nowMs;
      else keyboardWoke_ = false;
      screen_ = in.screen;
    }
    const View v = view(c_, in, seenOpen_);
    if (pianopolicy::gpuOwnSlot(in.gpuClients)) ownSlotSeen();
    if (v.close != Close::none) {
      // The order matters: clamshell sleep refused before the AC adaptor appears (plug-in), the AC adaptor gone before clamshell
      // sleep is allowed (unplug: IOPMrootDomain re-evaluates the closed clamshell then and sleeps), both before the clamshell state.
      const bool allow = v.close == Close::sleep;
      if (!allow && clamshell_ != 0) {
        out.add(Kind::root, kDisableClamshell, v.closeWhy);
        clamshell_ = 0;
      }
      if (in.power != Power::unknown) {
        const int ac = in.power == Power::external ? 1 : 0;
        if (ac_ != ac) {
          out.add(Kind::root, kSetAcAdaptorConnected | (ac ? kSetValue : 0), ac ? "external power" : "on battery");
          ac_ = ac;
        }
      }
      if (allow && clamshell_ != 1) {
        out.add(Kind::root, kEnableClamshell, v.closeWhy);
        clamshell_ = 1;
      }
      const int closed = in.lid == Lid::closed ? 1 : 0;
      if (lid_ != closed) {
        const bool opening = lid_ == 1 && !closed;
        out.add(Kind::root, closed ? kClamshellClosed : kClamshellOpened, closed ? v.closeWhy : "cover open");
        if (opening) out.add(Kind::userActive, 0, "cover open");
        lid_ = closed;
      }
    }
    if (c_.profile) {
      const uint32_t mode = in.lpm ? kThermalSaver : kThermalAuto;
      if (!profileUnsupported_ && mode != profileSent_)
        out.add(Kind::thermalProfile, mode, in.lpm ? "Low Power Mode: Power Saver whatever the power source"
                                                    : "PianoThermal's own choice by the power source");
      gpu(out, in, v.gpuKhz);
    }
    return out;
  }

  // The shell's answer to a thermalProfile action.
  void profileDone(uint32_t mode, Result r)
  {
    if (r == Result::taken) profileSent_ = mode;
    else if (r == Result::unsupported) profileUnsupported_ = true;
  }

  // The shell's answer to a gpuCeiling action: CLIENT the slot the write went to (the shell asks the very PianoA830 it calls for
  // piano-gpu-ceiling-clients, so a PianoA830 published between the inputs and the call still gets client 1). In the shared slot,
  // with PianoThermal's state read right after the write: a passive trip that came between the read before the write and the
  // write itself lost its ceiling to ours, so it is put back (a further action). In our own slot nothing is ever put back.
  Actions gpuDone(uint32_t khz, Result r, Thermal after, uint32_t client = kGpuClientShared)
  {
    Actions out;
    if (r != Result::taken) return out;
    if (client == kGpuClientPolicy) ownSlotSeen();
    gpuWritten_ = khz;
    if (ownSlot_) return out;
    if (c_.thermalOwnsGpu && after >= Thermal::passive && khz != c_.thermalGpuKhz) {
      out.add(Kind::gpuCeiling, c_.thermalGpuKhz, "PianoThermal went passive during the write: its ceiling put back");
      gpuHeld_ = true;
    }
    return out;
  }
  void gpuRestored(uint32_t khz, Result r)
  {
    if (r == Result::taken) gpuWritten_ = khz;
  }

  // The power key's short press (PianoInput's "piano-policy-power-key"): sleep now with the display on (or not known), a display
  // wake with it off; without system sleep, or at level 1, or for a long press, macOS's own power key (PianoInput's HID usage).
  Key powerKey(const Inputs &in, uint32_t heldMs, Actions &out, const char **why) const
  {
    if (!c_.act) return *why = "piano-policy=1: decisions only", Key::passThrough;
    if (!c_.powerKey) return *why = "piano-policy-powerkey=0", Key::passThrough;
    if (heldMs >= kLongPressMs) return *why = "a long press is macOS's", Key::passThrough;
    if (!in.sleepOn) return *why = "no system sleep: macOS's own power key", Key::passThrough;
    if (in.screen == Screen::off) {
      out.add(Kind::userActive, 0, "power key");
      return *why = "the display is off: wake it", Key::wakeDisplay;
    }
    out.add(Kind::root, kSleepNow, "power key");
    return *why = in.screen == Screen::on ? "the display is on: sleep now" : "display state unknown: sleep now", Key::sleep;
  }

  // gpio95 went high (PianoKeyboard's piano-keyboard-status 0 -> 1). One display wake per display-off period.
  bool keyboardRise(const Inputs &in, uint64_t nowMs, Actions &out, const char **why)
  {
    const View v = view(c_, in, seenOpen_);
    if (!c_.keyboard) return *why = "piano-policy-kbd=0", false;
    if (in.screen != Screen::off) return *why = in.screen == Screen::on ? "the display is on" : "display state unknown", false;
    if (in.lid != Lid::open) return *why = in.lid == Lid::closed ? "the cover is closed" : "no hall reading", false;
    if (!v.keyboardWake) return *why = "no cover attached", false;
    if (screen_ != Screen::off || nowMs - screenOffMs_ < c_.holdoffMs) return *why = "within the hold-off after the display went off", false;
    if (keyboardWoke_) return *why = "a wake was already asked for", false;
    keyboardWoke_ = true;
    out.add(Kind::userActive, 0, "keyboard");
    return *why = "display off, cover open: wake the display", true;
  }

  // How often the shell must look again without an event, in ms (0: no tick), and why. The display state while a keyboard wake
  // could come, only where the display sends no message per change (a NativeGopDisplay from before docs/policy.md 5.6: no
  // piano-display-power-messages and no message seen) or piano-policy-display-tick=1 asks for the old tick: the hold-off is then timed from the turn that first saw the display off, so it is between holdoffMs and holdoffMs + kTickMs.
  // With the messages the display's change is an event itself: the hold-off is timed from the change, no tick. PianoThermal's
  // state while a GPU ceiling of ours sits in the slot it shares (PianoThermal clears the slot when it leaves passive: ours is
  // written again within kTickMs); with our own slot the GPU needs no tick.
  static constexpr uint32_t kTickMs = 5000;
  enum class Tick : uint8_t { none, displayPoll, gpuShared };
  Tick tickWhy(const Inputs &in) const
  {
    if (c_.keyboard && in.attached && in.lid == Lid::open && in.screen != Screen::unknown && (c_.displayTick || !in.displayEvents))
      return Tick::displayPoll;
    if (c_.profile && c_.thermalOwnsGpu && !ownSlot_ && (gpuWritten_ || gpuHeld_)) return Tick::gpuShared;
    return Tick::none;
  }
  uint32_t tickMs(const Inputs &in) const { return tickWhy(in) == Tick::none ? 0 : kTickMs; }

 private:
  void ownSlotSeen()
  {
    if (ownSlot_) return;
    ownSlot_ = true;
    // Whatever the shared path had noted is not our slot's: PianoThermal's ceiling is in its own slot from now on, and ours is
    // written afresh (nothing can have been written to a PianoA830 with slots through the shared path: it publishes the property
    // before it registers its service).
    gpuHeld_ = false;
    gpuWritten_ = 0;
  }
  void gpu(Actions &out, const Inputs &in, uint32_t want)
  {
    if (ownSlot_) {
      if (want != gpuWritten_)
        out.add(Kind::gpuCeiling, want, want ? "Power Saver's GPU ceiling" : "Balanced: no GPU ceiling", kGpuClientPolicy);
      return;
    }
    if (c_.thermalOwnsGpu && in.thermal >= Thermal::passive) {
      gpuHeld_ = true;   // PianoThermal's ceiling is in the slot: never written over (nor cleared) while it holds it
      return;
    }
    if (gpuHeld_) {
      gpuHeld_ = false;
      gpuWritten_ = 0;   // PianoThermal cleared the slot when it left passive
    }
    if (want != gpuWritten_) out.add(Kind::gpuCeiling, want, want ? "Power Saver's GPU ceiling" : "Balanced: no GPU ceiling");
  }

  Config c_;
  bool seenOpen_ = false;
  int ac_ = -1, clamshell_ = -1, lid_ = -1;    // what IOPMrootDomain was told (-1 nothing yet)
  uint32_t profileSent_ = kThermalAuto;         // PianoThermal starts by the power source
  bool profileUnsupported_ = false;
  uint32_t gpuWritten_ = 0;                     // no ceiling at boot
  bool gpuHeld_ = false;
  bool ownSlot_ = false;                        // PianoA830's slot 1 is ours (kGpuClientPolicy)
  Screen screen_ = Screen::unknown;
  uint64_t screenOffMs_ = 0;
  bool keyboardWoke_ = false;
};

// machdep.piano_sleep (k27 0062: "on; state 1, poll 100 ms, wake mask 0x13, ..."; "off; ..." without piano-sleep=1; absent before
// 0062): whether system sleep exists, and the kernel's wake mask.
inline void parseSleep(const char *s, bool *on, uint32_t *mask)
{
  *on = false;
  *mask = 0;
  if (!s) return;
  *on = s[0] == 'o' && s[1] == 'n' && s[2] == ';';
  static const char kKey[] = "wake mask ";
  for (const char *p = s; *p; ++p) {
    unsigned i = 0;
    while (kKey[i] && p[i] == kKey[i]) ++i;
    if (kKey[i]) continue;
    p += i;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t v = 0;
    for (;; ++p) {
      const char ch = *p;
      const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
      if (d < 0) break;
      v = v * 16 + uint32_t(d);
    }
    *mask = v;
    return;
  }
}

inline const char *lidName(Lid l) { return l == Lid::open ? "open" : l == Lid::closed ? "closed" : "unknown"; }
inline const char *powerName(Power p) { return p == Power::external ? "external power" : p == Power::battery ? "battery" : "unknown"; }
inline const char *screenName(Screen s) { return s == Screen::on ? "on" : s == Screen::off ? "off" : "unknown"; }
inline const char *closeName(Close c) { return c == Close::sleep ? "sleep" : c == Close::lock ? "lock" : "none"; }
inline const char *profileName(Profile p) { return p == Profile::saver ? "Power Saver" : "Balanced"; }
inline const char *thermalName(Thermal t)
{
  return t == Thermal::normal ? "normal" : t == Thermal::warm ? "warm" : t == Thermal::passive ? "PASSIVE" : t == Thermal::critical ? "CRITICAL" : "unknown";
}
inline const char *rootName(uint32_t m)
{
  switch (m) {
    case kSleepNow: return "kIOPMSleepNow";
    case kClamshellClosed: return "kIOPMClamshellClosed";
    case kClamshellOpened: return "kIOPMClamshellOpened";
    case kDisableClamshell: return "kIOPMDisableClamshell";
    case kEnableClamshell: return "kIOPMEnableClamshell";
    case kSetAcAdaptorConnected: return "kIOPMSetACAdaptorConnected (none)";
    case kSetAcAdaptorConnected | kSetValue: return "kIOPMSetACAdaptorConnected (connected)";
    default: return "?";
  }
}

}  // namespace pianopolicy

#endif
