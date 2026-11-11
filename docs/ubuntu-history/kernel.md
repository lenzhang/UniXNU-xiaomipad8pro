# Ubuntu on piano, kernel history: sleep, IRQ routing, charging, GPU/CPU power

What the Ubuntu project's kernel for the same tablet changed, why, and which hardware facts those changes establish, read
for the native macOS 27 port (our kernel at EL1 under Gunyah). Read 2026-11-11. Companion page: [device.md](device.md)
(the device repository: release notes, userspace, issues); this page reads the kernel code and diffs.

**Source.** `yzddmr6/linux-sm8750-piano`, branch `piano/dev` at `7f8d6bedde47` ("Merge the v1.2.0 kernel into piano/dev",
2026-11-10), public. Base: mainline stable tag `v7.2.6` (the merge base, `git describe` = `v1.2.0-2`); later stable
releases v7.2.9, v7.2.10, v7.2.14 and v7.2.15 were merged in (2691 commits committed by the stable maintainer). The
**piano-specific set is the 521 non-merge commits in `v7.2.6..piano/dev` not committed by the stable maintainer**
(project commits plus upstream/FROMLIST picks). Release tags v0.1.0-preview .. v1.2.0 exist in the kernel repository with
the same names as the device releases; "first release" below is the first of those tags that contains the commit.
Commits are cited `K:<12-hex>`; paths are kernel-tree paths; values not stated in a commit were read from the tree at
`piano/dev` (`arch/arm64/boot/dts/qcom/sm8750.dtsi`, `sm8750-xiaomi-piano.dts`, `-desktop.dts`, the named drivers).

**Licence.** The kernel is GPL-2.0. This page states facts, register addresses, sequences and measurements in our own
words and cites where they come from; no code was copied. Re-implement from the facts.

**Status legend.** *measured* = the commit reports a device result; *code* = what the tree does, not necessarily
validated on hardware (many piano commits say "Not booted" or "offline candidate"); *inferred* = our reading.

## 0. Top findings

1. **CX collapse in sleep came from removing always-on votes, not from new sleep code** (v0.8.0). Linux already had
   the whole mechanism in mainline: PSCI OS-initiated mode with a CPU → cluster → system power-domain tree, the APPS RSC
   attached to the system domain so that its sleep/wake sets are flushed when the last CPU powers the system domain off,
   and s2idle. What was missing were consumers that kept CX, XO or DDR up: the crypto engine's ALWAYS interconnect vote,
   an unbound inline-crypto node and a driverless GMU that held GCC/GPUCC boot votes, UFS lane-clock references (CXO),
   GENI UART clocks/bandwidth held by an open port, PCIe root port stuck in D0, the WCN7860 RF clock in the sleep set,
   and the LPASS macro/dcodec votes from the audio codecs. Result: "cxsd and ddr residencies within 0.1 s of the time
   asleep" (*measured*, K:1d5861e2ee37). §2.1.
2. **LLCC: the ADSP island slice must own reserved ways or every suspend resets the SoC.** With the LPASS votes
   released and ISLAND1 (SCID 12) given only bonus ways, 16/16 suspends ended in a PS_HOLD reset; with
   `res_ways = 0x7fffffff` 6/6 resumed (*measured*, K:1d5861e2ee37). On LLCC v6 that is the per-slice ATTR0 word at
   broadcast base 0x26800000 + 0x41000 + 64·12. Natively only PianoA830 touches the LLCC (its GPU slice); nothing sets
   up ISLAND1, so check what boot firmware left before any CX-off sleep. §2.2.
3. **Wake routing is TLMM → PDC → GIC, plus the SPMI arbiter on PDC pin 1.** For a PDC-mapped GPIO Linux never uses the
   TLMM summary interrupt: it sets the TLMM INTR_CFG wakeup-enable bit (bit 7) and programs the type in the PDC
   (IRQ_i_CFG at 0xb220000 + 0x110 + 4·pin) and the GIC. Lid GPIO80 = PDC 122 = SPI 633; keyboard wake GPIO95 = PDC 128 =
   SPI 639; keyboard data GPIO97 = PDC 100 = SPI 611; touch GPIO162 = PDC 109 = SPI 620; RTC alarm, power key and volume
   keys arrive through the SPMI arbiter's `periph_irq` = PDC 1 = SPI 746. §2.3.
4. **A second PDC→GIC "SPI interface" register (0x164400f0, 1 bit per SPI, 1 = level) can keep firmware's level
   setting while the PDC/GIC are programmed for edge**; Ubuntu saw GPIO80 and GPIO95 pending at CX restoration and
   early wakeups; setting those bits to edge let short sleeps reach the RTC alarm (*measured*, K:cd3e49bae2b5), but the
   series was withdrawn (the 0x64-byte region does not cover SPIs ≥ 800, no verified map) and "the keyboard wake issue
   remains unresolved" (K:ff6cf0aafb8f). §2.3.
5. **Keyboard/cover wake semantics:** the WN8030 *status* line (GPIO95, rising) is the host wake request and only
   counts while the panel is blanked; the *data* line (GPIO97) stopped being a wake IRQ in v1.1.1 because it woke the
   tablet right after suspending (K:6824e7cdc219); keyboard wake is not armed with the lid closed (K:afe204252af2); the
   lid wakes only on opening (rising edge of the active-low GPIO80, K:b96cd03f088b). Stock Android took the lid from the
   ADSP over SMP2P (`mi_hall_lid`), not from TLMM (K:659e692d9b72). §2.9.
6. **Charging: Linux saw input current with no host writes at all** (USB 5.04 V, 394 mA, requested limit reading 100 mA,
   K:1e4a0fc89fee test frames); it never writes input suspend 0x2019 or AICL 0x2012. The DSP defaults are ICL 100 mA,
   charge current 0, float 4400 mV. What Linux runs that our PianoPower does not: a PDR (service-registry) listener gating
   every client on `charger_pd` "up", a UCSI client (owner 0x800b), the PAN enable/ack as USB-C *write requests* (owner
   0x800c, opcode 0x15, cmd 0x10/0x11), the board-configuration writes 0x201c/0x201a/0x201b per DSP generation (v0.5.0
   on), and a GLINK layer that does not drop the DSP's first QRTR HELLO (dropping it left 1 boot in 26 with no charger
   service, K:235ab34e2a6e). §2.5, §3.1.
7. **PSCI facts for the idle patch:** CPU state param 0x00000004, cluster 0x01000054, system 0x0200c354 (latencies
   93/129/560 µs and 172/130/686 µs for the two CPU clusters, 2150/1983/9144 µs cluster, 2800/4400/10150 µs system). In OSI
   the deepest powered-off level's param replaces the CPU's and is passed unchanged to CPU_SUSPEND. None has the
   power-down type bit (bit 16 original format, bit 30 extended), so Linux treats all of them as non-context-losing: the
   call returns like WFI and the physical collapse is left to Gunyah/TZ (*inferred* from the parameters and the code).
   Under Gunyah the HLOS reaches the system state at runtime too (ftrace, K:322fd1bfbda9). §2.1, §2.12.
