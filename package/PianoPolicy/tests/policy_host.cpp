// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoPolicy's state machine (Policy.h): every combination of cover x power source x display x Low Power Mode x
// system sleep (absent, on without the cover-open wake, on with it) against the rules; then the sequences IOPMrootDomain,
// PianoThermal and PianoA830 are sent (start, close and open on battery and on power, plugging and unplugging with the cover
// closed, a boot with the cover closed, the PianoKeyboard conflict, the power key, gpio95 with its hold-off, Low Power Mode on and
// off, PianoThermal's passive trip around the shared GPU ceiling, a missing PianoThermal call), and machdep.piano_sleep's parse.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. policy_host.cpp -o /tmp/policy_host && /tmp/policy_host
#include "Policy.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace pianopolicy;
static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The actions as one string: "root:kIOPMEnableClamshell user gpu:342000 profile:1", in order.
static std::string str(const Actions &a)
{
  std::string s;
  for (unsigned i = 0; i < a.n; ++i) {
    if (!s.empty()) s += ' ';
    const Action &x = a.a[i];
    switch (x.kind) {
      case Kind::root: s += std::string("root:") + rootName(x.value); break;
      case Kind::userActive: s += "user"; break;
      case Kind::thermalProfile: s += "profile:" + std::to_string(x.value); break;
      case Kind::gpuCeiling: s += "gpu:" + std::to_string(x.value); break;
    }
  }
  return s;
}

static Config actLid()
{
  Config c;
  c.act = true;
  c.lid = true;
  return c;
}
static Inputs base()
{
  Inputs in;
  in.lid = Lid::open;
  in.attached = true;
  in.power = Power::battery;
  in.screen = Screen::on;
  in.sleepOn = true;
  in.wakeMask = kWakePowerKey | kWakeLid | kWakeKeyboard;
  in.thermal = Thermal::normal;
  return in;
}
// A step whose profile and GPU calls all succeed (the shell's answers), returning what was sent.
static std::string run(Engine &e, const Inputs &in, uint64_t now = 0)
{
  const Actions a = e.step(in, now);
  for (unsigned i = 0; i < a.n; ++i) {
    if (a.a[i].kind == Kind::thermalProfile) e.profileDone(a.a[i].value, Result::taken);
    if (a.a[i].kind == Kind::gpuCeiling) e.gpuDone(a.a[i].value, Result::taken, in.thermal);
  }
  return str(a);
}

// ------------------------------------------------------------------------------------------------------------- the table
static void table()
{
  const Lid lids[] = {Lid::unknown, Lid::open, Lid::closed};
  const Power powers[] = {Power::unknown, Power::battery, Power::external};
  const Screen screens[] = {Screen::unknown, Screen::on, Screen::off};
  for (int lidOn = 0; lidOn < 2; ++lidOn)
    for (Lid lid : lids)
      for (Power power : powers)
        for (Screen screen : screens)
          for (int lpm = 0; lpm < 2; ++lpm)
            for (int sleep = 0; sleep < 3; ++sleep)
              for (int seen = 0; seen < 2; ++seen)
                for (int attached = 0; attached < 2; ++attached)
                  for (int gpuNeeded = 0; gpuNeeded < 2; ++gpuNeeded) {
                    Config c = actLid();
                    c.lid = lidOn;
                    c.gpuNeeded = gpuNeeded;
                    Inputs in;
                    in.lid = lid;
                    in.power = power;
                    in.screen = screen;
                    in.lpm = lpm;
                    in.attached = attached;
                    in.sleepOn = sleep > 0;
                    in.wakeMask = kWakePowerKey | kWakeKeyboard | (sleep == 2 ? kWakeLid : 0);
                    const View v = view(c, in, seen);
                    // The cover: acted on only with the opt-in and a hall reading; sleeps only on battery with sleep, the
                    // cover-open wake and the cover seen open; every other case locks only.
                    if (!lidOn || lid == Lid::unknown) CHECK(v.close == Close::none);
                    else if (power == Power::battery && sleep == 2 && seen) CHECK(v.close == Close::sleep);
                    else CHECK(v.close == Close::lock);
                    CHECK(*v.closeWhy);
                    // The profile: Power Saver with Low Power Mode or off external power, Balanced on it without.
                    CHECK(v.profile == ((lpm || power != Power::external) ? Profile::saver : Profile::balanced));
                    CHECK(v.gpuKhz == (v.profile == Profile::saver && gpuNeeded ? 342000u : 0u));
                    // gpio95: only with the display off, the cover open and attached; the latch for a suspend only with the cover
                    // open and attached, system sleep on and its bit 16.
                    CHECK(v.keyboardWake == (screen == Screen::off && lid == Lid::open && attached));
                    CHECK(v.keyboardLatch == (lid == Lid::open && attached && sleep > 0));
                    // The power key: sleep with the display on or unknown, a display wake with it off, macOS's without sleep.
                    Engine e(c);
                    Actions out;
                    const char *why = "";
                    const Key k = e.powerKey(in, 200, out, &why);
                    if (!in.sleepOn) CHECK(k == Key::passThrough && out.n == 0);
                    else if (screen == Screen::off) CHECK(k == Key::wakeDisplay && str(out) == "user");
                    else CHECK(k == Key::sleep && str(out) == "root:kIOPMSleepNow");
                    Actions longPress;
                    CHECK(e.powerKey(in, 1500, longPress, &why) == Key::passThrough && longPress.n == 0);
                    // A first step never sleeps the system at once: no Enable together with a Closed unless the cover was open.
                    const std::string first = run(e, in);
                    if (lid == Lid::closed) CHECK(first.find("kIOPMEnableClamshell") == std::string::npos);
                  }
  // No keyboard wake, no latch without the cover's attach bit; none with the feature off.
  Config c = actLid();
  c.keyboard = false;
  Inputs in = base();
  in.screen = Screen::off;
  CHECK(!view(c, in, true).keyboardWake);
}

