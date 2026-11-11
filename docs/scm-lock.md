# The secure-call lock: one owner of the secure world across kexts

Status, 2026-11-11: **built, host-tested and linked** (`package/include/PianoScmLock.h`; PianoA830 and PianoPower take it, PianoVideo
from its level 3); not run on the tablet. Code side and the full protocol: the header's comment.

## Why

Each kext that calls the secure world (SCM: SMCCC SMC64 yielding SiP calls) has its own `Sm8750Scm` and serialises only its own
calls (its `ops->lock`): PianoA830 (GPU_INIT_REGS, the SMMU aperture, PAS 13 for the ZAP shader, SET_REMOTE_STATE on slumber exit)
and PianoPower (PAS 36 and 1 for the DSP, PAS shutdown) could be in the secure world at the same time, and a PAS load is several
calls that belong together (init_image → mem_setup → the segments → auth_and_reset). Linux has one global lock per call
(qcom_scm's); the K90 port issued its DSP's PAS calls only after the GPU's ZAP load "so no two secure calls overlap"; on piano the
first DSP start stalled the whole system with a secure call as the leading suspect (`drivers/charging.md` §5a). PianoVideo adds PAS 9
and MP_VIDEO_VAR (`drivers/video.md` §4.3).

## The lock

| Item | What |
|---|---|
| Word | `sm8750_scm_owner`, 32 bits, defined by PianoA830 (`board/PianoA830.cpp`), next to its DRV-2 word `sm8750_rpmh_drv2_owner`: PianoA830 links before every user (`configs/k27-native-kexts.txt`), each user's descriptor names `com.unixnu.driver.PianoA830` as a library, so the KC resolves every import to this one copy. 0 free, else the holder's tag |
| Tags | `A830` 0x41383330, `POWR` 0x504f5752, `VIDE` 0x56494445 (`PianoScmTag*`) |
| Take | compare-exchange 0 → tag. A kext that already holds it (its own sequence, or another of its threads in a call) only counts one more hold: the count is the kext's, under its own hold mutex, held while waiting, so its threads queue behind its first waiter |
| Wait | 1 ms sleeps (callers are in thread context; yielding SCM calls already sleep); one line at the first wait, every second and when it comes free (`<kext>: secure-call lock (sm8750_scm_owner, ours ...): held by ... `) |
| Bound | 30 s, then the waiter **takes the word over** (a line, counted): a holder that long is stuck in the secure world (the Gunyah watchdog then resets the board anyway) or leaked the lock; taking over keeps the system running and gives no caller a new failure path |
| Give | the kext's last hold compare-exchanges tag → 0; a word taken over meanwhile is counted and left alone |
| Where | every yielding request: the kext's `Sm8750Scm` `ops->lock`/`unlock` take/give one hold after/before its own SCM mutex (lock order: SCM mutex → hold mutex). Every multi-call sequence: one hold around the whole sequence (the calls inside only count). Fast calls (GET_WQ_CTX in the wait-queue interrupt) take nothing |

| Kext | Requests | Sequences |
|---|---|---|
| PianoA830 | `scmLock`/`scmUnlock` in `Adreno830Start.cpp` through the board hook `adreno830_board_scm_hold` (ADRENO830_MACOS; a weak no-op for the host tests) | `stageZap`: `zap.load` (PAS 13 parse, metadata, init_image, mem_setup, the segment, auth_and_reset) |
| PianoPower | `scmLock`/`scmUnlock` in `PianoAdsp.cpp` | `startImage` (DTB PAS 36, then DSP PAS 1: metadata, init_image, mem_setup, segments, auth_and_reset) |
| PianoVideo | level 3+: its `Sm8750Scm` ops the same way | PAS 9 init_image .. auth_and_reset, then MP_VIDEO_VAR, as one section; resume's SET_REMOTE_STATE alone |

Without contention a take is one compare-exchange, so nothing else changes. A new user: define `PIANO_SCM_LOCK_KERNEL`, include
`<PianoScmLock.h>`, declare `PIANO_SCM_KERNEL_LOCK(name, tag, "Label")` with a tag of its own, name PianoA830 as a library in its
descriptor, list it after PianoA830, and add `--library-prefix _sm8750_scm_owner` to its build script's import check.

## Tests

`package/include/tests/run.sh` (run by the PianoA830 and PianoPower build scripts): nesting, waits and their reports, the bound and
the take-over, the lost give, unbalanced gives, bad arguments, and two kexts with one and two threads each doing sequences with
nested calls and lone calls: never both inside, the word free at the end. On the tablet: a boot with `piano-gpu=2` and
`piano-power=2` together shows either no lock line (no overlap) or `held by 0x...: waiting` followed by `free again`, and no
`TAKEN OVER`.
