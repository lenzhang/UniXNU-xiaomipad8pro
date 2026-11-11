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


Status: design and code, 2026-11-11. Built, host-tested and linked into a test KC (`out/k27/kc-k27-usb-test.bin`); **not
run on hardware**. Template: `README.md`. Code: `package/PianoDwc3`.

## 1. Purpose and gate

Today the only view into a native k27 boot is the pstore-console RAM mailbox, read back from Ubuntu after a reset
(`docs/bringup-debugging.md` §2). PianoDwc3 makes the tablet's single USB-C port, already cabled to the Mac, a live
channel while macOS runs: the DWC3 controller that the UEFI's debug fastboot used is taken over **in device mode**, and the
Mac sees a composite USB device with two CDC-ACM serial ports (no driver to install on the Mac):

| Port | Mac node | Tablet side |
|---|---|---|
| 0 "piano kernel console" | `/dev/cu.usbmodem…` | the console log streamed live; on open, the whole retained log (the pstore ring, ~650 KB) is replayed first; a line-based kernel monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `uptime`, `reset`, `reset fastboot` (PMIC restart reason bootloader: ABL fastboot after this reset, also on an installed system), `panic`, `log`, `live`) |
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

From the KC's `__PRELINK_INFO` (`droidvm-build/macOS-DroidVM/out/kc/kc27.bin`, the base `tooling/kc/build-kc-k27.sh` links):

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
   IOEthernetController side follows PianoNullEthernet / `DroidVMEthernet` (vtable-checked in this KC). The ACM console stays
   the fallback.
5. A vendor-class bulk pipe: no advantage over ACM, needs a Mac tool.

### 4.3 What runs

```
IOPlatformExpertDevice ── PianoDwc3 (start, matching thread)   read-only probe -> [SMMU SID 0x40 -> bypass bank]
        threads:  usb (takeover, then a 1 ms poll loop: events -> EP0/bulk, log tail -> port 0 IN, port OUT -> monitor/pipe)
                  monitor (port 0 command lines)   notes (IOLog off the USB thread)   shell (/dev/pianousb after IOBSD)
```

- **Takeover** (`takeover()`, stages 10–20 in the log): clocks checked (or switched on, `piano-usb=3`) → GSNPSID → halt
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
| `piano-usb-replay=0` | 1 | port 0 starts at "now" instead of replaying the retained log |
| `piano-usb-phyread=0`, `piano-usb-spmi=0` | 1 | skip the M31 / repeater reads of the probe |
| `piano-usb-pid=N` | 1 (`1209:0001`, pid.codes test PID) | another USB product ID |
| `piano-usb-trace=N` | 400 | log the first N controller events after each bus reset one by one (raw word, EP0 phase, TRB words); 0 off |
| `piano-usb-hid=1` | 0 | (needs `piano-usb=2`) a keyboard and a pointer HID device for macOS and the third port that drives them (§12); without it the USB device is the two-port one, unchanged |
| `piano-usb-hid-hold=MS` | 3000 | a key or button still down this long after the last input command is released |
| `piano-usb-net=1` | 0 | (needs `piano-usb=2`) the CDC-NCM network function after the ports and its Ethernet interface on the tablet (§14); without it the USB device is exactly the ACM-only one |
| `piano-usb-net-fifo=0` | 1 | with `piano-usb-net=1`: keep the TX FIFO sizes the core reset left instead of the plan of §14.2 (a first-run experiment) |
| `piano-usb-idle=0` | 1 | the old USB loop (1 ms after every quiet pass) instead of the adaptive wait of §15 |
| `piano-usb-irq=1` | 0 | the core's interrupt (SPI 133) ends the USB thread's waits (§15); falls back to polling by itself |
| `piano-wake-log=S` | 60 | the `idle: N wakeups in S s` line every S seconds (0 none; shared with PianoInput and PianoTouch) |

On the Mac (no root, standard library only; logs go to `logs/usb-console/`, gitignored):

```sh
tooling/native/usb-console.py --list                 # is the gadget there, which /dev/cu.* is which port
tooling/native/usb-console.py                        # waits for the tablet, port 0 interactive; Ctrl-] quits
tooling/native/usb-console.py --capture 300 --send usb --send 'ioreg 6'   # unattended: 300 s saved, reconnects
tooling/native/usb-console.py --port 1               # the user shell (below)
screen /dev/cu.usbmodem<…> 115200                    # any terminal works too (DTR on open starts the stream)
```

User shell (development tablets only: an unauthenticated root shell): build `tooling/native/pianousb-shell/pianousb-shell.c`
on the Mac (`clang -O2 -o pianousb-shell pianousb-shell.c`, ad-hoc signed), put it at `/usr/local/libexec/pianousb-shell`
and `com.unixnu.pianousb-shell.plist` in `/Library/LaunchDaemons` of the root image, boot with `piano-usb-shell=1`. Without
the helper, `/dev/pianousb` still carries anything a LaunchDaemon writes to it (e.g. `log stream --style compact`).

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
`@OUT@` = `out/`) and falls back to macOS-DroidVM's builder where that script is not there yet. `build-usb-test-kc.sh`
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
  comes up (PHY/repeater in low power). Level 3 and `piano-usb-phy=1` cover clocks and the M31 PHY only; RPMh rail votes and
  the repeater are **not** implemented (the full cold path: SunUEFI `PianoUsbPhy.c` PowerStart + repeater, later).
- **SMMU access under Gunyah**: a read that never returns was seen on sheng from another thread; PianoDwc3 touches the SMMU
  only on the matching thread after NativeUfs. A hang there is caught by the watchdog; `piano-usb-smmu=0` skips it.
- **Core soft reset without PHY clocks**: CSFTRST times out (logged, stage −16); the UTMI select exists to avoid the PIPE
  clock dependency.
- **The monitor's `peek`** of an unclocked block hangs the CPU (as any such read); `poke` is off unless asked for.
- **No disconnect detection** (VBUS forced valid); unplug/replug shows as a bus reset — harmless.
- **Security**: port 1 + the helper is an unauthenticated root shell: development tablets, boot-arg gated, never by default.
- **Charging** is unaffected (the PMIC charger is not touched; the override is in the DWC3 glue only).

## 11. Open questions

1. What do piano-uefi's Qualcomm DXEs leave on at ExitBootServices (GDSC, clocks, M31, repeater, RUN_STOP, QSCRATCH)? → T1.
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
  `IOHIDDevice` subclass pattern of PianoInput/PianoKeyboard (DroidVMInput's, the one proven on this KC): same overrides, Built-In set,
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
PianoNullEthernet (DroidVMEthernet's pattern, BSD-2-Clause-Patent).

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
(`docs/native-droidvm-inventory.md`, PianoNullEthernet's comment), and PianoNullEthernet's descriptor gates IOTimeSyncRootService
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
- **Throughput**: one 16 KiB NTB in flight each way on a polled loop; fine for ssh and NTP, slow for large copies.
- **Security**: whatever the tablet serves on this link (sshd, a shell) is reachable from the Mac (and with Internet Sharing from
  nothing else: NAT); development tablets only, as the user pipe.

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
