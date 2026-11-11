# Policy: the cover, the keyboard's wake, the power key and Low Power Mode (PianoPolicy)

Status 2026-11-11: **built, host-tested (`policy_host: all 21670 checks passed`), linked into the KC with the current kext set
(kernels 0059 and 0062: `com.unixnu.driver.PianoPolicy: linked, ... 322 pointers`), test images built; not run on the tablet.**
2026-11-12: the GPU ceiling as PianoA830's client 1 (§5.3: no interim arbitration with its slots), the profile checked end to end
through PianoThermal's `Governor.h` (§5.2), `piano-policy-lpm=1` for an unattended test (P7); `policy_host: all 21915 checks passed`;
not run. Later on 2026-11-12: NativeGopDisplay's display power is an event (§5.6: its messages listened to, the 5 s keyboard tick
gone; the tick only for a NativeGopDisplay without them, logged once); `policy_host: all 22217 checks passed`; not run.
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
| A kext's `kernel_sysctlbyname` of an OID without `CTLFLAG_KERN` fails with EPERM (1): `sysctl_root` refuses an in-kernel caller there, after the name lookup (an OID with the flag) has found it; a missing OID is ENOENT (2). Every 0062-0064 kernel built before 2026-11-12 declared `machdep.piano_sleep` without the flag (its `oid_kind` 0x80e00003 in kernel-k27-0062/0063/0064-native.macho), so PianoPolicy logged "absent (error 1 ...)" and never slept on close (queue5 P10). 0062 now sets it (0x81e00003); on the older kernels PianoPolicy takes `piano-sleep` / `piano-sleep-wake` the way 0062 reads them (`Policy.h` `resolveSleep`, host-tested) | verified (source, the kernels' OID words); needs-boot (the new lines) | `bsd/kern/kern_newsysctl.c` `sysctl_root`; `patches/k27/0062` |
| PianoKeyboard publishes `piano-keyboard-hall`, `-attached`, `-status` with a `kIOMessageServicePropertyChange` per change; what gpio95 does while the MCU runs is open | verified; needs-boot | `drivers/keyboard.md` §6c, §10.10 |
| NativeGopDisplay publishes `piano-display-power` on every power change call and messages `kIOMessageServicePropertyChange` (argument `"piano-display-power"`) on each change (since 2026-11-11; before: only with `piano-backlight=2`, no message); since 2026-11-12 its `piano-display-power-messages` = true (set before it registers) says so | verified | `NativeGopDisplay.cpp` `publishDisplayPower`, `start` |
| PianoThermal mode 3 takes Power Saver caps on battery or unknown power, Balanced on external power; `"piano-thermal-profile"` (§5.2, 2026-11-11) forces either, `kIOReturnUnsupported` below `piano-cpufreq=2` | verified | `drivers/thermal.md` §5-6, `Governor.h` |
| PianoA830's `piano-gpu-ceiling` has a slot per caller since 2026-11-11 (`piano-gpu-ceiling-clients` = 4, published before its service is; the DCVS runs at the lowest slot); PianoThermal (level 2) puts 160 MHz in slot 0 (param2 null) at passive and clears it below; its `ThermalState` property is refreshed every 30 s, the breadcrumb `piano_thermal_state` at every policy update. A PianoA830 from before the slots had one slot, the last caller winning | verified | `board/PianoGpuCeiling.h`, `board/PianoA830.cpp`, `PianoThermal.cpp` |
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
  watched service appearing (PianoKeyboard, IOPMPowerSource, NativeGopDisplay, PianoThermal, PianoA830), and since 2026-11-12
  NativeGopDisplay's `piano-display-power` messages (§5.6). Each turn reads every input afresh (a few properties) and runs
  `Engine::step`. **No timer** in the normal case: the cover and gpio95 are PianoKeyboard's messages, the power source PianoPower's
  `kIOPMMessageBatteryStatusHasChanged`, the display NativeGopDisplay's message, so the keyboard's 1 s hold-off is timed from the
  display's change itself. A 5 s tick only as a fallback: while a keyboard wake could come with the display state known **and** the
  display sends no messages (a NativeGopDisplay without `piano-display-power-messages` from which no message was seen yet: an older
  KC; logged once: `keyboard wake: NativeGopDisplay sends no display-power messages ...`; the hold-off is then between 1 and 6 s),
  or with `piano-policy-display-tick=1` (the old behaviour), or, with a PianoA830 that has no ceiling slots, while our GPU ceiling
  shares PianoThermal's slot (its breadcrumb has no message). `Engine::tickWhy` names the reason; `machdep.piano_policy` ends with
  `display messages N (events|polled), tick T ms`. A NativeGopDisplay with the property replaces one without it (D6's second
  instance shares the class and publishes no display power).
