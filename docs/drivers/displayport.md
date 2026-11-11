# USB-C DisplayPort Alt Mode (B8): design

Status, 2026-11-11: **design only, offline.** Nothing built. Backlog item B8 (`../BACKLOG-hardware.md`); it builds on B1 (the DPU,
`dpu.md`) and B7 (the Type-C port, `usb-host.md`). Template: `README.md`.

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
| Registers | aux: HPD_CTRL 0x00, HPD_INT_STATUS 0x04 (state b31:29), ACK 0x08, MASK 0x0c, REFTIMER 0x18, AUX_CTRL 0x30, AUX_DATA 0x34, AUX_TRANS_CTRL 0x38 (I2C 0x100, GO 0x200), AUX_STATUS 0x44; ahb: SW_RESET 0x10, PHY_CTRL 0x14, INTR_STATUS 0x20/0x24/0x2c; link: MAINLINK_CTRL 0x00, STATE_CTRL 0x04 (TPS1..4 = b0..3, SEND_VIDEO b7, PUSH_IDLE b8), CONFIGURATION_CTRL 0x08, lane map 0x38, MAINLINK_READY 0x40 | msm `dp_reg.h` | verified (source) |
| QMP DP part | v8 offsets: DP serdes +0x3000, TX A/B +0x3400/+0x3800, DP_PHY +0x3c00 (PD_CTL +0x18, MODE +0x1c, AUX_CFG0..9 +0x20..0x44); DP_COM (shared with USB3): PHY_MODE_CTRL +0 (b1 DP, b0 USB3), TYPEC_CTRL +0x10 (b1 software select, b0 reverse); AUX init PD_CTL 0x67, AUX_CFG = 00 13 a4 00 0a 26 0a 03 b7 03; rates 1.62/2.7/5.4/8.1 by serdes tables, VCO_DIV 1/1/2/0, vco_div_clk 810 MHz (RBR) / 1350 MHz; swing/pre-emphasis tables per rate band; supplies L2D 0.88 V, L3G 1.2 V; GDSC USB3_PHY | qmp | verified (source) |
| Orientation | DP_COM TYPEC_CTRL reverse bit; DP_PHY MODE 0x4c reversed / 0x5c normal; with 2 DP lanes (pin D) the unused pair is powered down by orientation | qmp | verified (source) |
| DIO4485 | I2C3 (`i2c@a8c000`) 0x42: DP only (pins C/E) ENABLE 0xe0, SELECT 0x00 / 0x60 reversed; DP + USB (D/F) 0xf8, 0x18 / 0x78; USB 0x98, 0x18; SBU off + 35 µs before a change, 15 µs after | fsa4480.c | verified (source) |
| HPD | **from the DSP**, not a pin: PAN owner 0x800c, notification opcode 0x16 (SVID in opcode b31:16), payload @12 port, @13 orientation, @14 mux (1 USB3, 2 DP 4 lanes, 3 USB3 + DP, 4 tunnel), @20 b5:0 pin assignment (1 = A .. 6 = F), b6 HPD state, b7 IRQ_HPD; enable = write request 0x15 cmd 0x10, ack = 0x15 cmd 0x11 arg port after every notification; `pan_ack` acks piano's two ports without a connector node (3 ports, 1 connector) | glink, `usb-host.md` §2.3 | verified (source); PianoPower enables/acks PAN, does not decode |
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
owns every DPU register (dpu.md §4.1) and grows an "external path" API. **PianoPower** owns the DSP and decodes PAN (B7 H2) and hands
PianoDp each port event (`callPlatformFunction "PianoPowerPortEvent"`, a struct copy; no link dependency). The combo PHY's shared
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
even a CPU copy needs the SMMU window because XNU's RAM is above 4 GiB) that starts **disconnected** (`hotPlug_notify(0)`) and on
PianoDp's "link up" publishes the EDID modes as its timings and the EDID (manufacturer, product, physical size) as display
attributes, then `hotPlug_notify(1)`; unplug → `hotPlug_notify(0)`, pipes parked. Swaps of that instance go to PianoDpu's external
flip. Whether WindowServer turns a paravirt-type IOMFB's hot-plug into an extended desktop is **unknown** (the VM's displays are
fixed at boot): D6 below tests it before any DP code exists. Fallback if it does not: a **hardware mirror** (D5b): the external path
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

## 6. Boot args (proposed)