// ------------------------------------------------------------------------------------------------------------- sequences
static void coverOnBattery()
{
  Engine e(actLid());
  Inputs in = base();
  // Start, cover open, on battery: the AC adaptor gone, then clamshell sleep allowed, then the state.
  CHECK(run(e, in) == "root:kIOPMSetACAdaptorConnected (none) root:kIOPMEnableClamshell root:kIOPMClamshellOpened");
  CHECK(run(e, in) == "");
  // Close: IOPMrootDomain sleeps (clamshell sleep allowed).
  in.lid = Lid::closed;
  CHECK(run(e, in) == "root:kIOPMClamshellClosed");
  // Open after the wake: the state and a user-active request.
  in.lid = Lid::open;
  CHECK(run(e, in) == "root:kIOPMClamshellOpened user");
  // Plug in with the cover open: clamshell sleep refused before the adaptor shows.
  in.power = Power::external;
  CHECK(run(e, in) == "root:kIOPMDisableClamshell root:kIOPMSetACAdaptorConnected (connected)");
  // Close on power: the state only (lock, no sleep).
  in.lid = Lid::closed;
  CHECK(run(e, in) == "root:kIOPMClamshellClosed");
  CHECK(view(e.config(), in, e.seenOpen()).close == Close::lock);
  // Unplug with the cover closed: the adaptor gone, then clamshell sleep allowed: IOPMrootDomain re-evaluates and sleeps.
  in.power = Power::battery;
  CHECK(run(e, in) == "root:kIOPMSetACAdaptorConnected (none) root:kIOPMEnableClamshell");
  // Plug in while closed (a USB-C wake): refused first.
  in.power = Power::external;
  CHECK(run(e, in) == "root:kIOPMDisableClamshell root:kIOPMSetACAdaptorConnected (connected)");
  // The power source disappears: nothing about the adaptor; clamshell sleep refused (it already is).
  in.power = Power::unknown;
  CHECK(run(e, in) == "");
}

static void bootClosed()
{
  // A boot with the cover closed on battery: lock only until the cover has been open once.
  Engine e(actLid());
  Inputs in = base();
  in.lid = Lid::closed;
  CHECK(run(e, in) == "root:kIOPMDisableClamshell root:kIOPMSetACAdaptorConnected (none) root:kIOPMClamshellClosed");
  in.lid = Lid::open;
  CHECK(run(e, in) == "root:kIOPMEnableClamshell root:kIOPMClamshellOpened user");
  in.lid = Lid::closed;
  CHECK(run(e, in) == "root:kIOPMClamshellClosed");
}

static void noSleep()
{
  // Without system sleep, or without the cover-open wake, the cover locks only, on battery too.
  for (int which = 0; which < 2; ++which) {
    Engine e(actLid());
    Inputs in = base();
    if (which) in.wakeMask = kWakePowerKey;
    else in.sleepOn = false;
    CHECK(run(e, in) == "root:kIOPMDisableClamshell root:kIOPMSetACAdaptorConnected (none) root:kIOPMClamshellOpened");
    in.lid = Lid::closed;
    CHECK(run(e, in) == "root:kIOPMClamshellClosed");
  }
}

