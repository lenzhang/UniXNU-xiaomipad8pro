# The Data store: format, the store guard, flatten, plugin staging

Status 2026-11-11: **built and host-tested offline; nothing of it has run on the tablet.** The installed ESP (`esp-daily-macos`,
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
written. Pages written now go into RAM: a directory of one pointer per 2 MiB of disk (48 KiB), plus 4 KiB leaves and pages
allocated on first write. They are gone at the next boot. A long volatile session costs RAM in proportion to what it writes.
The read-only probe (`data-persist=1`) of a large disk also used to dereference the missing sparse page array once the store was
valid. It now reports the store and applies nothing (state 6).

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

## 5. Plugin staging (part b): evaluated, not done; the plan

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
| `tooling/persist/build-esp.py` | the macos entries keep `data-persist=2`. `--replace-data [1\|HEX8]` adds `data-store-replace` for an intended one-time switch; `--flatten-data` adds `data-store-flatten=1` (a no-op once the store is self-contained). A seed that is not the installed one gets a loud note (with the guard: kept, Data volatile, unless the store is self-contained). With a collection without the guard it is refused unless `--replace-data`. The installed trust cache's CDHashes are checked against the new one (`--installed-trust`). `--arg data-*` is refused. The record has `data_store` |
| `tooling/persist/store-test-images.py` | the RAM images of section 7: `out/ram-store-probe` (`data-persist=1`, reset 90 s), `out/ram-store-guard` (`=2`, another seed, 240 s), `--flatten`: `out/ram-store-flatten` (the installed seed, trust cache and ADT from `--installed-esp`, `data-store-flatten=1`, 180 s) |
| `tooling/persist/maint-session.py` | step `map` reads 128 bytes of each store header (flags, self-contained); `--store auto` moves a self-contained store whatever the ESP's seed; the RAM tests accept state 7 |

Host tests (offline, about 10 s in all):

```sh
package/NativeRamBlock/Test/run-tests.sh [SEED.img]   # sparse, persist, direct, seeddisk, guard, volatile (+ the seed's identity)
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
cheap insurance: `maint-session.py` has no standalone step for it yet.

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
`PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`, then `build-esp.py --profile daily --default macos --seed
out/installed/scratch-data.img --trust out/installed/kernel.macho.trustcache`, then `maint-session.py … esp-final …`
(docs/persistent-boot.md). Once the store is self-contained, the ESP's seed may change; the trust cache rule of section 4 still
holds.

## 8. Risks and open items

- Nothing here has run on the tablet. The volatile large disk is new code on the boot path of every boot without the store
  (test images, `data-persist=0`). The host test covers it against an oracle; a long volatile session can exhaust RAM.
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
