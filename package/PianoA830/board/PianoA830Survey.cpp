// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-gpu=1: the read-only survey of the GPU's power, clock, SMMU, GMU, RPMh and ZAP state as ABL and our UEFI left it, before
// the first bring-up session (docs/drivers/gpu.md). Nothing here writes a register, votes or calls the secure world: every access is
// a 32-bit load through an uncached device mapping, and every block is read only when what it needs is on:
//   GCC                 always on;
//   GPU_CC              only when GCC_GPU_CFG_AHB_CBCR is enabled (its CLK_OFF is no criterion: hardware clock gated);
//   SMMU, GMU, RSCC,    only when the CX GDSC is on (GPU_CC_CX_GDS_HW_CTRL or CX_GDSCR PWR_ON) and their clocks run (CLK_OFF clear):
//   CX_MISC, GX GDSC    the SMMU's HLOS1 vote branch; GPU_CC_AHB and CX_GMU for the GMU and its AO block; GPU_CC_AHB for RSCC and
//                       CX_MISC. The GX block (GPU core registers) is never read: it is only accessible once the GMU powered GX.
//   DRV-2               the apps RSC's DRV-2 (always on; PianoPower also uses it);
//   ZAP carve-out       last, after a line that says so: a region the secure world protects may abort the read.
// Each block's line comes before its first access, so a hang is attributable from the console. Register facts: the driver's
// A830Regs.h (offsets, bits) and platform/sm8750/Sm8750Rpmh.c (the TCS layouts).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>
#include "PianoA830.h"
#include "PianoGpuInputs.h"
#include "PianoGpuFirmwarePins.h"
#include "Adreno830Board.h"
#include "A830Regs.h"
#include "NativeAdrenoSha256.h"

extern "C" const uint8_t native_adreno_firmware_image[], native_adreno_sqe_firmware_image[], native_adreno_aqe_firmware_image[],
    native_adreno_zap_image[];
extern "C" const uint64_t native_adreno_firmware_image_size, native_adreno_sqe_firmware_image_size,
    native_adreno_aqe_firmware_image_size, native_adreno_zap_image_size;

namespace {
struct Win {
  IOMemoryMap *map = nullptr;
  uintptr_t va = 0;
  uint64_t phys = 0, bytes = 0;
};

bool mapWin(Win &w, const Adreno830Window &window)
{
  if (w.va) return true;
  IODeviceMemory *memory = IODeviceMemory::withRange(window.phys, window.bytes);
  w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (memory) memory->release();
  w.va = w.map ? uintptr_t(w.map->getVirtualAddress()) : 0;
  w.phys = window.phys;
  w.bytes = window.bytes;
  if (!w.va) piano_a830_note("survey: FAILED to map %#llx+%#llx", window.phys, window.bytes);
  return w.va != 0;
}

void unmapWin(Win &w)
{
  if (w.map) w.map->release();
  w = Win{};
}

// An explicit 32-bit load (no compiler-merged or widened access on Device memory).
uint32_t rd(const Win &w, uint32_t offset)
{
  if (offset + 4 > w.bytes) return 0xdeadbeef;
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(w.va + offset) : "memory");
  return v;
}

struct Reg {
  const char *name;
  uint32_t offset;
};

// name=value pairs of one block on as few lines as fit.
void dump(const char *block, const Win &w, const Reg *regs, unsigned count)
{
  char line[200];
  size_t n = 0;
  line[0] = 0;
  for (unsigned i = 0; i < count; ++i) {
    char item[48];
    snprintf(item, sizeof item, " %s=%08x", regs[i].name, rd(w, regs[i].offset));
    if (n + strlen(item) >= sizeof line - 1) {
      piano_a830_note("%s:%s", block, line);
      n = 0;
      line[0] = 0;
    }
    strlcpy(line + n, item, sizeof line - n);
    n += strlen(item);
  }
  if (n) piano_a830_note("%s:%s", block, line);
}

bool cbcrOn(uint32_t v) { return !(v & (1u << 31)); }   // CLK_OFF clear

