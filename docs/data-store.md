# The Data store: format, the store guard, flatten, plugin staging

Status 2026-11-12: section 9 (the store's base and the system layer) is the way an installed ESP changes without losing the
owner's Data: built and host-tested offline, not run on the tablet. Status 2026-11-11: **built and host-tested offline; nothing of it has run on the tablet.** The installed ESP (`esp-daily-macos`,
docs/USING.md §2; before it `esp-gpu-macos`, the same seed `aced5bf9…`) still has the kernel collection from before this change:
it has no store guard and no flatten. The guard protects the owner's store from RAM images built with the new collection; section 7 is the test procedure.

The installed system's Data volume is `package/NativeRamBlock`: a RAM **seed** (the payload's `scratch-data.img`) plus a persistent
**store** of the pages the system changed, in a window at the end of `linux_rootfs` served by `package/NativeUfs`
(`native_ufs_store_open/read/write/sync`). Until this change, a store made for one seed was silently re-initialised by any boot
with another seed (`Data store state 3`, fresh). A rebuilt Turnip plugin, a staged file or a RAM test image with another seed
dropped every Data change. The store at the old place had generation 10 at the install: it had been started over about ten
times that day.

## 1. The pieces

| | What | Where |
|---|---|---|
| seed | packed `DBRDISK1` disk, 12 GiB logical (3145728 pages), about 30 000 nonzero pages kept in RAM (`SeedDisk.h`) | `EFI/UniXNU/Payloads/scratch-data.img` on the ESP or in a RAM image |
| seed identity | `SeedDisk.h seedIdentity`: SHA-256 of `SHNGSEED2`, the size and every nonzero page with its index (plain SHA-256 of the disk up to 2 GiB). `tooling/persist/common.py seed_info` computes the same; checked on the installed seed (`aced5bf9…`) with `Test/seed-identity.cpp` | the store header; printed as 8 hex digits |
| store window | W = 1 + 96 + 3145728 = 3145825 blocks of 4096 bytes, first LBA = `linux_rootfs` end + 1 - 1 GiB reserve - W = **68119455** since the ESP install | LUN 0, inside `linux_rootfs` |
| `data-persist` | 0 off; 1 read only (NativeUfs refuses every store write); 2 read-write (direct mode) | boot argument |

In direct mode (`data-persist=2`, `DirectDisk.h`) a changed page lives only in the window. A page whose bitmap bit is set is read
from the window, any other page from the seed (or as zero). Writes go to the window with FUA: first the run of pages, then the
bitmap blocks that gained bits, both before the write completes.

## 2. Store format (`PersistOverlay.h`)

All integers little endian. Block 0:

| Bytes | Field |
|---|---|
| 0-7 | `SHNGDATA` |
| 8-11 | version 1 |
| 12-15 | header bytes 64 |
| 16-23 | page count |
| 24-55 | seed identity (SHA-256) |
| 56-59 | generation (counts re-initialisations) |
| 60-63 | CRC-32 of bytes 0-59 |
| 64-71 | `SHNGEXT1`: the extension (UniXNU-piano, 2026-11-11); all zero when absent |
| 72-75 | flags: bit 0 `kSelfContained` |
| 76-79 | 0 |
| 80-87 | pages the flatten wrote (diagnostic) |
| 88-123 | 0 |
| 124-127 | CRC-32 of bytes 0-59 followed by bytes 64-123 (`extensionCrc`) |

Blocks 1 to 96 hold the presence bitmap (one bit per page). Page i is at block 97 + i and is valid only when its bit is set.

Compatibility. Bytes 0-63 are the version 1 header every earlier kernel writes and reads. With flags 0 the encoder writes it byte
for byte as before; `Test/guard.cpp` and `tooling/persist/test-data-store.py` check this against a hand-built old header. The
extension's CRC also covers bytes 0-59 (not the CRC field: a CRC over data that ends in its own CRC is a constant, so bytes 0-63
would bind nothing). An extension left behind after something rewrote the version 1 part is therefore void. Old headers resume
unchanged. A kernel without the extension sees a flattened store as an ordinary store of its seed. It reads exactly the same
disk from it, because every nonzero seed page is present.

## 3. The store guard

`Overlay::open` reads block 0 and decides:

| Header on the medium | `data-persist=1` | `=2` | `=2 data-store-replace=1` (or `=0x<its 8 hex>`) | `=2 sheng-data-reset=1\|2` |
|---|---|---|---|---|
| this seed's | resumed, read only (6) | **resumed (2)** | resumed (2): replace acts only on another seed's store | fresh (3), generation + 1 |
| self-contained, same page count, any seed | 6 | **resumed (2)**, the seed is not read | 2 | 3 |
| **another seed's** (or another page count) | kept (7) | **kept (7): untouched, Data volatile** | fresh (3), generation + 1 | 3 |
| blank (no `SHNGDATA`) | blank (1) | fresh (3), generation 1 | 3 | 3 |
| damaged (version, size, CRC) | 1 | fresh (3) | 3 | 3 |

