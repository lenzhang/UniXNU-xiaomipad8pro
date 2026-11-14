# USB device-mode debug channel: PianoDwc3

> **Status 2026-11-11 06:25: working.** With the Mac allowing new accessories (the owner switched "Allow accessories to connect" to
> automatic), the Mac configures the device (AppleUSBCDCCompositeDevice, configuration 1) and creates /dev/cu.usbmodemPIANODBG1
> (kernel console + monitor) and /dev/cu.usbmodemPIANODBG3 (user pipe); `tooling/native/usb-console.py --capture N` saved 1.5 MB of
> live console in 3 minutes and the monitor answered `usb`. Earlier the Mac held the device at UsbEnumerationState 2 (accessory
> approval). Build: `PIANO_EXTRA_ARGS="piano-usb=2" tooling/k27/build-native-image.sh`. The replayed boot log at connect time
> includes the previous boot's decayed mailbox content (garbled tail): to be trimmed.
>
> **2026-11-11, later: a network link over the same cable**, opt-in with `piano-usb-net=1`: a CDC-NCM function after the serial
> ports, which the Mac's own NCM driver shows as an Ethernet interface, and on the tablet `PianoUsbNet`, an IOEthernetController
> (not built-in; en0 stays PianoNullEthernet's). Built, host-tested, linked; **not run on hardware yet**. §14.
>
> **2026-11-11, battery: the USB thread no longer wakes every 1 ms.** Its wait follows the link (1 ms only while something
> moves, 20 ms with a port open, 50 ms configured, 250 ms with no host); `piano-usb-irq=1` adds the core's interrupt (SPI 133).
> Default on for the adaptive wait, the interrupt opt-in; `piano-usb-idle=0` is the old loop. Not run yet. §15 and
> [`../power-irq.md`](../power-irq.md).
>
> **2026-11-11, system sleep** (host-tested, linked, not run): in the power tree at `piano-usb=2`; a configured Mac keeps the
> console across a suspend (`piano-sleep-usb=1`, default), else a soft disconnect; no QSCRATCH write while asleep or with the glue
> unclocked (Ubuntu's SoC reset, K:7dffd78b342d). §16.
>
> **2026-11-12, the normal boot path: a cold start** (host-tested, linked, **not run**). The installed ESP ran for hours with the
> console at `no-host ... link state 3, configuration 0`: on the normal power-on path (no ABL fastboot) the takeover had trusted a
> state only ABL fastboot leaves. `piano-usb-cold=auto` (default) now reads the inherited state first and, when it is not a running
> device controller, brings the GDSC/clocks, the rails (RPMh), the eUSB2 repeater and the M31 PHY up from this driver's own tables;
> a link watch does the same when no host comes within 15 s. `piano-usb-cold-sim=MASK` proves it from ABL fastboot. §17;
> tests C0-C8 in [`../forensics-2026-11-11.md`](../forensics-2026-11-11.md) §7.


Status: design and code, 2026-11-11. Built, host-tested and linked into a test KC (`out/k27/kc-k27-usb-test.bin`); **not
run on hardware**. Template: `README.md`. Code: `package/PianoDwc3`.

## 1. Purpose and gate

Today the only view into a native k27 boot is the pstore-console RAM mailbox, read back from Ubuntu after a reset
(`docs/bringup-debugging.md` §2). PianoDwc3 makes the tablet's single USB-C port, already cabled to the Mac, a live
channel while macOS runs: the DWC3 controller that the UEFI's debug fastboot used is taken over **in device mode**, and the
Mac sees a composite USB device with two CDC-ACM serial ports (no driver to install on the Mac):

| Port | Mac node | Tablet side |
|---|---|---|
| 0 "piano kernel console" | `/dev/cu.usbmodem…` | the console log streamed live; on open, the whole retained log (the pstore ring, ~650 KB) is replayed first; a line-based kernel monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `uptime`, `reset`, `reset fastboot` (PMIC restart reason bootloader: ABL fastboot after this reset, also on an installed system; both first mark the restart begun, `machdep.piano_restart_started`, so the boot guard leaves the reason alone, and with k27 0069 the reset is immediate, `docs/halt.md`), `panic`, `log`, `live`) |
| 1 "piano user pipe" | `/dev/cu.usbmodem…` | `/dev/pianousb` (`piano-usb-shell=1`): a byte pipe to user space for a root shell (`tooling/native/pianousb-shell`) or `log stream > /dev/pianousb` |
| 2 "piano input" (only with `piano-usb-hid=1`) | `/dev/cu.usbmodemPIANODBG5` (expected) | keyboard and mouse input from the Mac (`tooling/native/piano-input.py`) into two HID devices, §12 |
| "piano network" (only with `piano-usb-net=1`; not a serial port) | an Ethernet interface `en…`, MAC 02:50:49:41:4e:03 | `PianoUsbNet`, an Ethernet interface `en…` (configd names it), MAC 02:50:49:41:4e:02: IP over the cable, §14 |

It gates nothing in M1–M5; it shortens every later loop (UFS, display, touch, GPU) from "reset, Ubuntu, read the mailbox"
to watching the log and asking the kernel questions while it runs.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Controller | `usb@a600000`, `qcom,sm8750-dwc3` + `qcom,snps-dwc3`, reg `0xa600000` + `0xfc100` (flattened qcom binding: QSCRATCH inside), `dr_mode = "otg"`, `maximum-speed = "super-speed"`, `usb-role-switch`, `dma-coherent`, quirks `snps,dis_u2_susphy_quirk`, `snps,dis_enblslpm_quirk`, `snps,usb2-gadget-lpm-disable`, `snps,dis-u1/u2-entry-quirk` | piano DT (`out/inputs/piano-boot.dtb`) | verified |
| Interrupts | `dwc_usb3` GIC SPI 133 (INTID 165) level-high; `pwr_event` SPI 130, `hs_phy_irq` SPI 131; dp/dm/ss on the PDC (14/15/17 → SPI 759/760/762) | DT | verified; SPI 133 used with `piano-usb-irq=1` (§15), else polled (`GEVNTSIZ.INTMASK` set) |
| SMMU | `iommus = <&apps_smmu 0x40 0x0>`; apps SMMU `0x15000000` + 1 MiB (`qcom,sm8750-smmu-500`); piano's SMMU: IDR1 `0x60000053` (83 banks, 128 pages), NativeUfs puts SID 0x60 on bank 82 | DT; `logs/native-runs/20261111-004859-loop` | verified |
| Power domains | `USB30_PRIM_GDSC` GCC `0x39004` (DT power-domains gcc 4), `USB3_PHY_GDSC` `0x50018` | gcc-sm8750.c; OnePlus 13 DT | verified (source); state after ExitBootServices: needs-boot |
| Clocks (GCC base `0x100000`) | master `0x39018` (RCG `0x39030`, 200 MHz assigned), sleep `0x39028`, mock_utmi `0x3902c` (RCG `0x39048`, 19.2 MHz), cfg_noc_usb3_axi `0x3908c` and aggre_usb3_axi `0x39090` (voted), phy aux/com_aux/pipe `0x39064/68/6c` | gcc-sm8750.c (offsets), DT `assigned-clock-rates` | verified (source) |
| Resets | `USB30_PRIM_BCR 0x39000`, `QUSB2PHY_PRIM_BCR 0x12000` (the M31 PHY's reset), `USB3_PHY_PRIM_BCR 0x50000`, `USB3PHY_PHY_PRIM_BCR 0x50004`, `USB3_DP_PHY_PRIM_BCR 0x50008` | gcc-sm8750.c | verified (source) |
| HS PHY | `phy@88e3000`, `qcom,sm8750-m31-eusb2-phy`, reg 0x29c, ref clock TCSR `USB2_CLKREF_EN` (`0xf204008 + 0x2000`), vdd `vreg_l2d_0p88`, vdda12 `vreg_l3g_1p2`, `phys = <repeater>`; byte-wide registers; Project-SunUEFI read `COMMON0 = 0x6b` (enabled, SIDDQ 0, 38.4 MHz) | DT, tcsrcc-sm8750.c, SunUEFI | verified / measured by SunUEFI |
| Repeater | `pmic@7` (pmih0108, SPMI SID 7) `phy@fd00`, `qcom,pm8550b-eusb2-repeater`, vdd18 `vreg_l15b_1p8`, vdd3 `vreg_l5b_3p1`, tune amplitude 0x0e, res-fsdif 1, disc-thres 7, preem 6, squelch −3000; SunUEFI read status `fd08 = 0x80`, enable `fd46 = 0x80` | DT, SunUEFI | verified |
| SS PHY | `phy@88e8000`, `qcom,sm8750-qmp-usb3-dp-phy`, power-domain `USB3_PHY_GDSC`; orientation/mode switch | DT | verified; **not used** (high speed only, UTMI clock as PIPE clock) |
| QSCRATCH | `0xa6f8800`: GENERAL_CFG +8 (PIPE_UTMI_CLK_SEL b0, PIPE3_PHYSTATUS_SW b3, PIPE_UTMI_CLK_DIS b8), HS_PHY_CTRL +0x10 (UTMI_OTG_VBUS_VALID b20, SW_SESSVLD_SEL b28), SS_PHY_CTRL +0x30 (LANE0_PWR_PRESENT b24) | dwc3-qcom.c; SunUEFI (from the stock UsbfnDwc3Dxe disassembly: RVA e988/e9c0) | verified |
| SPMI | arbiter v7, core `0xc400000`, chnls `0xc500000`, obsrvr `0xc440000`, cnfg `0xc42d000`, EE 0 | DT | verified |
| Role / Type-C | UCSI over `pmic-glink` (ADSP); an FSA4480 audio switch (`fsa4480` label) on the USB 2 lines; in the Android DT `usb0.extcon` → EUD `0x88e0000` | DT; SunUEFI `usb-link-audit.md` (Android DT) | verified (DT); no UCSI client natively: the device role comes from the VBUS/session override |
| UEFI's USB | piano-uefi (FD A) runs Qualcomm's binary `UsbConfigDxe` + `UsbfnDwc3Dxe` + `UsbDeviceDxe`; `PianoFastboot.c` (18d1:d00d, "piano-uefi") only cancels its poll timer at ExitBootServices; `UsbConfigDxe` registers its own ExitBootServices handler (strings `DisableUsbClocks`, `EnterLPM`, `UsbStopController`); the device leaves the Mac's bus at the hand-over (`bringup-debugging.md` §2) | piano-uefi sources and binaries | what the handler leaves on: **needs-boot** (probe step T1) |
| DWC3 IP | GSNPSID / GVERSION, IN endpoints (GHWPARAMS3) | — | needs-boot (accepted: DWC_usb3 0x5533…, DWC_usb31 0x3331…, DWC_usb32 0x3332…) |

## 3. Linux and Project-SunUEFI as the specification

Read, not copied (`package/PianoDwc3/UPSTREAM`): mainline `drivers/usb/dwc3/{core.h,core.c,gadget.c,ep0.c,dwc3-qcom.c}` for
the programming model (event buffer, DEPCMD, TRBs, control phases, conndone/reset handling, VBUS override, UTMI clock
select); `gcc-sm8750.c` / `tcsrcc-sm8750.c` for offsets. **Project-SunUEFI** (BSD-2-Clause-Patent, `unixnu-research/Project-SunUEFI`
4ef8740) brought up exactly this controller on piano from its own UEFI and enumerated at SuperSpeed with a bulk fastboot
(tests 58–82, `docs/archive/storage-usb/usb-link-audit.md`). Its findings are the piano-specific part of PianoDwc3:
- no events at all until the QSCRATCH session/VBUS override is set before RUN_STOP (test 65);
- `DCFG.DevAddr` must be written in the SETUP stage of SET_ADDRESS, before the status stage (tests 65–66);
- after the stock firmware's USB shutdown, `GUSB2PHYCFG.ENBLSLPM`/`SUSPHY` may be left set: clear both (and the USB3 PIPE
  suspend) before `DCTL.CSFTRST`, which needs the PHY clocks; DWC_usb31/32 reset in 20 ms steps up to 200 ms, then 50 ms;
- END TRANSFER with ForceRM, then 1 ms before reusing the TRB; DEPSTARTCFG(0) once and one transfer resource per endpoint;
- the M31 PHY init writes (used only with `piano-usb-phy=1`).

## 4. Design

### 4.1 Apple's device stack in the macOS 27.0.1 KC (vma2 26A434)

From the KC's `__PRELINK_INFO` (`$PIANO_K27_TREE/out/kc/kc27.bin`, the base `tooling/kc/build-kc-k27.sh` links):

| Piece | In the KC | Note |
|---|---|---|
| `AppleUSBXDCI` (the DWC3 device controller under IOUSBDeviceFamily) | **no** | iPadOS only; QcomDwc3 derives from it there |
| `com.apple.iokit.IOUSBDeviceFamily` 2.0.0 | yes | IOUSBDeviceController / Interface / Configurator; in-kernel configurations by `ConfigurationType` (`ncmBringup` = AppleUSBNCMControl + AppleUSBNCMData, `standardBringup` = AppleUSBMux, …) |
| `com.apple.driver.AppleUSBDeviceNCM` 5.0.0 | yes | `AppleUSBDeviceNCM11Control/Data` on `USBDeviceFunction` AppleUSBNCMControl/Data |
| `com.apple.driver.AppleUSBDeviceMux`, `IOAccessoryPortUSB` | yes | usbmux, iAP over HID |
| `AppleVirtIOUSBDeviceController` (AppleVirtualPlatform) | yes | the only IOUSBDeviceController subclass in this KC (virtio transport) |
| `IOSerialFamily`, `IONetworkingFamily` | yes (already kept) | IOSerialFamily has no headers in the macOS 27 SDK |
| Mac host side | — | CDC-ACM → `/dev/cu.usbmodem*`; CDC-ECM/NCM drivers exist for a later network function |

### 4.2 Options, and the choice

1. **Apple's NCM function** (best end state: a network interface, Apple's own code): needs an IOUSBDeviceController
   subclass for DWC3, i.e. reconstructing IOUSBDeviceFamily's private controller ABI (vtable, request and endpoint
   objects) from `AppleVirtIOUSBDeviceController`, then `ConfigurationType = ncmBringup`. Large reverse-engineering job; later.