void survey_inputs()
{
  const Adreno830Window gpu = PIANO_GPU_WINDOW_GPU, gmu = PIANO_GPU_WINDOW_GMU, smmu = PIANO_GPU_WINDOW_SMMU, zap = PIANO_GPU_WINDOW_ZAP,
                        gpucc = PIANO_GPU_WINDOW_GPUCC, drv2 = PIANO_GPU_WINDOW_RSC_DRV2;
  piano_a830_note("inputs: chip %#x soc %u (DT SoCs " PIANO_GPU_SOC_IDS_TEXT "); GPU %#llx+%#llx GMU %#llx+%#llx GPU_CC %#llx SMMU %#llx "
                  "DRV-2 %#llx ZAP %#llx+%#llx", PIANO_GPU_CHIP_ID, PIANO_GPU_SOC_ID, gpu.phys, gpu.bytes, gmu.phys, gmu.bytes, gpucc.phys,
                  smmu.phys, drv2.phys, zap.phys, zap.bytes);
  const uint32_t cb[] = PIANO_GPU_IRQ_SMMU_CB;
  piano_a830_note("inputs: INTID gpu %u cx_host %u hfi %u gmu %u smmu %u cb %u..%u scm-waitq %u; SID gpu %u lpac %u gmu %u; digest %.16s",
                  PIANO_GPU_IRQ_GPU, PIANO_GPU_IRQ_CX_HOST, PIANO_GPU_IRQ_HFI, PIANO_GPU_IRQ_GMU, PIANO_GPU_IRQ_SMMU_GLOBAL, cb[0], cb[3],
                  PIANO_GPU_IRQ_SCM_WAITQ, PIANO_GPU_SID_GPU, PIANO_GPU_SID_LPAC, PIANO_GPU_SID_GMU, PIANO_GPU_INPUTS_DIGEST);
  const struct {
    const char *name;
    const uint8_t *bytes;
    uint64_t length;
    const uint8_t *pin;
  } images[] = {{"gen80000_gmu.bin", native_adreno_firmware_image, native_adreno_firmware_image_size, PIANO_GPU_FW_GMU_SHA256},
                {"gen80000_sqe.fw", native_adreno_sqe_firmware_image, native_adreno_sqe_firmware_image_size, PIANO_GPU_FW_SQE_SHA256},
                {"gen80000_aqe.fw", native_adreno_aqe_firmware_image, native_adreno_aqe_firmware_image_size, PIANO_GPU_FW_AQE_SHA256},
                {"gen80000_zap.mbn", native_adreno_zap_image, native_adreno_zap_image_size, PIANO_GPU_FW_ZAP_SHA256}};
  for (const auto &image : images) {
    uint8_t digest[32];
    NativeAdrenoSha256::hash(image.bytes, size_t(image.length), digest);
    piano_a830_note("firmware %s: %llu bytes, sha256 %02x%02x%02x%02x... %s", image.name, image.length, digest[0], digest[1], digest[2],
                    digest[3], memcmp(digest, image.pin, 32) ? "DIFFERS from its pin" : "= pin");
  }
}

void survey_gcc(bool *gpuccReadable)
{
  *gpuccReadable = false;
  Win w;
  const Adreno830Window gcc = PIANO_GPU_WINDOW_GCC;
  piano_a830_note("GCC %#llx (always on): GPLL0, the GPU branches and votes", gcc.phys);
  if (!mapWin(w, gcc)) return;
  static const Reg regs[] = {{"GPLL0_MODE", SM8750_GCC_GPLL0_MODE}, {"GPLL0_L", SM8750_GCC_GPLL0_L_VAL},
                             {"GPLL0_ALPHA", SM8750_GCC_GPLL0_ALPHA_VAL}, {"BRANCH_VOTE", SM8750_GCC_APCS_CLOCK_BRANCH_ENA_VOTE},
                             {"GPLL_VOTE", SM8750_GCC_APCS_GPLL_ENA_VOTE}, {"GPU_CFG_AHB", SM8750_GCC_GPU_CFG_AHB_CBCR},
                             {"GPU_GEMNOC_GFX", SM8750_GCC_GPU_GEMNOC_GFX_CBCR}, {"DDRSS_GPU_AXI", SM8750_GCC_DDRSS_GPU_AXI_CBCR}};
  dump("GCC", w, regs, sizeof regs / sizeof regs[0]);
  const uint32_t ahb = rd(w, SM8750_GCC_GPU_CFG_AHB_CBCR), vote = rd(w, SM8750_GCC_APCS_CLOCK_BRANCH_ENA_VOTE);
  *gpuccReadable = (ahb & 3u) != 0;   // enabled or under hardware control
  piano_a830_note("GCC: GPU config AHB %s (CBCR %#x), GPLL0 GPU votes %s, GPLL0 %s", *gpuccReadable ? "enabled" : "NOT enabled", ahb,
                  (vote & SM8750_GCC_GPLL0_VOTE_GPU) == SM8750_GCC_GPLL0_VOTE_GPU ? "set" : "not set",
                  rd(w, SM8750_GCC_GPLL0_MODE) & SM8750_PLL_MODE_LOCK_DET ? "locked" : "not locked");
  unmapWin(w);
}