A kept store gets no writes at all. Its bitmap is read only to count its pages, so the console line can report them.
`data-store-replace=0x<8 hex>` replaces only the store whose seed starts with those digits, so an ESP that keeps the argument
can never replace any other store. `sheng-data-reset=1|2` keeps its meaning: start over, whatever the store holds.

States (`native_data_persist_state`): 0 off, 1 blank, 2 resumed, 3 fresh, 4 refused or failed, 5 degraded after a store I/O error
(writes refused), 6 resumed read only (probe), 7 kept. Every state except 2, 3 and 5 runs Data **volatile**.

Volatile mode for the 12 GiB disk is new (`VolatileDisk.h`). Before this change, a seed above 2 GiB had no write path without the
store: its writes failed with `kIOReturnNotReady`, so `data-persist=0` or a refused store meant a Data volume that could not be
written. Pages written now go into RAM: a directory of one pointer per 2 MiB of disk (48 KiB), plus 4 KiB leaves and pages taken
on first write. They are gone at the next boot. The read-only probe (`data-persist=1`) of a large disk also used to dereference
the missing sparse page array once the store was valid. It now reports the store and applies nothing (state 6).

**The volatile disk's memory (`VolatilePool.h`, fixed 2026-11-12).**

The problem. The first version took every page with `IOMallocZero(4096)` inside `doAsyncReadWrite`, with the disk's lock held.
Three attended `data-persist=0` runs on 2026-11-11 hard-reset the tablet 1 to 2 minutes after the login, under Spotlight's first
import: every kernel line stopped at once, and the Gunyah watchdog reset the tablet. The cause:
- That allocation is a kalloc zone allocation, and this kernel's zone maps are bounded (`zone_init: map DATA 2457M`, 1843M per
  general map).
- A zalloc that finds no free page waits for the pageout daemon, and the daemon's writeback of dirty Data pages needs this disk's
  lock.
- Nothing ever failed a write: the system stopped.

`Test/volatile-stress.cpp` part C reproduces that deadlock in a model.

Now:
- **Allocation.** The pages come from 2 MiB chunks (`IOMallocAligned`, which is `kernel_memory_allocate` into the kernel map's
  data range, not a zone). A refill thread allocates them with no lock held and may wait for memory as long as the VM needs. It
  keeps 16 MiB ready, more while a write waits.
- **Writes.** The I/O path takes only pages that are already there. A write first admits every page it may take (its data pages
  and leaves):
  - past the **budget** it is refused at once with `kIOReturnNoSpace`;
  - otherwise it waits at most **2 s** for the refill thread, the lock released, then is refused with `kIOReturnNoMemory`.
  Reads and overwrites of resident pages need nothing.
- **What happens at N GiB.** The disk holds at most the budget: writes needing new pages fail with an error and never wait without
  a bound. What APFS makes of a refused write is APFS's: an error to the writer, possibly a forced unmount or a panic. Either way
  the boot does not hang.
- **Unmap.** The disk advertises `Unmap` (only when volatile; it is decided before the disk is published). APFS's freed blocks
  then go back to the pool, so the disk holds about the live data, not every block APFS ever wrote. Copy-on-write metadata and
  Spotlight's index churn a lot of blocks.

| Boot argument | Default | Meaning |
|---|---|---|
| `data-volatile-mib=N` | a quarter of the memory XNU manages (about 1.9 GiB here), within 256 MiB to 4 GiB | the budget (64 to 65536) |
| `data-volatile-unmap=0` | 1 | do not advertise unmap |

Lines:

```
NativeRamBlock: volatile: budget 1966 MiB (data-volatile-mib), in 2 MiB chunks; writes past it are refused (no space), a write that waits 2000 ms for pages is refused (no memory); unmap on
NativeRamBlock: volatile: N pages, M MiB resident (H MiB held in chunks, peak P MiB, budget B MiB); U pages unmapped; writes refused: X no space, Y no memory; W waits, F chunk failures
```

The second line comes from the refill thread, never from the I/O path. It appears at most every 10 s while the numbers move, and
within a second of a new refusal.

What this does not cover: small seeds (up to 2 GiB: `SparseDisk.h ramSparseIO`) still take a zone page per written page in the lock.
No current seed is that small (all are 12 GiB).

Console lines (`grep NativeRamBlock`):

```
NativeRamBlock: the store belongs to seed aced5bf9 (this seed 469ed440): kept; Data is volatile this boot; data-store-replace=1 starts over
NativeRamBlock: the store was started over on request (data-store-replace or sheng-data-reset): a new store for seed …, generation N
NativeRamBlock: read-only probe: the store's P pages are not applied to a large disk; Data is the seed's this boot
NativeRamBlock: self-contained store (made with seed …, M pages flattened; this seed …): the seed is not read
NativeRamBlock: Data is volatile this boot (state S, data-persist=N): its changes stay in RAM and are gone at the next boot
NativeRamBlock: Data store state S (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header H, first LBA 68119455, 3145728 pages,
                direct D, seed <this>, store seed <store's>, flags F, generation G, present P
```