- **Level 1** logs every decision as "would send"; the platform functions answer kIOReturnUnsupported (PianoInput and PianoKeyboard
  keep their own behaviour). **Level 2** acts. The cover part needs `piano-policy-lid=1` as well (the hall polarity, `keyboard.md`
  §10.3) and refuses while `piano-keyboard-lid` is set (two clamshell controllers).
- **Safety rules** (`Policy.h`): the cover sleeps the system only on battery, with system sleep on, the cover-open wake armed and the
  cover seen open since boot (a boot inside a closed folio never sleeps at once); an unknown power source locks only; the order of
  the root domain's messages leaves no window where a closed cover on power could sleep; PianoThermal's passive ceiling is never
  lifted: with PianoA830's slots ours is client 1 and the lowest slot wins (§5.3), without them ours is never written over or
  cleared (a race between the read and the write is repaired by putting it back).
- **The agent** (`tooling/native/piano-policy-agent/piano-policy-agent.sh`, root): one `notifyutil -w` on
  `com.apple.system.lowpowermode` and `com.apple.system.powermanagement.clamshellstate`; LPM → `sysctl -w machdep.piano_policy_lpm`;
  closed without sleep (root domain's `AppleClamshellCausesSleep` No) → `pmset displaysleepnow`. Stock tools, no polling.
  Installed by the userland pipeline (`configs/userland.toml`, `docs/userland.md`): `/usr/local/libexec/piano-policy-agent` and
  the LaunchDaemon `com.unixnu.piano-policy-agent.plist`, on in the installed profile, through the Data seed's boot job (a new
  seed identity, `docs/data-store.md`) or a live update. By hand from the USB root shell:
  `launchctl submit -l com.unixnu.piano-policy-agent -- /bin/sh <the script>`.
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
Where PianoThermal does not take it (a PianoThermal older than this, or `piano-cpufreq` < 2, which answers `kIOReturnUnsupported`)
PianoPolicy logs once and the CPU caps follow the power source (Low Power Mode on power then caps only the GPU). **Done** in
PianoThermal (status below); PianoPolicy sends 1 with Low Power Mode and 0 otherwise (Balanced is PianoThermal's own choice on external
power, §3). Checked end to end on the host (`policy_host` `profileReachesThermal`): every power source and Low Power Mode transition,
PianoPolicy's decision, the mode sent and PianoThermal's own `Governor.h` `profilePower` give the battery caps exactly when PianoPolicy
decided Power Saver and the external-power caps exactly when it decided Balanced (an unknown source is battery caps on both sides).