struct CxState {
  bool cx, ahb, gmu, smmu;
};

CxState survey_gpucc()
{
  CxState st = {};
  Win w;
  const Adreno830Window gpucc = PIANO_GPU_WINDOW_GPUCC;
  piano_a830_note("GPU_CC %#llx: PLL0, RCGs, branches, GDSCs", gpucc.phys);
  if (!mapWin(w, gpucc)) return st;
  static const Reg pll[] = {{"PLL0_MODE", SM8750_GPUCC_PLL0_MODE}, {"OPMODE", SM8750_GPUCC_PLL0_OPMODE}, {"L", SM8750_GPUCC_PLL0_L_VAL},
                            {"ALPHA", SM8750_GPUCC_PLL0_ALPHA_VAL}, {"USER", SM8750_GPUCC_PLL0_USER_CTL},
                            {"USER_U", SM8750_GPUCC_PLL0_USER_CTL_U}, {"CFG", SM8750_GPUCC_PLL0_CONFIG_CTL},
                            {"CFG_U", SM8750_GPUCC_PLL0_CONFIG_CTL_U}, {"CFG_U1", SM8750_GPUCC_PLL0_CONFIG_CTL_U1}};
  dump("GPU_CC PLL0", w, pll, sizeof pll / sizeof pll[0]);
  static const Reg rcg[] = {{"GMU_CMD", SM8750_GPUCC_GMU_CMD_RCGR}, {"GMU_CFG", SM8750_GPUCC_GMU_CMD_RCGR + 4},
                            {"HUB_CMD", SM8750_GPUCC_HUB_CMD_RCGR}, {"HUB_CFG", SM8750_GPUCC_HUB_CMD_RCGR + 4}};
  dump("GPU_CC RCG", w, rcg, sizeof rcg / sizeof rcg[0]);
  static const Reg gdsc[] = {{"CX_GDSCR", SM8750_GPUCC_CX_GDSCR}, {"CX_CFG_GDSCR", SM8750_GPUCC_CX_CFG_GDSCR},
                             {"CX_GDS_HW_CTRL", SM8750_GPUCC_CX_GDS_HW_CTRL}, {"CX_BCR", SM8750_GPUCC_CX_BCR},
                             {"GX_BCR", SM8750_GPUCC_GX_BCR}, {"GMU_BCR", SM8750_GPUCC_GMU_BCR}, {"XO_BCR", SM8750_GPUCC_XO_BCR},
                             {"CB_BCR", SM8750_GPUCC_CB_BCR}, {"FAST_HUB_BCR", SM8750_GPUCC_FAST_HUB_BCR}};
  dump("GPU_CC GDSC/BCR", w, gdsc, sizeof gdsc / sizeof gdsc[0]);
  static const Reg cbcr[] = {{"SMMU_VOTE", SM8750_GPUCC_HLOS1_VOTE_SMMU_CBCR}, {"RSCC_XO_AON", SM8750_GPUCC_RSCC_XO_AON_CBCR},
                             {"CXO_AON", SM8750_GPUCC_CXO_AON_CBCR}, {"DEMET", SM8750_GPUCC_DEMET_CBCR},
                             {"GX_AHB_FF", SM8750_GPUCC_GX_AHB_FF_CBCR}, {"AHB", SM8750_GPUCC_AHB_CBCR},
                             {"SLEEP", SM8750_GPUCC_SLEEP_CBCR}, {"CX_GMU", SM8750_GPUCC_CX_GMU_CBCR}, {"CXO", SM8750_GPUCC_CXO_CBCR},
                             {"HUB_CX_INT", SM8750_GPUCC_HUB_CX_INT_CBCR}, {"CB", SM8750_GPUCC_CB_CBCR},
                             {"RSCC_HUB_AON", SM8750_GPUCC_RSCC_HUB_AON_CBCR}, {"HUB_AON", SM8750_GPUCC_HUB_AON_CBCR}};
  dump("GPU_CC CBCR", w, cbcr, sizeof cbcr / sizeof cbcr[0]);
  st.cx = (rd(w, SM8750_GPUCC_CX_GDS_HW_CTRL) & SM8750_GDSC_PWR_ON) || (rd(w, SM8750_GPUCC_CX_GDSCR) & SM8750_GDSC_PWR_ON);
  st.ahb = st.cx && cbcrOn(rd(w, SM8750_GPUCC_AHB_CBCR));
  st.gmu = st.ahb && cbcrOn(rd(w, SM8750_GPUCC_CX_GMU_CBCR)) && cbcrOn(rd(w, SM8750_GPUCC_HUB_CX_INT_CBCR));
  st.smmu = st.cx && cbcrOn(rd(w, SM8750_GPUCC_HLOS1_VOTE_SMMU_CBCR));
  const uint32_t mode = rd(w, SM8750_GPUCC_PLL0_MODE);
  piano_a830_note("GPU_CC: CX GDSC %s, AHB %s, GMU clocks %s, SMMU clock %s, PLL0 %s (L %#x)", st.cx ? "ON" : "off", st.ahb ? "on" : "off",
                  st.gmu ? "on" : "off", st.smmu ? "on" : "off",
                  mode & SM8750_PLL_MODE_LOCK_DET ? "locked" : (mode & SM8750_PLL_MODE_OUTCTRL ? "out, unlocked" : "off"),
                  rd(w, SM8750_GPUCC_PLL0_L_VAL));
  unmapWin(w);
  return st;
}

