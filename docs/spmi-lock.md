# The SPMI lock: one transaction at a time on the PMIC arbiter across kexts

Status, 2026-11-13: **built, host-tested and linked** (`package/include/PianoSpmi.h`; every kext that drives the arbiter runs its
transactions on it); not yet run on the tablet. Code side and the full protocol: the header's comment.

## Why

Each kext that reads or writes a PMIC drove the SPMI PMIC arbiter (v7, `spmi@c400000`, EE 0) with its own copy of the accessor:
PianoWifi's, PianoPower's and PianoInput's `SpmiObserver.h`, PianoThermal's `PmicAdc.h`, PianoTorch's `TorchCore.h`, direct code in
PianoPower, PianoBluetooth and PianoDwc3; PianoCamera, PianoUsbHost and PianoTorch read through PianoPower's copy. Nothing ordered
one kext's commands against another's. On the RAM boots of 2026-11-12/13 (same payload, charging level 2 with PianoThermal's ADC):

| Boot | What went wrong |
|---|---|
| v33p (23:10) | three of PianoWifi's regulator TYPE reads ended with an error status; level 2 stopped at step -201 |
| v33c (23:25) | ldof3's TYPE read failed through five tries, before the DSP started; the ADC's round 1 lost a conversion to a dropped write (status 0x9: done + dropped), 0 of 11 channels on |
| v33d (23:40) | passed after the 100 ms re-survey |
| v34p (01:19) | reads that ended **ok with wrong data**: ldof3 (an LDO, PPID 0x5c3) TYPE 0x00, smpi7 (an SMPS, 0x8ad) TYPE 0x04, an LDO's; level 2 failed at -201; the ADC stopped at conn2 with status 0x9 |

ESP v3.2 (charging level 1, no PianoThermal) had read every regulator. Linux's `spmi-pmic-arb` (studied for behaviour only) never
has two commands in flight on one bus: a bus-wide raw spinlock, taken with interrupts off, covers the command store, the poll for
done and the read of the data, for observer reads and owner-channel writes alike, whatever their APIDs (and a masked write's read
and write together). Nothing in it shows that v7's per-APID observer channels may run concurrently: v1 had a single channel per EE,
and "dropped" says a command was not performed. **Inferred** (not proven on the hardware): the channels of one EE share the
arbiter's command engine or its read data, so overlapping commands drop or return another command's bytes; the lock gives piano
Linux's regime whether or not that is the mechanism.

## The lock

| Item | What |
|---|---|
| Words | `piano_spmi_words` (`PianoSpmiWords`), defined by PianoA830 (`board/PianoA830.cpp`) next to its other shared words; every user names `com.unixnu.driver.PianoA830` as a library (PianoInput, PianoTorch and PianoUsbHost were added for it) and its build script's import check has `--library-prefix _piano_spmi_words`. `owner` 0 free, else the holder's tag; `holder` the thread; `depth`; every kext's counts |
| Tags | four letters of the kext's name after "Piano" (`PIANO_KEXT_NAME`, defined by `build-native-kext.py`): `WIFI`, `THER`, `POWE`, `CAME`, `DWC3`, `TORC`, `BLUE`, `INPU`, `USBH` |
| Take | interrupts masked on this CPU (`ml_set_interrupts_enabled(FALSE)`), then compare-exchange 0 → tag; while another holds it the waiter unmasks, spins 1 us, tries again. The same thread taking it again only counts (nesting) |
| Hold | one transaction: the data and command stored, the status polled (1000 × 1 us), the read data read; after a timeout up to 5 ms more until the channel reports done. Never a sleep, log line, mapping or allocation inside (PianoWifi now maps its observer window once, before the first command, instead of page by page) |
| Bound | a waiter that spins 20000 times (about 1 us each) while no new hold begins takes the word over (counted): a hold is at most about 6 ms; its own preemption and races lost to other waiters never count |
| Retries | observer reads: after a failure, a dropped command or a timeout, four more tries 500 us apart with the lock free (the 2026-11-12 retries); a denied read is final. Writes are not retried |
| Validation | PianoWifi reads each regulator's TYPE/SUBTYPE until two reads agree, reads a page of the wrong kind again (LDO 0x04; SMPS 0x03/0x1c/0x21/0x22), and surveys again (100 ms apart, up to 3 s) while a rail's page is unread *or of another kind*; `readStable` in the header for other users |
| Contexts | thread context everywhere today; an interrupt-context caller is safe too (the holder has interrupts masked, so a handler can never spin on its own CPU's holder) |
| Outside it | the k27 kernel's own accesses (0057 restart reason at EARLY_BOOT, 0060 RTC in `bsd_init`, 0062 in suspend, 0063 the boot guard's restart reason at SECS); RAM probes with `piano-reset-after` have no boot-guard write. Other EEs (the DSP's EE 3) are arbitrated by the hardware |

## Reading it on the tablet

One line per report, `SPMI lock: ours N tx (err E, timeout T, retry R, unstable U), waited W (max .. us, total .. us, last behind
TAG), nested .., taken over .., lost .., unbalanced .., max hold .. us; all kexts N tx, waited W (max .. us, last TAG behind TAG),
taken over .., max hold .. us`:

| Kext | Where |
|---|---|
| PianoWifi | `PianoWifi: SPMI: SPMI lock: ...` after the survey, `PianoWifi: level 2: SPMI lock: ...` after level 2 |
| PianoThermal | `PianoThermal: [Ns] adc: SPMI lock: ...` with each round line (round 1, every 10th, any event) |
| PianoPower | `PianoPower: [Ns] SPMI lock: ...` in every periodic report |
| PianoTorch, PianoCamera, PianoBluetooth, PianoUsbHost | once after their surveys |
| PianoInput | its `alive at` line (60 s, then every 5 min), in two lines |
| PianoDwc3 | the USB console's `spmi` command: the owner now, every kext's counts, its own |

Healthy: `taken over 0`, `lost 0`, `unbalanced 0`, `max hold` well under 1000 us, `err`/`timeout` 0 or near it; `waited` > 0 is
the lock doing its work (two kexts wanted the bus at once).

## Tests

`package/include/tests/spmi_lock_host.cpp` (run by `package/include/tests/run.sh`): the protocol (nesting, the mask balanced, the
bound's take-over, a lost hold, the line), the primitives on one channel (the command word, byte packing, retries with the lock
free between, the drain inside the hold, a denial final, `readStable`), and six threads running PianoWifi's, PianoPower's and
PianoInput's `SpmiObserver.h`, PianoThermal's `PmicAdc.h` and PianoTorch's write at once on a model with one command engine per EE
(a command stored while one is in flight is dropped; every observer channel shows the engine's last read data): without the lock it
must see failed reads, wrong values and wrong write read-backs, with it none. `PianoWifi/tests/wifi_host.cpp` `testPmicGlitch`: the
v34p shapes (an LDO's first TYPE read 0x00, an SMPS read three times as 0x04) are found; a TYPE that never reads the same twice is
no regulator and counts for the re-survey.