**5.3 PianoA830, a ceiling per caller.** `"piano-gpu-ceiling"` with `param2 = (void *)(uintptr_t)client`: 0 (null: today's callers,
PianoThermal) and 1 (the power profile, PianoPolicy); the DCVS ceiling is the lowest non-zero slot (`adreno830_dcvs_set_ceiling(min)`),
and the service publishes `piano-gpu-ceiling-clients` = 2. PianoPolicy then passes client 1 and drops its interim arbitration (the
breadcrumb read, the 5 s tick, the put-back) when that property is there. **Done in PianoA830 (2026-11-11, host-tested, not run):** 4
slots (0 thermal, 1 the power profile, 2-3 free), `piano-gpu-ceiling-clients` = 4, a client past them refused with kIOReturnBadArgument
(`package/PianoA830/board/PianoGpuCeiling.h`, `drivers/gpu.md` §8 "Ceiling slots"). **Done in PianoPolicy (2026-11-12, host-tested,
not run):** with `piano-gpu-ceiling-clients` >= 2 on the PianoA830 service, Power Saver's ceiling goes with `param2 = 1`; PianoThermal's
passive ceiling (slot 0) stays in force by itself, so `piano_thermal_state` is not read, the GPU needs no 5 s tick and nothing is put
back. The slot is read from the very service each call goes to (a PianoA830 published between a turn's inputs and its call still
gets client 1) and, once seen, holds for the boot. Without the property (a PianoA830 from before the slots, or none yet) the old
path is unchanged: param2 null, the breadcrumb, the tick while ours is in the slot, the put-back after a race. One line names the
path when PianoA830 is first seen (§8); `machdep.piano_policy` shows `(written N, client 1)` or `(written N, shared slot)`.

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
`kIOMessageServicePropertyChange` on each change: the 5 s tick and the hold-off's 5 s uncertainty go away. Its
`piano-display-power-messages` = true (published before the service registers) tells a reader so without waiting for a change.

**5.7 PianoBacklight, the kernel.** Nothing: `piano-backlight-lid=1` is the backlight off on close; 0062's wake rules already
implement the cover-open-only and keyboard-only-when-open rules.

**Status** (2026-11-11: built, host-tested, linked into the test KC with the 0063 kernel; not run on the tablet):

| § | Driver | State |
|---|---|---|
| 5.1 | PianoInput | **done** (`PowerKey.h`, `buttons_host` `testPowerKey`): held back only while a PianoPolicy service is there and the power key is reported at all (`piano-input-mask`); still down at 1500 ms → the HID Power usage down then, up at the release; released before → `"piano-policy-power-key"` with the press in ms, `kIOReturnSuccess` = consumed, else the usage down and up at once. Without PianoPolicy exactly as before. Lines `power key: short press N ms: PianoPolicy took it` / `...: macOS's (HID Power usage)`, `power key: held 1500 ms: a long press, ...` |
| 5.2 | PianoThermal | **done** (`Governor.h` Profile, `thermal_host` `testProfileAndSleep`): 0 / 1 / 2, else `kIOReturnBadArgument`; `kIOReturnUnsupported` without CPU frequency control (`piano-cpufreq` < 2); the governor's power source per tick, the fixed level ignores it; `cpufreq: profile Power Saver (PianoPolicy)`, the 30 s report's `(on power; profile Power Saver)`, the property `piano-thermal-profile` |
| 5.3 | PianoA830, PianoPolicy | **done**: PianoA830's 4 slots (2026-11-11, `test_gpu_ceiling`); PianoPolicy as client 1 when `piano-gpu-ceiling-clients` >= 2, the interim arbitration only without it (2026-11-12, `policy_host` `ownGpuSlot`) |
| 5.4 | PianoKeyboard | **done** (`CoverPower.h`, `keyboard_host` `testCoverPower`): `piano-keyboard-lid=0` stays the default and the messages per change stay; gpio95's latch at a system sleep from an acting PianoPolicy (its `piano-policy` property 2: at level 1 the function answers success with false, which is not taken), else the own rule (attached, not closed); key and touchpad reports dropped while closed behind `piano-keyboard-lid-drop=1` (opt-in until the hall polarity is verified); power-follows-panel behind `piano-keyboard-blank=1` (gpio95 rising while blanked wakes the MCU only with `now`) |
| 5.5 | PianoPower | not done (optional): PianoPolicy's mirror sends `kIOPMSetACAdaptorConnected`; `updateStatus()` on each change is unchanged |
| 5.6 | NativeGopDisplay, PianoPolicy | **done**: `piano-display-power` on every power change call whatever the arguments, a `kIOMessageServicePropertyChange` on each change; `piano-display-power-messages` (2026-11-12). PianoPolicy (2026-11-12, host-tested `policy_host` `displayEventsTick`, not run) registers interest on it and drops the 5 s keyboard tick when the property is there or a message was seen; the tick stays only as the fallback for an older NativeGopDisplay (logged once) and behind `piano-policy-display-tick=1` |
| 5.7 | PianoBacklight | nothing for the policy; its system-sleep part (dark before the freeze) is `power.md` §4's |

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
| `piano-policy-lpm=1` | 0 | Low Power Mode on from boot (`machdep.piano_policy_lpm` starts at 1; writing 0 turns it off): a test's switch, for an unattended run with the Mac's cable (external power) |
| `piano-policy-display-tick=1` | 0 | the 5 s display tick of before 2026-11-12 while a keyboard wake could come, even with NativeGopDisplay's messages (the A/B switch) |

