# ShengUfs

SM8550 UFS transport and sheng persistent storage.

The historical native UFS root qualification applies to the iOS 17 path.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-ufs). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

UniXNU's `scripts/build-sheng-source-drivers.py --with-storage-family`
compiles this driver and public IOStorageFamily 337.100.1 against XNU 12377
for arm64e iOS 26. Verified-root read policy remains enabled. These archives
still require family/APFS linkage and device qualification; they do not establish
an iPadOS 26 root boot. See the [migration procedure](https://unixnu.com/porting/migration#source-drivers).

The arm64e `build-arm-driver.py ShengUfs` entry now selects
`build-sheng-source-storage.py`. It builds a deferred public-source
IOBlockStorageDevice adapter and the board's SourceStorage runtime. The runtime
loads the original IOStorageFamily and APFS, checks the original base class
size, authenticates the 23F84 policies, registers the source classes, and starts
NativeUfs on the platform root. Its runtime metadata has a separate name and
inherits the original storage metaclass; its C++ class name is preserved for
compiler-generated arm64e pointer authentication.

`native-source-storage=1 native-ufs-root=1` selects this path. The root policy
tables travel in `kernel.macho.vroot`; only the owner public anchor is linked
into the kernel. On sheng it serves the iPadOS 27.0.1 (24A446) root and Data.

## Command path

Up to 32 commands run at once, one per command slot. The request list sits on
a write-combined page of its own, so slot s uses list entry s; without that page
16 slots use the even entries of the DMA region, so no two entries share a cache
line on this non-coherent stream. Region A holds slots 0 to 15 (slot 0 keeps the
original layout), region B slots 16 to 31. The last slot serves the panic path;
a full pool hands a freed slot to its oldest waiter.

Completion uses the controller interrupt (GIC SPI 265) with UTRCE and the UTRD
interrupt bit, as Linux ufshcd. The handler wakes the waiters of the commands
whose doorbell bits cleared. A wait polls for its first 200 microseconds, then
sleeps. Bring-up and the panic path poll.

Page-aligned buffers are read and written by DMA straight into their pages
through a PRDT of up to 64 entries and 1 MiB per command; other buffers use the
slot's 128 KiB data area. A command answered with BUSY or TASK SET FULL is sent
again after a pause. Root window reads each take their own bounce buffer.

`native_ufs_dme_get` reads UniPro attributes. The sheng link runs HS-G5 Rate A
on two lanes in FAST mode, as the boot firmware leaves it.

## Tests

`tests/RootCommands.cpp` runs the production command code against a host model:
per-slot entries, descriptors, tags and data, the pool, busy retries, the panic
slot and the one-slot fallback. `tests/check_arm64_root_commands.py` runs it on
QEMU's UFS controller at EL1 with real cache maintenance, including eight
outstanding commands and a 512 KiB zero-copy read.

## Wi-Fi DMA window

The optional WCN7850 window maps 128 MiB at IOVA 0x10000000 using 64 two-MiB
blocks in its own SMMU context bank. The Wi-Fi allocator reports current use,
peak use and failures. sheng device checks reach interface readiness and
network traffic with zero DMA allocation failures.

The consuming kernel build compiles the driver. The board's
`scripts/check-wifi-dma.py` checks window capacity and allocation boundaries.

## UniXNU-piano copy

ShengUfs b11de87 (`UPSTREAM`) with the piano/SM8750 deltas, built as a k27 native kext by `tooling/k27/build-native-ufs.sh`:
- SM8750 register facts from the SoC layer: `GccUfs.h` and `SmmuTakeover.h` include `UniXNU-sm8750-common/kits/Sm8750Platform`
  (`NATIVE_UFS_SM8750=1`), the 1 MiB apps-SMMU window and the TLB sync after the stream bypass (as the SM8750 UniXNU ports).
- Root partition `UFS_ROOT_PARTITION` = `linux_rootfs` (SHNGROOT manifest, `tooling/native/make-root-manifest.py`).
- Development build: no `NATIVE_VERIFIED_BLOCK` (windows published read-only without owner-signed block policies).
- `NATIVE_UFS_POLL_ONLY=1`: k27's ArmCpu has no `armcpu_spi_attach`; completion by polling.
- `gScratch` allocated at start (the KC has no gap for a 128 KiB `__bss`).
- Power (`RootPower.h`, opt-in): `native-ufs-suspend=1` puts the link into Hibern8 for a system sleep and undoes everything on any
  failure; `native-ufs-ahit=MS` the controller's auto-hibernate at run time. [docs/drivers/ufs.md](../../docs/drivers/ufs.md) §12.
- Read path speed (opt-in, all off by default): `native-ufs-perf=1` (per-second timing with `native-ufs-iolog`),
  `native-ufs-async=N` (read workers), `native-ufs-bounce=KiB` (up to 1 MiB per command), `native-ufs-maxio=KiB`,
  `native-ufs-nap=US`, `native-ufs-direct=1` (DMA into the client's pages), `native-ufs-fast=1`.
  [docs/drivers/ufs.md](../../docs/drivers/ufs.md) §13.
- Host tests: `tests/run-tests.sh` (the command, queue, link and power code against `tests/UfsModel.h`; the window reader's bounce
  sizes, `tests/RootWindowRead.cpp`; the direct reads' PRDT and command, `tests/RootDirect.cpp`).
