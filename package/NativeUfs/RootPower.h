// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: the UFS side of the piano power contract (package/include/PianoPowerContract.h, docs/power.md §4,
// docs/drivers/ufs.md §12). Included after RootQueue.h and RootCommands.h; shared with the host harness (tests/), whose port
// supplies the MMIO, the clock, the locks, IOSleep, hostPowerMicros, hostPowerLog and hostPowerPanic.
//
//   native-ufs-suspend=1  a system sleep (setPowerState off): hold the queue, drain it, SYNCHRONIZE CACHE when a window takes
//                         writes, auto-hibernate off, the link into Hibern8; a wake (on): Hibern8 exit, a NOP OUT, auto-hibernate
//                         as before, the queue released. Any failure before the link is down aborts NativeUfs's part: everything is
//                         put back as it was (link active, queue released) and the system sleeps with UFS up, today's behaviour. A
//                         wake that cannot bring the link back is logged at every attempt, retried, and ends in a panic (the root
//                         disk is gone; every acknowledged write is on the medium: FUA, and the sync before the link went down).
//   native-ufs-ahit=MS    runtime auto-hibernate: the controller puts the link into Hibern8 after MS ms without a command and
//                         brings it back by itself at the next doorbell. Without it AHIT stays 0 (hostWakeLink's bring-up value).
//
// Facts (UFSHCI as JEDEC JESD223 describes it; Linux ufshcd.c / ufs-qcom.c read as the specification only, nothing copied):
//   AHIT 0x18: bits 9:0 the idle timer, bits 12:10 its scale (0 1 us, 1 10 us, 2 100 us, 3 1 ms, 4 10 ms, 5 100 ms); 0 disables
//   auto-hibernate. CAP 0x00 bit 23 (AUTOH8): the controller has it. The controller enters Hibern8 only with no transfer, task or
//   UIC command outstanding, and leaves it for a doorbell; with AHIT 0 a link in Hibern8 stays there until DME_HIBERNATE_EXIT.
//   UIC 0x17 DME_HIBERNATE_ENTER / 0x18 DME_HIBERNATE_EXIT: UCCS (IS bit 10) with UCMDARG2[7:0] = 0, then UHES (IS bit 6) / UHXS
//   (bit 5), and HCS.UPMCRS (bits 10:8) = 1 PWR_LOCAL (what ufshcd_uic_pwr_ctrl requires of both); entered with the doorbells clear.
//   MPHY TX_FSM_STATE (attribute 0x41, TX lane 0) = 1: the lane is in HIBERN8 (ufs-qcom's check of a hibernated link).
// NativeUfs holds no clock, GDSC, regulator, RPMh or interconnect vote: UEFI's UFS driver left the controller, the PHY and their
// clocks on, and nothing here writes GCC, TCSR, the PHY or the PMIC (GccUfs.h). In Hibern8 they stay as UEFI left them
// (docs/drivers/ufs.md §12.3 lists them).

enum class UfsPower : uint32_t { On = 0, Suspended = 1, Failed = 2 };
// Why the last suspend was aborted (2026-11-12, docs/power.md §3.7): native-ufs-suspend-cancel=1 asks the kernel (k27 0064,
// piano-sleep-cancel=1) to cancel the whole system sleep for the kinds a sleep must not happen in: a power child's veto (the Data
// store not synced: data not yet safe), I/O still in flight, the cache not synced, a link that could not go into Hibern8 (or whose
// state could not be read). A precondition (the transport not up, quarantined, a panic) is no reason: the disk is no worse asleep.
enum class UfsAbort : uint8_t { None = 0, Precondition, Veto, InFlight, Sync, Link };
static inline bool ufsCancelWanted (UfsAbort kind)
{
  return kind == UfsAbort::Veto || kind == UfsAbort::InFlight || kind == UfsAbort::Sync || kind == UfsAbort::Link;
}
static inline const char *ufsAbortName (UfsAbort kind)
{
  return kind == UfsAbort::Veto ? "the Data store is not synced" : kind == UfsAbort::InFlight ? "I/O in flight" :
         kind == UfsAbort::Sync ? "the cache is not synced" : kind == UfsAbort::Link ? "the link cannot enter Hibern8" :
         kind == UfsAbort::Precondition ? "the transport is not usable" : "none";
}
struct UfsPowerStatus {
  UfsPower state = UfsPower::On;
  UfsAbort abortKind = UfsAbort::None;   // the last suspend's abort (None after a suspend that went down)
  uint32_t suspends = 0, aborts = 0, resumes = 0, retries = 0, vetoes = 0;
  uint32_t ahitRuntime = 0;          // AHIT while the system runs: 0 (auto-hibernate off, today's) or native-ufs-ahit's
  uint32_t ahitSaved = 0;            // AHIT as the last suspend found it (restored by the wake)
  uint32_t txBefore = 0xffffffff, txAfter = 0xffffffff;
  bool entered = false;              // the last suspend sent DME_HIBERNATE_ENTER (false: auto-hibernate had the link there)
  const char *lastStep = "", *lastWhy = "";
  uint64_t drainUs = 0, syncUs = 0, enterUs = 0, exitUs = 0, held = 0;
};
static UfsPowerStatus gPower;
// A power child's refusal (NativeRamBlock: its store did not sync): the next suspend leaves the link up and logs it.
static const char *volatile gPowerVeto = nullptr;

