// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: RootPower.h (native-ufs-suspend=1, native-ufs-ahit=MS) against the model (UfsModel.h): the auto-hibernate encoding and
// its refusals; a suspend and a wake (hold, drain, the cache sync only with a writable window and before the link, AHIT off for the
// sleep and back after, Hibern8 enter or the controller's own, the exit, a NOP, the queue released, a command that arrived in the
// sleep run after the link is back and never into Hibern8); every abort before the link (veto, quarantine, drain timeout, a foreign
// doorbell, the sync refused or unanswered, AHIT stuck, the UIC not ready) leaving the link untouched and the queue released; every
// failure at or after the enter bringing the link back first; the undo or the wake that cannot: each attempt logged, the backoff, the
// panic with the queue still held; the panic path not blocked by the hold.
#include "UfsModel.h"

static std::array<uint8_t, 131072> out;
using Events = std::vector<std::string>;

static bool has (const std::string &text, const char *needle) { return text.find (needle) != std::string::npos; }

// The link up, the queue free, AHIT as given: what the system runs with. A read goes through and never meets a hibernated link
// without auto-hibernate.
static void assertUp (uint32_t ahit = 0)
{
  assert (!gHostHeld && gPower.state == UfsPower::On && m.txState != 1 && reg (0x18) == ahit && !gHost.poisoned && gPanics == 0);
  const unsigned before = m.doorbellInHibern8;
  assert (hostRead4k (0, 3, 1, out.data ()) == 0 && out[0] == uint8_t (0x7b + m.lastSlot));
  assert (m.doorbellInHibern8 == before);
}

static void autoHibernate ()
{
  assert (ufsAhitEncode (150) == ((3U << 10) | 150) && ufsAhitEncode (1000) == 0xfe8);
  assert (ufsAhitEncode (5) == 0 && ufsAhitEncode (200000) == 0);
  assert (ufsAhitMicros (ufsAhitEncode (10231)) == 10300000);    // rounded up to the 100 ms step
  assert (ufsAhitMicros (0x1c01) == 0);                           // scale 7: reserved
  reset (); hostAutoHibernateStart (0);
  assert (reg (0x18) == 0 && m.events.empty () && gPowerLines.empty ());   // no argument: today's (AHIT stays 0)
  reset (); hostAutoHibernateStart (1000);
  assert (reg (0x18) == 0xfe8 && gPower.ahitRuntime == 0xfe8 && linesContain ("auto-hibernate: on (native-ufs-ahit=1000): AHIT 00000fe8"));
  // The controller's timer put the link into Hibern8 between commands: the next doorbell takes it out by itself.
  autoHibernated ();
  assert (hostRead4k (0, 3, 1, out.data ()) == 0 && m.autoExits == 1 && m.txState != 1 && m.doorbellInHibern8 == 0);
  assertUp (0xfe8);
  reset (); reg (0x00) &= ~(1U << 23); hostAutoHibernateStart (1000);
  assert (reg (0x18) == 0 && gPower.ahitRuntime == 0 && linesContain ("the controller has no auto-hibernate"));
  reset (); hostAutoHibernateStart (5);
  assert (reg (0x18) == 0 && linesContain ("outside 10..100000 ms"));
  reset (Fault::AutoHibernate); hostAutoHibernateStart (1000);
  assert (reg (0x18) == 0 && gPower.ahitRuntime == 0 && linesContain ("read back 00000000"));
  puts ("PASS: auto-hibernate: AHIT encoding (1/10/100 ms scales, rounded up, bounds), refused without AUTOH8, out of range or not "
        "read back; a link the timer hibernated leaves Hibern8 for the next doorbell");
}

