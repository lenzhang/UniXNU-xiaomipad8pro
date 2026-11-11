# USB-C DisplayPort Alt Mode (B8): design

Status, 2026-11-11: **design, and the first slices built offline** (host-tested and linked, not run): `package/PianoDp` level 1
(D1 the read-only survey, D2 the DSP's port notifications through PianoPower's new read-only fan-out) and D6 (a fake second display
in NativeGopDisplay, no hardware), all inert without `piano-dp=N` (§8.1); the batched tablet test list is §8.2. No DP register is
written. Backlog item B8 (`../BACKLOG-hardware.md`); it builds on B1 (the DPU, `dpu.md`) and B7 (the Type-C port, `usb-host.md`).
Template: `README.md`.

## 1. Purpose and gate

Ubuntu v0.6.0 drives an external display over the USB-C port: both plug orientations, hot-plug, a 4K 60 Hz extended desktop;
v1.1.0 recovers USB after a DisplayPort-only unplug. Natively nothing drives DP: the boot stages never light it, so every register is
programmed from scratch. Gate: daily use with a monitor or a dock. Not on any login milestone, after B1b (zero-copy flips and the
display SMMU window) and B7's Type-C half (the DSP's port notifications decoded).

## 2. Hardware facts

Sources: the piano boot DT ("DT"); the Ubuntu piano kernel piano/dev 7f8d6bed (GPL, facts only): `drm/msm/dp/*`, the DPU 12.0
catalog `dpu_12_0_sm8750.h`, `phy-qcom-qmp-combo.c`, `dispcc-sm8750.c`, `pmic_glink_altmode.c`, `fsa4480.c`, `dwc3-qcom.c` ("msm",
"qmp", "glink"); `dpu.md` and `usb-host.md` for what is already established.