void survey_smmu()
{
  Win w;
  const Adreno830Window smmu = PIANO_GPU_WINDOW_SMMU;
  piano_a830_note("SMMU %#llx: global space (Gunyah-emulated), stream-match table, banks", smmu.phys);
  if (!mapWin(w, smmu)) return;
  const uint32_t idr0 = rd(w, A830_SMMU_GR0_IDR0), idr1 = rd(w, A830_SMMU_GR0_IDR1), idr2 = rd(w, A830_SMMU_GR0_IDR2);
  piano_a830_note("SMMU: sCR0 %#x sACR %#x IDR0 %#x IDR1 %#x IDR2 %#x (13T: %#x %#x %#x) sGFSR %#x sGFAR %#x%08x sGFSYNR0 %#x",
                  rd(w, A830_SMMU_GR0_SCR0), rd(w, A830_SMMU_GR0_SACR), idr0, idr1, idr2, A830_SMMU_IDR0_SM8750, A830_SMMU_IDR1_SM8750,
                  A830_SMMU_IDR2_SM8750, rd(w, A830_SMMU_GR0_SGFSR), rd(w, A830_SMMU_GR0_SGFAR_HI), rd(w, A830_SMMU_GR0_SGFAR),
                  rd(w, A830_SMMU_GR0_SGFSYNR0));
  unsigned groups = A830_SMMU_IDR0_NUMSMRG(idr0), banks = A830_SMMU_IDR1_NUMCB(idr1);
  const unsigned pages = A830_SMMU_GLOBAL_PAGES(idr1);
  if (groups > 128) groups = 128;
  if (banks > 64) banks = 64;
  uint64_t used = 0;
  unsigned valid = 0;
  for (unsigned i = 0; i < groups; ++i) {
    const uint32_t smr = rd(w, A830_SMMU_GR0_SMR(i)), s2cr = rd(w, A830_SMMU_GR0_S2CR(i));
    if (!(smr & 0x80000000u) && !s2cr) continue;
    ++valid;
    const unsigned type = (s2cr >> 16) & 3u, bank = s2cr & 0xffu;
    if ((smr & 0x80000000u) && type == 0 && bank < 64) used |= 1ull << bank;
    piano_a830_note("SMMU smr%u %#x (%s sid %#x mask %#x) s2cr %#x (%s%s%u)", i, smr, smr & 0x80000000u ? "valid" : "free", smr & 0x7fffu,
                    (smr >> 16) & 0x7fffu, s2cr, type == 0 ? "translate" : type == 1 ? "bypass" : "fault", type == 0 ? " bank " : " ",
                    type == 0 ? bank : 0u);
  }
  piano_a830_note("SMMU: %u of %u stream-match groups set; %u banks, %u global pages; banks in use %#llx", valid, groups, banks, pages, used);
  used |= 1ull << 0 | 1ull << 2;   // and the two the driver plans (CB0 GPU, CB2 GMU)
  for (unsigned n = 0; n < banks; ++n) {
    if (!(used >> n & 1)) continue;
    const uint32_t cb = A830_SMMU_CB_OFF(pages, n);
    if (cb + 0x1000 > w.bytes) break;
    piano_a830_note("SMMU cb%u: CBAR %#x CBA2R %#x SCTLR %#x TCR %#x TTBR0 %#x%08x MAIR0 %#x ACTLR %#x FSR %#x FAR %#x%08x FSYNR0 %#x", n,
                    rd(w, A830_SMMU_GR1_CBAR(n)), rd(w, A830_SMMU_GR1_CBA2R(n)), rd(w, cb + A830_SMMU_CB_SCTLR), rd(w, cb + A830_SMMU_CB_TCR),
                    rd(w, cb + A830_SMMU_CB_TTBR0_HI), rd(w, cb + A830_SMMU_CB_TTBR0), rd(w, cb + A830_SMMU_CB_MAIR0),
                    rd(w, cb + A830_SMMU_CB_ACTLR), rd(w, cb + A830_SMMU_CB_FSR), rd(w, cb + A830_SMMU_CB_FAR_HI), rd(w, cb + A830_SMMU_CB_FAR),
                    rd(w, cb + A830_SMMU_CB_FSYNR0));
  }
  unmapWin(w);
}