static void lidOff()
{
  // piano-policy-lid=0, the PianoKeyboard conflict, no hall: IOPMrootDomain is never told about the cover or the adaptor.
  for (int which = 0; which < 3; ++which) {
    Config c = actLid();
    if (which == 0) c.lid = false;
    if (which == 1) c.lidConflict = true;
    Engine e(c);
    Inputs in = base();
    if (which == 2) in.lid = Lid::unknown;
    CHECK(run(e, in).find("root:") == std::string::npos);
    in.lid = Lid::closed;
    if (which == 2) in.lid = Lid::unknown;
    in.power = Power::external;
    CHECK(run(e, in).find("root:") == std::string::npos);
  }
  // The hall appears later (PianoKeyboard published after PianoPolicy): from then on.
  Engine e(actLid());
  Inputs in = base();
  in.lid = Lid::unknown;
  CHECK(run(e, in) == "");
  in.lid = Lid::open;
  CHECK(run(e, in) == "root:kIOPMSetACAdaptorConnected (none) root:kIOPMEnableClamshell root:kIOPMClamshellOpened");
}

static void keyboard()
{
  Engine e(actLid());
  Inputs in = base();
  run(e, in, 0);
  Actions out;
  const char *why = "";
  // Display on: ignored.
  CHECK(!e.keyboardRise(in, 100, out, &why) && out.n == 0);
  // Display off at t = 1000: within the hold-off, then a wake once.
  in.screen = Screen::off;
  run(e, in, 1000);
  CHECK(!e.keyboardRise(in, 1500, out, &why) && out.n == 0);
  CHECK(e.keyboardRise(in, 2000, out, &why) && str(out) == "user");
  Actions again;
  CHECK(!e.keyboardRise(in, 2500, again, &why) && again.n == 0);
  // Cover closed while off: never.
  in.lid = Lid::closed;
  run(e, in, 3000);
  Actions closed;
  CHECK(!e.keyboardRise(in, 9000, closed, &why) && closed.n == 0);
  // Detached: never.
  in.lid = Lid::open;
  in.attached = false;
  run(e, in, 10000);
  Actions detached;
  CHECK(!e.keyboardRise(in, 20000, detached, &why) && detached.n == 0);
  // Display on, then off again: a new period, a new wake after the hold-off.
  in.attached = true;
  in.screen = Screen::on;
  run(e, in, 21000);
  in.screen = Screen::off;
  run(e, in, 22000);
  Actions next;
  CHECK(e.keyboardRise(in, 23500, next, &why) && str(next) == "user");
  // Display state unknown: never (nothing to time the hold-off by).
  Engine u(actLid());
  Inputs un = base();
  un.screen = Screen::unknown;
  run(u, un, 0);
  Actions unknown;
  CHECK(!u.keyboardRise(un, 99000, unknown, &why) && unknown.n == 0);
  // The tick: while a keyboard wake can come (attached, open, display state known), every 5 s.
  CHECK(e.tickMs(in) == Engine::kTickMs);
  CHECK(u.tickMs(un) == 0);
}

static void profile()
{
  Config c = actLid();
  c.gpuNeeded = true;
  Engine e(c);
  Inputs in = base();
  in.power = Power::external;
  // On power without Low Power Mode: Balanced, nothing to send (PianoThermal by the power source, no ceiling at boot).
  std::string s = run(e, in);
  CHECK(s.find("profile:") == std::string::npos && s.find("gpu:") == std::string::npos);
  // Low Power Mode on: Power Saver on power too.
  in.lpm = true;
  CHECK(run(e, in) == "profile:1 gpu:342000");
  // Off again: back to PianoThermal's own choice, no ceiling.
  in.lpm = false;
  CHECK(run(e, in) == "profile:0 gpu:0");
  // On battery: PianoThermal's own choice already is Power Saver; the ceiling.
  in.power = Power::battery;
  s = run(e, in);
  CHECK(s.find("profile:") == std::string::npos && s.find("gpu:342000") != std::string::npos);
  // Without gpuNeeded (PianoA830's own maximum is 342 MHz): no GPU call ever.
  Engine q(actLid());
  Inputs b = base();
  CHECK(run(q, b).find("gpu:") == std::string::npos);
  b.lpm = true;
  CHECK(run(q, b).find("gpu:") == std::string::npos);
  // The profile part off: nothing.
  Config off = actLid();
  off.profile = false;
  off.gpuNeeded = true;
  Engine o(off);
  b.lpm = true;
  s = run(o, b);
  CHECK(s.find("profile:") == std::string::npos && s.find("gpu:") == std::string::npos);
}

