# PianoWifi

The Qualcomm WCN7860 Wi-Fi (PCI 17cb:110e on PCIe0, `pcie@1c00000`) for native macOS 27 on piano. **Levels 1 to 5**: the survey,
the power sequence, the PCIe0 link, the firmware over MHI, and the WLAN firmware's QMI service over the chip's IPC router channels.
Each level includes the ones below it, each stops the ladder where it fails (leaving WLAN_EN low and PERST# asserted), and nothing
runs without `piano-wifi`. Nothing above level 1 has run on hardware yet: read one boot's log before raising the level.

- `piano-wifi=1`: GCC (the PCIe0 and PHY power domains, resets, branch clocks and votes), TCSR, TLMM (WLAN_EN, SW_CTRL, PERST#,
  CLKREQ#, WAKE#), the controller (PARF, DBI, iATU) and the PHY (PCS) only when powered and clocked, the SMMU entries of streams
  0x1400/0x1401, the supplies' RPMh resources in the AOP command database. Every MMIO access is a load.
  `piano-wifi-spmi=1` adds the supplies' PMIC regulators and the BT_EN / sleep clock pins over the SPMI observer channel (the only
  stores: that channel's read commands).
- `piano-wifi=2`: the six PMU rails by RPMh on apps DRV-2 (mode NPM, the voltage piano's own DT asks for, enable — one transaction
  per rail under PianoA830's DRV-2 lock), with the PMIC read back before and after; PERST# low, CLKREQ# to its PCIe function,
  WLAN_EN low 150 ms then high, the PMU's 50 ms, SW_CTRL read. The PCIe PHY's two LDOs — one of them the UFS PHY's PLL supply — get
  no command at all and are only checked for not having moved.
- `piano-wifi=3`: the GDSCs out of collapse, the aux/rchng RCG sources, the branch votes (under PianoA830's GCC vote lock), the
  TCSR reference gate, the two block resets, the PARF set-up, the 128-write QMP v7 PHY sequence, the endpoint's BDF-to-SID entry,
  the root port, PERST# released at least 100 ms after it went low, the LTSSM, L0, the configuration window and `17cb:110e`, the
  memory window, BAR0's size and address, and stream 0x1401 through an SMMU bypass context bank.
- `piano-wifi=4`: a 24 MiB DMA window, BAR0's MHI block, `image/peach/amss20.bin` read at run time from the tablet's own `modem_a`
  (never from this repository), BHI for the boot loader, the MHI contexts, BHIe for the second ELF, mission mode. Polled, no MSI.
- `piano-wifi=5`: the chip's two IPC router channels (20 out, 21 in) over MHI with their rings and 8 KiB buffers in the same
  window (64 MiB by default at this level), the IPC router itself (hello, the firmware's service announcement), and the WLAN
  firmware's QMI service: the radio's capability, the indication registration, the host capability, the firmware's memory regions
  answered out of the window, the chip's capability, the regulatory database and the China-region board file in 6 KiB segments,
  the PHY and auxiliary microcodes by address, the trace configuration, and the firmware's ready indication — the level's gate.
  Every file is read at run time from the tablet's own `modem_a`. `piano-wifi-mode=1` also sends the firmware's options, the host's
  copy-engine configuration (with the shadow register list under `piano-wifi-shadow=1`) and the mode, which is level 6's doorway;
  `piano-wifi-iova=1` gives the endpoint's stream a translating context bank whose 32-bit window maps the DMA window, the only way
  to hand this chip a 32-bit address (piano's XNU memory is all above 4 GiB).

Design, hardware facts, the bring-up ladder, the macOS side, the per-level test procedure with its expected lines and failure
meanings: `docs/drivers/wifi.md`.

| File | What |
|---|---|
| `PianoWifi.cpp` | the IOService on IOPlatformExpertDevice: the survey thread, the mapped windows, the logged writes, the RPMh transactions, the SPMI reads, the SMMU stream, the DMA window, BAR0, the levels' reports and the `piano-wifi-survey` / `piano-wifi-link` properties |
| `WifiSurvey.h` | portable: what level 1 reads and the gates that keep it off unpowered blocks (no store in the file), decoders, the SMMU scan, the cmd-db lookup, the PMIC survey |
| `WifiBringup.h` | portable: levels 2 and 3 — the writes, their order, read-backs, bounded polls, what each failure leaves behind |
| `WifiPhySeq.h` | the PHY sequence table and its provenance note (PaganiWifi, from the 13T's vendor device tree) |
| `WifiMhi.h` | portable: the firmware file's layout, the DMA arena, the BHIe vector table, the BHI/BHIe boot machine, the MHI contexts |
| `WifiChannels.h` | portable: level 5's transport — the two IPC router channels' contexts and rings, the start commands, the receive buffers, a send, and the event walk |
| `WifiQmi.h` | portable: the QMI message writer and the typed readers over the copied codec's parse |
| `WifiWlfw.h` | portable: the WLAN firmware's message set and the step machine, the memory answer, the downloads, the copy-engine configuration and the shadow list |
| `WifiSmmuWindow.h` | portable: the 32-bit window for stream 0x1401 (a translating context bank and its two-page table), copies of PianoDpu's and PianoUsbHost's SMMU pieces (`UPSTREAM`) |
| `WifiQrtr.c/.h` | the IPC router and QMI codec, a copy of PianoPower's with two additions (`UPSTREAM`; PianoPower is not modified) |
| `WifiFirmware.c/.h` | the firmware ranges read in place from `modem_a` through NativeUfs (no file is held whole) |
| `WifiHw.h` | the register facts, each with its source |
| `SpmiObserver.h`, `SmmuStream.h`, `FatVolume.c/.h` | copies of PianoPower's and PianoDwc3's (`UPSTREAM`; those packages are not modified) |
| `tests/wifi_host.cpp` | host tests of level 1 against register models (gates, decoders, SMMU, cmd-db, PMIC) |
| `tests/bringup_host.cpp` | host tests of levels 2–4: the rail votes and their rules, the pin and PHY order, the link polls and timeouts, configuration space and BAR sizing, the MHI boot, each with injected faults |

Build: `tooling/k27/build-native-piano-wifi.sh` (the three host test programs, the `FatVolume.c` drift check, the objects, the
import check). PianoWifi is in `configs/k27-native-kexts.txt` after PianoA830 and NativeUfs, whose symbols it links against, and is inert
without `piano-wifi`. No firmware is in the build. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
