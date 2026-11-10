# USB device-mode debug channel: PianoDwc3

> **Status 2026-11-11:** T1 (`piano-usb=1`) on hardware: USB core powered and clocked, DWC3 3.20a, eUSB2 PHY and repeater
> enabled, SMMU stream 0x40 on bank 3 (kept). T2 (`piano-usb=2`): takeover runs, the Mac enumerates the device at high speed and
> reads the device, string and configuration descriptors, then sends no SET_CONFIGURATION (being fixed). Build:
> `PIANO_EXTRA_ARGS="piano-usb=2" tooling/k27/build-native-image.sh` (PianoDwc3 is in `configs/k27-native-kexts.txt`, inert without
> `piano-usb`); run: `tooling/native/native-loop.sh`. `build-piano-dwc3.sh` is now `build-native-piano-dwc3.sh` (no archive fallback);
> `build-usb-test-kc.sh` and `native-run.sh` are gone.


Status: design and code, 2026-11-11. Built, host-tested and linked into a test KC (`out/k27/kc-k27-usb-test.bin`); **not
run on hardware**. Template: `README.md`. Code: `package/PianoDwc3`.

## 1. Purpose and gate

Today the only view into a native k27 boot is the pstore-console RAM mailbox, read back from Ubuntu after a reset
(`docs/bringup-debugging.md` §2). PianoDwc3 makes the tablet's single USB-C port, already cabled to the Mac, a live
channel while macOS runs: the DWC3 controller that the UEFI's debug fastboot used is taken over **in device mode**, and the
Mac sees a composite USB device with two CDC-ACM serial ports (no driver to install on the Mac):

| Port | Mac node | Tablet side |
|---|---|---|
| 0 "piano kernel console" | `/dev/cu.usbmodem…` | the console log streamed live; on open, the whole retained log (the pstore ring, ~650 KB) is replayed first; a line-based kernel monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `uptime`, `reset`, `panic`, `log`, `live`) |
| 1 "piano user pipe" | `/dev/cu.usbmodem…` | `/dev/pianousb` (`piano-usb-shell=1`): a byte pipe to user space for a root shell (`tooling/native/pianousb-shell`) or `log stream > /dev/pianousb` |

It gates nothing in M1–M5; it shortens every later loop (UFS, display, touch, GPU) from "reset, Ubuntu, read the mailbox"
to watching the log and asking the kernel questions while it runs.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Controller | `usb@a600000`, `qcom,sm8750-dwc3` + `qcom,snps-dwc3`, reg `0xa600000` + `0xfc100` (flattened qcom binding: QSCRATCH inside), `dr_mode = "otg"`, `maximum-speed = "super-speed"`, `usb-role-switch`, `dma-coherent`, quirks `snps,dis_u2_susphy_quirk`, `snps,dis_enblslpm_quirk`, `snps,usb2-gadget-lpm-disable`, `snps,dis-u1/u2-entry-quirk` | piano DT (`out/inputs/piano-boot.dtb`) | verified |
| Interrupts | `dwc_usb3` GIC SPI 133 level-high; `pwr_event` SPI 130, `hs_phy_irq` SPI 131; dp/dm/ss on the PDC (14/15/17) | DT | verified; **not used** (polled, `GEVNTSIZ.INTMASK`) |
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
4. **Own gadget + CDC-ECM/NCM + IOEthernetController** (next step, §11): the gadget core takes more functions; the
   IOEthernetController side follows `DroidVMEthernet` (already vtable-checked in this KC). Useful once user space has a
   network configuration and sshd; the ACM console stays the fallback.
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
   `IOEthernetController` on this gadget core? Decide after T2; the ECM path needs no private ABI.
7. Panic output over USB: a polled gadget path from the panic handler (the core already polls) — after T3.

## 12. Licence note

New code under BSD-2-Clause-Patent (`package/PianoDwc3/LICENSE`). Taken from BSD-2-Clause-Patent sources with attribution
(`UPSTREAM`): Project-SunUEFI (takeover order, quirks, M31 init values), QcomDwc3's `QcomDwc3.cpp` (SPMI read and GDSC
pattern), UniXNU-sm8750-common `SmmuTakeover.h` (bypass bank shape). Not used: QcomDwc3's GPL-2.0 PHY headers, any Linux code
(Linux files were read as the specification only).