static void suspendAndWake ()
{
  // Nothing writable: no sync. DME_GET, DME_HIBERNATE_ENTER, DME_GET.
  reset ();
  assert (hostSuspend ());
  assert (gHostHeld && gPower.state == UfsPower::Suspended && m.txState == 1 && m.syncs == 0 && gPower.entered);
  assert ((m.events == Events { "get", "uic17", "get" }));
  assert (linesContain ("suspend 1: queue held and drained in 0 ms, nothing writable to sync, auto-hibernate was off, link into "
                        "Hibern8 (TX lane 0 5 -> 1)"));
  assert (hostSuspend () && m.enterCommands == 1);   // a second off: nothing more
  // A read issued in the sleep waits on the hold; the other thread is the wake. It runs after the link is back.
  bool woke = false;
  m.onSleep = [&] (uintptr_t event) { assert (event == uintptr_t (&gHostHeld) && !woke); woke = true; assert (hostResume ()); };
  assert (hostRead4k (0, 5, 1, out.data ()) == 0);
  assert (woke && gHoldWaits == 1 && m.exitCommands == 1 && m.nops == 1 && m.doorbellInHibern8 == 0);
  assert ((m.events == Events { "get", "uic17", "get", "get", "uic18", "get", "nop", "read" }));
  assert (linesContain ("resume 1: link out of Hibern8 (TX lane 0 1 -> 5), NOP OUT answered, auto-hibernate off, queue released (1 "
                        "commands had waited)"));
  m.onSleep = nullptr;
  assert (hostResume () && m.exitCommands == 1);     // a second on: nothing more
  assertUp ();
  // Three more sleeps in a row.
  for (unsigned k = 0; k < 3; ++k) { assert (hostSuspend () && hostResume ()); assertUp (); }
  assert (gPower.suspends == 4 && gPower.resumes == 4 && gPower.aborts == 0 && m.enterCommands == 4 && m.exitCommands == 4);

  // A writable window (the Data store): the sync after the drain, before the link.
  reset ();
  assert (hostStoreOpen (100, 64));
  { std::vector<uint8_t> data (4096, 0x42); assert (hostWrite4k (0, 100, 1, data.data ()) == 0); }
  m.events.clear ();
  assert (hostSuspend ());
  assert (m.syncs == 1 && (m.events == Events { "sync", "get", "uic17", "get" }) && linesContain ("cache synced"));
  assert (hostResume ());
  assertUp ();

  // Auto-hibernate on: AHIT 0 for the sleep (before any UIC command), back after the NOP.
  reset ();
  hostAutoHibernateStart (1000);
  m.events.clear ();
  assert (hostSuspend () && gPower.ahitSaved == 0xfe8 && reg (0x18) == 0);
  assert ((m.events == Events { "ahit=0", "get", "uic17", "get" }) && linesContain ("auto-hibernate off for the sleep"));
  assert (hostResume ());
  assert ((m.events == Events { "ahit=0", "get", "uic17", "get", "get", "uic18", "get", "nop", "ahit=fe8" }));
  assertUp (0xfe8);
  // The timer had the link in Hibern8 already: no enter; the wake exits all the same.
  autoHibernated ();
  m.events.clear ();
  assert (hostSuspend () && !gPower.entered && m.enterCommands == 1 && (m.events == Events { "ahit=0", "get" }));
  assert (linesContain ("link already in Hibern8 (auto-hibernate)"));
  assert (hostResume () && m.exitCommands == 2);
  assertUp (0xfe8);
  puts ("PASS: suspend and wake: hold, drain, sync only when writable and before the link, AHIT 0 for the sleep and back after, "
        "Hibern8 enter (or the timer's), exit, NOP, release; a command of the sleep waits and runs after the link is back");
}

