# PianoBluetooth

The Qualcomm WCN7860's Bluetooth side for native macOS 27 on piano: a 4-wire UART on QUP2 SE6 (`serial@898000`, "UART14", SPI 461)
to the chip, BT_EN on GPIO 3 of the pm8550ve "f" PMIC (SPMI SID 5, PPID 0x58a), the PMU's six rails shared with PianoWifi through
`package/include/PianoWcnRails.h`. Levels 1–4 built, host-tested and linked; **not run on the tablet yet**.

- `piano-bt=1`: a read-only survey: the QUP2 vote word and the five branch clocks of the wrapper and SE6, the SE6 clock source
  (RCG, DFS levels), the UART's four pins and the PMU's WLAN_EN / SW_CTRL, the wrapper's version and SE6's registers (only when
  all five branches run; never the FIFO windows, whose RX side pops on a read), the SMMU entries of stream 0x423, BT_EN's PMIC
  registers and its SPMI owner (observer reads); a summary (`piano-bt-survey` property) and a verdict. Every MMIO access is a load;
  the only store is the SPMI observer's read command.
- `piano-bt=2`: power: the PMU lock, the shared rails (voted over RPMh by whichever radio comes first, reference-counted), BT_EN to
  the DT's state and high over the SPMI owner channel (only when its APID belongs to EE 0), 50 ms, SW_CTRL.
- `piano-bt=3`: the UART: the QUP2 votes under PianoA830's GCC vote lock, SE6's protocol and FIFOs, the clock source (a DFS level or
  the RCG programmed), 115200 8N1 RTS/CTS, polled FIFO, the internal loopback self test, then the pins.
- `piano-bt=4`: the controller: HCI_Reset, the QCA version, 3.2 Mbit/s, the rampatch and NVM read at run time from the tablet's own
  `bluetooth_a`, logging off, HCI_Reset, the build label, the unit's address from the locally built ADT (private, never logged),
  HCI_Reset again, Read Local Version. The controller then idles (no host stack yet).
- From level 2 the driver joins the power tree: system sleep takes the radio down (BT_EN low, the UART stopped, its clock votes and
  the rails let go), the wake climbs the ladder again. Every failure leaves the radio off the same way.

Design, hardware facts, the shared-power helper and the change PianoWifi makes to use it, the macOS-side verdict, the ladder, the
batched tablet test list with its expected lines: `docs/drivers/bluetooth.md`.

| File | What |
|---|---|
| `PianoBluetooth.cpp` | the IOService on IOPlatformExpertDevice: the survey, the ladder's thread, the windows and their logged writes, the lock words, the rails helper's kernel side, BT_EN's owner channel, the firmware cache, the address, the power-state hooks |
| `BtHw.h` | the facts, each with its source |
| `GeniUart.h` | portable: the GENI/UART registers and decoders; level 1's survey and its gates (no store); the pure plans: the serial clock for a baud rate, the UART set-up writes, the SE6 RCG entry, FIFO word packing, the RX status |
| `BtBringup.h` | portable: levels 2..4 over callbacks (powerOn, uartUp, controllerUp, bringUp), the polled GENI UART, safeOff |
| `BtRpmh.h` | portable: one VRM transaction on apps DRV-2 (the rails when Bluetooth comes first) |
| `BtFirmware.c`, `BtFirmware.h` | `bluetooth_a`/`_b` through NativeUfs's partition reader and PianoPower's FatVolume.c, read only |
| `HciH4.h` | portable: H4 encoding and the incremental decoder (IBS bytes between packets, resynchronisation, bounded packets) |
| `QcaCodec.h` | portable: the QCA vendor commands and replies, file names, the NVM tag patcher, the download plan, the controller set-up sequence (steps 400..412) over callbacks |
| `tests/bt_host.cpp` | host tests: the survey against register models, the plans, the RCG table, H4, the codec, the set-up against a simulated controller with faults |
| `tests/bt_bringup_host.cpp` | host tests: levels 2..4 whole against a board model (GCC, TLMM, SE6 as a GENI UART, BT_EN over SPMI, the rails through the real shared helper) wired to the simulated controller, with faults at every level, sleep and wake, Wi-Fi first; the RSC transaction |
| `tests/QcaSim.h` | the simulated controller both host tests use |

Build: `tooling/k27/build-native-piano-bluetooth.sh` (the shared helper's tests, the host tests, a store check of the survey headers,
a check that nothing writes WLAN_EN, the objects, the import check). PianoBluetooth is in `configs/k27-native-kexts.txt` after
PianoA830 and NativeUfs (its libraries) and inert without `piano-bt`. It includes `package/PianoPower/SpmiObserver.h` and
`package/PianoKeyboard/CmdDb.h` and compiles `package/PianoPower/FatVolume.c` (none copied). No firmware is in the build or the
repository. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