2. **QcomDwc3's XDCI subclass**: impossible here (no `AppleUSBXDCI` in this KC); transplanting the iPadOS kext into a macOS
   collection is not ABI-safe.
3. **Own minimal gadget + CDC-ACM (chosen)**: ~2000 lines with no private Apple ABI (an IOService and BSD char-device KPI
   only), no Mac-side driver, a log stream even when user space never comes up.
4. **Own gadget + CDC-ECM/NCM + IOEthernetController**: done as CDC-NCM with `piano-usb-net=1` (§14); the
   IOEthernetController side follows PianoNullEthernet (the VM-era Ethernet kext's pattern, vtable-checked in this KC). The ACM console stays
   the fallback.
5. A vendor-class bulk pipe: no advantage over ACM, needs a Mac tool.

### 4.3 What runs

```
IOPlatformExpertDevice ── PianoDwc3 (start, matching thread)   read-only probe -> [SMMU SID 0x40 -> bypass bank]
        threads:  usb (takeover, then a 1 ms poll loop: events -> EP0/bulk, log tail -> port 0 IN, port OUT -> monitor/pipe)
                  monitor (port 0 command lines)   notes (IOLog off the USB thread)   shell (/dev/pianousb after IOBSD)
```

- **Takeover** (`takeover()`, stages 10–20 in the log; since 2026-11-12 the cold start of §17 first, stage 11, when the inherited
  state is not a running device controller): clocks checked (or switched on, `piano-usb=3`) → GSNPSID → halt
  (RUN_STOP off) → 64 KiB contiguous DMA region → [M31 PHY init] → UTMI clock as PIPE clock → PHY suspend bits off +
  `DCTL.CSFTRST` → device role, high speed, event buffer (masked line), EP0 → QSCRATCH VBUS/session override → SETUP armed,
  RUN_STOP. The QMP PHY and the SuperSpeed lanes, UCSI, the FSA4480 and the charger are not touched.
- **Gadget core** (`Dwc3Gadget.h`, portable): one event buffer; EP0 SETUP / DATA / STATUS with the SunUEFI ordering
  (DevAddr in SETUP, configuration before the status stage, bulk transfers after it); chapter 9 (descriptors incl. device
  qualifier and other-speed, GET/SET configuration and interface, CLEAR/SET_FEATURE(ENDPOINT_HALT)); CDC-ACM line coding and
  `SET_CONTROL_LINE_STATE` (DTR = the Mac opened the port); bulk OUT (4 KiB, flow-controlled) and IN (16 KiB, a chained
  zero-length packet on packet boundaries).
- **Coherency**: SID 0x40 goes through a bypass bank (non-coherent), so the CPU only uses a write-combined (Normal
  non-cacheable) mapping of the DMA region, after cleaning the cacheable alias — the NativeUfs pattern.
- **SMMU** (`SmmuStream.h`): an exact firmware entry for 0x40 is kept if it already bypasses with VA64, else redirected; no
  entry: a free stream-match group. The bank is the highest one below the top (NativeUfs's first choice) that no valid stream
  uses and that has translation off. Written from `start()` on the matching thread, right after NativeUfs's own SMMU step
  (ShengUfs: the stream-match registers were not safe to touch later from another thread).
- **No ADT node**: the driver matches `IOPlatformExpertDevice` with its addresses in `PianoUsbHw.h` (as NativeUfs). An
  `/arm-io` child at `0xa600000` would move the generated arm-io window's base from `0x16000000` to `0xa000000` (UniXNU's
  generator takes the lowest exported register), which XNU reads as the SoC base; not worth that for a polled debug driver.
- **Log tail**: the k27 0055/0058 pstore ring (`DBGR`/`DBGB` header), copy 0, read live through a write-combined mapping;
  without `pstore-console` the kernel message buffer. `\n` becomes `\r\n`.
- **Timing**: polled every 1 ms; nothing on the USB thread waits for the console (notes go through a ring to their own
  thread: with `-v`, IOLog scrolls the 3200x2136 console).

## 5. IOKit family / ABI

`PianoDwc3 : IOService` on `IOPlatformExpertDevice` (IOMatchCategory PianoDwc3, probe score 100, after NativeUfs's), libraries
`com.apple.kpi.*` only, no vtable check needed (no family base class). BSD KPI for `/dev/pianousb`: `cdevsw_add`,
`devfs_make_node`, `uiomove`, `msleep`/`wakeup`, `selrecord`/`selwakeup`. No weak imports, no large `__bss` (rings are
`IOMallocData`). Kext JSON: `tooling/k27/kext/PianoDwc3.json` (bundle `com.unixnu.driver.PianoDwc3`).

## 6. Boot args and the Mac side

| Boot arg | Default | Effect |
|---|---|---|
| `piano-usb=N` | 0 (off: probe() declines, nothing mapped) | 1 read-only probe; 2 takeover + console; 3 = 2 + switch on the USB30 GDSC, branch clocks and the USB2 clkref when off |
| `piano-usb-phy=1` | 0 | rerun the M31 PHY reset + init (SunUEFI's sequence) before the core reset |
| `piano-usb-smmu=0` | 1 | leave the SMMU alone (if the probe shows 0x40 already bypassing, or the SMMU step hangs) |
| `piano-usb-utmi=0` | 1 | keep the PIPE clock source (only if the QMP PHY is known to run) |
| `piano-usb-delay=S` | 0 | wait S seconds before the takeover |
| `piano-usb-shell=1` | 0 | create `/dev/pianousb` for port 1 |
| `piano-usb-poke=1` | 0 | allow the monitor's `poke` |
| `piano-usb-replay=0` / `=2` | 1 | 1: port 0's first open of the boot replays the retained log, and a later open (a reconnect) resumes where the last session stopped (§18); 0: every session starts at "now"; 2: every open replays the retained log (the behaviour before 2026-11-12). The monitor's `log` replays it on request |
| `piano-usb-phyread=0`, `piano-usb-spmi=0` | 1 | skip the M31 / repeater reads of the probe |
| `piano-usb-pid=N` | 1 (`1209:0001`, pid.codes test PID) | another USB product ID |
| `piano-usb-trace=N` | 400 | log the first N controller events after each bus reset one by one (raw word, EP0 phase, TRB words); 0 off |
| `piano-usb-hid=1` | 0 | (needs `piano-usb=2`) a keyboard and a pointer HID device for macOS and the third port that drives them (§12); without it the USB device is the two-port one, unchanged |
| `piano-usb-hid-hold=MS` | 3000 | a key or button still down this long after the last input command is released |
| `piano-usb-net=1` | 0 | (needs `piano-usb=2`) the CDC-NCM network function after the ports and its Ethernet interface on the tablet (§14); without it the USB device is exactly the ACM-only one |
| `piano-usb-net-fifo=0` | 1 | with `piano-usb-net=1`: keep the TX FIFO sizes the core reset left instead of the plan of §14.2 (a first-run experiment) |
| `piano-usb-idle=0` | 1 | the old USB loop (1 ms after every quiet pass) instead of the adaptive wait of §15 |
| `piano-usb-irq=1` | 0 | the core's interrupt (SPI 133) ends the USB thread's waits (§15); falls back to polling by itself |
| `piano-usb-handoff=1` | 0 | PianoUsbHost (levels 3/4 with `piano-usbhost-handoff=5\|6`) may borrow the controller for host mode: on its ask the USB thread stops the device controller, masks its event interrupt and parks; on its reclaim the clocks are re-read and the whole takeover runs again (`UsbHandoff.h`, `usb-host.md` §6.6). The service is registered for it. 0: the controller is PianoDwc3's for the whole boot |
| `piano-wake-log=S` | 60 | the `idle: N wakeups in S s` line every S seconds (0 none; shared with PianoInput and PianoTouch) |
| `piano-sleep-usb=0` | 1 | a system sleep disconnects the gadget even with the Mac attached (§16); 1: a configured host keeps it (the console survives) |
| `piano-usb-cold=0\|1\|auto` | auto | the cold start (§17): `0` the inherited state only (the takeover of before, no cold start, no link watch; the inherited state is still read and reported), `1` always, `auto` when the inherited state is not a running device controller, plus the link watch |
| `piano-usb-cold-steps=MASK` | 0x17 | the cold start's optional parts: 0x1 the rails, 0x2 the repeater, 0x4 the M31 PHY, 0x8 the controller's block reset (opt-in), 0x10 the RCG rates, 0x20 the repeater's DT tuning even when it was on (opt-in) |
| `piano-usb-cold-wait=S` | 15 | the link watch: no bus reset from a host S s after RUN_STOP: a cold start (the wait doubles; 3 cold starts a boot at most); 0 no watch |
| `piano-usb-cold-sim=MASK` | 0 | test hook (needs `piano-usb=2`): before the decision, leave 0x1 the repeater off, 0x2 the M31 uninitialised, 0x4 the core in the host role, 0x8 the clocks and the GDSC off, 0x10 the rails' votes withdrawn (§17.4) |

On the Mac (no root, standard library only; logs go to `logs/usb-console/`, gitignored):

```sh
tooling/native/usb-console.py --list                 # is the gadget there, which /dev/cu.* is which port
tooling/native/usb-console.py                        # waits for the tablet, port 0 interactive; Ctrl-] quits
tooling/native/usb-console.py --capture 300 --send usb --send 'ioreg 6'   # unattended: 300 s saved, reconnects
tooling/native/usb-console.py --port 1               # the user shell (below)
screen /dev/cu.usbmodem<…> 115200                    # any terminal works too (DTR on open starts the stream)
```

User shell (development tablets only: an unauthenticated root shell): `tooling/native/pianousb-shell/pianousb-shell.c`, ad-hoc
signed, at `/usr/local/libexec/pianousb-shell` with `com.unixnu.pianousb-shell.plist` in `/Library/LaunchDaemons` (on Data);
`configs/userland.toml` builds and installs both (`docs/userland.md`). Boot with `piano-usb-shell=1`: without it the helper exits
at once with status 0 and launchd leaves it stopped. Without the helper, `/dev/pianousb` still carries anything a LaunchDaemon
writes to it (e.g. `log stream --style compact`).

**The deaf shell of desk-dev (2026-11-12 17:27), and its fix.** The job ran (launchd: running, never exited), `/dev/pianousb`
existed, but `usb-console.py --port 1 --send 'echo HELLO'` got no byte back, and the console said
`process pianousb-shell[450] ... caught burning CPU! It used more than 50% CPU over 180 seconds`. The cause is XNU's `poll()`, which
is kqueue underneath (`poll_nocancel` registers an `EVFILT_READ` knote per descriptor): for a character device `spec_kqfilter`
sends the knote to `filt_specattach`, which refuses it with `EINVAL` unless the cdevsw is marked kqueue-capable
(`cdevsw_setkqueueok`, `CDEVSW_SELECT_KQUEUE`) or the caller asks `NOTE_LOWAT` 1, which `poll()` never does. A refused knote is
`POLLNVAL` at once, every call. The helper polled and acted only on `POLLIN | POLLHUP | POLLERR`, so it spun and never read what the
Mac sent: the shell got no input, so it printed nothing (its banner and first prompt were written before the Mac opened port 1, and
writes then are dropped by design). Port 1's transmit path was never at fault: PianoDwc3 logged `port 1 opened (DTR/RTS 3)`, and
nothing reached `/dev/pianousb`'s write side to send.

- `pianousb-shell.c` waits with `select()`, which goes to the cdevsw's `d_select` (`selscan`, `fo_select`) for any device: the fix
  that works with every kernel collection already built. A bad descriptor from `select()` reopens the device.
- PianoDwc3 marks its cdevsw kqueue-capable after `cdevsw_add` (`cdevsw_setkqueueok`, the kernel's export), so `poll()` and kevent
  work on the node too. `puSelect` returns the byte count when ready, and `selrecord`s otherwise, as that contract asks. The line
  `/dev/pianousb created (major N): port 1 is the user pipe; select, poll and kqueue` says so.
- Host test: `python3 tooling/native/pianousb-shell/test_pianousb_shell.py` (macOS). The device is a pty, and an interposed `poll()`
  (DYLD_INSERT_LIBRARIES) answers `POLLNVAL` on it, as XNU did for `/dev/pianousb`. A probe checks that the shim does that. The
  helper must then relay a command and its output both ways, start a new shell after `exit`, and stay idle (under 0.5 s of CPU in
  1.5 s). The 17:27 helper fails it exactly as the tablet did: nothing came back.

Use: `tooling/native/usb-console.py --port 1`, interactive, or unattended
`tooling/native/usb-console.py --port 1 --send 'echo HELLO' --capture 10`. The first bytes to arrive are the echo of what you sent:
the banner and the first prompt went out before the port was open.

## 7. Build, kernel collection, RAM image

```sh
tooling/k27/build-piano-dwc3.sh            # host tests (gadget core, SMMU planner), then the kext: out/native-kexts/PianoDwc3
tooling/k27/build-usb-test-kc.sh           # configs/k27-native-kexts.txt + PianoDwc3 -> out/k27/kc-k27-usb-test.bin
# One test image per level (never out/ram):
PIANO_EXTRA_ARGS="piano-usb=1" PIANO_MENU_TIMEOUT=0 PIANO_RESET_AFTER=300 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-usb-test/probe --kc out/k27/kc-k27-usb-test.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
```

`build-piano-dwc3.sh` uses `tooling/k27/build-native-kext.py` (objects in `out/native-kexts/PianoDwc3`, the descriptor's
`@OUT@` = `out/`) and falls back to the k27 tree's builder where that script is not there yet. `build-usb-test-kc.sh`
rebuilds only PianoDwc3, links the other kexts' objects as last built, resolves `@OUT@` itself and refuses to write
`kc-k27-native.bin`. To carry the driver in the regular image instead (and add `piano-usb=N` to its arguments):

```sh
tooling/k27/build-piano-dwc3.sh && mkdir -p out/k27/kext && sed "s#@OUT@#$PWD/out#" tooling/k27/kext/PianoDwc3.json > out/k27/kext/PianoDwc3.json
PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/PianoDwc3.json tooling/k27/build-native-image.sh
```

Built for the first test, 2026-11-11 02:20 (kernel `kernel-k27-0058-native.macho`, the native set ARMPlatformStandIn,
NativeUfs, NativeRamBlock, NativeGopDisplay of 02:16 + PianoDwc3; FD A; `PIANO_MENU_TIMEOUT=0`, `PIANO_RESET_AFTER=300`; this
branch's builder, so **no `data-persist`**: the Data disk is the RAM seed only, nothing is written to UFS's store window):

| File (under `out/`) | SHA-256 | Arguments added |
|---|---|---|
| `k27/kc-k27-usb-test.bin` | d7838e0bf0160846e59f73ed1207c967534b64898357fe642a6471dc02b42260 | — |
| `ram-usb-test/probe/boot-piano-macos-ram-A-bundled.img` | f63b6fc1fab3d12d9ab1071b53ab6410c1c232607a2997e78c09f083a2745326 | `piano-usb=1` |
| `ram-usb-test/console/boot-piano-macos-ram-A-bundled.img` | 4493ca316571d377956fae93e3fa15d24c5b51653014ad61b7216ab892623727 | `piano-usb=2` |

Both pass `verify-ram-images.py` (unsigned, as every RAM image so far).

## 8. Hardware test plan

RAM boots only, cable to the Mac, owner present; `piano-reset-after` stays the backstop. Read the run's
`xnu-console.txt` (`grep PianoDwc3`) and, from T2 on, the Mac's `logs/usb-console/`.

| Step | Image | Look for | Pass / next |
|---|---|---|---|
| T1 probe | `piano-usb=1` | `GCC usb30 gdsc …` (bit 31 = on), `USB core powered and clocked`, `core GSNPSID 3331…/3332…`, DCTL RUN_STOP 0 / DSTS DEVCTRLHLT, QSCRATCH session bits, M31 `[54 common0]` (0x6b on, bit 2 = SIDDQ powered down), repeater `fd08/fd46 = 80`, `SMMU stream 0x40: … -> keep/bypass bank N`, `stage probe done` | everything read without a hang → T2. Clocks off → T2 with `piano-usb=3`; M31 powered down → add `piano-usb-phy=1`; SMMU line missing (hang) → `piano-usb-smmu=0` and report |
| T2 console | `piano-usb=2` | log stages 10–20, then on the Mac: `usb-console.py --list` shows two ports within seconds; port 0 prints the banner and the replayed boot log; `usb`, `ioreg 4`, `args`, `peek 0x139004` answer | enumeration + live log: the channel works. No `bus reset` note → DSTS link state in the log; try `piano-usb-phy=1`, then `piano-usb=3` |
| T3 soak | `piano-usb=2` | leave `--capture 1800` running across the boot to login; `usb` counters: no command errors/timeouts, `tx0 dropped` small | no stall of the boot, nothing lost besides overruns while the port was closed |
| T4 shell | `piano-usb=2 piano-usb-shell=1` + helper in the root image | `usb-console.py --port 1`: a zsh prompt as root | interactive shell |

Fail-safe: everything is a RAM boot; without `piano-usb` the driver declines in probe(); level 1 writes nothing (the SPMI
observer read issues a read command only); a hang or panic is a reset (watchdog, `piano-reset-after`) back to ABL/Ubuntu.

### 8.1 T2, first run (2026-11-11): enumeration stops after the configuration descriptor

The Mac addressed the device, read the device descriptor, strings 2/1/3 (each first with wLength 2) and the configuration
(9, then 141 bytes), then sent nothing for ~100 s: no SET_CONFIGURATION, no reset; the last note was `DSTS 00838740` (link
on, high speed), 54 events. Reading of it:
- `setup 80 06 0200 0000 9 -> in 141` and `... 2 -> in 36` printed the descriptor's length, not the transfer's: the data TRB
  was `min(wLength, length)` (9, 2 bytes). The note now says `in 9 of 141 bytes (1 packets of 64)`; the host test asserts the
  TRB lengths (`tests/usb_gadget_host.cpp`).
- The 141-byte read is the first multi-packet data stage (3 packets of 64); everything before it was one packet. In the
  model every ordering of its events (data done then status not-ready, or the reverse) ends with SETUP armed again, so the
  log alone cannot tell whether the status stage of that read completed, or the next request arrived with no SETUP armed,
  or the host stopped by itself.

The next run therefore traces (`piano-usb-trace`, default on): every controller event of the first 400 after a reset, as
`ev <raw> ep <n> <type> … | phase <EP0 phase> busy <out>/<in> | trb <size> <ctrl> (posted <bytes>)`; a snapshot of EP0 (phase,
both EP0 TRBs, DEPCMD0/1, DSTS, DCFG, DALEPENA, GEVNTCOUNT, the last 6 events) every 2 s while a host is attached and the
device is not configured; and two recoveries that also act as experiments, each logged with a snapshot:
- **EP0 watchdog**: a control transfer with no progress for 1 s is ended and SETUP armed again (`EP0 stuck 1 s`);
- **re-enumeration**: not configured and no request for 10 s → soft disconnect / connect (RUN_STOP off, endpoints set up
  again, the event buffer kept), at most 3 times, to see whether the Mac stops at the same request each time.
Also new: a failed START on EP0 stalls and re-arms instead of leaving EP0 waiting; a data-stage not-ready event in the other
direction stalls (as Linux). On the Mac, `usb-console.py --list` now also prints what macOS made of the gadget (configuration,
drivers, interfaces) when no serial port exists: run it during T2. What to read in the trace after `setup … 141`:
`ev … ep 1 xfer-complete … trb 00000000` (all 141 bytes went out), then `ep 0 not-ready status`, `ep 0 xfer-complete` in
phase status, then the next `ep 0 xfer-complete` in phase setup (a new request) or only snapshots (the host is silent).

## 9. Fallback

The pstore mailbox (`tooling/native/xnu-log.py`) is unchanged and still has everything PianoDwc3 logs (its own lines
included). If the takeover fails, the driver logs the stage and leaves the controller alone; the boot continues.

## 10. Risks

- **UEFI powered USB down at ExitBootServices** (UsbConfigDxe's handler): then level 2 refuses (clocks) or the link never
  comes up (PHY/repeater in low power). Seen on the normal boot path (2026-11-11: no host for 2.4 h). Since 2026-11-12 the cold
  start covers it (§17: GDSC, clocks, RPMh rail votes, the repeater, the M31 PHY); level 3 and `piano-usb-phy=1` stay as they were
  for `piano-usb-cold=0`.
- **SMMU access under Gunyah**: a read that never returns was seen on sheng from another thread; PianoDwc3 touches the SMMU
  only on the matching thread after NativeUfs. A hang there is caught by the watchdog; `piano-usb-smmu=0` skips it.
- **Core soft reset without PHY clocks**: CSFTRST times out (logged, stage −16); the UTMI select exists to avoid the PIPE
  clock dependency.
- **The monitor's `peek`** of an unclocked block hangs the CPU (as any such read); `poke` is off unless asked for.
- **No disconnect detection** (VBUS forced valid); unplug/replug shows as a bus reset — harmless.
- **Security**: port 1 + the helper is an unauthenticated root shell: development tablets, boot-arg gated, never by default.
- **Charging** is unaffected (the PMIC charger is not touched; the override is in the DWC3 glue only).

## 11. Open questions

1. What do piano-uefi's Qualcomm DXEs leave on at ExitBootServices (GDSC, clocks, M31, repeater, RUN_STOP, QSCRATCH)? → T1
   answered it for the fastboot path (everything on, RUN_STOP off). The normal path: the cold start's `cold: inherited ...` lines
   (§17.5) answer it on the first installed boot with this driver.
2. Where is SID 0x40 after the firmware (no entry, an exact entry on which bank, VA64)? → T1 SMMU line.
3. DWC3 version, IN endpoint count (2 ports need ≥ 5), TX FIFO sizes → T1.
4. Does the HS link come up without re-initialising the M31 PHY / repeater, and with the FSA4480 and EUD as the firmware left
   them (SunUEFI did not touch either)? → T2.
5. macOS host behaviour with this composite ACM device (expected: AppleUSBCDC* → two `cu.usbmodem*`) → T2.
6. Network next: Apple's NCM via an IOUSBDeviceController subclass (RE of `AppleVirtIOUSBDeviceController`) or our own ECM +
   `IOEthernetController` on this gadget core? Decided: our own CDC-NCM + `IOEthernetController`, no private ABI (§14).
7. Panic output over USB: a polled gadget path from the panic handler (the core already polls) — after T3.

## 12. Keyboard and mouse from the Mac (`piano-usb-hid=1`)

Debug only: to type the login password and click through the first screens while no touch or keyboard driver is in use.
Not run on hardware yet (2026-11-11).

- **Transport: a third CDC-ACM port**, "piano input". It exists only with `piano-usb-hid=1`, so the default device (two ports) is
  byte-for-byte the one that works. The console port is not touched: no echo, no log tail and no replay mix with the input, and
  the input never has to share a tty with `usb-console.py`. The core needs 7 IN endpoints for it (logged; with fewer the input port
  is left out). Port 2's endpoints are EP5 IN (notification), EP6 OUT/IN, which use TX FIFOs 5 and 6; their sizes after the reset
  are logged (`TX FIFOs after the reset: …`).
- **Protocol** (`package/PianoDwc3/HidInput.h`): one ASCII line per command, answered by one line that never repeats it:
  `K <mods> [<usage>…]` (keyboard report: modifiers, up to six usages, hex), `C <usage>` (consumer key), `P <buttons> <dx> <dy>
  [<wheel> [<pan>]]` (relative pointer), `R` (release all), `?` (status) → `ok <n>` / `err <what>` / `status …`.
- **HID devices**: `PianoUsbHID` (vendor 0x1AF4, product 0x5055 'PU'), a keyboard with consumer keys and a relative pointer, with
  **package/PianoKeyboard's report descriptors** (`HidDescriptors.h`, the cover's layouts; included, not copied) in the
  `IOHIDDevice` subclass pattern of PianoInput/PianoKeyboard (the VM-era HID class's, the one proven on this KC): same overrides, Built-In set,
  attached to the PianoDwc3 service and started from `start()` as PianoInput does; codeless `IOHIDEventDriver` personality
  `PianoUsbHIDEventDriver` (VendorID 6900, ProductID 20565) in `tooling/k27/kext/PianoDwc3.json`, vtable checked (334 slots).
- **Off the USB thread**: the bytes go to a ring; a HID thread parses lines and calls `handleReport`, so input never delays EP0 or
  the console. Nothing pressed survives: the port closing (DTR off), a bus reset or `piano-usb-hid-hold` ms without a command
  releases everything.
- **Never logged**: the kernel logs counts only (`hid: activity: N commands (keyboard k, consumer c, pointer p), …`); the
  replies carry a counter, never content; `piano-input.py` prints counts (`typed 8 characters (40 reports)`), writes no file, and
  names no character in its errors. `type --secret` reads the text with getpass, so it is not in argv, the shell history or `ps`.

On the Mac (standard library only; the port is found by its interface name, `--device` overrides):

```sh
tooling/native/piano-input.py status                       # status keyboard 1 pointer 1 commands N errors 0
tooling/native/piano-input.py type --secret --enter        # the login password, not echoed anywhere
tooling/native/piano-input.py type "hello world"           # US layout, shift for capitals and symbols
tooling/native/piano-input.py key return | tab | esc | up | cmd+space | shift+tab | volumeup  [--repeat N]
tooling/native/piano-input.py move 200 -50 [--steps 10]   # relative; positive right / down
tooling/native/piano-input.py click [left|right|middle] [--double]
tooling/native/piano-input.py scroll 3 [DX]                # wheel notches
tooling/native/piano-input.py drag 100 0 | release | keys | raw "K 0 4"
```

Build and test (host tests run in the build script; the Python tests on their own):

```sh
package/PianoDwc3/tests/run.sh                             # gadget core (2 and 3 ports), SMMU planner, input parser
python3 tooling/native/test_piano_input.py                 # layout, keys, pointer, protocol (+ the kernel parser), fake-tablet run
PIANO_EXTRA_ARGS="piano-usb=2 piano-usb-hid=1" PIANO_RESET_AFTER=300 tooling/k27/build-native-image.sh
```

Expected log lines (`grep PianoDwc3`), in this order:

```
PianoDwc3: start: piano-usb=2 phy=0 smmu=1 utmi=1 delay=0 shell=0 hid=1 (DWC3 0xa600000, SMMU stream 0x40, polled)
PianoDwc3: stage 7 HID input: keyboard and pointer (piano-usb-hid=1)
PianoDwc3: hid: Piano USB Keyboard: vendor 0x1af4 product 0x5055, usage 0x1/0x6, 92-byte descriptor (PianoKeyboard's), registry ID 0x…
PianoDwc3: hid: Piano USB Pointer: vendor 0x1af4 product 0x5055, usage 0x1/0x2, 79-byte descriptor (PianoKeyboard's), registry ID 0x…
PianoDwc3: hid: event service IOHIDEventDriver published on Piano USB Keyboard: macOS has it      (and on Piano USB Pointer)
PianoDwc3: core DWC_usb31/32, <n> IN endpoints: 3 ACM port(s) (port 2: piano input)
PianoDwc3: TX FIFOs after the reset: …                     (FIFOs 5 and 6 non-zero for port 2's IN endpoints)
PianoDwc3: setup 21 22 0003 0004 0 -> status only          (piano-input.py opened port 2: DTR; if within the first 48 requests)
PianoDwc3: port 2 opened (DTR/RTS 3)
PianoDwc3: hid: activity: 40 commands (keyboard 40, consumer 0, pointer 0), 0 releases, 0 auto-releases, 0 bad lines, 0 send errors
PianoDwc3: port 2 closed (DTR/RTS 0)
PianoDwc3: hid: port closed: …
```

Failure signs: no `event service … published` (IOHIDEventDriver did not attach: nothing reaches macOS); `hid: … handleReport 0x…`
(a report refused); `(no input port: it needs 7 IN endpoints)`; on the Mac `piano-input.py` saying `no answer from the tablet` (the
port's IN endpoint, e.g. a TX FIFO 5/6 of size 0: `--no-ack` still sends). The monitor's `usb` command prints the HID counters.

## 13. Licence note

New code under BSD-2-Clause-Patent (`package/PianoDwc3/LICENSE`). Taken from BSD-2-Clause-Patent sources with attribution
(`UPSTREAM`): Project-SunUEFI (takeover order, quirks, M31 init values), QcomDwc3's `QcomDwc3.cpp` (SPMI read and GDSC
pattern), UniXNU-sm8750-common `SmmuTakeover.h` (bypass bank shape). Not used: QcomDwc3's GPL-2.0 PHY headers, any Linux code
(Linux files were read as the specification only). The network function (§14) is written from the public USB CDC NCM 1.0 and
ECM 1.2 specifications; Linux's `f_ncm` / `u_ether` (GPL) were not copied, and the DWC3 TX FIFO rules come from the controller's
programming model as Linux's `gadget.c` describes it (specification only). PianoUsbNet follows this repository's
PianoNullEthernet (the VM-era Ethernet kext's pattern, BSD-2-Clause-Patent; its UPSTREAM).

## 14. A network link to the Mac (`piano-usb-net=1`)

Status 2026-11-11: built, host-tested (`package/PianoDwc3/tests/run.sh`), linked into the native KC with its vtable checked; **not
run on hardware** (the owner is away). Goal: IP between the tablet and the Mac over the cable that already carries the console,
for NTP time, file transfer and ssh, with Internet Sharing for the rest.

### 14.1 What the Mac and the tablet see

- **USB:** with `piano-usb-net=1` (and `piano-usb=2`) the composite device gets one more function after the serial ports:
  **CDC-NCM 1.0** ("piano network"): an IAD, a communications interface (header, union, Ethernet networking descriptor with
  `iMACAddress` = string 8 and `wMaxSegmentSize` 1514, NCM functional descriptor: NCM 1.0, SetEthernetPacketFilter only) with a
  notification IN endpoint, and a data interface whose alternate 0 has no endpoints and alternate 1 the bulk pair (NTB protocol).
  The Mac's own NCM driver takes it; nothing to install. Interface numbers: 4 and 5 (two ports), 6 and 7 (with `piano-usb-hid=1`).
- **NCM, NTB16 only:** `GET_NTB_PARAMETERS` says 16 KiB both ways, datagrams and NDPs 4-byte aligned, no datagram limit; the host
  may lower the IN size (`SET_NTB_INPUT_SIZE`, clamped to 2048..16384). No CRC, no NTB32. After the host selects alternate 1
  the function sends ConnectionSpeedChange (480 Mb/s at high speed, 12 Mb/s at full) then NetworkConnection (connected).
  Alternate 0 resets the function (input size, sequence numbers). The optional requests a host may still send (multicast
  filters, `SET_NET_ADDRESS`, `SET_MAX_DATAGRAM_SIZE`) are taken and ignored; statistics and encapsulated commands stall.
- **Frames:** to the Mac, the frames queued at each round go into one NTB (NTH16, the datagrams, then the NDP16 that lists them;
  up to 32 frames; padding zeroed; frames under 60 bytes zero-padded to 60); a block that ends on a packet boundary below the
  host's input size gets a zero-length packet. From the Mac, every NTB is parsed (an NDP chain of up to 8, each datagram checked
  against the block and against 14..1514 bytes); a refused block is counted and its first five logged with the reason, never its
  bytes.
- **MACs** (locally administered, fixed per build, derived from nothing on the tablet): the tablet's interface
  **02:50:49:41:4e:02**, the Mac's end **02:50:49:41:4e:03** (PianoNullEthernet's en0 is 02:50:49:41:4e:01).
- **Tablet:** `PianoUsbNet`, an `IOEthernetController` (`package/PianoDwc3/PianoUsbNet.cpp`) under the PianoDwc3 service,
  published from a thread once IOBSD is up, **not built-in and not primary**: configd's InterfaceNamer names it (en1, or the next
  free `en` unit its database gives a new MAC). Link down until the Mac selects alternate 1, then up at 480 Mb/s.

Why **not** replace PianoNullEthernet's en0: IOTimeSyncClockManager reads the built-in interfaces' MAC **once, at its start**
(`docs/bringup-debugging.md` §5, PianoNullEthernet's comment), and PianoNullEthernet's descriptor gates IOTimeSyncRootService
on its resource. The NCM interface exists only after IOBSD and the takeover, and a cable, a host and an enumeration are not
guaranteed at boot; nothing shows it would be there in time on every boot, so en0 stays the null built-in interface and TimeSync
is unchanged. Why **NCM rather than ECM**: both need the same three endpoints, so the budget below does not force ECM; NCM
aggregates frames per USB transfer (fewer, larger transfers for a polled controller) and the Mac has a native driver for it.

### 14.2 Endpoints and TX FIFOs

piano's core (T1 probe): DWC_usb31 (GSNPSID 0x33313130), GHWPARAMS3 0x90420086 = **32 endpoints, 16 IN**; GHWPARAMS0 MDWIDTH 64
bits (8-byte FIFO words); GHWPARAMS1 bit 23 clear (dual-port RAM), so the TX FIFOs live in RAM1: GHWPARAMS7 = **3462 words**
(27 KiB). The firmware left GTXFIFOSIZ0..4 at 0043, 0043/030d, 0350/0083, 03d3/030d, 06e0/0083 (start/depth); what the soft reset
leaves for FIFOs 5..15 has not been read yet (`TX FIFOs after the reset:` prints 0..7).

| Function | Endpoints (IN endpoint n uses TX FIFO n) | IN endpoints needed |
|---|---|---|
| EP0 | 0 OUT/IN | 1 |
| port 0 console, port 1 user pipe | notify 1 IN, bulk 2 OUT/IN; notify 3 IN, bulk 4 OUT/IN | up to EP4: 5 |
| port 2 input (`piano-usb-hid=1`) | notify 5 IN, bulk 6 OUT/IN | up to EP6: 7 |
| network (`piano-usb-net=1`) | notify **7** IN (interrupt, 16 bytes, 32 ms), bulk **8** OUT/IN (512 bytes at high speed) | up to EP8: **9** |

The endpoint count fits with room to spare (9 of 16 IN, 5 of 16 OUT): **nothing is dropped** for the network function, neither
the user pipe nor the input port. The network function takes endpoints 7/8 whatever the port count, so its numbering does not
depend on `piano-usb-hid`. A core with fewer than 9 IN endpoints gets no network function (logged); the ports stay.

TX FIFO plan (`package/PianoDwc3/TxFifoPlan.h`, only with `piano-usb-net=1`): FIFO 0 (EP0) is kept as it is; the FIFOs of the IN
endpoints in use are laid out after it, k packets of 512 bytes taking k x 65 + 1 words: notifications 1 packet (66 words), the
ports' bulk IN 4 (261), the network bulk IN 12 (781); FIFOs of unused endpoints are not written. With FIFO 0 at 0x43 words:

| FIFO | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | used of 3462 |
|---|---|---|---|---|---|---|---|---|---|
| two ports + net | 00430042 | 00850105 | 018a0042 | 01cc0105 | – | – | 02d10042 | 0313030d | 1568 words |
| three ports + net | 00430042 | 00850105 | 018a0042 | 01cc0105 | 02d10042 | 03130105 | 04180042 | 045a030d | 1895 words |

High speed only (the UTMI clock as PIPE clock, `DCFG` high speed): at SuperSpeed a packet would be 1024 bytes (129 words) and
bursts would want more, still inside 3462 words, but the SS PHY is not used. If the plan does not fit (another core), the network
function is left out with a note and the ports keep the reset's FIFOs. DMA: the network function's buffers sit in the unused top
of the existing 128 KiB region (notification at 0x15000, NTB IN at 0x16000, NTB OUT at 0x1a000, 16 KiB each), with the same
non-cacheable mapping and SMMU bypass as the ports; the allocation does not change.

### 14.3 What changes for the existing ports

- **Without `piano-usb-net`: nothing.** Device, configuration (both speeds), other-speed and string descriptors are byte for
  byte those of the ACM-only device (FNV-1a sums of the pre-network bytes in `tests/usb_gadget_host.cpp`), no TX FIFO register is
  written, no endpoint beyond the ports' is touched, no thread or queue is created.
- **With it:** the ports keep their interface numbers, endpoints and names (so `/dev/cu.usbmodemPIANODBG1/3/5` and the host
  tools' lookups by interface name stay); the configuration descriptor is 85 bytes longer with two more interfaces; strings 7 and
  8 exist; the ports' IN FIFOs get the plan's sizes instead of the reset's (`piano-usb-net-fifo=0` keeps the reset's, to tell a
  FIFO problem from an NCM one). The device descriptor and the VID:PID are unchanged.

### 14.4 Data path and logging

```
macOS stack -> PianoUsbNet::outputPacket (copy into a 64 KiB frame queue; dropped when the link is down or the queue full)
            -> USB thread: NtbWriter into the NTB IN buffer -> bulk IN 8 -> the Mac's NCM driver
the Mac     -> bulk OUT 8 (16 KiB, queued while an NTB's worth of frames fits) -> USB thread: parseNtb16 -> 64 KiB frame queue
            -> the net thread: allocatePacket + inputPacket (one thread: the family wants inputPacket serialized)
```

One NTB in flight each way, polled with the rest (1 ms when idle). Expected throughput: a few MB/s, enough for ssh and file
copies; not measured. Logged (`grep "PianoDwc3: net:"`): link changes, the interface's name, refused NTBs (the first five), and
one line of counters at most every 10 s while they change; never frame contents. The monitor's `usb` prints the same counters
plus the NCM state (alternate, NTB input size, packet filter, notifications, zero-length packets).

### 14.5 Bring-up

Build the test image (all native kexts, the KC, the RAM image in `out/ram`; RAM boot only):

```sh
package/PianoDwc3/tests/run.sh                                         # host tests (the build runs them too)
PIANO_EXTRA_ARGS="piano-usb=2 piano-usb-net=1" PIANO_RESET_AFTER=600 tooling/k27/build-native-image.sh
# add piano-usb-shell=1 for the root shell on port 1 (the helper in the root image, §6) to configure the tablet's side;
# add piano-usb-hid=1 for the keyboard/pointer port as well (the network function's endpoints do not change).
```

**On the Mac** (the tablet attached, the accessory allowed):

```sh
tooling/native/usb-console.py --list      # the gadget, its interfaces and drivers, the serial ports, and the network interface:
                                          #   network: en7 (MAC 02:50:49:41:4e:03), status active, inet ...
ifconfig en7                              # the name --list printed; "status: active" once the Mac selected alternate 1
```

Then either

- **Internet Sharing** (the tablet gets an address, DNS and the Internet through the Mac): System Settings > General > Sharing >
  Internet Sharing: share the connection from Wi-Fi (or Ethernet) to devices using the piano's interface (listed under the name
  macOS gives the NCM interface, e.g. "piano network" or "piano macOS debug"), then turn it on. The Mac bridges it (`bridge100`)
  and runs DHCP there; `ifconfig bridge100` shows the Mac's address. Or
- **a static pair** (no Internet, no DHCP; documentation addresses here, use any free private /24):
  `sudo ifconfig en7 inet 192.0.2.1 netmask 255.255.255.0` on the Mac.

**On the tablet** (the root shell of `piano-usb-shell=1`: `tooling/native/usb-console.py --port 1`). The tools are the standard
macOS userland of the system volume (`/sbin/ifconfig`, `/usr/sbin/ipconfig`, `/usr/bin/sntp`, `/usr/sbin/sshd`; not yet looked at
on this image: `ls -l` them first):

```sh
for i in $(ifconfig -l); do ifconfig $i | grep -q 02:50:49:41:4e:02 && echo $i; done     # which en is ours (say en1)
ipconfig set en1 DHCP && ipconfig waitall && ipconfig getifaddr en1     # with Internet Sharing
ifconfig en1 inet 192.0.2.2 netmask 255.255.255.0 up                    # or the static pair
ping -c 3 192.0.2.1                                                     # (static) or the router of `ipconfig getpacket en1`
sntp -sS time.apple.com                                                 # NTP time (Internet Sharing: DNS and NAT through the Mac)
launchctl load -w /System/Library/LaunchDaemons/ssh.plist               # sshd; then ssh from the Mac (an account with a key)
```

Set to DHCP with no server, both ends fall back to self-assigned 169.254.x.x addresses (`ipconfig set en1 DHCP` here, "Using
DHCP" for the interface on the Mac), enough for ssh/scp by address. Files without sshd: on the Mac
`python3 -m http.server 8000 --bind 192.0.2.1`, on the tablet `curl -O http://192.0.2.1:8000/<file>`.

### 14.6 Expected log lines and failure signs

In this order (`grep PianoDwc3` in the console log; `usb-console.py` shows them live):

```
PianoDwc3: start: piano-usb=2 ... hid=0 net=1 (DWC3 0xa600000, SMMU stream 0x40, polled)
PianoDwc3: net: CDC-NCM network function requested (piano-usb-net=1): MAC 02:50:49:41:4e:02 here, 02:50:49:41:4e:03 on the Mac
PianoDwc3: core DWC_usb31/32, 16 IN endpoints: 2 ACM port(s) + the CDC-NCM network function (interfaces after the ports, endpoints 7 and 8)
PianoDwc3: TX FIFOs after the reset: 00000043 ...                 (what the reset left, FIFO 0 first)
PianoDwc3: net: TX FIFO plan: words 67..1568 of 3462 (8 bytes each): 1 00430042 2 00850105 3 018a0042 4 01cc0105 5 00000000 6 00000000 7 02d10042 8 0313030d
PianoDwc3: net: TX FIFOs now: 00000043 00430042 00850105 018a0042 01cc0105 <5 as reset> <6 as reset> 02d10042 0313030d
PianoDwc3: net: interface published (MAC 02:50:49:41:4e:02, not built-in; configd names it), registry ID 0x...
PianoDwc3: net: link down
PianoDwc3: net: the Mac selected the NCM data interface
PianoDwc3: net: link up: the Mac runs the NCM data interface (480 Mb/s)
PianoDwc3: net: macOS named the interface en1 (en0 stays PianoNullEthernet's built-in one)
PianoDwc3: net: link up, en1 up; to the Mac 12 frames 1480 bytes (9 NTBs), from the Mac 30 frames 4100 bytes (22 NTBs); NTB errors 0, bad datagrams 0; drops: send 0 (+0 bad length), receive 0 (queue) 0 (no mbuf) 0 (not up)
```

(The FIFO numbers assume FIFO 0 at 0x43 after the reset, as the firmware left it; the plan starts after whatever FIFO 0 is.)

| Sign | Meaning, next step |
|---|---|
| `(no network function: it needs 9 IN endpoints)` / `TX FIFO plan: ... left out` | not the core measured in T1; the ports still work |
| the console port fails only with `piano-usb-net=1` | the FIFO plan: retry with `piano-usb-net-fifo=0`; compare `TX FIFOs after the reset` and `now` |
| `--list`: `piano network/no driver` | the Mac did not take the NCM function: the `setup ...` lines (which requests, what was stalled) |
| no `the Mac selected the NCM data interface` | the Mac's driver never chose alternate 1: the `setup 01 0b 0001 ...` line and the requests before it |
| link up here, `status: inactive` on the Mac | the notifications: `usb` shows 2 notifications per selection; an interrupt read the Mac never completes |
| `NTB from the host refused (<reason>)` | the Mac's NTBs differ from the parser's reading of NCM 1.0: the reason names the field |
| no `macOS named the interface` | configd has not named it (user space not up yet, or InterfaceNamer skipped it): `ioreg -c PianoUsbNet` in the shell |
| `macOS named the interface` only about 240 s after launchd started | InterfaceNamer's quiet timeout: the MAC was not in its database (§14.8) |
| `drops: send N` growing | the link is down, the interface is down (`ifconfig en1 up`), or the queue is full (the Mac is not reading) |

### 14.7 Risks

- **The Mac's NCM driver** has only been reasoned about from the spec: its request sequence, whether it takes each notification
  as one interrupt transfer (16 bytes = one packet of the 16-byte endpoint, no zero-length packet after it), and whether it takes
  NTBs with the NDP after the datagrams (a valid NTB16 layout, the one Linux gadgets send) are first-run questions. The parser
  takes both NDP-first and NDP-last NTBs from the Mac.
- **AppleUSBCDCCompositeDevice with an NCM function in the same composite**: expected to hand each IAD to its class driver as it
  does the ACM ports; not seen yet.
- **TX FIFO writes** change the ports' FIFOs too (only with `piano-usb-net=1`); `piano-usb-net-fifo=0` is the way back.
- **Naming**: the interface is not built-in, so it depends on configd's InterfaceNamer; a Data volume whose
  `NetworkInterfaces.plist` lists VM-era interfaces may give it a higher unit than en1. en0 and TimeSync are untouched by design.
  A MAC that is not in that database is named only once the IOKit registry is quiet, or after 240 s (§14.8).
- **Throughput**: one 16 KiB NTB in flight each way on a polled loop; fine for ssh and NTP, slow for large copies.
- **Security**: whatever the tablet serves on this link (sshd, a shell) is reachable from the Mac (and with Internet Sharing from
  nothing else: NAT); development tablets only, as the user pipe.

### 14.8 The interface's name and address on desk-dev (2026-11-12)

**What desk-dev at 17:27 showed.** The Mac had its end at once (its NCM interface, MAC 02:50:49:41:4e:03, active, self-assigned 169.254).
The tablet logged `net: interface published` during boot. But for minutes after login, `ifconfig -a` there listed only `lo0` and
`utun*`, so `desk-dev.sh` gave up: `no interface with MAC 02:50:49:41:4e:02 after 120s`. Both `en0` and `en1` were attached
together, 242 s after launchd started (`ifnet_attach: ... en0`, `... en1`, then `net: macOS named the interface en1`). A self-assigned
169.254 address followed on en1 (the Mac's ARP table had it). The night's `net` run, about 190 s after login, was the same wait.

**The cause** is configd's InterfaceNamer (configd's `Plugins/InterfaceNamer/ifnamer.c`, the same in the Mac's 26.6 configd, whose
strings include `waiting for quiet` and `WaitQuietTimeout`). It registers an interface with IONetworkStack, which creates the BSD
interface, in one of two ways:
- **Known MAC:** the prefix (`en`) and the MAC are in `/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist`. It is
  named at once: `Assigned unit N (from database)`.
- **New MAC:** `copyUnitForEstablishedInterface` defers it ("new interface, wait until registry quiesces") until the IOKit
  registry is quiet, or until `WAIT_QUIET_TIMEOUT_DEFAULT`, 240 s after IONetworkStack appears.

On piano the registry never goes quiet (some service stays busy). The desk images' Data is volatile, so their database never keeps
a boot's names, and every boot's en0 and en1 are new: 240 s. The same wait holds `SCNetworkInterfaceCopyAll`'s callers
(`__wait_for_IOKit_to_quiesce`, quiet or timeout) and with them configd's own configuration of new interfaces. So the 169.254
address came only then.

**The fix (desk-dev).**
- The seed gets `tooling/k27/desk-dev-NetworkInterfaces.plist` as that database: en0 (02:50:49:41:4e:01, PianoNullEthernet,
  built-in) and en1 (02:50:49:41:4e:02, PianoUsbNet). Both are named as soon as InterfaceNamer finds IONetworkStack. The file has
  no `Model` key, because InterfaceNamer discards a database made on another `hw.model` and keeps one with none. It writes its own,
  with the model, at the next quiet.
- `tooling/k27/desk-dev.sh` looks for the interface for up to 300 s (it was 120 s, less than the timeout), then sets the address by
  `DESK_DEV_NET` (the builder's variable, the script's argument):

| Mode | Address |
|---|---|
| `auto` (default) | `ipconfig set en1 DHCP`. A lease within 20 s that is not 169.254 (Internet Sharing on the Mac) is kept. Otherwise `ipconfig set en1 MANUAL 169.254.80.2 255.255.0.0`, the **fixed link-local address** |
| `static` | 169.254.80.2/16 at once |
| `dhcp` | DHCP only: a lease, or IPConfiguration's self-assigned 169.254 address |

  Once configd has run 260 s (past the timeout, when the system's own configuration of the interface may replace an address), the
  script checks the fixed address and sets it again if it is gone. It then prints the IOKit registry's busy leaves (up to 6), which
  are what to fix so that the registry goes quiet.
- **On the Mac:** `tooling/native/desk-ssh.sh` (no root) finds the Mac's end by its MAC and runs
  `ssh -B <that interface> -i out/desk-dev/id_ed25519 <account>@169.254.80.2`. The `-B` is needed because macOS routes 169.254/16
  to its primary interface (Wi-Fi: `route -n get 169.254.80.2` says `en0`), not to the cable. A socket bound to the cable's
  address takes the route scoped to it. Options: `-a ADDRESS` (the one the console line printed: a DHCP lease, or `fe80::...`, to
  which the interface is added as its scope), `-u ACCOUNT` (default `DESK_SSH_USER`, else the Mac's user name), `-n` (print the
  command). No host key is kept or checked: the tablet's are new every boot (volatile Data), and the link is the cable.

Lines to look for (`grep -a 'desk-dev:'`):

```
desk-dev: ssh: port 22 answers
desk-dev: usb net: en1 is ours (named after 0s); mode auto
desk-dev: usb net: en1 inet 169.254.80.2 (fixed link-local) inet6 fe80::...
desk-dev: usb net: on the Mac: tooling/native/desk-ssh.sh -a 169.254.80.2 (ssh -B <the Mac's end> -i out/desk-dev/id_ed25519)
desk-dev: iokit busy (configd 260+ s): <name> (<class>) busy 1 (<ms>)       the registry's busy leaves, or "none"
```

`named after Ns` with N near 240 means the database did not match: check the MAC in `ifconfig en1`, and
`/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist` on the tablet. Not yet run on the tablet (2026-11-12 evening).

### 14.9 The address after the login (2026-11-14): `piano-usbnet-address`

What the tablet showed (confirmed in the consoles of ufs4a, ufs4b and the v6 RAM test): desk-dev set `en1 inet 169.254.80.2 (fixed
link-local)` at T+33-62 s and sshd answered locally, then ssh from the Mac timed out after the desktop login and PianoDwc3's `to the
Mac` frame counter stopped while `from the Mac` kept growing; on 10p (no login, desk-dev at T+95 s) ssh worked. The reading
(inferred, H1 of the 2026-11-14 log study): `ipconfig set en1 MANUAL` is IPConfiguration's setting, not a SystemConfiguration
service; when configd configures en1 itself (the registry's quiet, which on the fast boots comes with the login wave) it applies its
own service for it, DHCP with a self-assigned link-local address, and 169.254.80.2 is gone. desk-dev's own re-check (`late()`, at
configd's 260th second) came after the test boots ended.

The fix is a userland unit (`configs/userland.toml`, default on, inert without `piano-usb-net`), so it reaches the test seeds through
`stage.py` and the owner's installed store through the next live update (`docs/userland.md` §7):
`/usr/local/libexec/piano-usbnet-address` with `/Library/LaunchDaemons/com.unixnu.piano-usbnet-address.plist` (once per boot):

1. it waits (300 s) for PianoUsbNet's interface (MAC 02:50:49:41:4e:02) and desk-dev's fixed address on it; a DHCP lease (Internet
   Sharing on the Mac) or no address is left alone;
2. in the background, the interface's SystemConfiguration service (configd's own for it, else a new "Piano USB" on its hardware
   port) is set to IPv4 Manual 169.254.80.2/16 with `networksetup`, so configd applies the address itself whenever it configures en1;
   on the installed store the service persists across boots. `networksetup` waits until configd's interface list is ready; its line
   says how long;
3. for 600 s, every 10 s: the address checked; if gone, one line with what is there instead (inet, `scutil --nwi`'s IPv4 part) and
   the address set again (`ipconfig set en1 MANUAL`). That line is the check of H1 on every boot.

Lines (`grep -a 'piano-usbnet-address:'`):

```
piano-usbnet-address: en1 has 169.254.80.2 (after N s): its SystemConfiguration service next, the address guarded for 600 s
piano-usbnet-address: en1: service "NAME" set to IPv4 Manual 169.254.80.2/16 after N s: configd applies it from now on
piano-usbnet-address: en1 lost 169.254.80.2 (inet now: ...; scutil --nwi: ...): set again (loss N)       H1, seen
piano-usbnet-address: en1: guard over after 600 s, N losses, 169.254.80.2 present
```

To confirm H1 by hand on a running boot after the login, in the root shell (`tooling/native/usb-console.py --port 1`):
`ifconfig en1 | grep -w inet; scutil --nwi; pfctl -s info 2>/dev/null | grep -E '^Status'` (the fixed address gone and a
self-assigned 169.254.x.y in its place: H1; the address still there and pf enabled: a packet filter instead). Host test:
`tooling/native/piano-usbnet-address/test_piano_usbnet_address.py` (stub commands: no piano-usb-net, configd's service set Manual,
a new service when none, a loss logged and repaired, a DHCP lease left alone). Not run on the tablet yet.

## 15. Power: the USB thread's wait (`piano-usb-idle`, `piano-usb-irq`)

Until 2026-11-11 the USB thread slept 1 ms after every pass without events, forever: about 1000 wake-ups a second even with
no cable. Now (`UsbIdle.h`, host-tested by `tests/usb_idle_host.cpp`) each pass picks a tier: **busy** 1 ms (an event, log
bytes moved or a kick within 200 ms, a control transfer past its SETUP, bytes waiting for a free IN endpoint), **open** 20 ms
(configured with a port's DTR or the NCM link: the kernel-log pump), **configured** 50 ms, **no host** 250 ms (nothing
configured and no event for 3 s, or the link in suspend / disconnected / early suspend). The console and pipe writers, the
pipe, HID and network readers and the network output queue kick the thread, so their data never waits for a period. The HID
thread waits 100 ms only while a key or button is held (the auto-release), 60 s otherwise.

`piano-usb-irq=1`: `armcpu_spi_attach(165, level)`; the handler masks the event buffer's interrupt (GEVNTSIZ bit 31) when
GEVNTCOUNT is non-zero and kicks the thread; the thread takes the events and unmasks before each wait, so events that came in
between raise the line at once. The periods become 10 / 20 / 200 / 1000 ms (bookkeeping: EP0 watchdog, log pump, DSTS). A
storm (more than 2000 entries in 100 ms with nothing to mask) disables the line at the GIC; three timed-out waits in a row
with events pending and the line unmasked mean it is not delivered; either way the thread masks it for good and polls
(`irq: off (...)`). Opt-in until a run shows delivery (the first native SPI; [`../power-irq.md`](../power-irq.md) §2).

Lines: `wait: polled, adaptive (busy 1 ms, port open 20 ms, configured 50 ms, no host 250 ms); piano-usb-irq=1 for the core's
interrupt` or `irq: SPI 133 (INTID 165, level) attached (0): readback 0xa003 (...), router 0x...; events wake the USB thread`, then
every minute `idle: N wakeups in 60 s, irq M (kick K, timer T; passes busy b open o configured c no-host n; polled|interrupt;
link state L, configuration C; SPI 133 count X)`; the monitor's `usb` adds `wait: <mode>, tier <tier>; SPI 133: <state>, N
handled, S strays, M missed`. Expected at rest: no cable ~240/min (polled) or ~60/min (interrupt); Mac attached with the console
open ~3000/min (open tier, the log pump). A cable plugged in after more than 3 s without a host may lose the first SETUP in poll
mode (the Mac re-enumerates within seconds). Test plan and images: [`../power-irq.md`](../power-irq.md) §5.

## 16. System sleep (`piano-sleep-usb`, 2026-11-11)

[`../power.md`](../power.md) §4's contract for PianoDwc3, built as `UsbSleep.h` (host-tested by `tests/usb_sleep_host.cpp`; the
gadget's half, `sleepDisconnect` / `sleepConnect`, by `tests/usb_gadget_host.cpp`'s sleep cycle). At `piano-usb=2` the service
joins the power tree (`pianopower::join`); the gadget stays the USB thread's, so `setPowerState` only asks and the thread acts at
the top of its next pass (off: `setPowerState` waits for it, at most 500 ms; on: asynchronous).

| Case | off | on |
|---|---|---|
| a host has the device configured, `piano-sleep-usb=1` (default) | **kept**: nothing changes on the bus, the console survives (the Mac sees a device that does not answer while frozen, then the backlog); the core's interrupt (with `piano-usb-irq=1`) masked at its source and the thread polls until the freeze, so no USB event ends the suspend | the glue's clocks read; on: the passes as before (the interrupt unmasked by the next wait) |
| no host configured, or `piano-sleep-usb=0` | **soft disconnect**: RUN_STOP off (the Mac sees a detach; a cable plugged in while suspended is not half-enumerated), the endpoints and the address forgotten, the interrupt masked; the thread does not touch the core | the glue's clocks read; on: the event buffer drained, the device side set up again, RUN_STOP on: the Mac enumerates afresh |
| before the takeover (`piano-usb-delay`) | nothing; the takeover waits for the wake | the takeover runs |
| the glue not clocked at the wake | – | nothing is touched again this boot (core, QSCRATCH): logged with the four GCC words |

**QSCRATCH** (the glue's VBUS/session override, the UTMI-as-PIPE clock select, a monitor `poke` into 0x0a6f8800..+0x100): every
write goes through one guard and reaches the glue only while awake with the glue's clocks on (GCC: the USB30 GDSC on, the master
and both AXI branches running); otherwise it is recorded (the last value per register, four registers) and written by the thread
once awake and clocked (Ubuntu K:7dffd78b342d: the VBUS-override write that a UCSI role switch ran while the glue was suspended
never completed and reset the SoC; its fix records and applies later, the same rule). PianoDwc3 itself never gates these clocks;
the guard is for whoever does (the firmware, a later runtime-PM step). The monitor's `usb` adds `sleep: <state> (piano-sleep-usb=N);
N sleeps, N wakes; QSCRATCH writes recorded N (lost N)`.

Lines: `system sleep: in the power tree (0): a host that has the device configured keeps it across a suspend ...` at start; per
suspend `sleep N: the Mac has the device configured: kept (the console survives the suspend; ...)` or `sleep N: soft disconnect (no
host has the device configured): RUN_STOP off (halted), the endpoints and the address forgotten; ...`, after the wake `wake N: the
glue clocked; kept: the console goes on` or `wake N: the glue clocked: connected again (RUN_STOP on): the Mac enumerates afresh`.
Failure lines: `sleep: the USB thread did not answer within 500 ms`, `wake N: the glue is NOT clocked (GDSC ..., master ..., ...)`,
`QSCRATCH 0x10 = ... not written now (...): recorded for the wake`. The cable to a host keeps the HS PHY out of its low-power state
(power.md §4): a suspend measurement is made without it (`piano-sleep-usb` then decides nothing: no host is configured).

## 17. The cold start (`piano-usb-cold`, 2026-11-12)

Status: built, host-tested (`tests/usb_cold_host.cpp` against a model of the hardware), linked into the native kernel collection.
**Run on the tablet on 2026-11-12** (queue7, C0-C7 of [`../forensics-2026-11-11.md`](../forensics-2026-11-11.md) §7.5):
- C2a and C2b reproduce the installed ESP's "no host", and C3 and C4 cure it; C0, C1, C5 and C7 enumerate.
- **C6 stopped the whole system**: the core was in the host role, and the takeover soft-reset it there.

The role switch and the role rule (§17.8) fix that. They are host-tested but not yet run: re-test with C6b and C6c (forensics
§7.7). Code: `package/PianoDwc3/UsbCold.h` (portable: the inspection, the decision, the steps, the role switch, the test hook, the
link watch) and its kernel side in `PianoDwc3.cpp` (`ColdHal`, `coldDecide`, `takeover(cold)` with its role gate, `coldRestart`,
`coreRd`/`coreWr`). Test plan: forensics §7.

### 17.1 Why

Every console so far followed ABL fastboot. ABL ran the controller as a device for the Mac, and the FD stopped it at
ExitBootServices with power, clocks, the M31 PHY and the repeater still on. The probe of `20261111-201656` reads GDSC `fa22f800`,
the branches `08000001`, the master RCG `00100105`, GCTL `00102005` (device role), the firmware's event buffer at `0xd359f000`,
DEVTEN `0x1277`, M31 `[54] 6b` and repeater `fd08/fd46 = 80/80`. The takeover kept all of that.

The installed daily ESP (the normal path) got to `stage 20 running`: the core was clocked, since the takeover passed stage 10. It
then saw no host for 2.4 h. Its minute lines say `passes ... no-host 22x; polled; link state 3, configuration 0`
(`logs/native-runs/20261111-232127-installed-dump`). Link state 3 is USB 2 suspend: the device sees an idle bus and no reset ever
comes, which is how a D+ pull-up that the Mac never sees looks. Likely causes are the eUSB2 repeater (its 3.1 V side drives the USB 2
lines), the M31 PHY, or their rails not being voted, on a path where ABL never ran USB. The probe's own lines of that boot had been
overwritten in the ring by then. The cold start does not need to know which it was.

### 17.2 What runs

| Step | What | Shared resource, rule | On failure |
|---|---|---|---|
| inspect (every mode, read only) | GCC: the USB30 GDSC, the five branches, the master/mock_utmi RCG configuration, GPLL0's mode and vote word; the TCSR USB2 reference clock; the core while clocked (GSNPSID, GCTL role, GSTS, GEVNTADR; in the device role only DEVTEN (did the firmware run it as a device), DCTL/DSTS/DCFG; in the host role only the xHCI's USBCMD/USBSTS); the M31 while clocked, with its reference clock on and its vdd not off; the repeater (status, enable, `force_en_5`/`force_val_5`); the three rails (STATUS1, set point, MODE, EN_CTL, each LDO found by the page rule and its TYPE); GPIO61 (the Type-C orientation, read through TLMM); the PMIC's Type-C block only with `piano-power=0` and `piano-usbhost=0` (one observer reader per peripheral: otherwise PianoPower's `usb:` line has it) | none written | unknown reads never count as a reason |
| decision | `piano-usb-cold=auto`: cold when any of the 11 reasons holds: the GDSC collapsed, a branch off, an RCG not at 200 MHz / 19.2 MHz, the USB2 reference clock off, the core not answering, not in the device role, never run as a device by the firmware, the M31 not initialised, the repeater not enabled/ready, its host-mode clock forced, a rail off | — | — |
| c1 power | the GDSC on (SW_COLLAPSE off, RETAIN_FF, the waits 2/2/15); GPLL0's enable vote (GCC `0x52020` bit 0); the branches cfg_noc, aggre, master, sleep, mock_utmi; the RCGs at 200 MHz (GPLL0 / 3) and 19.2 MHz (TCXO), root forced on around the update; the TCSR USB2 reference clock | the vote word only under PianoA830's `sm8750_gcc_vote_owner` (tag `DWC3`, 50 ms, then set anyway: bits only ever set) | hard: the GDSC or a branch that does not come on stops it (`cold c1 FAILED`) |
| c2 stop, the device role | the event interrupt masked; a running device controller halted. In the host role (`toDevice`, §17.8): the xHCI stopped (RS and INTE cleared, HCHalted awaited), then the programming model's switch (PHYs awake, GCTL.CORESOFTRESET only on DWC_usb3 / DWC_usb31 before 1.90a, PRTCAPDIR device, CURMOD awaited, then DCTL.CSFTRST), which counts only when shown (CURMOD, CSFTRST cleared, DEVCTRLHLT, TX FIFOs 0-6 a device layout). The block reset `USB30_PRIM_BCR` (20 us, 1 ms settle) when that switch is not shown, the xHCI's registers do not answer, the role is neither host nor device, c1 found the GDSC collapsed, or steps bit 0x8 asks for it | — | hard: a core that does not answer as a DWC_usb3x; an xHCI that does not report halted (nothing more written: no reset under a running DMA master); GCTL not reading the device role at the end |
| c3 rails | `vreg_l2d_0p88` (`ldod2`: the M31's vdd, the QMP's vdda-phy), `vreg_l15b_1p8` (`ldob15`) and `vreg_l5b_3p1` (`ldob5`: the repeater's vdd18 and vdd3): one VRM transaction each on apps DRV-2 (mode 7, set point = the PMIC's when within the DT's range else the DT minimum, enable), 5 ms, read back over SPMI. `vreg_l3g_1p2` (the M31's vdda12) is never voted: it is the UFS PHY's PLL rail too | DRV-2 only under PianoA830's `sm8750_rpmh_drv2_owner` (tag `DWC3`, at most 2 s, else not sent); PianoBluetooth's `vrmSend` (TCS 2, 1, 0; a TCS that does not complete is never used again) | soft |
| c4 repeater | only when PPID `0x7fd`'s APID belongs to EE 0: EN_CTL1 = 0x80; the board's tuning when the repeater was found off (piano's DT: `0x51`=0x0e, `0x52`=0x01, `0x53`=0x07, `0x54`=0x03, `0x57`=0x06), else the firmware's kept (steps bit 0x20: always); device mode (`0xe8`/`0xed` = 0); status bit 7 awaited (10 ms) | SPMI write channel of that APID | soft |
| c5 PHY | not while its vdd reads off: the TCSR reference clock on, `QUSB2PHY_PRIM_BCR` pulsed, `kM31Init` (the stock firmware's sequence, `piano-usb-phy=1`'s), read back against its end state (`01 00 6b 07 00`) | — | soft |

The takeover then goes on:
- 12: the role gate (`ensureDevice`: the device role before the first device register, in every mode, §17.8), then the halt.
- 13: DMA.
- 15: the UTMI clock as PIPE clock.
- 16: the core soft reset. It needs the PHY's UTMI clock, so it is the first proof that the PHY runs.
- 17: device mode (high speed). The role is already device here; the gadget refuses otherwise and no longer writes PRTCAPDIR itself.
- 18: the VBUS/session override (QSCRATCH, through the sleep guard).
- 19: RUN_STOP.

Not touched: the ADSP (charging, audio and the sensor hub live there), the QMP PHY and the DIO4485 mux (high speed needs neither:
both D+/D- pairs meet in the receptacle), the CX/MX levels and the interconnect votes. Those are RPMh resources shared on DRV-2,
which keeps one vote per DRV, so a second writer would replace another kext's vote.

### 17.3 Timeouts, retries, the link watch

- Every poll is bounded: the GDSC and each branch 10 ms, an RCG 1 ms, a halt 100 ms (the xHCI's HCHalted too), GSTS.CURMOD 10 ms, the
  role switch's DCTL.CSFTRST 250 ms (then 50 ms settling), the repeater 10 ms, an SPMI access 1 ms, a TCS 100 ms, the DRV-2 lock 2 s,
  the GCC vote lock 50 ms. c1 and c2 fail hard and return at once. c3-c5 failures are logged and the start goes on. No core,
  QSCRATCH or M31 access happens with the glue unclocked. No M31 access happens with its vdd known off. No device register is touched
  outside the device role, and no xHCI register outside the host role (§17.8).
- A takeover that fails (modes 1 and auto) gets another cold start 2 s later, up to 3 cold starts per boot. After that the old
  failure path applies: the controller is left as it is and the boot goes on.
- The **link watch** (modes 1 and auto, `piano-usb-cold-wait=15`): if no bus reset has come 15 s after RUN_STOP, it runs a cold
  restart. That restart stops the device controller, reports the state again, does the cold start and the takeover. Each further
  wait doubles (30, 60 s), and the 3-per-boot limit applies (the boot's first cold start counts). A bus reset ends the watch for the
  boot. A hand-off to PianoUsbHost disarms it. It acts only while the system is awake. If the auto decision took a state as running
  that was not, the watch catches it. Without a cable it costs at most three cold starts in the first 2 minutes.

### 17.4 Proving it from ABL fastboot: the test hook and `usb cold`

`piano-usb-cold-sim=MASK` (with `piano-usb=2`) runs on the USB thread before the decision. It changes only what it can change
safely: the core only while clocked (halted first), the repeater only when its APID is EE 0's.

| Bit | Leaves the hardware with | The decision then reads |
|---|---|---|
| 0x1 | the repeater disabled (EN_CTL1 0) and its host clock forced (0x40/0x40) | the repeater not enabled/ready; its host clock forced |
| 0x2 | the M31 reset (BCR) and left at its reset values, its TCSR reference clock off | the USB2 reference clock off (the M31 is then not read) |
| 0x4 | the core in the host role (PRTCAPDIR 1), the VBUS/session override cleared | not in the device role (c2's role switch: C6c) |
| 0x8 | the RCGs parked on TCXO, the five branches off, the GDSC collapsed (RETAIN_FF as found: the core keeps its retention flops, C6) | the GDSC collapsed, an RCG not at its rate (the core not read; c2 then takes the block reset) |
| 0x10 | the three rails' DRV-2 votes withdrawn (enable 0) | a rail off if nothing else votes it (the M31 not read unless its vdd reads on) |

With `piano-usb-cold=0` the same masks show the old takeover's behaviour on such a state, which is the control experiment. The
monitor's `usb cold [sim MASK]` does the same live: the reply goes out, 0.5 s later the link drops, the Mac enumerates again, and
`usb-console.py` reconnects and shows the lines in its replay. `usb` prints a `cold:` line with the mode, the steps, the reasons
found at the takeover, the cold starts so far, the breadcrumb and the watch.

### 17.5 Lines

In this order (`grep 'PianoDwc3: cold'`), here from the fastboot path's values. The normal path shows its own:

```
PianoDwc3: start: piano-usb-cold=auto steps=0x17 sim=0 wait=15 s (the cold start, UsbCold.h: when the inherited state is not ...)
PianoDwc3: cold: inherited GCC: gdsc fa22f800 cfg_noc 08000001 aggre 08000001 master 08000001 sleep 08000001 mock_utmi 08000001; RCG cfg master 00100105 mock_utmi 00100001; GPLL0 mode ... vote ...; TCSR usb2 clkref 00000001
PianoDwc3: cold: inherited core 33313130 GCTL 00102005 (role 2: device) GEVNTADR 00000000d359f000 DEVTEN 00001277 DCTL 10f00000 DSTS 00d34554 DCFG 0048080c; M31 ready [3c] 01 [50] 00 [54] 6b [64] 07 [94] 00
PianoDwc3: cold: inherited repeater (SID 7 0xfd00): ready and enabled, status 80 enable 80 force_en_5 00 force_val_5 00 (APID owner EE 0)
PianoDwc3: cold: inherited rail vreg_l2d_0p88 (ldod2, M31 vdd, QMP vdda-phy): PPID 0x... status 80 enable 80 mode .. NNN mV: on      (and l15b, l5b)
PianoDwc3: cold: Type-C orientation normal (CC1) (GPIO61 level 0, cfg ...): high speed needs no lane mux; the PMIC's Type-C block is PianoPower's to read
PianoDwc3: cold: verdict (piano-usb-cold=auto): the inherited state is a running device controller (none): taken as it is (the takeover of before)
     or: ... is NOT a running device controller (the repeater is not enabled/ready; a rail is off): the cold start runs
PianoDwc3: stage takeover: cold start (piano-usb-cold: power, stop, rails, repeater, M31 PHY)
PianoDwc3: cold: start (steps 0x17: rails 1, repeater 1, M31 PHY 1, block reset 0, RCGs 1, tuning always 0)
PianoDwc3: cold c1 power: GDSC ...; GPLL0 mode ... vote ... (voted already); branches ... (0 switched on); RCG cfg master 00100105 mock_utmi 00100001; usb2 clkref 00000001
PianoDwc3: cold c2 stop: core 33313130, was role 2 (GCTL 00102005): nothing ran; no block reset; now GCTL 00102005 (role 2) GUSB2PHYCFG ... DSTS ...
     from the host role (sim 4, C6c), two role lines first:
PianoDwc3: cold c2 role: found role 1 (host, GCTL 00101005); xHCI halted already (USBCMD 00000000 USBSTS 00000001 after 0 us); soft switch PROVEN (GCTL.CORESOFTRESET 0, CURMOD device 1, CSFTRST cleared 1 after N us, halted 1); block reset not taken; now GCTL 00102005 (role 2: device)
PianoDwc3: cold c2 role: TX FIFOs 0-6 after the soft switch 00000043 0043030d 03500083 03d3030d 06e00083 07630105 08680083 RX 00000313: a device layout
PianoDwc3: cold c2 stop: core 33313130, was role 1 (GCTL 00101005): nothing ran (the xHCI was halted); no block reset; now GCTL 00102005 (role 2) ...
     after a collapse (sim 31, C6b): "soft switch not tried ...; block reset c1 found the GDSC collapsed (the core kept only its
     retention flops); now GCTL 00102004", the FIFO line "after the block reset ...", "block reset (USB30_PRIM_BCR) done"
PianoDwc3: cold: cmd-db: ldod2 0x..., ldob15 0x..., ldob5 0x...
PianoDwc3: cold: rsc: apps DRV-2 id 0x... (version 3.0), TCS block +0xd00
PianoDwc3: cold: rsc: ldod2 (0x...) mode 7, set point, enable 1: TCS 2 completed after N us (status ...; DRV-2 lock after 0 waits of 10 us)
PianoDwc3: cold c3 rail vreg_l2d_0p88 (ldod2, M31 vdd, QMP vdda-phy): PPID 0x... before status 80 enable 80 880 mV (on); voted 880 mV, mode 7, enable: done; after ...: on
PianoDwc3: cold c4 repeater: before status 80 enable 80 force 00/00 tune [51] .. ; enable, the tuning kept (it was on), device mode: written; after status 80 enable 80 ...: ready
PianoDwc3: cold c5 PHY (M31: reset, the stock init): before [3c] 01 [50] 00 [54] 6b [64] 07 [94] 00, after 01 00 6b 07 00: ready
PianoDwc3: cold: done: 3 rail votes, repeater ready, M31 ready; soft failures 0 (bit n: step cn)
PianoDwc3: ... the takeover's stages 12-20 as before ...
PianoDwc3: cold: link watch: no bus reset from a host within 15 s: a cold start (the wait doubles; at most 3 cold starts this boot, 1 so far)
PianoDwc3: cold: link watch: a host reset the device: the watch ends (1 cold starts this boot)
```

### 17.6 Failure signs

| Sign | Meaning, next step |
|---|---|
| `cold c1 FAILED: the USB30 GDSC does not power on` / `a USB branch clock does not come on` | GCC refuses (a vote elsewhere, a parent off): the `GCC branch ... stays off (...)` line names it; the boot goes on without the console |
| `cold c2 FAILED: the core does not answer` | the core is unclocked behind a branch that reads on, or the block reset did not release it |
| `cold c2 FAILED: the xHCI does not halt within 100 ms ...` | an xHCI the boot path left running does not stop: by design nothing more is written (no block reset under a running DMA master); the takeover's retries try again 2 s later; report the `cold c2 role:` line's USBSTS |
| `cold c2 role: ... soft switch NOT proven: <check>` then `block reset the soft switch is not shown ...` | not a failure: the programming model's switch left something of the host role (the FIFO line names the first FIFO that fails), so the block reset was taken |
| `FAILED at takeover: the device role` | the takeover's gate (`ensureDevice`) could not put the core in the device role: its `takeover: role:` line says why; no device register was touched |
| `role rule: N core accesses refused (the first at 0x...)` | some path reached a device register outside the device role (or the xHCI outside the host role) and the kernel's `coreRd`/`coreWr` refused it: a bug to report with the offset |
| `soft reset refused` / `device init refused` / `run refused: GCTL ... is not the device role` | the gadget's own guard: the role changed behind the takeover (a hand-off?) |
| `cold c3 rail ...: NOT done` / `after ...: OFF` | the DRV-2 lock was held (the `rsc:` line names the holder), cmd-db has no such name, or the VRM refused: the repeater and the PHY then show it too |
| `cold c4 repeater: its APID belongs to another execution environment` | the repeater cannot be written from EE 0: the inherited tuning and enable stay; report the line |
| `cold c4 ...: NOT ready within 10 ms` | the repeater's supplies (c3) or its enable: compare the before/after bytes |
| `cold c5 PHY ...: NOT ready` | the init's readback differs from its end state; stage 16 (soft reset) will tell whether its UTMI clock runs |
| `FAILED at takeover: core soft reset` after a cold start | no UTMI clock from the PHY: c3 (l2d) and c5 are the suspects |
| `cold: link watch: still no host after 3 cold starts` | the controller side is up; the path to the Mac is not: the DIO4485 (not touched), the cable, the Mac's accessory approval |

### 17.7 Shared resources, power, sources

- **Locks**: the GCC vote word `0x52020` (only GPLL0's bit, only ever set) under PianoA830's `sm8750_gcc_vote_owner`, and apps
  DRV-2 under `sm8750_rpmh_drv2_owner`, both with this kext's tag `DWC3`. These are the words PianoTouch, PianoKeyboard, PianoWifi
  and PianoBluetooth already take, and no new lock word was added. PianoA830 is now a library of `PianoDwc3.json` (it already linked
  first). The three VRM resources are USB's alone (piano's DT has no other consumer). The repeater's SPMI writes need EE 0's APID.
- **Power**: the rails' votes are never withdrawn (PianoWifi's rule). The three LDOs stay on while the boot runs, as they are after
  ABL fastboot. A boot without the console (`piano-usb=0`) runs none of this.
- **Sources** (nothing copied): the register facts and the order of the steps come from the Ubuntu piano kernel's
  `phy-qcom-eusb2-repeater.c` (registers, init order, device/host mode), `phy-qcom-m31-eusb2.c` (bit names; rails, then repeater,
  then reference clock, then reset, then tables), `dwc3-qcom.c` (the glue's block reset at probe), the dwc3 `core.c` (the role
  switch's order: the core soft reset where needed, PRTCAPDIR, then the device soft reset; which cores need GCTL.CORESOFTRESET),
  `gdsc.c` and `clk-rcg2.c` (bits)
  and `gcc-sm8750.c` (offsets, GPLL0's vote, the GDSC's waits). All of these are GPL-2.0 and were read as the specification only.
  The tables are this repository's: `kM31Init` (Project-SunUEFI's stock sequence, BSD-2-Clause-Patent, `UPSTREAM`), the repeater
  tuning and the rails from piano's DT (board facts, the same on every unit). Reused from this repository: PianoBluetooth's
  `BtRpmh.h`, PianoKeyboard's `CmdDb.h`, PianoPower's `ChargerPmic.h`/`SpmiObserver.h` (`UPSTREAM`).

### 17.8 The role rule (C6, 2026-11-12)

C6 (`piano-usb-cold-sim=31`) stopped the whole system at stage 19, with every c-step reported ready. The cause and the evidence are in
forensics §7.6. In short: c2 left the core in the host role, the takeover soft-reset it in the host role, and only afterwards did
the gadget's init write PRTCAPDIR = device. The device side then started with the host role's FIFO sizes, and its first DMA (EP0's
STARTTRANSFER, RUN_STOP) never completed for anyone. The DWC3 programming model's order is the other way round: halt the xHCI,
soft-reset the core where it needs it, set PRTCAPDIR = device, then DCTL.CSFTRST in the device role, then the device init.

What holds now:

| Where | Rule |
|---|---|
| `UsbCold.h` `toDevice()` (c2, and the takeover's gate) | in the host role: the xHCI halted first (100 ms); if it does not report halted, nothing more is written and the step fails. Then the switch in the programming model's order, counted only when shown (PRTCAPDIR and CURMOD device, CSFTRST cleared, DEVCTRLHLT, TX FIFOs 0-6 a device layout); otherwise the block reset. Also the block reset for an xHCI that does not answer, a role that is neither host nor device, a GDSC that c1 found collapsed, and steps bit 0x8. GCTL must read the device role at the end |
| `UsbCold.h` `ensureDevice()` | the takeover's gate before its first device register, in every mode (`piano-usb-cold=0`, a hand-off's reclaim): `toDevice()` when the role is not device; else `FAILED at takeover: the device role` |
| `UsbCold.h` `inspect()`, `simulate()`, c2 | device registers (DCFG..the endpoint commands, `dwc3::isDeviceReg`) read only in the device role; the xHCI's only in the host role |
| `Dwc3Gadget.h` | `halt`, `softReset`, `init`/`initDevice` and `run` refuse outside the device role (nothing touched); `initDevice` no longer writes PRTCAPDIR |
| `PianoDwc3.cpp` `coreRd`/`coreWr` | every core access but the interrupt's GEVNTSIZ/GEVNTCOUNT: the role remembered from each GCTL access (unknown after a hand-off's release); a device register outside the device role, or an xHCI register outside the host role, is refused (a read answers 0) and counted (`role rule: N core accesses refused`, `usb`'s `role:` line). The monitor's `peek`/`poke` of physical addresses are not guarded (a deliberate debug tool; writes need `piano-usb-poke=1`) |
| `tests/usb_cold_host.cpp` | the model flags any device register outside the device role, an xHCI register outside the host role, a role change under a running xHCI, a block reset of a running controller, GCTL.CORESOFTRESET with the PHY suspended, and RUN_STOP with the host role's FIFO sizes. sim 4, sim 31 and host boot paths, against each unknown FIFO behaviour, end in the device role, and the gadget replays the takeover to RUN_STOP. C6's order is flagged |

The DWC3 facts these rest on (Dwc3Regs.h, UsbCold.h):
- GCTL.PRTCAPDIR (13:12): 1 host, 2 device, 3 OTG.
- GSTS.CURMOD (1:0): 0 device, 1 host.
- GTXFIFOSIZn: the start in 31:16, the depth in 14:0, TXFRAMNUM in bit 15 (DWC_usb31/32). Piano's device sizes are C7's; the host
  role's are C6's.
- The xHCI: USBCMD.RS (bit 0) and INTE (bit 2) at CAPLENGTH. USBSTS.HCHalted (bit 0) is 1 by default and 0 while it executes.
- piano's reset value after `USB30_PRIM_BCR` is GCTL `00102004`, the device role (C7).

## 18. The console stream on long, busy runs (desk1, 2026-11-12)

**Symptom.** desk1 was a GPU desktop image at the login window, 1200 s. Its USB capture
(`logs/usb-console/20261112-081349-port0.txt`, 100 k lines) holds this boot's first 5-10 k lines 12 times, and never the live tail.
The mailbox dump of the same boot (`logs/native-runs/20261112-081349-loop/`) shows the run went on normally.

**What the capture shows.**
- `usb-console.py` connected once and disconnected once, at the planned reset.
- The 12 copies of `port 0 opened` and `bus reset (DSTS 000e0001)` are copies of the one original line. The mailbox's last 667 KB
  hold no bus reset and no open.
- Each copy is preceded by lines of decayed bytes, then this boot's byte 0 ("Darwin Kernel Version"). Each arrives within 0.1-0.4 s.
- All 12 fall between +90 and +180 s, the boot-to-login burst. Afterwards about 4 lines/s arrived until the end, all of them old:
  at +300 s the stream showed lines from 95 s of uptime.

So it was neither a reconnect nor a replay on open. The reader of the log ring ran a whole lap: past the write position, through the
region this boot had not written yet (the previous boot's decayed bytes), and on into byte 0. After that it stayed behind.

**The cause.** k27 0055's mirror writer (`console_pstore_put`, before `console_write`'s lock, on every CPU) is lock-free. It reserves
its bytes with an atomic add, copies them (11 copies), then stores the header's `start = end % cap` from its own end. Two writers
that finish out of order therefore move `start` back by up to one write: two CPUs at once, or an interrupt inside a write.

The old `logPump` took a step back of b bytes as a step forward of cap - b, and re-read a lap. When the reader was behind at that
moment (the host slower than the boot's burst), its clamp of `avail` at the ring's size broke `rdPos + avail == start`. From then on
it read the right number of bytes from the wrong place, roughly a ring behind, at the log's own rate.

`tests/log_cursor_host.cpp` reproduces both halves with a replica of the old reader against a model of the racing writers.

**The fix** (`package/PianoDwc3/LogCursor.h`, PianoDwc3.cpp's `logPump`, `consoleClosed`, `lineState`):
- **Absolute positions.** The writer's position only moves forward. A step back of at most 64 KiB (a quarter of a small ring) is a
  late writer's, counted (`usb`: `header steps back ignored`) and ignored. The USB thread syncs every pass, open or not, and once a
  second through `piano-usb-delay`.
- **A settle time.** A byte is sent 1 ms after the header first covered it. A late writer's copy is done by then, so no stale bytes
  are sent.
- **An overtaken reader** (the host stalled for more than a ring) skips to the oldest retained byte, 16 KiB from the writer. It says
  so on the console: `[PianoDwc3: N bytes of the log were overwritten before the console could send them]`.
- **Sessions** (`piano-usb-replay`, §6). The boot's first open replays the retained log. A later open (the host closed the port, a
  bus reset, a cold restart) resumes where the last session stopped: the banner says `session N: the log resumes where the last
  session stopped (M bytes ...)`. Behind the stop point it resends what had not reached the host: the bytes still queued for port 0,
  and port 0's bulk IN if it was in flight or ended unfinished (`Gadget::takeInUnconfirmed`). A few lines may repeat; none is
  skipped. `piano-usb-replay=2` keeps the old every-open replay. The monitor's `log` replays everything retained, `live on` goes
  on from now.
- **DSTS noise.** The once-a-second DSTS line ignores COREIDLE now. COREIDLE followed the console's own traffic, so the line came
  every second while streaming: about 5000 of them in desk1's 20 minutes, each one more traffic.

Not changed: the kernel side. Its header can still step back, and a reader that copies the ring while a write is in flight still
sees the old bytes for that write. The reader above tolerates both. The other mailbox reader (`tooling/native/xnu-log.py`) takes the
header after the reset, when no writer runs. A kernel fix (the header stored only by the writer that completes the furthest end)
would be a separate k27 patch.

The capture itself starts mid-line (`de000 (65M)`, about 1 KB into the log). The tool's first read came after the Mac's driver had
received the open's first bytes. That is host side and harmless now: a later open resumes, and `log` replays on request.

**Re-test.** Build any GPU desktop image (desk1's arguments) from this tree, then run `python3 tooling/native/usb-console.py --capture
1300` for the whole run. Expect:
- one replay at the start;
- then the live tail up to `piano-reset-after: 1200 s reached`, with no second `Darwin Kernel Version`;
- `usb` from the monitor: `header steps back ignored` above 0 on a busy boot, `lost 0` while the Mac keeps reading.

To check the resume, unplug and replug the cable once: the banner says `session 2: the log resumes ...`, and the capture goes on
without a second replay.