| Item | Value | Source | State |
|---|---|---|---|
| Controller | `displayport-controller@af54000`, `qcom,sm8750-dp` (driver data = sm8650's: controller 0, wide bus): ahb `af54000` (0x104), aux `af54200` (0xc0: HPD + AUX), link `af55000` (0x770), p0 `af56000` (0x9c), p1 `af57000` (MST, unused); MDSS hwirq 12 (MDSS = SPI 83); power domain MMCX; OPPs 270/540/810 MHz link = low_svs/svs_l1/nom; no SMMU stream of its own (it masters nothing) | DT, msm | verified (source); `status = okay` in the boot DT, nothing programmed: needs-boot |
| Endpoint | `data-lanes = <0 1 2 3>`, **`link-frequencies` 1.62/2.7/5.4 GHz: piano's stock limit is HBR2** ("stock limits the DP link clock to 540000 kHz") → at most 4 × 5.4 Gbit/s (17.28 Gbit/s of payload) | DT, Ubuntu dts K:b364e5a90e68 | verified |
| Graph | DPU INTF0 → DP → QMP DP input → connector SS port; DIO4485 on the connector's SBU port | DT | verified |
| DISPCC | AHB (6), DPTX0 AUX (16, TCXO 19.2 MHz, rcg 0x8234), LINK (19, src rcg 0x81e8 ← QMP `link_clk`), LINK_INTF (22, divider 0x8200), PIXEL0 (23, MND rcg 0x8204 ← QMP `vco_div_clk`), PIXEL1 (25, MST); branches 0x804c/0x8054/0x805c/0x8060/0x8064 | DT, dispcc-sm8750.c | verified (source) |
| Registers | aux: HPD_CTRL 0x00, HPD_INT_STATUS 0x04 (state b31:29), ACK 0x08, MASK 0x0c, REFTIMER 0x18, AUX_CTRL 0x30, AUX_DATA 0x34, AUX_TRANS_CTRL 0x38 (I2C 0x100, GO 0x200), AUX_STATUS 0x44; ahb: HW_VERSION 0x00 (major 31:28, minor 27:16, step 15:0; read once after a controller reset), SW_RESET 0x10, PHY_CTRL 0x14, INTR_STATUS 0x20/0x24/0x2c; link: MAINLINK_CTRL 0x00, STATE_CTRL 0x04 (TPS1..4 = b0..3, SEND_VIDEO b7, PUSH_IDLE b8), CONFIGURATION_CTRL 0x08 (lanes − 1 in 5:4), lane map 0x38, MAINLINK_READY 0x40; p0: TIMING_ENGINE_EN 0x10, INTF_CONFIG 0x14 | msm `dp_reg.h`, `dp_ctrl.c` | verified (source) |
| Clocked access | msm enables the core clocks (MDSS AHB + DPTX0 AUX) before any ahb/aux access, the link clocks (LINK + LINK_INTF) before the link block, PIXEL0 before p0; `msm_dp_snapshot` reads nothing while DP is off "to avoid crash due to unclocked access". The DPU's own snapshot (`dpu_kms_mdp_snapshot`) reads every CTL, LM and INTF (INTF0/3 included) with only the MDSS clocks on. AUX_DATA is the transaction FIFO (a read has an effect) | msm `dp_ctrl.c`, `dp_display.c`, `dpu_kms.c` | verified (source) |
| QMP DP part | v8 offsets: DP serdes +0x3000, TX A/B +0x3400/+0x3800, DP_PHY +0x3c00 (PD_CTL +0x18, MODE +0x1c, AUX_CFG0..9 +0x20..0x44); DP_COM (shared with USB3): PHY_MODE_CTRL +0 (b1 DP, b0 USB3), TYPEC_CTRL +0x10 (b1 software select, b0 reverse); AUX init PD_CTL 0x67, AUX_CFG = 00 13 a4 00 0a 26 0a 03 b7 03; rates 1.62/2.7/5.4/8.1 by serdes tables, VCO_DIV 1/1/2/0, vco_div_clk 810 MHz (RBR) / 1350 MHz; swing/pre-emphasis tables per rate band; supplies L2D 0.88 V, L3G 1.2 V; GDSC USB3_PHY. Read-only words (D1): DP_PHY REVISION_ID0..3 +0x00..0x0c, CFG +0x10, CFG_1 +0x14; sm8750's layout takes the v6 DP_PHY words VCO_DIV +0x70, AUX_INTERRUPT_STATUS +0xe0, STATUS +0xe4 (b0 tsync done, b1 PHY ready); DP serdes CMN_STATUS +0x2c8 (b0 freq done, b1 PLL locked), C_READY_STATUS +0x2f0 (b0); PD_CTL bits (1 = powered) PWRDN 0, PSR 1, AUX 2, LANE_0_1 3, LANE_2_3 4, PLL 5, CLAMP 6 (msm's power-off value 0x02) | qmp | verified (source) |
| Orientation | DP_COM TYPEC_CTRL reverse bit; DP_PHY MODE 0x4c reversed / 0x5c normal; with 2 DP lanes (pin D) the unused pair is powered down by orientation | qmp | verified (source) |
| DIO4485 | I2C3 (`i2c@a8c000`) 0x42: DP only (pins C/E) ENABLE 0xe0, SELECT 0x00 / 0x60 reversed; DP + USB (D/F) 0xf8, 0x18 / 0x78; USB 0x98, 0x18; SBU off + 35 µs before a change, 15 µs after | fsa4480.c | verified (source) |
| HPD | **from the DSP**, not a pin: PAN owner 0x800c, notification opcode 0x16 (SVID in opcode b31:16), payload @12 port, @13 orientation (0 normal, 1 reverse, else none), @14 mux (0 no connection, 1 USB3, 2 DP 4 lanes, 3 USB3 + DP, 4 tunnel), @15 reserved, @16 VID, @18 SVID (le16 each), @20 b5:0 pin assignment (0 none, 1 = A .. 6 = F), b6 HPD state, b7 IRQ_HPD (tunnelling: speed/cable bits), @28 reserved: exactly 32 bytes (Linux drops any other length); SVID DP with mux 0 = the safe state (an HPD-low report); enable = write request 0x15 cmd 0x10, ack = 0x15 cmd 0x11 arg port after every notification; `pan_ack` acks piano's two ports without a connector node (3 ports, 1 connector: `connector@0`, so port 0) | glink, DT, `usb-host.md` §2.3 | verified (source); PianoPower enables/acks PAN and, since 2026-11-11, hands each notification read-only to its port-event clients (PianoDp decodes) |
| Pin assignments | C, E: DP on four lanes, no USB 3 (USB 2 only): QMP "DP only", DIO4485 C/E words; D, F: two DP lanes + USB 3: QMP "USB3 + DP"; A, B (legacy): fsa4480 refuses them, msm's QMP mux takes any non-C/E DP state as USB3 + DP. At piano's HBR2 cap only four lanes carry 4K60 (12.8 of 17.28 Gbit/s; two lanes: 8.64, i.e. 1440p60 or 4K30): **4K60 needs pins C/E, and then USB 2 only** | qmp `qmp_combo_typec_mux_set`, fsa `fsa4480_set`, DT | verified (source) |
| USB in DP-only modes | pins C/E take all four lanes: USB3 is gone, USB2 continues with the UTMI clock as PIPE clock (QSCRATCH GENERAL_CFG +0x08), else the next xHCI reset times out (K:68d28640e1d1) | dwc3-qcom.c | verified (source) |
| DPU path | INTF0 `+0x34000` = DP (INTF3 `+0x37000` its MST pair); 6 CTLs, 8 LMs (pairs; **DSPP only on LM0–3**), PP0–7 (PP2/3 → MERGE_3D_1 `+0x4f000`), max mixer width 2560 (4K needs two LMs + MERGE_3D); the panel holds CTL0, LM0/1, PP0/1, MERGE_3D_0, DSC; INTF0 underrun/vsync = TOP0 bits 24/25 | catalog, `dpu.md` §2 | verified (source); the panel's allocation: needs-boot (B1a) |
| SMMU | all MDSS fetch uses stream 0x800 (`iommus = <0x800 0x2>`): an external display's pipes use **the same bank as the panel**, i.e. B1b's windows | DT, `dpu.md` §4.2 | verified |
| Bandwidth | MDSS interconnects `mdp0-mem` (MDP → EBI1) and `cpu-cfg`; no `mdp1-mem` on piano; 4K60 ARGB fetch ≈ 2.0 GB/s on top of the panel's 3.28 | DT | verified (DT); the vote path natively: open |

## 3. Ubuntu as the specification