8. **Deep idle stalls DMA-driven peripherals:** a GPI-DMA SPI transfer stalls while the system domain (4400 µs exit)
   is entered; the 5450-byte touch reads at 19.2 MHz ran 43–51 % slow (2.46 ms or exactly 4.85 ms) and caused a
   one-frame panel washout; holding a CPU latency limit of 2000 µs (above the cluster's 1983 µs exit, below the system
   state's) from message prepare to unprepare fixed it (*measured*, K:322fd1bfbda9). Our idle patch needs an equivalent
   latency-hold hook for DMA clients.
9. **Sleep-time SoC resets and spurious wakes all came from touching hardware at the wrong moment:** the DWC3 glue's
   QSCRATCH VBUS-override write ran from the UCSI work while the glue clocks were off in s2idle → SoC reset
   (K:7dffd78b342d); tsens lower-threshold interrupts, wake-armed, refire when the device cools during suspend and abort
   or end every sleep (K:08e750858038); a GENI UART level IRQ answered "not mine" during shutdown got the Bluetooth UART
   disabled (K:386c1f901aa0, K:a2b4feb14264). §2.1, §2.4.
10. **GPU idle (A830) needs correct RPMh dependency votes:** the GMU's per-level MX votes must be built from the GPU OPP
    table (1100 MHz → MX TURBO_L3), GMXC must be enabled above retention (first level 50, not 16), the HFI IRQ handler
    must clear only the CM3-fault bit and the GMU's interrupts must be masked before GMU boot, the kernel's boot bandwidth
    vote must be dropped once the GMU votes bandwidth (else DDR stays at its top frequency), and the GMU firmware init
    timeout is 100 ms (K:cbb3f4900bb4, K:925bbbba6eca, K:891bc63f703a, K:ce9c0ed98aab, K:97567648b85d, K:d86e3f7b2204). §2.7.

Also: CPU DVFS on SM8750 is **SCMI performance protocol over the CPUCP mailbox**, not cpufreq-hw (§2.8); the backlight
must never be turned off by dropping KTZ8866 HWEN, which resets the LCD bias the panel runs from (K:f4a810d07ea2, §2.6);
the Gunyah-emulated APSS watchdog is stopped unless the OS starts it (vendor-hyp SMCCC 0x86000005..8, K:4ba1501653d1, §2.12).

## 1. The piano-specific commits

521 commits, 2026-09-21 .. 2026-11-09, grouped by area below (one line each: the subject, which is descriptive in this
repository; the power-relevant ones are explained in §2). New commits per first release: v0.1.0-preview 226, v0.2.0 29,
v0.3.0 55, v0.3.1 6, v0.4.0 1, v0.5.0 13, v0.5.1 7, v0.6.0 19, v0.6.1 17, v0.6.2 1, v0.7.0 8, v0.8.0 25, v0.9.0 11,
v0.9.1 1, v0.9.2 3, v0.10.0 9, v1.0.0 13, v1.1.0 28, v1.1.1 40, v1.2.0 8, after v1.2.0 1. Kernel bases: 7.2.6 at
v0.1.0, 7.2.9/7.2.10 merged for v0.3.x, 7.2.14 for v0.9.0, 7.2.15 for v1.1.0. Branches besides `piano/dev`:
`piano/v1.2-features`, `piano/linkalign-20261030`; diagnostic DT variants live on a `piano/diag` branch
(K:b80130ed84db). Large upstream series carried as picks (ASoC AudioReach/TDM, CAMSS for SM8750/Kaanapali, WCN7860,
Adreno 830, GMU-driver FROMLIST) are listed in their areas.

| Area | Commits | Power-relevant highlights |
|---|---:|---|
| Gunyah and virtualization | 14 | watchdog driver, SEA fix-up |
| Suspend, idle, interrupts, SoC power plumbing | 51 | v0.8.0 CX-off series, LLCC, PDC, GENI, tsens, GLINK |
| Charging, battery, PMIC, thermistors, RTC | 65 | MCA client, policy, board config, sleep handover |
| CPU DVFS, capacities, thermal trips | 5 | 105 °C / 10 ms trips, SCMI capacities |
| GPU (Adreno 830, GMU, RPMh votes) | 36 | MX dependency votes, GMXC, HFI IRQ, bus vote, GMU driver |
| Display, DSI, DPU, panel, backlight | 71 | teardown order, panel timings, KTZ8866 pairing |
| Input | 53 | keyboard power/wake, lid, touch power/QoS |
| USB, Type-C, DisplayPort | 33 | dwc3 suspend reset fix, UCSI, DIO4485 |
| Audio | 77 | LPASS macro runtime PM (votes) |
| Camera, video, flash | 43 | — |
| Wi-Fi, Bluetooth, PCIe | 30 | RF clock vote, PCIe D3 (in suspend group) |
| Configs, packaging, diagnostics, meta | 43 | QCE off, LLCC built in, lockup detectors |

### 1.1 Gunyah and virtualization (14)

| Commit | First release | Subject |
|---|---|---|
| `4ba1501653d1` | v0.9.0 | arm64: config: build the Gunyah watchdog into piano kernels |
| `25aaee9ba719` | v1.0.0 | virt: gunyah: import Gunyah hypervisor drivers from android17-6.18 |
| `9041a4c985df` | v1.0.0 | virt: gunyah: adapt to mainline 7.2 |
| `b6771d7ac834` | v1.0.0 | virt: gunyah: report the VM status when a vCPU of an exiting VM is re-entered |
| `1251200eb86a` | v1.0.0 | virt: gunyah: build the Gunyah vCPU timer as gunyah_timer.ko |
| `bacb9bce6055` | v1.0.0 | virt: gunyah: rsc_mgr: unregister the PM notifier on unload |
| `7e8435c99355` | v1.0.0 | virt: gunyah: Never release memory that may still be donated or assigned |
| `aec0036c3074` | v1.0.0 | virt: gunyah: preserve the first VM start error |
| `87fdbed4ba74` | v1.0.0 | arm64: config: add modular Gunyah and vhost support for piano |
| `ab77a9b5bab9` | v1.0.0 | arm64: piano: keep virtualization config independent of release ABI |
| `3a62c6125c32` | v1.0.0 | virt: gunyah: clarify the missing direct-map lend integration |
| `bde3f45d6a82` | v1.0.0 | arm64: fix up Gunyah SEAs from unaligned zero-padding loads |
| `a8f3c185549f` | v1.0.0 | virt: gunyah: restore parcel tracking when VM start fails |
| `bf42598aceec` | v1.0.0 | virt: gunyah: quarantine memory when APPEND rollback fails |

### 1.2 Suspend, idle, interrupts, SoC power plumbing (51)

| Commit | First release | Subject |
|---|---|---|
| `93db8797d995` | v0.1.0-preview | arm64: dts: qcom: piano: preserve firmware-owned GPIO ranges |
| `81f600e94806` | v0.1.0-preview | arm64: dts: qcom: piano: describe the UFS supplies, keep UFS disabled |
| `1d02527d1da8` | v0.1.0-preview | spi: geni-qcom: vote memory bandwidth for GPI transfers |
| `f35c6802905d` | v0.1.0-preview | dmaengine: qcom: gpi: order ring descriptors and doorbells |
| `4c54b17cc1be` | v0.1.0-preview | Revert "dmaengine: qcom: gpi: order ring descriptors and doorbells" |
| `bdb6d4e2617e` | v0.1.0-preview | Revert "spi: geni-qcom: vote memory bandwidth for GPI transfers" |
| `e7d6f4959c95` | v0.1.0-preview | soc: qcom: apr: install the GPR callback before probing the service |
| `9adeb3213d24` | v0.1.0-preview | remoteproc: qcom_q6v5: mask the handover interrupt once it is taken |
| `cee97ac74f4b` | v0.1.0-preview | scsi: ufs: core: skip the critical health event where it is refused |
| `56c009d9bf51` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: disable the trace NOC |
| `430f4f6e5f9a` | v0.1.0-preview | Revert "remoteproc: qcom_q6v5: mask the handover interrupt once it is taken" |
| `bdd6f6b688b0` | v0.1.0-preview | remoteproc: qcom: q6v5: Make handover IRQ one-shot |
| `4baf6bdaa1a5` | v0.1.0-preview | remoteproc: qcom: q6v5_pas: Don't enable handover IRQ on attach |
| `5641eefbcaf3` | v0.2.0 | clk: keep orphan references intact when parent adoption fails |
| `0fc5e1634469` | v0.3.0 | PCI: qcom: retain active controller and QMP PHY wake paths |
| `31f73d72b1b8` | v0.3.0 | clk: qcom: gcc-sm8750: honor active PCIe wake paths during sleep |
| `3d62db784e8e` | v0.6.0 | arm64: configs: add lockup detectors for bounded piano RAM trials |
| `a962ac675375` | v0.6.0 | firmware: qcom: tzmem: Use DO_ONCE_SLEEPABLE() in qcom_tzmem_enable() |
| `808f3bc84121` | v0.6.0 | clk: pin runtime PM providers before core registration orphan walks |
| `f89e71d5ce99` | v0.6.0 | clk: Ensure all RPM enabled clocks are enabled before reparenting orphans |
| `fe98a3eca32d` | v0.6.1 | arm64: configs: piano: build in the LLCC driver |
| `322fd1bfbda9` | v0.6.1 | spi: geni-qcom: keep the system out of deep idle while a GPI DMA message runs |
| `235ab34e2a6e` | v0.6.2 | rpmsg: glink: hold messages that arrive before the endpoint can take them |
| `c155f0cfe970` | v0.7.0 | arm64: dts: qcom: sm8750: Enable MCQ support for UFS controller |
| `d21a171ee48e` | v0.7.0 | arm64: dts: qcom: sm8750: Limit the UFS MCQ window to its register space |
| `e73f7dcc1ac2` | v0.8.0 | piano: config: disable the Qualcomm Crypto Engine |
| `2ee29b15aa99` | v0.8.0 | arm64: dts: qcom: sm8750-xiaomi-piano: disable the unused crypto engine |
| `4248c3226460` | v0.8.0 | PCI: Allow D3 for native hotplug-capable Root Ports on non-x86 platforms |
| `000b34528897` | v0.8.0 | arm64: dts: qcom: sm8750-xiaomi-piano: disable the unused inline crypto engine |
| `c553a9503af7` | v0.8.0 | arm64: configs: piano: build the native PCIe hotplug driver |
| `09b81da85469` | v0.8.0 | scsi: ufs: ufs-qcom: Enable only lane clocks in lane clock APIs |
| `7291c862688c` | v0.8.0 | serial: qcom-geni: add force suspend/resume to system sleep callbacks |
| `f9d1044c0102` | v0.8.0 | serial: qcom-geni: Fix unbalanced runtime PM resume for no_console_suspend |
| `f46f8386ed88` | v0.8.0 | arm64: dts: qcom: piano: drop the normal-mode RF clock vote |
| `1d5861e2ee37` | v0.8.0 | soc: qcom: llcc-qcom: Reserve ISLAND1 ways on SM8750 |
| `19114681e92c` | v1.1.1 | soc: qcom: notify the DSPs before system sleep |
| `30f99539539e` | v1.1.1 | dt: arm64: qcom: sm8750: add the SMP2P sleep-state notifier |
| `d3ee98c9c80e` | v1.1.1 | arm64: config: enable the SMP2P sleep-state notifier |
| `08e750858038` | v1.1.0 | thermal: qcom: tsens: mask lower threshold interrupts across system suspend |
| `abaaf7753253` | v1.1.0 | scsi: ufs: core: Fix a deadlock between clock scaling and device commands |
| `d87d77b8ca88` | v1.1.1 | arm64: dts: qcom: piano: drop the serial console stdout-path |
| `7cb962b0430a` | v1.1.1 | arm64: configs: piano: build the null TTY for console=ttynull |
| `a504cd537e94` | v1.1.1 | soc: qcom: close the sleepstate wake chain and make notification opt-in |
| `c72f799e61e1` | v1.1.1 | dt-bindings: qcom: use the common interrupt-parent schema for sleepstate |
| `2ad18a4de500` | v1.1.1 | arm64: dts: qcom: piano: leave sleepstate entries disabled by default |
| `81bc110b53c3` | v1.2.0 | irqchip/qcom-pdc: Additionally set type in SPI config registers |
| `cd3e49bae2b5` | v1.2.0 | irqchip/qcom-pdc: Bound SPI configuration accesses and initialize IRQ routes |
| `386c1f901aa0` | v1.2.0 | serial: qcom-geni: drain interrupts before shutting down the port |
| `7a7a3dcb48ea` | v1.2.0 | arm64: configs: piano: enable nonfatal hang detection |
| `a2b4feb14264` | v1.2.0 | serial: qcom-geni: mask the IRQ across the complete sleep transition |
| `ff6cf0aafb8f` | v1.2.0 | irqchip/qcom-pdc: defer SPI synchronization without a complete register map |

### 1.3 Charging, battery, PMIC, thermistors, RTC (65)

| Commit | First release | Subject |
|---|---|---|
| `8022864edc52` | v0.1.0-preview | arm64: dts: qcom: piano: describe the two fuel gauges on disabled buses |
| `47f4e7fd6802` | v0.1.0-preview | arm64: dts: qcom: piano: use the strong I2C pull-up on the fuel-gauge buses |
| `7f6129ac02ce` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised read-only fuel-gauge variant |
| `3d6a2446b5f0` | v0.1.0-preview | arm64: configs: piano: add the read-only fuel-gauge fragment |
| `bee9d703c511` | v0.1.0-preview | power: supply: qcom_battmgr: release the GLINK client after its users |
| `d6185adc951a` | v0.1.0-preview | dt-bindings: soc: qcom: pmic-glink: document the Xiaomi Pad 8 Pro |
| `cc36810ca732` | v0.1.0-preview | soc: qcom: pmic_glink: add a client for the Xiaomi MCA charger firmware |
| `1e4a0fc89fee` | v0.1.0-preview | power: supply: add a read-only driver for the Xiaomi MCA charger firmware |
| `d46621b81e63` | v0.1.0-preview | arm64: dts: qcom: piano: describe the PMIC GLINK charger firmware |
| `f70e494f8927` | v0.1.0-preview | arm64: configs: piano: build the Xiaomi MCA charger firmware client |
| `dff644eeb6e4` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: keep the RTC offset in SDAM |
| `fd03bee1a1ee` | v0.1.0-preview | power: supply: xiaomi_mca: allow setting the input and charge currents |
| `982e72b25245` | v0.1.0-preview | Revert "arm64: dts: qcom: sm8750-xiaomi-piano: keep the RTC offset in SDAM" |
| `8cac9ef5ae44` | v0.2.0 | power: supply: xiaomi_mca: refuse unknown sources and report charging types |
| `12380c1cd31b` | v0.2.0 | dt-bindings: power: supply: import the community SC8541 binding |
| `d998b2273637` | v0.2.0 | power: supply: xiaomi_mca: port the community charging policy as one client |
| `50ceb157a48a` | v0.2.0 | arm64: piano: allocate ABI 8 for the charging candidate |
| `889ae838e1c7` | v0.2.0 | power: supply: xiaomi_mca: preserve conservative policy source choices |
| `bcdf33caef53` | v0.2.0 | soc: qcom: serialize PAN opt-in with device unbinding |
| `3ef018a0e9d5` | v0.2.0 | arm64: dts: qcom: piano: enable Iris and expose read-only SDAM2 |
| `13a696ff6c3b` | v0.3.0 | power: supply: bq27xxx: serialize gauge access with system sleep |
| `886f911c4d11` | v0.3.0 | power: supply: xiaomi_mca: use the firmware acknowledgement for FCC requests |
| `f768a5585abc` | v0.3.0 | power: supply: sc8541: refuse suspend when switching cannot be stopped |
| `7e3c6dd57e2b` | v0.3.0 | power: supply: sc8541: verify consumer stop requests before returning |
| `a988f93ff6ad` | v0.3.0 | power: supply: xiaomi_mca: hand charging back before system sleep |
| `0d2249b22782` | v0.5.0 | power: supply: sc8541: add firmware-directed OTG configuration |
| `45c8dac697d7` | v0.5.0 | dt-bindings: soc: qcom: pmic-glink: describe optional MCA charge pump |
| `cf4d132fdee4` | v0.5.0 | power: supply: xiaomi_mca: sync OTG board configuration on service up |
| `1cc1d6544880` | v0.5.0 | power: supply: xiaomi_mca: dispatch firmware OTG notifications |
| `3ff90b4ce987` | v0.5.0 | power: supply: xiaomi_mca: expose read-only OTG diagnostics |
| `ae333708813d` | v0.5.0 | arm64: dts: qcom: piano: connect MCA to its SC8541 charge pump |
| `27490ca3d1f4` | v0.5.0 | power: supply: xiaomi_mca: synchronize Piano CID firmware configuration |
| `a5cf45da243b` | v0.5.0 | power: supply: xiaomi_mca: allow explicit CC toggle control on Piano |
| `ff6018d383a6` | v0.7.0 | dt-bindings: iio: adc: Add support for QCOM PMIC5 Gen4 ADC |
| `5812ec75bb03` | v0.7.0 | iio: adc: qcom-spmi-adc5-gen3: Add support for QCOM PMIC5 Gen4 ADC |
| `fa38723bd8cd` | v0.7.0 | iio: adc: qcom-spmi-adc5-gen3: tidy Gen4 channel helpers |
| `43dd63f2f45a` | v0.7.0 | arm64: dts: qcom: sm8750-pmics: describe the PMK8550 ADC |
| `898cec7421ca` | v0.7.0 | arm64: dts: qcom: piano: expose board thermistor temperatures |
| `bd2734598570` | v0.7.0 | arm64: configs: piano: enable PMIC board temperature monitoring |
| `e6da2db2e7bb` | v0.8.0 | power: supply: xiaomi_mca: let the system sleep with no source attached |
| `affe1b0b3ec4` | v0.8.0 | RFC: arm64: dts: qcom: piano: back the RTC offset with SDAM2 |
| `4f1a76dede11` | v1.1.1 | power: supply: add SC96231 pen transmitter |
| `850a36232b1b` | v1.1.1 | dt: bindings: qcom,pmic-glink: describe the pen transmitter supply |
| `ecc09e294bfc` | v1.1.1 | dt: arm64: qcom: sm8750-xiaomi-piano: add the pen transmitter of the dock |
| `5737404f3873` | v1.1.1 | arm64: config: build the transmitter of the pen |
| `bdd0228f991f` | v0.10.0 | power: supply: xiaomi_mca: add thermally bounded full charging |
| `118eebfdd943` | v1.1.0 | power: supply: xiaomi_mca: preserve failed-stop and VBUS error state |
| `ea5c83ee493f` | v1.1.0 | power: supply: xiaomi_mca: expose shared battery charge thresholds |
| `74b5f533ec7e` | v1.1.0 | power: supply: xiaomi_mca: skip protection voltage restore after detach |
| `80eeae96d596` | v1.1.0 | power: supply: xiaomi_mca: stay awake only during protected wired charging |
| `899a44826075` | v1.1.0 | power: supply: xiaomi_mca: report a refused sleep as expected |
| `e9acb111dc96` | v1.1.0 | power: supply: xiaomi_mca: retain 5 V charging limits during sleep |
| `b1b3f9c11a0d` | v1.1.1 | power: supply: xiaomi_mca: extend the stock normal charging window |
| `f48fd47fb25b` | v1.1.1 | power: supply: xiaomi_mca: recover unknown PD input classification |
| `f5b1de558983` | v1.1.1 | power: supply: xiaomi_mca: authenticate Xiaomi chargers before PPS |
| `5b013d328ddc` | v1.1.1 | power: supply: xiaomi_mca: bind authentication and limits to live attachments |
| `de5a8c2b874f` | v1.1.1 | power: supply: xiaomi_mca: retry failed factory OTG configuration |
| `f7388e42cb05` | v1.1.1 | power: supply: xiaomi_mca: finish authentication cleanup before sleep |
| `881c30ec02a9` | v1.1.1 | power: supply: xiaomi_mca: fix normal-profile KUnit coverage |
| `e20ddf530f90` | v1.1.1 | power: supply: xiaomi_mca: select stock charging tables per CP session |
| `f90f8471331e` | v1.1.1 | power: supply: xiaomi_mca: add a consumer-controlled pen boost regulator |
| `e25fefe5c036` | v1.1.1 | power: supply: sc96231: integrate dock and supply lifecycle fixes |
| `9662a7f87f77` | v1.1.1 | power: supply: xiaomi_mca: select CN FFC automatically and correct DIV2 limits |
| `8717a0112754` | v1.1.1 | power: supply: sc96231: invalidate the receiver after supply loss |
| `849689e9f63c` | v1.1.1 | power: supply: xiaomi_mca: distinguish the bus ceiling from the charge target |

### 1.4 CPU DVFS, capacities, thermal trips (5)

| Commit | First release | Subject |
|---|---|---|
| `ecb9d2588b69` | v0.1.0-preview | arm64: dts: qcom: sm8750: throttle the CPUs before they cook |
| `4689914b1ea2` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: use the stock CPU passive trips |
| `c78194b27b55` | v0.5.0 | arm64: dts: qcom: sm8750: Add capacity and DPC properties |
| `fa9b92356393` | v0.5.0 | arm64: dts: qcom: sm8750: retain CPU capacities with SCMI energy data |
| `549976b18fb6` | v0.6.1 | arm64: dts: qcom: sm8750-xiaomi-piano: hold the CPU hotspots at 105C |

### 1.5 GPU (Adreno 830, GMU, RPMh votes) (36)

| Commit | First release | Subject |
|---|---|---|
| `a4f75b6a8deb` | v0.1.0-preview | drm/msm/a8xx: Add support for Adreno 830 GPU |
| `2bdd739dea76` | v0.1.0-preview | arm64: dts: qcom: sm8750: add gpucc/gxclkctl/adreno-smmu nodes |
| `3a80b5b4a39b` | v0.1.0-preview | dt-bindings: display/msm: Document Adreno 830 GPU/GMU |
| `b6d5cacb9ced` | v0.1.0-preview | arm64: dts: qcom: sm8750: add GPU and GMU nodes |
| `15a4bb280e6e` | v0.1.0-preview | arm64: dts: qcom: sm8750: Add GPU cooling |
| `72c335eeba89` | v0.1.0-preview | arm64: dts: qcom: piano: name the device-signed GPU zap shader |
| `8d1e9502dbe6` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised Adreno 830 diagnostic variant |
| `9ac6a06cdaa6` | v0.1.0-preview | arm64: configs: piano: add the Adreno 830 diagnostic fragment |
| `1e23b49b8036` | v0.1.0-preview | drm/msm/a8xx: correct MX dependency OPP lookup and vote mask |
| `4d8253431365` | v0.1.0-preview | Revert "drm/msm/a8xx: correct MX dependency OPP lookup and vote mask" |
| `48879e6e07ab` | v0.1.0-preview | drm/msm/a6xx: build MX dependency votes from the GPU OPP table |
| `925bbbba6eca` | v0.1.0-preview | pmdomain: qcom: rpmhpd: skip the retention level for GMXC |
| `188051ff2cb1` | v0.1.0-preview | Revert "drm/msm/a6xx: build MX dependency votes from the GPU OPP table" |
| `cbb3f4900bb4` | v0.1.0-preview | drm/msm/a6xx: Fix RPMH dependency votes |
| `ec181b09d97a` | v0.1.0-preview | Revert "arm64: dts: qcom: sm8750: Add GPU cooling" |
| `31dc67bcf4c8` | v0.1.0-preview | Revert "arm64: dts: qcom: sm8750: add GPU and GMU nodes" |
| `70af3d189bfd` | v0.1.0-preview | Revert "dt-bindings: display/msm: Document Adreno 830 GPU/GMU" |
| `223254546075` | v0.1.0-preview | Revert "arm64: dts: qcom: sm8750: add gpucc/gxclkctl/adreno-smmu nodes" |
| `7cfa3bd691a3` | v0.1.0-preview | Revert "drm/msm/a8xx: Add support for Adreno 830 GPU" |
| `4d96a3ab23b2` | v0.1.0-preview | arm64: dts: qcom: sm8750: Add GPU clock & IOMMU nodes |
| `e496fb5508dd` | v0.1.0-preview | dt-bindings: display/msm: Document Adreno 830 GPU/GMU |
| `847af4ea48ee` | v0.1.0-preview | drm/msm/a8xx: Add support for Adreno 830 GPU |
| `94f73048774b` | v0.1.0-preview | arm64: dts: qcom: sm8750: Add GPU and GMU nodes |
| `b01d84d78209` | v0.1.0-preview | arm64: dts: qcom: sm8750: Add GPU cooling |
| `c38b25059164` | v0.2.0 | piano: native display (NT36532 DSC) and Adreno 830 GPU (#14) |
| `d86e3f7b2204` | v0.5.0 | drm/msm/a6xx+: Increase GMU FW init timeout |
| `5e69b7737f3a` | v0.8.0 | FROMLIST: drm/msm/adreno/a6xx: Mark cxpd device_link as stateless |
| `5589ca0aff52` | v0.8.0 | FROMLIST: drm/msm: Centralize the standalone drm device check for GPU |
| `e1d8d6b3a997` | v0.8.0 | FROMLIST: drm/msm/adreno: Fix invalid drvdata typecast in adreno_remove() |
| `027e7dc229f3` | v0.8.0 | FROMLIST: drm/msm: Always use component model for standalone GPU |
| `bcfcfb70be9c` | v0.8.0 | FROMLIST: drm/msm: Attach a driver to the GMU |
| `891bc63f703a` | v1.1.0 | drm/msm/a6xx: the HFI IRQ clears only the CM3 fault bit |
| `6565c5044f6d` | v1.1.0 | drm/msm/a6xx: bound the GMU's wait for a fault devcoredump |
| `ce9c0ed98aab` | v1.1.0 | drm/msm/a6xx: mask the HFI interrupt before booting the GMU |
| `97567648b85d` | v1.1.0 | drm/msm/a6xx: drop the boot bus vote once the GMU votes bandwidth |
| `d1dae9e66e3f` | v1.1.0 | drm/msm: map submitqueue priority onto HIGH..LOW |

### 1.6 Display, DSI, DPU, panel, backlight (71)

| Commit | First release | Subject |
|---|---|---|
| `6672896b465b` | v0.1.0-preview | drm/mipi-dsi: add flag for sending all DSC slices in one packet |
| `c4d81f62210c` | v0.1.0-preview | drm/msm/dsi: support DSC configurations with slice_per_pkt > 1 |
| `cb10ec490558` | v0.1.0-preview | dt-bindings: display: panel: Add Novatek NT36532 |
| `6a46190b293a` | v0.1.0-preview | drm/panel: Add Novatek NT36532 panel driver |
| `9ed2e0443069` | v0.1.0-preview | backlight: ktz8866: report register errors and support distinct instances |
| `de40d6a5fdcd` | v0.1.0-preview | arm64: dts: qcom: piano: preserve ABL splash and enable NCM diagnostics |
| `52257333ba8e` | v0.1.0-preview | arm64: dts: qcom: piano: add experimental firmware framebuffer |
| `402cc6d6196b` | v0.1.0-preview | arm64: dts: qcom: piano: retain the firmware scanout AXI clock |
| `b364e5a90e68` | v0.1.0-preview | arm64: dts: qcom: sm8750-piano: preserve the stock DisplayPort rate limit |
| `d80a4a03082b` | v0.1.0-preview | arm64: dts: qcom: piano: retain firmware-owned display clocks |
| `29038e69c916` | v0.1.0-preview | Revert "arm64: dts: qcom: piano: retain firmware-owned display clocks" |
| `fefda5a426c2` | v0.1.0-preview | drm/simpledrm: retain piano firmware scanout resources |
| `33750614d8f7` | v0.1.0-preview | drm/simpledrm: describe firmware scanout bandwidth with an OPP table |
| `85073722ecf9` | v0.1.0-preview | arm64: configs: piano: build the SM8750 display clock controller in |
| `404e5aa42d87` | v0.1.0-preview | clk: qcom: dispcc-sm8750: Stop disp_cc_mdss_mdp_clk_src from getting parked |
| `21b98ced6aa0` | v0.1.0-preview | Revert "drm/panel: Add Novatek NT36532 panel driver" |
| `03ab49628d1c` | v0.1.0-preview | Revert "dt-bindings: display: panel: Add Novatek NT36532" |
| `66397c87ad65` | v0.1.0-preview | dt-bindings: display: panel: Add Novatek NT36532 |
| `4c546d0c0451` | v0.1.0-preview | drm/panel: Add Novatek NT36532 panel driver |
| `cb1016b4cdcc` | v0.1.0-preview | dt-bindings: leds: backlight: kinetic,ktz8866: describe a secondary chip |
| `f4a810d07ea2` | v0.1.0-preview | backlight: ktz8866: drive a secondary chip and keep the boot state |
| `e71bf8d6f0b0` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: describe the two KTZ8866 |
| `9e4179d8dfd0` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano-desktop: enable the backlight buses |
| `0dcf246b487d` | v0.1.0-preview | arm64: configs: piano: build the KTZ8866 backlight driver |
| `6e69d3285ab1` | v0.2.0 | drm/msm: fail the KMS probe when there is nothing to bind |
| `4ccf0de173b4` | v0.2.0 | iommu/arm-smmu-qcom: use an identity default domain for the SM8750 MDSS |
| `1d764f1bc245` | v0.2.0 | drm/msm/dpu: round up the compressed INTF width |
| `96266ff042f2` | v0.2.0 | arm64: dts: qcom: sm8750-xiaomi-piano: describe the native display path, keep it disabled |
| `32efa36968f4` | v0.2.0 | arm64: dts: qcom: sm8750-xiaomi-piano: describe the panel I/O rail, keep it disabled |
| `182452c8a012` | v0.2.0 | arm64: piano: enable the native P81 display with local board supplies |
| `b7a813e2245c` | v0.2.0 | drm/msm/dpu: disable the slave encoder before the master |
| `07e214b5f624` | v0.2.0 | drm/msm/dpu: compute the CRTC bandwidth from the state being checked |
| `a5f4b203b1db` | v0.2.0 | drm/msm/dsi: round the byte clock rate after reparenting to the PHY PLL |
| `608ff1b238a0` | v0.2.0 | drm/msm/dsi: stop the bonded clock consumer before its source |
| `72cfce4c86e5` | v0.2.0 | drm/panel: nt36532: use the P81 stock shutdown timing |
| `5bc3801c580a` | v0.2.0 | drm/msm/dsi: trace the raw FIFO error before acknowledgement |
| `ebae86fbc724` | v0.2.0 | drm/msm/dsi/phy: unwind the 7nm PLL bias on prepare failure |
| `91f22a927947` | v0.2.0 | drm/msm/dsi/phy: defer 7nm PLL preparation until configured |
| `fdd9b17d3d14` | v0.2.0 | arm64: dts: qcom: piano: let the host parent both DSI links to PHY0 |
| `ddb6036edf64` | v0.3.0 | drm/msm/dpu: import SunUEFI GC v2 REGDMA v3 backend |
| `86d97917e833` | v0.3.0 | drm/msm/dpu: import SunUEFI SM8750 GC v2 catalog |
| `cb95b1883e5c` | v0.3.0 | arm64: dts: qcom: import SunUEFI SM8750 REGDMA resource |
| `c1142eac05ab` | v0.3.0 | drm/msm/dpu: retain GC2 DSPPs while the CRTC is enabled |
| `2d0d48d3c391` | v0.3.0 | drm/msm/dpu: keep MDSS kernel-doc attached to its structure |
| `b1048e915691` | v0.3.0 | dt-bindings: display: msm: use a valid REGDMA conditional |
| `3e4939b02f77` | v0.3.0 | drm/msm/dpu: fix adjusted clock parameter documentation |
| `4e8489b1c0df` | v0.3.0 | drm/msm/dpu: stop the video pipeline before the timing engine |
| `c6a971f9bedb` | v0.3.0 | drm/msm/dsi: restore SM8750 bonded slave clock state |
| `884fed5d2d41` | v0.3.0 | drm/msm/dpu: bound REGDMA teardown and leak on unconfirmed reset |
| `f541816ce096` | v0.3.1 | drm/msm/dsi/phy: manage the interface clock from runtime PM directly |
| `8c4edb0399f8` | v0.3.1 | Revert "drm/msm/dpu: stop the video pipeline before the timing engine" |
| `c55958fa09dd` | v0.3.1 | drm/msm/dpu: tear a video-mode slave down after both timing engines stop |
| `26a6e6130de8` | v0.6.1 | drm/panel: nt36532: name the piano panel modules by their real vendor |
| `709f199e8a49` | v0.6.1 | drm/panel: nt36532: reset the piano BOE module with the stock sequence |
| `b18aad686851` | v0.6.1 | drm/panel: nt36532: send the piano BOE rate registers in stock order |
| `330548a3c7ed` | v0.6.1 | drm/panel: nt36532: select the piano panel module from the ID pin |
| `4b8b85811299` | v0.6.1 | dt-bindings: display: panel: nt36532: describe the piano panel by ID pin |
| `47ca23f6206f` | v0.6.1 | arm64: dts: qcom: sm8750-xiaomi-piano: pick panel and touch by module ID |
| `82ca0aef10b2` | v0.6.1 | drm/panel: nt36532: drop the vendor-named piano compatibles |
| `55a703614e15` | v0.6.1 | drm/panel: nt36532: test the piano panel module profiles |
| `9bb1254f4b1e` | v0.9.0 | drm/msm/dsi: Resynchronize SM8750 PHYs after link clocks start |
| `d1dceaecb63a` | v0.9.0 | drm/msm/dsi: allow board-provided D-PHY timing registers; use the piano panel tuning |
| `231d7384a34f` | v0.9.0 | drm/msm/dsi: allow a board-provided HS TX strength; use the piano panel tuning |
| `55fa3a10e38d` | v0.9.0 | drm/msm/dsi: program the bonded slave PHY dividers like the vendor driver |
| `4d13b6e74241` | v0.9.0 | drm/msm/dsi: program the HS TX timer and the video MISR like the vendor driver |
| `9a8a1df0d0a5` | v0.9.0 | drm/msm/dsi: Reset SM8750 bonded controllers after PHY resync |
| `2ec9aa0784d5` | v0.9.0 | drm/msm/dpu: flush shared video CTLs once per kickoff |
| `5ab22dc1564d` | v0.9.1 | drm/panel: nt36532: keep the piano DSI clock lanes in HS |
| `40b4de3f37e5` | v0.9.2 | drm/msm/dsi: initialize SYSTEM_MUXES on the v7.2 slave PHY |
| `b36b5181e9b4` | v0.9.2 | drm/msm/dsi: release the PHY device reference once on deferred probe |
| `b097f51215b2` | v0.9.2 | drm/msm/dsi: propagate clock failures and balance runtime PM references |

### 1.7 Input: keyboard cover, touchpad, lid, touch, keys (53)

| Commit | First release | Subject |
|---|---|---|
| `002f21a73f9a` | v0.1.0-preview | HID: nanosic: add bounded firmware and piano framing helpers |
| `02d1fd9a2be1` | v0.1.0-preview | HID: nanosic: add initial piano I2C and HID transport |
| `cdbc2d42a8b9` | v0.1.0-preview | Input: add NT36532E cascade THP transport and firmware tests |
| `c21596ad89e0` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised THP diagnostic variant |
| `b5446828a2e9` | v0.1.0-preview | arm64: configs: piano: build in SPI, GPI DMA, THP transport and uinput |
| `11b207f74765` | v0.1.0-preview | Input: nt36532e-thp: do not require actual_length from GPI-backed SPI controllers |
| `4c676064f66d` | v0.1.0-preview | Input: nt36532e-thp: apply the vendor's idle-baseline and op-mode commands after boot |
| `c777d7a132a0` | v0.1.0-preview | Input: nt36532e-thp: send the host commands only after the first frame |
| `a66598f919ef` | v0.1.0-preview | arm64: dts: qcom: piano: describe the keyboard bridge, keep it disabled |
| `d2deeca1ee57` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised keyboard bridge diagnostic variant |
| `a5cc878b48a5` | v0.1.0-preview | arm64: configs: piano: add the keyboard bridge diagnostic fragment |
| `e56fb656b08f` | v0.1.0-preview | arm64: dts: qcom: piano: combine tested keyboard and touch in RAM diagnostic |
| `70885ef74567` | v0.1.0-preview | Input: nt36532e-thp: disable unused stylus scanning before finger idle setup |
| `dcb09f4fb8d9` | v0.1.0-preview | HID: nanosic: report tablet mode from the keyboard attach state |
| `659e692d9b72` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: describe the cover lid switch, keep it disabled |
| `7c2a0d29ba35` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: describe the volume keys |
| `741e0378c87c` | v0.2.0 | piano: Nanosic WN8030 keyboard cover driver with the piano variant (#17) |
| `d571ddf64747` | v0.2.0 | HID: integrate community WN8030 transport with piano accessory semantics |
| `770e5fdd47de` | v0.2.0 | Input: nt36532e-thp - follow the shared TDDI panel power sequence |
| `9ab0942f9734` | v0.2.0 | Input: nt36532e-thp - start scanning only after panel enable |
| `e98efd360100` | v0.3.0 | Input: nt36532e-thp - preserve open streams across system sleep |
| `9b5479c44a22` | v0.3.0 | arm64: dts: qcom: piano: link the keyboard to its panel and wake lines |
| `24142165d4f9` | v0.3.0 | HID: nanosic: replace periodic keyboard wake with an attach command |
| `64f228ab0389` | v0.3.0 | HID: nanosic: align piano bridge and accessory power with panel blanking |
| `f15d206ba17e` | v0.3.0 | HID: nanosic: use the dedicated wake IRQ for sleeping keyboard input |
| `625303bdfeee` | v0.3.0 | HID: nanosic: test panel, system sleep and event-driven wake lifecycle |
| `0c2df0fb97d0` | v0.3.0 | HID: nanosic: leave the wakeup source of a wakeup-source client to the I2C core |
| `b96cd03f088b` | v0.3.0 | arm64: dts: qcom: piano: enable lid events and open-cover wakeup |
| `e6c332c6149f` | v0.3.1 | Input: nt36532e-thp: tolerate one recovered SPI frame read failure |
| `cb032046d810` | v0.4.0 | input: nt36532e-thp: start panel and resume streams asynchronously |
| `5e26c272313f` | v0.5.1 | input: nt36532e-thp: wait for firmware REK before enabling IRQs |
| `5e05cea6fa50` | v0.5.1 | input: nt36532e-thp: report REK, frame and command startup timing |
| `f42ee49ae269` | v0.5.1 | input: nt36532e-thp: test and document the REK readiness contract |
| `a8c2a13b2ffb` | v0.5.1 | input: nt36532e-thp: read back modes after a failed startup command |
| `5ed69822d95a` | v0.5.1 | input: nt36532e-thp: wait for firmware NORMAL_RUN before host commands |
| `bd04ba0e1289` | v0.6.0 | Input: nt36532e-thp - restore external-power notifications |
| `3ca03dbb32ed` | v0.6.0 | Input: nt36532e-thp - test charger state and stop boundaries |
| `909a3b50a558` | v0.6.0 | Input: nt36532e-thp - distinguish charger protocol and code sources |
| `722ec8462836` | v0.6.0 | Input: nt36532e-thp - exclude charge pumps and drain supply queries on suspend |
| `9e19be65a16f` | v0.6.1 | Input: nt36532e-thp - select the firmware by the panel module ID pin |
| `b688c1ab67c4` | v0.6.1 | dt-bindings: input: touchscreen: nt36532e: firmware per panel module |
| `793335aee569` | v0.6.1 | Input: nt36532e-thp - test the panel module firmware selection |
| `ba725ea02c92` | v0.6.1 | Input: nt36532e-thp - select the event buffer page once per stream |
| `4ea3f8ce93bc` | v1.1.1 | HID: xiaomi: keep P81C pressure reports out of keyboard input |
| `c236b4ebabc7` | v1.1.1 | arm64: config: build the Xiaomi HID driver as a module |
| `5ef2173e6417` | v0.9.0 | HID: nanosic-wn8030: auto-detect touchpad with property as hint |
| `703fbaf8d755` | v0.9.0 | HID: nanosic-wn8030: report the touchpad's missing feature transport |
| `38b9f656ef06` | v0.9.0 | HID: nanosic-wn8030: retry touchpad creation after a failed attempt |
| `1cca070836d2` | v1.1.1 | HID: nanosic-wn8030: match standalone lid switches |
| `afe204252af2` | v1.1.1 | HID: nanosic-wn8030: keep closed covers from issuing wake events |
| `979b0b4ce585` | v1.1.1 | Input: nt36532e: let the stream owner request pen scanning at runtime |
| `81eadaae36d0` | v1.1.1 | Input: nt36532e: expose the acknowledged pen scan setting read-only |
| `6824e7cdc219` | v1.1.1 | HID: nanosic: use the status IRQ for Piano keyboard wakeup |

### 1.8 USB, Type-C, DisplayPort alt mode (33)

| Commit | First release | Subject |
|---|---|---|
| `ac80aa87e93a` | v0.1.0-preview | arm64: dts: qcom: piano: describe USB2 diagnostic supplies and repeater |
| `aeeb41b0f117` | v0.1.0-preview | arm64: dts: qcom: piano: describe stock USB DP PHY supplies |
| `3d33814bc0ae` | v0.1.0-preview | arm64: dts: qcom: piano: prepare isolated capture and USB2 role diagnostics |
| `4318393e8e20` | v0.1.0-preview | arm64: configs: add isolated USB loopback lifecycle test fragment |
| `a32240f1eacc` | v0.1.0-preview | dt-bindings: usb: fsa4480: describe DIO4485 compatible |
| `8de7b956a8d7` | v0.1.0-preview | usb: typec: fsa4480: support DIO4485 and propagate I/O failures |
| `93dcfedee755` | v0.1.0-preview | soc: qcom: pmic_glink_altmode: encode DP assignments and honor route failures |
| `ec45faa85d2b` | v0.1.0-preview | arm64: piano: enable standard UAS and exFAT modules |
| `b1eb75bcf65f` | v0.5.0 | arm64: dts: qcom: piano: connect USB2 dual-role controller to UCSI |
| `2403d37a8011` | v0.5.0 | usb: typec: ucsi: Set ucsi->connector before enabling notifications |
| `ed4c4df5093c` | v0.6.0 | drm/msm/dp: don't report link training success after a reinit |
| `9001d4cc22d3` | v0.6.0 | phy: qcom: qmp-combo: Prevent unnecessary PM runtime suspend at boot |
| `e48b1df708c6` | v0.6.0 | drm/msm/dp: Serialize HPD state updates |
| `8bc01b889c90` | v0.6.0 | arm64: dts: qcom: piano: connect SuperSpeed PHY and orientation input |
| `4e98895fb2c3` | v0.6.0 | arm64: dts: qcom: piano: connect DIO4485 SBU switch and DisplayPort |
| `55b55246a1da` | v0.6.0 | phy: qcom: qmp-combo: track whether the cached typec_mux mode was committed to hardware |
| `d374c65648b0` | v0.8.0 | usb: typec: ucsi: Skip CAM query when partner has no alt modes |
| `7dffd78b342d` | v0.8.0 | FROMLIST: usb: dwc3: qcom: Defer VBUS override update during system suspend |
| `6ccaa73e98d1` | v0.10.0 | drm/msm/dp: power the phy off before taking it down |
| `bb5b7685c75c` | v0.10.0 | drm/msm/dp: release only the link resources that were taken |
| `5cef77d8d336` | v1.1.0 | usb: gadget: f_ncm: restart the TX timer if the TX freelist is empty |
| `e0809dab97a5` | v1.1.0 | drm/msm/dp: hold one runtime PM reference per plugged state |
| `6d3ee7df5868` | v1.1.0 | drm/msm/dp: reset the link caps on every DPCD read |
| `095e64bebff3` | v1.1.0 | drm/msm/dp: check the PHY power-on and DPCD link status returns |
| `e3e3ff83996a` | v1.1.0 | phy: qcom: qmp-combo: Drop the stale err_disable_pipe_clk teardown |
| `2a5ccfa32100` | v1.1.0 | usb: typec: ucsi: glink: keep failed registrations unregistered |
| `403a8305adc1` | v1.1.0 | dt-bindings: usb: qcom,snps-dwc3: Add mode-switch |
| `68d28640e1d1` | v1.1.0 | usb: dwc3: qcom: Use the UTMI clock as PIPE clock in DisplayPort-only modes |
| `9834284b801f` | v1.1.0 | usb: dwc3: qcom: share the role lock with Type-C clock updates |
| `d2710cd13b33` | v1.1.0 | arm64: dts: qcom: piano: expose the USB controller mode switch |
| `8a8d9fdb5c76` | v1.1.1 | fix(usb): clear UCSI connectors after unregister |
| `c7eb9eea767c` | v1.1.1 | usb: typec: ucsi: glink: only walk the connector nodes |
| `d50fa91b421a` | v1.1.1 | soc: qcom: pmic_glink_altmode: only walk the connector nodes |

### 1.9 Audio (77)

| Commit | First release | Subject |
|---|---|---|
| `e1bba7d3e14a` | v0.1.0-preview | ASoC: qcom: sdw: avoid orphan streams on direct DMIC links |
| `b6b230609e37` | v0.1.0-preview | ASoC: tests: exercise capture gate and shared supply power ordering |
| `580d46029cc2` | v0.1.0-preview | arm64: configs: piano: include the standard ALSA sequencer module |
| `d77babe90b20` | v0.1.0-preview | ASoC: lpass-tx-macro: add channel control and mute regression tests |
| `49c7eedc010e` | v0.1.0-preview | ASoC: lpass-tx-macro: connect synthetic inputs to test mixer controls |
| `72bb70152290` | v0.1.0-preview | ASoC: lpass-tx-macro: keep fixture input names in test context |
| `0ec66629bc3a` | v0.1.0-preview | ASoC: lpass-tx-macro: separate card and codec devices in regression fixture |
| `e01a500844cb` | v0.1.0-preview | ASoC: lpass-tx-macro: use the channel mask for mixer state and mute |
| `79dee9cc9b2a` | v0.1.0-preview | arm64: dts: qcom: piano: capture the internal DMICs through the VA macro |
| `8129cf5d1ce7` | v0.1.0-preview | ASoC: qcom: common: use dev |
| `fecd1048a5b3` | v0.1.0-preview | ASoC: qcom: sdm845: remove unused card |
| `198b60d7773d` | v0.1.0-preview | ASoC: qcom: storm: use dev instead of card on storm_parse_of() |
| `ddbeb29c03c9` | v0.1.0-preview | ASoC: qcom: qdsp6: Remove unused Q6AFE_MAX_CLK_ID define |
| `497712ae83e1` | v0.1.0-preview | ASoC: dt-bindings: qcom,q6apm-lpass-dais: Document DAI subnode |
| `6e63200cb9b8` | v0.1.0-preview | ASoC: qcom: q6apm-lpass-dais: Add MI2S clock control |
| `a659ecb4917c` | v0.1.0-preview | ASoC: qcom: sc8280xp: enhance machine driver for board-specific config |
| `b1f986afd61b` | v0.1.0-preview | ASoC: dt-bindings: qcom,sm8250: Add Hawi sound card |
| `3670c86596a6` | v0.1.0-preview | ASoC: qcom: sc8280xp: Add support for Hawi |
| `a6cbb0e11dc1` | v0.1.0-preview | ASoC: qcom: sc8280xp: allow setting m2is clocks for SM8[456]50 boards |
| `b2c6be338686` | v0.1.0-preview | ASoC: dt-bindings: qcom,sm8250: Add compatible string for SM8475 |
| `49f667279f24` | v0.1.0-preview | ASoC: qcom: sc8280xp: Add support for SM8475 |
| `e825aa2ffbaa` | v0.1.0-preview | ASoC: qcom: audioreach: compute active channel maps from channel_map |
| `72f1f506af26` | v0.1.0-preview | ASoC: dt-bindings: qcom,sm8250: Add Ayaneo Pocket S2 sound card |
| `52937ef44155` | v0.1.0-preview | ASoC: qcom: sc8280xp: add Ayaneo Pocket S2 card with special WSA channel mapping |
| `f539dc300b20` | v0.1.0-preview | ASoC: dt-bindings: qcom: add LPASS LPR vote clock ID |
| `6c1f637c4129` | v0.1.0-preview | ASoC: qcom: qdsp6: Increase Q6DSP_MAX_CLK_ID for LPASS LPR vote clock |
| `1692f626f6b0` | v0.1.0-preview | ASoC: qcom: q6prm: add support for LPASS LPR resource voting |
| `ff76b8543d12` | v0.1.0-preview | ASoC: dt-bindings: qcom,sm8250: add compatible for sdm660 |
| `790996fa4594` | v0.1.0-preview | ASoC: dt-bindings: qcom: q6dsp: add support for lpi mi2s ports 5-6 |
| `6cbaccdb9244` | v0.1.0-preview | ASoC: qdsp6: q6dsp-lpass-ports: add support for lpi mi2s ports 5-6 |
| `ab4fa3baa435` | v0.1.0-preview | ASoC: qdsp6: q6afe: add internal mi2s support |
| `172d315d9f91` | v0.1.0-preview | ASoC: qdsp6: q6afe-dai: add internal mi2s support |
| `5b9ed35fcbd3` | v0.1.0-preview | ASoC: qdsp6: q6routing: add lpi mi2s support |
| `776953a5ad9f` | v0.1.0-preview | ASoC: qdsp6: common: support headphone jacks connected to lpi mi2s |
| `e2c737b360cf` | v0.1.0-preview | ASoC: qcom: sm8250: add support for LPI_MI2S_RX_0 and LPI_MI2S_TX_3 |
| `7df30080059b` | v0.1.0-preview | ASoC: qcom: sm8250: add SDM660 compatible |
| `f60dc2b303a3` | v0.1.0-preview | ASoC: qcom: sc8280xp: tolerate -ENOTSUPP from codec set_sysclk |
| `f0532e50a5ae` | v0.1.0-preview | ASoC: qcom: qdsp6: q6prm: add the missing MCLK clock IDs |
| `0cec2134e65c` | v0.1.0-preview | ASoC: qcom: sc8280xp: rename snd_soc_common to qcom_snd_soc_common |
| `ae26f44a404c` | v0.1.0-preview | ASoC: qcom: sc8280xp: add monaco/monza controls for qcs8275 |
| `b7f99c8a205b` | v0.1.0-preview | ASoC: qcom: qdsp6: add topology-driven Audio IF support |
| `6b8fb6b65033` | v0.1.0-preview | ASoC: qcom: q6apm-lpass-dais: add TDM DAI operations |
| `31889afff053` | v0.1.0-preview | dt-bindings: sound: qcom,q6dsp-lpass-ports: add Audio IF clocks |
| `fcf8f1709efe` | v0.1.0-preview | ASoC: qcom: q6prm: add Audio IF clock IDs |
| `71652367b97d` | v0.1.0-preview | dt-bindings: sound: qcom,sm8250: allow TDM slot properties |
| `67647ddea052` | v0.1.0-preview | ASoC: qcom: common: add DAI-node TDM slot helpers |
| `aae8b4245f09` | v0.1.0-preview | ASoC: qcom: sc8280xp: add TDM hw_params support |
| `c4f3ea2abd92` | v0.1.0-preview | ASoC: qcom: audioreach: use C-style "/*" comment |
| `e66c911fd4d2` | v0.1.0-preview | ASoC: qcom: sc8280xp: configure codec sysclk for QCS615 |
| `750eb9ff752b` | v0.1.0-preview | ASoC: qcom: Use auto-cleanup for firmware loading |
| `7a3c0c73586c` | v0.1.0-preview | ASoC: dt-bindings: qcom,sm8250: Add Maili sound card |
| `2beca1b32ce6` | v0.1.0-preview | ASoC: qcom: sc8280xp: Add Maili sound card support |
| `ef56cd6d5510` | v0.1.0-preview | ASoC: dt-bindings: qcom,q6dsp-lpass-ports: Rename QAIF clock IDs |
| `213285410409` | v0.1.0-preview | ASoC: qcom: qdsp6: Zero-initialize AudioReach module config |
| `0f50ef2942d3` | v0.1.0-preview | ASoC: qcom: common: Distinguish missing and invalid TDM slot configuration |
| `ec9404a1c7d3` | v0.1.0-preview | ASoC: qcom: sc8280xp: Fix TDM hw_params error handling |
| `d3ea34500f8a` | v0.1.0-preview | piano: ASoC: qcom: q6prm: add the LPAIF TDM bit clocks |
| `a47c1f7010c9` | v0.1.0-preview | piano: ASoC: qcom: audioreach: support the LPAIF TDM endpoints |
| `e002d1585f54` | v0.1.0-preview | piano: ASoC: qcom: q6apm-lpass-dais: accept channel maps on TDM ports |
| `db80f2168294` | v0.1.0-preview | piano: ASoC: dt-bindings: add the FourSemi FS19xx amplifier |
| `58b945496e08` | v0.1.0-preview | piano: ASoC: codecs: add the FourSemi FS19xx amplifier driver |
| `e271edbc1f24` | v0.1.0-preview | piano: ASoC: dt-bindings: qcom,sm8250: add the Xiaomi Pad 8 Pro sound card |
| `5164f83efc5f` | v0.1.0-preview | piano: ASoC: qcom: sc8280xp: add the Xiaomi Pad 8 Pro |
| `f6db79f7ee17` | v0.1.0-preview | arm64: dts: qcom: sm8750-xiaomi-piano: describe the speaker amplifiers |
| `c354c6d5352b` | v0.1.0-preview | arm64: dts: qcom: piano: play through the four speakers in the desktop |
| `69421e5de9bc` | v0.1.0-preview | ASoC: codecs: fs19xx: honour a platform volume limit |
| `6f161e3fd93e` | v0.1.0-preview | ASoC: qcom: sc8280xp: cap the piano speaker amplifier gain |
| `16ba217a02af` | v0.3.1 | ASoC: q6apm: serialize readiness queries with device removal |
| `2a6f3cf111e4` | v0.5.1 | ASoC: qcom: q6apm: stop PCM frontend graphs in hw_free |
| `3a5e957a04fe` | v0.5.1 | ASoC: qcom: q6apm: start capture graphs from PCM trigger |
| `ac637afc636d` | v0.6.0 | ASoC: piano: set the owner-selected -9 dB speaker gain |
| `52280a6d4ed5` | v0.8.0 | ASoC: codecs: lpass-va-macro: Switch to PM clock framework for runtime PM |
| `333826b0a11b` | v0.8.0 | ASoC: codecs: lpass-tx-macro: switch to PM clock framework for runtime PM |
| `e95745885cdf` | v0.8.0 | FROMLIST: ASoC: codecs: lpass-tx-macro: manage clocks in runtime PM |
| `c5508d8393b4` | v0.8.0 | FROMLIST: ASoC: codecs: lpass-va-macro: manage clocks in runtime PM |
| `3348a47d033c` | v0.8.0 | ASoC: codecs: lpass: remove unused PM clock dependencies |
| `d5117c9cf54c` | v1.2.0 | ASoC: wcd-common: handle interrupts without a local regmap |

### 1.10 Camera, video decode, flash (43)

| Commit | First release | Subject |
|---|---|---|
| `d5b8d114b24b` | v0.1.0-preview | arm64: dts: qcom: piano: add a module-based Iris diagnostic variant |
| `01b345ec2719` | v0.3.0 | BACKPORT: media: dt-bindings: Add CAMSS device for Kaanapali |
| `7cfb5be3fefe` | v0.3.0 | BACKPORT: media: qcom: camss: Add Kaanapali compatible camss driver |
| `730c1d74523e` | v0.3.0 | BACKPORT: media: qcom: camss: csiphy: Add support for v2.4.0 two-phase CSIPHY |
| `78c6c0401e61` | v0.3.0 | BACKPORT: media: qcom: camss: csid: Add support for CSID gen4 |
| `36ebfa93fcfb` | v0.3.0 | BACKPORT: media: qcom: camss: vfe: Add support for VFE gen4 |
| `1d7b3ef9b599` | v0.3.0 | BACKPORT: media: dt-bindings: Add CAMSS device for SM8750 |
| `1a7c22fc2c68` | v0.3.0 | BACKPORT: media: qcom: camss: Add SM8750 compatible camss driver |
| `966bc2701382` | v0.3.0 | BACKPORT: media: qcom: camss: csiphy: Add support for v2.3.0 two-phase CSIPHY |
| `e8b72cbd851b` | v0.3.0 | BACKPORT: media: qcom: camss: csid: Add support for CSID 980 |
| `95e1673ea0f9` | v0.3.0 | BACKPORT: media: qcom: camss: vfe: Add support for VFE 980 |
| `ad086b0546b0` | v0.3.0 | media: i2c: Add OV32D40 image sensor driver |
| `3613f31e7b07` | v0.3.0 | media: i2c: sheng cameras: modernize runtime PM handling |
| `2b6c350da3c7` | v0.3.0 | i2c: qcom-cci: initialise the completion of every master |
| `04cda5b4b7ab` | v0.3.0 | piano: defconfig: enable the OV32D40 sensor and the DW9768 VCM |
| `01e7f82429bf` | v0.3.0 | piano: camss: csid-980: feed the IPP path to the TFE pixel pipeline |
| `b31395556ffb` | v0.3.0 | piano: camss: TFE 980 pixel path through the hardware ISP |
| `6a32699e6a34` | v0.3.0 | arm64: dts: qcom: piano: import community camera board graph |
| `be3fff89d9d4` | v0.3.0 | arm64: dts: qcom: piano: use stock camera rails and real interconnects |
| `4d7b4e00e91e` | v0.3.0 | arm64: dts: qcom: piano: retain stock camera sleep pin states |
| `104a458768cc` | v0.3.0 | media: dt-bindings: describe the imported piano ISP and sensor clock contracts |
| `f37c00a37078` | v0.3.0 | arm64: dts: qcom: piano: isolate the unverified rear actuator |
| `7c28b2795e60` | v0.3.0 | media: iris: discard unmatched timestamp metadata |
| `0a639d1cfcc6` | v0.3.0 | media: iris: guard against a NULL hfi_sys_ops in the interrupt handler |
| `57616eba4ce2` | v0.3.0 | media: iris: cancel sys error work on driver removal |
| `041e33ce19af` | v0.3.0 | Revert "media: qcom: iris: use disable_irq() during power-off" |
| `0adb163ceb36` | v0.3.0 | media: qcom: iris: Wait for threaded IRQ completion during power-off |
| `fc16bcb2fef9` | v0.3.0 | media: i2c: dw9768: support the piano GT9764 power and initialization sequence |
| `5a8bd27a559d` | v0.3.0 | arm64: dts: qcom: piano: enable the stock GT9764 rear focus actuator |
| `9b211680836d` | v0.3.0 | Revert "arm64: dts: qcom: piano: enable the stock GT9764 rear focus actuator" |
| `1c507b3d51b9` | v0.6.0 | media: iris: support decode-order output on HFI gen2 |
| `e6537c292a51` | v0.6.0 | media: iris: allow larger capture buffer pools on HFI gen2 |
| `f93e35f5cbc6` | v1.1.1 | leds: flash: qcom: correct the cleanup indexing |
| `82a40d864b46` | v1.1.1 | dt: arm64: qcom: sm8750-xiaomi-piano: add the flash of the camera |
| `7916770aec16` | v1.1.1 | arm64: config: build the flash of the camera |
| `12e68887b324` | v0.10.0 | media: i2c: s5kjn1: report the real selection rectangles |
| `74840fbc9af1` | v0.10.0 | media: i2c: s5kjn1: expose the analogue gain code directly |
| `8e914bcc19ca` | v0.10.0 | media: i2c: ov32d40: describe the timing in output pixel units |
| `4d00f8297193` | v0.10.0 | media: iris: finish drains when the final buffer has no valid payload |
| `1cb49011cf28` | v0.10.0 | media: qcom: iris: Add request key frame support for encoder |
| `1721620e2ad5` | v1.1.1 | leds: flash: qcom: retain the existing torch conversion |
| `d5c155422778` | v1.1.1 | leds: flash: qcom: correct the cleanup indexing |
| `946e2d7c87c2` | v1.2.0 | arm64: dts: qcom: piano: connect the rear sensor to its flash |

### 1.11 Wi-Fi, Bluetooth, PCIe (30)

| Commit | First release | Subject |
|---|---|---|
| `0e2f97d3cf13` | v0.1.0-preview | wifi: ath12k: add initial hardware definition for WCN7860 |
| `10256e232704` | v0.1.0-preview | PCI/pwrctrl: pwrseq: add support for WCN7860 |
| `173ca1817b71` | v0.1.0-preview | power: sequencing: qcom-wcn: add support for WCN7860 |
| `490bebf5dd8f` | v0.1.0-preview | Bluetooth: qca: add WCN7860 support (rebased onto 7.1) |
| `c954792de0b1` | v0.1.0-preview | dt-bindings: regulator: qcom,qca6390-pmu: document the WCN7860 PMU |
| `00254efef39e` | v0.1.0-preview | dt-bindings: net: bluetooth: add the Qualcomm WCN7860 binding |
| `05f9cafdd25e` | v0.1.0-preview | arm64: dts: qcom: piano: describe the WCN7860 PMU and UART14 Bluetooth, keep disabled |
| `b7cbbad05442` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised Bluetooth diagnostic variant |
| `9a3a022668ec` | v0.1.0-preview | arm64: configs: piano: add the Bluetooth diagnostic fragment |
| `ee7c09196123` | v0.1.0-preview | wifi: ath12k: declare WCN7860 firmware files |
| `7d9bbb32cd40` | v0.1.0-preview | arm64: dts: qcom: piano: enable the QUP2 wrapper in the Bluetooth diagnostic variant |
| `2e77223ec19a` | v0.1.0-preview | dt-bindings: net: wireless: qcom,ath12k: add WCN7860 |
| `42bd5ead3969` | v0.1.0-preview | arm64: dts: qcom: piano: describe PCIe0 and the WCN7860 WLAN function |
| `a165d9dc9a25` | v0.1.0-preview | arm64: dts: qcom: piano: add a supervised WLAN diagnostic variant |
| `1ef6739d0b3a` | v0.1.0-preview | wifi: ath12k: send QDSS config when CNSS_QDSS_CFG_MISS_V01 is disabled |
| `8680cc843bc1` | v0.1.0-preview | wifi: ath12k: disable CNSS_QDSS_CFG_MISS_V01 for the WCN7860 |
| `1a6e2f6a468a` | v0.1.0-preview | FROMLIST: PCI: qcom: Honor IOMMU provider's #iommu-cells in qcom_pcie_config_sid_1_9_0() |
| `5018718fd089` | v0.1.0-preview | arm64: dts: qcom: piano: enable WLAN in the combined desktop |
| `4059a2e9e48e` | v0.1.0-preview | dt-bindings: net: wireless: qcom,ath12k: allow a MAC address property |
| `668c9d7e36d6` | v0.1.0-preview | wifi: ath12k: retrieve MAC address from system firmware if needed |
| `df949a731864` | v0.1.0-preview | FROMLIST: wifi: ath12k: fix frequency range for single-pdev devices |
| `e43c167d0a94` | v0.1.0-preview | wifi: ath12k: use a random MAC address if none is available |
| `5d45ec650c5a` | v0.1.0-preview | Revert "FROMLIST: PCI: qcom: Honor IOMMU provider's #iommu-cells in qcom_pcie_config_sid_1_9_0()" |
| `6f4c292dfad0` | v0.1.0-preview | PCI: qcom: Honor IOMMU provider's #iommu-cells in qcom_pcie_config_sid_1_9_0() |
| `0caaab7d0411` | v0.1.0-preview | FROMLIST: wifi: ath12k: prevent scan during firmware recovery |
| `916ea531ea6a` | v0.1.0-preview | arm64: configs: piano: build uhid and hidraw for Bluetooth LE input |
| `d9d029f0c5da` | v0.1.0-preview | wifi: ath12k: apply the vendor SAR TX power limit on WCN7860 |
| `0ba4d25b6ba4` | v0.6.0 | Revert "wifi: ath12k: add panic handler" |
| `eab862e939ac` | v0.10.0 | wifi: ath12k: don't count a vdev restart as a started vdev |
| `c96d234381f8` | v1.1.0 | wifi: ath12k: replenish the RX buffers reaped without a complete MSDU |

### 1.12 Configs, packaging, diagnostics, meta (43)

| Commit | First release | Subject |
|---|---|---|
| `c83f71d02484` | v0.1.0-preview | arm64: configs: add piano offline RAM diagnostic profile |
| `20652f2476cc` | v0.1.0-preview | arm64: dts: qcom: add piano RAM bring-up skeleton |
| `e22ff4d99316` | v0.1.0-preview | arm64: configs: piano: enable Ubuntu SquashFS compression formats |
| `25bb0e5cd444` | v0.1.0-preview | arm64: configs: piano: support read-only network root and Ubuntu portals |
| `af5bef17ce99` | v0.1.0-preview | arm64: dts: qcom: piano: add supervised read-only UFS identification variant |
| `9ffeff394f65` | v0.1.0-preview | arm64: configs: piano: add the read-only UFS identification fragment |
| `f3621a953f3a` | v0.1.0-preview | pinctrl: qcom: sm8750: describe the TLMM I2C pull-up bit |
| `15dfe3012ce4` | v0.1.0-preview | arm64: dts: qcom: piano: add isolated ADSP RAM diagnostic |
| `dad276d3996c` | v0.1.0-preview | driver core: add KUnit coverage for device_move glue references |
| `9d20666184b4` | v0.1.0-preview | driver core: release the old glue directory after a successful move |
| `fdf567d607fb` | v0.1.0-preview | dt-bindings: arm: describe the saved Xiaomi root identity |
| `71369ce6718d` | v0.1.0-preview | soc: qcom: restore the DT root identity replaced by ABL |
| `d3bf0a0e904f` | v0.1.0-preview | arm64: dts: qcom: sm8750-piano: save the root identity for ABL boots |
| `f274d9189077` | v0.1.0-preview | arm64: configs: piano: restore the root identity replaced by ABL |
| `03c8a18310ab` | v0.1.0-preview | arm64: qcom: piano: assemble tested subsystems in a combined desktop diagnostic |
| `536f2fe758f3` | v0.1.0-preview | arm64: qcom: piano: enable UFS root and bootconfig for persistent desktop |
| `0834ebf10c61` | v0.1.0-preview | arm64: configs: piano: include the BPF LSM tracing dependencies |
| `84925154026b` | v0.1.0-preview | arm64: configs: piano: load XZ and ZSTD compressed firmware |
| `fc9d7c7d2dce` | v0.1.0-preview | arm64: configs: piano: give every packaged kernel a unique release |
| `6f1a510bd32e` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano2 |
| `b80130ed84db` | v0.1.0-preview | arm64: dts: qcom: piano: move diagnostic variants to the piano/diag branch |
| `8d4d5d32c581` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano3 |
| `5f683c1d6393` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano4 |
| `24dd771023e5` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano5 |
| `32c92fc0dc8e` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano6 |
| `c56c9f79bc27` | v0.1.0-preview | arm64: configs: piano: enable PSI, zram and the systemd resource defaults |
| `683df357e4d8` | v0.1.0-preview | arm64: configs: piano: build nftables and the common xtables modules |
| `dd4be5147ba9` | v0.1.0-preview | arm64: configs: piano: build WireGuard with policy routing |
| `2d2c2a34ad7d` | v0.1.0-preview | arm64: configs: piano: enable the Yama and Landlock LSMs |
| `a1f2f11d5d90` | v0.1.0-preview | arm64: configs: piano: build the desktop filesystem and binfmt modules |
| `667263fd4496` | v0.1.0-preview | arm64: configs: piano: release 7.2.6-piano7 |
| `4f9f7acfdeec` | v0.2.0 | arm64: dts: qcom: sm8750: Correct DPU VBIF address space size |
| `a5831ee94d2f` | v0.3.0 | piano: include the existing PHY and GPIO key policy definitions |
| `3dd68c8d03b1` | v0.3.1 | misc: fastrpc: freeze userspace invocation waiters in place |
| `15c6cf63b301` | v0.6.0 | arm64: piano: default desktop filesystem names to UTF-8 |
| `47298efcbd6b` | v0.6.1 | arm64: configs: piano: build in the shared GPIO proxy |
| `ac14d1dc48db` | v0.6.1 | piano: keep tracker references out of comments and driver notes |
| `57592f423dce` | v0.8.0 | github: add contribution guidance for the piano kernel |
| `f7729800dbd4` | v1.1.0 | arm64: configs: enable common SM8750 tablet peripherals |
| `4fcf838868ac` | v1.1.0 | arm64: configs: share the netfilter selection of SM8750 tablets |
| `96fd1279d744` | v1.1.0 | arm64: configs: support Waydroid on SM8750 tablets |
| `62c9304ad126` | v1.1.0 | arm64: configs: build IPsec support for Waydroid's netd |
| `9014edfd0cf5` | after v1.2.0 | github: a checklist contributors and their agents read before a PR |

## 2. Power topics: facts, sequences, why, pitfalls

### 2.1 System suspend under Gunyah: how Linux reaches CX off

**Mode.** s2idle (`SuspendState=freeze` in the device repo, see device.md §2.1). PSCI SYSTEM_SUSPEND ("deep") is offered
and was tried once in the LLCC investigation (K:1d5861e2ee37: "15 s2idle, one PSCI system suspend"); the product never
uses it. Nothing piano-specific was added to PSCI, cpuidle, rpmh-rsc or genpd: the deep state rests on mainline code.

**CPU idle and PSCI topology** (*code*, `sm8750.dtsi` `idle-states`, `domain-idle-states`, `psci`; unchanged from
v7.2.6). `psci` node, `method = "smc"`, hierarchical power domains (OSI):

| Level | Node | `arm,psci-suspend-param` | entry / exit / min-residency (µs) |
|---|---|---|---|
| CPU0–5 (cluster 0, 6 cores) | `cluster0_c4`, "ret" | 0x00000004 | 93 / 129 / 560 |
| CPU6–7 (cluster 1, 2 prime cores) | `cluster1_c4`, "ret" | 0x00000004 | 172 / 130 / 686 |
| cluster 0 and cluster 1 domains | `cluster_cl5` | 0x01000054 | 2150 / 1983 / 9144 |
| system domain (parent of both clusters) | `domain_ss3` | 0x0200c354 | 2800 / 4400 / 10150 |

- Linux enables OSI with PSCI_SET_SUSPEND_MODE when the firmware reports it; without OSI the domain states are not used
  at all (`drivers/cpuidle/cpuidle-psci-domain.c`). On piano the system state *was* entered (ftrace, K:322fd1bfbda9), so
  Gunyah's PSCI implements OSI for the HLOS VM (*inferred from measured*).
- When a CPU idles, its genpd powers off; when the last CPU of a cluster goes, the cluster domain's param is recorded;
  when both clusters go, the system domain's param is recorded. The recorded param **replaces** the CPU's param
  (it is not ORed) and goes unchanged into CPU_SUSPEND (`drivers/cpuidle/cpuidle-psci.c`, domain idle entry).
- None of the three params has the PSCI power-down type bit (bit 16 in the original format, bit 30 in the extended
  format), so Linux calls CPU_SUSPEND as a plain call that returns, with no resume entry point and no context save
  (`psci_cpu_suspend_enter`). The physical core/cluster/CX collapse is Gunyah's and TZ's business; the guest's context is
  preserved by the hypervisor (*inferred*: check `PSCI_FEATURES(CPU_SUSPEND)` bit 1 natively to know the format).
- In s2idle each CPU's domain is suspended with the genpd "suspend" path instead of runtime PM, so the same hierarchy
  is walked down to the system state.

**APPS RSC and the sleep/wake sets** (*code*, `drivers/soc/qcom/rpmh-rsc.c`, `rpmh.c`; DT `apps_rsc: rsc@16500000`).
- Three DRV regions at 0x16500000/0x16510000/0x16520000; the HLOS uses **DRV 2** (0x16520000), TCS block at +0xd00,
  TCS configuration ACTIVE 3, SLEEP 2, WAKE 2, CONTROL 0; IRQs GIC SPI 3/4/5 (DRV 0/1/2). The RSC version is read from
  the DRV base +0x00 (major in bits 23:16); register offsets inside a TCS differ between major 2 and ≥ 3 (TCS stride 672
  bytes in both; per-command stride 20 vs 24).
- "Solver" mode: the driver reads DRV_SOLVER_CONFIG (DRV base +0x04), bit 24. If the hardware solver bit is set, Linux
  installs **no** flush hook at all. If clear and the RSC device has a power domain (SM8750: `power-domains =
  <&system_pd>`), Linux registers a genpd notifier: on the system domain's PRE_OFF it writes the cached sleep and wake
  votes into the SLEEP/WAKE TCSes (`rpmh_flush`), and refuses the power-off (system state not used, cluster state taken
  instead) if an active TCS is busy or the flush fails. Without a power domain it falls back to a CPU-PM notifier run
  by the last CPU. The RSC then executes the SLEEP set when the APSS subsystem goes down and the WAKE set on the way up.
- With CONTROL_TCS 0 the "next wakeup" programming (`rpmh_rsc_write_next_wakeup`) is skipped on SM8750.
- How the sets are filled (`rpmh.c`): an active-only write goes out at once on an active TCS and is cached as that
  resource's *wake* value; clients that must keep something up in sleep send a separate *sleep* value (rpmhpd, clk-rpmh
  and the BCM voter aggregate: always-on demand into sleep, active-only demand into active/wake). `rpmh_flush` writes
  every cached sleep and wake value into the SLEEP/WAKE TCSes.
- Consequence (*inferred*, the core of §3.2): an APPS vote (rail level, BCM bandwidth, XO, RF clock) persists through
  sleep unless a sleep value lowers it or the active vote was already dropped before the last CPU went down.

**What had to be quiesced before CX reached collapse** (v0.8.0, all *measured* collectively by K:1d5861e2ee37):

| Blocker | Mechanism | Fix |
|---|---|---|
| Qualcomm Crypto Engine | keeps an ALWAYS interconnect vote with no runtime PM: "keeps CX and DDR out of their low-power states during s2idle" | driver not built, node disabled (K:e73f7dcc1ac2, K:2ee29b15aa99) |
| Inline crypto engine node at 0x1d88000 | unbound consumer of GCC clocks and the UFS GDSC, so GCC sync_state never ran and "the GCC power domains that were on at boot never get their stay-on vote dropped" | node disabled, UFS `qcom,ice` removed (K:000b34528897) |
| GMU without a driver | same sync_state hold on GCC, GXCLKCTL and GPUCC | FROMLIST "attach a driver to the GMU" + stateless cx device link (K:bcfcfb70be9c, K:5e69b7737f3a) |
| UFS lane-clock helpers | enabled the whole clock array, so non-lane clocks never reached refcount 0, "ultimately blocking CXO shutdown" | only the three lane symbol clocks (tx_lane0, rx_lane0, rx_lane1) (K:09b81da85469) |
| GENI UART held open | system sleep did not reach runtime suspend while a file was open: clocks and interconnect stayed on | force runtime suspend in system sleep (K:7291c862688c, K:f9d1044c0102) |
| PCIe root port | advertises hot-plug capable; without native pciehp it never leaves D0, so link, clocks and GDSC stay on | allow D3 for native hotplug ports on non-x86 + build pciehp (K:4248c3226460, K:c553a9503af7) |
| WCN7860 power sequencer | supplying RPMH_RF_CLK1 requested the RF buffer in active, wake **and sleep** states while either radio was on | clock removed from the PMU node; the stock Bluetooth driver enables it only in a special-operation path (`perform_fmd_operation`), never for normal power-on (K:f46f8386ed88) |
| LPASS TX/VA macro codecs | held LPASS_HW_MACRO_VOTE and LPASS_HW_DCODEC_VOTE (ADSP PRM clock votes) from probe | runtime PM with explicit clock gating, 100 ms autosuspend (K:52280a6d4ed5, K:333826b0a11b, K:e95745885cdf, K:c5508d8393b4) |
| LLCC ISLAND1 table | releasing the LPASS votes exposed it: reset on every suspend | §2.2 (K:1d5861e2ee37) |

Earlier (v0.3.0, suspend first enabled) the work was about *surviving* suspend, not reaching CX off: PCIe kept its
link and the GDSCs under it awake (`device_set_awake_path` on the controller and QMP PHY, GCC PCIe GDSCs with the
active-wakeup flag, K:0fc5e1634469, K:31f73d72b1b8; superseded in v0.8.0 by D3); fuel gauges stop I2C before the bus
suspends (K:13a696ff6c3b); touch stops IRQ transfers before the SPI bus suspends (K:e98efd360100); the charger hands back
(§2.5); keyboard power follows panel blanking (§2.9).

**Measuring it.** The RPMh sleep statistics live in AOP message RAM, node `sram@c3f0000` (`qcom,rpmh-stats`): three
32-byte records from offset 0x48, each {u32 name (4 ASCII chars: "aosd", "cxsd", "ddr"), u32 count, u64 last entered,
u64 last exited, u64 accumulated}, timestamps in 19.2 MHz arch-timer ticks; a DDR-stats block at offset 0xb8 starts
with magic 0xA1157A75 (`drivers/soc/qcom/qcom_stats.c`). Per-subsystem stats are SMEM items (APSS 631, ADSP 606, ADSP
island 613, GPU 609, display 610). Ubuntu's pass criterion was cxsd and ddr accumulated ≈ time asleep. It does not report
aosd, the AOSS deep-sleep (XO shutdown) counter (*inferred* meaning; not reached or not checked).

**Pitfalls and fixes in order of appearance.**
- `e6da2db2e7bb` (v0.8.0): the charger's PM prepare refused sleep with -95 on battery after any charge session.
- `7dffd78b342d` (v0.8.0): unplugging the cable during s2idle ran the UCSI role switch on a non-freezable workqueue before
  devices resumed; the DWC3 glue wrote the QSCRATCH VBUS override with its clocks off; "the access does not complete and
  the SoC resets". Fix: while the glue is suspended only record the override and apply it after the core resumed, under
  one lock with suspend/resume; v1.1.0 shares that lock with the Type-C clock (UTMI-as-PIPE) updates (K:9834284b801f).
- `08e750858038` (v1.1.0): the thermal core freezes zones at the start of prepare; a device suspended warm then cools
  through the last lower thresholds; the level interrupt cannot be serviced or cleared, refires, and being wake-armed
  aborts suspend or wakes s2idle at once. Fix: mask every sensor's *lower* threshold interrupt in prepare; upper and
  critical stay armed; the first post-resume zone update re-arms both.
- `a2b4feb14264`, `386c1f901aa0` (v1.2.0): GENI UART: the serial core marks the port suspended before shutdown; an IRQ
  answered "not handled" in that window left a level interrupt unacknowledged and the IRQ core disabled the Bluetooth
  UART. Fix: stop RX/TX, clear sequencer and DMA status, mask the SE IRQ and wait for in-flight handlers before runtime
  PM removes power; keep it masked across the whole sleep transition.
- `19114681e92c` .. `2ad18a4de500` (v1.1.1): an ADSP fatal error after suspend (a FastRPC listener assertion) led to an
  SMP2P "sleepstate" notifier (§2.13); it ships **disabled**, so CX-off sleep works without telling the DSP.
- Not fixed in Ubuntu: USB hub/DisplayPort after resume (device.md §2.1), keyboard early wakes (§2.3).

### 2.2 LLCC: the ISLAND1 slice

- Facts (*code*, `drivers/soc/qcom/llcc-qcom.c`, DT `system-cache-controller@24800000`): SM8750 uses the LLCC v6 register
  layout; four banks at 0x24800000, 0x25800000, 0x24c00000, 0x25c00000; broadcast-OR base 0x26800000 and broadcast-AND
  base 0x26c00000; EDAC IRQ SPI 266. Per-slice configuration through the broadcast base: TRP_ATTR0 at 0x41000 + 64·SCID
  (reserved-way mask), ATTR1 at +8 (bonus-way mask), ATTR2 at +0x10 (cache mode, probe-target ways, fixed size,
  priority, parent SCID), ATTR3 at +0x14 (max capacity per bank in cache lines); then one bit per SCID (word SCID/32)
  in the write-sub-cache, stale and MRU/allocation algorithm registers from 0x42008 to 0x42088; slices flagged
  activate-on-init are then activated (`_qcom_llcc_cfg_program_v6`).
- ISLAND1 = usecase LLCC_ISLAND1, **SCID 12**, max_cap 7936 KB, priority 7, fixed size, **res_ways 0x7fffffff, no bonus
  ways** (the downstream table; the mainline v2 conversion had turned them into bonus ways). So ATTR0 at 0x26841300
  should read 0x7fffffff and ATTR1 (0x26841308) 0 before CX-off sleep is attempted.
- Why (*measured*, K:1d5861e2ee37): with the LPASS votes released and bonus ways, all 16 suspends that went to sleep
  reset (PMIC PON log: PS_HOLD only); holding LPASS_HW_MACRO_VOTE avoided the reset but CX then did not collapse; with
  reserved ways 6/6 s2idle cycles of 48–60 s resumed. "Why the missing reservation ends in a reset is not visible from
  Linux." At runtime the ADSP enters island with either table.
- Kernel build note: the LLCC driver is built in since v0.6.1 (K:fe98a3eca32d), so the table is programmed early.

### 2.3 Wake sources and IRQ routing (PDC, TLMM, SPMI arbiter)

**PDC** (*code*, DT `pdc: interrupt-controller@b220000`, `drivers/irqchip/qcom-pdc.c`). Registers 0xb220000 (0x10000)
plus the optional SPI-interface region 0x164400f0 (0x64). `qcom,pdc-ranges` = (first PDC pin, first GIC **SPI**, count):
(0, 745, 51), (51, 527, 47), (98, 609, 32), (130, 717, 12), (142, 251, 5), (147, 796, 16). GIC INTID = SPI + 32.
- Version register at +0x1000 (major 23:16, minor 15:8). From version 3.2 each pin's enable is bit 3 of IRQ_i_CFG;
  before 3.2 there is an enable bank at +0x10 (one bit per pin). IRQ_i_CFG is at +0x110 + 4·pin, type in bits 2:0:
  level-low 000, falling 010, level-high 100, rising 110, both edges 111.
- The PDC converts what it forwards: falling and both-edges are presented to the GIC as rising, level-low as
  level-high; the GIC side is always configured rising or level-high. After a type change the GIC pending bit is cleared.
- **SPI interface register** (withdrawn series K:81bc110b53c3 → K:cd3e49bae2b5 → K:ff6cf0aafb8f): one bit per GIC SPI,
  word = SPI / 32 at 0x164400f0 + 4·word, bit = SPI mod 32, **1 = level, 0 = edge**. Firmware initialises it with
  common types; Linux never wrote it after v1.2.0. With the original level bits, GPIO80 and GPIO95 were pending at CX
  restoration in three reversible A/B pairs; with only those two bits set to edge all three short windows reached the RTC
  alarm; a longer window still woke from GPIO95 after ~375 s (*measured*, K:cd3e49bae2b5). The declared 0x64 bytes stop
  at SPI 799, so SPIs 800–811 (the last range: pen-dock hall pins) are outside it; the stock DT has the same size; "there
  is no verified register map authorizing a larger resource" (K:ff6cf0aafb8f).

**TLMM** (*code*, `drivers/pinctrl/qcom/pinctrl-sm8750.c`, `pinctrl-msm.c`; DT `tlmm: pinctrl@f100000`, 216 GPIOs,
summary IRQ SPI 208). Per GPIO n at 0xf100000 + 0x1000·n: CTL +0 (pull 1:0, drive from bit 6, OE bit 9, eGPIO 11/12,
strong I2C pull-up bit 13, K:f3621a953f3a), IO +4, INTR_CFG +8, INTR_STATUS +0xc. INTR_CFG: enable bit 0, polarity 1,
detection 3:2, raw-status 4, **wakeup-present 6 (read-only), wakeup-enable 7**, target from bit 8 (APSS = 3).
- For every GPIO in the PDC map Linux sets `skip_wake_irqs`: the TLMM summary interrupt is not used; the GPIO's
  interrupt is owned by the PDC. When the IRQ is requested Linux sets INTR_CFG bit 7 if bit 6 reads 1, and clears it on
  release. The driver comment: the bit is needed "to allow the interrupt event to be transferred to the PDC/MPM HW ...
  it's also required for the interrupt event", i.e. not only for wake. The type is programmed in PDC and GIC only.
- GPIOs without a PDC entry use the TLMM summary IRQ and cannot wake the SoC from system-level sleep (*inferred*).
- Firmware-owned GPIOs Linux must not touch: 36–39, 48–51, 74 (`gpio-reserved-ranges`, K:93db8797d995).

**SPMI arbiter** (DT `spmi_bus: spmi@c400000`): core 0xc400000, channels 0xc500000, observer 0xc440000, interrupt
accumulators 0xc4c0000, config 0xc42d000; `qcom,ee = <0>`, channel 0; its `periph_irq` is **PDC pin 1, level-high
(SPI 746)**. All PMIC interrupts (PON, RTC, PMIC GPIOs) reach the CPU through it (`drivers/spmi/spmi-pmic-arb.c` is the
specification for per-peripheral enable and the accumulated status).

**Wake sources Ubuntu arms** (v0.3.0 on; *code* + device.md for behaviour):

| Source | Pin / peripheral | Route | Trigger | Notes |
|---|---|---|---|---|
| Lid (cover open) | TLMM GPIO80, active low | PDC 122 → SPI 633 (INTID 665); SPI-cfg word 19 bit 25 | rising only (open) | `gpio-keys` SW_LID, `wakeup-event-action` deasserted; closing never wakes (K:b96cd03f088b) |
| Keyboard wake request | WN8030 status pin, GPIO95 | PDC 128 → SPI 639 (INTID 671); word 19 bit 31 | rising, one-shot | only when panel blanked and lid open (K:f15d206ba17e, K:afe204252af2) |
| Keyboard data | GPIO97 | PDC 100 → SPI 611; word 19 bit 3 | rising | not a wake IRQ since v1.1.1 (K:6824e7cdc219) |
| Power key | PMK8550 (SID 0) PON "hlos" 0x1300, IRQ 7 | SPMI arbiter → PDC 1 | both edges | KEY_POWER (`pmk8550.dtsi`) |
| Volume down | PMK8550 PON resin, IRQ 6 | SPMI arbiter | both edges | KEY_VOLUMEDOWN (Ubuntu does not list it as a wake source) |
| Volume up | PM8550 (SID 1) GPIO6 (peripheral 0x8d00), active low, pull-up | SPMI arbiter | 32 ms debounce | `wakeup-source` |
| RTC alarm | PMK8550 RTC 0x6100, alarm 0x6200, IRQ 1 | SPMI arbiter | rising | wall-clock offset in SDAM2 byte 0x72 (SID 0 0x7172), 4 bytes (K:affe1b0b3ec4) |
| DSP | SMP2P `sleepstate_see` | IPCC (SPI 229) | both edges | only with the opt-in notifier (§2.13) |

Other lines for reference (not wake sources in Ubuntu): touch GPIO162 → PDC 109 → SPI 620; pen-dock transmitter IRQ
GPIO78 → PDC 115 → SPI 626 (falling); table hall GPIO11 → PDC 76 → SPI 552; ADSP watchdog → PDC 6 → SPI 751 (rising);
the GENI serial engines and IPCC are plain GIC SPIs (touch SPI2 0xa88000 SPI 355, keyboard I2C6 0xa98000 SPI 363, gauge
I2C5 0xa94000 SPI 358 and I2C11 0x88c000 SPI 585, backlight I2C8 0x880000 SPI 373 and hub SE3 0x98c000 SPI 467, IPCC SPI
229, UFS SPI 265).

### 2.4 GENI serial engines around sleep and idle

- UART (`drivers/tty/serial/qcom_geni_serial.c`): force runtime suspend/resume in the system-sleep callbacks (v0.8.0);
  quiesce order before power removal: stop RX and TX under the port lock, clear the sequencer and DMA interrupt status,
  mask the SE IRQ, synchronise with running handlers; keep the IRQ masked for the whole transition; consoles kept alive
  by `no_console_suspend` are skipped (v1.2.0, §2.1).
- SPI with GPI DMA (`drivers/spi/spi-geni-qcom.c`): hold a CPU latency request of 2000 µs from prepare_message to
  unprepare_message, GPI mode only (K:322fd1bfbda9). A bandwidth vote alone did not help (the same commit); an earlier
  bandwidth-vote patch and a GPI ring-ordering patch were reverted for lack of evidence (K:1d02527d1da8, K:f35c6802905d,
  reverts K:bdb6d4e2617e, K:4c54b17cc1be).
- Board fact: some SEs have FIFO mode disabled by firmware and must use GPI DMA (desktop DT enables `gpi_dma1`,
  `gpi_dma2` for that reason).

### 2.5 Charging: transport, firmware defaults, sequences, sleep

**Transport** (*code*, `drivers/soc/qcom/pmic_glink*.c`, `drivers/power/supply/xiaomi_mca.c`,
`drivers/usb/typec/ucsi/ucsi_glink.c`, `include/linux/soc/qcom/pmic_glink.h`). pmic_glink over the ADSP's GLINK edge.
Header {u32 owner, u32 type (1 request/response, 2 notification), u32 opcode}. Owners: battery manager **0x800a** (on
piano answered by Xiaomi's MCA firmware, not Qualcomm battmgr), UCSI **0x800b**, USB-C PAN **0x800c**.
- MCA request, 276 bytes: header (owner 0x800a, type 1, opcode 1 read / 2 write), u32 property, u32 sequence, 256-byte
  data (scalars in the first 4 bytes). Reply, 280 bytes: header, property at 12, return code at 16 (0 = ok), sequence at
  20, data from 24. One request in flight, 1 s timeout, match on opcode + property + sequence (K:1e4a0fc89fee).
  Notification: header (type 2) + u32 id + 256 data = 272 bytes; ids 0 type change, 3 boost, 5 Type-C change, 10 OTG
  charge-pump configuration (K:1cc1d6544880). Qualcomm battmgr's 0x30 property reads and its notification request get
  replies with return code ≠ 0 (K:cc36810ca732).
- PAN: request {header (owner 0x800c, type 1, **opcode 0x15** = USBC write request), u32 cmd, u32 arg, u32 reserved};
  cmd 0x10 = enable notifications (arg 0), 0x11 = ack (arg = port index). The DSP answers each write with opcode 0x15
  (one outstanding write, 5 s timeout). Notifications arrive as opcode low byte 0x16 with the SVID in bits 31:16. Linux
  sends the enable from a work item after PDR "up"; it acks port 0 (described in DT) after processing every
  notification; `pan_ack=1` additionally acks ports without a DT connector (K:d998b2273637, K:889ae838e1c7).
- UCSI: owner 0x800b; read-buffer request opcode 0x11, write-buffer 0x12, notification 0x13; registered (full UCSI init)
  on PDR "up" and unregistered on "down" (K:2a5ccfa32100 fixes failed registration).
- PDR: on the `PMIC_RTR_ADSP_APPS` channel pmic_glink looks up service `tms/servreg`, path
  `msm/adsp/charger_pd` and gates every client on its SERVREG state; only the SoCCP channel variant assumes "up"
  without PDR (`pmic_glink.c`). Each DSP restart is a new "service generation"; the MCA driver
  re-applies board configuration, defaults and policy per generation.
- GLINK fix (v0.6.2, K:235ab34e2a6e): the DSP may send as soon as it was granted an intent; a message arriving before
  the local endpoint callback exists (or while the client's probe returns "try again") was dropped and its intent
  returned. The lost message was the remote name service's QRTR HELLO; the local side only answers a HELLO, so the two
  never exchanged servers: 1 cold boot in 26 had every channel open but no PDR, no GPR, no sound, no Type-C, no sensors,
  no charger. Fix: queue complete messages per channel until the callback can take them; return the intent only after
  delivery.
- Handover IRQ: the piano ADSP raises the q6v5 handover signal again "every time the remote processor comes back from a
  power collapse"; it must be one-shot per boot of the DSP (K:9adeb3213d24 → reverted for upstream K:bdd6f6b688b0,
  K:4baf6bdaa1a5).

**Properties** (*code*, `xiaomi_mca.c`; ids from the downstream MCA sources and the stock DT, see K:fd03bee1a1ee):

| Id | Name | Width / unit | Linux use |
|---|---|---|---|
| 0x2001 | charger status | u32 | read, raw only |
| 0x2003 | charging enabled | 1 byte | write only for battery-protection stop/start (K:ea5c83ee493f) |
| 0x2005 | charge current (FCC) | mA | write; reads back 0 although accepted, trust the ack (K:886f911c4d11) |
| 0x2007 | float voltage | encoded (reads 5200 for 4400 mV) | write only with `full_charge` (4480 / 4060 mV, K:bdd0228f991f) |
| 0x2008 | input current limit | mA | write + readback; the readback is the *requested* limit, not the one in force |
| 0x2012 / 0x2029 | AICL enable / status | u32 | read, raw only |
| 0x2019 | input suspend | u32 | read, raw only; **never written** |
| 0x200d / 0x200e | OTG boost / gate status | | read (diagnostics, K:3ff90b4ce987) |
| 0x200f / 0x2010 | pen boost enable / voltage | | dock pen regulator (K:f90f8471331e) |
| 0x201a | OTG_CFG | 16 bytes: words 0, 1, 0xffffffff, 0xffffffff | board config, per DSP generation |
| 0x201b | CID_CFG | u32 0x708 (stock `cid_gpio_int`, a firmware id, not an AP GPIO) | board config |
| 0x201c | CP_STATE | 1 byte: SC8541 charge pump present | board config, first |
| 0x202a | battery temperature | °C | policy input |
| 0x20001 / 0x20002 / 0x20004 | USB online / bus µV / bus µA | | telemetry; bus current reads 0 "once charging is done" |
| 0x2000c | real charger type | enum: 1 SDP, 2 CDP, 3 DCP, 4 float, 5–8 HVDCP 2/3/3B/3.5, 9 Type-C, 10 PD, 11 PD verified, 12 PPS, 15 OCP | gates every write; unknown types refused |
| 0x21002 | verify_process | non-zero during adapter authentication | must be cleared on every exit path |
| 0x21003..0x2100a | MiPPS VDM commands 1–8 | 16 bytes | kernel computes the digests (K:f5b1de558983) |
| 0x2100c / 0x2100d | data role (1 UFP, 2 DFP; write = swap) / PD state (2 = SNK_Ready) | | authentication needs DFP |
| 0x2100e / 0x2100f / 0x21010 / 0x21012 | adapter id / SVID / PD verified / UVDM state | | |
| 0x21011 | source PDOs | up to 7 × {min mV, max mV, max mA} | |
| 0x21015 / 0x21020 / 0x21021 / 0x21022 | Type-C mode / OTG plug-in / CC toggle (1 byte, root-writable, K:a5cf45da243b) / sink-source mode | | read-only telemetry except CC toggle |
| 0x21017 / 0x21018 | PPS APDO select / fixed PDO request (mV) | | 9 V and PPS |

**Firmware defaults and Linux observations.**
- With no host client: requested ICL 100 mA, charge current 0 (K:fd03bee1a1ee); float 4400 mV (K:bdd0228f991f); JEITA
  inside the firmware (0–55 °C window; the downstream table further limits current below 8 °C and float above 48 °C,
  which Linux does not replicate, K:fd03bee1a1ee).
- The read-only build of 2026-10-09 (Linux 7.2.6, no write ever made) logged an SDP source at 5.041 V with **394 mA**
  bus current while 0x2008 read 100 and the real type read 1 (SDP) (*measured*, test frames in
  `drivers/power/supply/xiaomi_mca_test.c`). The firmware also sent no unsolicited message in 125 s and rejected the
  battmgr notification request (K:1e4a0fc89fee).
- The first write ever accepted was ICL 500 mA (request words 0x800a, 1, 2, 0x2008, seq 0x22, 0x1f4).

**Sequences Linux runs** (*code*):
1. PDR "up" for `charger_pd` → service generation + 1.
2. Board configuration (v0.5.0 on; stock `qcom_subpmic_sync_cfg_work` order): CP_STATE (1 if the SC8541 answered) →
   OTG_CFG (16 bytes above) → CID_CFG 0x708; any failure → retried on the next poll; success suppresses repeats for the
   generation (K:cf4d132fdee4, K:27490ca3d1f4, K:de5a8c2b874f).
3. PAN enable (altmode client), UCSI registration (ucsi client), both on the same PDR "up".
4. Poll every 2–3 s (online, status, type; policy reads), and on notifications.
5. Policy, only after userspace sets `charge_policy=1` (device.md §2.5): reject unknown types; per-type ICL/FCC
   (SDP 500/500, CDP 900/900, DCP and 5 V QC/PD 1500/2000, float/Type-C fallback 500); 9 V fixed PDO after it settles
   ≥ 8.0 V (1600/3000), PPS/verified (2500/3500); FCC temperature derating; the SC8541 pump only on PPS. On detach the
   limits go back to 100 mA.
6. Before system sleep (`prepare`): stop the charge pump (verified: CHG_EN read back off, else refuse suspend,
   K:f768a5585abc, K:7e3c6dd57e2b), return the source to 5 V, keep the **attached type's 5 V limits** (not 100 mA,
   K:e9acb111dc96, v1.1.0), finish authentication cleanup (K:f7388e42cb05), then block all transactions until
   `complete`; with no VBUS there is nothing to hand back (K:e6da2db2e7bb). With battery protection on and wired power
   below the limit, hold a wakeup source and refuse prepare with -EBUSY (stock `online_wake_lock`, K:80eeae96d596).

**Pitfalls.** Charging lost after some resumes (device.md #18: online 0, VBUS 0, replugs ignored until the driver
rebound and re-sent everything); FCC readback 0 misread as failure (K:886f911c4d11); authentication deadlock when the
SVID was read before becoming DFP (device.md #20); null-pointer and boot oops from pmic_glink child nodes that are not
connectors (K:c7eb9eea767c, K:d50fa91b421a, K:bee9d703c511).

### 2.6 Display on/off and backlight

**Panel** (*code*, `drivers/gpu/drm/panel/panel-novatek-nt36532.c`, DT `panel@0`): NT36532 TDDI, two bonded DSI links,
DSC 1.1 (2 slices of 800 × 24, 10 bpc, 8 bpp), 3200 × 2136. Two modules selected by the ID pin GPIO100: "42-02-0a"
(CSOT) and "35-02-0b" (BOE) with different init streams (BOE writes rate registers B2 then B3, K:b18aad686851). Reset
GPIO98 (active low). Supplies: vddio = L12B at 1896 mV (stock requests 1.9 V, the NLDO grid gives 1896, VSET 0x0768,
K:32efa36968f4); avdd/avee = fixed 5.8 V rails switched by GPIO117 (vsp) and GPIO118 (vsn), generated by the KTZ8866's
LCD bias block and left on by firmware. Modes 144 / 120 / 90 / 60 Hz; the firmware scanout runs **120 Hz** (INTF frame
counters, vertical total 2310: 2136 active + 68 front + 104 back + 2 sync) and v0.8.0 made 120 Hz the desktop default.
- Power on: vddio, avdd, avee, each followed by 2–3 ms; reset physical low 10 ms, high 3 ms, low 3 ms, high 15 ms (both
  modules, stock `<0 10 1 3 0 3 1 15>`, K:709f199e8a49); init stream; exit sleep + 120 ms; display on + 10 ms.
- Power off (P81 timing, K:72cfce4c86e5): display off, wait 20 ms; enter sleep, wait 100 ms; assert reset; supplies off
  in reverse order with post-off waits avee 2 ms, avdd 3 ms, vddio 2 ms.
- Keep both DSI clock lanes in HS while the display is on: with non-continuous clock the slave link's clock (taken from
  link 0's PHY) stopped ~174 times per frame; some units showed a corrupted right half (K:5ab22dc1564d, v0.9.1).

**Teardown order** (*measured* fixes): DPU: stop the slave's timing engine, then the master's; reset the shared CTL only
after both have stopped (poll the slave INTF up to 40 ms) — resetting it while the master scanned produced an underrun
the panel showed as a garbled frame on every screen-off (K:c55958fa09dd, K:b7a813e2245c). DSI: power off host 1 (clock
consumer) before host 0 (clock source), PHYs after both (K:608ff1b238a0); save/restore the SM8750 slave PHY's own PLL
divider/mux state across PHY reset (CMN_CLK_CFG0 0x51 on both PHYs at 120 Hz; after DPMS the slave read 0xf1 and
overflowed its FIFO, K:c6a971f9bedb). Boot handoff: MDSS stream SID 0x800 must stay identity-mapped while the splash
scans out (K:4ccf0de173b4); the MDP RCG must not be parked at registration (575 MHz from DISP_CC_PLL0 at boot,
K:404e5aa42d87); the firmware framebuffer needs its own DDR bandwidth vote once interconnect sync drops the boot floor
(avg 3 280 896 kB/s, peak 4 257 792 kB/s at 120 Hz: the `chosen/framebuffer` comment in `sm8750-xiaomi-piano.dts`,
mechanism K:33750614d8f7).

**Backlight** (*code*, `drivers/video/backlight/ktz8866.c`, K:f4a810d07ea2, K:e71bf8d6f0b0): two KTZ8866 at I2C 0x11,
each driving half the LED strings: primary on I2C hub SE3 (0x98c000) owning HWEN = GPIO2 (active high); secondary on
QUP2 SE0 (I2C8, 0x880000), no HWEN. Registers: brightness 11-bit, low 3 bits in 0x04, bits 10:3 in 0x05; BL_EN 0x08 =
bit 6 enable + bits 5:0 sinks: **0x7f on, 0x3f off** (6 sinks); FLAG 0x0f clears on read; max 2047 (stock maps normal
levels to ≤ 1952). Write brightness before enabling sinks; write the primary then the secondary with the same value.
**Never drive HWEN low**: it resets every register including the LCD bias the panel runs from. At probe read BL_EN and
the brightness and keep the boot state if lit. Buses at 400 kHz with the TLMM strong I2C pull-up (GPIO0/1, 70/71).

### 2.7 GPU power (Adreno 830, GMU)

Facts (*code*, `drivers/gpu/drm/msm/adreno/a6xx_catalog.c`, `a6xx_gmu.c`, `a6xx_hfi.c`, DT `gpu_opp_table`): firmware
gen80000_sqe/gmu/aqe, GMEM 12 MiB, quirks include **IFPC** and preemption (mainline A830 entry has an IFPC register
list); GPU OPPs 160 (LOW_SVS_D3), 222, 342, 389, 443, 525, 607, 660, 734, 832, 900, 967, 1050, 1100 MHz (the top
ones gated by speed-bin `opp-supported-hw`); GMU OPP table 500/650 MHz.
GPU cooling throttles at 105 °C (mainline K:b01d84d78209). Ubuntu profiles keep the GPU minimum at 160 MHz; power saver
caps at 342 MHz.
- **MX dependency votes** (K:cbb3f4900bb4, upstream fix; local analysis K:48879e6e07ab): the per-level MX index sent
  in the A8xx performance table must be looked up on the GPU's OPP table. Looked up on the GMU's (500/650 MHz) every
  level got MX index 0; once rpmhpd had synced, the only MX vote left was APPS at LOW_SVS (64), and the first
  GX_BW_PERF_VOTE (1100 MHz, GFX TURBO_L3 = 448) timed out, then OOB GPU_SET, then hw_init (-110). With the right table
  1100 MHz carries MX index 9 (448). The bug hides while all rails are held at maximum before sync_state.
- **GMXC** (K:925bbbba6eca, *code*, "not booted yet" at the time): the GX memory rail's level table starts 0, 16, 50,
  52 …; enabling it "at the first non-zero level" voted retention (16) while GFX got 50; cmd-db's minimum operating level
  is 50 and the downstream DT floors it at MIN_SVS (48). Skip retention as for MXC. RPMh addresses seen in the trace:
  gfx 0x30050, gmxc 0x300d0.
- HFI interrupt: clear only the CM3-fault bit in GMU2HOST_INTR_CLR, never the bits the driver polls (MSGQ, OOB acks);
  otherwise an interrupt landing between the GMU's answer and the poll loses the answer (K:891bc63f703a). Mask all of
  GMU2HOST_INTR_MASK before booting the GMU: it reads 0 after GX power-off and every HFI response raises the line until
  programmed (K:ce9c0ed98aab).
- Bandwidth: on GMUs that vote DDR themselves, remove the kernel's boot bus vote when the GMU takes over; otherwise DDR
  stays at its top frequency while the GPU is powered (K:97567648b85d).
- GMU firmware init timeout 100 ms (KGSL's), not 10 ms (K:d86e3f7b2204). A fault-devcoredump wait inside GMU calls is
  bounded to 2 s (K:6565c5044f6d).
- Sleep relevance: the GMU needing a bound driver (K:bcfcfb70be9c) was a v0.8.0 CX-off blocker (§2.1).

### 2.8 CPU DVFS and thermal

- **DVFS = SCMI performance protocol (0x13)**, no cpufreq-hw/EPSS node on SM8750 (*code*, DT `firmware/scmi`):
  transport = CPUCP mailbox `mailbox@16430000` with regions 0x16430000 and 0x17830000, IRQ SPI 26; channel 0 = tx,
  2 = rx; shared memory 0x17b4e000, two 0x200-byte areas (tx at +0, rx at +0x200). Perf domain 0 = CPU0–5,
  domain 1 = CPU6–7. Energy model from SCMI's own power estimates; the DT dynamic-power coefficients were dropped as
  unused (K:fa9b92356393). Capacities 1792 (CPU0–5) and 1894 (CPU6–7) → normalised 836 / 1024 (K:fa9b92356393).
- Frequencies used by Ubuntu (device repo `device/power/tuned`): cluster 0 max 3532.8 MHz; cluster 1 max 4089.6 MHz
  non-boost, 4320 MHz with boost; power saver 1785.6 / 1958.4 MHz. Governor schedutil.
- **Trips** (DT, K:4689914b1ea2, K:549976b18fb6): stock Android runs the prime cores at 4.32 GHz with per-core zones up to
  104–105 °C (never above 105.5 °C in 2410 samples). Nine per-core zones: passive 105 °C, 5 °C hysteresis, **10 ms
  passive polling** (the hotspot rises ~200 °C/s; a 250 ms poll overshot by ~10 °C; stock polls 4 ms); seven other
  per-core zones: 118 °C / 8 °C (stock uses core pause there; mainline throttles the cluster); CPUSS zones: no passive
  trip; hot 120 °C, critical 125 °C everywhere. With the old 95 °C trip a single prime-core load reached it within half
  a second (34.5 → 91 °C in 0.26 s) and step_wise oscillated 3.51–4.20 GHz.
- tsens: four instances 0xc228000–0xc22b000 (uplow and critical IRQs; instance 0 uses SPI 771/484), 15 sensors on
  instance 0. Lower-threshold interrupts must be masked across suspend (§2.1).
- Board thermistors (v0.7.0, K:898cec7421ca, K:43dd63f2f45a): 11 NTC channels (100 kΩ pull-up, 1:1): one on PMK8550,
  six on PM8550, four on PMIH0108 (Gen4 ADC, 16-bit ratios); names xo, msm, flash, wifi, display, backlight, conn, conn2,
  battery, quiet, charger1; PM8550 GPIOs 2–4 and PMIH0108 GPIOs 6 and 12 must be high-impedance; PMK8550 SDAMs 0x9000
  and 0x9100 carry the ADC5 Gen3 conversions. No ADC threshold monitoring or cooling policy is attached.

### 2.9 Keyboard cover (WN8030), touchpad, hall

Wiring (*code*, DT `keyboard@4c` on I2C6 0xa98000 at **1 MHz**, no internal pull-up): data IRQ GPIO97 (rising), status
GPIO95, reset GPIO188 (active low), sleep GPIO3 (active low); rails L14B at 3296 mV (stock asks 3.3 V, which the
8 mV grid cannot give; VSET 0x0ce0) and L11B at 1800 mV, neither voted until a consumer enables it (K:a66598f919ef,
K:9b5479c44a22). Firmware `nanosic/piano/MCU_Upgrade.bin`.
- Power follows the panel (stock `nanosic_driver.ko` offsets cited in K:64f228ab0389): on blank send the 66-byte
  command 0x25 = 0 (checksum 0x5d), then assert sleep; on unblank release sleep, wait for the data line high (at most
  100 ms polled every 1 ms, then 5 ms settle only if it had to wait), drain pending frames until an empty frame, send
  0x25 = 1 (checksum 0x5e), restore LEDs/backlight. No periodic wake: the community 12 s keep-alive was removed; stock
  keeps awake 10 s only during authentication (K:24142165d4f9). System suspend runs the same sequence if needed and gates
  all producers before the I2C bus suspends; a resume without a panel-enable event sends nothing.
- Wake: the status pin is the host wake request (stock requests it rising + one-shot); stock reports KEY_WAKEUP and
  takes a 1 s wakeup event only when the panel is blanked; while lit it is ignored (K:f15d206ba17e). The I2C core also
  attached the data IRQ as a wake IRQ for a `wakeup-source` client; removed in v1.1.1 because it woke the tablet just
  after suspending from a lit screen (K:6824e7cdc219). Not armed while the lid reads closed (K:afe204252af2).
- Attach state from the MCU's 0x38/0xa2 report (bit 0 attached, bit 1 powered); the MCU answers a 0xa1 request at probe
  either way (wait ≤ 500 ms); SW_TABLET_MODE = not attached (K:dcb09f4fb8d9). Touchpad presence auto-detected (v0.9.0).
- Hall sensors: lid GPIO80 (SW_LID; stock: ADSP SMP2P `mi_hall_lid`), table hall GPIO11 (stock SW_TABLET_MODE); the dock's
  pen hall inputs GPIO200, 196, 201, 197, 55, 59 and keyboard placement GPIO80/11 are read by the SC96231 pen transmitter
  driver, which stops transmitter and boost before suspend (K:e25fefe5c036).

### 2.10 Touch (NT36532E, THP)

Wiring (*code*): SPI2 (0xa88000) at 19.2 MHz over GPI DMA, IRQ GPIO162 rising, module ID GPIO100 selects the firmware
(CSOT / BOE); reset shared with the panel (TDDI).
- The touch must stop its IRQ before panel unprepare (which asserts the shared reset) and restart after panel *enable*:
  "power/prepare alone does not supply the display timing needed for touch scans" (K:770e5fdd47de, K:9ab0942f9734); the
  restart runs asynchronously outside the display commit (K:cb032046d810).
- Startup gates: wait for REK, then NORMAL_RUN (states 0xa3..0xaf) before host commands, 10 ms polls, 500 ms budget
  (K:5e26c272313f, K:5ed69822d95a); then BF04 = 0, BF19 = 1 (idle-baseline update), BF01 = 2 (op mode) written at event
  buffer +0x50, completion = firmware clears the command byte (110 ms budget) (K:4c676064f66d); disable unused stylus
  scanning first (K:70885ef74567). Without the BF commands the firmware never idled (scan mode 4 only) and the display
  flickered.
- Charger state: on external-power changes, while the stream is awake, select the event page (`ff 23 88`) then write
  `d0 53 00` (online) or `d0 51 00` (offline), i.e. host command 0x53/0x51 at 0x11c450; resend after every firmware
  start; only an external system-scope supply's ONLINE counts (SC8541's ONLINE is its CHG_EN) (K:bd04ba0e1289,
  K:722ec8462836, driver README).
- Sleep: stop transfers before the SPI controller suspends, keep open streams (K:e98efd360100). DMA vs deep idle: §2.4.

### 2.11 USB-C: DIO4485, roles, DWC3

- DIO4485 SBU/orientation mux at I2C 0x42 on I2C3 (0xa8c000, GPIO44/45), ID 0xf6 family; register bound 0x21
  (K:8de7b956a8d7, K:4e98895fb2c3). Orientation input GPIO61 (input, pull-down, 2 mA): low normal, high reverse, read
  by UCSI (K:8bc01b889c90). DP capped at the stock HBR2 rate (K:b364e5a90e68).
- Roles through UCSI over pmic_glink (K:b1eb75bcf65f). DWC3 glue: defer the VBUS-override (QSCRATCH) write while
  suspended (§2.1); use the UTMI clock as PIPE clock in DP-only pin assignments C/E, else the next xHCI reset times out
  (K:68d28640e1d1).
- OTG cooperation with the DSP: the SC8541 has firmware-directed reverse-gate sequences (K:0d2249b22782); notification 10
  hands the pump to OTG; boost status notification 3. Known Ubuntu limitation: a plain OTG adapter after unplugging an OTG
  device may not be detected.

### 2.12 Gunyah specifics

- PSCI: HLOS SMC calls; OSI and the system-level state are available to the HLOS VM; no context loss from the guest's
  point of view (§2.1).
- Watchdog: Gunyah emulates the APSS watchdog for the HLOS VM via vendor-hypervisor SMCCC fast calls (SMC32, owner 6):
  CONTROL 0x86000005 (enable = bits 0+1, disable = bit 1), STATUS 0x86000006, PING 0x86000007, SET_TIME 0x86000008
  (a 16-bit millisecond value, above 0x8000 reserved, so at most ~32 s). Before the driver existed "a hard hang never
  resets the tablet" (K:4ba1501653d1), i.e. it is not running after boot; the driver still stops it at probe, starts it
  only when userspace opens it, and stops it across system sleep (`drivers/watchdog/gunyah_wdt.c`).
- Memory lent to other VMs loses host stage-2 access; a read that crosses into such a page gets a synchronous external
  abort injected by Gunyah. Linux fixes up only the unaligned zero-padding load case (K:bde3f45d6a82). The hypervisor
  identifies as "Gunyah hypervisor 51/v1" on SM8850 (K:9041a4c985df).
- The VM-hosting stack (v1.0.0, `drivers/virt/gunyah/`) is imported from Android common kernel; its resource manager
  registers a PM notifier (hibernation restore of the address-space info area), which must be unregistered on unload or
  the next suspend jumps into freed module text (K:bacb9bce6055).

### 2.13 DSP sleep-state notification (off by default)

SMP2P on the ADSP edge carries an extra outbound entry "sleepstate" and inbound entry "sleepstate_see". The notifier
clears AP-awake bit 12 of the outbound entry on suspend-prepare and sets it after resume, and keeps the inbound
interrupt (both edges) as a wake source (K:19114681e92c, K:30f99539539e, protocol from Xiaomi's open-source
`smp2p_sleepstate.c`). v1.1.1 ships both entries and the consumer **disabled** (K:2ad18a4de500), a runtime parameter
enables it for the next cycle (K:a504cd537e94). CX-off sleep (v0.8.0) was reached without it.

## 3. Lessons for the macOS port (priority order, mapped to our components)

### 3.1 PianoPower: input held at 0 mA with the DSP running (P0)

Our level-2 run (docs/drivers/charging.md §5c): CDP from a Mac, bus 5.23 V, 0 mA, ICL write 900 accepted and read
back, charge enable 1, input suspend 0, AICL enable 1 / status 0x30, no port notification. Kernel-side facts that bear
on it, then what to try (device.md §3.1 has the full ladder; this adds the wire encodings):

- **Linux's baseline is current with zero writes.** SDP, 5.041 V, 394 mA while 0x2008 read 100 (§2.5). So the DSP's own
  defaults let input flow on Linux; the 100 mA readback is a *request*, not the limit in force. Nothing in the kernel
  ever writes 0x2019 or 0x2012. An un-suspend write would be a native-only difference.
- **Gate every MCA client on the charger PD being up** the way pmic_glink does: register a PDR listener for service
  `tms/servreg`, path `msm/adsp/charger_pd` (QMI service registry over QRTR; `drivers/soc/qcom/pdr_interface.c` is the
  spec), wait for state "up", and acknowledge state indications. Count a "service generation" from there and redo
  everything per generation.
- **Send the PAN enable in Linux's encoding** once up: owner 0x800c, type 1, opcode 0x15, cmd 0x10, arg 0, reserved 0;
  expect an opcode-0x15 reply within 5 s (no reply = that owner is not being served: a useful diagnostic in itself);
  then ack every opcode-0x16 notification with opcode 0x15, cmd 0x11, arg = port index, one write at a time. Ubuntu
  always acks port 0 and, with `pan_ack`, the unmodelled ports too.
- **Try a minimal UCSI client** (owner 0x800b; read buffer 0x11, write buffer 0x12, notification 0x13): Linux runs a
  complete UCSI init on every "up". Whether the DSP's Type-C/charger state machine waits for the PPM to be initialised
  is not shown by Ubuntu (*open*), but it is the largest protocol Linux speaks that we do not.
- **Send the board configuration** per generation before any policy write: 0x201c (1 byte, 1 = SC8541 present), 0x201a
  (words 0, 1, 0xffffffff, 0xffffffff), 0x201b (0x708). Linux added it for OTG in v0.5.0, after sink charging worked,
  so it is not proven necessary for input current.
- **Read the DSP's view of the port** in the diag line: 0x200d, 0x200e (OTG boost/gate), 0x21015 (Type-C mode),
  0x21020 (OTG plug-in), 0x21021 (CC toggle), 0x21022 (sink/source mode), 0x21002 (verify_process), 0x2100d (PD state),
  0x20001 (online). A DSP that thinks it is sourcing or authenticating would show here.
- **Never drop the DSP's first QRTR HELLO** (queue messages that arrive before our endpoint is ready, answer the remote
  name service's HELLO): losing it left Linux with every channel open but no charger service (K:235ab34e2a6e).
- Make the handover IRQ one-shot per DSP boot; the piano ADSP re-raises it after every power collapse (K:9adeb3213d24).
- Test conditions: 40–90 % battery, a wall DCP/PD charger as well as a computer port; judge by the gauges' summed current.

### 3.2 Suspend design: input for the k27 CPU idle / s2idle patch (P1)

1. **Idle states.** Use exactly the DT parameters: CPU 0x00000004 (per cluster latencies 93/129/560 µs and 172/130/686
   µs), cluster 0x01000054 (2150/1983/9144 µs), system 0x0200c354 (2800/4400/10150 µs), PSCI over SMC. Linux uses OSI:
   switch the mode once at boot (PSCI SET_SUSPEND_MODE, function 0x8400000f, after checking PSCI_FEATURES of CPU_SUSPEND
   for the OSI bit), then the last running CPU of a cluster may ask for the cluster param and the last CPU of the system
   for the system param; the OS keeps that bookkeeping and must accept a refusal (Linux counts rejected domain entries
   and simply returns). No param has the power-down bit, so the call returns like WFI: no warm-boot entry, no context
   save (*verify* the state format with PSCI_FEATURES first). If OSI is not wanted, PC mode gives only the CPU state
   (Linux disables the domain states without OSI).
2. **The RPMh sleep set is ours to manage.** Every vote our drivers make through DRV 2's active TCS (PianoPower's
   lcx/lmx proxy votes until the DSP's handover, PianoA830's GPU rails and bandwidth, any display/UFS BCM votes)
   survives into sleep unless it is dropped or a sleep set lowers it. Read DRV 2's solver bit (0x16520004 bit 24)
   natively. Simplest native design: drop every non-essential active vote before the system state is allowed; full
   design: program SLEEP (value 0) and WAKE (restore) TCS entries for each resource when the last CPU goes down, as
   `rpmh_flush` does, and refuse the system state while an active TCS is busy.
3. **Before the first CX-off attempt, check LLCC ISLAND1.** With the DSP running and no LPASS vote held (exactly our
   case: PianoPower starts the ADSP, nothing votes LPASS), Linux reset on 16/16 suspends unless SCID 12 had reserved ways.
   Read 0x26841300 (ATTR0) and 0x26841308 (ATTR1); expect 0x7fffffff and 0. If firmware left bonus-only ways, either
   program the slice (ATTR0..3 through the broadcast base, then the per-SCID algorithm bits; PianoA830's `Sm8750Llcc`
   already writes v6 slice attributes for the GPU slice and can be reused) or keep CX-off sleep disabled.
4. **Quiesce list per component** (v0.8.0 blockers translated; each is a runtime vote or clock that kept CX/XO/DDR up):
   - NativeUfs: hibern8, then gate *all* UFS clocks (the three lane symbol clocks and the controller/PHY/ref clocks)
     and the UFS GDSC; no extra references; no ICE.
   - PianoDwc3: never touch QSCRATCH or core registers with the glue clocks off; defer role/VBUS-override changes until
     after resume (SoC reset otherwise). With a cable to a host the HS PHY cannot enter L2 (extra drain, device.md §2.1).
   - PianoWifi: PCIe link down and root port D3 in sleep; do not vote RPMH_RF_CLK1 for normal operation.
   - PianoAudio: release LPASS_HW_MACRO_VOTE / LPASS_HW_DCODEC_VOTE when idle (100 ms autosuspend in Linux).
   - PianoA830: GMU slumber, GX/CX GDSC off, drop the boot bandwidth vote; mask GMU interrupts before the next boot.
   - Display (NativeGopDisplay / DPU path): §3.6 sequence, then MDSS GDSC off and the MMCX/bandwidth votes dropped
     (the firmware framebuffer path keeps a 3.28 GB/s average DDR vote while it scans out).
   - GENI users (PianoTouch, PianoKeyboard, any UART): stop transfers, clear SE status, mask the SE IRQ, wait for the
     handler, then gate clocks and bandwidth.
   - Thermal (when tsens interrupts are used): mask lower-threshold interrupts in the prepare phase.
   - PianoPower: §3.5.
5. **DMA latency holds.** The system state's 4400 µs exit stalls GPI DMA; PianoTouch transfers (and any DMA client
   with a deadline) need a "max exit latency 2000 µs" hold for the duration of the transfer, which the idle governor
   must honour (K:322fd1bfbda9).
6. **Verify with the RPMh stats** at 0xc3f0048 (aosd, cxsd, ddr records, 19.2 MHz ticks) and SMEM item 631 (APSS):
   pass = cxsd and ddr accumulated within ~0.1 s of the time asleep, as Ubuntu measured.
7. Do not depend on the DSP sleep-state SMP2P bit; Linux reaches CX off without it (off by default).

### 3.3 Polling → interrupts: routing facts to implement (P1)

For each line: TLMM INTR_CFG wakeup-enable bit 7 (if bit 6 reads 1) at 0xf100000 + 0x1000·gpio + 8; PDC IRQ_i_CFG type
and enable at 0xb220000 + 0x110 + 4·pin (check the version register at +0x1000 for where the enable bit lives); GIC
SPI as rising or level-high (the PDC inverts falling/low); EOI at the GIC. Optional and unproven: the SPI-interface
bit (0x164400f0 + 4·(SPI/32), 1 = level, 0 = edge); read and log firmware's value first, change only the bits of lines
we own, only below SPI 800.

| Component | Line | GPIO / source | PDC pin | GIC SPI | Type |
|---|---|---|---|---|---|
| PianoKeyboard | data | 97 | 100 | 611 | rising |
| PianoKeyboard | wake request (status) | 95 | 128 | 639 | rising (wake only while blanked, lid open) |
| PianoKeyboard / lid | lid hall | 80 (active low) | 122 | 633 | both edges for events; wake on rising (open) only |
| PianoKeyboard | table hall | 11 | 76 | 552 | as needed |
| PianoTouch | touch IRQ | 162 | 109 | 620 | rising |
| PianoInput, RTC | SPMI arbiter `periph_irq` | PON 0x1300 IRQ 7 (power), resin IRQ 6 (vol−), PM8550 GPIO6 (vol+), RTC alarm 0x6200 IRQ 1 | 1 | 746 | level-high |
| PianoPower | IPCC (GLINK, SMP2P doorbells) | — | — | 229 | level-high (plain GIC) |
| PianoPower / PianoA830 | APPS RSC DRV 2 (TCS done) | — | — | 5 | level-high (plain GIC) |
| PianoPower | ADSP watchdog | — | 6 | 751 | rising |

The SPMI arbiter needs its per-peripheral interrupt enables and the accumulated status read (spec:
`drivers/spmi/spmi-pmic-arb.c`); once it raises SPI 746, PianoInput's 10 ms poll can go.

### 3.4 PianoA830: GPU idle (P2)

Build the GMU perf table's MX dependency index from the GPU OPP levels (1100 MHz → MX 448); never enable GMXC at
retention (first level 50); clear only the CM3-fault bit on the HFI interrupt and mask all GMU→host interrupts before
GMU boot; once the GMU votes bandwidth, drop the kernel's boot vote; GMU firmware init timeout 100 ms. IFPC is enabled for
A830 in mainline (with its register list), so GPU-idle power collapse is the GMU's job once those are right; slumber on
inactivity follows `inactive_period`. Keep the GPU floor at 160 MHz as Ubuntu does.

### 3.5 Charging across sleep (P2, PianoPower)

Before allowing sleep: stop the charge pump and verify CHG_EN off (refuse sleep otherwise), return the source to 5 V,
keep the attached type's 5 V ICL/FCC (not 100 mA), pause MCA traffic until resume; with no VBUS skip the handback.
After resume re-sync everything for the generation (device.md #18: the firmware's port state can wedge until the host
re-sends it). On driver stop keep the last good limits (Linux's hand-back to 100 mA is what drained a running tablet).

### 3.6 Display off and backlight (P2, PianoBacklight + display path)

Off: backlight BL_EN 0x3f on both KTZ8866 (primary then secondary; never HWEN low, which also kills the LCD bias); DPU
slave timing engine, master timing engine, then shared CTL reset; DSI host 1 then host 0, then PHYs; panel display-off +
20 ms, sleep-in + 100 ms, reset low, avee/vsn (GPIO118) off + 2 ms, avdd/vsp (GPIO117) off + 3 ms, vddio L12B off + 2
ms; stop touch and keyboard before the panel's unprepare (shared reset, panel followers). On: reverse, panel reset
10/3/3/15 ms, init, sleep-out + 120 ms, display-on + 10 ms, restore the slave PHY divider state, then backlight
(brightness before BL_EN 0x7f). Keep DSI clock lanes in HS while on. Default refresh 120 Hz, the firmware's mode.

### 3.7 Keyboard and hall wake (P2, PianoKeyboard)

On panel blank: 0x25 = 0 then sleep GPIO3 low; on unblank: sleep high, wait ≤ 100 ms for GPIO97 high, drain, 0x25 = 1.
Arm GPIO95 (not GPIO97) as the wake line, and only with the lid open; arm GPIO80 for rising edge (cover open) only.
Expect early wakes from GPIO80/95 if the PDC SPI-interface bits stay level (§2.3); Ubuntu left this unresolved.

### 3.8 CPU DVFS and thermal (P2, PianoThermal)

CPU frequency is an SCMI perf-level set over the CPUCP mailbox (0x16430000/0x17830000, shmem 0x17b4e000, domains 0 and
1), as PianoThermal's `piano-cpufreq` already does. Thermal policy Ubuntu settled on: per-core 105 °C passive with
**10 ms polling while passive** on the nine hotspot zones, 118 °C on the other seven per-core zones, no passive trip on
CPUSS, 120 °C hot, 125 °C critical, GPU throttle at 105 °C. Mind the rate: a single prime-core load took a hotspot from
34.5 °C to 91 °C in 0.26 s (K:549976b18fb6), so a 1 s base poll that only speeds up within 10 °C of a trip can step
over the whole window; either keep the fast poll whenever a cluster runs above a low level, or arm the tsens upper
thresholds as interrupts (uplow IRQ; tsens0 SPI 771) and keep the lower ones masked across sleep. Prefer the SCMI
firmware's power numbers for any energy model.

### 3.9 USB-C (P3, PianoDwc3)

DIO4485 at I2C3 0x42 for SBU/orientation, GPIO61 for orientation, UTMI-as-PIPE in DP-only modes, roles from UCSI. No
QSCRATCH access while suspended.

### 3.10 Gunyah (P3)

The APSS watchdog is Gunyah's and stays stopped unless we start it (0x86000005..8); if we ever enable it, stop it before
sleep as Linux does. Lent pages raise synchronous external aborts on access; keep GPIO 36–39, 48–51, 74 untouched.

## 4. Still open in Ubuntu at v1.2.0

- Keyboard early wakes (GPIO95 after ~375 s even with edge SPI bits; series withdrawn), K:ff6cf0aafb8f.
- PDC SPI-interface register map beyond SPI 799 (pen-dock hall routes), K:ff6cf0aafb8f.
- DSP sleep-state notifier unvalidated, off by default; the ADSP fatal error after suspend not explained, K:19114681e92c.
- No AOSD/XO-shutdown result and no suspend drain figure published.
- USB hub and DisplayPort after resume (device repo issue, see device.md).
- GMXC retention change "not booted yet" when committed (K:925bbbba6eca); MX vote mask width left open (K:48879e6e07ab).