void survey_gmu(const CxState &st, bool gx)
{
  const Adreno830Window gmuWindow = PIANO_GPU_WINDOW_GMU, rscc = PIANO_GPU_WINDOW_RSCC, cxm = PIANO_GPU_WINDOW_CX_MISC;
  if (st.gmu) {
    Win w;
    piano_a830_note("GMU %#llx: CM3, HFI, AO block, TCM version words", gmuWindow.phys);
    if (mapWin(w, gmuWindow)) {
      static const Reg regs[] = {{"CM3_SYSRESET", A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_SYSRESET)},
                                 {"CM3_BOOT_CONFIG", A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_BOOT_CONFIG)},
                                 {"CM3_FW_INIT", A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_FW_INIT_RESULT)},
                                 {"GFX_PWR_CLK", A830_GMU_REG_BYTE(A8XX_GMUCX_GFX_PWR_CLK_STATUS)},
                                 {"RPMH_PWR_STATE", A830_GMU_REG_BYTE(A8XX_GMUCX_RPMH_POWER_STATE)},
                                 {"G2H_INFO", A830_GMU_REG_BYTE(A8XX_GMUCX_GMU2HOST_INTR_INFO)},
                                 {"G2H_MASK", A830_GMU_REG_BYTE(A8XX_GMUCX_GMU2HOST_INTR_MASK)},
                                 {"HFI_CTRL", A830_GMU_REG_BYTE(A8XX_GMUCX_HFI_CTRL_STATUS)},
                                 {"GENERAL_10", A830_GMU_REG_BYTE(A8XX_GMUCX_GENERAL_10)},
                                 {"GENERAL_11", A830_GMU_REG_BYTE(A8XX_GMUCX_GENERAL_11)},
                                 {"AO_HOST_INT", A830_GMU_REG_BYTE(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS)},
                                 {"RSCC_ACK", A830_GMU_REG_BYTE(A8XX_GMUAO_RSCC_CONTROL_ACK)},
                                 {"CX_BUSY", A830_GMU_REG_BYTE(A8XX_GMUAO_GPU_CX_BUSY_STATUS)},
                                 {"AHB_FENCE", A830_GMU_REG_BYTE(A8XX_GMUAO_AHB_FENCE_STATUS)},
                                 {"DTCM_CORE_FW", A830_GMU_REG_BYTE(A8XX_GMU_DTCM_CORE_FW_VERSION)},
                                 {"DTCM_PWR_FW", A830_GMU_REG_BYTE(A8XX_GMU_DTCM_PWR_FW_VERSION)},
                                 {"DTCM_HFI", A830_GMU_REG_BYTE(A8XX_GMU_DTCM_HFI_VERSION)}};
      dump("GMU", w, regs, sizeof regs / sizeof regs[0]);
      if (gx) {
        piano_a830_note("GX GDSC (gx_clkctl %#llx, opt-in)", A830_GX_GDSCR_PHYS);
        const uint32_t at = uint32_t(A830_GX_GDSCR_PHYS - gmuWindow.phys);
        piano_a830_note("GX: GDSCR %#x CFG_GDSCR %#x", rd(w, at), rd(w, at + 4));
      }
      unmapWin(w);
    }
  } else {
    piano_a830_note("GMU: skipped (CX GDSC %s, AHB %s, GMU clocks %s): reading it now could hang the bus", st.cx ? "on" : "off",
                    st.ahb ? "on" : "off", st.gmu ? "on" : "off");
  }
  if (!st.ahb) {
    piano_a830_note("RSCC, CX_MISC: skipped (CX GDSC or GPU_CC AHB off)");
    return;
  }
  Win r;
  piano_a830_note("RSCC %#llx", rscc.phys);
  if (mapWin(r, rscc)) {
    static const Reg regs[] = {{"RSC_STATUS0", A830_RSCC_REG_BYTE(A8XX_RSCC_RSC_STATUS0_DRV0)},
                               {"SEQ_BUSY", A830_RSCC_REG_BYTE(A8XX_RSCC_SEQ_BUSY_DRV0)},
                               {"TCS0", A830_RSCC_REG_BYTE(A8XX_RSCC_TCS_DRV0_STATUS(0))},
                               {"TCS1", A830_RSCC_REG_BYTE(A8XX_RSCC_TCS_DRV0_STATUS(1))},
                               {"TCS2", A830_RSCC_REG_BYTE(A8XX_RSCC_TCS_DRV0_STATUS(2))},
                               {"TCS3", A830_RSCC_REG_BYTE(A8XX_RSCC_TCS_DRV0_STATUS(3))}};
    dump("RSCC", r, regs, sizeof regs / sizeof regs[0]);
    unmapWin(r);
  }
  Win c;
  piano_a830_note("CX_MISC %#llx", cxm.phys);
  if (mapWin(c, cxm)) {
    static const Reg regs[] = {{"AHB_AON", A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_CX_AHB_AON_CNTL)},
                               {"AHB_GMU", A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_CX_AHB_GMU_CNTL)},
                               {"AHB_CP", A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_CX_AHB_CP_CNTL)},
                               {"AHB_VBIF_SMMU", A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_CX_AHB_VBIF_SMMU_CNTL)},
                               {"AHB_HOST", A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_CX_AHB_HOST_CNTL)}};
    dump("CX_MISC", c, regs, sizeof regs / sizeof regs[0]);
    unmapWin(c);
  }
}