static constexpr uint32_t kDrainMs = 1000;           // commands admitted before the hold: they finish, or the suspend is aborted
static constexpr unsigned kResumeAttempts = 5;
static constexpr uint32_t kResumeBackoffMs[kResumeAttempts] = { 0, 10, 50, 200, 1000 };   // before each attempt
static constexpr uint32_t kTxLane0Fsm = 0x00410000U, kTxHibern8 = 1;

// ---- Auto-hibernate timer -----------------------------------------------------------------------------------------
// AHIT for MS milliseconds without a command: the finest scale that holds it (1 ms steps up to 1023 ms, 10 ms up to 10230, 100 ms
// up to 102300), rounded up so the link never sleeps earlier than asked. 0 (refused) outside [kAhitMinMs, kAhitMaxMs].
static constexpr uint32_t kAhitMinMs = 10, kAhitMaxMs = 100000;
static constexpr uint32_t ufsAhitEncode (uint32_t ms)
{
  return ms < kAhitMinMs || ms > kAhitMaxMs ? 0U :
         ms <= 1023 ? (3U << 10) | ms :
         ms <= 10230 ? (4U << 10) | ((ms + 9) / 10) :
         (5U << 10) | ((ms + 99) / 100);
}
// The idle time an AHIT value stands for, in microseconds (0 for 0 or a reserved scale).
static constexpr uint64_t ufsAhitMicros (uint32_t ahit)
{
  return ((ahit >> 10) & 7) > 5 ? 0 :
         uint64_t (ahit & 0x3ff) * (((ahit >> 10) & 7) == 0 ? 1 : ((ahit >> 10) & 7) == 1 ? 10 : ((ahit >> 10) & 7) == 2 ? 100 :
                                    ((ahit >> 10) & 7) == 3 ? 1000 : ((ahit >> 10) & 7) == 4 ? 10000 : 100000);
}
static_assert (ufsAhitEncode (9) == 0 && ufsAhitEncode (10) == 0xc0a && ufsAhitEncode (1023) == 0xfff, "1 ms scale");
static_assert (ufsAhitEncode (1024) == 0x1067 && ufsAhitEncode (10230) == 0x13ff, "10 ms scale, rounded up");
static_assert (ufsAhitEncode (10231) == 0x1467 && ufsAhitEncode (100000) == 0x17e8 && ufsAhitEncode (100001) == 0, "100 ms scale");
static_assert (ufsAhitMicros (ufsAhitEncode (150)) == 150000 && ufsAhitMicros (ufsAhitEncode (1500)) == 1500000, "round trip");

// At the end of the bring-up: the runtime timer (native-ufs-ahit=MS, 0 none). The controller must have auto-hibernate and take the
// value back; anything else leaves AHIT 0 and says why.
static void hostAutoHibernateStart (uint32_t ms)
{
  if (!ms) return;
  const uint32_t cap = read32 (gHost.hc, 0x00), ahit = ufsAhitEncode (ms);
  if (!(cap & (1U << 23))) {
    hostPowerLog ("auto-hibernate: native-ufs-ahit=%u refused: the controller has no auto-hibernate (CAP %08x bit 23 clear); AHIT stays 0",
                  ms, cap);
    return;
  }
  if (!ahit) {
    hostPowerLog ("auto-hibernate: native-ufs-ahit=%u refused: outside %u..%u ms; AHIT stays 0", ms, kAhitMinMs, kAhitMaxMs);
    return;
  }
  hostWrite (0x18, ahit);
  const uint32_t back = read32 (gHost.hc, 0x18);
  if (back != ahit) {
    hostWrite (0x18, 0);
    hostPowerLog ("auto-hibernate: native-ufs-ahit=%u refused: AHIT %08x read back %08x; AHIT 0 again", ms, ahit, back);
    return;
  }
  gPower.ahitRuntime = ahit;
  hostPowerLog ("auto-hibernate: on (native-ufs-ahit=%u): AHIT %08x, the controller takes the link into Hibern8 after %llu ms "
                "without a command and back at the next one", ms, ahit, (unsigned long long) (ufsAhitMicros (ahit) / 1000));
}