Read, not owned: `piano-keyboard-lid`, `piano-gpu-dcvs-max`, `piano-gpu-maxlevel`, `piano-thermal`, `piano-thermal-gpu-mhz`; PianoA830's
property `piano-gpu-ceiling-clients`. sysctl:
`machdep.piano_policy` (state, decisions, counters), `machdep.piano_policy_lpm` (root, 0/1).

## 7. Idle sleep on battery: macOS's timers (pmset)

The 5-minute idle sleep on battery (Ubuntu since v0.8.0) is macOS's own timer; PianoPolicy does not touch it:
```sh
sudo pmset -b sleep 5 displaysleep 2 hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0
sudo pmset -c sleep 0 displaysleep 10
```
(no hibernation natively; plugged in never sleeps, as stock and Ubuntu). powerd picks the Battery or AC set from PianoPower's
`ExternalConnected` (`piano-power-ps=1`). The lock: System Settings › Lock Screen › require the password immediately after the display
turns off. Installed as the one-shot LaunchDaemon `piano-pmset-defaults` (`docs/userland.md` §6): each power source's timers
set once, unless the owner set that source's timers first. The sleep timer stays inert until the profile has `piano-sleep=1`
(the ADT keeps sleep-disabled), which waits for `power.md` §6 S1-S4. Only display sleep acts before that, and its native wake is
`userland.md`'s test U10.

## 8. Lines

```
PianoPolicy: start: piano-policy=2 (acting), cover acted on, profile on, GPU ceiling 342 MHz (a no-op: PianoA830's own maximum is 342 MHz), power key on, keyboard wake on (hold-off 1000 ms)
PianoPolicy: system sleep: on, wake mask 0x13 (power key, cover open, keyboard); machdep.piano_sleep: on; state 1, poll 100 ms, wake mask 0x13, ...
PianoPolicy: system sleep: on, wake mask 0x3 (power key, cover open) from the boot arguments (machdep.piano_sleep: error 1, EPERM: its OID lacks CTLFLAG_KERN, k27 0062-0064 before 2026-11-12)   (an older kernel)
PianoPolicy: system sleep: machdep.piano_sleep absent (error 2, ENOENT: a kernel before k27 0062): no sleep, the cover only locks   (0059/0060 kernels)
PianoPolicy: watching PianoKeyboard (the cover)            PianoPolicy: watching AppleSmartBattery (the power source)
PianoPolicy: watching NativeGopDisplay (the display power: a message per change, no tick)
PianoPolicy: watching NativeGopDisplay (the display power; it publishes no piano-display-power-messages: until one of its messages is seen it is read every 5 s while a keyboard wake could come)   (an older KC)
PianoPolicy: keyboard wake: NativeGopDisplay sends no display-power messages (no piano-display-power-messages, none seen): its piano-display-power is read every 5000 ms while the cover is open and attached (the fallback of an older KC)   (once)
PianoPolicy: inputs: cover open (attached), battery, display on, Low Power Mode off, thermal unknown (change 3)
PianoPolicy: root domain: kIOPMSetACAdaptorConnected (none) (on battery)
PianoPolicy: root domain: kIOPMEnableClamshell (on battery: sleep)
PianoPolicy: root domain: kIOPMClamshellOpened (cover open)
PianoPolicy: decision: closing the cover: sleep (on battery: sleep); profile Power Saver (on battery); GPU ceiling 0 MHz; keyboard wake off, latch on
PianoPolicy: root domain says: clamshell closed, closing sleeps
PianoPolicy: root domain: user active requested (cover open): the display wakes
PianoPolicy: keyboard: gpio95 rising (4): display off, cover open: wake the display
PianoPolicy: power key: short press (180 ms), display on: the display is on: sleep now
PianoPolicy: PianoA830: 4 GPU ceiling slots (piano-gpu-ceiling-clients): Power Saver's ceiling is client 1, the lowest slot wins, so PianoThermal's passive ceiling stays in force by itself: no breadcrumb read, no GPU tick, no put-back
PianoPolicy: PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling; client 1, the power profile's slot)
PianoA830: gpu ceiling 342 MHz (power profile's; slots thermal 0, power profile 342, 0, 0 MHz; the lowest wins; #1)   (PianoA830's side)
PianoPolicy: PianoThermal: profile 1 (Low Power Mode: Power Saver whatever the power source)
PianoThermal: [Ns] cpufreq: profile Power Saver (PianoPolicy)          (PianoThermal's side, §5.2)
PianoInput: power key: short press 180 ms: PianoPolicy took it          (PianoInput's side, §5.1)
```
Level 1: `root domain: would send ...`, `PianoA830: would send GPU ceiling ...`. `sysctl machdep.piano_policy`: `level 2 (acting);
cover open, attached; battery; display on; Low Power Mode 0; sleep on, wake mask 0x13; thermal unknown; close: sleep (on battery:
sleep); profile Power Saver (on battery); GPU ceiling 0 MHz (written 0, client 1); keyboard wake 0, latch 1; sent: root 3, ...`
(`shared slot` or `PianoThermal holds the slot` with a PianoA830 without slots; the inputs line then names the thermal state, else
`thermal not read (own GPU slot)`). Without slots: `PianoA830 publishes no piano-gpu-ceiling-clients (one shared slot): ...` and
`PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling; the shared slot)`.