static void abortsBeforeTheLink ()
{
  // A power child's veto (NativeRamBlock: its store did not sync): only that sleep.
  reset ();
  gPowerVeto ="NativeRamBlock: the Data store's cache sync failed";
  assert (!hostSuspend () && gPower.aborts == 1 && gPower.vetoes == 1 && !gHostHeld && m.events.empty () && !gPowerVeto);
  assert (linesContain ("suspend 1 ABORTED at preconditions: NativeRamBlock: the Data store's cache sync failed") &&
          linesContain ("queue released, link untouched; this sleep goes on with UFS up"));
  assert (hostResume () && m.events.empty ());       // the wake after it: nothing to undo
  assert (hostSuspend () && hostResume ());
  assertUp ();
  // A quarantined transport, a panic in progress.
  reset (); gHost.poisoned = true;
  assert (!hostSuspend () && m.events.empty () && !gHostHeld && linesContain ("the transport is quarantined"));
  reset (); gHostPanic = true;
  assert (!hostSuspend () && m.events.empty () && !gHostHeld && linesContain ("a panic is in progress"));
  // A command that keeps its slot past the drain's 1000 ms.
  reset ();
  const int slot = ufs_test_slot_take ();
  assert (slot >= 0);
  uint64_t t0 = m.now;
  assert (!hostSuspend () && !gHostHeld && m.events.empty () && m.now - t0 >= 1000000);
  assert (linesContain ("ABORTED at drain: commands still running after 1000 ms"));
  ufs_test_slot_give (unsigned (slot));
  assert (hostSuspend () && hostResume ());
  assertUp ();
  // A doorbell bit that is not ours (a task) with the pool drained.
  reset (); reg (0x78) = 1;
  assert (!hostSuspend () && m.events.empty () && !gHostHeld && linesContain ("a doorbell is set with the queue drained"));
  reg (0x78) = 0;
  assertUp ();
  // The sync refused (CHECK CONDITION): the transport stays usable, the link untouched.
  reset (Fault::SyncCheck);
  assert (hostStoreOpen (100, 64));
  assert (!hostSuspend () && m.syncs == 1 && m.getCommands == 0 && !gHostHeld && !gHost.poisoned);
  assert (linesContain ("ABORTED at cache sync: SYNCHRONIZE CACHE failed"));
  m.fault = Fault::None;
  assertUp ();
  // The sync never answers: quarantined as any command that times out; the link untouched, the queue released.
  reset (Fault::SyncTimeout);
  assert (hostStoreOpen (100, 64));
  assert (!hostSuspend () && gHost.poisoned && m.getCommands == 0 && m.enterCommands == 0 && !gHostHeld);
  assert (linesContain ("SYNCHRONIZE CACHE got no answer (the transport is quarantined)"));
  // AHIT does not go to 0: no UIC command, AHIT as it was.
  reset ();
  hostAutoHibernateStart (1000);
  m.fault = Fault::AutoHibernate;
  assert (!hostSuspend () && m.getCommands == 0 && reg (0x18) == 0xfe8 && !gHostHeld && linesContain ("AHIT did not read back 0"));
  // The UIC not ready, or DME_GET refused: AHIT back, the link untouched.
  reset ();
  hostAutoHibernateStart (1000);
  reg (0x30) &= ~8U;
  assert (!hostSuspend () && m.getCommands == 0 && reg (0x18) == 0xfe8 && !gHostHeld);
  assert (linesContain ("ABORTED at link state: UIC command not ready"));
  reg (0x30) |= 8U;
  assertUp (0xfe8);
  reset (Fault::UicResult);
  assert (!hostSuspend () && m.getCommands == 1 && m.enterCommands == 0 && !gHostHeld && linesContain ("ABORTED at link state"));
  m.fault = Fault::None;
  assertUp ();
  puts ("PASS: aborts before the link (veto, quarantine, panic, drain timeout, a foreign doorbell, sync refused or unanswered, AHIT "
        "stuck, UIC not ready or refused): nothing on the link, AHIT as before, the queue released");
}