// ---- The queue hold -----------------------------------------------------------------------------------------------
static void hostHoldSet (bool on)
{
  IOLockLock (gSlotLock);
  gHostHeld = on;
  if (on) gPower.held = gHoldWaits;
  else IOLockWakeup (gSlotLock, &gHostHeld, false);
  IOLockUnlock (gSlotLock);
}

// Under the hold: wait until every command admitted before it has given its slot back (no slot taken, nobody queued for one, no
// doorbell bit of ours outstanding). Bounded; *waitedMs the time it took.
static bool hostDrain (uint32_t timeoutMs, uint32_t *waitedMs)
{
  const uint32_t all = gSlotCount > 1 ? (1U << (gSlotCount - 1)) - 1 : 1;
  for (uint32_t ms = 0;; ++ms) {
    IOLockLock (gSlotLock);
    const bool idle = gSlotFree == all && !gSlotWaitHead && !__atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE);
    IOLockUnlock (gSlotLock);
    if (idle) { *waitedMs = ms; return true; }
    if (ms >= timeoutMs) { *waitedMs = ms; return false; }
    IOSleep (1);
  }
}

// TX lane 0's FSM state through DME_GET; false (and *why) when the UIC command fails.
static bool hostTxState (uint32_t *tx, const char **why)
{
  *tx = 0xffffffff;
  if (!hostLinkCommand (0x01, kTxLane0Fsm, why)) return false;
  *tx = read32 (gHost.hc, 0x9c);
  return true;
}

// The link back for the queue (one attempt, under gHost.lock): Hibern8 exit when lane 0 reads HIBERN8, lane 0 out of it, and a NOP
// OUT through slot 0 (the transport end to end: the device answers).
static bool hostLinkUp (const char **step, const char **why)
{
  uint32_t tx = 0;
  gPower.txBefore = gPower.txAfter = 0xffffffff;
  *step = "link state";
  if (!hostTxState (&tx, why)) return false;
  gPower.txBefore = tx;
  if (tx == kTxHibern8) {
    *step = "Hibern8 exit";
    if (!hostLinkCommand (0x18, 0, why)) return false;
    *step = "link state after the exit";
    if (!hostTxState (&tx, why)) return false;
    gPower.txAfter = tx;
    if (tx == kTxHibern8) { *why = "TX lane 0 still in HIBERN8"; return false; }
  }
  gPower.txAfter = tx;
  *step = "NOP OUT";
  if (hostNop () != kIOReturnSuccess) {
    *why = gHost.poisoned ? "no answer (the transport is quarantined)" : "rejected";
    return false;
  }
  return true;
}

// The link back with a wake's attempts, each failure logged with the registers; a quarantined transport (a command timed out: the
// controller may still own its buffers) ends them at once. Then the running AHIT again. false: the link is not back.
static bool hostLinkRestore (unsigned number, const char *what)
{
  for (unsigned attempt = 0; attempt < kResumeAttempts; ++attempt) {
    if (kResumeBackoffMs[attempt]) IOSleep (kResumeBackoffMs[attempt]);
    const char *step = "", *why = "";
    const uint64_t t0 = mach_absolute_time ();
    IOLockLock (gHost.lock);
    const bool up = hostLinkUp (&step, &why);
    IOLockUnlock (gHost.lock);
    if (up) {
      gPower.exitUs = hostPowerMicros (mach_absolute_time () - t0);
      gPower.retries += attempt;
      if (gPower.ahitSaved) {
        hostWrite (0x18, gPower.ahitSaved);
        if (read32 (gHost.hc, 0x18) != gPower.ahitSaved)
          hostPowerLog ("%s %u: AHIT %08x did not read back (%08x): auto-hibernate stays off", what, number, gPower.ahitSaved,
                        read32 (gHost.hc, 0x18));
      }
      return true;
    }
    gPower.lastStep = step;
    gPower.lastWhy = why;
    hostPowerLog ("%s %u: attempt %u of %u FAILED at %s: %s (IS %08x HCS %08x, TX lane 0 %x -> %x)%s", what, number, attempt + 1,
                  kResumeAttempts, step, why, read32 (gHost.hc, 0x20), read32 (gHost.hc, 0x30), gPower.txBefore, gPower.txAfter,
                  gHost.poisoned ? "; the transport is quarantined: no more attempts" : attempt + 1 < kResumeAttempts ? "; retrying" : "");
    if (gHost.poisoned) break;
  }
  return false;
}

