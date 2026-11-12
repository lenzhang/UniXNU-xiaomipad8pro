// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synopsys DWC_usb3x device-mode register facts used by PianoDwc3 (offsets from the core base, usb@a600000 on piano).
// Facts from the controller's programming model as mainline Linux drivers/usb/dwc3/core.h and gadget.h describe it, and as
// Project-SunUEFI's piano DWC3 driver (BSD-2-Clause-Patent) checked them on this tablet (docs/drivers/usb-device-debug.md).
// Names are ours; no Linux text.
#pragma once
#include <stdint.h>

namespace pianousb {
namespace dwc3 {

// Global registers
constexpr uint32_t GSBUSCFG0 = 0xc100, GSBUSCFG1 = 0xc104, GCTL = 0xc110, GSTS = 0xc118, GUCTL1 = 0xc11c, GSNPSID = 0xc120,
                   GUCTL = 0xc12c, GHWPARAMS0 = 0xc140, GHWPARAMS1 = 0xc144, GHWPARAMS3 = 0xc14c, GHWPARAMS6 = 0xc158,
                   GHWPARAMS7 = 0xc15c, GDBGLTSSM = 0xc164, GVERSION = 0xc1a0, GVERTYPE = 0xc1a4, GUSB2PHYCFG = 0xc200,
                   GUSB3PIPECTL = 0xc2c0, GTXFIFOSIZ0 = 0xc300, GRXFIFOSIZ0 = 0xc380, GEVNTADRLO = 0xc400, GEVNTADRHI = 0xc404,
                   GEVNTSIZ = 0xc408, GEVNTCOUNT = 0xc40c;
// Device registers
constexpr uint32_t DCFG = 0xc700, DCTL = 0xc704, DEVTEN = 0xc708, DSTS = 0xc70c, DALEPENA = 0xc720;
// Endpoint command block of physical endpoint n: DEPCMDPAR2, PAR1, PAR0, DEPCMD at +0, +4, +8, +12.
constexpr uint32_t depcmdBase(unsigned phys) { return 0xc800 + 16 * phys; }

// GCTL
constexpr uint32_t GCTL_DSBLCLKGTNG = 1u << 0, GCTL_CORESOFTRESET = 1u << 11, GCTL_PRTCAPDIR_MASK = 3u << 12,
                   GCTL_PRTCAP_DEVICE = 2u << 12;
// The role (GCTL.PRTCAPDIR): 1 host, 2 device, 3 OTG (0 is not a role). GSTS.CURMOD (bits 1:0) says which side runs: 0 device, 1 host.
enum : uint32_t { ROLE_NONE = 0, ROLE_HOST = 1, ROLE_DEVICE = 2, ROLE_OTG = 3 };
constexpr uint32_t roleOf(uint32_t gctl) { return (gctl >> 12) & 3u; }
constexpr uint32_t GSTS_CURMOD_MASK = 3u, GSTS_CURMOD_DEVICE = 0u, GSTS_CURMOD_HOST = 1u;
// The device registers (DCFG 0xc700 up to the last endpoint command block, below 0xcc00): only touched while GCTL reads the device
// role. The xHCI's registers (below 0x8000, CAPLENGTH at 0) only while it reads the host role. Everything else from 0xc100 is global.
constexpr bool isDeviceReg(uint32_t off) { return off >= 0xc700 && off < 0xcc00; }
constexpr bool isXhciReg(uint32_t off) { return off < 0x8000; }
// GUSB2PHYCFG / GUSB3PIPECTL: PHY suspend controls (piano DT: snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk)
constexpr uint32_t GUSB2PHYCFG_SUSPHY = 1u << 6, GUSB2PHYCFG_ENBLSLPM = 1u << 8, GUSB3PIPECTL_SUSPHY = 1u << 17;
// GEVNTSIZ
constexpr uint32_t GEVNTSIZ_INTMASK = 1u << 31;
// DCFG
constexpr uint32_t DCFG_DEVSPD_MASK = 7u, DCFG_SPEED_HS = 0u, DCFG_DEVADDR_MASK = 0x7fu << 3, DCFG_NUMP_MASK = 0x1fu << 17,
                   DCFG_LPM_CAP = 1u << 22;
constexpr uint32_t dcfgAddress(unsigned a) { return (a & 0x7fu) << 3; }
constexpr uint32_t dcfgNump(unsigned n) { return (n & 0x1fu) << 17; }
// DCTL
constexpr uint32_t DCTL_RUN_STOP = 1u << 31, DCTL_CSFTRST = 1u << 30, DCTL_ULSTCHNGREQ_MASK = 0xfu << 5,
                   DCTL_U1U2_MASK = 0xfu << 9, DCTL_TSTCTRL_MASK = 0xfu << 1;
// DSTS
constexpr uint32_t DSTS_CONNECTSPD_MASK = 7u, DSTS_DEVCTRLHLT = 1u << 22, DSTS_COREIDLE = 1u << 23;
constexpr uint32_t dstsLinkState(uint32_t v) { return (v >> 18) & 0xfu; }
// DEVTEN
constexpr uint32_t DEVTEN_DISCONN = 1u << 0, DEVTEN_USBRST = 1u << 1, DEVTEN_CONNECTDONE = 1u << 2, DEVTEN_ULSTCNG = 1u << 3,
                   DEVTEN_ERRTICERR = 1u << 9;
// DSTS.CONNECTSPD values
constexpr uint32_t SPEED_HS = 0, SPEED_FS_30_60 = 1, SPEED_LS = 2, SPEED_FS_48 = 3, SPEED_SS = 4;

// DEPCMD
constexpr uint32_t DEPCMD_CMDACT = 1u << 10, DEPCMD_CMDIOC = 1u << 8, DEPCMD_FORCERM = 1u << 11;
constexpr uint32_t depcmdStatus(uint32_t v) { return (v >> 12) & 0xfu; }
constexpr uint32_t depcmdResource(uint32_t v) { return (v >> 16) & 0x7fu; }
constexpr uint32_t depcmdParam(uint32_t v) { return (v & 0xffffu) << 16; }
enum : uint32_t {
  CMD_SETEPCONFIG = 1, CMD_SETTRANSFRESOURCE = 2, CMD_GETEPSTATE = 3, CMD_SETSTALL = 4, CMD_CLEARSTALL = 5,
  CMD_STARTTRANSFER = 6, CMD_UPDATETRANSFER = 7, CMD_ENDTRANSFER = 8, CMD_DEPSTARTCFG = 9,
};
// DEPCFG parameter 0 / 1
constexpr uint32_t depcfgType(unsigned t) { return (t & 3u) << 1; }
constexpr uint32_t depcfgMps(unsigned m) { return (m & 0x7ffu) << 3; }
constexpr uint32_t depcfgFifo(unsigned f) { return (f & 0x1fu) << 17; }
constexpr uint32_t DEPCFG_ACTION_INIT = 0, DEPCFG_ACTION_MODIFY = 2u << 30;
constexpr uint32_t DEPCFG_XFER_COMPLETE_EN = 1u << 8, DEPCFG_XFER_NOT_READY_EN = 1u << 10;
constexpr uint32_t depcfgInterval(unsigned m1) { return (m1 & 0xffu) << 16; }
constexpr uint32_t depcfgEpNumber(unsigned phys) { return (phys & 0x1fu) << 25; }
enum : unsigned { EP_CONTROL = 0, EP_ISOC = 1, EP_BULK = 2, EP_INTERRUPT = 3 };

// TRB (16 bytes: buffer low, buffer high, size/status, control)
constexpr uint32_t TRB_HWO = 1u << 0, TRB_LST = 1u << 1, TRB_CHN = 1u << 2, TRB_ISP_IMI = 1u << 10, TRB_IOC = 1u << 11;
constexpr uint32_t trbCtl(unsigned t) { return (t & 0x3fu) << 4; }
enum : unsigned { TRBCTL_NORMAL = 1, TRBCTL_CONTROL_SETUP = 2, TRBCTL_CONTROL_STATUS2 = 3, TRBCTL_CONTROL_STATUS3 = 4,
                  TRBCTL_CONTROL_DATA = 5 };
constexpr uint32_t trbResidual(uint32_t size) { return size & 0xffffffu; }
constexpr uint32_t trbStatus(uint32_t size) { return size >> 28; }
constexpr uint32_t TRBSTS_SETUP_PENDING = 2;

// Events (4 bytes each in the event buffer)
constexpr bool eventIsDevice(uint32_t e) { return (e & 1u) && ((e >> 1) & 0x7fu) == 0; }
constexpr uint32_t devEventType(uint32_t e) { return (e >> 8) & 0xfu; }
constexpr uint32_t devEventInfo(uint32_t e) { return (e >> 16) & 0x1ffu; }
enum : uint32_t { DEVT_DISCONNECT = 0, DEVT_RESET = 1, DEVT_CONNECT_DONE = 2, DEVT_LINK_CHANGE = 3, DEVT_WAKEUP = 4,
                  DEVT_SUSPEND = 6, DEVT_SOF = 7, DEVT_ERRATIC = 9, DEVT_CMD_CMPLT = 10, DEVT_OVERFLOW = 11 };
constexpr uint32_t epEventPhys(uint32_t e) { return (e >> 1) & 0x1fu; }
constexpr uint32_t epEventType(uint32_t e) { return (e >> 6) & 0xfu; }
constexpr uint32_t epEventStatus(uint32_t e) { return (e >> 12) & 0xfu; }
constexpr uint32_t epEventCommand(uint32_t e) { return (e >> 24) & 0xfu; }   // EPCMDCMPLT: the command type
enum : uint32_t { EPT_XFER_COMPLETE = 1, EPT_XFER_IN_PROGRESS = 2, EPT_XFER_NOT_READY = 3, EPT_STREAM = 6, EPT_CMD_COMPLETE = 7 };
// XferNotReady on a control endpoint: status & 3 = 1 data stage, 2 status stage
constexpr uint32_t EP0_NOTREADY_DATA = 1, EP0_NOTREADY_STATUS = 2;

}  // namespace dwc3
}  // namespace pianousb
