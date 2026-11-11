// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDp: the hardware facts of piano's USB-C DisplayPort path (SM8750: the DP controller at af54000, the DP half of the QMP USB3/DP
// combo PHY at 88e8000, the DSP's USB-C port notifications, the DIO4485 SBU switch), each with its source. Every number here is a fact
// read from a source, never code:
//   DT     the piano boot DT (out/inputs/piano-boot.dtb): displayport-controller@af54000, clock-controller@af00000, phy@88e8000
//   msm    the Ubuntu piano kernel (piano/dev 7f8d6bed, GPL-2.0, read as the specification only): drivers/gpu/drm/msm/dp/dp_reg.h,
//          dp_ctrl.c, dp_display.c; drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c (dpu_kms_mdp_snapshot)
//   qmp    the same kernel: drivers/phy/qualcomm/phy-qcom-qmp-combo.c (qmp_combo_offsets_v8, sm8750_usb3dpphy_cfg, the v8 register
//          layout, qmp_combo_typec_mux_set, qmp_v456_configure_dp_phy), phy-qcom-qmp-dp-phy.h, phy-qcom-qmp-dp-phy-v6.h,
//          phy-qcom-qmp-qserdes-com-v8.h
//   cc     the same kernel: drivers/clk/qcom/dispcc-sm8750.c, clk-rcg2.c
//   glink  the same kernel: drivers/soc/qcom/pmic_glink_altmode.c (struct usbc_notify, the callback and the worker)
//   fsa    the same kernel: drivers/usb/typec/mux/fsa4480.c (fsa4480_set)
// The shared facts are included, not copied: the DPU's (package/PianoDpu/DpuHw.h: MDP base, CTL/LM/INTF/MERGE_3D offsets, DISPCC's
// MDSS words) and the USB side's (package/PianoUsbHost/UsbHostHw.h: the QMP base, DP_COM, the GCC PHY clocks, the TLMM pins).
// docs/drivers/displayport.md section 2 has the same facts with their state (verified / inferred / needs-boot).
// No DP PHY tuning value is here (serdes tables, swing and pre-emphasis matrices, AUX_CFG init words): an open owner decision
// (docs/BACKLOG-hardware.md section 4). Nothing here writes them, and nothing in PianoDp writes the PHY at all.
#pragma once
#include <stdint.h>