The old collection's line is `Data store state S (1 blank, 2 resumed, 3 fresh, 4 failed), header H, first LBA L, 3145728 pages,
direct D`. `generation` and `present` describe the store as found (or as made). A boot after a kept one shows the same two
numbers if nothing wrote to the store.

`native-ram-trace=1` (off by default; not a `data-*` name, so `PIANO_EXTRA_ARGS` and `build-esp.py --arg` take it) adds a line
before and after each step of the set-up, `NativeRamBlock: store +<ms> ms: …`: the waits for IOBSD and NativeUfs's root layout,
the GPT check, the window, `header read: SHNGDATA v1, <pages> pages, seed <8 hex>, generation G, flags F` (the store as found,
before any decision), the bitmap read or clear (first block, every 32nd, last), each cache sync, the header write, the disk's
registration ([forensics-2026-11-11.md](forensics-2026-11-11.md) §4). A set-up that stops names its step.

Across a system sleep (`native-ufs-suspend=1`, opt-in, not run yet: [drivers/ufs.md](drivers/ufs.md) §12): with the store open
read-write the disk is NativeUfs's power child; before the link goes into Hibern8 it syncs the store (every acknowledged write is
FUA already) and holds new requests until the link is back; a failed sync keeps the link up for that sleep.

## 4. Flatten: a store that no longer needs its seed

`data-store-flatten=1` (`Overlay::flatten`, `NativeRamBlock::flattenStore`) works only on this seed's store, opened read-write
(`data-persist=2`), and runs in `attachStore` before the disk is published:

1. Every nonzero seed page whose bit is clear goes into the window with its bit, in runs of up to 256 pages (1 MiB) through
   `writeRun`: the pages, then the bitmap blocks that gained bits, each run durable before the next one starts. Pages the system
   already changed are skipped. The disk reads the same at every point of the pass.
2. Then it checks that every seed page is present, and that bytes 0-63 on the medium are the header the store was opened with.
3. Last, block 0 is rewritten: the same bytes 0-63 plus the extension with `kSelfContained`.

From then on the store is **self-contained**. `DirectDisk.h` reads a clear bit as zero, the store is resumed with any seed of its
page count, and a seed only matters when a store is started (blank, `sheng-data-reset`, `data-store-replace`). For the installed
seed, the pass writes at most 30433 pages (118 MiB, 261 extents), about 400 runs.

Power loss. Every run leaves a valid store of the same seed with more pages present, so a cut anywhere means the next boot
resumes the same disk. `data-store-flatten=1` again skips what is there and finishes. Two cases are left to check:
- A torn header write in step 3 can only damage the extension. Bytes 0-63 are rewritten with the bytes they already hold, so the
  store stays a valid store of its seed, just not yet flagged.
- A store error during the pass behaves like any direct-mode write error. The line is `flatten stopped by a store error after N
  pages: the store stays seed …'s and valid; Data writes are off this boot; data-store-flatten=1 again resumes it` (state 5).

`Test/guard.cpp` cuts the power before each of the pass's 125 writes, and inside the header write at 12 byte offsets. After each
cut it checks a same-seed boot (same disk), a second flatten, and a boot with another seed (same disk again).

Older kernels and a flattened store:
- The same seed: an older kernel resumes it as before and sees the same disk.
- Another seed: an older kernel still replaces it, as it replaced every store. The tooling refuses `data-persist=2` for a seed
  that is not known to be the installed one with such a collection (section 6).

After a flatten:
- A test image with **any** seed and `data-persist=2` resumes the owner's Data and writes into it. The guard no longer separates
  them. `build-ram-images.py` defaults another seed's image to `data-persist=0`.
- Files staged into a later seed (a new Turnip plugin, `tu-debug`, boot job changes) **no longer reach the tablet's Data**, which
  keeps the files it has (section 5).
- The store keeps its `/Library/GPUBundles` plugin, so the ESP's trust cache must still hold that plugin's CDHashes, or
  WindowServer cannot load it. `build-esp.py` compares the installed trust cache with the new one and prints a loud note.
  Measured 2026-11-11: the current Turnip staging lacks the installed Turnip's CDHash `eaa367a2…`.

## 5. Plugin staging (part b): evaluated; superseded by section 9 (2026-11-12)

Section 9 replaces the choice below: the store keeps its seed as its base, the plugin is a file of the system layer, and no flatten
is needed for a plugin update. What follows is the evaluation of 2026-11-11; its facts about the plugin's search path and the trust
cache still hold.

How macOS finds the plugin:
- PianoA830's `AGXAccelerator` publishes `MetalPluginName NativeAdrenoMetal`.
- IOGPU looks for `<name>.bundle` first in `/Library/GPUBundles`, then in `/System/Library/Extensions` (`stage-soft-metal.py`,
  as the VM-era `stage-gpu-plugin.py` did). `/Library` is a firmlink into the **Data** volume.
- The plugin's Mach-Os are ad hoc signed. They load only while their CDHashes are in the static trust cache, the ESP's
  `kernel.macho.trustcache`, which AMFI and libsystem_sandbox's `gpu_bundle_find_trusted` consult.

