# PianoTorch

Piano's rear torch for native macOS 27: the PM8550 flash LED module (`led-controller@ee00`, SID 1, PPID 0x1ee) with its two
factory-wired channels driven together, as Ubuntu v1.1.1 does. Backlog B12.

- `piano-torch=1`: a read-only survey over the SPMI observer channel: the flash APID and its owner EE, the module's identity,
  enables, per-channel current/strobe/timer, clamp, thermal thresholds, status bytes and raw bytes. The only store is the
  observer's read command.
- `piano-torch=2`: the survey, then torch control. Writes go only to that peripheral, only to the ten registers of
  `TorchCore.h`'s allowlist, each read-modify-write under its mask and read back. Off after `piano-torch-timeout` (300 s),
  on the module's own fault/over-temperature bits, at SoC >= `piano-torch-soc-max` (85 C, PianoThermal) or battery >=
  `piano-torch-batt-max` (45 C, PianoPower), when the enables read off, on system sleep and on shutdown. 50 mA by default
  (`piano-torch-ma`), never more than Ubuntu's 100 mA. `piano-torch-test=S` lights it once at boot for S seconds.

Control from macOS: `sudo piano-torch on|off|toggle|status` (`tooling/native/piano-torch`, an IORegistry property the kext
accepts from an administrator only); from other drivers: `PianoTorchSet` / `PianoTorchToggle` / `PianoTorchStatus`
(`PianoTorchClient.h`). Design, facts, the console hook PianoDwc3 could add, the test plan: `docs/drivers/torch.md`.

| File | What |
|---|---|
| `PianoTorch.cpp` | the IOService on IOPlatformExpertDevice: the thread, the SPMI windows, the report, the control loop, the properties, setProperties, callPlatformFunction, sleep and shutdown |
| `TorchCore.h` | portable: the survey, the write allowlist, the masked writes with read-back, the on/off sequences, the guard, the state |
| `TorchSpmi.h` | portable: the flash APID's port (observer reads, owner-channel writes) as TorchCore's bus |
| `FlashHw.h` | the facts, each with its source; the current/ITARGET/clamp arithmetic |
| `PianoTorchClient.h` | the names other code uses (properties, platform functions) |
| `tests/torch_host.cpp` | host tests against an SPMI arbiter v7 model |

Build: `tooling/k27/build-native-piano-torch.sh` (host tests, a store check of the portable headers, the user-space tool on a
Mac, the objects, the import check). PianoTorch is in `configs/k27-native-kexts.txt` and inert without `piano-torch`. It
includes `package/PianoPower/SpmiObserver.h` (not copied). Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