static void profileUnsupported()
{
  // PianoThermal without "piano-thermal-profile": asked once, then never again; a missing service is asked again.
  Engine e(actLid());
  Inputs in = base();
  in.power = Power::external;
  in.lpm = true;
  Actions a = e.step(in, 0);
  CHECK(str(a).find("profile:1") != std::string::npos);
  e.profileDone(1, Result::absent);
  a = e.step(in, 0);
  CHECK(str(a).find("profile:1") != std::string::npos);
  e.profileDone(1, Result::unsupported);
  CHECK(e.profileUnsupported());
  in.lpm = false;
  CHECK(str(e.step(in, 0)).find("profile:") == std::string::npos);
}

static void thermalSharedSlot()
{
  Config c = actLid();
  c.gpuNeeded = true;
  c.thermalOwnsGpu = true;
  Engine e(c);
  Inputs in = base();
  CHECK(run(e, in).find("gpu:342000") != std::string::npos);
  CHECK(e.gpuWritten() == 342000);
  CHECK(e.tickMs(in) == Engine::kTickMs);
  // PianoThermal goes passive (its 160 MHz in the slot): not written over, and a change to Balanced does not clear it.
  in.thermal = Thermal::passive;
  CHECK(run(e, in).find("gpu:") == std::string::npos);
  CHECK(e.gpuHeld());
  in.power = Power::external;
  CHECK(run(e, in).find("gpu:") == std::string::npos);
  in.power = Power::battery;
  // Back below passive (PianoThermal cleared the slot): ours again.
  in.thermal = Thermal::warm;
  CHECK(run(e, in).find("gpu:342000") != std::string::npos);
  // The race: passive came between the read and the write: PianoThermal's ceiling put back.
  Engine r(c);
  Inputs ri = base();
  Actions a = r.step(ri, 0);
  CHECK(str(a).find("gpu:342000") != std::string::npos);
  const Actions back = r.gpuDone(342000, Result::taken, Thermal::passive);
  CHECK(str(back) == "gpu:160000");
  r.gpuRestored(160000, Result::taken);
  CHECK(r.gpuHeld() && r.gpuWritten() == 160000);
  ri.thermal = Thermal::passive;
  CHECK(str(r.step(ri, 0)).find("gpu:") == std::string::npos);
  ri.thermal = Thermal::normal;
  CHECK(str(r.step(ri, 0)).find("gpu:342000") != std::string::npos);
  // PianoA830 not there yet: asked again at the next step.
  Engine m(c);
  Inputs mi = base();
  Actions first = m.step(mi, 0);
  CHECK(str(first).find("gpu:342000") != std::string::npos);
  m.gpuDone(342000, Result::absent, Thermal::normal);
  CHECK(str(m.step(mi, 0)).find("gpu:342000") != std::string::npos);
  // Thermal tick without the keyboard: 5 s while our ceiling is in the slot.
  Config nk = c;
  nk.keyboard = false;
  Engine t(nk);
  Inputs ti = base();
  run(t, ti);
  CHECK(t.tickMs(ti) == Engine::kTickMs);
  Config nt = nk;
  nt.thermalOwnsGpu = false;
  Engine t2(nt);
  run(t2, ti);
  CHECK(t2.tickMs(ti) == 0);
}

static void observe()
{
  // Level 1: the same decisions (the shell logs them as "would"), the power key always macOS's.
  Config c = actLid();
  c.act = false;
  Engine e(c);
  Inputs in = base();
  CHECK(run(e, in) == "root:kIOPMSetACAdaptorConnected (none) root:kIOPMEnableClamshell root:kIOPMClamshellOpened");
  Actions out;
  const char *why = "";
  CHECK(e.powerKey(in, 100, out, &why) == Key::passThrough && out.n == 0);
}

static void parse()
{
  bool on = false;
  uint32_t mask = 0;
  parseSleep("on; state 1, poll 100 ms, wake mask 0x13, max 0 s; suspends 0 (aborted 0)", &on, &mask);
  CHECK(on && mask == 0x13);
  parseSleep("off; state 1, poll 100 ms, wake mask 0x1, max 0 s", &on, &mask);
  CHECK(!on && mask == 1);
  parseSleep("not built\n", &on, &mask);
  CHECK(!on && mask == 0);
  parseSleep(nullptr, &on, &mask);
  CHECK(!on && mask == 0);
  parseSleep("on; wake mask 0X1f", &on, &mask);
  CHECK(on && mask == 0x1f);
}

int main()
{
  table();
  coverOnBattery();
  bootClosed();
  noSleep();
  lidOff();
  keyboard();
  profile();
  profileUnsupported();
  thermalSharedSlot();
  observe();
  parse();
  if (failures) {
    std::printf("policy_host: %d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("policy_host: all %d checks passed\n", checks);
  return 0;
}
