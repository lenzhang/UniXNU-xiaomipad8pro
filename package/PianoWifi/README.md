# PianoWifi

The Qualcomm WCN7860 Wi-Fi (PCI 17cb:110e on PCIe0, `pcie@1c00000`) for native macOS 27 on piano. **Level 1 only**: a read-only
survey of what the boot stages leave of the Wi-Fi path, so that level 2 (power-up and link) starts from measured facts.

- `piano-wifi=1`: GCC (PCIe0 and PHY power domains, resets, branch clocks and votes), TCSR (the PHY's reference clock gate), TLMM
  (WLAN_EN, SW_CTRL, PERST#, CLKREQ#, WAKE#), the controller (PARF, DBI, iATU) and the PHY (PCS) only when powered and clocked, the
  SMMU entries of streams 0x1400/0x1401, the supplies' RPMh resources in the AOP command database. Every MMIO access is a load.
- `piano-wifi-spmi=1` adds the supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI observer channel (the only
  stores: the observer channel's read commands).

Design, hardware facts, the bring-up ladder, the macOS side and the test plan: `docs/drivers/wifi.md`.

| File | What |
|---|---|
| `PianoWifi.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-wifi-survey` property |
| `WifiSurvey.h` | portable: what level 1 reads and the gates that keep it off unpowered blocks (no store in the file), decoders, the SMMU scan, the cmd-db lookup, the PMIC survey |
| `WifiHw.h` | the register facts, each with its source |
| `SpmiObserver.h` | the SPMI arbiter v7 observer reads (PianoPower's header in this namespace) |
| `tests/wifi_host.cpp` | host tests against register models (gates, decoders, SMMU, cmd-db, PMIC) |

Build: `tooling/k27/build-native-piano-wifi.sh` (host tests, the objects, the import check). PianoWifi is in
`configs/k27-native-kexts.txt` and inert without `piano-wifi`. No firmware is read at this level. Sources and licences: `UPSTREAM`,
`LICENSE` (BSD-2-Clause-Patent).