namespace pianodp {

// ---------------------------------------------------------------------------------------------------- the DP controller (DT)
// reg = ahb af54000 (0x104), aux af54200 (0xc0), link af55000 (0x770), p0 af56000 (0x9c), p1 af57000 (0x9c, MST: unused).
// interrupts-extended = MDSS hwirq 12; power-domains = MMCX; a child of the MDSS node (its runtime parent: the MDSS GDSC).
// clocks (DISPCC ids): core_iface 6 (MDSS AHB), core_aux 16 (DPTX0 AUX), ctrl_link 19 (DPTX0 LINK), ctrl_link_iface 22 (LINK_INTF),
// stream_pixel 23 (PIXEL0), stream_1_pixel 25 (PIXEL1). The register blocks' clock rule is msm's (dp_ctrl.c): the core clocks
// (core_iface + core_aux) before any ahb/aux access, the link clocks (ctrl_link + ctrl_link_iface) before the link block, the stream
// pixel clock before p0; msm_dp_snapshot reads none of them while DP is off "to avoid crash due to unclocked access" (dp_display.c).
constexpr uint64_t kDpAhb = 0x0af54000, kDpAux = 0x0af54200, kDpLink = 0x0af55000, kDpP0 = 0x0af56000, kDpP1 = 0x0af57000;
constexpr uint32_t kDpAhbBytes = 0x104, kDpAuxBytes = 0xc0, kDpLinkBytes = 0x770, kDpP0Bytes = 0x9c;
constexpr uint64_t kDpFirst = kDpAhb, kDpEnd = kDpP1 + 0x9c;   // the controller's whole window (host tests: never loaded unclocked)

// ahb (msm dp_reg.h): HW_VERSION is read once after a controller reset (msm_dp_ctrl_reset); MAJOR 31:28, MINOR 27:16, STEP 15:0
// (DP_HW_VERSION_1_2 = 0x10020000). SW_RESET bit 0; PHY_CTRL bit 0 PLL reset, bit 2 PHY reset; INTR_STATUS bit 0 HPD, bit 24 PLL
// unlocked; INTR_STATUS2 bit 0 ready for video. None of these is read-to-clear (msm acknowledges by writing the ack bits).
constexpr uint32_t kAhbHwVersion = 0x00, kAhbSwReset = 0x10, kAhbPhyCtrl = 0x14, kAhbClkCtrl = 0x18, kAhbClkActive = 0x1c,
                   kAhbIntrStatus = 0x20, kAhbIntrStatus2 = 0x24, kAhbIntrStatus3 = 0x28, kAhbIntrStatus4 = 0x2c, kAhbIntrMask4 = 0x30;
// aux (msm dp_reg.h, offsets from the aux block): HPD_CTRL bit 0 HPD enable; HPD_INT_STATUS 31:29 the HPD state (0 disconnected,
// 1 pending, 2 connected); HPD_INT_MASK; REFTIMER bit 16 enable; AUX_CTRL bit 0 enable; AUX_TRANS_CTRL; AUX_STATUS. AUX_DATA (0x34)
// is the transaction FIFO and HPD_INT_ACK (0x08) an acknowledge register: never read (a FIFO read has an effect).
constexpr uint32_t kAuxHpdCtrl = 0x00, kAuxHpdIntStatus = 0x04, kAuxHpdIntMask = 0x0c, kAuxHpdRefTimer = 0x18, kAuxHpdEventTime0 = 0x1c,
                   kAuxHpdEventTime1 = 0x20, kAuxCtrl = 0x30, kAuxTransCtrl = 0x38, kAuxTimeoutCount = 0x3c, kAuxLimits = 0x40,
                   kAuxStatus = 0x44;
constexpr uint32_t kAuxDataNeverRead = 0x34, kAuxHpdAckNeverRead = 0x08;
// link (msm dp_reg.h): MAINLINK_CTRL bit 0 enable, bit 1 reset; STATE_CTRL TPS1..4 bits 0..3, bit 7 SEND_VIDEO, bit 8 PUSH_IDLE;
// CONFIGURATION_CTRL lanes - 1 in 5:4, bpc code in 10:8; lane map 0x38 (2 bits a lane); MAINLINK_READY bit 0 ready for video.
constexpr uint32_t kLinkMainlinkCtrl = 0x00, kLinkStateCtrl = 0x04, kLinkConfigCtrl = 0x08, kLinkTotalHorVer = 0x1c,
                   kLinkActiveHorVer = 0x28, kLinkMisc = 0x2c, kLinkLaneMap = 0x38, kLinkMainlinkReady = 0x40, kLinkLevels = 0x44,
                   kLinkTu = 0x4c;
// p0 (msm dp_reg.h, MMSS_DP_*): TIMING_ENGINE_EN bit 0, INTF_CONFIG, HSYNC_CTL, VSYNC_PERIOD_F0, DISPLAY_HCTL, TPG_MAIN_CONTROL.
constexpr uint32_t kP0BistEnable = 0x00, kP0TimingEngineEn = 0x10, kP0IntfConfig = 0x14, kP0HsyncCtl = 0x18, kP0VsyncPeriodF0 = 0x1c,
                   kP0DisplayHctl = 0x4c, kP0TpgMain = 0x60;

// ---------------------------------------------------------------------------------------------------- DISPCC's DP clocks (cc)
// Branch CBCRs (bit 31 CLK_OFF, bit 0 enable): DPTX0 LINK 0x804c, USB_ROUTER_LINK_INTF 0x8050, LINK_INTF 0x8054, CRYPTO 0x8058,
// PIXEL0 0x805c, PIXEL1 0x8060, AUX 0x8064. RCGs (clk-rcg2.c: CMD +0 bit 31 ROOT_OFF, bit 1 ROOT_EN; CFG +4 source 10:8, mode 13:12,
// half-integer divider 4:0; M +8, N +0xc, D +0x10): AUX 0x8234 (TCXO), LINK 0x81e8 (parent: the QMP link clock), PIXEL0 0x8204 and
// PIXEL1 0x821c (MND, parent: the QMP vco_div clock); the link divider 0x8200 (LINK_INTF's source).
constexpr uint32_t kCcDpLink = 0x804c, kCcDpRouterLinkIntf = 0x8050, kCcDpLinkIntf = 0x8054, kCcDpCrypto = 0x8058,
                   kCcDpPixel0 = 0x805c, kCcDpPixel1 = 0x8060, kCcDpAux = 0x8064;
constexpr uint32_t kCcDpAuxRcg = 0x8234, kCcDpLinkRcg = 0x81e8, kCcDpPixel0Rcg = 0x8204, kCcDpPixel1Rcg = 0x821c, kCcDpLinkDiv = 0x8200;
constexpr uint32_t kRcgCmd = 0x0, kRcgCfg = 0x4, kRcgM = 0x8, kRcgN = 0xc, kRcgD = 0x10;
constexpr uint32_t kRcgRootOff = 1u << 31;

// ------------------------------------------------------------------------------------------ the QMP PHY's DP half (qmp, DT)
// phy@88e8000 (UsbHostHw.h kQmp), sm8750_usb3dpphy_cfg with qmp_combo_offsets_v8: DP serdes +0x3000, DP TX A +0x3400, TX B +0x3800,
// DP_PHY +0x3c00. DP_PHY (phy-qcom-qmp-dp-phy.h): REVISION_ID0..3 +0x00..+0x0c, CFG +0x10, CFG_1 +0x14, PD_CTL +0x18, MODE +0x1c,
// AUX_CFG0..9 +0x20..+0x44; the v6 words sm8750's layout names (qmp_v8_usb3phy_regs_layout: QSERDES_V6_DP_PHY_*): VCO_DIV +0x70,
// AUX_INTERRUPT_STATUS +0xe0, STATUS +0xe4. The DP serdes' status words (QSERDES_V8_COM_*): CMN_STATUS +0x2c8, C_READY_STATUS +0x2f0.
// Register access needs the PHY's power domain and its aux and com_aux clocks (PianoUsbHost's rule, UsbHostSurvey.h phyOn).
constexpr uint32_t kQmpDpSerdes = 0x3000, kQmpDpTxA = 0x3400, kQmpDpTxB = 0x3800, kQmpDpPhy = 0x3c00;
constexpr uint32_t kDpPhyRevision0 = 0x00, kDpPhyCfg = 0x10, kDpPhyCfg1 = 0x14, kDpPhyPdCtl = 0x18, kDpPhyMode = 0x1c,
                   kDpPhyAuxCfg0 = 0x20, kDpPhyVcoDiv = 0x70, kDpPhyAuxIntStatus = 0xe0, kDpPhyStatus = 0xe4;
constexpr unsigned kDpPhyAuxCfgCount = 10;
constexpr uint32_t kSerdesCmnStatus = 0x2c8, kSerdesCReadyStatus = 0x2f0;
// PD_CTL bits (phy-qcom-qmp-dp-phy.h; set = that part powered): PWRDN 0, PSR_PWRDN 1, AUX_PWRDN 2, LANE_0_1 3, LANE_2_3 4, PLL 5,
// DP_CLAMP_EN 6. msm writes PSR_PWRDN alone at power-off (0x02) and PWRDN|PSR|AUX|PLL|CLAMP after the AUX init.
constexpr uint32_t kPdPwrdn = 1u << 0, kPdPsr = 1u << 1, kPdAux = 1u << 2, kPdLane01 = 1u << 3, kPdLane23 = 1u << 4, kPdPll = 1u << 5,
                   kPdClamp = 1u << 6;
constexpr uint32_t kPdPoweredOff = kPdPsr;
// The bring-up polls (qmp_v456_configure_dp_phy): C_READY_STATUS bit 0, CMN_STATUS bit 0 then bit 1, DP_PHY STATUS bit 0 (and bit 1
// once the lanes are configured, qmp_v4_configure_dp_phy).
constexpr uint32_t kCReady = 1u << 0, kCmnFreqDone = 1u << 0, kCmnPllLocked = 1u << 1, kPhyTsyncDone = 1u << 0, kPhyReady = 1u << 1;
// DP_COM (UsbHostHw.h offsets): RESET_OVRD_CTRL bits (qmp): DP PHY reset 0, its mux 1, USB3 PHY reset 2, its mux 3.
constexpr uint32_t kOvrdDpReset = 1u << 0, kOvrdDpResetMux = 1u << 1, kOvrdUsbReset = 1u << 2, kOvrdUsbResetMux = 1u << 3;

// --------------------------------------------------------------------------- the DPU's DP interfaces (msm catalog, dpu_kms.c)
// INTF0 (+0x34000) is DP controller 0's stream interface, INTF3 (+0x37000) its MST pair; TOP0 interrupt bits 24/25 (INTF0 underrun /
// vsync), 30/31 (INTF3). PINGPONG blocks have no registers on DPU 12 (catalog len 0): not read. msm's own snapshot
// (dpu_kms_mdp_snapshot) reads every CTL, DSPP, INTF and LM with only the MDSS clocks on: the INTF0/INTF3 words are read under
// PianoDpu's MDSS gate. TOP DP_PHY_INTF_SEL +0x460: which INTF feeds which DP PHY (dpu_hw_dp_phy_intf_sel).
constexpr unsigned kIntfDp = 0, kIntfDpMst = 3;

// ------------------------------------------------------------------------------------------ the port notification (glink)
// PMIC GLINK owner 0x800c, opcode word: bits 7:0 0x16 (USBC_NOTIFY_IND), bits 31:16 the SVID. struct usbc_notify: the 12-byte header,
// then port_idx @12, orientation @13 (0 normal, 1 reverse, else none), mux_ctrl @14 (0 no connection, 1 USB3 only, 2 DP 4 lanes, 3 USB3
// + DP, 4 tunnelling), res @15, vid @16 (le16), svid @18 (le16), the extended data @20 (DP: 5:0 pin assignment 0 HPD_OUT, 1 A .. 6 F,
// bit 6 HPD state, bit 7 IRQ_HPD; tunnelling: 2:0 USB speed, 5:3 cable type, 6 rounded, 7 power limited), reserved @28: 32 bytes,
// and Linux takes only exactly 32. Three ports (PMIC_GLINK_MAX_PORTS); piano's connector is port 0, the other two are acknowledged
// only (pan_ack). Linux's worker: orientation to the switch; SVID DP with mux "no connection" = the safe state (an HPD-low report),
// else the pin assignment (mode = pin - 1, at most F) to the mux and the HPD state to the DRM bridge; other SVIDs by mux: tunnelling
// TBT/USB4, USB3 only, none = safe; then the acknowledgement (PianoPower sends it).
constexpr uint32_t kPanOwner = 0x800c, kPanNotify = 0x16, kNotifyBytes = 32, kPorts = 3;
constexpr uint16_t kSvidDp = 0xff01, kSvidTbt = 0x8087;
enum Mux : uint8_t { kMuxNone = 0, kMuxUsb3 = 1, kMuxDp4 = 2, kMuxUsb3Dp = 3, kMuxTunnel = 4 };

// ----------------------------------------------------------------------------------------------------- the DIO4485 (fsa)
// I2C3 (i2c@a8c000) address 0x42 (UsbHostHw.h kMuxAddress): SWITCH_ENABLE 0x04 (DEVICE bit 7, SBU 6:5, USB 4:3, SENSE 2, MIC 1, AGND
// 0), SWITCH_SELECT 0x05 (SBU reverse 6:5, USB 4:3). fsa4480_set: USB states ENABLE DEVICE|USB, SELECT USB; pins C/E ENABLE DEVICE|SBU,
// SELECT 0 (SBU reverse when reversed); pins D/F ENABLE DEVICE|USB|SBU, SELECT USB (| SBU reverse); anything else (A, B) refused. Before
// a change with SBU on: SBU off and 35 us; after one turning SBU on: 15 us. Nothing here writes it (levels >= 2, docs section 6).
constexpr uint8_t kDioDevice = 0x80, kDioSbu = 0x60, kDioUsb = 0x18, kDioSelSbuReverse = 0x60, kDioSelUsb = 0x18;

// ------------------------------------------------------------------------------------------------------- the link (DT)
// The endpoint's link-frequencies: 1.62, 2.7, 5.4 GHz: piano's stock cap is HBR2 (Ubuntu dts K:b364e5a90e68); data-lanes 0..3.
// 8b/10b: a lane carries rate x 8 / 10 of payload.
constexpr uint32_t kRateRbr = 1620, kRateHbr = 2700, kRateHbr2 = 5400, kRateMaxMbps = kRateHbr2;

}  // namespace pianodp