static void hostLinkLost (unsigned number, const char *what)
{
  gPower.state = UfsPower::Failed;
  static char text[300];
  snprintf (text, sizeof (text), "the UFS link did not come back (%s %u): %s: %s; IS %08x HCS %08x doorbell %08x; %u commands waited "
            "for it. Every write acknowledged before the sleep is on the medium.", what, number, gPower.lastStep, gPower.lastWhy,
            read32 (gHost.hc, 0x20), read32 (gHost.hc, 0x30), read32 (gHost.hc, 0x58), unsigned (gHoldWaits - gPower.held));
  hostPowerPanic (text);
}

// ---- System sleep (setPowerState off / on) ---------------------------------------------------------------------------
// true: the link is in Hibern8 with the queue held. false: NativeUfs's part is aborted and undone (queue released, AHIT and the link
// as before), or (state Failed) the undo itself could not bring the link back, which panics in the kernel.
static bool hostSuspend (void)
{
  if (gPower.state == UfsPower::Suspended) return true;
  if (gPower.state != UfsPower::On) return false;
  const unsigned number = ++gPower.suspends;
  const uint64_t t0 = mach_absolute_time ();
  const char *step = "preconditions", *why = nullptr;
  bool linkTouched = false;
  uint32_t drained = 0, tx = 0xffffffff;
  UfsAbort kind = UfsAbort::Precondition;
  gPower.abortKind = UfsAbort::None;
  gPower.entered = false;
  gPower.txBefore = gPower.txAfter = 0xffffffff;
  if (const char *veto = gPowerVeto) {
    gPowerVeto = nullptr;
    ++gPower.vetoes;
    why = veto;
    kind = UfsAbort::Veto;
  } else if (!gHost.ready || gHost.poisoned || gHostPanic) {
    why = gHost.poisoned ? "the transport is quarantined (an earlier command failed)" : gHostPanic ? "a panic is in progress"
                                                                                          : "the transport is not up";
  }
  if (why) goto aborted;
  step = "queue hold";
  hostHoldSet (true);
  step = "drain";
  kind = UfsAbort::InFlight;
  if (!hostDrain (kDrainMs, &drained)) { why = "commands still running after 1000 ms"; goto release; }
  gPower.drainUs = drained * 1000ULL;
  if (read32 (gHost.hc, 0x58) || read32 (gHost.hc, 0x78)) { why = "a doorbell is set with the queue drained"; goto release; }
  kind = UfsAbort::Link;
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) { why = "the controller reports a fatal error"; goto release; }
  step = "cache sync";
  kind = UfsAbort::Sync;
  if (hostAnyWritable ()) {
    const uint64_t s0 = mach_absolute_time ();
    if (hostSyncOn (ufsSlot (0)) != kIOReturnSuccess) {
      why = gHost.poisoned ? "SYNCHRONIZE CACHE got no answer (the transport is quarantined)" : "SYNCHRONIZE CACHE failed";
      goto release;
    }
    gPower.syncUs = hostPowerMicros (mach_absolute_time () - s0);
  } else {
    gPower.syncUs = 0;
  }
  IOLockLock (gHost.lock);   // one UIC command at a time (native_ufs_dme_get)
  kind = UfsAbort::Link;
  step = "auto-hibernate off";
  gPower.ahitSaved = read32 (gHost.hc, 0x18);
  if (gPower.ahitSaved) {
    hostWrite (0x18, 0);
    if (read32 (gHost.hc, 0x18)) { why = "AHIT did not read back 0"; IOLockUnlock (gHost.lock); goto restoreAhit; }
  }
  step = "link state";
  // A DME_GET that fails changes nothing on the link: AHIT back, the queue released.
  if (!hostTxState (&tx, &why)) { IOLockUnlock (gHost.lock); goto restoreAhit; }
  gPower.txBefore = tx;
  if (tx != kTxHibern8) {
    const uint64_t e0 = mach_absolute_time ();
    step = "Hibern8 enter";
    linkTouched = true;
    gPower.entered = true;
    if (!hostLinkCommand (0x17, 0, &why)) { IOLockUnlock (gHost.lock); goto relink; }
    step = "link state after the enter";
    if (!hostTxState (&tx, &why)) { IOLockUnlock (gHost.lock); goto relink; }
    if (tx != kTxHibern8) { why = "TX lane 0 is not in HIBERN8 after the enter"; IOLockUnlock (gHost.lock); goto relink; }
    gPower.enterUs = hostPowerMicros (mach_absolute_time () - e0);
  } else {
    gPower.enterUs = 0;
  }
  gPower.txAfter = tx;
  IOLockUnlock (gHost.lock);
  gPower.state = UfsPower::Suspended;
  hostPowerLog ("suspend %u: queue held and drained in %u ms, %s, auto-hibernate %s, link %s (TX lane 0 %x -> %x) in %llu us; "
                "%llu us in all", number, drained,
                hostAnyWritable () ? "cache synced" : "nothing writable to sync",
                gPower.ahitSaved ? "off for the sleep" : "was off",
                gPower.entered ? "into Hibern8" : "already in Hibern8 (auto-hibernate)", gPower.txBefore, gPower.txAfter,
                (unsigned long long) gPower.enterUs, (unsigned long long) hostPowerMicros (mach_absolute_time () - t0));
  return true;