void survey_drv2()
{
  Win w;
  const Adreno830Window drv2 = PIANO_GPU_WINDOW_RSC_DRV2;
  piano_a830_note("apps RSC DRV-2 %#llx: version, TCS 0..2 (the driver's votes go through an idle one)", drv2.phys);
  if (!mapWin(w, drv2)) return;
  const uint32_t id = rd(w, 0x0), config = rd(w, 0xc), major = id >> 16 & 0xff;
  const uint32_t wait = major == 2 ? 0x10 : 0x20, control = wait + 4, status = wait + 8, enable = wait + 0xc;
  piano_a830_note("DRV-2: id %#x (v%u) config %#x (DRV-2 NTCS %u NCMD %u) IRQ_ENABLE %#x IRQ_STATUS %#x", id, major, config, config >> 12 & 0x3f,
                  config >> 27 & 0x1f, rd(w, 0xd00), rd(w, 0xd04));
  for (unsigned t = 0; t < 3; ++t) {
    const uint32_t b = 0xd00 + t * 0x2a0;
    piano_a830_note("DRV-2 TCS%u: WAIT %#x CONTROL %#x STATUS %#x CMD_ENABLE %#x (%s)", t, rd(w, b + wait), rd(w, b + control),
                    rd(w, b + status), rd(w, b + enable),
                    !rd(w, b + control) && !rd(w, b + enable) && !rd(w, b + wait) && rd(w, b + status) == 1 ? "idle" : "busy");
  }
  unmapWin(w);
}