- **Bring-up** (`dp_display.c` plug, `dp_ctrl.c`, `dp_aux.c`, `dp_panel.c`, `dp_link.c`): plug → runtime PM on → AUX on → PHY init (DP
  mode, orientation) → DPCD receiver caps (0x000–0x00f, LTTPR) → EDID over I2C-over-AUX (address 0x50, 128-byte blocks) → link
  training → stream. Training: CONFIGURATION_CTRL, DPCD 0x107/0x108 (downspread, 8b/10b), per LTTPR then sink; TPS1 with scrambling
  off (≤ 4 tries at a swing level, the counter reset when the swing changes, fail at swing 3), then TPS4/3/2 by sink capability
  (≤ 6 tries), swing ≤ 3 and pre-emphasis ≤ 3 − swing, from lane 0's request; fallback: CR failure lowers the rate (8.1 → 5.4 →
  2.7 → 1.62), at RBR halves the lanes; EQ failure halves the lanes, or lowers the rate if CR was lost; at most 4 attempts.
  Stream: MSA and TU parameters, STATE_CTRL = SEND_VIDEO, READY_FOR_VIDEO within 500 ms.
- **Ubuntu's own fixes** (behaviour to keep): HPD state updates serialised (K:e48b1df708c6); no "trained" after a reinit
  (K:ed4c4df5093c); PHY powered off before it is torn down, only taken link resources released (K:6ccaa73e98d1, K:bb5b7685c75c); one
  runtime PM reference per plugged state, link caps reset on every DPCD read, PHY power-on and DPCD status returns checked
  (K:e0809dab97a5, K:6d3ee7df5868, K:095e64bebff3); DIO4485 and altmode route failures honoured (K:93dcfedee755).
