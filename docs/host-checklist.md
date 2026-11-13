# Host-side checklist (no tablet, no boot, no judgment mid-run)

Simple checks another assistant can run on the build Mac while tablet work is paused (written 2026-11-13 at the owner's pause).
Each item names the command, where to run it, its inputs, what counts as a pass, and where the result goes. None of them needs
the tablet, a debug login, or a decision while it runs. Only macOS 27.3 matters: the 27.0.1 line is retired, so nothing here
builds or checks a 27.0.1 image.

## Ground rules

- **Never run** anything that touches the tablet or the installed system: `fastboot`, `tooling/native/usb-console.py`,
  `tooling/native/native-loop.sh`, `tooling/persist/maint-session.py`, `tooling/native/desk-ssh.sh`, any `/tmp/run-*.sh` helper.
- **Never run** a line build or an ESP build: `tooling/k27/build-native-image.sh`, `tooling/k273/build-k273-image.sh`,
  `tooling/persist/build-esp.py`, `tooling/k273/build-dual-esp.py`, `tooling/bootimg/build-ram-images.py`.
- **Never publish**: no `private/publish.sh`, no `git push`, no commits. Leave tracked files unchanged; if a check writes into
  `out/`, that is listed below.
- Do not run two checks that write `out/native-kexts/` at the same time, and none while someone builds in the same checkout.
- Working directory for every item: the repository root of the main checkout (`UniXNU-piano/`). Run `source ~/.config/piano/env`
  first in the same shell (it sets `PIANO_K27_TREE`, `PIANO_V110` and the backup paths) and use `bash` for shell snippets.
- In the tables, `\|` is Markdown's escape of `|`: type a plain `|` in the shell.
- **Results:** make one directory per session, `R=private/host-checks/$(date +%Y%m%d-%H%M); mkdir -p "$R"` (`private/` is
  git-ignored). Each item writes `"$R"/NN-name.log` (its full output, `2>&1`). At the end write `"$R"/SUMMARY.md` with one line
  per item: number, name, PASS / FAIL / KNOWN (a listed known failure) / SKIPPED (with the reason), and for a FAIL the last
  20 lines of its log. Report only the summary; do not try to fix anything.

## 1. Shared headers and kernel policy (pure host tests)

| # | Command | Pass |
|---|---|---|
| 1.1 | `bash package/include/tests/run.sh > "$R"/11-include-tests.log 2>&1` | exit 0; no `FAIL` line. Covers the SPMI lock (`spmi_lock_host`), the MM votes, the SCM lock, the WCN rails. **Known:** `spmi_lock_host.cpp:128` failed once while the Mac was busy; on a failure run it again alone and record both runs |
| 1.2 | `bash tooling/k27/test-piano-bootargs.sh > "$R"/12-bootargs.log 2>&1` | exit 0 (k27 0066's string handling, also under the sanitizers, and the patch carries its header byte for byte) |
| 1.3 | `bash tooling/k27/test-piano-guard.sh > "$R"/13-guard.log 2>&1` | exit 0 (k27 0063, the boot guard) |
| 1.4 | `bash tooling/k27/test-piano-pm.sh > "$R"/14-pm.log 2>&1` | exit 0 (k27 0061/0062/0064, idle and suspend decisions) |
| 1.5 | `python3 tooling/adt/boot_args.py --self-test > "$R"/15-boot-args-py.log 2>&1` | exit 0 (the Python side of 0066 against the kernel header's numbers) |
| 1.6 | `python3 tooling/k27/check-native-kexts.py --self-test > "$R"/16-native-kext-rule.log 2>&1` | exit 0 (the native kext rule on synthetic descriptors) |
| 1.7 | `python3 tooling/metal/check-bundle-paths.py --self-test > "$R"/17-bundle-paths.log 2>&1` | exit 0 |

## 2. Per-driver host tests (each script runs its kext's host tests first, then compiles the kext object)

Inputs: `PIANO_K27_TREE` from the env file. Use the 27.3 kernel for the import checks: `export
K27_KERNEL=out/ram-k273-bootargs/kernel-k273-0066-s0001-native.macho` (if that file is missing, mark the section SKIPPED). Each
script writes its objects to `out/native-kexts/<NAME>/` (expected; nothing else).

Run them one after the other:

```sh
for s in ufs ram-block gop-display software-aes null-ethernet standin piano-a830 piano-power piano-thermal piano-wifi \
         piano-wifi-ethernet piano-dwc3 piano-keyboard piano-touch piano-backlight piano-audio piano-camera piano-torch \
         piano-input piano-sensors piano-bluetooth piano-dpu piano-dp piano-video piano-policy piano-usbhost usbhost-standin; do
  f=tooling/k27/build-native-$s.sh; [ -f "$f" ] || { echo "SKIPPED $s (no script)"; continue; }
  bash "$f" > "$R/2-$s.log" 2>&1; echo "$s exit $?"
done > "$R"/20-drivers.log 2>&1
```

Pass per driver: exit 0, and its log has no line starting with `FAIL` and no `Error:`. A host-test summary such as
`... all passed` / `PASS` appears in most of them. Record the per-driver exit codes from `20-drivers.log` in the summary.

## 3. Userland and persistence tooling

| # | Command | Pass |
|---|---|---|
| 3.1 | `python3 tooling/userland/test-stage.py > "$R"/31-stage.log 2>&1` | exit 0 (the system layer pipeline in a temp directory; it must list `piano-login-front` among the agents) |
| 3.2 | `python3 tooling/persist/test-data-store.py > "$R"/32-data-store.log 2>&1` | exit 0 |
| 3.3 | `python3 tooling/persist/test-persist.py > "$R"/33-persist.log 2>&1` | **KNOWN failure** until the reference images are restored: `FileNotFoundError ... out/persistent/ref/13-boot_a.img`. Record KNOWN if that is the error; any other failure is a FAIL |

## 4. Image and record checks (read only)

| # | Command | Pass |
|---|---|---|
| 4.1 | `for j in out/ram-k273-*/build-A-bundled.json; do echo "== $j"; python3 tooling/bootimg/verify-ram-images.py "$j" 2>&1 \| tail -1; done > "$R"/41-verify-k273.log` | every 27.3 image set prints `PASS: 0 failure(s)`. The image limit is 250,000,000 bytes: an older oversized set (if one still exists) must FAIL; name it in the summary |
| 4.2 | `shasum -a 256 out/persistent/esp-dual33d-k273d.img > "$R"/42-esp.log; python3 -c "import json;print(json.load(open('out/persistent/esp-dual33d-k273d.json'))['sha256'])" >> "$R"/42-esp.log` | both lines show `93baea7af730611145f97e51b09b1779dbacd043700aa5f156b044d56a855f59` (the ESP installed on 2026-11-13 09:45) |
| 4.3 | `python3 tooling/adt/check-ram-window.py --adt out/ram-k273-bootargs/piano-k27.adt --kc out/ram-k273-bootargs/kc-k27-native.bin --fd out/fd/piano-uefi-A-bundled.fd > "$R"/43-ram-window.log 2>&1` | exit 0; the last line names the window `0x8c0004000..0xbffe04000 (13310 MiB)` |
| 4.4 | `python3 tooling/adt/boot_args.py --show out/ram-k273-bootargs/piano-k27.adt > "$R"/44-adt-args.log 2>&1` | exit 0; one set `piano-boot-args-ram` of 408 bytes is listed |
| 4.5 | `for d in "$PIANO_BACKUP_DIR"/persistent-boot-*/; do echo "== $d"; (cd "$d" && shasum -a 256 -c SHA256SUMS); done > "$R"/45-backups.log 2>&1` | every file `OK`; no `FAILED`. These are the pre-write backups of the tablet's partitions on the external SSD (read only) |
| 4.6 | `(cd "$(eval echo "$PIANO_BACKUP_NAS")" 2>/dev/null && for d in persistent-boot-2026111[23]-*/; do echo "== $d"; (cd "$d" && shasum -a 256 -c SHA256SUMS); done) > "$R"/46-backups-nas.log 2>&1` | if the NAS is not mounted: SKIPPED. Otherwise every file `OK`, and every set of 4.5 also exists here (the background copy finished) |

## 5. Log checks against the recorded results (read only)

The USB console logs of the 27.3 boots are kept in `logs/usb-console/` (git-ignored). Each check is a `grep -a` and a count;
write the output to `"$R"/5N-*.log`.

| # | Log (`logs/usb-console/…-port0.txt`) | Command | Pass |
|---|---|---|---|
| 5.1 | `20261113-104131` (27.3 Wi-Fi probe) | `grep -a -E "scan round 2 .*18 network\(s\)\|piano-wifi-scan-summary\|piano-boot-args:" logs/usb-console/20261113-104131-port0.txt` | a `scan round 2 (with the receive buffers): completed, ... 29 management frame(s), 18 network(s)` line |
| 5.2 | `20261113-102514` (0066 test) | `grep -a -E "PianoAudio: start: piano-audio=1\|PianoAudio: survey done" logs/usb-console/20261113-102514-port0.txt` | both lines present |
| 5.3 | `20261113-015232`, `20261113-015906` (the 27.3 store's first two boots) | `grep -a -h "NativeRamBlock: Data store state" logs/usb-console/20261113-015232-port0.txt logs/usb-console/20261113-015906-port0.txt` | state 3 (fresh) then state 2 (resumed), both `first LBA 117998394` and `seed 89860516` |
| 5.4 | `20261113-094514` (the installed 27.3's first boot) | `grep -a -E "root-part=linux_data\|SoftwareAES: NativeAES registered\|Data store state 2" logs/usb-console/20261113-094514-port0.txt` | all three present |
| 5.5 | every 27.3 log of the day | `for f in logs/usb-console/20261113-*-port0.txt; do grep -a -q "root-part=linux_data" "$f" \|\| continue; printf "%s panic=%s\n" "$f" "$(grep -a -c -E 'panic\(\|Unhandled implementation specific' "$f")"; done` | every count is 0 |

## 6. Repository and document consistency (read only)

| # | Command | Pass |
|---|---|---|
| 6.1 | Broken relative links in README and docs: the snippet below | last line `broken 0`; otherwise the list (no fixing) |
| 6.2 | Privacy audit of the tracked tree without publishing: `P=$(sed -n "s/^P='\(.*\)'$/\1/p" private/publish.sh); git grep -I -l -E "$P" HEAD -- . > "$R"/62-privacy.log; wc -l < "$R"/62-privacy.log` | `0` (no tracked file matches the private-inventory pattern). Do not print the pattern itself into any shared place |
| 6.3 | Working tree clean: `git status --short > "$R"/63-status.log; wc -l < "$R"/63-status.log` | `0` before and after the session (the checks above must not change tracked files) |
| 6.4 | README states the current system: `grep -n -E "macOS 27\.3 \(26C5035r\)" README.md > "$R"/64-readme.log` | at least one line |
| 6.5 | The native kext list and its descriptors agree: `python3 tooling/k27/check-native-kexts.py . tooling/k27/kext/*.json > "$R"/65-kext-list.log 2>&1` | exit 0 |

The snippet for 6.1:

```sh
python3 - > "$R"/61-links.log <<'EOF'
import re, pathlib
root = pathlib.Path('.')
bad = 0
for md in [root / 'README.md', *root.glob('docs/**/*.md')]:
    for t in re.findall(r'\]\(([^)#]+)(?:#[^)]*)?\)', md.read_text(errors='ignore')):
        if '://' in t or t.startswith('mailto:'):
            continue
        if not (md.parent / t).exists():
            print(md, '->', t)
            bad += 1
print('broken', bad)
EOF
```

## 7. What to hand back

`"$R"/SUMMARY.md` and the path of `"$R"`. A FAIL is information for the next session, not something to fix during this run.
