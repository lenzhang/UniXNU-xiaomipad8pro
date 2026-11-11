# The k27 boot arguments and their readers

The arguments every native image carries (`K27_ARGS` in `tooling/bootimg/build-ram-images.py`; the ESP's set in
`tooling/persist/build-esp.py`), checked 2026-11-11. They came with the macOS 27.0.1 install the tablet runs, which was first
brought up under a hypervisor; only the ones something native reads were kept.

## Legacy names

Four arguments keep the `droidvm-` prefix of that earlier VM-based port. The names are what the k27 kernel and the OS image read;
renaming one means changing the kernel (`patches/k27` or the baseline series) and the OS image together.

| Argument | Read by | Why natively |
|---|---|---|
| `droidvm-utc`, `droidvm-tz` | the OS image's boot job (`org.droidvm.run.0`, which runs `settime.sh`) | the clock and time zone when the PMIC RTC gives no date: with k27 0060 and `piano-rtc=1` the kernel sets the calendar first and `settime.sh` leaves it (it only acts near 1970), so `droidvm-utc` (the image's build time) stays as the fallback ([drivers/rtc.md](drivers/rtc.md)) |
| `droidvm-timebase-hz=24000000` | the k27 kernel (pexpert, baseline patch 0042) | userland expects a 24 MHz timebase over SM8750's 19.2 MHz counter |
| `droidvm-disable-jobs` | nothing natively ([boot-time.md](boot-time.md) §4) | its reader was another kernel line's launch policy; the k27 kernel and the KC have none, and the jobs it lists run. Kept until the boot-time test; `PIANO_FAST_BOOT` drops it. Natively a daemon is kept from running by `k27-deny-exec=` (baseline 0035) or the seed's `disabled.plist` |

## The k27 kernel's arguments

`offline-apfs-load`, `owner-staged-apps`, `union-hide-lower`, `malloc-ranges-off`, `mac-amfi-launch-permissive`, `rootauth=0`,
`exec-env`, `kt_fixed_zones`, `tlto_us`, `-no_slto_panic`, `vm_compression_limit`, `arm64_maxoffset`: read by the k27 kernel
(the baseline series). They make the macOS 27.0.1 userland run on a machine that is not a Mac. `-apfs_shared_datavolume` is
Apple's APFS's (not verified), kept until shown unused. The piano arguments (`native-ufs-*`, `data-persist`, `pstore-console*`,
`piano-*`) are this repository's: [bringup-debugging.md](bringup-debugging.md), [data-store.md](data-store.md) and the driver pages.

## Removed

Nothing in the native collection reads them: `offline-hid-input`, `offline-hid-home`, `offline-key-events`, `sheng-aks`,
`sheng-aks-sls` (only a key-store kext the native set does not link), `ml-timeout-lock-scale`, `ml-timeout-lock-panic-scale`,
`ca-triple-buffer`, `ca-free-wakeup` (the VM-era Metal plugin's). The VM's device arguments were never in a native image.