- **Open in Ubuntu**: DP (and the hub) after resume: the DP link clock fails to enable (device.md issue #21). Natively: unplug the
  stream before s2idle and re-train on wake; never carry DP across a sleep at first.

## 4. Design

**4.1 Owners.** A new kext **PianoDp** owns the DP controller, the DP half of the QMP PHY and the DIO4485's DP states. **PianoDpu**
owns every DPU register (dpu.md §4.1) and grows an "external path" API. **PianoPower** owns the DSP and hands PianoDp each port
event through the DSP client contract (`package/include/PianoAdspContract.h` "USB-C port events": `piano_port_events` /
`piano_port_seen`, next to its channels and QRTR ports; **built 2026-11-11** as a read-only fan-out in a new file,
`package/PianoPower/PortEvents.c`, with one hook where PianoMca.c's codec takes a notification). Under the contract's rules: called on
PianoPower's thread (`piano-power`, which runs the charger), after the acknowledgement is queued, the bytes as received; the client
copies the event and wakes its own thread; PianoDp is listed after PianoPower. The decoding is the client's (`package/PianoDp/DpPort.h`),
PianoPower stays the only one that sends anything to the port owner. The combo PHY's shared
DP_COM block (mode, orientation, reset, power-down) gets **one** owner shared by PianoUsbHost's USB3 mode (B7 H7) and PianoDp: a small
header-level "combo PHY" state machine in PianoUsbHost that PianoDp calls (mode USB3 / USB3+DP / DP, orientation), so a mode change
never races. NativeGopDisplay grows the second IOMFB instance (4.5).

**4.2 HPD and the port.** On a DP SVID notification: orientation → DIO4485 SELECT (SBU → AUX lanes) → DP_COM mode by the pin
assignment (C/E: DP only, and PianoDwc3/PianoUsbHost's UTMI-as-PIPE; D/F: USB3 + DP 2 lanes) → PianoDp's plug. HPD state 1 → plug
(4.3), 0 → unplug, IRQ_HPD → read DPCD 0x200–0x205 (sink count, IRQ vector, lane status) and re-train if CR/EQ/symbol lock was lost.
Every notification is acked (PianoPower already does). Serialised on one work thread (Ubuntu's K:e48b1df708c6 race).

**4.3 Link.** PHY: GDSC + aux/com_aux/ref clocks (GCC, PianoUsbHost's checks), DP_COM mode, AUX init (§2 values), DISPCC AUX clock;
AUX transactions through AUX_DATA/AUX_TRANS_CTRL (16-byte native AUX, I2C-over-AUX for EDID, DDC segment pointer 0x30 for blocks
above 1); training as §3 with the serdes table of the chosen rate, VCO_DIV, DISPCC LINK rcg parent = QMP link clock (its frequency
is the rate / 10), swing tables. The rate is capped at HBR2 (DT). **Lanes**: pin C/E → 4 (4K60 8 bpc RGB: 533.25 MHz × 24 = 12.8
Gbit/s < 17.28); pin D → 2 (8.64 Gbit/s: 4K30 or 1440p60 at most); mode choice from EDID against that budget.

**4.4 Stream and the DPU's second path.** From EDID the preferred mode that fits the link: DISPCC PIXEL0 MND from `vco_div_clk`
(1350 MHz at HBR2) to the pixel clock (wide bus: INTF0 runs at pixel / 2); INTF0 timing from scratch (dpu.md's INTF fields); MSA +
TU programming; CTL1 (video mode, INTF0 active), LM2 (+ LM3 and MERGE_3D_1 above 2560 wide), PP2/PP3, one DMA pipe (multirect for
the two halves) or VIG2/VIG3 (QSEED for a scaled mirror), DSPP2/3 only for Night Light on the external display (optional). Fetch
addresses from B1b's SMMU window allocator on stream 0x800 (a 4K ARGB surface is 33.2 MB: one 32 MiB-window slot of 8100 pages just
fits; a 5K surface would need two slots). Vblank: TOP0 bit 25 (INTF0 vsync), the same SPI 83 dispatcher B1b adds. MDP clock and
MMCX corner: the panel's 120 Hz path plus a 4K60 path may need a higher MDP clock than ABL left (B1a measures the current one);
DDR: +2 GB/s needs a bus vote (open: RPMh BCM path natively).

**4.5 The macOS side.** Facts from `native-gop-display.md` §2: WindowServer's IOMobileFramebuffer.framework finds the
`paravirtualizedgraphics,gpu` arm-io node, sums the `DisplayPortCount` of its `AppleParavirtGPU` services and waits for that many
IOMobileFramebuffer services; NativeGopDisplay is one `IOMobileFramebufferLegacy` subclass (27.0.1 ABI generated at build time)
that reports one timing and `hotPlug_notify(1)`. Design: `DisplayPortCount = 2` on the nub; **a second NativeGopDisplay instance**
(the same class, a second matching category, a "DPU flip" backend instead of the GOP copy: there is no GOP framebuffer for DP, and
even a CPU copy needs the SMMU window because XNU's RAM is above 4 GiB; for D7 a leaf class of its own, so the drivers that look the
panel's display up by class name keep finding the panel) that starts **disconnected** (`hotPlug_notify(0)`) and on
PianoDp's "link up" publishes the EDID modes as its timings and the EDID (manufacturer, product, physical size) as display
attributes, then `hotPlug_notify(1)`; unplug → `hotPlug_notify(0)`, pipes parked. Swaps of that instance go to PianoDpu's external
flip. Whether WindowServer turns a paravirt-type IOMFB's hot-plug into an extended desktop is **unknown** (the VM's displays are
fixed at boot): D6 below tests it before any DP code exists (built 2026-11-11, §8.1: the second instance with a RAM surface where the
"DPU flip" backend will be, its own match category and UUID, `hotPlug_notify` on a timer). Fallback if it does not: a **hardware mirror** (D5b): the external path
scans the panel's current surface through a VIG pipe's scaler, no macOS change at all; or, last, two displays fixed from boot (the
external one dark until plugged).

**4.6 Power.** DP lit ≈ the QMP PHY + DP controller + a second DPU path + its DDR traffic: *inferred* 0.4–0.8 W at 4K60; nothing when
unplugged (PHY powered down, DISPCC DP clocks off, CTL1 idle). Unplug before s2idle (§3 open issue).

## 5. Dependencies

- **B1b** (hard): the SMMU bank of stream 0x800 reprogrammed in place with IOVA windows, the flip path and vblank IRQ; B1a's
  allocation of CTL/LM/PP/MERGE_3D on the panel.
- **B7** (hard): H2 (PAN payload decoded in PianoPower), the combo PHY owner, the UTMI-as-PIPE switch for pins C/E, the role lock
  (K:9834284b801f: Type-C clock updates share the dwc3 role lock); the DSP up (PianoPower level 2 healthy).
- I2C to the DIO4485 (QUP1 SE3 pins gpio44/45: bit-banged as PianoBacklight's `BitI2c.h`, or a GENI I2C helper).
- An RPMh/ICC path for the MMCX corner and the DDR bandwidth vote (shared need with B10 video, B11 camera).

## 6. Boot args

Built (2026-11-11), all off without `piano-dp`:

| Arg | Default | Effect |
|---|---|---|
| `piano-dp=1` | 0 | level 1, read only: D1 the survey, then D2 the port events (they come only with `piano-power=2`). A value above 1 is not built yet and acts as 1 (said so in the start line) |
| `piano-dp-survey=0` | 1 | no survey: no MMIO at all (D2 and D6 alone) |
| `piano-dp-raw=N` | 16 | the bytes of the first N port notifications logged (0..1000) |
| `piano-dp-delay=S` | 0 | wait S s before the survey |
| `piano-dp-fake=S` | 0 | **D6** (NativeGopDisplay, with `piano-dp>=1`): the nub reports 2 display ports, a second display backed by RAM starts disconnected and is hot-plugged S s after its start (1..3600) |
| `piano-dp-fake-boot=1` | 0 | D6: the second display connected from its start instead (the "fixed from boot" fallback) |
| `piano-dp-fake-unplug=U` | 0 | D6: then unplug and plug again every U s (5..3600) |
| `piano-dp-fake-cycles=N` | 1 | D6 with U: N plugs in all (1..50), ending plugged |
| `piano-dp-fake-mode=WxH@Hz` | 1920x1080@60 | D6: its one timing (640..3840 × 480..2160, 24..120 Hz, W even) |
| `piano-dp-fake-mm=WxH` | 527x296 | D6: its physical size in mm (a 24-inch 16:9 monitor) |
| `piano-dp-fake-primary=N` | 0 | D6: its `primary` property (the panel's is 0; 1 is the variant to try if D6a fails) |

Planned for the later levels: `piano-dp=2` AUX (DPCD + EDID), 3 train, 4 test pattern, 5 mirror, 6 extended; `piano-dp-rate=1620|2700|5400`
(cap below HBR2), `piano-dp-lanes=1|2|4`, `piano-dp-mode=WxH@Hz` (override EDID).

## 7. Kernel collection

Built: `package/PianoDp` (`PianoDp.cpp`; portable `DpHw.h`, `DpSurvey.h`, `DpPort.h`, `DpFake.h`; `tests/dp_host.cpp`),
`tooling/k27/build-native-piano-dp.sh` (host tests, a store check of the portable headers and of the kext source, the objects, the
import check: PianoPower's `piano_port_*` / `piano_adsp_health` come from PianoPower), `tooling/k27/kext/PianoDp.json` (library
`com.unixnu.driver.PianoPower`), in `configs/k27-native-kexts.txt` after PianoPower. PianoPower gains `PortEvents.c` (+ its host test
`tests/port_events_host.cpp` in `build-native-piano-power.sh`); NativeGopDisplay gains D6 (the personality `NativeGopDisplayExternal` in
`NativeGopDisplay.json`, the vtable check unchanged). Linked 2026-11-11: PianoDp ≈ 30 KB of code and strings, 2.7 KB `__DATA,__const`
(its vtable), 1.2 KB `__bss` (the page table).

Later levels add host tests for the AUX transaction encoder, an EDID parser (base block + CTA-861 + DisplayID tiles), the training
state machine against a DPCD model, MSA/TU arithmetic and mode fitting from EDID against the link budget (the standard-mode budget is
there already).

## 8. Ladder and effort (RAM boots, owner present with a monitor and a USB-C/DP cable or dock)

| Step | What | Pass | Effort |
|---|---|---|---|
| D0 | offline: register map, decoders, EDID, training model, timings, host tests | host tests | 1–1.5 weeks (D1/D2/D6's parts built 2026-11-11) |
| D1 | survey (`piano-dp=1`, read only, **unattended**): the DP gates, the controller and PHY when clocked, the DPU's DP side | the survey lines through `survey done`, no stall or reset; the gates and the DPU verdict as found | built; 1 boot |
| D2 | port events (`piano-dp=1`, PianoPower level 2, read only, **attended**): plug a monitor both ways, with a dock | orientation/mux/pin/HPD per plug match Ubuntu's | built; 1 session |
| D3 | DIO4485 + DP_COM + AUX: DPCD caps, EDID; then link training RBR → HBR2, both orientations, 2 and 4 lanes | DPCD rev/rate/lanes, the monitor's EDID name; CR + EQ done, the monitor leaves "no signal" | 2–2.5 weeks + 3 sessions |
| D4 | stream: 1080p60 test pattern from a kernel buffer (CTL1, LM2, one pipe, SMMU window) | the pattern on the monitor | 1–1.5 weeks |
| D5 | mirror: the panel's surface scaled through a VIG pipe (no macOS change) | desktop on both, follows the panel | 3–5 d |
| D6 | macOS: a second NativeGopDisplay instance with a fake connected 1080p display (no DP) | Settings shows two displays, windows move across (unattended: the second display's swaps and thumbnails) | built; 1–4 boots (independent of D1–D5) |
| D7 | extended desktop on DP: EDID modes, hot-plug → `hotPlug_notify` | arrange, plug/unplug 20× without a hang | 1–2 weeks |
| D8 | 4K60 (two LMs + MERGE_3D_1, pins C/E, USB2 continues), clocks and DDR vote | 3840×2160@60 stable 30 min, USB keyboard still works | 1 week |
| D9 | robustness: teardown order, unplug while streaming, USB back after unplug (v1.1.0), s2idle with/without DP | no reset, USB3 back after a DP-only unplug | 1 week |

**Verdict:** feasible on the hardware side (Ubuntu proved every piece; nothing needs firmware or a vendor blob); the macOS-side
extended desktop is the real unknown (D6 settles it early and cheaply). Total ≈ **9–13 weeks** of work after B1b and B7 H2, about
half of it attended. A mirror (D0–D5) is ≈ 5–7 weeks and needs no macOS change.

### 8.1 The first slices as built (2026-11-11, offline: host-tested and linked, not run)

**D1, the survey** (`piano-dp=1`; `package/PianoDp/DpSurvey.h`, every access a load, no store in the file or in the kext). Order and
gates: PianoDpu's display gate (GCC display AHB → DISPCC; MDSS GDSC, AHB, MDP); DISPCC's DP branches and RCGs (only DISPCC's own
gate: GCC display AHB); the controller as msm clocks it: ahb + aux only with the MDSS gate open **and** DPTX0 AUX on, link only with
LINK + LINK_INTF on too, p0 only with PIXEL0 on too, AUX_DATA and HPD_INT_ACK never, p1 (MST) never; the QMP PHY by PianoUsbHost's
rule (USB3_PHY_GDSC on, aux and com_aux on): DP_COM, the USB3 PCS words, the DP half's revision/CFG/PD_CTL/MODE/AUX_CFG0..9 (as
found, logged, not used)/VCO_DIV/STATUS and the DP serdes status; with the MDSS gate open the DPU's DP side: every CTL (the panel's
path from INTF1/INTF2), every LM, MERGE_3D 0..3, INTF0 and INTF3, TOP's interface and interrupt words, then the verdict (a free CTL
from CTL1, the LM pair from LM2/LM3, MERGE_3D_1, INTF0: "the 4K path is available" or what is taken); the TLMM words of gpio61
(orientation) and gpio44/45 (the DIO4485's bus; the DIO4485 itself is not read: an I2C read takes the pins). Expected on a native
boot (needs-boot where marked):

```
PianoDp: start: piano-dp=1: read only: the D1 survey (...), then the port events (D2: needs piano-power=2, whose DSP sends them); nothing is written
PianoDp: stage 1: D1 survey (read only; every block behind its clock gate)
PianoDp: gate: MDSS powered, AHB and MDP clocks on (gcc disp ahb ..., dispcc mdss gdsc 8....... ahb ... mdp ...)
PianoDp: dispcc dp branches: aux 8....... (off) link 8....... (off) link_intf ... (off) pixel0 ... (off) ...      <- expected: ABL never lit DP
PianoDp: dispcc dp rcgs: aux .../... (root on|off, source N, half-divider N) link ... pixel0 ... (M N D) pixel1 ... link divider ...
PianoDp: dp controller: DPTX0 AUX clock off (the controller's core clocks): not read (nothing natively lights DP)
PianoDp: qmp: not read (the PHY's power domain or its aux/com_aux clock is off: ...)    | or: qmp dp_com: mode ... (USB3 only) ...  (needs-boot, §10.4)
PianoDp: dpu top: ...; dpu ctl0: ... intf 0x6, lm 0x3 ...; dpu ctl1: ... (free); dpu lmN: ...; dpu merge_3d: ...
PianoDp: dpu intf0 (DP): en 00000000 (timing engine off), ...; dpu intf3 (DP MST): ...
PianoDp: dpu: the panel's path is ctlN (intf 0x6, lm ..., merge_3d ...); busy: ...                                      (needs-boot, B1a)
PianoDp: dpu verdict: ctl1, lm2/lm3, merge_3d 1 and intf0 are free: the DP path of displayport.md 4.4, up to 4K (two LMs), is available
PianoDp: pins: gpio61 (Type-C orientation) cfg ... io ...: in N (...), pull ...; gpio44/gpio45 (DIO4485 I2C) function N/N, in N/N; ...
PianoDp: summary: dp not clocked; qmp ...; panel ctlN lm ...; dp ctl1 lm pair 1 merge_3d1 free intf0 free
PianoDp: survey done: N MMIO loads, 0 stores; N pages mapped, 0 map failures
```

**D2, the port events** (`piano-dp=1` with `piano-power=2`; `DpPort.h`). PianoPower's new fan-out (`PortEvents.c`, one hook after its
codec took a notification and queued the acknowledgement) hands each notification's bytes to PianoDp on PianoPower's thread; PianoDp
copies it into a 16-entry ring and wakes its own thread, which sleeps otherwise (no polling; one status line 3 minutes in when nothing
came). Decoded as Linux does (exactly 32 bytes, port < 3, SVID from the opcode word) and run through a per-port state machine; each
change is one line, plus the raw bytes of the first `piano-dp-raw` (16) notifications. A plug's would-be writes are logged as a plan
(the DIO4485 words, the QMP mode, the lanes and the standard modes they carry at HBR2), nothing is written. The first HPD high
re-reads the survey's gates (still read only) to show that nothing lit the path. Expected per event:

```
PianoPower: mca: USB-C port notification (port 0): acknowledged
PianoDp: event #N: 32 bytes: 0c800000 02000000 1600 01ff 00010300 0000 01ff ...                       (the bytes as received)
PianoDp: port 0: SVID 0xff01 DisplayPort, mux 3 (USB3 + DP), orientation reverse, pin D, HPD high, IRQ_HPD 0 (vid ..., payload svid 0xff01, ext 44)
PianoDp: port 0: attach (USB3 + DP, reverse)
PianoDp: port 0: DisplayPort alt mode entered: pin D (2 lanes + USB 3)
PianoDp: port 0: plan (nothing written at level 1): DIO4485 enable 0xf8 select 0x78, QMP USB3 + DP (2 lanes), DP 2 lanes; at HBR2 8 bpc: 2560x1440@60, 3840x2160@30, 1920x1080@60
PianoDp: port 0: HPD high: a sink is there (plug 1); level 2 (not built) would read DPCD and EDID, level 3 train
PianoDp: after the first HPD high, read again: dispcc dp aux off link off pixel0 off; dp controller not clocked; qmp ...; intf0 off
PianoDp: port 0: IRQ_HPD: the sink asks for attention (...)  |  HPD low: unplug 1 (...)  |  DisplayPort alt mode left (...)  |  detach
PianoDp: port 0: orientation normal -> reverse  |  pin assignment C -> D (...)  |  mux ... -> ...  |  no change (a repeat)
PianoDp: port 0: pin A is not driven (...)  |  pin C (4 lanes) does not match mux USB3 + DP      (flags worth a note in the run's record)
```

The registry gets `piano-dp-port0` (attached/DisplayPort/detached, mux, orientation, pin, HPD, counts) and `piano-dp-survey`.

**D6, the fake second display** (NativeGopDisplay with `piano-dp=1` and `piano-dp-fake=S` or `piano-dp-fake-boot=1`; `DpFake.h`). The
nub reports `DisplayPortCount = 2`; the personality `NativeGopDisplayExternal` (own match category, `NativeGopDisplayRole`
"piano-dp-fake") makes a second instance of the same class on the same nub; its probe refuses silently without the arguments. It maps
no framebuffer: a RAM surface (1920x1080x4 by default, 8.3 MB) takes its copies, from which its statistics and thumbnails come. Its
own timing (the same CVT-RB-style blanking), size (527x296 mm), `IOMFBUUID` (`4E474F50-4449-5350-0001-0000000000D6`) and product
(`PianoDp test display`, product 0xd6); none of the panel's callouts (backlight, PianoDpu), no refresh copies, the boot console left
alone. `hotPlug_notify(0)` at its start (1 with `-boot`), then a work-loop timer walks the schedule (plug at S, then with `-unplug=U`
alternately every U s, `-cycles` plugs, ending plugged); its VBL timer stops while it is unplugged. If its start fails, the nub's count
goes back to 1. No register, no DMA, no hardware. It publishes no `piano-display-power` (that property and its messages are the
panel's). Caveat: PianoBacklight, PianoKeyboard, PianoPolicy, PianoSensors and PianoTouch find the panel's display by the class
name `NativeGopDisplay`, which the second instance shares, so with D6 armed such a lookup may return it; the D6 images run none of
them (and D7's real external display gets a class of its own, §4.5). Its lines say `NativeGopDisplay-ext:` (the panel's are
unchanged), so `boot-timeline.py`'s login-frame match stays the panel's:

```
NativeGopDisplay: AppleParavirtGPU nub on gop-display, 2 display ports (piano-dp-fake: the second is D6's RAM-backed test display)
NativeGopDisplay-ext: probe on AppleParavirtGPU (ADT gop-display): taking it
NativeGopDisplay-ext: D6 (piano-dp-fake): a second display 1920x1080@60, 527x296 mm, primary 0, disconnected until its plug timer; RAM-backed, no hardware touched
NativeGopDisplay-ext: D6: drivers that look NativeGopDisplay up by class may find this instance: test it without them
NativeGopDisplay-ext: RAM surface 1920x1080 row 7680 (8294400 bytes): frames are copied there and never scanned out
NativeGopDisplay-ext: display attributes published (one 1920x1080@60 timing, 8-bit SDR sRGB)
NativeGopDisplay-ext: hardware started (VBL timer 60 Hz while connected, power states registered, hot plug 0; plugged in 180 s)
NativeGopDisplay-ext: started, display 1920x1080 at 60 Hz, 527x296 mm, no refresh copies, thumbnail every 30 s (at most 8, 0: no limit)
NativeGopDisplay-ext: hot plug 1, 180 s after the start (piano-dp-fake: connection 1 of 1); the schedule is done
NativeGopDisplay-ext: display on                                     <- WindowServer powered the second display
NativeGopDisplay-ext: first swap submitted / first frame (swap N): WindowServer draws on the second display
NativeGopDisplay-ext: thumb frame N: 1920x1080 'BGRA', P% non-black, ...  + 27 thumbnail rows      <- the screenshot check
NativeGopDisplay-ext: N swaps submitted, N shown, ...                                                (every ~10 s while swaps arrive)
```

What answers §10.1: **extended** = after the plug, `display on` and swaps on the `-ext` instance with a non-black thumbnail (the
desktop picture or a window; the panel's thumbnails of the same run for comparison); **ignored** = `hot plug 1` and no `display on`/swap on `-ext`
while the panel's swaps go on; **broken** = WindowServer restarts (a second `display on` of the panel, `first swap submitted` again) or
a panic. `PianoDp: D6 fake display (...)` repeats the configuration in PianoDp's log.

### 8.2 The batched tablet test list

Unattended boots first, then one attended session. RAM boots only, each from `fastboot boot` with `piano-reset-after` as the backstop
(Ubuntu and ABL return on the next boot), the USB console capturing (`tooling/native/usb-console.py --capture SECS`) and the pstore
mailbox read by the dump image (`tooling/native/native-loop.sh IMAGE out/ram-dp-dump/boot-piano-macos-ram-A-bundled.img`). Build
(main checkout, after the merge): `PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh` (the KC with PianoDp and
PianoA830, the Turnip stage, the vmm-present 0 ADT), then `tooling/k27/build-dp-test-images.sh` → `out/ram-dp-<name>/` and
`out/ram-dp-dump/`. Every image carries the GPU profile (`piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1`), `piano-usb=2`
and `PIANO_FAST_BOOT=1`. The D6 images use `piano-dp-survey=0`, so they touch no DP hardware at all.

| # | Image | Boot args (beyond the profile) | Owner | What the log must show | If it fails |
|---|---|---|---|---|---|
| 1 | `d1` (300 s) | `piano-power=1 piano-dp=1` | unattended | §8.1's D1 lines through `survey done: ... 0 stores`, then `stage 2` and the 3-minute status line; the login screen as without it | a stall after `gate:` or `qmp`: the last line names the block; boot 2–5 do not depend on it (they skip the survey) |
| 2 | `d6a` (480 s) | `piano-power=1 piano-dp=1 piano-dp-survey=0 piano-dp-fake=180` + thumbnails | unattended | the nub's `2 display ports`; the `-ext` start lines; `hot plug 1, 180 s after the start`; then the verdict of §8.1 (extended / ignored / broken) and the `-ext` thumbnails | ignored → boot 3; broken → boot 3 too, then stop (the hot-plug path itself is the problem) |
| 3 | `d6b` (420 s) | `... piano-dp-fake-boot=1` + thumbnails | unattended | two displays from WindowServer's start: `-ext` `display on` and swaps; extended or mirrored on the thumbnails | ignored here too → boot 4 (`primary`); works here but not in 2 → hot-plug needs more than `hotPlug_notify` (the fallback of §4.5: two displays fixed from boot) |
| 4 | `d6c` (480 s) | `... piano-dp-fake=180 piano-dp-fake-primary=1` + thumbnails | unattended | as boot 2, with `primary 1` in the `-ext` D6 line | still ignored: the paravirt-type IOMFB may not extend at all → the hardware mirror (D5b) is the plan, §10.1 answered "no" |
| 5 | `d6d` (660 s) | `... piano-dp-fake=180 piano-dp-fake-unplug=30 piano-dp-fake-cycles=5` + thumbnails | unattended, only after 2 or 3 passed | five `hot plug 1` and four `hot plug 0`, the `-ext` swaps stopping and resuming, the panel's swaps throughout, no WindowServer restart | the cycle that broke and what the panel did (D7's robustness question, early) |
| 6 | `d2` (1200 s) | `piano-power=2 piano-power-write=0 piano-dp=1` | **attended**: the owner, a USB-C monitor (pin C/E), a USB-C dock with power (pin D) or a USB-C-to-DP adapter | booted on the Mac's cable (`mca: USB-C port notifications enabled`, perhaps a first USB-only event), then within the 20 minutes: Mac cable out; monitor in, out; monitor in turned over; dock in (charging through it), out; adapter in, out; Mac cable back before the reset so the loop reads the dump. Per plug: attach, `DisplayPort alt mode entered: pin X`, `HPD high`, the orientation; per unplug: `HPD low`, detach. Compare with Ubuntu's `pmic_glink_altmode` view of the same plugs (orientation, pin, HPD) | `port notifications NO ANSWER` → the owner is not served (charging.md §5e); no `PianoDp: event` while PianoPower logs notifications → the fan-out; `not decoded: not 32 bytes` → record the bytes (the firmware's layout differs from Linux's) |

D1 and the D6 boots are independent: D6 needs neither D1 nor any DP code, and runs first if the session is short. With a monitor on
the port there is no USB console: boot 6 is read from the dump image (the pstore mailbox), and the tablet runs from its battery except
on the dock (`piano-power-write=0`: the DSP keeps its own input limit, as image G2 showed about 400 mA drawn, so a dock barely charges).

## 9. Fallback

Without `piano-dp` nothing is touched; the port stays USB (PianoDwc3 / PianoUsbHost). Any failure parks the external path (CTL1 off,
PHY down, DIO4485 back to USB) and leaves the panel untouched; the panel path never depends on DP. As built: PianoDp declines in
probe() without `piano-dp` (no thread, no mapping, no client registration); PianoPower's fan-out costs one atomic load per port
notification without a client; NativeGopDisplay's second personality refuses silently without D6's arguments, the nub reports one
display port and the panel's lines are what they were; a D6 display that cannot start puts the nub back to one port.

## 10. Open questions

1. Does WindowServer accept a hot-plugged second paravirt-type IOMFB as an extended display? → D6 (§8.2 boots 2–5: hot-plugged,
   fixed from boot, the other `primary`, cycles).
2. The panel's DPU allocation (which CTL/LM/PP/MERGE_3D/DSC are free) → B1a.
3. MDP clock and MMCX corner with both paths lit; who votes them natively (DISPCC RCG + RPMh) → D8.
4. The QMP PHY's state after the boot stages (USB3 mode? powered?) → B7 H1 survey.
5. Does the DSP send PAN notifications without UCSI being set up (Ubuntu runs both)? → D2 (§8.2 boot 6; PianoPower only reads the
   UCSI buffer once).
6. DP audio: not in Ubuntu's notes; not planned.
7. Is the DSP's notification exactly Linux's 32-byte `usbc_notify`, and does the header's SVID match the copy at @18? → D2 (the raw
   bytes of the first 16 notifications are logged; a different length is logged as "not decoded").
8. In pins C/E fsa4480 turns the DIO4485's USB switch off, yet USB 2 continued under Ubuntu in DP-only mode (K:68d28640e1d1): do
   piano's D+/D- pass through the DIO4485 at all? → the USB-only words come first at level 2, and D8's "USB keyboard still works".
9. What does WindowServer take `primary` to mean on a second paravirt-type display (an index, or a flag)? → D6 boots 2 and 4.

## 11. Licence note

The msm DP/DPU drivers, the QMP combo PHY, DISPCC, pmic_glink_altmode, fsa4480 and dwc3-qcom (GPL-2.0) are the specification only
(offsets, fields, sequences). The QMP DP serdes/TX tables (about 20 register/value pairs per rate, plus the 4×4 swing and
pre-emphasis matrices) are vendor tuning values that exist only as tables in that driver: taking them is the same **owner
decision** as B7's H7 (USB3 PHY tables) and the camera sensor tables (`camera.md` §10). The clean alternative is to measure them:
dump the DP PHY's registers on the tablet while Ubuntu drives a monitor at each rate (reads only, values of the hardware's state).
No Apple binary is read beyond the facts already in `native-gop-display.md` (the framework's behaviour observed in the VM); no vendor
blob. As built (`package/PianoDp/UPSTREAM`): offsets, bit names, the clock order, the notification layout, the pin-to-mode mapping
and the DIO4485 words were taken as facts; no serdes/TX table, swing/pre-emphasis matrix or AUX_CFG init word is in the code (D1 reads
AUX_CFG0..9 as found and logs them, which is the measurement route). D6 uses the IOMFB ABI NativeGopDisplay already generates;
nothing new was read from Apple binaries.
