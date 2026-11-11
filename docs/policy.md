# Policy: the cover, the keyboard's wake, the power key and Low Power Mode (PianoPolicy)

Status 2026-11-11: **built, host-tested (`policy_host: all 21670 checks passed`), linked into the KC with the current kext set
(kernels 0059 and 0062: `com.unixnu.driver.PianoPolicy: linked, ... 322 pointers`), test images built; not run on the tablet.**
Backlog B2 (cover / keyboard / power key) and B5 (Low Power Mode → power profile), `BACKLOG-hardware.md`. Inert without
`piano-policy=N`. The kernel side of sleep is `power.md` (k27 0062, the driver contract); this page is the policy on top of it.

Facts are marked as in `power.md`: **verified** (read from source, a binary or a register a running driver uses), **spec**,
**Ubuntu** (measured on this tablet by the Ubuntu project), **inferred**, **needs-boot**.

## 1. Purpose and the split

The drivers stay mechanisms: PianoKeyboard reads the hall sensor and gpio95, PianoPower publishes the power source, NativeGopDisplay
the display power, PianoThermal sets CPU caps, PianoA830 takes a GPU ceiling, k27 0062 suspends and polls the wake sources.
`package/PianoPolicy` decides what they mean (Ubuntu's rules, `ubuntu-history/device.md` §2.2-2.3, `kernel.md` §2.3/§2.9) and
says so through public calls only: IOPMrootDomain's power notifications, PianoThermal's and PianoA830's platform functions. The
rules are a pure state machine (`Policy.h`), tested on the host for every combination of cover × power source × display × Low
Power Mode × system sleep. A userland agent (`tooling/native/piano-policy-agent`) does the two things the kernel cannot (§2.1, §2.3).

## 2. The signals

### 2.1 macOS Low Power Mode is not kernel-visible

| Fact | State | Source |
|---|---|---|
| IOPMrootDomain relays 21 PM settings from `setProperties` to `setPMSetting` clients (Sleep On Power Button, the AutoWake/AutoPower/DebugWake keys, Wake On Ring/LAN/AC/Clamshell, Restart On Power Loss/Connect, TimeZoneOffset, DisplaySleepUsesDim, MobileMotionModule, GPUSwitch, ConsoleShutdown, ProModeControl/Defer, SilentRunning, LowLatencyAudioMode); none is Low Power Mode, any other key is "not handled" | verified | k27 `iokit/Kernel/IOPMrootDomain.cpp` `start`, `setProperties` |
| No aggressiveness type (`kPMGeneralAggressiveness`..`kPMMotionSensor`) and none of the root domain user client's 21 methods carries it | verified | `IOPM.h`, `IOPMLibDefs.h`, `RootDomainUserClient.cpp` |
| The kernel's only "low power mode" is a network interface flag (`SIOCSIFLOWPOWER`, `IFXF_LOW_POWER`) | verified | `bsd/net/if_low_power_mode.c` |
| powerd keeps Low Power Mode in its `PMPowerModeHandler`: the PM preference `LowPowerMode` per power source (`pmset -b/-c lowpowermode`), the notify name `com.apple.system.lowpowermode` with its state 0/1 (`notify_set_state`), `LPM Active` in its own (userland) power-source dictionary | verified (macOS 26 binary) | powerd's strings and the calls at their references |
| powerd's kernel-facing writes (`IORegistryEntrySetCFProperty`, `IOPMSetAggressiveness`, `IOPMSetSystemPowerSetting`, `sysctlbyname`, user clients of the root domain, AppleSmartBatteryManager, AppleSMC) are the energy settings, SleepDisabled, SilentRunning, hibernation, battery data; no Low Power Mode key | verified (macOS 26) | powerd's disassembly |
| `IOPMFeatureIsAvailable("LowPowerMode")` is decided in IOKit.framework by the model, not by the kernel's Supported Features: MacBookPro above 12, MacBookAir above 7, `MacNN,M` except 13,1-2 and 14,8 | verified (macOS 26) | IOKit.framework `IOPMFeatureIsAvailableWithSupportedTable` |
| The native ADT's model is `VirtualMac2,1`: Low Power Mode is then not offered (no toggle, `pmset` without `lowpowermode`) | inferred (macOS 27 not read); needs-boot: `pmset -g` | `configs/k27-adt.toml` |

So the signal is made, not found: **`sysctl machdep.piano_policy_lpm`** (0/1, root writes, PianoPolicy wakes on the write). The agent
mirrors `com.apple.system.lowpowermode` into it whenever macOS offers the mode; on piano today the switch is `sudo sysctl
machdep.piano_policy_lpm=1`. Changing the model to a MacBook's was rejected (every daemon keys on it).

### 2.2 The cover: IOPMrootDomain's clamshell

| Fact | State | Source |
|---|---|---|
| `receivePowerNotification(kIOPMClamshellClosed / Opened)` sets the root domain's clamshell, publishes `AppleClamshellState` / `AppleClamshellCausesSleep` and messages `kIOPMMessageClamshellStateChange`; closed with `shouldSleepOnClamshellClosed()` (= not `clamshellDisabled`, not desktop mode with AC, no disable bit) → `privateSleepSystem(kIOPMSleepReasonClamshell)` | verified | `IOPMrootDomain.cpp` `handlePowerNotification` |
| `kIOPMEnableClamshell` re-evaluates a closed clamshell (sleeps); `kIOPMDisableClamshell` does not; `kIOPMSetACAdaptorConnected` (`IOPMPrivate.h`, `| kIOPMSetValue` = connected) re-evaluates too; `acAdaptorConnected` starts true and nothing native sends it (PianoPower publishes `ExternalConnected` only) | verified | same; `PianoBattery.cpp` |
| A closed clamshell without AC cannot sustain a full wake (`checkSystemCanSustainFullWake`) | verified | same |
| powerd evaluates clamshell sleep too (desktop mode with AC, `AppliesOnLidClose` assertions → its own disable bit), tells WindowServer the clamshell state and posts `com.apple.system.powermanagement.clamshellstate` (state 1 closed) | verified (macOS 26) | powerd's strings; a Mac's `notifyutil -g` |
| `setClamShellSleepDisable` (the disable bits) is private; desktop mode means "an external display" to WindowServer and powerd | verified | `RootDomain.h` |

Choice: kIOPMEnableClamshell / kIOPMDisableClamshell and kIOPMClamshellClosed / Opened, as PianoKeyboard's `piano-keyboard-lid`
mode does (now in one place), with the AC adaptor mirrored so the root domain's own rules see the power source. Rejected: desktop
mode (WindowServer would treat the panel as an external display in clamshell mode and keep it lit), the private disable mask,
`sleepSystemOptions` (a software sleep without the clamshell state).

### 2.3 Display sleep and lock from the kernel: none

| Fact | State | Source |
|---|---|---|
| k27 is built `NO_KERNEL_HID`: no display wrangler; display power is WindowServer's (IOMobileFramebuffer → NativeGopDisplay) | verified | `drivers/backlight.md` §5 |
| No kernel request puts the display to sleep; the root domain's `kPMSetDisplayPowerOn` / `kPMSetDisplayState` go userland → kernel | verified | `RootDomainUserClient.cpp` |
| `requestUserActive(device, reason)` without a wrangler messages powerd (`kIOPMMessageRequestUserActive`, "name:reason"), which makes the user active: the display wakes | verified (source); needs-boot (the wake) | `IOPMrootDomain.cpp` |
| A Mac's WindowServer turns the built-in panel off on a closed clamshell; NativeGopDisplay's display is published as external | inferred: it likely stays lit; needs-boot | `drivers/backlight.md` §5 |

Choice (least invasive): the clamshell state of §2.2 and, in userland, `pmset displaysleepnow` when the clamshell closes without
sleep (the agent, on powerd's clamshell notification): the display sleeps and the session locks (System Settings › Lock Screen:
require the password immediately). `piano-backlight-lid=1` (PianoBacklight) cuts the backlight on close regardless.

### 2.4 The power key, the keyboard, the kernel's wake table, the profile outputs

| Fact | State | Source |
|---|---|---|
| k27's `kIOPMPowerButton` only takes a stackshot during a wake; power-button sleep is userland's (the HID Power usage, powerd's "Sleep On Power Button"); a driver's sleep request is `kIOPMSleepNow` (reason Software) | verified | `IOPMrootDomain.cpp` `receivePowerNotification` |
| 0062 arms the cover-open wake (bit 2) only when the cover reads closed at entry, the keyboard (bit 16) only with the cover open and PianoKeyboard's gpio95 latch configured and clear; gpio97 never; the mask is a boot argument | verified | `tooling/k27/piano-pm/piano_pm_policy.h`; `power.md` §3.3 |
| `machdep.piano_sleep` begins "on;" with piano-sleep=1 and carries "wake mask 0x..": PianoPolicy reads it once (no 0062 symbol linked: the KC links with the 0059 kernel too) | verified | `patches/k27/0062` `piano_sleep_format` |
| PianoKeyboard publishes `piano-keyboard-hall`, `-attached`, `-status` with a `kIOMessageServicePropertyChange` per change; what gpio95 does while the MCU runs is open | verified; needs-boot | `drivers/keyboard.md` §6c, §10.10 |
| NativeGopDisplay publishes `piano-display-power` only with `piano-backlight=2`, without a message | verified | `NativeGopDisplay.cpp` |
| PianoThermal mode 3 takes Power Saver caps on battery or unknown power, Balanced on external power; no call forces a profile | verified | `drivers/thermal.md` §5-6 |
| PianoA830's `piano-gpu-ceiling` is one slot (the last caller wins); PianoThermal (level 2) puts 160 MHz there at passive and clears it below; its `ThermalState` property is refreshed every 30 s, the breadcrumb `piano_thermal_state` at every policy update | verified | `board/PianoA830.cpp`, `PianoThermal.cpp` |
| PianoA830's own maximum is 342 MHz unless `piano-gpu-dcvs-max` / `piano-gpu-maxlevel` is given: the Power Saver ceiling is then a no-op and PianoPolicy sends none | verified | `adreno830/A830Dcvs.h`; `drivers/gpu.md` |

## 3. The policy table

| Event | Cover | Power | Display | System sleep (0062) | What happens |
|---|---|---|---|---|---|
| cover closes | was open since boot | battery | any | on, wake bit 2 | clamshell sleep allowed, closed → IOPMrootDomain sleeps (reason Clamshell); cover-open wake |
| cover closes | any | external or unknown | any | any | **lock only**: closed, no sleep; agent: display sleep → lock; `piano-backlight-lid=1`: backlight off |
| cover closes | not yet seen open, or | battery | any | off / no bit 2 | lock only, as above |
| unplug | closed | → battery | – | on, bit 2 | AC gone, then clamshell sleep allowed: the root domain re-evaluates and sleeps |
| plug in | any | → external | – | – | clamshell sleep refused first, then the AC adaptor (no sleep in between) |
| cover opens (awake) | → open | any | any | – | clamshell open + user active: the display wakes |
| cover opens (suspended) | → open | – | – | bit 2 | 0062 wakes (`piano.lid`), then as above |
| gpio95 rises | open, attached | any | off ≥ 1 s | – | user active (display wake), once per display-off period; closed, detached, display on or unknown: ignored |
| gpio95 rises (suspended) | open at entry, attached | – | – | bit 16 | 0062 wakes (`piano.keyboard`); the latch is PianoKeyboard's (§5.4) |
| gpio97 | – | – | – | – | never a wake source (PianoPolicy does not read it; 0062 never arms it) |
| power key, short | any | any | on or unknown | on | sleep now (`kIOPMSleepNow`) |
| power key, short | any | any | off | on | user active: the display wakes |
| power key | – | – | – | off; level 1; a long press | macOS's own (PianoInput's HID Power usage, as today) |
| power key (suspended) | – | – | – | bit 1 | 0062 wakes and swallows the press |
| profile | – | external, LPM off | – | – | **Balanced**: PianoThermal by the power source (0), no GPU ceiling |
| profile | – | battery or unknown, or LPM on | – | – | **Power Saver**: PianoThermal's battery caps (`piano-thermal-profile` 1 with LPM), GPU ceiling 342 MHz when PianoA830's own maximum is above it, never over PianoThermal's passive ceiling |
| idle | – | battery | – | on | macOS's idle sleep after 5 min (§7); PianoPolicy never overrides macOS's timers |

## 4. Design

- **The kext** (`PianoPolicy.cpp`): an IOService on IOPlatformExpertDevice, one thread blocked on an event. Events: PianoKeyboard's
  property messages (gpio95's rises are counted in the handler, so a short pulse is not lost), the power source's
  `kIOPMMessageBatteryStatusHasChanged`, the root domain's clamshell message and wake, a write of `machdep.piano_policy_lpm`, a
  watched service appearing (PianoKeyboard, IOPMPowerSource, NativeGopDisplay, PianoThermal, PianoA830). Each turn reads every input
  afresh (a few properties) and runs `Engine::step`. A 5 s tick runs only while a keyboard wake could come with the display state
  known (NativeGopDisplay sends no message: the 1 s hold-off is then between 1 and 6 s) or while our GPU ceiling shares PianoThermal's
  slot; otherwise no timer at all.
- **Level 1** logs every decision as "would send"; the platform functions answer kIOReturnUnsupported (PianoInput and PianoKeyboard
  keep their own behaviour). **Level 2** acts. The cover part needs `piano-policy-lid=1` as well (the hall polarity, `keyboard.md`
  §10.3) and refuses while `piano-keyboard-lid` is set (two clamshell controllers).
- **Safety rules** (`Policy.h`): the cover sleeps the system only on battery, with system sleep on, the cover-open wake armed and the
  cover seen open since boot (a boot inside a closed folio never sleeps at once); an unknown power source locks only; the order of
  the root domain's messages leaves no window where a closed cover on power could sleep; PianoThermal's passive ceiling is never
  written over or cleared (a race between the read and the write is repaired by putting it back).
- **The agent** (`tooling/native/piano-policy-agent/piano-policy-agent.sh`, root): one `notifyutil -w` on
  `com.apple.system.lowpowermode` and `com.apple.system.powermanagement.clamshellstate`; LPM → `sysctl -w machdep.piano_policy_lpm`;
  closed without sleep (root domain's `AppleClamshellCausesSleep` No) → `pmset displaysleepnow`. Stock tools, no polling.
  Installed into the Data seed by `tooling/k27/stage-soft-metal.py` (with the A830 staging's other arguments; a new seed identity,
  `docs/data-store.md`), or started by hand from the USB root shell with the same `launchctl submit`:
  ```sh
  --file tooling/native/piano-policy-agent/piano-policy-agent.sh:private/var/piano/piano-policy-agent.sh
  --boot-run "launchctl submit -l com.unixnu.piano-policy-agent -- /bin/sh /private/var/piano/piano-policy-agent.sh"
  ```
- Nothing to power: no power-tree membership; across a suspend the thread is frozen with the rest (contract, `power.md` §4).

## 5. What the other drivers must add (their owners; nothing here changes their packages)

**5.1 PianoInput, the power key.** Look the policy up once by class (`IOService::copyMatchingService(IOService::serviceMatching(
"PianoPolicy"))`, cached, released in `stop`; no link dependency). Power key only: on the debounced press send nothing yet and note
the time. Still held at 1500 ms (`pianopolicy::kLongPressMs`): a long press, send the HID Power usage down now and up at the release,
PianoPolicy is not asked. Released before: `r = policy ? policy->callPlatformFunction("piano-policy-power-key", false, (void
*)(uintptr_t)heldMs, nullptr, nullptr, nullptr) : kIOReturnUnsupported`; kIOReturnSuccess = consumed (no report at all), anything else
= the Power usage down and up now (today's behaviour, late by the press). One line: `power key: short press N ms: PianoPolicy took it`
or `...: macOS's (HID Power usage)`. This replaces the contract's direct `kIOPMSleepNow` (`power.md` §4): PianoPolicy sends it.

**5.2 PianoThermal, `"piano-thermal-profile"`.** `callPlatformFunction("piano-thermal-profile", false, (void *)(uintptr_t)mode, ...)`
on the PianoThermal service: 0 by the power source (today), 1 Power Saver whatever the source (the `-bat0/-bat1` ceilings), 2 Balanced
whatever the source (`-ac0/-ac1`), else kIOReturnBadArgument. An atomic the governor reads each tick (in `cpuTick`: the power source
it passes to `Governor::tick/enforce` becomes battery for 1, external for 2); mode 2 (fixed level) ignores it; any thread, no lock;
one line per change (`cpufreq: profile Power Saver (PianoPolicy)`), the 30 s report names it, a `piano-thermal-profile` property.
Until then PianoPolicy logs once and the CPU caps follow the power source (Low Power Mode on power then caps only the GPU).

**5.3 PianoA830, a ceiling per caller.** `"piano-gpu-ceiling"` with `param2 = (void *)(uintptr_t)client`: 0 (null: today's callers,
PianoThermal) and 1 (the power profile, PianoPolicy); the DCVS ceiling is the lowest non-zero slot (`adreno830_dcvs_set_ceiling(min)`),
and the service publishes `piano-gpu-ceiling-clients` = 2. PianoPolicy then passes client 1 and drops its interim arbitration (the
breadcrumb read, the 5 s tick, the put-back) when that property is there.

**5.4 PianoKeyboard.** With PianoPolicy, boot with `piano-keyboard-lid=0` (its lid mode stays for tests without PianoPolicy). In
`kOff` (contract §4), arm the gpio95 latch only when `callPlatformFunction("piano-policy-keyboard-wake", false, &latch, &now, ...)`
gives `latch` true (cover open and attached, system sleep on with bit 16; without PianoPolicy: its own rule, cover open). Drop key
and touchpad reports while `piano-keyboard-hall` is 0 (Linux's input-off-while-closed: a closed cover pressed in a bag neither types
nor wakes the display). Later, Ubuntu's power-follows-panel (0x25 = 0 and the sleep line while the display is off): on gpio95 rising
wake the MCU only when `now` is true; the display wake itself is PianoPolicy's (from the `-status` message), so keep messaging every
`-status` change.

**5.5 PianoPower (optional).** Send `kIOPMSetACAdaptorConnected | (external ? kIOPMSetValue : 0)` to the root domain on each change
of `ExternalConnected`, as a Mac's battery manager does; PianoPolicy's mirror then repeats the same value (harmless). Keep
`updateStatus()` on every change (PianoPolicy listens to its message).

**5.6 NativeGopDisplay.** Publish `piano-display-power` always (not only with `piano-backlight=2`) and message
`kIOMessageServicePropertyChange` on each change: the 5 s tick and the hold-off's 5 s uncertainty go away.

**5.7 PianoBacklight, the kernel.** Nothing: `piano-backlight-lid=1` is the backlight off on close; 0062's wake rules already
implement the cover-open-only and keyboard-only-when-open rules.

## 6. Boot arguments

| Argument | Default | Meaning |
|---|---|---|
| `piano-policy=N` | 0 | 0 inert (no probe); 1 decisions logged, nothing sent; 2 act |
| `piano-policy-lid=1` | 0 | act on the cover (clamshell, clamshell sleep, AC adaptor); only after the hall polarity is verified, and with `piano-keyboard-lid=0` |
| `piano-policy-profile=0` | 1 | no PianoThermal profile, no GPU ceiling |
| `piano-policy-gpu-mhz=M` | 342 | Power Saver's GPU ceiling (160..1100; 0 none); sent only when `piano-gpu-dcvs-max` (above M) or `piano-gpu-maxlevel` is given |
| `piano-policy-powerkey=0` | 1 | the power key stays macOS's |
| `piano-policy-kbd=0` | 1 | no display wake by gpio95 |
| `piano-policy-kbd-holdoff=MS` | 1000 | the display off at least this long before gpio95 counts (0..10000) |

Read, not owned: `piano-keyboard-lid`, `piano-gpu-dcvs-max`, `piano-gpu-maxlevel`, `piano-thermal`, `piano-thermal-gpu-mhz`. sysctl:
`machdep.piano_policy` (state, decisions, counters), `machdep.piano_policy_lpm` (root, 0/1).

## 7. Idle sleep on battery: macOS's timers (pmset)

The 5-minute idle sleep on battery (Ubuntu since v0.8.0) is macOS's own timer; PianoPolicy does not touch it:
```sh
sudo pmset -b sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0
sudo pmset -c sleep 0 displaysleep 10
```
(no hibernation natively; plugged in never sleeps, as stock and Ubuntu). powerd picks the Battery or AC set from PianoPower's
`ExternalConnected` (`piano-power-ps=1`). The lock: System Settings › Lock Screen › require the password immediately after the display
turns off. In the daily profile, both through the seed's boot job (`--boot-run "pmset -b sleep 5 displaysleep 2 hibernatemode 0
standby 0 powernap 0 tcpkeepalive 0"`, `--boot-run "pmset -c sleep 0 displaysleep 10"`), only once `power.md` §6 S1-S4 passed.

## 8. Lines

```
PianoPolicy: start: piano-policy=2 (acting), cover acted on, profile on, GPU ceiling 342 MHz (a no-op: PianoA830's own maximum is 342 MHz), power key on, keyboard wake on (hold-off 1000 ms)
PianoPolicy: system sleep: on, wake mask 0x13 (power key, cover open, keyboard); machdep.piano_sleep: on; state 1, poll 100 ms, wake mask 0x13, ...
PianoPolicy: watching PianoKeyboard (the cover)            PianoPolicy: watching AppleSmartBattery (the power source)
PianoPolicy: inputs: cover open (attached), battery, display on, Low Power Mode off, thermal unknown (change 3)
PianoPolicy: root domain: kIOPMSetACAdaptorConnected (none) (on battery)
PianoPolicy: root domain: kIOPMEnableClamshell (on battery: sleep)
PianoPolicy: root domain: kIOPMClamshellOpened (cover open)
PianoPolicy: decision: closing the cover: sleep (on battery: sleep); profile Power Saver (on battery); GPU ceiling 0 MHz; keyboard wake off, latch on
PianoPolicy: root domain says: clamshell closed, closing sleeps
PianoPolicy: root domain: user active requested (cover open): the display wakes
PianoPolicy: keyboard: gpio95 rising (4): display off, cover open: wake the display
PianoPolicy: power key: short press (180 ms), display on: the display is on: sleep now
PianoPolicy: PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling)
PianoPolicy: PianoThermal has no "piano-thermal-profile" (docs/policy.md 5.2): its CPU caps stay by the power source; ...
```
Level 1: `root domain: would send ...`, `PianoA830: would send GPU ceiling ...`. `sysctl machdep.piano_policy`: `level 2 (acting);
cover open, attached; battery; display on; Low Power Mode 0; sleep on, wake mask 0x13; thermal unknown; close: sleep (on battery:
sleep); profile Power Saver (on battery); GPU ceiling 0 MHz (written 0); keyboard wake 0, latch 1; sent: root 3, ...`.

## 9. Test plan (RAM boots, owner present, cable for the console)

**Build** (main checkout; the k27 tree at 0062):
```sh
KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1)
K27_KERNEL=$K/out/k27/kernel-k27-0062-native.macho PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh
tooling/k27/build-policy-test-images.sh        # out/ram-pol-{observe,lock,sleep,lpm} and out/ram-pol-dump
```
(Built offline here 2026-11-11 from an isolated checkout: KC with the 0062 kernel and PianoKeyboard `c72c2f2dddadc49a…`; images
`ram-pol-observe` `424723c0f235348a…`, `-lock` `fe38abc2235c35ed…`, `-sleep` `06b2dda81e190a44…`, `-lpm` `9ff286aad2bd7ba8…`; command line
898 bytes. Rebuild in the main checkout before a run.) Every image: PianoKeyboard 2, PianoPower with its battery (`piano-power-ps=1`),
PianoBacklight 2 (the display power), PianoThermal 1 + governor, the USB console. Run each with
`tooling/native/native-loop.sh out/ram-pol-<name>/boot-piano-macos-ram-A-bundled.img` and `usb-console.py --capture`.
Prerequisites: `keyboard.md` §8 (a)/(b) and the hall polarity (§10.3) before any `piano-policy-lid=1` image; `power.md` §6 S1-S4
before `sleep`.

| # | Image | Risk | Run | Pass | On failure |
|---|---|---|---|---|---|
| P1 | `ram-pol-observe` (900 s) | none: nothing sent | close and open the cover; plug and unplug a charger; root shell: `sysctl -w machdep.piano_policy_lpm=1`, `=0`, `sysctl machdep.piano_policy` | `start: piano-policy=1 (decisions only: nothing sent)`, `watching PianoKeyboard`, `watching AppleSmartBattery`, `inputs:` per change, `decision: closing the cover: lock (no system sleep ...)`, `root domain: would send kIOPMClamshellClosed`, the LPM write → `profile Power Saver (Low Power Mode)` | no `watching PianoKeyboard`: PianoKeyboard not at level 2; power `unknown`: PianoPower published no battery yet; display `unknown`: piano-backlight not 2 |
| P2 | `ram-pol-lock` (1200 s) | low: the root domain gets the clamshell; no sleep exists | close on battery and on power, open each time | `root domain: kIOPMDisableClamshell (no system sleep ...)`, `kIOPMClamshellClosed`, `root domain says: clamshell closed, closing does not sleep`, PianoBacklight's lid line and backlight off; open: `kIOPMClamshellOpened`, `user active requested (cover open)`; with the agent: the display sleeps and opening shows the lock screen | panel lit under the cover without the agent: expected (§2.3); the display does not come back at opening: `requestUserActive` does not wake it (report; the HID path still does) |
| P3 | `ram-pol-sleep` (1800 s) | medium: the cover sleeps the system | on battery: close, wait 30 s, open; plug in, close (stays awake), unplug while closed (sleeps), open; the Apple menu's Sleep, then the power key | `decision: closing the cover: sleep (on battery: sleep)`, `kIOPMEnableClamshell`, `kIOPMClamshellClosed`, `IOCPUSleepKernel enter (piano: suspend-to-idle)`, `woke after N s by lid`, `kIOPMClamshellOpened`; plug-in: `kIOPMDisableClamshell (on external power: lock only)` before `kIOPMSetACAdaptorConnected (connected)`; unplug while closed: `(none)`, `kIOPMEnableClamshell`, a suspend | closing on battery does not sleep: `root domain says: ... does not sleep` → powerd's own bit (`pmset -g assertions`: a lid-close assertion); a sleep right after a charger wake with the cover closed: §10 risk 3 |
| P4 | `ram-pol-lpm` (900 s) | low | on power: `sysctl -w machdep.piano_policy_lpm=1`, 60 s, `=0`; unplug | `=1`: `PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling)` and PianoA830's `gpu ceiling 342 MHz (an external limit ...)`, PianoThermal's missing-call line (until §5.2); `=0`: `GPU ceiling 0 MHz (Balanced ...)`, `gpu ceiling cleared`; unplugged: 342 MHz again | `not taken (no PianoA830 service ...)`: PianoA830 inert (piano-gpu=2) |

## 10. Risks

1. The hall polarity is unverified: the cover part is opt-in and a boot with the cover closed never sleeps until it was open.
2. WindowServer may keep the "external" panel lit under a closed cover: the agent's display sleep and PianoBacklight's lid rule.
3. A wake with the cover closed on battery may go straight back to sleep (the root domain's full-wake check; a Mac does the same);
   a charger wake (bit 4) with the cover closed races PianoPower's re-read and our kIOPMDisableClamshell against the root domain's
   delayed clamshell evaluation, and may sleep again while charging (harmless for the battery; B4's charge-while-awake is later).
4. powerd's own clamshell rule (lid-close assertions, desktop mode) can keep a closed cover awake: macOS's behaviour, by design.
5. Low Power Mode is likely not offered on VirtualMac2,1: the sysctl is the switch.
6. The GPU ceiling slot is shared: interim arbitration until §5.3 (a tiny race window, repaired); PianoThermal's profile call is
   missing until §5.2. With PianoA830's default maximum (342 MHz) neither matters.
7. `requestUserActive` is the kernel's only display-wake request: needs-boot that powerd honours it natively.
8. `kIOPMSetACAdaptorConnected` is a private root-domain message (IOPMPrivate.h); it also reaches DriverKit's power-source hook.
9. PianoPolicy links PianoThermal's breadcrumb: PianoThermal must stay in the KC (it is listed first).

## 11. Files

`package/PianoPolicy/` (`PianoPolicy.cpp`, `Policy.h`, `tests/policy_host.cpp`, `README.md`, `UPSTREAM`, `LICENSE`),
`tooling/k27/build-native-piano-policy.sh`, `tooling/k27/kext/PianoPolicy.json`, `configs/k27-native-kexts.txt`,
`tooling/k27/build-policy-test-images.sh`, `tooling/native/piano-policy-agent/piano-policy-agent.sh`.