relink:
  // The link may be in Hibern8 (or between): bring it back as a wake does before the queue is released.
  gPower.lastStep = step;
  gPower.lastWhy = why;
  hostPowerLog ("suspend %u: %s FAILED: %s (IS %08x HCS %08x TX %x): bringing the link back", number, step, why,
                read32 (gHost.hc, 0x20), read32 (gHost.hc, 0x30), tx);
  if (!hostLinkRestore (number, "suspend undo")) {
    hostLinkLost (number, "suspend undo");
    return false;
  }
  goto release;
restoreAhit:
  if (gPower.ahitSaved) hostWrite (0x18, gPower.ahitSaved);
release:
  hostHoldSet (false);
aborted:
  ++gPower.aborts;
  gPower.abortKind = kind;
  gPower.lastStep = step;
  gPower.lastWhy = why;
  char link[64];
  if (linkTouched) snprintf (link, sizeof (link), "up again (TX lane 0 %x -> %x, NOP OUT answered)", gPower.txBefore, gPower.txAfter);
  else snprintf (link, sizeof (link), "untouched");
  hostPowerLog ("suspend %u ABORTED at %s: %s (IS %08x HCS %08x doorbells %x/%x): queue released, link %s; this sleep goes on with "
                "UFS up", number, step, why, gHost.hc ? read32 (gHost.hc, 0x20) : 0, gHost.hc ? read32 (gHost.hc, 0x30) : 0,
                gHost.hc ? read32 (gHost.hc, 0x58) : 0, gHost.hc ? read32 (gHost.hc, 0x78) : 0, link);
  return false;
}

// The wake: nothing to do unless the suspend left the link in Hibern8; else the link back (attempts logged), then the queue.
static bool hostResume (void)
{
  if (gPower.state != UfsPower::Suspended) return gPower.state == UfsPower::On;
  const unsigned number = gPower.suspends;
  const uint64_t t0 = mach_absolute_time ();
  if (!hostLinkRestore (number, "resume")) {
    hostLinkLost (number, "resume");
    return false;
  }
  ++gPower.resumes;
  gPower.state = UfsPower::On;
  hostHoldSet (false);
  hostPowerLog ("resume %u: link out of Hibern8 (TX lane 0 %x -> %x), NOP OUT answered, auto-hibernate %s, queue released (%llu "
                "commands had waited) in %llu us", number, gPower.txBefore, gPower.txAfter, gPower.ahitSaved ? "on again" : "off",
                (unsigned long long) (gHoldWaits - gPower.held), (unsigned long long) hostPowerMicros (mach_absolute_time () - t0));
  return true;
}