## 9. Test plan (RAM boots, owner present, cable for the console)

**Build** (main checkout; the k27 tree at 0062 or 0063, which contains it):
```sh
KB=$(tooling/k27/build-native-piano-keyboard.sh | tail -1)
K27_KERNEL=$K/out/k27/kernel-k27-0063-native.macho PIANO_EXTRA_KEXTS=$KB PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh
tooling/k27/build-policy-test-images.sh        # out/ram-pol-{observe,lock,sleep,lpm,suspend,gpuslot} and out/ram-pol-dump
tooling/k27/build-policy-test-images.sh chargesleep                  # P6 by name (PianoPower level 2)
```
(Built offline here 2026-11-11 from an isolated checkout: on main `9c6cf7c` `build-native-image.sh` with the default 0059 kernel
links 21 native kexts (PianoPolicy 322 pointers); on `f9f2352` the test KC (0062 kernel + PianoKeyboard) `49f1e96da5536c8f…`; images `ram-pol-observe`
`572e85c75e2a6358…`, `-lock` `5a43e7a3d21700d6…`, `-sleep` `6d04aea4678d0318…`, `-lpm` `599150d2fd92bbdd…`; command line 898 bytes.
Rebuild in the main checkout before a run.) Rebuilt 2026-11-11 with the drivers' parts of §5 and `power.md` §4 (an agent worktree
on main `9867677` + those commits): the test KC (kernel 0063 `ee596f39be1ded3c…` + PianoKeyboard) `c3f214c7472605cf…`, `native
kext rule: 22 native kexts linked, none from outside this repository`, every vtable check 0 problem slots (PianoInput 692,
PianoDwc3 1195, PianoKeyboard 733, PianoBacklight 336, PianoThermal 435, PianoPower 782, NativeGopDisplay 851, PianoPolicy 322,
PianoTouch 713 pointers); `check-native-kexts.py` self-test, descriptors and KC log pass; images (all `verify-ram-images.py` PASS)
`ram-pol-observe` `903b4184c226ae2a…`, `-lock` `9caae3115e28944c…`, `-sleep` `9ddf36a5b77a5e48…`, `-lpm` `c3e366f03c0bc76a…`,
`-suspend` `9fa2dcdba6270134…`, `-chargesleep` `55141369fa2306ad…`, `-dump` `973731b2571fcfa3…`; the longest command line
(`-suspend`) 965 bytes. The script now gives
PianoPower's and PianoThermal's levels per image: the kernel takes an argument's first occurrence, so P4's `piano-thermal=2` after
BASE's `piano-thermal=1` had run PianoThermal at level 1 (its GPU ceiling share untested). Every image: PianoKeyboard 2,
PianoPower with its battery (`piano-power-ps=1`), PianoBacklight 2 (the display power), PianoThermal 1 (P4: 2), the
USB console. The CPU clock (2026-11-12): the fixed Power Saver level (`piano-cpufreq=2`, 1785 / 1958 MHz) in every image but P4 and
P7, which test the profile's hand-off to the governor and so keep `piano-cpufreq=3`, under a hard cap of 2400 / 2438 MHz
(`piano-cpufreq-cap0/1`); before, every image ran the uncapped governor, which on the Mac's cable held cluster 0 at 3532 MHz and
102-105 C for the whole of pol-observe and pol-suspend (`power.md` §1.2, `drivers/thermal.md` §8). `POL_CPUFREQ` overrides the
choice for every image. Run each with
`tooling/native/native-loop.sh out/ram-pol-<name>/boot-piano-macos-ram-A-bundled.img` and `usb-console.py --capture`.
2026-11-12 (client 1, P7): the KC with the 0064 kernel (any kernel from 0062 does for these images) and PianoKeyboard, 24 native
kexts, `ram-pol-gpuslot` `593e2fd733b9335a…` (`verify-ram-images.py` PASS, command line 819 bytes); the default 0059 build links
23 kexts with this PianoPolicy (`check-kext-imports`: 330 kernel imports and `_piano_thermal_state`, none unresolved). P7 is in
`power.md` §6's next unattended queue.
Prerequisites: `keyboard.md` §8 (a)/(b) and the hall polarity (§10.3) before any `piano-policy-lid=1` image; `power.md` §6 S1-S4
before `sleep`.

