# Bluetooth: the WCN7860's Bluetooth over QUP2 SE6 ("UART14"): PianoBluetooth

Status: levels 1–4 built, 2026-11-11 (backlog B9). `package/PianoBluetooth` is a native kext with level 1 (a read-only survey), level
2 (power: the PMU rails through the helper shared with PianoWifi, BT_EN over SPMI), level 3 (the GENI UART at 115200 with RTS/CTS)
and level 4 (the controller set-up: rampatch and NVM from the tablet's own `bluetooth_a`, 3.2 Mbit/s, the unit's address), all
**host-tested, linked into the native KC, not run**: inert without `piano-bt`, the radio powered only from `piano-bt=2`. The shared
rails helper is `package/include/PianoWcnRails.h` (§7.1, its words in PianoA830); the change PianoWifi makes to use it is §7.2. The
macOS side is the hard half: §5 is its verdict. The batched tablet test list is §8.4. Template: `README.md`.

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
Ubuntu). So: **the rails are shared and must stay up while either radio runs; the enables are independent.** PianoWifi's level 2 votes
the six rails (only ever adding, never lowering, the UFS PHY's LDOs untouched) and drives WLAN_EN; it never touches BT_EN and, by
default, never withdraws a rail (`piano-wifi-release=1` aside). The contract between the two drivers is the shared helper of §7.1
(built; PianoBluetooth uses it, PianoWifi after the change of §7.2).

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
8. The HCI core afterwards (`net/bluetooth/hci_sync.c`, `hci_dev_open_sync`/`hci_init1_sync`): the configured public address
   (`set_bdaddr` = `0xfc14` [6 bytes, most significant first], answered by a Command Complete), then HCI_Reset, then Read Local
   Features / Read Local Version / Read BD_ADDR.

Around sleep (`ubuntu-history/kernel.md` §2.4, K:7291c862688c, K:a2b4feb14264, K:386c1f901aa0): the UART is force-suspended in system
sleep (else its clocks and bandwidth stay on), and quiesced in this order: stop RX/TX, clear sequencer and DMA status, mask the SE IRQ,
wait for handlers; a level IRQ answered "not mine" once disabled the Bluetooth UART for good.

## 4. UniXNU templates, and what can be borrowed

| Piece | Where | Licence | Use |
|---|---|---|---|
| The whole path on SM8550 + WCN7850, same SE6 at 0x898000, INTID 493, stream 0x423: clocks, pins, BT_EN, SE DMA through a bypass bank, RX wake, WcnPower's shared rails; on iPadOS 27 pairing, AirPods, A2DP, Wi-Fi coexistence measured | UniXNU-sheng `package/BluetoothProbe.cpp`, `WcnPower.h`, `porting.toml` | BSD-2-Clause-Patent | design and facts (UPSTREAM); its pins and TLMM BT_EN are not piano's |
| Linux btqca/GENI/H4/IBS compiled byte-identical into XNU; the UART.SK (iPadOS) and `/dev/cu.BLTH` (macOS) front ends; version-locked bluetoothd patches | `unixnu-research/QcaBluetooth` | GPL-2.0-only core, APSL transport, BSD front end and patchers | **not used**: GPL core = specification only here; the patchers are binary analysis of Apple's daemon (§5.3 A) |
| Kext shape, register-model tests, SPMI observer, read-only SMMU scan | PianoUsbHost, PianoPower `SpmiObserver.h`, PianoWifi `WifiSurvey.h` | BSD (this repo) | level 1 |
| Firmware read in place from a FAT partition through NativeUfs | PianoWifi `WifiFirmware.c`, PianoPower `FatVolume.c` (compiled in, not copied) | BSD (this repo) | level 4 (`BtFirmware.c`): `bluetooth_a`, directory `image/` |
| SPMI write channel (owner EE 0 checked, read back) | k27 0057 (`patches/k27/0057-…`), PianoTorch `TorchCore.h` | this repo | level 2's BT_EN |
| cmd-db lookup, RSC layout, the VRM transaction | PianoKeyboard `CmdDb.h` (included), PianoWifi `vrmLocked` / PianoKeyboard `voteLocked` (re-expressed: `BtRpmh.h`) | BSD (this repo) | level 2's rails when Bluetooth comes first |
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
| `piano-bt=N` | 0 (probe() declines, nothing mapped) | 1 the read-only survey; 2 + power (rails, BT_EN); 3 + the UART; 4 + the controller set-up (5 and up run 4). Below 2 the radio is never powered |
| `piano-bt-delay=S` | 0; **30** at level ≥2 when `piano-wifi≥1` is in the same boot | wait S seconds first (PianoWifi's SPMI reads of the same regulators and of BT_EN must not overlap ours until it takes the PMU lock, §7.2) |
| `piano-bt-se=0` / `piano-bt-smmu=0` / `piano-bt-hwcg=1` | 1 / 1 / 0 | level 1 only: skip SE6 even when clocked / skip the SMMU / count a hardware-gated AHB branch with its vote bit as running (level 3 always does) |
| `piano-bt-spmi=0\|1` | 1; 0 at level 1 when `piano-wifi-spmi=1` or `piano-wifi≥2` | level 1's BT_EN reads (levels ≥2 read BT_EN regardless, under the PMU lock) |
| `piano-bt-baud=B` | 3200000 | the operating speed (a QCA baud code: 115200 stays at the initial speed; 921600, 1000000, 2000000, 3000000 are fallbacks) |
| `piano-bt-peri=0` | 1 | no WCN7860 peripheral baud frame (`31 00 f1 ff 02 02 code`, Ubuntu-only) before the vendor baud command |
| `piano-bt-ibs=1` | 0 | keep the NVM's in-band sleep bit (0: cleared; nothing serves IBS before level 6) |
| `piano-bt-reset0=0` | 1 | no HCI_Reset at 115200 before the version request (its answer is only noted) |
| `piano-bt-loop=0` | 1 | no internal loopback self test at level 3 |
| `piano-bt-release=0` | 1 | never withdraw a rail enable (on a failure, at sleep): the rails stay voted like PianoWifi's default. Even with 1 nothing is withdrawn while another owner runs, or while an unpatched PianoWifi (`piano-wifi≥2`, not an owner in the helper's word) does |
| `piano-bt-blind=1` | 0 | vote a rail whose PMIC state could not be read (PianoWifi's `piano-wifi-blind`) |
| `piano-bt-addr=0` | 1 | keep the NVM's default address even when the ADT has the unit's own |
| `piano-bt-stop=N` | 0 | return after step N (201..205, 301..306, 400..412); the state of that step stays (a sleep still takes it down) |

## 7. Build, kernel collection

`tooling/k27/build-native-piano-bluetooth.sh`: the host tests (`package/include/tests/run.sh`: the shared helper; `tests/bt_host.cpp`;
`tests/bt_bringup_host.cpp`), a store check of the survey headers (`GeniUart.h`, `BtHw.h`), a check that nothing writes WLAN_EN, the
objects (`PianoBluetooth.cpp`, `BtFirmware.c`, PianoPower's `FatVolume.c` compiled in, the glue), the import check (0 unresolved;
six imports from the libraries: `sm8750_rpmh_drv2_owner`, `sm8750_gcc_vote_owner`, `piano_wcn_rails_state`, `piano_wcn_pmu_owner`,
`native_ufs_part_find`/`_read`). Descriptor `tooling/k27/kext/PianoBluetooth.json` (`com.unixnu.driver.PianoBluetooth`) names
`com.unixnu.driver.PianoA830` (the lock words, the helper's words) and `com.unixnu.driver.NativeUfs` (the partition reader) as
libraries; both are listed before PianoBluetooth in `configs/k27-native-kexts.txt`, so `tooling/k27/build-native-image.sh` links it
(inert). Link check: §8.4's "build" row.

| File | What |
|---|---|
| `BtHw.h` | the facts, each with its source (now also the SE6 RCG table, BT_EN's write values, the owner channel, cmd-db / DRV-2, the ADT node of the address) |
| `GeniUart.h` | level 1's survey (loads only) and the pure plans: the serial clock, the UART set-up writes, the RCG entry, FIFO packing |
| `BtBringup.h` | levels 2..4 over callbacks: `powerOn`, `uartUp`, the polled GENI UART (`portSend`/`portEvent`/`portSetBaud`/`portSetFlow`), `controllerUp`, `bringUp`, `safeOff` |
| `BtRpmh.h` | one VRM transaction on apps DRV-2 (the caller holds the DRV-2 lock) |
| `BtFirmware.c` | `bluetooth_a`/`bluetooth_b` through NativeUfs, FAT, `image/<name>`, read only |
| `HciH4.h`, `QcaCodec.h` | H4 framing; the QCA vendor codec and `qcaSetup` (steps 400..412) |
| `PianoBluetooth.cpp` | the IOService: the survey, the ladder's thread, the windows and their logged writes, the locks, the rails helper's kernel side, BT_EN's owner channel, the firmware cache, the address, the power-state hooks |
| `tests/QcaSim.h` | the simulated controller both host tests use |

### 7.1 The shared rails helper (`package/include/PianoWcnRails.h`, built)

One helper both radios **run** (header-only, C and C++), over two 32-bit words that **PianoA830 owns** (`board/PianoA830.cpp`, next to
`sm8750_scm_owner`), because the KC linker merges weak definitions only inside one kext (`tooling/k27/coalesce-weak.py`): a word every
user defined for itself would split, so the words live in the kext that links before both (as the DRV-2, GCC-vote and secure-call
words do), and each radio names PianoA830 as a library. Whichever radio asks first does the rail work; the second only reads back.

| Word | Bits |
|---|---|
| `piano_wcn_rails_state` | 7:0 owners (`PIANO_WCN_WLAN` 1, `PIANO_WCN_BT` 2); 15:8 the rails whose enable the helper added (they read disabled before its vote; bit 8 + index in DT order); 31 the rails are up (voted and read back). Changed only with the lock held; read anywhere |
| `piano_wcn_pmu_owner` | 0 free, else the holder's tag: `WIFI` 0x57494649, `BTH1` 0x42544831 |

```c
int piano_wcn_pmu_lock(PianoWcnRails *r, int take);   /* 1 take (nested holds of one kext count), 0 give; 0 or PianoWcnBusy */
int piano_wcn_rails_get(PianoWcnRails *r, unsigned client);   /* 0, or PianoWcnRailRead -4 / RailVote -5 / RailDropped -6 / Busy -16 */
unsigned piano_wcn_rails_put(PianoWcnRails *r, unsigned client_flags);   /* | PIANO_WCN_RELEASE; returns the owners left */
uint32_t piano_wcn_rails_owners(const PianoWcnRails *r);   /* owners | bit 31, any context */
```

A `PianoWcnRails` carries the words, the kext's tag and its ops: `vrm` (one VRM transaction of 1..3 commands under the DRV-2 lock, the
resource by its cmd-db name), `pmic` (one regulator's STATUS / set point / MODE / EN_CTL over the observer channel: exactly PianoWifi's
`Hw::pmic`), optional `pmic_type` (to find a rail's PPID by the page rule when the caller has no survey), sleep, the kext's hold mutex,
a report hook. The kernel glue (`PIANO_WCN_RAILS_KERNEL`, `PIANO_WCN_KERNEL_RAILS(name, tag, "Label", vrm, pmic, pmic_type)`) adds an
IOLock and one IOLog line per event (`<Label>: wcn rails (ours 0x...): ...`).

- **lock**: compare-exchange 0 → tag, 1 ms sleeps, a line at the first wait and every 5 s, bounded at 30 s (six rail transactions
  behind a DRV-2 lock held by others take at most ~13 s); past the bound `PianoWcnBusy` and nothing done (no take-over: a radio that
  stays off is the safe outcome).
- **get**: rails up → each voted rail read back enabled → the client's bit, nothing sent. Else PianoWifi's level 2 rules (steps
  201..203): every rail and the two PHY LDOs read (a voted rail that cannot be read refuses, unless `blind`); the six in DT order, one
  transaction each, mode NPM + a voltage (the DT minimum, or the PMIC's own value when higher within the DT maximum; none above it) +
  enable; 5 ms; every rail read again (none lower, none that was enabled now disabled); bit 31, the added-enable bits, the client's
  bit. A failure adds no owner bit, clears bit 31, and withdraws the enables this attempt added when nobody owns the rails (else it
  records them for the last owner's release).
- **put**: the client's bit dropped; when no owner is left **and** the caller passes `PIANO_WCN_RELEASE`, only the enables the helper
  added are withdrawn (enable 0; mode and voltage stay), bit 31 cleared. Without `PIANO_WCN_RELEASE` the rails stay voted.
- **rules for both drivers**: rails before any enable pin; raising an enable pin and every rail change with the lock held (lowering
  one's own pin needs none); a radio never touches the other's pin (PianoBluetooth's build checks it never writes WLAN_EN); SW_CTRL is
  read by both. 50 ms after an enable before talking to that radio.

PianoBluetooth: `piano_wcn_rails_get(PIANO_WCN_BT)` inside its PMU-lock section of level 2 (rails, then BT_EN high), with its own
`vrm` (cmd-db + `BtRpmh.h` under the DRV-2 lock) and `pmic`/`pmic_type` (observer reads); `put(PIANO_WCN_BT | RELEASE)` on every
failure, at system sleep and at shutdown (`piano-bt-release=0`: without RELEASE). Host test: `package/include/tests/wcn_rails_host.cpp`
(§8.4 "host").

### 7.2 The change PianoWifi makes to use it (for PianoWifi's owner; not applied here)

Against PianoWifi as on main at 2026-11-11 (level 5, `PianoWifi.cpp` sha256 `6ac47f0a…dad600`); only `PianoWifi.cpp` and its build
script change, the descriptor already names PianoA830. Checked on a scratch copy: the kext object compiles with the native flags (no
warning) and its import check reads "6 from the libraries (… `_piano_wcn_pmu_owner`, `_piano_wcn_rails_state` …), 0 unresolved";
PianoWifi's host tests do not change (`WifiBringup.h` is untouched).

What it does: level 2 takes the PMU lock, gets the rails through the helper (a read-back when Bluetooth holds them, else the vote),
then runs its own `powerUp` unchanged inside the same lock section (its steps 201..203 then find the rails up and send the same votes
again: apps DRV-2 holds one vote per resource, and none of them counts as enabled by PianoWifi), so BT_EN can never rise between
Wi-Fi's rails and WLAN_EN; level 1's PMIC survey runs under the PMU lock (no SPMI reads of PianoBluetooth's on the same peripherals
meanwhile); every path where Wi-Fi gives the chip up (level 2..5 failures, all ending in `safeOff`: WLAN_EN low) puts its owner bit
back, with `PIANO_WCN_RELEASE` when `piano-wifi-release=1`. After it, PianoBluetooth's default 30 s delay with `piano-wifi` in the same
boot is no longer needed (the lock orders them); it stays until this change has run once on the tablet.

```diff
--- a/package/PianoWifi/PianoWifi.cpp
+++ b/package/PianoWifi/PianoWifi.cpp
@@ -55,6 +55,8 @@
 #include "WifiSmmuWindow.h"
 #include "WifiSurvey.h"
 #include "WifiWlfw.h"
+#define PIANO_WCN_RAILS_KERNEL
+#include <PianoWcnRails.h>   // the PMU's six rails, shared with PianoBluetooth (docs/drivers/bluetooth.md section 7)
 
 using namespace pianowifi;
 
@@ -708,6 +710,20 @@
 }
 
 const Hw gHw = {nullptr, hwRead, hwWrite, hwUdelay, hwMsleep, hwNow, hwVoteLock, hwVrm, hwPmic, hwStep};
+
+// The rails helper both radios run (PianoWcnRails.h; its words are PianoA830's): this driver's own VRM transaction (hwVrm, under the
+// DRV-2 lock) and regulator read (hwPmic, the observer channel); the PPIDs from level 1's survey. The PMU lock also keeps
+// PianoBluetooth's SPMI reads of the same peripherals out of ours.
+int wcnVrm(void *, const char *resource, const PianoWcnVrmCmd *cmds, unsigned count)
+{
+  VrmCmd c[3];
+  if (!cmds || !count || count > 3) return -22;
+  for (unsigned i = 0; i < count; ++i) c[i] = {cmds[i].reg, cmds[i].value};
+  return hwVrm(nullptr, resource, c, count, nullptr);
+}
+PIANO_WCN_KERNEL_RAILS(gWcn, PianoWcnTagWifi, "PianoWifi", wcnVrm, hwPmic, nullptr);
+// Wi-Fi no longer runs on the rails: its owner bit back (with piano-wifi-release=1 the last owner withdraws the helper's enables).
+void wcnPut() { piano_wcn_rails_put(&gWcn.rails, PIANO_WCN_WLAN | (gArgs.release ? unsigned(PIANO_WCN_RELEASE) : 0u)); }
 
 Options options()
 {
@@ -1457,7 +1473,9 @@
   }
   if (gArgs.spmi || gArgs.level >= 2) {
     stage(6, "the supplies' PMIC regulators and the BT_EN / sleep clock pins over SPMI (observer reads)");
+    const bool held = !piano_wcn_pmu_lock(&gWcn.rails, 1);
     reportPmic();
+    if (held) piano_wcn_pmu_lock(&gWcn.rails, 0);
   }
   char line[200];
   summary(s, line, sizeof line);
@@ -1497,7 +1515,27 @@
     return;
   }
   note("level 2: the six PMU rails, PERST# low, WLAN_EN; the PHY's two LDOs are read only (one is the UFS PHY's PLL)");
-  int rc = powerUp(power, gHw, opt);
+  // The rails through the shared helper first (PianoBluetooth may hold them already: then only a read-back), then level 2's own
+  // steps, the PMU lock across both: BT_EN never rises between our rails and WLAN_EN. powerUp's steps 201..203 then find the rails
+  // up and send the same votes again (apps DRV-2 holds one vote per resource), and enable nothing of their own.
+  for (unsigned i = 0; i < kRailCount && i < PIANO_WCN_RAIL_COUNT; ++i) gWcn.rails.ppid[i] = gRailPpid[i];
+  gWcn.rails.blind = int(gArgs.blind);
+  int rc = piano_wcn_pmu_lock(&gWcn.rails, 1);
+  if (!rc) {
+    rc = piano_wcn_rails_get(&gWcn.rails, PIANO_WCN_WLAN);
+    if (rc) {
+      power.step = gWcn.rails.step ? gWcn.rails.step : -kStepRailsRead;
+      power.error = rc == PianoWcnBusy ? kErrRailVote : rc;
+    } else {
+      rc = powerUp(power, gHw, opt);
+      if (rc) wcnPut();
+    }
+    piano_wcn_pmu_lock(&gWcn.rails, 0);
+  } else {
+    power.step = -kStepRailsRead;
+    power.error = kErrRailVote;
+    note("level 2: the PMU lock stayed held by %#x (PianoBluetooth): nothing written", piano_wcn_load(&piano_wcn_pmu_owner));
+  }
   reportPower(power);
   writeSummary("level 2");
   if (rc) {
@@ -1528,6 +1566,7 @@
   if (rc) {
     stage(link.step, "level 3 stopped here");
     note("level 3: FAILED at step %d (%s): WLAN_EN is low and PERST# asserted", link.step, errorName(link.error));
+    wcnPut();
     unmapWindows();
     unmapAll();
     return;
@@ -1556,6 +1595,7 @@
   if (!dmaWindow()) {
     stage(-400, "level 4: no DMA window; nothing was given to the endpoint");
     safeOff(gHw, power, opt);
+    wcnPut();
     unmapWindows();
     unmapAll();
     return;
@@ -1563,6 +1603,7 @@
   if (gArgs.iova && !iovaWindow()) {
     stage(-400, "level 4: the window's 32-bit addresses were not set up; nothing was given to the endpoint");
     safeOff(gHw, power, opt);
+    wcnPut();
     unmapWindows();
     unmapAll();
     return;
@@ -1572,6 +1613,7 @@
   stage(rc ? -401 : 401, rc ? "level 4 stopped before mission mode" : "level 4: the firmware runs (mission mode)");
   note("level 4: %s%s", rc ? "the chip did not reach mission mode" : "the chip is in mission mode",
        rc ? "" : gArgs.level < 5 ? "; the ladder ends here (QMI is level 5, WMI and the station levels 6 and up)" : "");
+  if (rc) wcnPut();   // level 4's failures end in safeOff: WLAN_EN low
   if (rc || gArgs.level < 5) {
     unmapAll();
     return;
@@ -1580,6 +1622,7 @@
   // ---- level 5 ---------------------------------------------------------------------------------------------------------
   rc = level5(power, opt);
   piano_wifi_firmware_close();
+  if (rc) wcnPut();   // level 5's failures end in safeOff too
   writeSummary("level 5");
   stage(rc ? -504 : 504, rc ? "level 5 stopped before the firmware was ready" : "level 5: the firmware is ready");
   note("level 5: %s; %s", rc ? "the firmware did not become ready" : "the firmware is ready",
--- a/tooling/k27/build-native-piano-wifi.sh
+++ b/tooling/k27/build-native-piano-wifi.sh
@@ -13,6 +13,7 @@
 . "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
 P=$D/package/PianoWifi; O=$D/out/native-kexts/PianoWifi
 mkdir -p "$O/host-tests"
+"$D/package/include/tests/run.sh"   # PianoWcnRails.h: the rails shared with PianoBluetooth (docs/drivers/bluetooth.md section 7)
 # WifiQrtr.c (the copy of PianoPower's QRTR codec) is a C source: the level 5 test links its object.
 "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/WifiQrtr.c" -o "$O/host-tests/WifiQrtr.o"
 for t in wifi_host bringup_host qmi_host; do
@@ -31,4 +32,4 @@
 # PianoA830's lock words and NativeUfs's partition reader come from those kexts (libraries in PianoWifi.json, both linked before
 # PianoWifi in configs/k27-native-kexts.txt), resolved by the KC link.
 [ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_rpmh_drv2_owner \
-  --library-prefix _sm8750_gcc_vote_owner --library-prefix _native_ufs_part_ | cut -c1-160 >&2
+  --library-prefix _sm8750_gcc_vote_owner --library-prefix _piano_wcn_ --library-prefix _native_ufs_part_ | cut -c1-160 >&2
```

Its tablet check (one boot, with this driver): `piano-wifi=2 piano-bt=2 piano-bt-delay=0` shows either PianoWifi's level 2 first
(`PianoWifi: wcn rails (ours 0x57494649): the six rails voted and read back …`, then PianoBluetooth's `… were up and read back
enabled: owners now 0x80003f03`) or the reverse; one `PMU lock held by 0x…: waiting` / `free again` pair at most, and no `given up`.
`wifi.md` §8 then gains one line: level 2's rails come from the helper.

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop; each level only after the previous one's log.

| Level | What | Writes | Pass | Effort |
|---|---|---|---|---|
| 1 (built) | the survey (§8.1) | none (observer read commands) | the log; nothing written | done; 1 boot |
| 2 (built) power | the PMU lock; `piano_wcn_rails_get(BT)` (cmd-db, six VRM transactions under the DRV-2 lock, read back); BT_EN's APID owner (EE 0, else **stop**: `level 2: FAILED at step 202 (BT_EN's SPMI peripheral is not owned by the apps processor (EE 0))`), its kind (LV/MV GPIO), its registers; the DT's configuration when not already there; low 100 ms when found high; high, read back; the lock given back; 60 ms; SW_CTRL | ≤ 6 rail transactions; 1 (stock state) to 6 PMIC writes, BT_EN's five registers only | rails up, BT_EN out_src 80, SW_CTRL read | 1 boot |
| 3 (built) UART | the five QUP2 votes (0x52010 bits 0–3, 10) under the GCC vote lock; the branches; SE6 protocol 2, FIFO interface on, FIFO depths; the source: a DFS level, else the RCG (0x1e77c) at 7.3728 MHz; the engine at 115200 8N1 RTS/CTS (planUart, 31 writes), polled; the receiver; the **internal loopback** (48 bytes) with the pins not yet the engine's; loopback off; gpio24/25 pull-down, 26/27 pull-up, 2 mA, `qup2_se6`; IOS; drain | 1 vote word, ≤ 5 RCG words, ~35 SE words, 4 TLMM words | `loopback: 48 of 48 bytes echoed`; `cts low: the controller is ready` | 1 boot |
| 4 (built) controller | `qcaSetup` steps 400..412: HCI_Reset (noted), version (ROM 0x20), the WCN7860 frame, flow off, `0xfc48` [0x11], the host to 51.2 MHz ÷ 1, flow on, the version again; the rampatch (957 segments, ELF: acknowledged at its end) and the NVM by board id (56 segments, each acknowledged; in-band sleep cleared, baud 0x11) read whole from `bluetooth_a` at run time; logging off; HCI_Reset; build label; the address (§8.3); HCI_Reset again; Read Local Version (manufacturer 0x001d), the address again | H4 traffic only (plus the RCG's second entry) | `level 4: done: the controller is set up at 3200000 bit/s` | 1–3 boots (`piano-bt-baud=115200` if the speed change fails) |
| 5 HCI smoke | LE extended scan / inquiry for a few seconds | H4 | a count of advertisers (no addresses logged) | 2 days; 1 boot |
| 6 interrupts, sleep | SPI 461 via `armcpu_spi_attach` (the handler always clears M/S status: Ubuntu's lesson), IBS (`piano-bt-ibs=1`), quiesce for sleep | – | no storm; idle power | 1 week |
| 7 macOS | §5.4: A0, then C (or A by decision) | – | a keyboard types | §5.3 |

Every failure from level 2 up ends in `safeOff` (`BtBringup.h`): the engine's commands cancelled/aborted, loopback and interrupt
outputs off, the four pins back to GPIO inputs with pull-downs (no pad drives an unpowered chip), BT_EN low (only if this driver
touched it), the vote bits this driver added withdrawn (not the others'), the rails put (with RELEASE by default). Success keeps the
state of the level asked for (2: powered; 3: + the UART; 4: + the controller idle at 3.2 Mbit/s).

**Battery and the power contract** (`package/include/PianoPowerContract.h`, `docs/power.md` §4): the radio is never powered below
`piano-bt=2`. From level 2 the driver joins the power tree; `setPowerState(kOff)` (system sleep) and `systemWillShutdown` set an abort
flag that ends any running poll (each TX command and every wait checks it), take the bring-up lock and run `safeOff`, synchronously
(the engine's cancels, one SPMI write, the vote word, at most six rail transactions); `kOn` wakes the driver's thread, which climbs the
ladder again from level 2 (the controller lost its RAM patch with BT_EN low: the firmware goes down again, ~1 s at 3.2 Mbit/s). No
interrupt is attached (polled), so there is none to mask. While up and idle the controller holds RFR deasserted once the RX FIFO is
full; nothing polls it between levels.

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
PianoBluetooth: SE6: not read (its clocks are off: level 3 votes them first)      (clocked: "QUP2 version 3....... (3.N); SE6 firmware revision 0000020N: protocol 2 (UART) ..." and four SE6 lines)
PianoBluetooth: stage 4: SMMU: the entries of the wrapper's stream 0x423 (read only)
PianoBluetooth: SMMU idr0 ........ idr1 60000053: N stream-match groups, 83 banks, 128 pages
PianoBluetooth: SMMU stream 0x423: no stream-match entry (SE DMA needs one; FIFO mode does not)      (or: group G smr ........ s2cr ........: bypass | translate ... | fault)
PianoBluetooth: stage 5: BT_EN over the SPMI observer channel (observer reads)
PianoBluetooth: BT_EN (pm8550ve-f gpio3, PPID 0x58a): APID A owner EE 0 (the apps processor: level 2 can drive it); type 10 subtype .. (LV/MV) status1 .. rt_sts ..
PianoBluetooth: BT_EN mode 01 (output) vin 01 pull 05 (none) dig_in .. out_src 00 (level 0, function 0) dig_out .. en 80 (enabled)
PianoBluetooth: summary: bt_en low owner apps | wlan_en 0 sw_ctrl 0 | se6 unclocked | uart pins not as dt | smmu 0x423 none | codec ok
PianoBluetooth: verdict: level 3 votes SE6's clocks first; level 2 raises BT_EN after the shared rails
PianoBluetooth: stage 6: the survey is done
PianoBluetooth: level 1: N MMIO loads, 0 MMIO stores; M SPMI observer reads; P pages mapped, 0 map failures
```

What to read from it: whether SE6 and the wrapper are clocked, the RCG's DFS table (level 3 then only selects a level), the pins'
function at hand-over, BT_EN's subtype (LV/MV: the level is bit 7 of 0x44), its level and **its SPMI owner** (EE 0 or level 2 stops at
step 202), the SMMU entry for 0x423 (only SE DMA needs one). A hang after "stage 3" would mean a gate let a load through to an
unclocked block: rerun with `piano-bt-se=0`. `FAILED`/`not read` lines name their cause.

### 8.2 What levels 2..4 write, and their lines

Every MMIO write goes through the kext's `hwWrite` (one line per kind, `write <window>+<offset> = <value>: <what>`, then counted; the
TX FIFO's words are never logged), every PMIC write through `hwPmicWrite` (BT_EN's five registers only, owner EE 0 only: `pmic write
BT_EN 0x44 = 80: status 00000001 (done)`), every rail transaction through the helper (`rsc: ldof3 (0x...): 3 commands through TCS 2
completed ...`). Each level ends with `level N: W writes (K kinds, 0 refused accesses in all); repeated: ...`. The lines in order
are §8.4's expected blocks; `..` are values to record.

### 8.3 The unit's address (private)

The address is a private installation input, as on Ubuntu (`/etc/piano/bluetooth-address` there): one line, six hex pairs separated
by colons, most significant first, in **`private/bluetooth-address`** (ignored: `.gitignore` has `/private/*`), never committed, never
logged. `tooling/adt/make-piano-adt.py` (run by `build-native-image.sh`) puts it, when that file exists, into the locally built ADT as
`/arm-io/piano-bt` property `bt-address` (six bytes; `--bt-address-file FILE` another file, `--no-bt-address` none; the builder prints
only "with /arm-io/piano-bt (a private Bluetooth address)"). No driver matches that node. Level 4 reads it at run time (refusing all
zeros, all ones and a group address), writes it with `0xfc14`, reads it back with Read BD_ADDR, checks it again after the second
HCI_Reset (written once more if the reset dropped it), and only ever logs "the unit's own set and read back". Without it the
controller keeps its NVM's default (`level 4: address: the controller started with its NVM's default; no private address in this
ADT ...`). `tooling/adt/adt-dump.py` shows the property: do not paste such a dump into a log or a document. Option A0 (§5.3) will
copy it into a `bluetooth` node's `local-mac-address` for bluetoothd.

### 8.4 Results so far and the batched tablet test list

| Check | Result (2026-11-11, offline) |
|---|---|
| host: `package/include/tests/run.sh` | `wcn_rails_host: all tests passed`: the first get votes six rails in DT order (mode, voltage, enable), the second reads back only; a put that is not the last changes nothing; the last put keeps the rails unless RELEASE, which withdraws only the helper's enables (a rail kept on by another master stays; the PHY LDOs are never voted); the voltage rules; an unreadable rail (refused; blind votes); a refused transaction (the attempt's own enables withdrawn, or recorded when an owner runs); a sagging read-back; a rail dropped while up (voted again); the PPID page rule; the lock's nesting, wait, reports, bound, lost and unbalanced gives; two radios on real threads (never two rail transactions or two pin sections at once, the words free and ownerless at the end); the header also compiles as C |
| host: `tests/bt_host.cpp` | all passed: level 1's gates, decoders, plans, H4, the codec, `qcaSetup` against the simulator (now with the first reset unanswered, without it, an address the reset drops, stops after 400 and 410), the SE6 RCG table (every entry's rate through `rcgRate`, `planRcg`'s M/N/D/CFG) |
| host: `tests/bt_bringup_host.cpp` | all passed: levels 2..4 whole against the board model wired to the simulated controller, at 3.2 Mbit/s with DFS off (the RCG programmed 19.2 → 7.3728 → 51.2 MHz, CLK_SEL 6) and on (CLK_SEL 3, the RCG never written), steps 201..205, 301..306, 400..412 in order; then a sleep (everything off, the rails' own enables withdrawn) and a wake (the whole ladder again); levels 2 and 3 alone (level 3 sends not a byte to the controller); no loopback; 115200 only; stops at 204 and 403; BT_EN found high (low 100 ms first); BT_EN an input (the five registers in PGPIO's order); Wi-Fi owning the rails first (no vote, nothing withdrawn at Bluetooth's sleep) and Wi-Fi holding the PMU lock (level 2 stops before anything); faults: BT_EN owned by EE 2 / not found / an older GPIO, a denied PMIC write, a refused rail vote, a branch that never runs, SE6 without UART firmware, the FIFO interface off, an RCG that never updates, a corrupting loopback, the controller's RTS never asserted (step 400, one TX timeout), deaf at the new speed (403), no rampatch (404), a sleep pending mid-download (out at once): each ends safe; invariants held throughout: no SE6/QUP2 access while unclocked, no byte to the controller before the pins are the engine's or before 50 ms of power, only BT_EN's five registers written, only this driver's vote bits withdrawn; the RSC transaction (TCS 2 first, commands, trigger, reclaim; a stuck TCS skipped; no idle TCS) |
| build | `build-native-piano-bluetooth.sh`: 4 objects, "330 imports from the kernel, 6 from the libraries, 0 unresolved"; `build-native-image.sh` (kernel 0059): "native kext rule: 21 native kexts linked, none from outside this repository", PianoBluetooth 472 pointers, every vtable check 0 problem slots; `check-native-kexts.py --self-test` passes |
| tablet | not run |

The batch (one RAM boot each, in this order; stop at the first that fails and send its log). Images:

```sh
for n in 2 3 4; do
  PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-bt=$n" tooling/k27/build-native-image.sh \
    && rm -rf out/ram-bt$n && cp -R out/ram out/ram-bt$n
done
PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-bt=4 piano-bt-baud=115200" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bt4s && cp -R out/ram out/ram-bt4s
PIANO_RESET_AFTER=600 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-bt=4 piano-sleep=1 piano-sleep-test=60 piano-sleep-max=30" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bt4z && cp -R out/ram out/ram-bt4z
PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-wifi=2 piano-bt=4" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-bt4w && cp -R out/ram out/ram-bt4w
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-bt2/boot-piano-macos-ram-A-bundled.img
grep -a "PianoBluetooth\|wcn rails" logs/usb-console/<newest>-port0.txt
```

| Boot | Arguments | Pass | If not |
|---|---|---|---|
| B2 | `piano-bt=2` | the B2 block below | step 202 owner ≠ 0: stop the ladder (§10.1); a rail refused: its `rail ...` line and `rsc:` line |
| B3 | `piano-bt=3` | the B3 block | step 302 `protocol 255`: no UART firmware in SE6 (§10.2); 305: the loopback count; 306 `cts high`: the controller is not ready (BT_EN, rails, 50 ms) |
| B4 | `piano-bt=4` | the B4 block | step 400 `the UART failed` with one TX timeout: CTS never asserted; 403 `the controller does not answer at the new speed`: B4s; 404/406: the partition's file names (`firmware: ... not on the partition`) |
| B4s | `piano-bt=4 piano-bt-baud=115200` | B4 at 115200 (`level 4: 115200 bit/s`; the rampatch takes ~20 s) | as B4 |
| B4z | B4 + `piano-sleep=1 piano-sleep-test=60 piano-sleep-max=30` | B4, then `system sleep: the radio is off (it was up; ...; rails state 0)`, the 0062 suspend lines, `wake: the ladder climbs again to level 4`, `levels 2..4: climb 2 (after a wake)` and the B4 block again | a sleep line missing: the power-tree join (`docs/power.md` §3) |
| B4w | `piano-wifi=2 piano-bt=4` | PianoWifi's level 2 first (it is no helper owner until §7.2), `waiting 30 s`, then B4 with `rails: up already (state 0x8000....)` or six votes again; with §7.2 applied also `owners now 0x80003f03` | rails dropped: the two drivers' `rail` lines side by side |

**B2 expected** (after the level 1 block of §8.1, with `levels: 1 survey, 2 power (...)` after `start:`):
```
PianoBluetooth: levels 2..2: climb 1
PianoBluetooth: stage 200: level 2: the shared rails and BT_EN
PianoBluetooth: step 201: the PMU lock and the shared rails (PianoWcnRails.h)
PianoBluetooth: cmd-db: ldof3 0x....., ldof2 0x....., smpf5 0x....., smpd4 0x....., smpi7 0x....., smpg3 0x.....
PianoBluetooth: rsc: apps DRV-2 id 0x...... (version 3.N), TCS block +0xd00
PianoBluetooth: rsc: ldof3 (0x.....): 3 commands through TCS 2 completed after N us (status 0x10100 0x10100 0x10100)      (six lines: ldof3 ldof2 smpf5 smpd4 smpi7 smpg3; a busy poll under the DRV-2 lock, docs/drivers/wifi.md §10.22)
PianoBluetooth: wcn rails (ours 0x42544831): the six rails voted and read back (N with a voltage): state 0x8000..02
PianoBluetooth: step 202: BT_EN (pm8550ve-f gpio3): its APID's owner, its kind, its registers
PianoBluetooth: step 203: BT_EN to the DT's state (output, VIN1, no bias, normal), low first
PianoBluetooth: step 204: BT_EN high (rails up, the PMU lock held), read back
PianoBluetooth: pmic write BT_EN 0x44 = 80: status 00000001 (done)
PianoBluetooth: step 205: the PMU's power-up time; SW_CTRL read
PianoBluetooth: rail pmu vddio vreg_l3f_1p8 (ldof3) ppid 5c3: before read N mV mode .. en .. -> sent mode 7, voltage, enable -> after read N mV en 80 (rc 0)      (six lines, then two "never voted" PHY lines)
PianoBluetooth: rails: step 203 error 0 (ok); state 0x8000..02, PMU lock 0; 0 lock waits, max 0 ms
PianoBluetooth: BT_EN APID A owner EE 0 type 10 subtype 1.: before mode 01 vin 01 pull 05 out_src 00 en 80 (the DT's state) -> out_src 80 en 80; 1 PMIC writes
PianoBluetooth: level 2: done: rails up (state 0x8000..02), BT_EN high, SW_CTRL in ., WLAN_EN out 0 (not ours)
PianoBluetooth: level 2: 0 writes (0 kinds, 0 refused accesses in all)
PianoBluetooth: state: level 2 up step 205; rails 0x8000..02
PianoBluetooth: stage 299: the ladder reached its level
```
Record: the six rails' before/after (which ones the helper enabled: bits 13:8 of the state), SW_CTRL after BT_EN (the chip's answer).

**B3 expected** (B2's block, then):
```
PianoBluetooth: stage 300: level 3: the UART (clocks, source, engine, loopback, pins)
PianoBluetooth: step 301: GCC: the QUP2 wrapper's and SE6's votes (0x52010), the five branches running
PianoBluetooth: write gcc+0x52010 = ........: GCC QUP2 votes: core, s-ahb, m-ahb, core2x, s6
PianoBluetooth: step 302: SE6: the UART protocol, the FIFO interface, the FIFO depths; the QUP2 version
PianoBluetooth: step 303: the SE6 clock source for 115200 x 16 (a DFS level, else the RCG's table entry)
PianoBluetooth: write gcc+0x1e784 = 00000180: SE6 clock source: M      (and N, D, CFG 00002601, update; none with DFS on)
PianoBluetooth: step 304: SE6: FIFO mode, packing, 115200 8N1, hardware flow control, polled; the receiver started
PianoBluetooth: write se6+0x294 = 000000a0: RX stale count: 16 characters of 10 bits      (one line per register of planUart)
PianoBluetooth: step 305: the internal loopback: TX to RX, RFR to CTS (the pins are not the engine's yet)
PianoBluetooth: write se6+0x700: TX FIFO word (values not logged)
PianoBluetooth: step 306: gpio24..27 to qup2_se6 (the DT's pulls, 2 mA); the line state; what the controller sent drained
PianoBluetooth: write tlmm+0x18000 = 00000005: UART pin to qup2_se6
PianoBluetooth: level 3: GCC QUP2 votes ........ -> ........ (added ........); branches m-ahb ........ s-ahb ........ core ........ core2x ........ s6 ........ after N polls
PianoBluetooth: level 3: QUP2 version 3.......; SE6 firmware 0000020. (protocol 2: UART); FIFO interface on; FIFOs tx N rx N words of 32 bits
PianoBluetooth: level 3: SE6 source: RCG N Hz -> 7372800 Hz (cfg ........ -> 00002601, update after N polls)      (or: DFS on (cmd_dfsr ........), level L (7372800 Hz))
PianoBluetooth: level 3: serial clock 7372800 Hz / 4 / 16 = 115200 baud (0 ppm); 31 set-up writes
PianoBluetooth: level 3: loopback: 48 of 48 bytes echoed, 0 wrong
PianoBluetooth: level 3: pins gpio24 00000005 gpio25 00000005 gpio26 00000007 gpio27 00000007; IOS 0000000. (rx line 1, cts low: the controller is ready); N bytes drained
PianoBluetooth: level 3: done: UART at 115200 baud 8N1, RTS/CTS, polled FIFO (tx N, rx N words)
PianoBluetooth: level 3: N writes (N kinds, 0 refused accesses in all); repeated: TX FIFO word x12, ...
PianoBluetooth: state: level 3 up step 306; rails 0x8000..02
```
Record: the QUP version and FIFO depths, DFS or not, IOS (rx line high and CTS low mean a powered controller on the pads), the bytes
drained (anything the ROM said at power-up).

**B4 expected** (B3's block without its `state:` line, then):
```
PianoBluetooth: firmware partition bluetooth_a (LUN N, N blocks of 4 KiB)
PianoBluetooth: level 4: a private address is in this ADT: it will be set (never logged)      (or: no usable private address in this ADT: the NVM's default stays)
PianoBluetooth: stage 400: level 4: the controller (HCI over the UART)
PianoBluetooth: step 400: HCI_Reset at 115200 (does the ROM answer?)
PianoBluetooth: step 401: the controller's version at 115200
PianoBluetooth: step 402: WCN7860: in-band wake, then the peripheral baud frame
PianoBluetooth: step 403: flow control off, the vendor baud command, the host to the new speed, flow control on
PianoBluetooth: step 404: the rampatch, read from the firmware partition
PianoBluetooth: firmware: gngbtfw20.mbn found
PianoBluetooth: firmware: image/gngbtfw20.mbn, 232436 bytes, read in N ms (held in memory while it is sent)
PianoBluetooth: step 405: the board id
PianoBluetooth: step 406: the NVM, read from the firmware partition and patched
PianoBluetooth: firmware: gngbtnv20.b.. found      (or: not on the partition, then gngbtnv20.bin found)
PianoBluetooth: step 407: SoC logging off
PianoBluetooth: step 408: HCI_Reset
PianoBluetooth: step 409: the firmware's build information
PianoBluetooth: step 410: the address: the NVM's default or not, and the unit's own when one was given (never logged)
PianoBluetooth: step 411: HCI_Reset again (the host's initialisation)
PianoBluetooth: step 412: Read Local Version Information; the address after the reset
PianoBluetooth: level 4: version product ........ patch .... rom .... soc ........ (rom code 0x20); first reset answered
PianoBluetooth: level 4: 3200000 bit/s (the version read again there); rampatch gngbtfw20.mbn (232436 bytes, 957 segments, ELF mode 3); board id ....; NVM gngbtnv20.b.. (13399 bytes, 56 segments, N tags, hci .. -> .., baud .. -> 11, sleep .. -> ..); 57 acknowledgements, N other events
PianoBluetooth: level 4: build "...."
PianoBluetooth: level 4: address: the controller started with its NVM's default; the unit's own set and read back, kept over the reset
PianoBluetooth: level 4: HCI .. revision ...., LMP .. subversion ...., manufacturer 001d (Qualcomm)
PianoBluetooth: uart: N bytes sent, N received, 0 TX timeouts, N cancels, N aborts, N IBS bytes, 0 packets dropped
PianoBluetooth: level 4: done: the controller is set up at 3200000 bit/s and idle (no host stack yet: docs/drivers/bluetooth.md levels 5..7)
PianoBluetooth: level 4: N writes (N kinds, 0 refused accesses in all); repeated: TX FIFO word xN, ...
PianoBluetooth: state: level 4 up step 412; rails 0x8000..02
PianoBluetooth: stage 499: the ladder reached its level
```
Record: the version words, the board id (selects the NVM file), the build label, the HCI/LMP versions, `first reset` (answered or not),
the read time of the rampatch, the counts. A `FAILED at step N (...; last opcode .... status .. hardware error ..)` line names the
step, the command and the controller's answer.

## 9. Fallback

`piano-bt=0` (the default): nothing runs, the controller stays off (BT_EN low), the shared rails as PianoWifi leaves them. A level that
fails leaves the radio off (§8, safeOff); `piano-bt-baud=115200`, `piano-bt-peri=0`, `piano-bt-reset0=0`, `piano-bt-loop=0`,
`piano-bt-stop=N` narrow a failure down. USB keyboards and mice (B7) or the keyboard cover meanwhile.

## 10. Open questions and risks

1. BT_EN's SPMI owner (level 1). If it is not EE 0, the apps processor cannot drive it: level 2 stops at step 202 with that line and
   writes nothing; Linux's working BT_EN would then mean a different arbiter path (a question for the SPMI docs, then the owner).
   (needs-boot)
2. SE6's protocol at hand-over and the RCG's DFS state (level 1/3). Protocol 0xff means no UART firmware in SE6: the SE protocol
   loader (Linux's `geni_load_se_firmware`, with a firmware file) becomes a level of its own. (needs-boot)
3. The QUP2 core and config paths: Linux raises the interconnect votes (`qup-core` 2x 50 MHz above 115200, `qup-config`, the CPU
   path) where this driver votes none and relies on QUP2's BCM keepalive. If 3.2 Mbit/s loses bytes or stalls (TX timeouts, events
   dropped), run B4s and add the BCM vote (RPMh, like PianoA830's host votes) before level 5. (needs-boot)
4. The `bluetooth_a` GPT entry and FAT layout as NativeUfs sees them (level 4 opens it, retrying 30 s while NativeUfs comes up); the
   board id this unit's controller reports (selects `gngbtnv20.bXX`). (needs-boot)
5. The WCN7860 peripheral baud frame (`31 00 f1 ff 02 02 code`) is Ubuntu-only (not mainline); `piano-bt-peri=0` tests without it.
   The first HCI_Reset (ours) to a ROM without its patch: an answer is noted, none is not fatal (`piano-bt-reset0=0` skips it).
6. The address: whether the controller keeps a `0xfc14` address over HCI_Reset (written once more if not); the private file must be
   in `private/` of the checkout that builds the image. Natively the stock partition that holds the unit's address is still unknown.
7. Polled FIFO at 3.2 Mbit/s: lossless with RTS/CTS, but the thread spins in 5–20 µs steps during the download (~1 s); SE DMA (stream
   0x423 through a bypass bank, sheng's 32 KiB) or the IRQ path (level 6) before audio.
8. Two radios: until §7.2 is applied, PianoWifi neither takes the PMU lock nor owns a bit, so a boot with both waits 30 s on
   Bluetooth's side and Bluetooth never withdraws a rail while `piano-wifi≥2`; the words are PianoA830's, so a list without PianoA830
   cannot link either radio.
9. Sleep: system sleep takes the radio down and the wake reloads the firmware (no IBS, no wake-on-Bluetooth). The rails the helper
   enabled are withdrawn at sleep when nobody else owns them; rails other masters keep on are untouched.
10. Pairing-key storage: Apple's stack wants the keychain (AppleKeyStore, absent natively); option C keeps keys in its own file.
11. Coexistence: both radios' firmware runs on the one chip; Ubuntu runs them together. Heat and power: sessions stay attended.

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (`hci_qca`, `btqca`, `hci_sync`, `qcom_geni_serial`, `qcom-geni-se`,
`pwrseq-qcom-wcn`, `clk-rcg2`, `gcc-sm8750`, `pinctrl-spmi-gpio`) and QcaBluetooth's GPL core are the specification only: opcodes,
reply layouts, file names, tag numbers, register offsets, table values and the order of steps are facts; no code, comment or table is
copied (`package/PianoBluetooth/UPSTREAM`). UniXNU-sheng (BSD) is used for facts and design with attribution. No Apple binary is
analysed beyond the KC's kext names, class names and Info.plist matching dictionaries, and bluetoothd's own log lines; option A would
change that and is the owner's decision. No Qualcomm binary is reverse engineered: the rampatch and NVM
(`LicenseRef-Proprietary-Firmware`) are data, read at run time from the tablet's own `bluetooth_a` (or a staged copy under the ignored
`out/`), never committed; only their names and sizes are logged. The Bluetooth address is a private installation input
(`private/bluetooth-address`, the locally built ADT), read at run time, never logged or committed.