void survey_zap()
{
  Win w;
  const Adreno830Window zap = PIANO_GPU_WINDOW_ZAP;
  piano_a830_note("ZAP carve-out %#llx+%#llx (no-map): reading it now; if the log ends here the secure world protects it", zap.phys, zap.bytes);
  if (!mapWin(w, zap)) return;
  uint32_t nonzero = 0, sum = 0;
  for (uint32_t at = 0; at < w.bytes; at += 4) {
    const uint32_t v = rd(w, at);
    nonzero += v != 0;
    sum = (sum << 1 | sum >> 31) ^ v;
  }
  piano_a830_note("ZAP: words %08x %08x %08x %08x | +0x1000 %08x %08x; %u of %llu words non-zero, fold %#x", rd(w, 0), rd(w, 4), rd(w, 8),
                  rd(w, 12), rd(w, 0x1000), rd(w, 0x1004), nonzero, w.bytes / 4, sum);
  unmapWin(w);
}
}  // namespace

// After the bring-up (the GPU on, its SMMU clocked): where each GPU-side stream goes. The only way the GPU or the GMU could write
// memory XNU does not own (everything below 0x8c0000000 on piano: firmware carve-outs, the console mailbox cvp@9cb00000) is a
// stream that reaches memory untranslated, IOVA = PA: an S2CR bypass entry, a translating entry whose bank has SCTLR.M = 0, or no
// entry at all while sCR0.USFCFG = 0 (unmatched streams bypass). Every GPU client window (0x20000000 .. 0x3ffffe000) lies below
// XNU's DRAM, so such a stream would write there. Read only; one line per stream ID 0..15 and a verdict.
extern "C" unsigned piano_a830_smmu_routes(const char *when)
{
  Win w;
  const Adreno830Window smmu = PIANO_GPU_WINDOW_SMMU;
  if (!mapWin(w, smmu)) return ~0u;
  const uint32_t scr0 = rd(w, A830_SMMU_GR0_SCR0), idr0 = rd(w, A830_SMMU_GR0_IDR0), idr1 = rd(w, A830_SMMU_GR0_IDR1);
  unsigned groups = A830_SMMU_IDR0_NUMSMRG(idr0), banks = A830_SMMU_IDR1_NUMCB(idr1);
  const unsigned pages = A830_SMMU_GLOBAL_PAGES(idr1);
  if (groups > 128) groups = 128;
  if (banks > 64) banks = 64;
  const bool clientpd = scr0 & A830_SMMU_SCR0_CLIENTPD, usfcfg = scr0 & A830_SMMU_SCR0_USFCFG;
  piano_a830_note("SMMU routes (%s): sCR0 %#x (CLIENTPD %u: %s; USFCFG %u: unmatched streams %s), %u groups, %u banks", when, scr0,
                  clientpd, clientpd ? "EVERY stream bypasses" : "translation on", usfcfg, usfcfg ? "fault" : "BYPASS", groups, banks);
  unsigned danger = 0;
  for (uint32_t sid = 0; sid < 16; ++sid) {
    unsigned matches = 0, slot = 0;
    uint32_t s2cr = 0;
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = rd(w, A830_SMMU_GR0_SMR(i));
      if (!(smr & 0x80000000u) || ((smr ^ sid) & 0x7fffu & ~((smr >> 16) & 0x7fffu))) continue;
      if (!matches) {
        slot = i;
        s2cr = rd(w, A830_SMMU_GR0_S2CR(i));
      }
      ++matches;
    }
    const char *route;
    char detail[96] = "";
    bool untranslated = false;
    if (clientpd) {
      route = "bypass (CLIENTPD)";
      untranslated = true;
    } else if (!matches) {
      route = usfcfg ? "unmatched: fault" : "unmatched: BYPASS";
      untranslated = !usfcfg;
    } else {
      const unsigned type = (s2cr >> 16) & 3u, bank = s2cr & 0xffu;
      if (type == 1) {
        route = "S2CR BYPASS";
        untranslated = true;
      } else if (type == 2) {
        route = "S2CR fault";
      } else if (bank >= banks || A830_SMMU_CB_OFF(pages, bank) + 0x1000 > w.bytes) {
        route = "bank out of range";
      } else {
        const uint32_t sctlr = rd(w, A830_SMMU_CB_OFF(pages, bank) + A830_SMMU_CB_SCTLR), cbar = rd(w, A830_SMMU_GR1_CBAR(bank));
        untranslated = !(sctlr & 1u);
        route = untranslated ? "bank with SCTLR.M=0: UNTRANSLATED" : "translated";
        snprintf(detail, sizeof detail, " bank %u SCTLR %#x CBAR %#x TTBR0 %#x%08x", bank, sctlr, cbar,
                 rd(w, A830_SMMU_CB_OFF(pages, bank) + A830_SMMU_CB_TTBR0_HI), rd(w, A830_SMMU_CB_OFF(pages, bank) + A830_SMMU_CB_TTBR0));
      }
      if (matches > 1) route = "MULTIPLE stream-match entries (unpredictable)";
    }
    if (matches || untranslated || sid < 8)
      piano_a830_note("SMMU sid %u: %s%s%s (slot %u, %u match%s, S2CR %#x)", sid, route, detail, untranslated ? " <- IOVA = PA" : "",
                      slot, matches, matches == 1 ? "" : "es", s2cr);
    danger += untranslated;
  }
  piano_a830_note("SMMU routes (%s): %s", when,
                  danger ? "a GPU-side stream reaches memory UNTRANSLATED: its writes at a client IOVA land at that physical address "
                           "(below 0x8c0000000: firmware memory, the console mailbox)"
                         : "every GPU-side stream that has an entry is translated or faults; nothing reaches memory as IOVA = PA");
  unmapWin(w);
  return danger;
}