Today `build-native-image.sh` stages the bundle into a copy of the Data seed (`out/turnip-a830/stage`), so every plugin build is a
new seed.

- **(b1) The OS window** (`/System/Library/Extensions` on the root volume, writable in the development build:
  `NATIVE_UFS_UNVERIFIED_ROOT`). Not clean:
  - The Data copy in `/Library/GPUBundles`, which every store has from its seed, is searched first. It would have to be deleted
    once anyway, which is a Data change.
  - Changing the root means changing macOS's sealed system volume in place: offline on the Mac (mounting it read-write breaks
    the seal; the k27 root mounts that volume) or live on the tablet.
  - A mistake there leaves the tablet without a root, whereas a mistake on Data loses only Data. None of it can be tested offline.
- **(b2) A separate "system additions" disk or window.** Not clean:
  - DarwinBoot hands the kernel one RAM disk (`PackedRAMDisk`). A second disk needs a loader change (UniXNU firmware) or a split of
    the seed.
  - Something must also mount it on `/Library/GPUBundles` before WindowServer's first Metal device. That means a launchd job on
    Data, itself a Data change and a race with WindowServer, or a kernel mount, which would be a k27 patch.
  - A symlink to `/Volumes/...` would most likely fail `gpu_bundle_find_trusted`'s path trust and the sandboxed Metal clients'
    read rules.
- **Chosen: (a) flatten, then update the plugin in place.** After a flatten the store is the system's Data like any Mac's, and a
  plugin update becomes a file update on it:
  1. Stage the new bundle's CDHashes into a trust cache that keeps the installed one's modules:
     `stage-soft-metal.py --trust <installed trust cache> --out out/turnip-a830/update <bundle>`. Its `trust_bundle` puts the new
     module first and keeps every input module, so both the old and the new plugin load. Install an ESP with that trust cache
     (`build-esp.py --trust …`, `maint-session.py … esp-final`).
  2. On the running tablet (the root shell over the USB link, `piano-usb-shell=1`, or the network link `piano-usb-net=1`), copy
     the new bundle next to the old one and swap them. Keep the old one as `NativeAdrenoMetal.bundle.prev` for the way back. Then
     reboot.

  Still to build: a host step that pushes and swaps the bundle, with the CDHash check before the swap. Keep staging the current
  plugin into the ESP's seed too: a store that is ever started over (blank, `sheng-data-reset`, `data-store-replace`) begins
  from that seed.

## 6. Tooling

| Tool | Data store behaviour |
|---|---|
| `tooling/bootimg/build-ram-images.py` | `data-persist` of a RAM macOS image (`data_store_args`, note on stderr). Without `PIANO_DATA_PERSIST`: **2 only when the image's seed is the installed one**; another seed gets **0**; an unknown installed seed gets 2 with a collection that has the guard, else 0. `PIANO_DATA_PERSIST=0\|1\|2` overrides, but 2 is refused when the seed is not known to be the installed one and the collection has no guard. `PIANO_DATA_STORE_REPLACE=1\|<8 hex>` and `PIANO_DATA_STORE_FLATTEN=1` (need 2 and the guard). `data-*` in `PIANO_EXTRA_ARGS` is refused, and so is 2 for a seed whose size is not the installed store's (section 8). The build record has `data_store` |
| installed seed | `PIANO_INSTALLED_SEED` (environment or `~/.config/piano/env`): the identity (8 to 64 hex digits) or an ESP build record (`.json`); else `out/persistent/installed.json`, which `maint-session.py` writes with every ESP it installs (and removes on its rollback). For the installed ESP: `PIANO_INSTALLED_SEED=aced5bf9794b42f3c8ffeeef3078a408ff506f5b0b374e02eed6f0ab56e52616` |
| `tooling/k27/build-native-image.sh` | `out/ram` as above; the dump image `out/ram-dump` keeps `data-persist=0` |
| `tooling/persist/base-seed.py` | section 9: `pin` the installed ESP's seed and trust cache as the store's base (`out/base/<identity16>/`, read only, never replaced); `show`, `path`, `diff` |
| `tooling/persist/build-esp.py` | the daily profile takes the pinned base's seed and trust cache (`--seed base --trust base`); `--layer DIR` the system layer's trust cache (section 9). The macos entries keep `data-persist=2`. `--replace-data [1\|HEX8]` adds `data-store-replace` for an intended one-time switch; `--flatten-data` adds `data-store-flatten=1` (a no-op once the store is self-contained). A seed that is not the installed one gets a loud note (with the guard: kept, Data volatile, unless the store is self-contained). With a collection without the guard it is refused unless `--replace-data`. The installed trust cache's CDHashes are checked against the new one (`--installed-trust`). `--arg data-*` is refused. The record has `data_store` |
| `tooling/persist/store-test-images.py` | the RAM images of section 7: `out/ram-store-probe` (`data-persist=1`, reset 90 s), `out/ram-store-guard` (`=2`, another seed, 240 s), `--flatten`: `out/ram-store-flatten` (the installed seed, trust cache and ADT from `--installed-esp`, `data-store-flatten=1`, 180 s) |
| `tooling/persist/maint-session.py` | step `map` reads 128 bytes of each store header (flags, self-contained), and once installed says whether the store at its place is the one the ESP resumes; `esp`/`esp-final` refuse an ESP whose seed is not that store's unless its record has `--replace-data` (section 9); `--store auto` moves a self-contained store whatever the ESP's seed; the RAM tests accept state 7 |

