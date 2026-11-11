# PianoWifi

The Qualcomm WCN7860 Wi-Fi (PCI 17cb:110e on PCIe0, `pcie@1c00000`) for native macOS 27 on piano. **Levels 1 to 6**: the survey,
the power sequence, the PCIe0 link, the firmware over MHI, the WLAN firmware's QMI service over the chip's IPC router channels,
and then the copy engine, the host transport, the firmware's message interface and a passive scan. Each level includes the ones
below it, each stops the ladder where it fails (leaving WLAN_EN low and PERST# asserted), and nothing runs without `piano-wifi`.
Nothing above level 1 has run on hardware yet: read one boot's log before raising the level.

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
- `piano-wifi=6`: the copy engine's four pipes in the same window (a source, a destination and a destination-status ring each,
  programmed through BAR0's window register, polled through the chip's own pointers in host memory), the host transport's
  handshake (its ready message, the firmware's message interface connected, set-up complete, the credit accounting), that
  interface's service-ready and ready events, the host's initialisation with this chip's 72-word resource configuration, a
  station virtual device, and a **passive scan**: the networks it finds are logged (the address masked unless
  `piano-wifi-bssid=1`) and published as the property `piano-wifi-scan`. Then the teardown: the scan stopped, the device
  deleted, the rings disabled. `piano-wifi-band=0` scans only the 2.4 GHz band; `piano-wifi-wmitimeout` and
  `piano-wifi-scanwait` are the deadlines. `piano-wifi=6` implies `piano-wifi-mode=1`. No association, no keys, no data path.
- The macOS side (`PianoWifiEthernet.cpp`, its own kext): an `IOEthernetController` publishing `en1` built in and **link down**
  with that scan list, gated on the resource `PianoWifiLink` which only a level 6 that came up publishes. On the tablet,
  `tooling/native/piano-wifi` prints the list (addresses masked, `--full` for all of it, `status` for the ladder's result).
- Power: the radio is off below `piano-wifi=2`; from there the driver joins the power tree and a suspend stops the radio (the
  scan, the device, the rings, then WLAN_EN low and PERST# asserted), with nothing brought back on the wake.

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
| `WifiCe.h` | portable: level 6's copy engine — the three rings of a pipe, their registers, a send, the receive buffers, the event walk, the teardown |
| `WifiHtc.h` | portable: the host transport — the header, the ready message, a service connected, set-up complete, the credit records |
| `WifiWmi.h` | portable: the firmware's message interface — the codec, the initialisation, the capability and ready events, a virtual device, the scan, the management-frame parse and the scan list |
| `WifiLink.h` | portable: level 6's ladder over those three, as one step machine with a clock and one outstanding command |
| `PianoWifiEthernet.cpp` | the macOS side: `en1` (built in, link down) with the scan list; its own kext, gated on the resource `PianoWifiLink` |
| `WifiHw.h` | the register facts, each with its source |
| `SpmiObserver.h`, `SmmuStream.h`, `FatVolume.c/.h` | copies of PianoPower's and PianoDwc3's (`UPSTREAM`; those packages are not modified) |
| `tests/wifi_host.cpp` | host tests of level 1 against register models (gates, decoders, SMMU, cmd-db, PMIC) |
| `tests/bringup_host.cpp` | host tests of levels 2–4: the rail votes and their rules, the pin and PHY order, the link polls and timeouts, configuration space and BAR sizing, the MHI boot, each with injected faults |
| `tests/qmi_host.cpp` | host tests of level 5: the QMI codec, the copied router, the two MHI channels, the firmware's service and the 32-bit window, each with injected faults |
| `tests/wmi_host.cpp` | host tests of level 6: the copy engine's registers and rings against a model of the chip, the transport's handshake and credits, the initialisation's TLVs, the scan and its list, each with injected faults |

Build: `tooling/k27/build-native-piano-wifi.sh` (the four host test programs, the `FatVolume.c` drift check, the objects, the
import check) and `tooling/k27/build-native-piano-wifi-ethernet.sh` for the macOS side. Both are in
`configs/k27-native-kexts.txt`, PianoWifi after PianoA830 and NativeUfs (whose symbols it links against) and PianoWifiEthernet
after PianoWifi; both are inert without `piano-wifi` (the Ethernet one without `piano-wifi=6`). No firmware is in the build.
Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