`piano-dp=N` (0; 1 survey: DP/QMP/DIO4485 state + PAN events logged, read only; 2 AUX: DPCD + EDID; 3 train; 4 test pattern; 5 mirror;
6 extended), `piano-dp-rate=1620|2700|5400` (cap below HBR2), `piano-dp-lanes=1|2|4`, `piano-dp-mode=WxH@Hz` (override EDID).

## 7. Kernel collection

`package/PianoDp` (+ `tooling/k27/build-native-piano-dp.sh`, `tooling/k27/kext/PianoDp.json`, the kext list), host tests for the PAN
decoder, the DIO4485 words, the AUX transaction encoder, an EDID parser (base block + CTA-861 + DisplayID tiles), the training state
machine against a DPCD model, MSA/TU arithmetic, mode fitting against the link budget.

## 8. Ladder and effort (RAM boots, owner present with a monitor and a USB-C/DP cable or dock)

| Step | What | Pass | Effort |
|---|---|---|---|
| D0 | offline: register map, decoders, EDID, training model, timings, host tests | host tests | 1–1.5 weeks |
| D1 | survey + PAN log (`piano-dp=1`, PianoPower level 2): plug a monitor both ways, with a dock | orientation/mux/pin/HPD per plug match Ubuntu's | 2 d + 1 session |
| D2 | DIO4485 + DP_COM + AUX: DPCD caps, EDID | DPCD rev/rate/lanes, the monitor's EDID name | 1 week + 2 sessions |
| D3 | link training RBR → HBR2, both orientations, 2 and 4 lanes | CR + EQ done, the monitor leaves "no signal" | 1–1.5 weeks |
| D4 | stream: 1080p60 test pattern from a kernel buffer (CTL1, LM2, one pipe, SMMU window) | the pattern on the monitor | 1–1.5 weeks |
| D5 | mirror: the panel's surface scaled through a VIG pipe (no macOS change) | desktop on both, follows the panel | 3–5 d |
| D6 | macOS: a second NativeGopDisplay instance with a fake connected 1080p display (no DP) | Settings shows two displays, windows move across | 1 week + sessions (can run before D1) |
| D7 | extended desktop on DP: EDID modes, hot-plug → `hotPlug_notify` | arrange, plug/unplug 20× without a hang | 1–2 weeks |
| D8 | 4K60 (two LMs + MERGE_3D_1, pins C/E, USB2 continues), clocks and DDR vote | 3840×2160@60 stable 30 min, USB keyboard still works | 1 week |
| D9 | robustness: teardown order, unplug while streaming, USB back after unplug (v1.1.0), s2idle with/without DP | no reset, USB3 back after a DP-only unplug | 1 week |

**Verdict:** feasible on the hardware side (Ubuntu proved every piece; nothing needs firmware or a vendor blob); the macOS-side
extended desktop is the real unknown (D6 settles it early and cheaply). Total ≈ **9–13 weeks** of work after B1b and B7 H2, about
half of it attended. A mirror (D0–D5) is ≈ 5–7 weeks and needs no macOS change.

## 9. Fallback

Without `piano-dp` nothing is touched; the port stays USB (PianoDwc3 / PianoUsbHost). Any failure parks the external path (CTL1 off,
PHY down, DIO4485 back to USB) and leaves the panel untouched; the panel path never depends on DP.

## 10. Open questions

1. Does WindowServer accept a hot-plugged second paravirt-type IOMFB as an extended display? → D6.
2. The panel's DPU allocation (which CTL/LM/PP/MERGE_3D/DSC are free) → B1a.
3. MDP clock and MMCX corner with both paths lit; who votes them natively (DISPCC RCG + RPMh) → D8.
4. The QMP PHY's state after the boot stages (USB3 mode? powered?) → B7 H1 survey.
5. Does the DSP send PAN notifications without UCSI being set up (Ubuntu runs both)? → D1.
6. DP audio: not in Ubuntu's notes; not planned.

## 11. Licence note

The msm DP/DPU drivers, the QMP combo PHY, DISPCC, pmic_glink_altmode, fsa4480 and dwc3-qcom (GPL-2.0) are the specification only
(offsets, fields, sequences). The QMP DP serdes/TX tables (about 20 register/value pairs per rate, plus the 4×4 swing and
pre-emphasis matrices) are vendor tuning values that exist only as tables in that driver: taking them is the same **owner
decision** as B7's H7 (USB3 PHY tables) and the camera sensor tables (`camera.md` §10). The clean alternative is to measure them:
dump the DP PHY's registers on the tablet while Ubuntu drives a monitor at each rate (reads only, values of the hardware's state).
No Apple binary is read beyond the facts already in `native-gop-display.md` (the framework's behaviour observed in the VM); no vendor
blob.