Host tests (offline, about 10 s in all):

```sh
package/NativeRamBlock/Test/run-tests.sh [SEED.img]   # sparse, persist, direct, seeddisk, guard, volatile, volatile-stress, power (+ the seed's identity)
python3 tooling/persist/test-data-store.py            # header reader vs the driver's encoder, installed seed, every tooling decision
python3 tooling/persist/test-persist.py               # the install session against a fake tablet (needs out/persistent's inputs)
```

## 7. Procedure (RAM boots only; nothing here writes the owner's store before step T4)

On the Mac, from a checkout with these commits. The installed ESP image is the one `maint-session.py` wrote, in the persist
checkout's `out/persistent/`:

```sh
grep -q PIANO_INSTALLED_SEED ~/.config/piano/env || \
  echo 'PIANO_INSTALLED_SEED=aced5bf9794b42f3c8ffeeef3078a408ff506f5b0b374e02eed6f0ab56e52616' >> ~/.config/piano/env
PIANO_A830_METAL=0 tooling/k27/build-native-image.sh       # the collection with the guard, out/ram-dump (about 8 min)
python3 tooling/persist/store-test-images.py --installed-esp <persist checkout>/out/persistent/esp-daily-macos.img
```

(`esp-gpu-macos.img` if that is the one installed; both have seed `aced5bf9`, and the tool prints the seed it found.)

The tablet waits in ABL fastboot (from macOS: power + volume-down, 15-20 s). Each run: `tooling/native/native-loop.sh IMAGE`,
then `grep NativeRamBlock logs/native-runs/<time>-loop/xnu-console.txt`.

**T0, probe** (`out/ram-store-probe/boot-piano-macos-ram-A-bundled.img`; seed 469ed440, `data-persist=1`, nothing can be
written). Expected when the store is the installed system's:

```
NativeRamBlock: the store belongs to seed aced5bf9 (this seed 469ed440): kept; Data is volatile this boot; data-store-replace=1 starts over
NativeRamBlock: Data is volatile this boot (state 7, data-persist=1): its changes stay in RAM and are gone at the next boot
NativeRamBlock: Data store state 7 (…), header 3, first LBA 68119455, 3145728 pages, direct 0, seed 469ed440, store seed aced5bf9, flags 0, generation G, present P
```

Write down G and P. Other outcomes:
- `store seed 469ed440`, state 6: a store an earlier RAM test left, not the owner's. Rebuild the guard image with another seed
  (`store-test-images.py … --guard-seed out/turnip-a830/stage/scratch-data.img --guard-trust out/turnip-a830/stage/kernel.macho.trustcache`).
- Any other store seed: an earlier test image replaced the store and the installed system has not booted since; the owner's Data
  is not in it. T1 still tests the guard.
- State 1 (header 1 or 2): there is no store to keep; skip T1.
- No `Data store state` line: stop and read NativeUfs's lines.

