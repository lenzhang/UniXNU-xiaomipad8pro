// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile the SAME command, queue and power code as the kernel (RootCommands.h, RootQueue.h, RootPower.h). Test-only callbacks
// replace physical I/O, the clock and synchronization, not descriptor, response, link or power logic.
// From ShengUfs b11de87 tests/RootCommandsBridge.cpp and UniXNU-Odin3 6d58c09 tests/RootCommandsBridge.cpp (BSD-2-Clause-Patent,
// blu-sharky; ../UPSTREAM); UniXNU-piano: the slot pool's sleep hook, the hold, RootPower.h's port (IOSleep, hostPowerLog,
// hostPowerPanic), a clock that advances on every read (hostWaitRegister polls without IODelay on SM8750).
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include "../RootDiagnostics.h"
#include "../TransferPoll.h"
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 0xe00002bc, kIOReturnBadArgument = 0xe00002c2, kIOReturnIOError = 0xe00002ca,
                   kIOReturnBusy = 0xe00002d5, kIOReturnTimeout = 0xe00002d6, kIOReturnNotReady = 0xe00002d8;
#include "../RootTransportLayout.h"
constexpr unsigned kMicrosecondScale = 1;
constexpr unsigned kMillisecondScale = 1000;
static UfsRootDiagnostics gRootDiagnostic;
struct Host { uint8_t *virt; uint64_t phys; bool ready, poisoned; uintptr_t hc; void *lock; };
static Host gHost;
alignas(4096) static uint8_t memory[kDmaBytes];
using Callback = uint64_t (*) (unsigned operation, uint64_t address, uint64_t value, void *bytes);
static Callback callback;
// 0: MMIO read; 1: ordered MMIO write; 2: release DMA; 3: acquire DMA; 4: monotonic microseconds (advances on every read);
// 5: delay microseconds; 6: a thread would sleep on `address` (the hold, or the slot pool). Callbacks must not throw.
static uint32_t read32 (uintptr_t, unsigned offset) { return uint32_t (callback (0, offset, 0, nullptr)); }
static void hostWrite (unsigned offset, uint32_t value) { callback (1, offset, value, nullptr); }
static void cacheCivac (const void *start, size_t length)
{
  callback (2, gHost.phys + uint64_t (static_cast<const uint8_t *> (start) - memory), length, const_cast<void *> (start));
}
static void cacheInvalidate (const void *start, size_t length)
{
  callback (3, gHost.phys + uint64_t (static_cast<const uint8_t *> (start) - memory), length, const_cast<void *> (start));
}
static uint64_t mach_absolute_time () { return callback (4, 0, 0, nullptr); }
static void clock_interval_to_deadline (uint32_t interval, unsigned scale, uint64_t *deadline)
{
  *deadline = mach_absolute_time () + uint64_t (interval) * scale;
}
[[maybe_unused]] static void IODelay (uint32_t us) { callback (5, 0, us, nullptr); }
static void IOSleep (uint32_t ms) { callback (5, 0, uint64_t (ms) * 1000, nullptr); }
struct IOLock;
static void IOLockLock (void *) {}
static void IOLockUnlock (void *) {}
static bool IOLockTryLock (void *) { return true; }
constexpr int THREAD_UNINT = 0;
// Single-threaded: a sleeping taker hands control to the model (op 6), which plays the other thread (e.g. the wake that releases the
// hold) and returns; the caller's loop then checks its condition again.
static int IOLockSleep (void *, void *event, int) { callback (6, uint64_t (reinterpret_cast<uintptr_t> (event)), 0, nullptr); return 0; }
static void IOLockWakeup (void *, void *, bool) {}
static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = result;
  gRootDiagnostic.ocs = ocs;
  return result;
}
// Polling port: no completion interrupt.
static uint32_t hostCompletionCount (unsigned) { return 0; }
static bool hostInterruptMode () { return false; }
static void hostWaitCompletion (unsigned, uint32_t, uint64_t) { IODelay (2); }
static uint64_t hostDmaAddress (const void *) { return 0; }   // no zero copy in the host model
#include "../RootQueue.h"
#include "../RootCommands.h"
// RootPower.h's port.
static std::vector<std::string> gPowerLines;
static std::string gPanicText;
static unsigned gPanics;
static uint64_t hostPowerMicros (uint64_t ticks) { return ticks; }
static void hostPowerLog (const char *format, ...) __attribute__ ((format (printf, 1, 2)));
static void hostPowerLog (const char *format, ...)
{
  char line[512];
  va_list ap;
  va_start (ap, format);
  vsnprintf (line, sizeof (line), format, ap);
  va_end (ap);
  gPowerLines.push_back (line);
  if (getenv ("UFS_TEST_VERBOSE")) printf ("  NativeUfs: %s\n", line);
}
static void hostPowerPanic (const char *text)
{
  ++gPanics;
  gPanicText = text;
  if (getenv ("UFS_TEST_VERBOSE")) printf ("  panic: NativeUfs: %s\n", text);
}
#include "../RootPower.h"

static bool linesContain (const char *needle)
{
  for (const std::string &line : gPowerLines)
    if (line.find (needle) != std::string::npos) return true;
  return false;
}

static void ufs_test_reset (Callback cb, uint64_t physical)
{
  callback = cb;
  memset (memory, 0, sizeof (memory));
  gHost = { memory, physical, true, false, 0x1000, nullptr };
  gRootDiagnostic = UfsRootDiagnostics {};
  gStoreOpen = gLogOpen = false;
  gStoreFirst = gStoreBlocks = gLogFirst = gLogBlocks = 0;
  hostSlotGeometryRegionA (kRegionSlots);
  hostSlotsInit (kRegionSlots, nullptr);
  gHostPanic = false;
  gOutstanding = 0;
  gSlotWaitHead = gSlotWaitTail = nullptr;
  gSlotsBusy = 0;
  gHostHeld = false;
  gHoldWaits = 0;
  gPower = UfsPowerStatus {};
  gPowerVeto = nullptr;
  gPowerLines.clear ();
  gPanicText.clear ();
  gPanics = 0;
}
static void ufs_test_slots (unsigned count) { hostSlotGeometryRegionA (count); hostSlotsInit (count, nullptr); }
static int ufs_test_slot_take () { bool pooled; UfsSlot slot = hostSlotTake (&pooled); return pooled ? int (slot.index) : -1 - int (slot.index); }
static void ufs_test_slot_give (unsigned index) { hostSlotGive (ufsSlot (index), true); }
static uint32_t ufs_test_scsi_slot (unsigned slot, uint8_t lun, const uint8_t *cdb, unsigned length, uint32_t bytes, uint32_t *scsi)
{
  return hostScsiDir (ufsSlot (slot), lun, cdb, length, bytes ? 2U : 0U, bytes, 200000, scsi);
}
static bool ufs_test_stop_queue () { const char *why = nullptr; return hostStopQueue (&why); }
static bool ufs_test_start_queue () { const char *why = nullptr; return hostStartQueue (&why); }
static uint32_t ufs_test_scsi (uint8_t lun, const uint8_t *cdb, unsigned length, bool read, uint32_t bytes, uint32_t *scsi)
{
  return hostScsi (lun, cdb, length, read, bytes, 200000, scsi);
}
