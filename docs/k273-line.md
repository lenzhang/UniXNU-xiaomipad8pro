# The 27.3 line: macOS 27.3 (26C5035r) as Mac17,11 on piano

Owner decision 2026-11-12 22:40: move off the VM identity. Today's system is macOS 27.0.1 (26A434) from Apple's VM build: its
kernel collection takes Apple's kexts from the vma2 kernelcache and the ADT says VirtualMac2,1 / VMA2MACOS (configs/k27-adt.toml).
The 27.3 line runs macOS 27.3 (26C5035r) from the UniversalMac restore IPSW of the UniXNU `macos-27.3-26C5035r` profile, with the
profile's identity Mac17,11 / K114c / K114cAP, as a parallel line: the 27.0.1 line stays the owner's daily system (installed on
linux_rootfs) until this line reaches a stable desktop in RAM boots. Root on linux_data (owner decision 2026-11-12: Linux is not
needed any more, all of linux_data is the 27.3 line's).

Approach (owner, 2026-11-12): migrate everything at once (every native kext of configs/k27-native-kexts.txt, the k27 kernel with
0053-0065, the ADT and board pieces, the image tooling, the Turnip plugin), build the first RAM image fast, then fix by booting.

## 1. What differs from the 27.0.1 line, and what does not

| Piece | 27.0.1 line | 27.3 line | Where |
|---|---|---|---|
| Kernel | k27 tree: xnu-12377 + 0001-0052 + patches/k27 0053-0065, `K27_VARIANT=native` | the same source + `patches/k273` (0001: UniXNU's Darwin 26 system calls), relinked at the 27.3 collection's segment addresses | `tooling/k273/prepare-k27-tree.sh`, `patches/k273/`, `patches/k273-kc/` |
| Apple kexts | vma2 kernelcache 26A434 (`$PIANO_K27_TREE/out/kc/kc27.bin`) | `kernelcache.release.mac17j` 26C5035r (sha256 d1003b51..., the profile's pin) | `tooling/k273/decompress-kernelcache.py` |
| Kept kexts | `configs/kc-keep.txt` + the natives' libraries: 48 kexts | the same 48 (all present in mac17j; 0 kernel symbols they reference are missing from our kernel) | `tooling/k27/check-native-kexts.py` unchanged |
| Native kexts | `configs/k27-native-kexts.txt` | the same list and objects; NativeGopDisplay with `NATIVE_GOP_IOMFB_ABI=2` (below) | `tooling/k27/build-native-gop-display.sh` |
| ADT identity | `configs/k27-adt.toml` (VirtualMac2,1, chip 0x8011) | `configs/k273-adt.toml` (Mac17,11, K114c, K114cAP, chip 0x6050, board 4; product name "Xiaomi Pad 8 Pro"; vmm-present 0) | `make-piano-adt.py --identity` |
| Root | linux_rootfs: SHNGROOT + OS (27.0.1) + SystemOS | linux_data: SHNGROOT + OS (27.3 System volume, unchanged IPSW image) + SystemOS (made like 27.0.1's from the 27.3 cryptexes) | `tooling/k273/make-systemos.sh`, `make-root-image.sh`; NativeUfs `native-ufs-root-part=linux_data` |
| Trust | 27.0.1 bundle | the same own module + 27.3's static, System and App trust caches | `tooling/k273/make-trust.py` |
| Data seed | out/inputs/scratch-data.img | first images: the same seed (the daily Data as before an update), volatile (`data-persist=0`) | `make-root-image.sh` (`K273_SEED` for another) |
| GPU userland | Turnip on ABI v2 + NativeAdrenoMetal.bundle | the same bundle, staged into the 27.3 seed copy | `tooling/metal/build-turnip-a830.sh` |

### Kernel collection findings (2026-11-12)

- mac17j is an SPTM collection: top-level `__DATA_SPTM` and `__TEXT_BOOT_EXEC` hold only Apple's kernel and dropped kexts; the
  KC builder leaves them out after checking that nothing kept or linked lies in them (`patches/k273-kc/0001`). Its header takes
  two pages (0x8000); our collection's one-page header leaves the second to the kernel's `__TEXT` at 0x4000, as with kc27.
- The relinked kernel fits: `__TEXT` 0x4000, `__DATA_CONST` 0xdc0000, `__TEXT_EXEC` 0x49d0000 (ends before Apple's `__KLD`
  0x549c000), `__KLDDATA`/`__DATA`/`__BOOTDATA` 0x58a8000-0x5b04000, `__LINKINFO` 0x5d3c000 (`k27/kc27-layout.py`).
- AMFI's PAGE_SHIFT_CONST slot (the k27 rebind to `_k27_amfi_page_shift`) is 0xfffffe00081d4630 in mac17j, now found by its
  target instead of a fixed address.
- Family ABIs our kexts derive from (`tooling/k273/check-family-abi.py` 27.0.1 vma2 vs 27.3 mac17j): IOHIDDevice,
  IOHIDEventService, IOEthernetController, IONetworkController, IOBlockStorageDevice, IOAccelerator, IOSurface,
  IOMobileFramebufferUserClient, IOAVElement, IOMobileFramebuffer: same vtables and instance sizes. **IOMobileFramebufferLegacy
  9120 bytes (7640) and IOMFBSwapIORequest 4056 (3400)**, same vtables: Apple builds IOMobileGraphicsFamily per platform (both
  are version 343.0.0). NativeGopDisplay's offsets for mac17j come from the family's own code, instruction for instruction over
  the 1105 functions of the same length (`tooling/k273/diff-field-offsets.py`): Legacy fields +1472 (6720 -> 8192 ...), the swap
  request's +160 (980 -> 1140 ...), its surfaces 2944 -> 3336, its queue links 3384 -> 4040; rectangles, dirty region, notify
  request and IOAVElement unchanged. 27.0.1's `kSwapInfoHeld` (+7634) is AppleParavirtDisplay's own field in the family's tail
  padding (no family code touches it): not read for 27.3. mac17j has no IOMobileFramebufferLegacy subclass to name the pure
  virtual slots, so the declarations come from the vma2 collection (same vtables) with mac17j's instance size.

## 2. Plan and effort (2026-11-12)

| # | Step | State | Effort left |
|---|---|---|---|
| 1 | Inputs: the K114c members of the pinned IPSW (13.3 GB of 27.2 GB, resumable ranges), decrypted, pins checked | done | - |
| 2 | Kernel + KC: relink, 27.3 KC with all natives, the IOMFB ABI | done (first image) | by boot logs |
| 3 | ADT identity, root image (OS + SystemOS), trust bundle, seed | done (seed: 27.0.1's) | fresh 27.3 seed: 0.5 day |
| 4 | RAM image + checklist (below) | built | - |
| 5 | Boot-driven fixes: what 27.3's userland asks of a 27.0.1-era kernel (syscalls, KPIs: UniXNU's `darwin26-*` / `macos-*` patches are the reference), Metal selectors new in 27.3, Mac17,11-specific daemons | after the first boot | 1-3 days |
| 6 | A fresh 27.3 Data seed (UniXNU's `make-macos-data-seed.py` or the k27 recipe with the owner's account record) | open | 0.5 day |
| 7 | The 27.3 line's installed profile (ESP with the 27.3 KC/ADT/seed, store at the end of linux_data) | after a stable desktop | 0.5-1 day |

## 3. The root image and its write (owner's step)

`tooling/k273/make-root-image.sh` -> `out/inputs/k273/root/root.img` and `root.json` (volumes, windows, sha256s, LBAs).
2026-11-12: 20936916992 bytes, sha256 `2f8ef3550f5c94bb831676e1e8a3894416563c08297c0192f8e29971fd5db1b8`; manifest `089b937d...`;
OS window 0x100000 (14493417472 B, sha256 `40d8282e...` = the profile's `os_image_sha256`, volume 84581E99-85BE-4957-AC53-2947E887B3E4);
SystemOS window 0x35ff00000 (6442450944 B, sha256 `a5346509...`, volume 07AD53B3-7565-4004-B574-F45FAC3753FB).

Write: LUN 0 `linux_data` (#36), 4096-byte LBAs 71789568-76901119 (5111552 blocks, the image exactly) from the partition's first
block; the rest of linux_data (to LBA 124552186) is untouched; no GPT change. From the maintenance RAM image:
`dd if=root.img of=/dev/sda36 bs=4M conv=fsync`, then read back 20936916992 bytes and compare the sha256. The old ext4 of
linux_data is overwritten (owner decision).

## 4. RAM images

`tooling/k273/build-k273-image.sh` (`K273_ROOT=k273`, the default: root on linux_data; `K273_ROOT=k27`: the 27.3 collection and
identity on the installed 27.0.1 root, no write needed). Data volatile, `native-ufs-root-part=linux_data` for the 27.3 root.
The owner's test image is the desk-dev profile on top of it:

```sh
PIANO_GPU=2 tooling/k273/build-k273-image.sh            # KC, ADT, Turnip stage (27.3 trust) in out/
DESK_EXTRA=native-ufs-root-part=linux_data DESK_RESET_AFTER=1800 PIANO_DATA_PERSIST=0 PIANO_K27_TREE=$PWD/out/k273/k27-tree \
  tooling/k27/build-desktop-images.sh desk-dev            # out/ram-desk-dev, out/ram-desk-dump
```

First image (2026-11-13 00:10): `ram-k273-desk-dev` sha256 `5d998d3d5a70e8f0ba6f0524b6e82150a97a7c63bafda91d65b999a53d693d67`
(KC `7bf32dc8...`: 48 Apple kexts of mac17j, 25 native kexts, every vtable check 0 problems; ADT `5bc3409f...`; trust `5e4d958e...`;
seed: the 27.0.1 seed + Turnip + the desk-dev stage, identity `a53a3c05...`, data-persist=0), returns to fastboot after 1800 s
(`piano-reset-after`, `piano-reset-to=bootloader`) or on `reset fastboot`. The Metal plugin is byte for byte the 27.0.1 line's.
Second image (00:20): `ram-k273-desk-dev-s1` sha256 `344297d9a22986279f29ed0ff92f5624dc5f2bced8cc511078c786990eb338c9`, the same
with `patches/k273/0001` (UniXNU's darwin26-syscalls: the Darwin 26 system calls k27 leaves as nosys) in the kernel, KC `725856ed...`;
the added calls are inert unless called, so it is the one to boot first, the first image its control.

**Size limit (2026-11-13).** `ram-k273-desk-dev-s1` (294952960 bytes) never left ABL after `fastboot boot`'s OKAY (owner's loop
log 20261113-004027): the UEFI map's kernel region is 256 MiB, and the 27.0.1 line's 247767040-byte images boot. What had grown was
the kernel collection (92.8 -> 135.9 MB: mac17j keeps the symbol tables of all its 370 kexts). Now:
`tooling/k273/compact-kc-linkedit.py` (`KC_COMPACT_LINKEDIT=1`, the 27.3 line's default) keeps only the kept entries' symbols
(114.2 MB), and `tooling/k27/compact-seed.py` (`DESK_COMPACT_SEED=1`) zeroes the seed's free blocks, which held the Metal plugin the
Turnip stage replaced (124.7 -> 101.1 MB). `build-ram-images.py` refuses an image above `PIANO_IMAGE_MAX_BYTES` (250000000) and
`verify-ram-images.py` fails one. `ram-k273-desk-dev-s1b`: 245669888 bytes, sha256
`96e5ef285001ea36d92605f965ed54592d96fe5428d9ab2e6ff97ada7891cace` (KC `20b08b86...`, seed `57a354da...`); build it with
`DESK_COMPACT_SEED=1` added to the desk-dev command above.

**First boot (2026-11-13 01:02, `ram-k273-desk-dev-s1b`, owner's run).** Confirmed on the tablet: NativeUfs takes the root from
linux_data (`root-part=linux_data`, 2 windows published), the root waits on 84581E99 and mounts, every firmlink 0, Data volatile;
over the root shell `sw_vers` 27.3 26C5035r and `hw.model` Mac17,11; WindowServer, loginwindow, SecurityAgent and MenuBarAgent are
GPU clients. The panel stayed black but for the cursor: 27.3's WindowServer draws its framebuffers as '&BGA' (Apple's
lossless-compressed BGRA spelling, one plane), which NativeGopDisplay refused (`a frame surface 3200x2136 format 0x26424741 row 3200
... is not read`). Swaps did complete (the family's last-completed went 1 -> 814; the `swap_wait` lines are WindowServer's
non-blocking polls, as on 27.0.1), so the ABI=2 offsets held. Fix: '&BGA' read as BGRA from its plane 0, which the Metal plugin
renders linearly (`ram-k273-desk-dev-s1c`, sha256 `43a25f0c56d40178b6a9a9b96d904765904418f32692b2a564273701b83aa61b`).

The large inputs live on the external SSD (`/Volumes/ExtSSD/k273/`: the IPSW members, `root/root.img`, `root/systemos.img`), with
links at their `out/inputs/k273/` paths.

Checklist of the first 27.3 boot (each with the console line that answers it):

1. The collection links and starts: `kc-build` log `wrote ...`; on the console the native kexts' start lines (`NativeUfs: start ...
   root-part=linux_data`, `PianoDwc3`, `NativeGopDisplay`).
2. The root image is found: `NativeUfs` manifest status 0, windows OS and SystemOS published; `Waiting on <dict> IOMedia ... UUID
   84581E99...` then the root mounted (`k27: firmlink ...` lines, `owner cache` stage 6).
3. launchd starts (`launchd` exec log), boot tasks run, no panic in the first 120 s (`piano-reset-after` returns to fastboot).
4. The USB console and root shell (desk-dev profile): `pianousb-shell` reachable; `sw_vers` says 27.3 26C5035r; `sysctl hw.model`
   Mac17,11; `ioreg -l | grep product-name` Xiaomi Pad 8 Pro.
5. WindowServer and the login window on the A830 (`piano-gpu=2`): the panel shows the login window (webcam); NativeGopDisplay's
   swap lines carry IDs and dirty regions (the mac17j offsets work: a wrong offset shows as swaps never completing or garbage IDs).
6. Input: keyboard cover keys reach the login field; touch taps (`piano-touch=2`).
7. Login with the debug account; Dock, Finder; Metal clients (`log show` of WindowServer for Metal plugin errors: new MTLDevice
   selectors in 27.3 abort here).

## 5. Open questions and blockers

- Trust/signing: the System volume of the IPSW is unsealed ("Sealed: No") and the k27 dev build publishes the windows unverified
  (`NATIVE_UFS_UNVERIFIED_ROOT`), as for 27.0.1; the profile's owner-signed root policies are not used by the k27 chain.
- 27.3's `/usr/share/firmlinks` adds `/pkg`, which k27's fixed list of ten firmlinks lacks (`/pkg` is then the read-only system's
  empty directory); a one-line kernel change if something writes there.
- The UniXNU profile's root list keeps more kexts (IOGPUFamily, the USB device/host stacks, IOBluetoothFamily, IO80211Family,
  EndpointSecurity, AppleHIDKeyboard ...); this line keeps the 27.0.1 line's set, whose drivers stand in for the hardware ones.
- The 27.0.1 seed with the 27.3 system skips the Data migration of an update; a fresh 27.3 seed is step 6.

## 6. The dual-boot entry on the installed ESP (2026-11-13)

`tooling/k273/build-dual-esp.py` makes the installed ESP (its build record, default `out/persistent/installed.json`'s image) with
one entry more, `macos-27.3`, and nothing else changed: every file and every entry of the base (the default `macos`, its timeout,
`macos-diag`, `fastboot`, the actions) byte for byte, checked against the base's record. The new entry boots
`Payloads/k273/{kernel.macho,.adt,.trustcache}` with `Payloads/k273/scratch-data.img`, and its arguments are the base's `macos` line
plus `native-ufs-root-part=linux_data data-store-lba=N` (the daily profile's drivers and levels: charging 2 with
`piano-power-delay=0`, USB net and shell, keyboard, touch, backlight, Wi-Fi 6, boot guard 180; 989 of DarwinBoot's 1000 bytes).

Its Data store is its own, and the two can never resume each other:

| | 27.0.1 (`macos`) | 27.3 (`macos-27.3`) |
|---|---|---|
| root partition | linux_rootfs | linux_data (`native-ufs-root-part`) |
| store window | end of linux_rootfs, LBA 68119455 | end of linux_data below the 1 GiB reserve: LBA 117998394-124290042 (the root image ends at 76901119) |
| seed | 8f333110 (12 GiB), unbound | 89860516 (24 GiB: `compact-seed.py --grow-gib 24` of the desk-dev seed) |
| binding | none (its collection has no `data-store-lba`) | `data-store-lba=117998394`: NativeUfs refuses any other window place; NativeRamBlock keeps any valid store not bound to that LBA (`PersistOverlay.h` `kBound`, the LBA in the header's extension) and binds its new store to it |

A kernel that never names linux_data (the installed 27.0.1 collection) cannot open the 27.3 window; the 27.3 kernel with its
arguments cannot open any window but its own, and keeps an unbound store (the 27.0.1 one) even of its own seed (host tests:
`package/NativeRamBlock/Test/guard.cpp`, the binding section). The seed identities differ as well, so the seed guard separates them
too. Tools: `common.check_store_size` takes a linux_data store of any size only with its `data-store-lba`; `maint-session.py
k273-store` reads both store headers (the 27.3 one blank or bound to its LBA with its seed; the installed one unbound).

The 27.3 store's seed is its base from now on: every later ESP's `macos-27.3` must take the same seed file
(`out/inputs/k273/store-seed/scratch-data.img`, identity 89860516...), as the 27.0.1 line keeps its base seed (docs/data-store.md §9).
Its RAM test: `build-ram-images.py` with `PIANO_EXACT_ARGS` = the entry's line (`out/ram-k273-entry`): the same payload and the same
arguments, `data-persist=2` on the new store (its first boot creates the store; the installed entry then resumes it).
