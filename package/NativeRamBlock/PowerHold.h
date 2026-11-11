// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// UniXNU-piano: the Data disk's part of a system sleep (native-ufs-suspend=1; NativeUfs is the disk's power parent, so IOPMrootDomain
// takes the disk down before the UFS link and brings it up after: docs/power.md §4, docs/drivers/ufs.md §12).
//
// off() runs under the disk's gate, so no request is half done. Every request already completed is on the medium (the store's writes
// are FUA: PersistOverlay.h writes a run of pages, then the bitmap blocks that name them, both durable before the request completes);
// the store's SYNCHRONIZE CACHE then makes sure of anything the device might still cache. After it new requests wait (held) until
// on(). A sync that fails vetoes the link's suspend (NativeUfs leaves the link up: its suspend is aborted) and the disk keeps
// running, unheld, so this sleep goes on as without native-ufs-suspend. A second off() without an on() changes nothing.
//   Store: bool sync(); void veto(const char *who);
struct DataPowerHold {
  bool held = false;
  uint32_t holds = 0, syncs = 0, vetoes = 0;

  // storeWritable: the store is open read-write (the disk writes to UFS); otherwise there is nothing on UFS to sync.
  template <class Store> bool off (Store &store, bool storeWritable)
  {
    if (held) return true;
    if (storeWritable) {
      if (!store.sync ()) {
        ++vetoes;
        store.veto ("NativeRamBlock: the Data store's cache sync failed");
        return false;
      }
      ++syncs;
    }
    held = true;
    ++holds;
    return true;
  }
  // Whether requests may be waiting (the caller wakes them).
  bool on ()
  {
    const bool was = held;
    held = false;
    return was;
  }
};