| # | Image | Risk | Run | Pass | On failure |
|---|---|---|---|---|---|
| P1 | `ram-pol-observe` (900 s) | none: nothing sent | close and open the cover; plug and unplug a charger; root shell: `sysctl -w machdep.piano_policy_lpm=1`, `=0`, `sysctl machdep.piano_policy` | `start: piano-policy=1 (decisions only: nothing sent)`, `watching PianoKeyboard`, `watching AppleSmartBattery`, `inputs:` per change, `decision: closing the cover: lock (no system sleep ...)`, `root domain: would send kIOPMClamshellClosed`, the LPM write → `profile Power Saver (Low Power Mode)` | no `watching PianoKeyboard`: PianoKeyboard not at level 2; power `unknown`: PianoPower published no battery yet; display `unknown`: piano-backlight not 2 |
| P2 | `ram-pol-lock` (1200 s) | low: the root domain gets the clamshell; no sleep exists | close on battery and on power, open each time | `root domain: kIOPMDisableClamshell (no system sleep ...)`, `kIOPMClamshellClosed`, `root domain says: clamshell closed, closing does not sleep`, PianoBacklight's lid line and backlight off; open: `kIOPMClamshellOpened`, `user active requested (cover open)`; with the agent: the display sleeps and opening shows the lock screen | panel lit under the cover without the agent: expected (§2.3); the display does not come back at opening: `requestUserActive` does not wake it (report; the HID path still does) |
| P3 | `ram-pol-sleep` (1800 s) | medium: the cover sleeps the system | on battery: close, wait 30 s, open; plug in, close (stays awake), unplug while closed (sleeps), open; the Apple menu's Sleep, then the power key | `decision: closing the cover: sleep (on battery: sleep)`, `kIOPMEnableClamshell`, `kIOPMClamshellClosed`, `IOCPUSleepKernel enter (piano: suspend-to-idle)`, `woke after N s by lid`, `kIOPMClamshellOpened`; plug-in: `kIOPMDisableClamshell (on external power: lock only)` before `kIOPMSetACAdaptorConnected (connected)`; unplug while closed: `(none)`, `kIOPMEnableClamshell`, a suspend | closing on battery does not sleep: `root domain says: ... does not sleep` → powerd's own bit (`pmset -g assertions`: a lid-close assertion); a sleep right after a charger wake with the cover closed: §10 risk 3 |
| P4 | `ram-pol-lpm` (900 s) | low | on power: `sysctl -w machdep.piano_policy_lpm=1`, 60 s, `=0`; unplug | `=1`: `PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling; client 1, the power profile's slot)` and PianoA830's `gpu ceiling 342 MHz (power profile's; slots thermal 0, power profile 342, 0, 0 MHz; the lowest wins; #n)`, `PianoThermal: profile 1 (Low Power Mode: ...)` with PianoThermal's `cpufreq: profile Power Saver (PianoPolicy)` and its next 30 s lines `ceiling 1785 MHz (on power; profile Power Saver)` (cluster 1: 1958); `=0`: `GPU ceiling 0 MHz (Balanced ...)`, `gpu ceiling cleared`, `PianoThermal: profile 0 (...)`, `cpufreq: profile by the power source (PianoPolicy)`, `ceiling 2400 MHz (on power)` (Balanced's 3532 under the image's 2400 MHz cap); unplugged: 342 MHz again, `ceiling 1785 MHz (on battery)` | `not taken (no PianoA830 service ...)`: PianoA830 inert (piano-gpu=2); `PianoThermal does not take "piano-thermal-profile"`: `piano-cpufreq` < 2 or the KC is older than this change; `(... the shared slot)`: the KC's PianoA830 has no slots |
| P5 | `ram-pol-suspend` (900 s) | medium: every driver of `power.md` §4 in the power tree takes part (power.md §6 S2 first) | unattended: three test sleeps of 30 s 150 s after boot completed (Mac cable in, console open); then by hand: the power key (sleep), the power key (wake), the cover closed on battery and opened | per suspend, before `IOCPUSleepKernel enter`: `PianoInput: sleep N: the poll thread is parked ...`, `PianoDwc3: sleep N: the Mac has the device configured: kept ...`, `PianoKeyboard: blank N (system sleep) ...` (or `(display off)` when NativeGopDisplay's off came first) and `sleep N: scanning stopped, MCU blanked; gpio95 latch ARMED (PianoPolicy: ...)` (cover open, attached), `PianoBacklight: system sleep: the backlight off on both ICs ...`, `PianoThermal: [Ns] sleep: the thread is parked (no SCMI, no TSENS); threshold interrupts as they are ...`, then `piano-sleep: suspend N: ... wake on power-key lid keyboard timer ...`; `woke after 30.0 s by timer` (or `by power-key`, `by keyboard`, `by lid`); after `IOCPUSleepKernel exit`: `PianoInput: wake N: polling again ...`, `PianoDwc3: wake N: the glue clocked; kept: the console goes on`, `PianoKeyboard: wake N: serving again; gpio95 did not rise while suspended ...`, `PianoThermal: [Ns] wake N: polling again`, `PianoBacklight: system wake: ...`, `PianoKeyboard: unblank N (display on) ...` once the display is on; the console continuous across each suspend; the power key's sleep: `PianoInput: power key: short press N ms: PianoPolicy took it` and `PianoPolicy: power key: short press ... sleep now` | a driver's `did not park/answer within ...` line: its thread was busy (report the line before); no `unblank` after a wake with the display on: `NativeGopDisplay` did not message (its `piano-display-power` in `ioreg`); the keyboard dead after a wake: rerun without `piano-keyboard-blank=1` (keyboard.md §10.11) |
| P6 | `ram-pol-chargesleep` (900 s, by name only) | medium-high: PianoPower level 2 (the DSP), then two test sleeps of 30 s 240 s after boot | unattended on the Mac's cable | P5's lines plus `PianoPower: policy: system sleep: handing the charger over ...`, `policy: sleep handover done: acknowledged`, `policy: reports paused until the wake ...`, then `policy: awake again (wake N)`, `policy: reports again ...`, `policy: wake N: the codec is relinked and everything is sent again` | `handover not complete after 2 s`: the firmware's answers did not come (report the `mca:` lines) |
| P7 | `ram-pol-gpuslot` (600 s) | low: nothing but the profile and the GPU ceiling | unattended on the Mac's cable (external power); `piano-policy-lpm=1`, PianoThermal level 2 with the governor, `piano-gpu-dcvs-max=607`; at the end of the window the console's `sysctl machdep.piano_policy` if a shell is there | `piano-policy-lpm=1: Low Power Mode on from boot ...`; `PianoA830: 4 GPU ceiling slots (piano-gpu-ceiling-clients): Power Saver's ceiling is client 1 ...`; `PianoA830: GPU ceiling 342 MHz (Power Saver's GPU ceiling; client 1, the power profile's slot)`; PianoA830's `gpu ceiling 342 MHz (power profile's; slots thermal 0, power profile 342, 0, 0 MHz; the lowest wins; #1)`; `PianoThermal: profile 1 (Low Power Mode: ...)` and PianoThermal's `cpufreq: profile Power Saver (PianoPolicy)`, its 30 s lines `ceiling 1785 MHz (on power; profile Power Saver)` (cluster 1: 1958); the `inputs:` lines say `thermal not read (own GPU slot)` | `(... the shared slot)` or `PianoA830 publishes no piano-gpu-ceiling-clients`: the KC's PianoA830 predates the slots; `ceiling 2400 MHz (on power)` (the image's cap; 3532 in an image built before 2026-11-12) with no profile line: PianoThermal did not take the profile (its `piano-cpufreq`) |

## 10. Risks

1. The hall polarity is unverified: the cover part is opt-in and a boot with the cover closed never sleeps until it was open.
2. WindowServer may keep the "external" panel lit under a closed cover: the agent's display sleep and PianoBacklight's lid rule.
3. A wake with the cover closed on battery may go straight back to sleep (the root domain's full-wake check; a Mac does the same);
   a charger wake (bit 4) with the cover closed races PianoPower's re-read and our kIOPMDisableClamshell against the root domain's
   delayed clamshell evaluation, and may sleep again while charging (harmless for the battery; B4's charge-while-awake is later).
4. powerd's own clamshell rule (lid-close assertions, desktop mode) can keep a closed cover awake: macOS's behaviour, by design.
   PianoPower's B4 charging hold (`ChargePolicy.h`: no idle sleep while protected charging runs below the limit) only concerns
   idle sleep on power, where the cover never sleeps the system anyway.
5. Low Power Mode is likely not offered on VirtualMac2,1: the sysctl is the switch.
6. The GPU ceiling: with PianoA830's slots (since 2026-11-11) PianoPolicy is client 1 and the lowest slot wins, so nothing can lift
   PianoThermal's passive ceiling and there is no race; only a PianoA830 without `piano-gpu-ceiling-clients` falls back to the shared
   slot and its interim arbitration (a tiny race window, repaired). PianoThermal's profile call is in (§5.2) and checked end to end
   on the host; on the tablet P4/P7 are its proof. With PianoA830's default maximum (342 MHz) PianoPolicy sends no GPU ceiling at all.
7. `requestUserActive` is the kernel's only display-wake request: needs-boot that powerd honours it natively.
8. `kIOPMSetACAdaptorConnected` is a private root-domain message (IOPMPrivate.h); it also reaches DriverKit's power-source hook.
9. PianoPolicy links PianoThermal's breadcrumb: PianoThermal must stay in the KC (it is listed first).

## 11. Files

`package/PianoPolicy/` (`PianoPolicy.cpp`, `Policy.h`, `tests/policy_host.cpp`, `README.md`, `UPSTREAM`, `LICENSE`),
`tooling/k27/build-native-piano-policy.sh`, `tooling/k27/kext/PianoPolicy.json`, `configs/k27-native-kexts.txt`,
`tooling/k27/build-policy-test-images.sh`, `tooling/native/piano-policy-agent/piano-policy-agent.sh` and its
`com.unixnu.piano-policy-agent.plist` (installed by `configs/userland.toml`).