**T1, guard** (`out/ram-store-guard/…`; seed 469ed440, `data-persist=2`: the case that used to wipe the store). Expected: the
same three lines with `data-persist=2` in the second one and the **same G and P**. Then the boot goes on with volatile Data
(Data mounted, launchd's boot tasks) until the 240 s reset.

**Stop at once** on `Data store state 3` or `the store was started over`: the guard did not hold. Boot no other image with
`data-persist=2` and keep the log.

**T2, probe again** (the T0 image): the same lines as T0, the same G and P. T1 wrote nothing.

**T3, the installed boot**: `fastboot -s $PIANO_SERIAL reboot`. The owner's macOS (old collection) comes up with its Data as
before.
- With the daily ESP's USB console, `tooling/native/usb-console.py --capture 200` shows
  `NativeRamBlock: Data store state 2 (1 blank, 2 resumed, 3 fresh, 4 failed), header 0, first LBA 68119455, 3145728 pages, direct 1`.
  The console's `reset fastboot` gets back to ABL.
- Without the console (`esp-gpu-macos`): power + volume-down, then T2 once more: `store seed aced5bf9`, the same G (a resume
  never changes it), present at least P.
- If T0 showed another store seed, the installed (old) kernel starts the store over here (state 3), as it always did.

**T4, flatten (optional, tomorrow, with the owner's go-ahead).** It writes the store. A NAS copy of the store region first is
cheap insurance: since 2026-11-12 `maint-session.py maint-boot map store-backup maint-exit` takes one (the store window at its
installed place, verified on the NAS; `rollback store-backup` writes it back).

```sh
python3 tooling/persist/store-test-images.py --installed-esp <…>/esp-daily-macos.img --flatten
tooling/native/native-loop.sh out/ram-store-flatten/boot-piano-macos-ram-A-bundled.img
```

Expected (M at most 30433, a few seconds):

```
NativeRamBlock: flatten: seed aced5bf9, P of 3145728 pages present; writing the seed's other pages into the store
NativeRamBlock: flatten done: M seed pages written now (P present before, P+M now); the store is self-contained: any seed boots it from now on, a seed only starts a new store
NativeRamBlock: Data store state 2 (…), header 0, first LBA 68119455, 3145728 pages, direct 1, seed aced5bf9, store seed aced5bf9, flags 1, generation G, present P+M
```

Then:
- T2: state 6, `self-contained store (made with seed aced5bf9, M pages flattened; this seed 469ed440): the seed is not read`, flags 1.
- The installed boot: state 2, header 0. The old kernel reads the flattened store as before.
- An interrupted flatten (reset, power): boot the flatten image again.
- `flatten stopped by a store error`: stop.

From now on the guard image (`data-persist=2`, any seed) resumes the owner's Data. Do not boot it unless that is meant.

**T5, later (cable session): the guard on the installed system.** Build an ESP from a collection with these commits. Keep the
installed seed and trust cache, so the plugin stays the one the store has:
`PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`, then `build-esp.py --profile daily --default macos` (since section 9 its
default seed and trust cache are the pinned base's: `--seed base --trust base`), then `maint-session.py … esp-final …`
(docs/persistent-boot.md §11). Once the store is self-contained, the ESP's seed may change; the trust cache rule of section 4 still
holds.

**V1, the volatile disk under load (after the 2026-11-11 hangs).** Use the attended desktop image (`ram-attend3`'s arguments:
`data-persist=0`, GPU, USB console) built from a collection with `VolatilePool.h`:

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 PIANO_DATA_PERSIST=0 \
PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 exec-log=1 stderr-log=1 piano-touch-flip=2" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-volatile1 && cp -R out/ram out/ram-volatile1
tooling/native/native-loop.sh out/ram-volatile1/boot-piano-macos-ram-A-bundled.img     # with tooling/native/usb-console.py --capture 1500
```

Log in and use the desktop as in the hung runs (let Spotlight import). Expected in the console:
- `NativeRamBlock: volatile: budget … MiB …; unmap on` once at boot.
- `NativeRamBlock: volatile: N pages, M MiB resident …` every 10 s while Data is written. M should level off below the budget,
  with `U pages unmapped` growing if APFS trims.
- No hard reset over at least 10 minutes. The kernel's periodic lines (PianoDwc3, PianoTouch) keep coming.

If the budget is reached, the line shows `writes refused: X no space` and APFS reports write errors. That is the designed limit,
not a hang. To see the limit on purpose, add `data-volatile-mib=512` to `PIANO_EXTRA_ARGS` for a second image.

## 8. Risks and open items

- Nothing here has run on the tablet. The volatile large disk is new code on the boot path of every boot without the store
  (test images, `data-persist=0`). Its first version hung the tablet under load (section 3). The pool version is host-tested:
  sustained writes, budget, low memory, unmap, threads and the hang's own model (`Test/volatile-stress.cpp`).
  - At the budget, APFS gets write errors.
  - Whether APFS actually sends unmaps to this disk is not yet seen on the tablet. The `pages unmapped` count will tell.
- Until the ESP has the new collection (T5), the installed system itself has no guard. Any RAM image **built before this change**
  with `data-persist=2` and another seed still wipes the store: the installed system's kernel does not protect it, the test image's
  does.
- After a flatten the guard no longer separates test images from the owner's Data (section 4). Staged files no longer reach it,
  and the trust cache must keep the store's plugin CDHashes.
- `data-store-replace=1` left in an ESP replaces any store that is not its seed's (a test image's, after a `sheng-data-reset`
  elsewhere). Prefer `--replace-data <8 hex>`, which names the one store meant.
- A flatten error turns writes off for that boot (state 5, as any store write error). The store stays valid.
- **A seed of another logical size** is the one case the guard cannot see. The window's place depends on its size
  (`first = end - reserve - W`), so such a seed reads its "header" from inside the installed store. That block is blank, so the
  first `data-persist=2` boot initialises a store there, over the owner's pages. All seeds so far are 12 GiB.
  `build-ram-images.py` and `build-esp.py` refuse `data-persist=2` for any other size (`common.INSTALLED_STORE_PAGES`) unless
  `PIANO_DATA_STORE_RELAYOUT=1` (a deliberate re-layout, with `maint-session.py --store fresh` and a backup first).

## 9. The store's base and the system layer (2026-11-12)

Built and host-tested offline; nothing of it has run on the tablet.

### 9.1 What the seed identity is made of

The identity (`SeedDisk.h seedIdentity`, `common.seed_info`) is SHA-256 over `SHNGSEED2`, the logical size, and every nonzero
4 KiB page of the 12 GiB Data disk with its index. The disk is one APFS container with the volume Data. Every byte of it counts:

| Layer | What | Where it comes from |
|---|---|---|
| input seed | the Data volume of the macOS 27.0.1 install (users, preferences, `settime.sh` and its boot job `org.droidvm.run.0`) | `out/inputs/scratch-data.img`, identity `469ed440…` |
| Metal plugin | `/Library/GPUBundles/NativeAdrenoMetal.bundle`: `Contents/Info.plist`, `MacOS/NativeAdrenoMetal` (plugin, frontend, Rust bridge), `MacOS/libvulkan_freedreno.dylib` (Turnip) | `tooling/metal/build-turnip-a830.sh`, staged by `tooling/k27/stage-soft-metal.py` |
| debug file | `/private/var/ngpu/debug/tu-debug` (`PIANO_A830_TU_DEBUG`, default `sysmem`) | `build-native-image.sh` |
| boot job edits | `--log-errors` loops, Reduce transparency (soft Metal), the userland installer's command | `stage-soft-metal.py`, `tooling/userland/stage.py` |
| APFS itself | the container superblock, checkpoints, transaction ids, inode times: every staging mounts the volume read-write | the Mac's APFS during the staging |

The plugin is in the seed because IOGPU looks for `<MetalPluginName>.bundle` in `/Library/GPUBundles` first, and `/Library` is on
Data (section 5). So every plugin build, and every restage even of the same files (APFS writes times and transaction ids), is a new
identity. A seed is therefore never reproduced: neither from another checkout nor from the same one.

The installed base, `8f3331104e03c970` (file `23a5ec59…`, the daily2 ESP), against tonight's Turnip stage with patch 0006
(`f6176467…`), measured with `base-seed.py diff`: **3913 pages differ**: page 0 (the container superblock), 4 pages of the checkpoint
descriptor area, 13 of the checkpoint data area, 11 pages that were zero in the base, and **3884 pages that are in use in the base**.
Those hold the old `libvulkan_freedreno.dylib`: both stagings start from the same input seed, and APFS put the new dylib (the same
17678800 bytes) where the old one is.

### 9.2 Why no block-level migration

The store's disk is the store's page where its bitmap bit is set, else the seed's. Two ways to carry a store over to a new seed:

- **Rebase when the store's pages and the changed pages are disjoint.** Every read-write mount writes page 0 and checkpoints, on
  the tablet as in a staging, so a store's pages and a new seed's changed pages always meet there: the check always refuses.
  Relaxing it to ignore the superblock and checkpoints is unsound. The 3884 changed pages above are file data that the store's
  filesystem still refers to (the base's plugin, unless something rewrote it). A store that never rewrote them passes, and the
  rebased disk then shows the new dylib's bytes under the old file's inode and extents. That happens to be a whole new file here
  (same size, same place), but in general it is a mix of two filesystems: APFS checksums each metadata block, not a tree against
  another tree, and file data not at all.
- **Rebase where the store's pages win** (copy the base's pages the store lacks into it, then relabel). The disk is byte for byte
  what it was, so it is safe. It also delivers nothing: the store's checkpoint is the filesystem, and the new seed's files sit in
  its free space, unreachable. Flatten (section 4) already gives the same seed independence.

Files reach an existing Data volume only through its filesystem, on the running system.

### 9.3 The design

1. **The base is immutable.** The store's seed stays its base. `tooling/persist/base-seed.py pin --esp <installed ESP image>`
   extracts the seed and trust cache with the independent FAT32 reader and checks the identity against `PIANO_INSTALLED_SEED`.
   It writes them read only to `out/base/<identity16>/` (or `PIANO_BASE_DIR`), and never replaces a base. `common.pinned_base`
   checks both files' sha256 and the identity again at every use.
2. **Every ESP that keeps the store carries the base byte for byte.** `build-esp.py --profile daily` takes `--seed base --trust base`
   by default. The kernel collection, the device tree, the arguments and the trust cache are separate payload files and change
   freely.
3. **The files that change are the system layer**: the Metal plugin and our userland. `tooling/userland/stage.py --live DIR
   --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle --installed-trust <the base's trust cache>` builds the bundle
   (`userland-update.tar`, `apply-update.sh`). The plugin is one more unit, `gpu-nativeadrenometal`, at
   `/Library/GPUBundles/NativeAdrenoMetal.bundle/...`, root:wheel. `DIR/kernel.macho.trustcache` is the base's trust cache with one
   module of the layer's new CDHashes first.
4. **The ESP trusts the layer before the layer is installed**: `build-esp.py --layer DIR` takes `DIR`'s trust cache. It refuses a
   layer made against another trust cache, and any trust cache that lacks one of the base's CDHashes while the ESP resumes the
   store, because the resumed Data still has the base's plugin.
5. **The layer goes in as files**, by `install.sh --live` on the running system (`docs/userland.md` §7):
   - Each file is replaced by `install -S`, a new file renamed over the old one, so each file is either the old or the new.
   - Before the installer first takes over a file it did not put there (the base's plugin), it keeps a copy in
     `/private/var/db/piano-userland/prev/<path>`. Without that copy it does not touch the file.
   - When the layer drops the unit, the copies go back. Taking the layer out restores the base's files byte for byte.

Why it is safe:
- The ESP's seed is the store's own, so the guard's existing rule (resume only the store's seed; keep any other untouched, Data
  volatile) decides as before. Nothing in the kernel changes, and no store is ever relabelled.
- The trust cache only grows: the base's modules stay as they are, so the base's plugin and the layer's both load, and no binary
  is ever installed before it is trusted (ESP first, files after).
- APFS sees only file operations of its own system. The base's files can always be restored.
- The install session checks the same rule on the tablet: once installed, `maint-session.py map` reads the store at its place,
  and `esp`/`esp-final` refuse an ESP whose seed is not that store's unless its build record says `--replace-data`
  (`tooling/persist/test-persist.py`).

What it does not do:
- **The first layer installation needs root on the running system.** The base's Data has no installer, no USB root shell and no
  network job: only `settime.sh`. So the first time is the owner's: Terminal, `sudo sh apply-update.sh` with the owner's password
  (`docs/userland.md` §7.5), over the USB network link (`piano-usb-net=1`). After that, `pianousb-shell` is on Data, and an ESP with
  `piano-usb-shell=1` gives later updates without the owner. That ESP argument opens an unauthenticated root shell on the cable, so it
  is the owner's decision.
- Until the layer is in, the system runs the base's plugin (Turnip without 0006) on the new kernel. The kernel ABI is unchanged
  (ABI v2, `20ac9298…`). PianoA830's NoResources fix is kernel-side. 0006's BadArgument fix (a device whose connection lands in
  another slot) waits for the layer.
- RAM test images keep their staged seeds (`out/turnip-a830/stage`, `out/userland/stage`), which run Data volatile against the
  installed store as before.

### 9.4 Reproducible from every checkout

The ESP's seed is the pinned base, the same file from every checkout. The layer's plugin must be the same bytes too, since its
CDHashes are in the trust cache. A worktree on 2026-11-12 built another `libvulkan_freedreno.dylib` (`919c6fb9…` against
`8c6958ba…`):
- Its `out/turnip-a830` had been copied from main, build directory included. Meson's coredata still named main's source tree.
- The `-Dtu-darwin-abi-dir` option names this checkout's `out/turnip-a830/abi`, so it differed and triggered `meson configure`. That
  regenerated every compile command against main's tree: `../../../../../../out/turnip-a830/mesa/src/...` instead of `../mesa/src/...`.
- Mesa's `vk_error`/`mesa_log` embed `__FILE__`, so the 51 longer strings moved everything after them.

Builds from fresh worktrees give main's bytes. Another worktree's fresh build that night gave the same dylib. On 2026-11-12, this
change's own worktree built Mesa from scratch and linked the plugin itself, and all three files equal main's: `NativeAdrenoMetal`
`d4ffaf27…`, `libvulkan_freedreno.dylib` `8c6958ba…`, `Info.plist` `9595a4d2…`. Its staged test seed did not (`f16615ae…` against
main's `f6176467…`, with the same staging stamp): APFS again. Two fixes:
- `build-turnip-a830.sh` sets the build directory up again when meson configured it for another source or build directory.
- Its step 7, `tooling/metal/check-bundle-paths.py`, stops the build on any string in the bundle that names the checkout, a worktree,
  or a source path climbing out of `out/turnip-a830`.

It reports but accepts the one machine-dependent path: the plugin's debug map (N_OSO) names the prebuilt bridge archive in the
k27 tree, the same from every checkout of this Mac. Dropping it (`ld -S`) would change the plugin that runs today, so it is left.

### 9.5 Commands and tests

```sh
python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/esp-daily2-macos.img   # once: out/base/8f3331104e03c970/
python3 tooling/persist/base-seed.py show                       # checked again
python3 tooling/persist/base-seed.py diff out/base/8f3331104e03c970/scratch-data.img out/turnip-a830/stage/scratch-data.img
python3 tooling/userland/stage.py --live out/layer --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle \
        --installed-trust out/base/8f3331104e03c970/kernel.macho.trustcache
python3 tooling/persist/build-esp.py --profile daily --default macos --layer out/layer ...   # docs/persistent-boot.md section 11
PIANO_SEED=$(python3 tooling/persist/base-seed.py path) ...      # a RAM image with the base (build-native-image.sh)
```

Host tests:
- `tooling/persist/test-data-store.py` part 5: pin from files and from an mtools ESP image, the refusals, a changed base, the daily
  profile's inputs, `--layer` and its refusals, `check_trust` for a resuming ESP, `diff` on seeds with an APFS superblock.
- `tooling/userland/test-stage.py` parts 1-2: `--gpu-bundle`, `dest_kind`, and the take-over with a kept copy under `/bin/sh` and
  `/bin/dash`: an update, the copy kept as the base's, the base's files back, a file the owner changed left alone, and no take-over
  when no copy can be made.
- `tooling/metal/check-bundle-paths.py --self-test`.
