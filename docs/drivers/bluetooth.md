# Bluetooth: the WCN7860's Bluetooth over QUP2 SE6 ("UART14"): PianoBluetooth

Status: design + level 1 code, 2026-11-11 (backlog B9). `package/PianoBluetooth` carries level 1 (a read-only survey) as a kext, built
and linked into the native KC (20 native kexts, PianoBluetooth 326 pointers, inert without `piano-bt`), **not run**; and, host-tested
only (`tests/bt_host.cpp`), the portable code of levels 3 and 4: the GENI UART plans, H4 framing and the QCA vendor codec with the
whole controller set-up sequence. The macOS side is the hard half: §5 is its verdict. Template: `README.md`.

## 1. Purpose and gate

Keyboards, mice and headphones without a cable (Ubuntu: "pairing, file transfer and audio work"). Gates nothing in M1–M5; daily use,
after B7/B10 in the backlog order (`../BACKLOG-hardware.md` §3). The chip half (power, UART, firmware, HCI) is well specified by
Ubuntu and has a hardware-verified UniXNU precedent on SM8550 (§4); the macOS half has no kernel transport to plug into (§5).

## 2. Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb` (Ubuntu's running DT); KDTS = the Ubuntu kernel's `sm8750.dtsi` / `sm8750-xiaomi-piano.dts`
(piano/dev 7f8d6bed); GCC/RCG/PINS/PGPIO = its `gcc-sm8750.c`, `clk-rcg2.c`, `pinctrl-sm8750.c`, `pinctrl-spmi-gpio.c`; FW =
`xiaomipad-8pro-mainline/firmware/piano-bluetooth-rom20.json`; SHENG = UniXNU-sheng `porting.toml` / reports (SM8550, measured).
Every value is in `package/PianoBluetooth/BtHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Serial engine | QUP2 (`geniqup@8c0000`, wrapper version at +4) SE6 = `serial@898000` (`qcom,geni-uart`, 0x4000), "UART14", GIC SPI 461 level-high (INTID 493); no `dmas` (Linux runs it in SE DMA mode on the wrapper's stream) | DT | verified |
| Child | `bluetooth` (`qcom,wcn7860-bt`): six supplies, all PMU outputs (aon, btcx, btcmx, rfa 0.9/1.3/1.9 V); `max-speed` 3200000; no `enable-gpios`, so Linux powers it through the PMU's sequencer | DT | verified |
| Pins | gpio24 CTS, gpio25 RTS (`qup-uart14-cts-rts`, pull-down), gpio26 TX, gpio27 RX (`qup-uart14-default`, pull-up), all function 1 = `qup2_se6`, 2 mA; at hand-over: needs-boot | DT, KDTS comments, PINS | verified (DT) |
| Flow control | 4-wire, hardware RTS/CTS both ways (KDTS: "TX/RX plus CTS/RTS for hardware flow control"); the engine reports RX (`SE_GENI_IOS` bit 0) and CTS (bit 1, low = asserted) and drives RFR itself or by hand (`SE_UART_MANUAL_RFR`) | KDTS, `qcom_geni_serial.c` | verified (source) |
| Baud rates | 115200 at power-up; 3.2 Mbit/s operating (DT `max-speed`; "Baud index 0x11 (3.2 Mbaud) in every stock gngbtnv20 NVM") | DT, KDTS | verified |
| Serial clock | GCC vote word `0x52010`: bit 0 core, 1 s-ahb, 2 m-ahb, 3 core2x, 10 s6; CBCRs m-ahb `0x23298`, s-ahb `0x2329c` (both hardware-gated), core `0x232a0`, core2x `0x232b4`, s6 `0x1e76c`; source RCG `0x1e77c` (DFS-capable), parents TCXO / GPLL0 / GPLL7 / GPLL0-even. 115200 × 16 = 7.3728 MHz ÷ 4; 3.2 Mbit/s × 16 = the 51.2 MHz table entry ÷ 1 (SHENG: "51.2 MHz serial source and 16x oversampling") | GCC, RCG, SHENG | verified (source); state at hand-over: needs-boot (level 1) |
| QUP core clock | interconnect `qup-core` = BCM "QUP2", which Linux keeps alive (`keepalive`) | the kernel's `interconnect/qcom/sm8750.c` | inferred: on at hand-over (level 1's core/core2x lines) |
| SE protocol | the wrapper has no `firmware-name`, so the boot chain loads the UART protocol into SE6 (Linux would refuse otherwise); SHENG read revision 0x204 (protocol 2) on SM8550 | DT, `qcom-geni-se.c` | inferred; level 1/2 reads `GENI_FW_REVISION_RO` |
| SMMU | the wrapper's stream `0x423` (SE DMA); FIFO mode needs none | DT, SHENG (same on SM8550) | verified (DT); entry at hand-over: needs-boot |
| BT_EN | pm8550ve "f" (`pmic@5`) GPIO 3 = PPID `0x58a` (`gpio@8800`, page 0x88 + n − 1); stock state ("bt_uwb_en"): function normal, VIN1, no bias, output low. A PMIC GPIO: driving it is an **SPMI write** (MODE_CTL 0x40 output, DIG_OUT_SOURCE_CTL 0x44 bit 7 = level, EN_CTL 0x46 bit 7), possible only if its APID's owner is the apps EE (0) | DT, KDTS, PGPIO, k27 0057 (the SPMI write channel) | verified (DT); owner: needs-boot (level 1) |
| PMU | `wcn7860-pmu`, shared with Wi-Fi: six SoC rails in (vddio `l3f` 1.8, vddio1p2 `l2f` 1.2, vddaon `s5f`, vdddig `s4d`, vddrfa1p2 `s7i`, vddrfa1p8 `s3g`), WLAN_EN tlmm 16, BT_EN as above, SW_CTRL tlmm 18 | DT; `wifi.md` §2 | verified |
| Firmware | the stock **`bluetooth_a`** FAT image, directory `image/`: `gngbtfw20.mbn` (232436 B, the ROM 0x20 rampatch, an ELF), `gngbtnv20.bin` and 25 `gngbtnv20.bXX` (13399 B; b2e/b3e 13291), picked by board id. Not `modem_a` (that holds Wi-Fi's `image/peach/`); "bdwlan" is a Wi-Fi board-file name | FW | verified (names, sizes); the partition's GPT entry: needs-boot |
| Address | the controller reports its NVM's default address: Ubuntu sees it "unconfigured" and sets the unit's own (from the stock system, written by the installer) with MGMT Set Public Address = vendor `0xfc14`. The unit's address is private: never committed, never logged | `device/bluetooth/README.md`, btqca | verified (behaviour) |

### 2.1 One PMU, two radios

The PMU is one power unit with two enables. Linux (`pwrseq-qcom-wcn.c`, the wcn7860 entry) models it as one shared unit (the six rails,
reference-counted) under two targets: "bluetooth" = rails, then BT_EN high, then 50 ms; "wlan" = rails, then WLAN_EN high, then 50 ms;
no gap is required between the two enables on this part, and since K:f46f8386ed88 no RF clock is voted (Bluetooth works without it on
Ubuntu). So: **the rails are shared and must stay up while either radio runs; the enables are independent.** PianoWifi's level 2 already
votes the six rails (only ever adding, never lowering, the UFS PHY's LDOs untouched) and drives WLAN_EN; it never touches BT_EN and, by
default, never withdraws a rail (`piano-wifi-release=1` aside). What is missing is the contract between the two drivers: §7.1.

## 3. Ubuntu as the specification

Kernel `piano/dev` 7f8d6bed (`unixnu-research/ubuntu-ref/kernel`; v1.1.0 = d2710cd1), behaviour only (GPL-2.0, §11):
`drivers/bluetooth/hci_qca.c` (`qca_setup`, `qca_power_on`, `qca_set_speed`, `qca_set_baudrate`, the WCN7860-only `peri_set_baudrate`
from commit 490bebf5dd8f, IBS `send_hci_ibs_cmd`), `btqca.c` (`qca_read_soc_version`, `qca_uart_setup`, `qca_download_firmware`,
`qca_tlv_check_data`, `qca_read_fw_board_id`, `qca_get_nvm_name_by_board`, `qca_disable_soc_logging`, `qca_read_fw_build_info`,
`qca_check_bdaddr`, `qca_set_bdaddr`), `drivers/tty/serial/qcom_geni_serial.c` (`port_setup`, `set_termios`, `geni_serial_set_rate`,
`get_mctrl`/`set_mctrl`), `drivers/soc/qcom/qcom-geni-se.c` (`geni_se_init`, `geni_se_config_packing`, `geni_se_clk_freq_match`),
`drivers/power/sequencing/pwrseq-qcom-wcn.c`. The sequence, as `QcaCodec.h` re-expresses it:

1. Power: rails, BT_EN, 50 ms. Host UART 115200, 8N1, RTS/CTS.
2. EDL version (`0xfc00` [0x19]) → Command Complete `00 19 ..` with product, patch, ROM, SoC ids; ROM code = 0x20 for piano's part.
3. WCN7860 only: IBS WAKE_IND (0xfd), 10 ms, the raw 7-byte frame `31 00 f1 ff 02 02 <code>`, 100 ms.
4. Flow control off (RTS deasserted), vendor baud `0xfc48` [0x11], line drained, 1–10 ms, host to 3.2 Mbit/s, flow control on.
5. Rampatch `gngbtfw20.mbn` in 243-byte EDL segments (`0xfc00` [0x1e][len][data]); as an ELF only the last segment is acknowledged.
6. 10 ms; board id (`0xfc00` [0x23]); NVM `gngbtnv20.b<id>` (else `.bin`), its tags patched (17: in-band sleep bit, baud byte;
   27: deep sleep), every segment acknowledged.
7. SoC logging off (`0xfc17` [0x14 00]); HCI_Reset; build info (`0xfc00` [0x20]); Read BD_ADDR vs the NVM's default → the address.

Around sleep (`ubuntu-history/kernel.md` §2.4, K:7291c862688c, K:a2b4feb14264, K:386c1f901aa0): the UART is force-suspended in system
sleep (else its clocks and bandwidth stay on), and quiesced in this order: stop RX/TX, clear sequencer and DMA status, mask the SE IRQ,
wait for handlers; a level IRQ answered "not mine" once disabled the Bluetooth UART for good.

## 4. UniXNU templates, and what can be borrowed

| Piece | Where | Licence | Use |
|---|---|---|---|
| The whole path on SM8550 + WCN7850, same SE6 at 0x898000, INTID 493, stream 0x423: clocks, pins, BT_EN, SE DMA through a bypass bank, RX wake, WcnPower's shared rails; on iPadOS 27 pairing, AirPods, A2DP, Wi-Fi coexistence measured | UniXNU-sheng `package/BluetoothProbe.cpp`, `WcnPower.h`, `porting.toml` | BSD-2-Clause-Patent | design and facts (UPSTREAM); its pins and TLMM BT_EN are not piano's |
| Linux btqca/GENI/H4/IBS compiled byte-identical into XNU; the UART.SK (iPadOS) and `/dev/cu.BLTH` (macOS) front ends; version-locked bluetoothd patches | `unixnu-research/QcaBluetooth` | GPL-2.0-only core, APSL transport, BSD front end and patchers | **not used**: GPL core = specification only here; the patchers are binary analysis of Apple's daemon (§5.3 A) |
| Kext shape, register-model tests, SPMI observer, read-only SMMU scan | PianoUsbHost, PianoPower `SpmiObserver.h`, PianoWifi `WifiSurvey.h` | BSD (this repo) | level 1 |
| Firmware read in place from a FAT partition through NativeUfs | PianoWifi `WifiFirmware.c` + `FatVolume.c` | BSD (this repo) | level 4, with the partition `bluetooth_a`, directory `image/` |
| SPMI write channel (owner EE 0 checked, read back) | k27 0057 (`patches/k27/0057-…`) | this repo | level 3's BT_EN |
| IOHIDDevice publication | PianoKeyboard, PianoTouch | BSD (this repo) | §5.3 C |

## 5. IOKit family / ABI: the macOS side

### 5.1 What the macOS 27 KC has (vma2 26A434: Info.plists and class-name strings only)

- `com.apple.iokit.IOBluetoothFamily` 9.0.0: personalities `IOBluetoothHCIController` (IOClass on `IOResources`, a singleton) and two
  `AppleUSBHostMergeProperties` ones that put `UsbUserClientEntitlementRequired = com.apple.bluetooth.control` on Apple's own USB
  Bluetooth devices (idVendor 0x05ac, a product list) and a Mac Pro 6,1 Broadcom hub. Its classes: `IOBluetoothHCIController`,
  `IOBluetoothHCIUserClient`, `IOBluetoothDevice`, `IOBluetoothL2CAPChannel`, `IOBluetoothHIDChannel`, `IOBluetoothServiceNub`, memory,
  timer and work-loop helpers. **There is no host-controller transport class**: no `IOBluetoothHostControllerTransport`,
  `…UARTTransport`, `…USBTransport`, no `AppleBluetoothHostControllerUART` (neither a personality nor a class-name string). The kernel
  transport layer of older macOS is gone; the HCI transport lives in `bluetoothd`.
- What feeds bluetoothd on Apple's own hardware: `AppleConvergedPCI` (IOPCIMatch Broadcom 14e4:5fa0/5f69/5f31/5f71/5f72/5f83) +
  `AppleConvergedIPCOLYBTControl` (Skywalk) for Apple-silicon Macs; `AppleBluetoothModule` (IONameMatch `bluetooth` on
  `AppleARMIODevice`, and on `AppleOnboardSerialSync` for UART parts) + `AppleOnboardSerial` (`…SkywalkInterface`, `…BSDClient`), whose
  `AppleOnboardSerialSync` comes from `AppleSamsungSerial` (IONameMatch `uart-1,samsung`: Apple's UART hardware); IOUSBHost user
  clients for USB controllers. HID devices: `IOBluetoothHIDDriver`, `AppleBluetoothHIDKeyboard/Mouse/Multitouch` on
  `IOBluetoothL2CAPChannel`, i.e. fed by bluetoothd too.

### 5.2 What bluetoothd does natively today (measured: its stderr in the console of the 2026-11-11 native runs)

It starts (`MobileBluetooth-2700.51`), finds no `BT_DEVICE_ADDRESS`, reports `marconi-bt: Unable to match bluetooth service in device
tree` and `bluetooth: Unable to match bluetooth service in device tree`, `Not a valid MAC address (all 00s)` for both, and
`FindLocalMacAddress did not find bootargs for random address generation`; then nothing more. So its entry point is a device-tree
service named `bluetooth` with a MAC: the contract UniXNU-sheng's board meets (ADT `bluetooth` node, `device_type` bluetooth,
`transport-encoding` 1, `transport-speed` 3200000, a matching IOService with `local-mac-address`).

### 5.3 Options

| | Option | Private ABI | Effort | Main risks |
|---|---|---|---|---|
| A0 | **Probe Apple's stack, no patching**: the ADT `bluetooth` node + IOService (transport properties, `local-mac-address` from the private address) and a BSD serial device carrying H4 to the controller that PianoBluetooth has set up (the `/dev/cu.BLTH` model, written fresh); boot the stock daemon with `vmm-present` 1 and 0 and read what it opens and sends (its log and the bytes on our device; observation, no binary analysis) | none | ~1 week after level 4 | the daemon assumes a Broadcom chip (sheng's macOS 27.3 run stopped at a Broadcom vendor command) and has a virtual-machine check (QcaBluetooth patches one; our identity is VirtualMac2,1) |
| A1 | A0 + our serial device answering the vendor commands the daemon was **observed** to send (Broadcom-style), passing standard HCI through | none, but a moving target per macOS release | 2–4 weeks, only if A0 shows a short, fixed list | firmware-load and chipset paths may go beyond a few commands; Settings/pairing unproven anywhere on macOS |
| A | QcaBluetooth's route: a version-locked patch of bluetoothd (controller type 0, VM check, chipset default) for our 26A434 build | Apple daemon internals | 3–6 weeks | **binary analysis of an Apple daemon: owner decision**, outside this page's rules; its 26C5035r adapter does not apply to 26A434; unconfirmed on macOS even on sheng; pairing keys need the keychain (AppleKeyStore, which native boots do not have) |
| B | A USB-HCI shim: present the controller as a USB Bluetooth device | AppleUSBHostController (virtual host controller) or AppleUSBUserHCI's user-client protocol, plus IOUSBHostFamily natively (`usb-host.md` §5.2: links only with ten stand-ins) | 4–8 weeks | more private ABI than A and the same daemon gate (public reports: macOS 12+ needs bluetoothd patched for third-party USB controllers, e.g. BlueToolFixup): **not easier** — rejected |
| C | **Our own HID host, no bluetoothd**: HCI host + L2CAP + SMP (LE Secure Connections) + ATT/GATT + HID-over-GATT, then BR/EDR (SSP, HID on L2CAP PSM 0x11/0x13), publishing `IOHIDDevice`s (PianoKeyboard's proven shape); pairing by a control socket + `pianobt` CLI; link keys under `/private/var` (mode 600) | none | keyboards/mice 6–10 weeks (LE first); A2DP source (AVDTP + SBC + an IOAudio2 output) +6–8 weeks | no Bluetooth menu, AirPods features, Continuity or Bluetooth File Exchange; interoperability and pairing-security work is ours; fresh code (BTstack's licence and BlueZ's GPL rule them out; Apache-2.0 stacks like Zephyr's host are candidates) |

### 5.4 Verdict

The chip side is a two-to-three-week job on known ground (levels 1–5, §8). The macOS side has no supported kernel transport in macOS 27:
**do A0 first** (cheap, licence-clean, and the only way to learn what the stock daemon accepts without analysing it); if it stops at
chipset/VM gates, as is likely, choose between **C** (licence-clean, keyboards and mice in about two months, headphones later, no Apple
UI) and **A** (the full Apple experience, by owner decision only, like `wifi.md`'s IO80211 option). B is not a shortcut. My
recommendation: C for input devices, A kept open for audio if the owner accepts binary patching of bluetoothd.

## 6. Boot args

| Boot arg | Default | Effect |
|---|---|---|
| `piano-bt=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey (higher values run level 1 until the levels exist) |
| `piano-bt-delay=S` | 0 | wait S seconds first |
| `piano-bt-se=0` | 1 | skip SE6 even when clocked |
| `piano-bt-smmu=0` | 1 | skip the SMMU scan |
| `piano-bt-hwcg=1` | 0 | count a hardware-gated AHB branch with its vote bit set as running (PianoWifi's rule) |
| `piano-bt-spmi=0\|1` | 1; 0 when `piano-wifi-spmi=1` or `piano-wifi>=2` | BT_EN over the observer channel (one reader per channel: PianoWifi reads the same peripheral then) |
| later: `piano-bt-baud=115200`, `piano-bt-ibs=0\|1`, `piano-bt-peri=0`, `piano-bt-stop=N` | 3200000, 0, 1, 0 | level 4: stay at 115200; the NVM's in-band sleep bits; skip the WCN7860 frame; stop after step N |

## 7. Build, kernel collection

`tooling/k27/build-native-piano-bluetooth.sh` (host tests, a store check of the survey headers, the objects, the import check: 0
unresolved), descriptor `tooling/k27/kext/PianoBluetooth.json` (`com.unixnu.driver.PianoBluetooth`), listed in
`configs/k27-native-kexts.txt`, so `tooling/k27/build-native-image.sh` links it (inert). Link check 2026-11-11 (kernel 0059): "20 native
kexts linked, none from outside this repository", PianoBluetooth 326 pointers, every vtable check 0 problem slots. No library dependency yet; level 3
adds PianoWifi (§7.1) and PianoA830 (the GCC vote lock) to the descriptor, level 4 NativeUfs.

### 7.1 The shared power helper PianoWifi should expose (not built; PianoWifi is not changed here)

PianoWifi owns the six rails (its level 2 steps 201–203) and WLAN_EN; PianoBluetooth owns BT_EN and the UART. PianoWifi should export,
as PianoA830 exports its lock words (a library in PianoBluetooth's descriptor):

```c
enum { PIANO_WCN_WLAN = 1u << 0, PIANO_WCN_BT = 1u << 1 };
/* Brings the six PMU rails up for `client`: level 2's steps 201..203 (PMIC read, RPMh mode/voltage/enable under the DRV-2 lock, read
 * back, the two PHY LDOs unchanged). Idempotent per client; the first one does the work, a later one adds its bit after checking
 * that the rails still read enabled. Touches no pin (WLAN_EN, PERST#, CLKREQ#, BT_EN stay the callers'). 0, or a negative
 * pianowifi::Error (kErrRailRead .. kErrRailDropped). Thread context; may sleep. */
int piano_wcn_rails_get(unsigned client);
/* Drops `client`'s bit; returns the remaining ones. The rails stay voted while any bit is set, and after the last one too unless
 * piano-wifi-release=1, which then withdraws only the enables PianoWifi itself added (safeOff's rule). */
unsigned piano_wcn_rails_put(unsigned client);
/* The owner bits; bit 31: the rails are up. Any context. */
unsigned piano_wcn_rails_owners(void);
/* Serialises rail transactions and enable-pin changes between the two drivers: take(1) / release(0), 0 = taken, bounded wait
 * (the weak lock-word protocol of sm8750_gcc_vote_owner, tag "WCN1"/"BTH1"). */
int piano_wcn_pmu_lock(int take);
```

Rules for both sides: rails before any enable, 50 ms after an enable before talking to that radio, an enable never lowered by the other
driver, SW_CTRL readable by both (no owner). PianoWifi's own level 2 becomes `piano_wcn_rails_get(PIANO_WCN_WLAN)` + its pins.
Bluetooth-only start needs no PCIe vote (SHENG).

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop; each level only after the previous one's log.

| Level | What | Writes | Pass | Effort |
|---|---|---|---|---|
| 1 (built) | the survey (§8.1) | none (observer read commands) | the log; nothing written | done; 1 boot |
| 2 clocks | vote `0x52010` bits 0–3, 10 under the GCC vote lock; the SE6 rate (a DFS level if the boot chain programmed DFS, else the RCG's 7.3728 MHz entry); read the SE | GCC vote bits, possibly the RCG | branches running; `protocol 2 (UART)`; FIFO interface on; QUP version | 0.5 day; 1 boot |
| 3 power + UART | `piano_wcn_rails_get(BT)`; pins 24–27 to `qup2_se6` with the DT's pulls; the UART plan at 115200 (`GeniUart.h` `planUart`), FIFO mode, polled; **loopback self test** (TX→RX, RTS→CTS inside the engine) with BT_EN still low; then BT_EN high over the SPMI write channel (owner EE 0, read back), 50 ms | rails (shared), 4 TLMM words, ~32 SE words, 3 PMIC registers | loopback echoes; BT_EN reads high; SW_CTRL; the RX line idles high | 2–3 days + PianoWifi's helper; 1–2 boots |
| 4 HCI set-up | `qcaSetup` (`QcaCodec.h`): version, WCN7860 frame, 3.2 Mbit/s (the version again), rampatch and NVM from `bluetooth_a` at run time, logging off, HCI_Reset, build info, local version (manufacturer 0x001d), the address (the private one, written, never logged) | H4 traffic only | `HCI_Reset` complete; build label; the address check | 2–3 days; 2–4 boots (`piano-bt-baud=115200` first if the speed change fails) |
| 5 HCI smoke | LE extended scan / inquiry for a few seconds | H4 | a count of advertisers (no addresses logged) | 2 days; 1 boot |
| 6 interrupts, sleep | SPI 461 via `armcpu_spi_attach` (the handler always clears M/S status: Ubuntu's lesson), IBS (`piano-bt-ibs=1`), quiesce for sleep | – | no storm; idle power | 1 week |
| 7 macOS | §5.4: A0, then C (or A by decision) | – | a keyboard types | §5.3 |

### 8.1 Level 1 test image

```sh
tooling/k27/build-native-image.sh            # the native KC (PianoBluetooth linked, inert) and the regular images
PIANO_EXTRA_ARGS="piano-usb=2 piano-bt=1" PIANO_RESET_AFTER=300 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)" \
  --out out/ram-bt-l1
python3 tooling/bootimg/verify-ram-images.py out/ram-bt-l1/build-A-bundled.json   # PASS; the image: out/ram-bt-l1/boot-piano-macos-ram-A-bundled.img
```

Read the lines live (`tooling/native/usb-console.py`) or `grep PianoBluetooth` in the run's `xnu-console.txt`. Adding
`piano-wifi=1 piano-wifi-spmi=1` in the same boot also reports the six rails (PianoWifi), and PianoBluetooth then skips BT_EN unless
`piano-bt-spmi=1 piano-bt-delay=30`. Expected (the likely hand-over: nothing has used UART14 since reset):

```
PianoBluetooth: start: piano-bt=1 delay=0 se=1 smmu=1 hwcg=0 spmi=1 (WCN7860 Bluetooth on QUP2 SE6 0x898000 "UART14", SPI 461, BT_EN PPID 0x58a)
PianoBluetooth: level 1: loads only, nothing written (the SPMI observer's read command is the only store)
PianoBluetooth: codec self check: version request 01 00 fc 01 19, a canned reply decoded (rom 0x20, gngbtfw20.mbn, gngbtnv20.b2e): ok
PianoBluetooth: stage 1: GCC: the QUP2 wrapper's and SE6's clocks, the SE6 clock source (read only)
PianoBluetooth: GCC QUP2 vote 0x52010 ........: core . s-ahb . m-ahb . core2x . s6 .
PianoBluetooth: GCC branches: m-ahb ........ (..) s-ahb ........ (..) core ........ (..) core2x ........ (..) s6 8....... (off): SE6 NOT clocked (its registers are not read)
PianoBluetooth: GCC s6 rcg cmd ........ (root ..) cfg ........ m ........ n ........ d ........: source N, N Hz
PianoBluetooth: GCC s6 DFS off (cmd_dfsr ........): the clock select chooses from the RCG table (7.3728 .. 128 MHz)      (or: DFS on, level L: 8 rates)
PianoBluetooth: stage 2: TLMM: the UART's pins, the PMU's WLAN_EN and SW_CTRL (read only)
PianoBluetooth: pin gpio24 CTS: cfg ........ io ........: function F pull P N mA output ..., in I out O (as the DT's state | not the DT's state)   (6 lines: 24 25 26 27 16 18)
PianoBluetooth: stage 3: QUP2 wrapper and SE6 (read only, only when clocked; never the FIFO windows)
PianoBluetooth: SE6: not read (its clocks are off: level 2 votes them first)      (clocked: "QUP2 version 3....... (3.N); SE6 firmware revision 0000020N: protocol 2 (UART) ..." and four SE6 lines)
PianoBluetooth: stage 4: SMMU: the entries of the wrapper's stream 0x423 (read only)
PianoBluetooth: SMMU idr0 ........ idr1 60000053: N stream-match groups, 83 banks, 128 pages
PianoBluetooth: SMMU stream 0x423: no stream-match entry (SE DMA needs one; FIFO mode does not)      (or: group G smr ........ s2cr ........: bypass | translate ... | fault)
PianoBluetooth: stage 5: BT_EN over the SPMI observer channel (observer reads)
PianoBluetooth: BT_EN (pm8550ve-f gpio3, PPID 0x58a): APID A owner EE 0 (the apps processor: level 3 can drive it); type 10 subtype .. (LV/MV) status1 .. rt_sts ..
PianoBluetooth: BT_EN mode 01 (output) vin 01 pull 05 (none) dig_in .. out_src 00 (level 0, function 0) dig_out .. en 80 (enabled)
PianoBluetooth: summary: bt_en low owner apps | wlan_en 0 sw_ctrl 0 | se6 unclocked | uart pins not as dt | smmu 0x423 none | codec ok
PianoBluetooth: verdict: level 2 must vote SE6's clocks first; level 3 raises BT_EN after the shared rails
PianoBluetooth: stage 6: done
PianoBluetooth: done: N MMIO loads, 0 MMIO stores; M SPMI observer reads; P pages mapped, 0 map failures
```

What to read from it: whether SE6 and the wrapper are clocked (and so whether the boot chain left QUP2 running), the RCG's DFS table
(level 2 then only selects a level), the pins' function at hand-over, BT_EN's subtype (LV/MV: the level is bit 7 of 0x44), its level
and **its SPMI owner** (EE 0 or level 3's BT_EN plan is void), the SMMU entry for 0x423 (only SE DMA needs one). A hang after "stage 3"
would mean a gate let a load through to an unclocked block: rerun with `piano-bt-se=0`. `FAILED`/`not read` lines name their cause.

## 9. Fallback

`piano-bt=0` (the default): nothing runs. USB keyboards and mice (B7) or the keyboard cover meanwhile; the controller stays off
(BT_EN low), the shared rails as PianoWifi leaves them.

## 10. Open questions and risks

1. BT_EN's SPMI owner (level 1). If it is not EE 0, the apps processor cannot drive it, and Linux's working BT_EN would mean a
   different arbiter path (a question for the SPMI docs, then the owner). (needs-boot)
2. SE6's clocks and protocol at hand-over; whether the boot chain programs the RCG's DFS table (level 1/2). If SE6 reads protocol
   0xff, the UART firmware is not loaded and the SE protocol loader (Linux's `geni_load_se_firmware`, with a firmware file) becomes a
   level of its own. (needs-boot)
3. The `bluetooth_a` GPT entry and FAT layout as NativeUfs sees them (level 4 lists them); the board id this unit's controller
   reports (selects `gngbtnv20.bXX`). (needs-boot)
4. The address source natively: the private copy (`private/`, never committed) into the locally built ADT's `bluetooth` node
   (`local-mac-address`, which bluetoothd reads too), never printed by any tool; the stock partition that holds it is not known.
5. The WCN7860 peripheral baud frame (`31 00 f1 ff 02 02 code`) is Ubuntu-only (not mainline); `piano-bt-peri=0` tests without it.
6. Polled FIFO at 3.2 Mbit/s: hardware flow control makes it lossless, but the CPU cost is high; SE DMA (stream 0x423 through a
   bypass bank, sheng's 32 KiB) or the IRQ path (level 6) before audio.
7. Sleep: the UART's clocks and the BT_EN/IBS state across s2idle (Ubuntu's §3 lessons); keep `sleep-disabled` until level 6.
8. Pairing-key storage: Apple's stack wants the keychain (AppleKeyStore, absent natively); option C keeps keys in its own file.
9. Coexistence: both radios' firmware runs on the one chip; Ubuntu runs them together. Heat and power: sessions stay attended.

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (`hci_qca`, `btqca`, `qcom_geni_serial`, `qcom-geni-se`, `pwrseq-qcom-wcn`,
the clock and pinctrl drivers) and QcaBluetooth's GPL core are the specification only: opcodes, reply layouts, file names, tag
numbers, register offsets and the order of steps are facts; no code, comment or table is copied (`package/PianoBluetooth/UPSTREAM`).
UniXNU-sheng (BSD) is used for facts and design with attribution. No Apple binary is analysed beyond the KC's kext names, class names
and Info.plist matching dictionaries, and bluetoothd's own log lines; option A would change that and is the owner's decision. No
Qualcomm binary is reverse engineered: the rampatch and NVM (`LicenseRef-Proprietary-Firmware`) are data, read at run time from the
tablet's own `bluetooth_a` (or a staged copy under the ignored `out/`), never committed. The Bluetooth address is read at run time from
a private source and never logged or committed.