extern "C" void piano_a830_survey(uint32_t mask)
{
  if (mask & PianoA830SurveyInputs) survey_inputs();
  bool gpucc = false;
  if (mask & (PianoA830SurveyGcc | PianoA830SurveyGpuCc | PianoA830SurveySmmu | PianoA830SurveyGmu | PianoA830SurveyGx))
    survey_gcc(&gpucc);
  CxState st = {};
  if (mask & (PianoA830SurveyGpuCc | PianoA830SurveySmmu | PianoA830SurveyGmu | PianoA830SurveyGx)) {
    if (gpucc) st = survey_gpucc();
    else piano_a830_note("GPU_CC: skipped (GCC's GPU config AHB clock is not enabled): the CX blocks stay unread");
  }
  if (mask & PianoA830SurveySmmu) {
    if (st.smmu) survey_smmu();
    else piano_a830_note("SMMU: skipped (CX GDSC %s, SMMU clock %s): reading it now could hang the bus", st.cx ? "on" : "off",
                         st.smmu ? "on" : "off");
  }
  if (mask & (PianoA830SurveyGmu | PianoA830SurveyGx)) survey_gmu(st, (mask & PianoA830SurveyGx) != 0);
  if (mask & PianoA830SurveyDrv2) survey_drv2();
  if (mask & PianoA830SurveyZap) survey_zap();
}