static void failuresAtTheLink ()
{
  for (Fault f : { Fault::EnterResult, Fault::EnterTimeout, Fault::EnterStatus, Fault::EnterNoChange }) {
    reset (f);
    hostAutoHibernateStart (1000);
    assert (!hostSuspend ());
    assert (!gHostHeld && gPower.state == UfsPower::On && m.txState != 1 && reg (0x18) == 0xfe8 && gPanics == 0);
    assert (m.enterCommands == 1 && m.nops == 1);
    const bool inHibern8 = f == Fault::EnterTimeout || f == Fault::EnterStatus;
    assert (m.exitCommands == (inHibern8 ? 1U : 0U));
    assert (linesContain ("bringing the link back"));
    assert (linesContain (f == Fault::EnterNoChange ? "ABORTED at link state after the enter: TX lane 0 is not in HIBERN8 after the enter"
                                                    : "ABORTED at Hibern8 enter: UIC link command failed"));
    assert (linesContain (inHibern8 ? "link up again (TX lane 0 1 -> 5, NOP OUT answered)"
                                    : "link up again (TX lane 0 5 -> 5, NOP OUT answered)"));
    m.fault = Fault::None;
    assertUp (0xfe8);
    assert (hostSuspend () && hostResume ());   // and the next sleep works
    assertUp (0xfe8);
  }
  // The undo cannot bring the link back: five logged attempts, then the panic, the queue still held (nothing reaches the link).
  reset (Fault::EnterTimeout);
  m.exitBroken = true;
  assert (!hostSuspend () && gPower.state == UfsPower::Failed && gPanics == 1 && gHostHeld && m.exitCommands == 5);
  assert (has (gPanicText, "the UFS link did not come back (suspend undo 1): Hibern8 exit: UIC link command failed"));
  assert (linesContain ("suspend undo 1: attempt 5 of 5 FAILED at Hibern8 exit"));
  puts ("PASS: failures at or after the enter (refused, no UHES, PWR_FATAL, a lane still active): the link brought back (exit when "
        "hibernated, NOP) before the queue is released, AHIT back; an undo that cannot: logged attempts, panic, queue held");
}

static void wakeFailures ()
{
  // One failed exit: retried after 10 ms.
  reset (Fault::ExitFailOnce);
  assert (hostSuspend ());
  uint64_t t0 = m.now;
  assert (hostResume () && gPower.retries == 1 && m.exitCommands == 2 && gPanics == 0);
  assert (linesContain ("resume 1: attempt 1 of 5 FAILED at Hibern8 exit: UIC link command failed") && linesContain ("; retrying"));
  assert (m.now - t0 >= 500000 + 10000);
  assertUp ();
  // The link never comes back: every attempt logged with the backoff between them, then the panic with the queue held.
  reset (Fault::ExitFailAlways);
  assert (hostStoreOpen (100, 64));
  assert (hostSuspend ());
  t0 = m.now;
  assert (!hostResume () && gPanics == 1 && gPower.state == UfsPower::Failed && gHostHeld && m.exitCommands == 5);
  assert (m.now - t0 >= 5 * 500000ULL + 10000 + 50000 + 200000 + 1000000);
  for (unsigned k = 1; k <= 5; ++k) {
    char line[80];
    snprintf (line, sizeof (line), "resume 1: attempt %u of 5 FAILED at Hibern8 exit", k);
    assert (linesContain (line));
  }
  assert (has (gPanicText, "the UFS link did not come back (resume 1): Hibern8 exit: UIC link command failed"));
  assert (has (gPanicText, "Every write acknowledged before the sleep is on the medium"));
  // The device does not answer the NOP after the exit: quarantined, no further attempt, the panic.
  reset (Fault::NopTimeout);
  assert (hostSuspend ());
  assert (!hostResume () && gPanics == 1 && m.nops == 1 && m.exitCommands == 1 && gHost.poisoned);
  assert (linesContain ("resume 1: attempt 1 of 5 FAILED at NOP OUT: no answer (the transport is quarantined) (IS 00000000 HCS "
                        "0000010f, TX lane 0 1 -> 5); the transport is quarantined: no more attempts"));
  puts ("PASS: wake failures: one failed exit retried; a link that never comes back logged at all five attempts with the backoff, "
        "then the panic (queue held); a NOP without answer ends the attempts at once");
}

static void panicPath ()
{
  // The panic path never waits on the hold (it would wait for ever): its command meets the hibernated link and times out.
  reset ();
  assert (hostSuspend ());
  gHostPanic = true;
  assert (hostRead4k (0, 1, 1, out.data ()) == kIOReturnTimeout && m.doorbellInHibern8 == 1 && gHoldWaits == 0);
  puts ("PASS: the panic path is not held (its log write cannot reach a hibernated link and times out instead of blocking)");
}

int main ()
{
  autoHibernate ();
  suspendAndWake ();
  abortsBeforeTheLink ();
  failuresAtTheLink ();
  wakeFailures ();
  panicPath ();
  puts ("PASS: RootPower.h (native-ufs-suspend=1, native-ufs-ahit=MS)");
}
