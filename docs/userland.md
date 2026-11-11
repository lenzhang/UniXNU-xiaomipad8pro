# Userland: what this repository installs on the tablet's macOS, and how

Status 2026-11-11: **built and host-tested offline** (`tooling/userland/test-stage.py`: 125 checks, among them a 64 MiB APFS seed
made with hdiutil, the real units' builds, the installer under `/bin/sh` and `/bin/dash`); staged once into a copy of the Turnip
stage's seed. **Nothing has run on the tablet**, and the installed system has none of it. Section 9 is the batched tablet list.

Facts are marked as in `power.md`: **verified** (read from source, a binary, a log, or run here), **inferred**, **needs-boot**.

Before this, several userland pieces existed with no way onto the tablet but hand-written `stage-soft-metal.py --file/--boot-run`
lines: PianoPolicy's agent, PianoAuto's rotation agent, the USB root shell, the torch tool, and the pmset policy of `policy.md` §7.
Now there is one manifest, one staging tool, and one installer on the tablet.

## 1. The pieces (`configs/userland.toml`)

| Unit | Installed files (all root:wheel) | Installed profile | Inert unless | Trust cache |
|---|---|---|---|---|
| `pianousb-shell` | `/usr/local/libexec/pianousb-shell` 0755, `/Library/LaunchDaemons/com.unixnu.pianousb-shell.plist` 0644 | on | `piano-usb-shell=1` (without it, it exits at once with status 0 and launchd leaves it stopped) | its CDHash |
| `piano-policy-agent` | `/usr/local/libexec/piano-policy-agent` 0755 (the shell script), `/Library/LaunchDaemons/com.unixnu.piano-policy-agent.plist` 0644 | on | PianoPolicy (`piano-policy=N`) for the sysctl; the clamshell state for display sleep. One idle `notifyutil -w`, no polling | none (a script) |
| `piano-rotated` | `/usr/local/libexec/piano-rotated` 0755, `/Library/LaunchAgents/com.unixnu.piano-rotated.plist` 0644 | on | `piano-rotate=N` (exits 0 without it; `KeepAlive SuccessfulExit false`) | its CDHash |
| `piano-torch` | `/usr/local/bin/piano-torch` 0755 | installed (a command) | PianoTorch at `piano-torch=2` | its CDHash |
| `piano-pmset-defaults` | `/usr/local/libexec/piano-pmset-defaults` 0755, `/Library/LaunchDaemons/com.unixnu.piano-pmset-defaults.plist` 0644 (one-shot) | on | sets each power source's timers once, unless the owner set them first (section 6) | none |
| `piano-wifi` | `/usr/local/bin/piano-wifi` | planned | the CLI of `drivers/wifi.md` §13.1 (not written: `status = "planned"`, listed and skipped) | — |

Two changes to existing pieces came with this. `pianousb-shell` now exits at once (status 0) on `/dev/pianousb` when the kernel
argument `piano-usb-shell` is missing, and its job is `KeepAlive SuccessfulExit false`. Before, it retried the open every 2 s
forever. `piano-policy-agent` got a LaunchDaemon plist instead of the `launchctl submit` line.

## 2. The manifest

`configs/userland.toml` (format 1; its header documents every key):

- `staging = "/private/var/piano/userland"`: where the files are carried on Data.
- `[[unit]]` has `name`, `about`, `default_on`, and optionally `status = "planned"`. A unit has at most one launchd job. A unit
  without a job is a command-line tool and is always installed.
- `[[unit.file]]` has `dest`, `owner` (default `root:wheel`), `mode`, and `build`:
  - `copy`
  - `cc`: `xcrun clang -arch arm64|arm64e -mmacosx-version-min=14.0 -O2 -Wall -Wextra -Werror`, with `cflags` and `frameworks`. The
    output is named as `dest`, because the linker's ad hoc signature names the file and so enters the CDHash.
  - `script`: a command and its `output`, with the `inputs` its stamp covers. `piano-rotated` builds through
    `tooling/k27/build-piano-rotated.sh`, which runs PianoAuto's host tests first.
  - `trust` (default true) puts a Mach-O's CDHash into the trust cache.

`stage.py` refuses each of these, and every refusal is host-tested:

- a destination outside `/Library/LaunchDaemons`, `/Library/LaunchAgents`, `/usr/local/bin`, `/usr/local/libexec`;
- a mode writable by group or others;
- a job file that is not `<Label>.plist` root:wheel 0644 (launchd refuses any other);
- two jobs in one unit, or `default_on = false` without a job;
- a duplicate destination, an unknown key, a missing source;
- a job whose program the manifest does not install, or that names a `/usr/local` path the manifest does not install;
- `Disabled` written by hand in a job file;
- a fat Mach-O, or a build output that is not a Mach-O.

## 3. Staging into the seed: `tooling/userland/stage.py`

```sh
python3 tooling/userland/stage.py --plan            # the manifest, checked, as a table
python3 tooling/userland/stage.py                   # out/turnip-a830/stage/{scratch-data.img,kernel.macho.trustcache} -> out/userland/stage
python3 tooling/userland/stage.py --off piano-pmset-defaults   # one build with a unit's job off (or --on); the record says so
```

What a run does:

1. **Builds** each file. A build is skipped when its stamp is unchanged: its sources' bytes, its command, and the toolchain
   (`clang --version`, the SDK path and version, `ld -v`). The builds are reproducible anyway: verified, with `pianousb-shell`,
   `piano-torch` and `piano-rotated` built 2-4 times to byte-identical files. ld's `__objc_stubs` ordering, which
   `build-turnip-a830.sh` works around, did not show in `piano-rotated`'s four links.
2. **Checks** each launchd job against the manifest (section 2). For a unit that is off in this build, it writes its plist with
   `Disabled = true`.
3. **Renders** the staging directory: `install.sh`, `manifest.tsv` (one line per file: unit, kind, on, mode, owner, group,
   sha256, staged path, destination, label), `SHA256SUMS`, and `files/<unit>/<name>`.
4. **Trust cache**: the input trust cache with one more module first, holding the CDHashes it lacks. This is
   `stage-soft-metal.py`'s `trust_bundle`. CDHashes already present are not added again.
5. **Seed**: `stage-soft-metal.py`'s mechanics, imported and not changed. `build-native-image.sh`'s stamp hashes that script, so
   editing it would restage the Turnip seed, which means a new identity. The steps:
   - unpack the DBRDISK1 seed to a sparse raw file in the output directory;
   - `hdiutil attach -nomount`, then `diskutil mount` the volume named Data (no sudo: ownership is ignored, so files are stored
     as uid/gid 99 with their modes);
   - make the staging directory exactly the rendered tree (stale files are removed);
   - add the boot command;
   - unmount, detach, and pack again.
6. **The boot job**: the seed's one boot job (`stage-soft-metal.py` `BOOT_JOB`, which runs `settime.sh`) is rewritten in place,
   so it keeps its owner. `sh /private/var/piano/userland/install.sh --boot` goes right after `settime.sh`, once. Commands
   already there stay after it: a `--log-errors` loop never ends, so a command appended at the end would never run.
7. **Writes** `userland.json` (inputs, every file with its sha256 and CDHash, outputs, identities) and `stamp`.

Idempotent:

- The staging stamp covers the input seed's and trust cache's sha256, every staged file's sha256 and mode, the boot command, and
  `FORMAT`. When it is unchanged, the outputs are left byte for byte as they were, and the seed keeps its identity
  (host-tested).
- A restage always starts from the input seed. Its result has the same files, the same trust cache and the command once.
- The input seed and trust cache are only read, and `--out` over them is refused.

Measured on a copy of `out/turnip-a830/stage` (2026-11-11 23:29, 12 s):

- input identity `8f3331104e03c970`, output `2b6eb91fab2d8a9f`;
- 9 files;
- 3 CDHashes added: pianousb-shell `83876bc9…`, piano-rotated `c401d51b…`, piano-torch `f386f217…`;
- trust cache 5 → 6 modules;
- the boot job: `sleep 0; sh /private/var/settime.sh; sh /private/var/piano/userland/install.sh --boot`.

The second run printed `up to date ... seed identity 2b6eb91fab2d8a9f unchanged`.

The trust cache modules:

| Fact | State | Source |
|---|---|---|
| The boot loads the payload's trust cache modules from the device tree's `chosen/memory-map` `TrustCache` range. Module 0 is static (a failure panics), module 1 the second static, the others engineering (only when the runtime allows them) | verified (source) | k27 `bsd/kern/kern_trustcache.c` `load_static_trust_cache` |
| On piano every module loads: `attempting to load 5 external trust cache modules`, `loaded external trust cache module: 0` … `4`, and never `skipping engineering trust cache module`, in 37 native boots | verified (logs) | `logs/native-runs/*/xnu-console.txt` |
| A sixth module (ours first, the Turnip module second, the OS's after) is therefore loaded the same way | inferred; needs-boot (U1) | — |

## 4. The seed identity and the Data store (`data-store.md`)

Every restage makes a new seed identity, because APFS writes times and transaction ids. `stage.py` prints what that means:

| The tablet's store | An ESP or RAM image with `out/userland/stage`'s seed |
|---|---|
| the installed seed's, not flattened (today: `aced5bf9…`) | **kept untouched, Data volatile**. The installer installs everything at every boot, and it is all gone at the next. `build-esp.py --replace-data <8 hex>` starts the store over from this seed and **drops the owner's Data changes** |
| flattened (self-contained) | resumed. The seed is **not read**: none of these files reach Data. Use the live update (section 7) |
| blank, `data-store-replace`, `sheng-data-reset` | a new store from this seed: everything installed at its first boot, then kept |
| RAM test images (`build-ram-images.py`: another seed → `data-persist=0`) | volatile Data with the files, the store untouched: the tablet tests of section 9 |

So **the seed path serves test images and fresh stores**. The owner's existing Data gets the userland through the live update
(section 7). That update keeps the installed seed, or comes after a flatten. `build-esp.py` and `build-ram-images.py` still
refuse `data-persist=2` for a seed of another size (`data-store.md` §8). The 64 MiB test seed prints that note.

## 5. The installer on the tablet: `install.sh`

The boot job runs `sh /private/var/piano/userland/install.sh --boot` as root, at every boot from such a seed. A live update runs
the same script with `--live`, and `--check` changes nothing.

- **Files**: a staged file whose sha256 is not the manifest's (a torn copy) is not installed, and the run fails. A file is
  replaced only when its bytes differ, by `install -S`: a new file renamed over the old one, so a running program keeps its
  pages. Owner and mode are set as the manifest says, also on an unchanged file. A missing `/usr/local/bin` or
  `/usr/local/libexec` is made root:wheel 0755.
- **Jobs**:
  - A job that is on is loaded (`launchctl bootstrap system`; agents into the console user's `gui/<uid>`) when it is not loaded.
  - A loaded job whose files changed is restarted: `kickstart -k`, or bootout and bootstrap when its plist changed. A job that
    has just become off is booted out.
  - A job that is off has `Disabled` in its plist and is never loaded by the installer.
  - With nobody logged in, an agent waits for the next login window or login. Installed before loginwindow starts, the
    login-window session takes it.
- **The owner decides**:
  - `launchctl disable system/<label>` (kept in launchd's override database on Data) makes launchd refuse the installer's
    bootstrap. The installer leaves the job off and says so.
  - `launchctl enable system/<label>` (an agent: `gui/<uid>/<label>`) turns an off job on for good. The override wins over the
    plist's `Disabled`.
  - Verified on macOS 26.6 with a test agent: a plist with `Disabled` is refused by `launchctl bootstrap` and never loaded;
    without it, it loads. The override's precedence is stock launchd behaviour, needs-boot (U9).
- **Removal**: `/private/var/db/piano-userland/receipt.tsv` lists what the installer put in place. A file the manifest no longer
  has is removed, its job booted out, when it is unchanged since. Anything else is left (host-tested).
- **SIGHUP is ignored**. Through the USB root shell, a restart of `pianousb-shell` ends that shell but not the installer, and
  the receipt is written before any job is touched.
- **Lines** go to stdout, which is the kernel log with `stderr-log=1`: `fd1: pid N sh: piano-userland: ...`. They also go to
  the system log (`logger -t piano-userland`) and to `/private/var/db/piano-userland/last.txt`.

## 6. The pmset defaults (`piano-pmset-defaults`)

`policy.md` §7's timers:

- **Battery Power**: `sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0`;
- **AC Power**: `sleep 0 displaysleep 10` (plugged in, it never sleeps).

The one-shot LaunchDaemon (`RunAtLoad`, `LaunchOnlyOnce`) runs at every boot. It returns at once once both power sources are
settled. Its state is `/private/var/db/piano-userland/pmset-defaults`.

- **Already set by the owner**: that source's dictionary in `/Library/Preferences/com.apple.PowerManagement*.plist` holds a
  `System Sleep Timer` or `Display Sleep Timer` that the script did not write. The files' sha256 after its own writes is kept.
  Such a source is never touched, then or later. That includes a change the owner makes after a partial run (host-tested).
  Verified on a Mac (macOS 26.6): powerd keeps the settings in those files, with exactly those key names per source.
- **A source pmset does not offer** (no `Battery Power:` section in `pmset -g custom`) is tried again at the next boot. That is
  the daily profile today: it has no `piano-power-ps=1`, so no battery is published.
- **A key not listed for the source** is left out and named. Applying each key on its own means one unsupported key cannot fail
  the rest.
- System sleep stays impossible until the kernel allows it: the ADT keeps `sleep-disabled`, and only k27 0062's `piano-sleep=1`
  overrides that (`power.md` §3). On today's profiles the sleep timer is inert, and only display sleep acts (2 min on battery,
  10 on AC). Display sleep and wake on NativeGopDisplay have not been exercised natively (U10).

## 7. Live update (design; its bundle is built and host-tested)

### 7.1 What can change without a reboot

| Fact | State | Source |
|---|---|---|
| The static and engineering trust cache modules are read once, at boot, from the payload's range in the device tree (§3) | verified (source) | `kern_trustcache.c` `load_static_trust_cache` |
| A trust cache loaded at run time must be an Image4 object (payload and manifest), validated by AppleImage4 through AMFI's `TrustCache.load`. The static, engineering and legacy types are refused at run time; the typed ones also need the caller's `com.apple.private.pmap.load-trust-cache` entitlement; no trust cache is ever unloaded (`xnu_unload_trust_cache`: not supported) | verified (source) | `load_trust_cache_with_type`, `xnu_load_trust_cache` |
| A bare module (`load_legacy_trust_cache`) is accepted only where the runtime allows legacy caches, which is bridgeOS | verified (source) | `trust_cache_runtime_init`, `XNU_HAS_LEGACY_TRUST_CACHE_LOADING` |
| `sysctl security.codesigning.trustcaches.{num_static,num_engineering,num_loadable}` count them | verified (source) | the same file |
| The kernel reports `AMFI: developer mode is force enabled on this platform` | verified (logs) | native runs |
| Ad hoc signed code outside the trust cache runs on stock macOS (every self-built tool does); natively, on this AMFI configuration | needs-boot (L1) | — |

So **no CDHash can be added to the running kernel** without an Apple-signed Image4 payload or a kernel change (the collection is
not changed here). What a live update can do:

| Change | Live (no reboot) |
|---|---|
| scripts and job files (no CDHash: the policy agent, the pmset defaults, every plist) | yes |
| a Mach-O whose CDHash the boot's trust cache already has (unchanged, or trusted ahead by an ESP) | yes |
| a Mach-O with a new CDHash | only if L1 shows untrusted ad hoc code runs. Then it runs as ordinary (non-platform) code, which is enough for these tools but not for code loaded into a platform process, such as the Metal plugin. Otherwise: **a new ESP with a trust cache that has it, and a reboot** |

The ESP step keeps the seed identity rules. The trust cache is its own payload file (`EFI/UniXNU/Payloads/kernel.macho.trustcache`).
An ESP can therefore change it and keep the **installed seed**: the same identity, so the store resumes and the owner's Data is
kept. `stage.py --live` writes exactly that trust cache: the installed one plus a module of the new CDHashes. It is a superset,
so `build-esp.py`'s missing-CDHash check (`data-store.md` §4) stays quiet. After a flatten, any seed may go with it.

### 7.2 The bundle: `stage.py --live DIR [--installed-trust FILE]`

The same build and staging tree as section 3, without a seed:

- `DIR/staging/`;
- `DIR/userland-update.tar`: deterministic, root:wheel, time 0, the same bytes on every run (host-tested);
- `DIR/apply-update.sh`;
- `DIR/live.json`.

The trust check compares each Mach-O's CDHash with the installed ESP's trust cache. That is `--installed-trust`, or the
`inputs.trust` of the ESP record that `PIANO_INSTALLED_SEED=<record>.json` names. Each one is listed as live, or as **NOT in the
installed trust cache**. When any is new, the command also writes `DIR/kernel.macho.trustcache` and prints the `build-esp.py`
line with the installed seed.

`apply-update.sh` runs on the tablet, as root. It:

1. checks the archive's sha256 (written into the script);
2. unpacks it to `/private/var/piano/userland.new` and verifies `SHA256SUMS`;
3. keeps the current directory as `userland.prev` and moves the new one into place;
4. runs `install.sh --live` under `nohup`, detached, with its log in `/private/var/db/piano-userland/live.log`.

### 7.3 Transports

- **The USB network** (`piano-usb-net=1`, `drivers/usb-device-debug.md` §14): the preferred path for files. On the Mac,
  `python3 -m http.server 8000 --bind <the Mac's address on the link>` in `out/userland/live`. On the tablet,
  `curl -O http://<that address>:8000/userland-update.tar -O http://<that address>:8000/apply-update.sh`. Or scp, once Remote
  Login is on.
- **The user pipe shell** (`piano-usb-shell=1`, `pianousb-shell`, `usb-console.py --port 1`): commands. Files go through it only
  as a base64 here-document in lines under the terminal's 1024-byte line limit. `usb-console.py` has no file mode yet, so this
  is still to build (section 7.6).

### 7.4 The procedure

1. Run `python3 tooling/userland/stage.py --live out/userland/live --installed-trust <the installed ESP's trust cache>`. That is
   the persist checkout's copy, for example `out/installed/kernel.macho.trustcache` (`data-store.md` T5).
2. If the report lists Mach-Os that are not in the installed trust cache, and L1 said untrusted code is refused:
   1. Build the ESP with `python3 tooling/persist/build-esp.py --profile daily --default macos --seed <the installed seed>
      --trust out/userland/live/kernel.macho.trustcache ...`.
   2. Install it (`USING.md` §5) and reboot. The old files keep working (their modules are kept), and the new CDHashes are
      trusted from this boot on.
   3. Then go on to step 3. This order leaves no moment with an installed binary the kernel does not trust.
3. Transport (7.3), then on the tablet run `sudo sh apply-update.sh`, then `cat /private/var/db/piano-userland/live.log`, then
   `sudo sh /private/var/piano/userland/install.sh --check`.
4. Rollback: `cd /private/var/piano && sudo mv userland userland.bad && sudo mv userland.prev userland && sudo sh
   userland/install.sh --live`. Files the bad update added are removed through the receipt.

### 7.5 The first time, on the owner's installed system

The daily profile has none of the transports yet: no `piano-usb-shell`, no `piano-usb-net`, and no `pianousb-shell` on Data. The
first update therefore needs the owner once, with the owner's own password:

1. Build an ESP with the installed seed, the trust cache of 7.4 step 2, and the daily profile's arguments plus
   `piano-usb-net=1`. It changes only arguments and the trust cache: the same seed, so no Data consequence.
2. After its boot, with the owner logged in, run in Terminal: `curl` the two files (7.3), then `sudo sh apply-update.sh`. The
   Mac can type the commands (`piano-input.py`, since the daily profile has `piano-usb-hid=1`). The owner types the password
   (`piano-input.py type --secret`).
3. From then on `pianousb-shell` is on Data. An ESP with `piano-usb-shell=1` gives the root shell for later updates, without
   the owner at the keyboard.

The alternative is a fresh Data from `out/userland/stage`'s seed (`build-esp.py --replace-data <installed 8 hex>`): everything is
installed at its first boot, and the owner's Data changes are lost. It is not recommended.

### 7.6 Still to build

- A host step that pushes the bundle through the user pipe (`usb-console.py --port 1` with a file mode, and the here-document
  above), with a read-back of the sha256.
- A host step that runs 7.4 end to end over NCM.

## 8. Host tests

```sh
python3 tooling/userland/test-stage.py            # 125 checks, about 2 minutes (hdiutil, the real units' builds)
python3 tooling/userland/test-stage.py --quick    # parts 1-3 (no k27 tree, no disk images): seconds
python3 package/PianoAuto/tests/run-host-tests.py # (piano-rotated's own, run by its build)
```

The parts:

1. The manifest and every refusal.
2. `install.sh` under `/bin/sh` and `/bin/dash`, on a scratch root with launchctl and logger stand-ins:
   - the first install, and a second run that touches nothing;
   - `--check` and tampering, a torn staged file;
   - a changed program (kickstart), a changed job file (bootout and bootstrap), a loaded job turned off and on again;
   - the owner's disable, an off job, nobody logged in;
   - the receipt's removals.
3. `piano-pmset-defaults.sh` with a pmset stand-in that writes powerd's settings file:
   - both sources set, in order, once;
   - a key this model lacks;
   - the battery appearing later;
   - the owner's timers first;
   - the owner's change after a partial run.
4. `add_trust`.
5. `cdhash` against codesign's CDHash.
6. The seed, on a 64 MiB bare APFS container image made with `hdiutil create -layout NONE -fs APFS -volname Data`, converted to
   raw and packed:
   - the files, modes and sha256s on the volume;
   - a stale file removed;
   - the boot command placed after `settime.sh` and before an endless loop;
   - `Disabled` in the off job, the trust cache, the record;
   - up to date on the second run, `--force`, `--out` refused, `--on`/`--off`;
   - `install.sh` run from the output seed's staging directory (end to end).
7. `--live`:
   - the trust report and its ESP trust cache;
   - the archive's determinism and `SHA256SUMS`;
   - `configs/userland.toml` itself: all three Mach-Os thin arm64 with CDHashes, reproducible, installed on a scratch root with
     the three daemons and the agent loaded, and `--check` clean.

## 9. Batched tablet test list (RAM boots; Data volatile; the owner's store untouched)

Build, on the Mac, from a checkout with these commits:

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh              # once: the KC and the current Turnip stage
python3 tooling/userland/test-stage.py && python3 tooling/userland/stage.py   # out/userland/stage (prints its identity)
PIANO_SEED=out/userland/stage/scratch-data.img PIANO_TRUST=out/userland/stage/kernel.macho.trustcache PIANO_GPU=2 PIANO_VMM_PRESENT=0 \
  PIANO_RESET_AFTER=1200 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-usb-shell=1 piano-power=1 piano-policy=1 stderr-log=1" \
  tooling/k27/build-native-image.sh                                            # image U (out/ram)
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
python3 tooling/native/usb-console.py --port 1                                # the root shell, from U2 on
```

The image's seed is not the installed one, so `build-ram-images.py` gives it `data-persist=0`. Data is volatile: the files are
installed at each boot of the image and gone afterwards.

| # | Step (root shell unless said) | Pass | On failure |
|---|---|---|---|
| U1 | boot; `grep -E 'trust cache module|piano-userland' logs/native-runs/<ts>-loop/xnu-console.txt` | `attempting to load 6 external trust cache modules`, `loaded ... module: 0` to `5`; `piano-userland: /usr/local/libexec/pianousb-shell: installed ...` for the 9 files; `com.unixnu.pianousb-shell: loaded in system` (and policy-agent, pmset-defaults); `--boot done: ok` | no `piano-userland` lines: the boot job did not run it (`stderr-log=1`? `/private/var/db/piano-userland/last.txt` from the shell, if there is one); a module not loaded: report, and keep the trust cache to 5 modules |
| U2 | `usb-console.py --port 1` | `pianousb-shell: a new shell on the tablet`: the chain seed → boot job → installer → launchd → trust cache → AMFI works | no prompt: `/var/log/pianousb-shell.log`; from the GUI (HID keyboard, Terminal): `launchctl print system/com.unixnu.pianousb-shell` |
| U3 | `launchctl list \| grep com.unixnu`; `ls -l /usr/local/bin /usr/local/libexec /Library/LaunchDaemons/com.unixnu.* /Library/LaunchAgents/com.unixnu.*`; `cat /private/var/db/piano-userland/last.txt` | pianousb-shell and piano-policy-agent with a PID, piano-pmset-defaults `-  0`; every file root wheel, 755/644 | a job missing: `launchctl print system/<label>` (its last exit reason) |
| U4 | `sysctl security.codesigning.trustcaches`; `piano-torch status; echo $?`; `codesign -dv /usr/local/libexec/pianousb-shell 2>&1 \| grep CDHash` | static + engineering = 6; `piano-torch: no PianoTorch service (boot with piano-torch=2)`, status 2 (it ran); the CDHash is the record's | `Killed: 9` on piano-torch: AMFI refused it in spite of the trust cache (report the console's AMFI lines) |
| U5 | `log show --last 10m --predicate 'process == "logger"' \| grep piano-policy-agent`; `sysctl machdep.piano_policy_lpm`; `notifyutil -s com.apple.system.lowpowermode 1; notifyutil -p com.apple.system.lowpowermode; sleep 1; sysctl machdep.piano_policy_lpm`; then the same with 0 | `started (pid N)`, `Low Power Mode 0: machdep.piano_policy_lpm=0`; after the post `machdep.piano_policy_lpm: 1` and `Low Power Mode 1: ...=1`; the console's `PianoPolicy: ... profile Power Saver (Low Power Mode)` (level 1: logged only) | no `started`: the job (U3); the sysctl unchanged: the notify name's state (`notifyutil -g com.apple.system.lowpowermode`) |
| U6 | after logging in: `launchctl print gui/$(stat -f %u /dev/console)/com.unixnu.piano-rotated \| grep -E 'state\|last exit'`; `launchctl asuser $(stat -f %u /dev/console) /usr/local/libexec/piano-rotated --probe` | the agent loaded, not running, last exit 0 (inert without `piano-rotate`); `--probe` prints its level-0 line, the displays, the setter, `canChangeOrientation` and changes nothing | not loaded: the login-window race (section 5); `--probe` killed: as U4 |
| U7 | `cat /private/var/db/piano-userland/pmset-defaults`; `pmset -g custom`; the logger lines of piano-pmset-defaults | `ac applied sleep 0 displaysleep 10`; `Battery Power: not offered by pmset yet` (no `piano-power-ps=1`); AC Power shows sleep 0, displaysleep 10 | `skipped ... the owner's ...` on a fresh volatile Data: powerd wrote timers by itself; note which keys (`plutil -p /Library/Preferences/com.apple.PowerManagement*.plist`) |
| U8 | `sh /private/var/piano/userland/install.sh --live`; `... --check; echo $?`; `cd /private/var/piano/userland && shasum -a 256 -c -s SHA256SUMS; echo $?` | `--live done: ok (changed:)`; `--check` status 0; SHA256SUMS 0 | a difference: the line names the file |
| U9 | `launchctl disable system/com.unixnu.piano-policy-agent; launchctl bootout system/com.unixnu.piano-policy-agent; sh .../install.sh --live`, then `launchctl enable ...` and `install.sh --live` again | `turned off by the owner (launchctl disable): left off`, then `loaded in system` | — |
| U10 | (before the pmset defaults go to the daily profile) `pmset displaysleepnow`; 5 s; `python3 tooling/native/piano-input.py key shift` from the Mac | the panel goes dark and comes back with the login window or the desktop | it stays dark: keep the unit off for the daily profile (`stage.py --off piano-pmset-defaults`) and file it against NativeGopDisplay (`drivers/backlight.md` §5) |

Image L: the same build with `piano-usb-net=1` added to `PIANO_EXTRA_ARGS`, once U passed and the NCM link of
`usb-device-debug.md` §14.5 is up (Internet Sharing, or self-assigned addresses).

| # | Step | Pass | On failure |
|---|---|---|---|
| L1 | Mac: `printf 'int main(void){return 42;}\n' \| xcrun clang -arch arm64 -mmacosx-version-min=14.0 -x c - -o out/userland/adhoc-probe` and serve `out/userland`; tablet: `curl -o /private/tmp/adhoc-probe http://<mac>:8000/adhoc-probe && chmod 755 /private/tmp/adhoc-probe && /private/tmp/adhoc-probe; echo $?` | **42**: untrusted ad hoc code runs. Live updates of these tools need no ESP step (section 7.1). Write the result into this page | `Killed: 9`: they need the ESP step (7.4 step 2); keep that order |
| L2 | Mac: `stage.py --off piano-pmset-defaults --live out/userland/live --installed-trust out/userland/stage/kernel.macho.trustcache`, serve `out/userland/live`; tablet: `cd /private/tmp && curl -O .../userland-update.tar -O .../apply-update.sh && sh apply-update.sh; sleep 5; cat /private/var/db/piano-userland/live.log; sh /private/var/piano/userland/install.sh --check` | the report says every CDHash is live (this image's trust cache has them); `com.unixnu.piano-pmset-defaults: off now: booted out of system`; `--live done: ok`; `--check` status 0 | `not the archive this script was made for`: the transfer (compare `shasum -a 256`) |
| L3 | rollback (7.4 step 4) | `userland.prev` back in place, `com.unixnu.piano-pmset-defaults: loaded in system` | — |

Then the owner's system: section 7.5, with the owner present.

## 10. Risks

1. **The boot job is the only root hook on a seed.** If a seed's boot job is not the expected shape, `stage.py` refuses: it does
   not write a new plist, which would be uid 99 and refused by launchd. The installer runs after `settime.sh`, before any later
   boot-job command.
2. **A new seed identity per staging run**: a daily ESP built from `out/userland/stage` would run the owner's Data volatile (or,
   with `--replace-data`, drop it). Section 4; `stage.py` prints it, and `build-esp.py` prints its own loud note. Use 7.5 for the
   owner's system.
3. **Trust cache module 0 is ours.** A malformed module 0 panics the boot, a risk the GPU staging already carries. `trust_bundle`
   sorts and formats it the same way, and U1 checks the sixth module loads.
4. **Platform binaries.** Our three Mach-Os are in the trust cache, so the kernel treats them as platform code. They link only
   system frameworks. A future unit that loads a non-system library would be refused by library validation.
5. **Disabled and the override database**: stock launchd semantics, verified for `Disabled` on macOS 26.6, the override's
   precedence needs-boot (U9).
6. **pmset**:
   - "Set by the owner" is inferred from powerd's settings file. If powerd writes timers by itself on a fresh Data, the defaults
     are skipped, which is conservative (U7 shows it).
   - The 2-minute display sleep on battery acts once a battery is published (`piano-power-ps=1`). Display wake natively is U10.
7. **Agents and the login window**: an agent installed after loginwindow started reaches only the next login window or login.
   The installer runs early in the boot job, which in the native boots starts well before WindowServer, so this is a corner case.
8. **The live path has no host step for the user pipe yet** (7.6). Its first use on the owner's system needs the owner's password
   (7.5).
9. **The planned `piano-wifi`**: its entry is ready, its source is not. `stage.py` skips it until `status = "planned"` is removed.

## 11. Files

- `configs/userland.toml`;
- `tooling/userland/stage.py`, `install.sh`, `piano-pmset-defaults.sh`, `com.unixnu.piano-pmset-defaults.plist`, `test-stage.py`;
- `tooling/native/piano-policy-agent/com.unixnu.piano-policy-agent.plist`;
- `tooling/native/pianousb-shell/pianousb-shell.c` (the boot-argument check), `com.unixnu.pianousb-shell.plist`.

The outputs are `out/userland/{build,stage,live}` (ignored).
