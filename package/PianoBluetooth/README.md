# PianoBluetooth

The Qualcomm WCN7860's Bluetooth side for native macOS 27 on piano: a 4-wire UART on QUP2 SE6 (`serial@898000`, "UART14", SPI 461)
to the chip, BT_EN on GPIO 3 of the pm8550ve "f" PMIC (SPMI SID 5, PPID 0x58a), the PMU's six rails shared with PianoWifi.
**Level 1 only** as a kext; the code of levels 3 and 4 is portable and host-tested, not run.

- `piano-bt=1`: a read-only survey: the QUP2 vote word and the five branch clocks of the wrapper and SE6, the SE6 clock source
  (RCG, DFS levels), the UART's four pins and the PMU's WLAN_EN / SW_CTRL, the wrapper's version and SE6's registers (only when
  all five branches run; never the FIFO windows, whose RX side pops on a read), the SMMU entries of stream 0x423, BT_EN's PMIC
  registers and its SPMI owner (observer reads); a summary (`piano-bt-survey` property) and a verdict for level 2. Every MMIO
  access is a load; the only store is the SPMI observer's read command.

Design, hardware facts, the shared-power contract with PianoWifi, the macOS-side verdict, the ladder and the test plan:
`docs/drivers/bluetooth.md`.

| File | What |
|---|---|
| `PianoBluetooth.cpp` | the IOService on IOPlatformExpertDevice: the survey thread, its log lines, the codec self check, the summary property |
| `BtHw.h` | the facts, each with its source |
| `GeniUart.h` | portable: the GENI/UART registers and decoders; level 1's survey and its gates (no store); the later levels' plans: the serial clock for a baud rate, the UART set-up writes, FIFO word packing, the RX status |
| `HciH4.h` | portable: H4 encoding and the incremental decoder (IBS bytes between packets, resynchronisation, bounded packets) |
| `QcaCodec.h` | portable: the QCA vendor commands and replies, file names, the NVM tag patcher, the download plan, the controller set-up sequence over callbacks |
| `tests/bt_host.cpp` | host tests: the survey against register models, the plans, H4, the codec, the set-up against a simulated controller with faults |

Build: `tooling/k27/build-native-piano-bluetooth.sh` (host tests, a store check of the survey headers, the objects, the import
check). PianoBluetooth is in `configs/k27-native-kexts.txt` and inert without `piano-bt`. It includes
`package/PianoPower/SpmiObserver.h` (not copied). No firmware is in the build or the repository. Sources and licences: `UPSTREAM`,
`LICENSE` (BSD-2-Clause-Patent).
