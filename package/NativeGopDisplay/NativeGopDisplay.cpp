// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeGopDisplay: macOS 27's display on piano's panel without a display controller driver. An IOMobileFramebufferLegacy
// (IOMobileGraphicsFamily) whose scanout is the linear BGRA framebuffer the firmware left running: ABL's splash region, which
// UEFI's SimpleFbDxe exposes as the GOP and DarwinBoot hands to XNU as the boot console (boot_args Video, PE_state.video). The DPU
// pipeline ABL started keeps scanning that memory at the panel's refresh; this driver only writes it. No display register is touched.
//
// Derived from macOS-DroidVM's DroidVMDisplay (overlay/kext/DroidVMDisplay/DroidVMDisplay.cpp, see UPSTREAM): its stage 1, the CPU copy
// of each swap's main IOSurface into the boot framebuffer, with the family hooks AppleParavirtDisplay (vma2) overrides. The virtio-gpu
// scanout (stage 2) and the waits for the virtio GPU's 3D work are gone; the defaults are the panel's.
//
// Discovery is DroidVMDisplay's: IOMobileFramebuffer.framework (iomfb_populate_all_display_infos) counts the DisplayPortCount of the
// AppleParavirtGPU services when an /arm-io node is compatible "paravirtualizedgraphics,gpu", and waits for that many framebuffers.
// The native ADT has such a node (tooling/adt/make-piano-adt.py, configs/k27-gop-display.toml); this kext also provides the nub,
// AppleParavirtGPU (end of this file), so DroidVMDisplay is not linked natively. In a collection that still has DroidVMDisplay this
// class is a second candidate for the nub's display with a higher probe score. It takes the nub only on a node marked native-gop-display (that opt-in ADT's own node), not with boot-arg
// native-gop-display=0, and not in the VM (no /arm-io/virtio-gpu). Otherwise DroidVMDisplay's own display (AppleParavirtDisplay)
// starts on the nub, which on piano is its CPU copy into the same framebuffer. One display is published either way: WindowServer
// never has two to choose from.
//
// Every step logs "NativeGopDisplay: ..." (IOLog), so the pstore console mailbox of a headless boot tells how far it got.
#include "IOMobileFramebufferABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/device_tree.h>

extern "C" task_t kernel_task;

// macOS 27.0.1's (26A434, the k27 collection's) offsets, as DroidVMDisplay has them (its notes give the AppleParavirtDisplay code each
// was read from).
// IOMobileFramebufferLegacy (instance 0x1dd8)
static constexpr size_t kCurrentSwap = 6720;       // the swap being programmed
static constexpr size_t kSwapQueueFirst = 6728;    // TAILQ head of the pending swaps
static constexpr size_t kSwapQueueLast = 6736;     // its tqh_last
static constexpr size_t kDisplayWidth = 7152;      // display_width() / display_height()
static constexpr size_t kDisplayHeight = 7156;
static constexpr size_t kLastCompletedSwap = 7164; // u32: the swap swap_complete_gated finished last
static constexpr size_t kSystemPowerState = 7180;  // u32: setPowerState's state
static constexpr size_t kAPIPowerState = 7184;     // u32: setAPIPowerState's state
static constexpr size_t kHardwarePowered = 7188;   // u32: the display is powered (swap_wait fails kIOReturnOffline while it is 0)
static constexpr size_t kSwapInfoHeld = 7634;      // u16: swap info notifications are held
static constexpr size_t kSwapInfoTime = 264;       // the swap info's time
// IOMobileFramebuffer (instance 144): set before power management in AppleParavirtDisplay's start_hardware block
static constexpr size_t kHardwareStarted = 140;
// IOMFBSwapIORequest
static constexpr size_t kSwapLayers = 984;         // bit 0: main surface, bit 1: cursor surface
static constexpr size_t kSwapID = 980;             // u32: the swap's ID
static constexpr size_t kSwapDropReasons = 1004;   // programmed when nothing but bit 3 is set, else dropped
static constexpr size_t kSwapInfoA = 912;          // swap info words 3 and 4
static constexpr size_t kSwapInfoB = 976;
static constexpr size_t kSwapMainSurface = 2944;   // layer i's surface at + 8 * i
static constexpr unsigned kSwapLayerCount = 4;
static constexpr size_t kSwapNext = 3384;          // TAILQ entry of the queue
static constexpr size_t kSwapPrev = 3392;
// IOMFBNotifyRequest
static constexpr size_t kNotifyAsyncReference = 24;   // OSAsyncReference64 of the user client
static constexpr size_t kNotifyMask = 120;            // which swap info words the client wants; bit 31: as 32-bit halves
static constexpr size_t kNotifyLastTime = 128;        // the time of the previous notification (word 1 is the delta)
// IOAVElement: the element ID and its score
static constexpr size_t kElementID = 16;
static constexpr size_t kElementScore = 24;

template <typename T> static T &at(void *base, size_t offset) {return *reinterpret_cast<T *>(static_cast<uint8_t *>(base) + offset);}

// Kernel interfaces of the collection's kexts with no header here.
void iomfbUpdateSwapInfoState(IOMobileFramebufferLegacy *, IOMFBSwapIORequest *, uint32_t)
    __asm("__ZN25IOMobileFramebufferLegacy28update_swap_info_state_gatedEP18IOMFBSwapIORequest24IOMFBSwapCompletionState");
void iomfbSwapInfoDispatch(IOMobileFramebufferLegacy *, void *)
    __asm("__ZN25IOMobileFramebufferLegacy31swap_info_notify_dispatch_gatedEPNS_12SwapInfoBlobE");
IOReturn userClientSendAsyncResult(io_user_reference_t *reference, IOReturn, io_user_reference_t *args, UInt32 count, IOOptionBits)
    __asm("__ZN12IOUserClient28sendAsyncResult64WithOptionsEPyiS0_jj");   // protected in IOUserClient.h
uint64_t ioSurfaceWidth(const IOSurface *) __asm("__ZNK9IOSurface8getWidthEv");
uint64_t ioSurfaceHeight(const IOSurface *) __asm("__ZNK9IOSurface9getHeightEv");
uint64_t ioSurfaceBytesPerRow(const IOSurface *) __asm("__ZNK9IOSurface14getBytesPerRowEv");
uint64_t ioSurfaceBaseOffset(const IOSurface *) __asm("__ZNK9IOSurface13getBaseOffsetEv");
uint32_t ioSurfacePixelFormat(const IOSurface *) __asm("__ZNK9IOSurface14getPixelFormatEv");
IOMemoryDescriptor *ioSurfaceMemory(const IOSurface *) __asm("__ZNK9IOSurface19getMemoryDescriptorEv");
uint32_t ioSurfacePlaneCount(const IOSurface *) __asm("__ZNK9IOSurface13getPlaneCountEv");
uint64_t ioSurfacePlaneOffset(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface14getPlaneOffsetEj");
uint64_t ioSurfacePlaneBytesPerRow(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface19getPlaneBytesPerRowEj");
uint64_t ioSurfacePlaneBytesPerElement(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface23getPlaneBytesPerElementEj");
uint64_t ioSurfacePlaneWidth(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface13getPlaneWidthEj");
uint64_t ioSurfacePlaneHeight(const IOSurface *, uint32_t) __asm("__ZNK9IOSurface14getPlaneHeightEj");
// IOAVVideoTimingData: per axis (horizontal, vertical) 32 bytes of which total +4, active +8, sync width +12, back porch +16,
// front porch +20, sync rate +24, sync polarity +28, then the pixel clock at +68.
OSObject *ioavTimingWithData(const void *data, uint32_t standardType, uint32_t timingStandard)
    __asm("__ZN22IOAVVideoTimingElement8withDataEPK19IOAVVideoTimingData16IOAVStandardType23IOAVVideoTimingStandard");
void ioavTimingSetColorElements(OSObject *, OSArray *) __asm("__ZN22IOAVVideoTimingElement16setColorElementsEP7OSArray");
OSObject *ioavColorWithCapabilities(uint32_t standardType, uint32_t depth, uint32_t encoding, uint32_t range, uint32_t eotf,
                                    uint32_t colorimetry, bool, const OSDictionary *, const OSDictionary *, const OSDictionary *,
                                    const OSSet *)
    __asm("__ZN21IOAVVideoColorElement16withCapabilitiesE16IOAVStandardTypej22IOAVVideoPixelEncoding26IOAVVideoColorDynamicRange"
          "18IOAVVideoColorEOTF20IOAVVideoColorimetrybPK12OSDictionaryS7_S7_PK5OSSet");

class NativeGopDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeGopDisplay)
public:
    IOService *probe(IOService *, SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    bool start_hardware() override;
    IOReturn stop_hardware_gated() override;
    IOReturn swap_begin_gated() override;
    IOReturn swap_wait(IOMobileFramebufferUserClient *, unsigned int, unsigned int, unsigned int) override;
    IOReturn swap_submit(IOMFBSwapRec *, IOUserClient *, unsigned int *) override;
    IOReturn verify_swap(IOMFBSwapIORequest *) override;
    IOReturn swap_layer_map(IOMFBSwapIORequest *, unsigned int) override;
    IOReturn surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int, IOSurface *) override;
    IOReturn surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int) override;
    IOReturn set_display_device(unsigned int) override;
    IOReturn set_digital_out_mode(unsigned int, unsigned int) override;
    IOReturn get_dot_pitch(unsigned int *) override;
    IOReturn get_gamma_table(IOMFBGammaTable *) override;
    IOReturn set_gamma_table(IOMFBGammaTable *) override;
    IOReturn set_color_remap_mode(DisplayColorRemapMode) override;
    IOReturn get_color_remap_mode(DisplayColorRemapMode *) const override;
    IOReturn swap_set_color_matrix(IOMFBColorFixedMatrix *, IOMFBColorMatrixFunction, unsigned int) override;
    IOReturn set_notify_tag_mask_gated(IOMFB_NotificationType, IOMFBNotifyRequest *, unsigned int) override;
    IOReturn supported_frame_info(unsigned int, unsigned int *, char (*)[64], unsigned int *) override;
    IOReturn factory_portal(void *, task *) override;
    IOReturn executeFactoryConfigurations_gated() override;
    IOReturn set_frame_info_notification_state_gated(IOMFBNotifyRequest *, bool) override;
    IOReturn do_swap_info_notify_gated(IOMFBNotifyRequest *, const SwapInfoBlob *) override;
    IOReturn notify_swap_done_gated(unsigned int, ::SwapInfoBlob *, unsigned int *, unsigned int) override;
    IOReturn notify_crc_ready_gated(unsigned int) override;
    bool are_timings_enabled() const override;
    IOReturn do_power_state_change_gated() override;

private:
    // The timing published (ADT refresh-hz, boot-arg native-gop-display-hz, 30 to 240; default 60). The panel itself keeps the mode
    // ABL set (120 Hz); this is the rate WindowServer is told and the VBL timer runs at.
    uint32_t fRefreshHz;
    // The panel's physical size (ADT panel-width-mm / panel-height-mm, boot-arg native-gop-display-mm=WIDTHxHEIGHT; default piano's
    // 239x163 mm, about 340 dpi at 3200x2136). macOS offers its HiDPI ("looks like" half the pixels) modes by the density these give.
    uint32_t fWidthMM, fHeightMM;
    static constexpr uint32_t kColorID = 100, kTimingID = 1000;  // AppleParavirtDisplay's IDs of its first color and timing element

    bool readFramebuffer();
    bool clean_queue_gated();
    void program_hardware_gated();
    void copySurface(IOSurface *, bool refresh = false);
    struct FrameStats;
    void sampleRow(const uint32_t *line, const uint8_t *source, unsigned pixelBytes, uint64_t y, uint64_t width, uint64_t height,
                   FrameStats &stats, bool thumb);
    void logThumbnail(const FrameStats &stats, uint64_t width, uint64_t height, uint32_t format, bool refresh);
    void requestCopy(IOSurface *, bool refresh, bool fenced = false, uint32_t fence = 0);
    void copyLoop();
    static void copyThreadMain(void *, wait_result_t);
    const uint8_t *surfaceBytes(IOSurface *, IOMemoryDescriptor *);
    void releaseSurfaceMaps();
    void stopCopies();
    bool createDisplayAttributes();
    void drawBars();
    static void vblFired(OSObject *, IOTimerEventSource *);

    struct SwapInfo {   // IOMobileFramebufferLegacy::SwapInfoBlob as AppleParavirtDisplay fills it (48 bytes)
        uint32_t swapID;
        uint64_t words[5];
    };
    static_assert(sizeof(SwapInfo) == 48, "IOMobileFramebufferLegacy::SwapInfoBlob");
    static constexpr unsigned kSwapInfoRing = 6;

    PE_Video fVideo;
    uint64_t fVideoPhys;
    IOMemoryMap *fScanoutMap;
    uint8_t *fScanout;
    uint8_t *fHalfToByte;     // 'RGhA' surfaces: the 8-bit value of each half float >> 2 (a channel), clamped to [0, 1]
    uint16_t *fRow;           // one 'RGhA' row
    uint32_t *fLine;          // one converted row, written to the scanout at once (it is write-combined)
    uint64_t fRowBytes;
    uint8_t fXr10ToByte[1024];   // 'b3a8' surfaces: the 8-bit value of each 10-bit extended-range channel, clamped to [0, 1]
    IOTimerEventSource *fVBL;
    DisplayColorRemapMode fColorRemapMode;
    uint32_t fSurfaceFormat;
    bool fDisplayOn;
    bool fRefreshCopies;      // boot-arg native-gop-display-refresh (default 1): copy the surface on screen again after a swap
    uint64_t fFrames;
    uint64_t fLayerShape;             // the layers of the last swap, logged when they change
    // What the last new frame copied to the panel looked like, from every 4th pixel of every 4th row of the copied (BGRA) rows, in any
    // source format: the share of non-black pixels in per mille, the mean colour and the centre pixel (0xRRGGBB).
    struct FrameStats {
        uint64_t samples, lit, raw, red, green, blue;
        uint32_t centre;
    };
    uint64_t fLastLitPerMille, fLastMean, fLastCenter;
    // The thumbnail (boot-arg native-gop-display-thumb=SECONDS, default 30, 0 off): a coarse picture of a frame as copied, logged on the
    // first frame and then every period, so a run without a camera shows what the panel showed. kThumbCols x kThumbRows cells, each the
    // mean luminance of its samples, as one of kThumbRamp's characters.
    static constexpr unsigned kThumbCols = 80, kThumbRows = 27, kSampleStep = 4;
    uint32_t fThumbPeriod;
    uint32_t fThumbLimit, fThumbsTaken;   // boot-arg native-gop-display-thumbs=N: at most N a boot (default 3, 0: no limit)
    uint64_t fThumbNext;              // mach time the next thumbnail is due (0: on the next new frame)
    volatile bool fThumbRequested;    // a refresh copy was asked for to take it (a static screen has no new frames)
    uint32_t *fThumbLuma, *fThumbCount;
    uint64_t fTicks, fDropped, fReported;
    uint64_t fWaitFailures, fWaitReleased;
    uint64_t fCopies, fCopyNs, fCopyMaxNs, fReadNs, fConvertNs, fWriteNs, fPrepareNs;
    uint64_t fSubmitted, fStalled;
    uint64_t fSubmitTime[16], fFenceWaits, fFenceWaitNs, fFenceWaitMaxNs;   // swap submitted -> programmed (IOSurface fences)
    uint64_t fLastFrameTime, fSecondStart, fSecondFrames, fPeakFps;
    uint64_t fCopyRequestTime, fWakeMaxNs, fWakeLate, fVBLDue, fVBLLateMaxNs, fVBLLate;
    // The kernel mappings of the last surfaces copied (WindowServer swaps a few framebuffers in turn).
    struct SurfaceMap {
        IOSurface *surface;   // retained while mapped
        IOMemoryMap *map;
        uint64_t used;
    };
    static constexpr unsigned kSurfaceMaps = 4;
    SurfaceMap fSurfaceMaps[kSurfaceMaps];
    uint64_t fSurfaceMapClock;
    // The copies run on a thread of their own, so the command gate WindowServer swaps through is never held for a copy. A swap completes
    // at once; the thread copies the newest surface asked for, the ones it had no time for are skipped.
    IOLock *fCopyLock;
    IOSurface *fCopyNext;             // retained; the next surface to copy
    bool fCopyRefresh, fCopyStop, fCopyThreadDone;
    uint64_t fCopiesSkipped;
    // The GPU's fence rule (PianoA830, docs/drivers/gpu.md): WindowServer swaps a frame when its command buffer is scheduled, not when
    // the GPU finished it, and the family's IOSurface fences do not see the A830's work. So a swap records the newest fence the GPU
    // had handed out when it was programmed, and its copy first waits until the GPU retired that fence (at most kGpuFenceWaitMs, then
    // it copies anyway). PianoA830's registered service is found by class and asked with callPlatformFunction("native-adreno-fences"),
    // so neither kext links against the other; without it, or with boot-arg native-gop-display-gpu-fence=0, nothing waits.
    static constexpr unsigned kGpuFenceWaitMs = 50;
    bool fGpuFences;
    IOService *fGpu;                  // retained once found (written once, by the swap path)
    const OSSymbol *fGpuFenceSymbol;
    uint64_t fGpuLookupNext;          // mach time of the next lookup while none was found
    uint32_t fCopyFence;              // with fCopyNext: the fence its swap waits for, when fCopyFenced
    bool fCopyFenced;
    uint64_t fGpuWaits, fGpuWaitNs, fGpuWaitMaxNs, fGpuTimeouts;
    bool gpuFence(uint32_t *fence);
    void waitGpuFence(uint32_t fence);
    SwapInfo fSwapInfo[kSwapInfoRing];   // held while kSwapInfoHeld is set, sent in order afterwards
    // The surface on screen, copied again on a few ticks after its swap (1, 2, 4, 8, 16, 30): a renderer that swapped before its work
    // was done (a GPU) is shown complete once it finishes. WindowServer does not draw into the surface on screen.
    static constexpr unsigned kRefreshTicks = 30;
    IOSurface *fShown;
    unsigned fRefreshLeft;
    unsigned fSwapInfoRead, fSwapInfoWrite;
};

OSDefineMetaClassAndStructors(NativeGopDisplay, IOMobileFramebufferLegacy)

static IOPMPowerState gPowerStates[2] = {   // AppleParavirtDisplay's gPowerState
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

// The ADT node under the nub (DroidVMDisplay's AppleParavirtGPU is the provider, its provider the /arm-io node).
static IORegistryEntry *adtNode(IOService *provider)
{
    return provider ? provider->getProvider() : nullptr;
}

static bool adtNumber(IORegistryEntry *node, const char *key, uint64_t *value)
{
    OSData *data = node ? OSDynamicCast(OSData, node->getProperty(key)) : nullptr;
    if (!data || (data->getLength() != 4 && data->getLength() != 8))
        return false;
    *value = data->getLength() == 4 ? *static_cast<const uint32_t *>(data->getBytesNoCopy())
                                    : *static_cast<const uint64_t *>(data->getBytesNoCopy());
    return true;
}

// Whether this display takes the nub: the ADT node's native-gop-display marker (configs/k27-gop-display.toml, only in the ADT made with
// make-piano-adt.py --gop-display) says yes, boot-arg native-gop-display overrides it (0: DroidVMDisplay's display instead, 1: take
// it), and never in the VM.
IOService *NativeGopDisplay::probe(IOService *provider, SInt32 *score)
{
    IORegistryEntry *node = adtNode(provider);
    uint64_t marker = 0;
    unsigned enabled = adtNumber(node, "native-gop-display", &marker) && marker;
    PE_parse_boot_argn("native-gop-display", &enabled, sizeof(enabled));
    IORegistryEntry *virtio = IORegistryEntry::fromPath("/arm-io/virtio-gpu", gIODTPlane);
    const char *refusal = virtio ? "a virtio-gpu is present (the VM): DroidVMDisplay's display takes the nub"
                        : !enabled ? (marker ? "off (boot-arg native-gop-display=0): DroidVMDisplay's display takes the nub"
                                             : "the nub's ADT node is not marked native-gop-display: DroidVMDisplay's display takes the nub")
                        : nullptr;
    OSSafeReleaseNULL(virtio);
    IOLog("NativeGopDisplay: probe on %s (ADT %s): %s\n", provider ? provider->getName() : "?", node ? node->getName() : "?",
          refusal ? refusal : "taking it");
    if (refusal)
        return nullptr;
    return IOMobileFramebufferLegacy::probe(provider, score);
}

// The boot framebuffer: PE_state.video (DarwinBoot's boot_args Video, the GOP). The ADT node may describe it too (framebuffer-*):
// a mismatch is logged, and the ADT's description stands in only when the platform has no console info at all.
bool NativeGopDisplay::readFramebuffer()
{
    IORegistryEntry *node = adtNode(getProvider());
    uint64_t base = 0, width = 0, height = 0, stride = 0, size = 0;
    const bool described = adtNumber(node, "framebuffer-base", &base) && adtNumber(node, "framebuffer-width", &width) &&
                           adtNumber(node, "framebuffer-height", &height) && adtNumber(node, "framebuffer-stride", &stride);
    adtNumber(node, "framebuffer-size", &size);
    memset(&fVideo, 0, sizeof(fVideo));
    if (getPlatform()->getConsoleInfo(&fVideo) == kIOReturnSuccess && fVideo.v_baseAddr) {
        fVideoPhys = fVideo.v_baseAddr & ~3UL;
        IOLog("NativeGopDisplay: boot framebuffer %#llx %lux%lu row %lu depth %lu\n", fVideoPhys, fVideo.v_width, fVideo.v_height,
              fVideo.v_rowBytes, fVideo.v_depth);
        if (described && (base != fVideoPhys || width != fVideo.v_width || height != fVideo.v_height || stride != fVideo.v_rowBytes))
            IOLog("NativeGopDisplay: the ADT describes %#llx %llux%llu row %llu instead; the boot framebuffer stands\n", base, width,
                  height, stride);
    } else if (described) {
        IOLog("NativeGopDisplay: no console info; the ADT's framebuffer %#llx %llux%llu row %llu\n", base, width, height, stride);
        fVideoPhys = base;
        fVideo.v_baseAddr = base;
        fVideo.v_width = width;
        fVideo.v_height = height;
        fVideo.v_rowBytes = stride;
        fVideo.v_depth = 32;
    } else {
        IOLog("NativeGopDisplay: no boot framebuffer (no console info, no ADT framebuffer-* description)\n");
        return false;
    }
    if (fVideo.v_depth != 32 || !fVideo.v_width || !fVideo.v_height || fVideo.v_width > 16384 || fVideo.v_height > 16384 ||
        fVideo.v_rowBytes < fVideo.v_width * 4) {
        IOLog("NativeGopDisplay: unusable framebuffer: depth %lu, %lux%lu row %lu (needs 32-bit linear)\n", fVideo.v_depth,
              fVideo.v_width, fVideo.v_height, fVideo.v_rowBytes);
        return false;
    }
    if (size && fVideo.v_rowBytes * fVideo.v_height > size)
        IOLog("NativeGopDisplay: the framebuffer (%lu bytes) is larger than the ADT's region (%llu bytes)\n",
              fVideo.v_rowBytes * fVideo.v_height, size);
    return true;
}

// Eight vertical bars, white yellow cyan green magenta red blue black (boot-arg native-gop-display-bars=1): the channel order checks.
void NativeGopDisplay::drawBars()
{
    static const uint32_t colors[8] = {0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000};
    for (unsigned long y = 0; y < fVideo.v_height; y++) {
        auto *row = reinterpret_cast<volatile uint32_t *>(fScanout + y * fVideo.v_rowBytes);
        for (unsigned long x = 0; x < fVideo.v_width; x++)
            row[x] = colors[x * 8 / fVideo.v_width];
    }
}

bool NativeGopDisplay::start(IOService *provider)
{
    IORegistryEntry *node = adtNode(provider);
    IOLog("NativeGopDisplay: start on %s (ADT %s)\n", provider->getName(), node ? node->getName() : "?");
    uint64_t number = 0;
    fRefreshHz = adtNumber(node, "refresh-hz", &number) ? static_cast<uint32_t>(number) : 60;
    PE_parse_boot_argn("native-gop-display-hz", &fRefreshHz, sizeof(fRefreshHz));
    if (fRefreshHz < 30 || fRefreshHz > 240)
        fRefreshHz = 60;
    fWidthMM = adtNumber(node, "panel-width-mm", &number) ? static_cast<uint32_t>(number) : 239;
    fHeightMM = adtNumber(node, "panel-height-mm", &number) ? static_cast<uint32_t>(number) : 163;
    char mm[32] = {};
    if (PE_parse_boot_arg_str("native-gop-display-mm", mm, sizeof(mm))) {
        uint32_t w = 0, h = 0;
        const char *c = mm;
        while (*c >= '0' && *c <= '9')
            w = w * 10 + uint32_t(*c++ - '0');
        if (*c == 'x')
            for (++c; *c >= '0' && *c <= '9';)
                h = h * 10 + uint32_t(*c++ - '0');
        if (w >= 50 && w <= 2000 && h >= 50 && h <= 2000) {
            fWidthMM = w;
            fHeightMM = h;
        }
    }
    if (fWidthMM < 50 || fWidthMM > 2000 || fHeightMM < 50 || fHeightMM > 2000) {
        fWidthMM = 239;
        fHeightMM = 163;
    }
    unsigned refresh = 1;
    PE_parse_boot_argn("native-gop-display-refresh", &refresh, sizeof(refresh));
    fRefreshCopies = refresh != 0;
    fThumbPeriod = 30;
    PE_parse_boot_argn("native-gop-display-thumb", &fThumbPeriod, sizeof(fThumbPeriod));
    fThumbLimit = 3;   // the console ring is 7 MiB and a thumbnail is about 3 KB: a few say what the panel showed
    PE_parse_boot_argn("native-gop-display-thumbs", &fThumbLimit, sizeof(fThumbLimit));
    unsigned gpuFence = 1;
    PE_parse_boot_argn("native-gop-display-gpu-fence", &gpuFence, sizeof(gpuFence));
    fGpuFences = gpuFence != 0;
    fGpuFenceSymbol = OSSymbol::withCStringNoCopy("native-adreno-fences");
    if (!readFramebuffer())
        return false;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(fVideoPhys, fVideo.v_rowBytes * fVideo.v_height,
                                                                         kIODirectionInOut);
    if (memory) {
        fScanoutMap = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        memory->release();
    }
    if (!fScanoutMap) {
        IOLog("NativeGopDisplay: cannot map the framebuffer at %#llx\n", fVideoPhys);
        return false;
    }
    fScanout = reinterpret_cast<uint8_t *>(fScanoutMap->getVirtualAddress());
    IOLog("NativeGopDisplay: framebuffer mapped write-combined at %p (%lu bytes)\n", fScanout, fVideo.v_rowBytes * fVideo.v_height);
    // A solid colour until the first frame (or the bars): the screen shows that this mapping reaches the panel. The boot console keeps
    // drawing its text over it until WindowServer's first frame.
    unsigned bars = 0;
    PE_parse_boot_argn("native-gop-display-bars", &bars, sizeof(bars));
    if (bars) {
        drawBars();
    } else {
        for (unsigned long y = 0; y < fVideo.v_height; y++) {
            auto *row = reinterpret_cast<volatile uint32_t *>(fScanout + y * fVideo.v_rowBytes);
            for (unsigned long x = 0; x < fVideo.v_width; x++)
                row[x] = 0xff203050;
        }
    }
    IOLog("NativeGopDisplay: framebuffer %s\n", bars ? "shows the colour bars" : "cleared");
    fCopyLock = IOLockAlloc();
    thread_t copier = nullptr;
    if (!fCopyLock || kernel_thread_start(copyThreadMain, this, &copier) != KERN_SUCCESS) {
        IOLog("NativeGopDisplay: no copy thread\n");
        if (fCopyLock)
            IOLockFree(fCopyLock);
        fCopyLock = nullptr;
        OSSafeReleaseNULL(fScanoutMap);
        return false;
    }
    thread_deallocate(copier);
    IOLog("NativeGopDisplay: copy thread started\n");
    // IOMobileFramebufferLegacy::start waits for IOSurfaceRoot, calls start_hardware, creates the default surface and registers.
    if (!IOMobileFramebufferLegacy::start(provider)) {
        IOLog("NativeGopDisplay: IOMobileFramebufferLegacy::start failed\n");
        stopCopies();
        OSSafeReleaseNULL(fScanoutMap);
        return false;
    }
    IOLog("NativeGopDisplay: started, display %ux%u at %u Hz, %ux%u mm, %s copies, thumbnail %s%u s (at most %u, 0: no limit)\n",
          display_width(), display_height(), fRefreshHz, fWidthMM, fHeightMM, fRefreshCopies ? "with refresh" : "no refresh",
          fThumbPeriod ? "every " : "off: ", fThumbPeriod, fThumbLimit);
    return true;
}

void NativeGopDisplay::stop(IOService *provider)
{
    IOLog("NativeGopDisplay: stop\n");
    IOMobileFramebufferLegacy::stop(provider);
    stopCopies();
    releaseSurfaceMaps();
    if (fHalfToByte)
        IOFree(fHalfToByte, 16384);
    if (fRow)
        IOFree(fRow, fRowBytes);
    if (fLine)
        IOFree(fLine, fVideo.v_width * 4);
    fHalfToByte = nullptr;
    fRow = nullptr;
    fLine = nullptr;
    if (fThumbLuma)
        IOFree(fThumbLuma, kThumbCols * kThumbRows * sizeof(uint32_t));
    if (fThumbCount)
        IOFree(fThumbCount, kThumbCols * kThumbRows * sizeof(uint32_t));
    fThumbLuma = fThumbCount = nullptr;
    if (fShown)
        reinterpret_cast<OSObject *>(fShown)->release();
    fShown = nullptr;
    OSSafeReleaseNULL(fScanoutMap);
    fScanout = nullptr;
    OSSafeReleaseNULL(fGpu);
    OSSafeReleaseNULL(fGpuFenceSymbol);
}

// Called by IOMobileFramebufferLegacy::start before it creates the default surface and registers the service (it tests bit 0).
bool NativeGopDisplay::start_hardware()
{
    if (!createDisplayAttributes())
        return false;
    IOLog("NativeGopDisplay: display attributes published (one %lux%lu@%u timing, 8-bit SDR sRGB)\n", fVideo.v_width, fVideo.v_height,
          fRefreshHz);
    fVBL = IOTimerEventSource::timerEventSource(this, vblFired);
    if (!fVBL || getWorkLoop()->addEventSource(fVBL) != kIOReturnSuccess) {   // the work loop of the family's command gate
        IOLog("NativeGopDisplay: Failed to create VBL timer\n");
        OSSafeReleaseNULL(fVBL);
        return false;
    }
    fVBL->enable();
    at<uint8_t>(this, kHardwareStarted) = 1;
    PMinit();
    getPlatform()->PMRegisterDevice(getProvider(), this);   // the provider is no power-managed AppleARMIODevice here
    registerPowerDriver(this, gPowerStates, 2);
    // The panel is always there: report it plugged in (the digital out state IOMobileFramebufferGetDigitalOutState answers). Unplugged
    // (0), WindowServer kept the display disabled and composited a virtual one as the main display.
    hotPlug_notify(1);
    IOLog("NativeGopDisplay: hardware started (VBL timer %u Hz, power states registered, hot plug 1)\n", fRefreshHz);
    return true;
}

IOReturn NativeGopDisplay::stop_hardware_gated()
{
    if (fVBL) {
        fVBL->cancelTimeout();
        getWorkLoop()->removeEventSource(fVBL);
        OSSafeReleaseNULL(fVBL);
    }
    return kIOReturnSuccess;
}

void NativeGopDisplay::vblFired(OSObject *owner, IOTimerEventSource *)
{
    auto *display = static_cast<NativeGopDisplay *>(owner);
    uint64_t now = mach_absolute_time(), late = 0;
    if (display->fVBLDue && now > display->fVBLDue)
        absolutetime_to_nanoseconds(now - display->fVBLDue, &late);
    if (late > 20000000) {
        display->fVBLLate++;
        if (late > display->fVBLLateMaxNs)
            display->fVBLLateMaxNs = late;
    }
    display->swap_begin_gated();
}

// AppleParavirtDisplay::clean_queue_gated: whether a queued swap is ready to be programmed.
bool NativeGopDisplay::clean_queue_gated()
{
    if (at<IOMFBSwapIORequest *>(this, kCurrentSwap) || !at<IOMFBSwapIORequest *>(this, kSwapQueueFirst))
        return false;
    if (dropObsoleteSwaps_gated())
        return false;
    dropNoOpSwaps_gated();
    dropAbortedSwaps_gated(false);
    dropDummySwaps_gated();
    return at<IOMFBSwapIORequest *>(this, kSwapQueueFirst) != nullptr;
}

// AppleParavirtDisplay::swap_begin_gated: the family calls it when a swap is submitted, the VBL timer every refresh.
IOReturn NativeGopDisplay::swap_begin_gated()
{
    if (!fVBL)
        return kIOReturnNotReady;
    fVBL->cancelTimeout();
    uint64_t frames = fFrames;
    while (clean_queue_gated()) {
        IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kSwapQueueFirst);
        at<IOMFBSwapIORequest *>(this, kCurrentSwap) = swap;
        iomfbUpdateSwapInfoState(this, swap, 2);
        IOMFBSwapIORequest *next = at<IOMFBSwapIORequest *>(swap, kSwapNext);
        IOMFBSwapIORequest **prev = at<IOMFBSwapIORequest **>(swap, kSwapPrev);
        if (next)
            at<IOMFBSwapIORequest **>(next, kSwapPrev) = prev;
        else
            at<IOMFBSwapIORequest **>(this, kSwapQueueLast) = prev;
        *prev = next;
        if ((at<uint32_t>(swap, kSwapDropReasons) & ~8u) == 0) {
            program_hardware_gated();
        } else {
            dropSwap_gated(swap, false);
            fDropped++;
        }
    }
    if (fFrames == frames && fShown && fRefreshLeft && fRefreshCopies) {
        unsigned elapsed = kRefreshTicks - --fRefreshLeft;
        if (!(elapsed & (elapsed - 1)) || !fRefreshLeft)
            requestCopy(fShown, true);
    }
    // A thumbnail that is due while no new frame comes: one refresh copy of the surface on screen takes it.
    if (fThumbPeriod && fThumbNext && fShown && !fThumbRequested && mach_absolute_time() >= fThumbNext) {
        fThumbRequested = true;
        requestCopy(fShown, true);
    }
    if (at<IOMFBSwapIORequest *>(this, kSwapQueueFirst))
        fStalled++;
    if (++fTicks % (10 * fRefreshHz) == 0 && fSubmitted + fFrames + fDropped + fStalled != fReported) {   // about every 10 s while swaps arrive
        fReported = fSubmitted + fFrames + fDropped + fStalled;
        IOLog("NativeGopDisplay: %llu swaps submitted, %llu shown, %llu dropped, %llu ticks stalled (current %p), last frame %llu.%llu%% "
              "non-black mean #%06llx centre #%06llx; %llu copies mean %llu us (prepare %llu read %llu convert %llu write %llu) max %llu us, "
              "%llu waits released\n",
              fSubmitted, fFrames, fDropped, fStalled, at<IOMFBSwapIORequest *>(this, kCurrentSwap), fLastLitPerMille / 10,
              fLastLitPerMille % 10, fLastMean, fLastCenter,
              fCopies, fCopies ? fCopyNs / fCopies / 1000 : 0, fCopies ? fPrepareNs / fCopies / 1000 : 0,
              fCopies ? fReadNs / fCopies / 1000 : 0, fCopies ? fConvertNs / fCopies / 1000 : 0, fCopies ? fWriteNs / fCopies / 1000 : 0,
              fCopyMaxNs / 1000, fWaitReleased);
        if (fCopiesSkipped)
            IOLog("NativeGopDisplay: %llu copies skipped for newer ones\n", fCopiesSkipped);
        IOLog("NativeGopDisplay: at most %llu frames in a second\n", fPeakFps);
        if (fFenceWaits)
            IOLog("NativeGopDisplay: %llu swaps from submit to programmed mean %llu us max %llu us\n", fFenceWaits,
                  fFenceWaitNs / fFenceWaits / 1000, fFenceWaitMaxNs / 1000);
        IOLog("NativeGopDisplay: scheduling: %llu copy wakeups over 20 ms (longest %llu ms), %llu VBL ticks over 20 ms late (latest by %llu ms)\n",
              fWakeLate, fWakeMaxNs / 1000000, fVBLLate, fVBLLateMaxNs / 1000000);
        if (fGpuWaits)
            IOLog("NativeGopDisplay: %llu copies waited for the GPU's fence, mean %llu us max %llu us, %llu gave up after %u ms\n", fGpuWaits,
                  fGpuWaitNs / fGpuWaits / 1000, fGpuWaitMaxNs / 1000, fGpuTimeouts, kGpuFenceWaitMs);
        fGpuWaits = fGpuWaitNs = fGpuWaitMaxNs = fGpuTimeouts = 0;
        fFenceWaits = fFenceWaitNs = fFenceWaitMaxNs = 0;
        fPeakFps = 0;
        fWakeLate = fWakeMaxNs = fVBLLate = fVBLLateMaxNs = 0;
        fCopies = fCopyNs = fCopyMaxNs = fPrepareNs = fReadNs = fConvertNs = fWriteNs = 0;
    }
    uint64_t period;
    nanoseconds_to_absolutetime(1000000000ULL / fRefreshHz, &period);
    fVBLDue = mach_absolute_time() + period;
    return fVBL->setTimeoutUS(1000000 / fRefreshHz);
}

// CoreAnimation asks swap_wait, without blocking, whether its last swaps are done before it composes the next frame. The family
// answers kIOReturnBusy for the last completed swap as long as no later one replaces it (a display controller is still scanning it
// out). Here the swap's surface was copied when it was programmed, so it is free: told it is busy, WindowServer updated at about 10
// frames a second however little it drew (DroidVMDisplay's finding).
IOReturn NativeGopDisplay::swap_wait(IOMobileFramebufferUserClient *client, unsigned int swapID, unsigned int options, unsigned int timeout)
{
    IOReturn result = IOMobileFramebufferLegacy::swap_wait(client, swapID, options, timeout);
    if (result == kIOReturnBusy && (options & 0x7fffffffu) == 0 && swapID == at<uint32_t>(this, kLastCompletedSwap)) {
        fWaitReleased++;
        return kIOReturnSuccess;
    }
    if (result != kIOReturnSuccess && (++fWaitFailures <= 20 || fWaitFailures % 1000 == 0))
        IOLog("NativeGopDisplay: swap_wait(%u, 0x%x, %u) = 0x%x (%llu failures): last %u power %u/%u powered %u started 0x%x current %p queue %p\n",
              swapID, options, timeout, result, fWaitFailures, at<uint32_t>(this, kLastCompletedSwap), at<uint32_t>(this, kSystemPowerState),
              at<uint32_t>(this, kAPIPowerState), at<uint32_t>(this, kHardwarePowered), at<uint8_t>(this, kHardwareStarted),
              at<IOMFBSwapIORequest *>(this, kCurrentSwap), at<IOMFBSwapIORequest *>(this, kSwapQueueFirst));
    return result;
}

// Diagnostic: when each swap arrives. The family holds a swap until its surfaces' fences have passed (swap_apply_fences_gated), so
// submit -> programmed is how long the swap waited for its renderer.
IOReturn NativeGopDisplay::swap_submit(IOMFBSwapRec *rec, IOUserClient *client, unsigned int *out)
{
    if (rec)
        fSubmitTime[reinterpret_cast<const uint32_t *>(rec)[38] % 16] = mach_absolute_time();
    return IOMobileFramebufferLegacy::swap_submit(rec, client, out);
}

// AppleParavirtDisplay::do_power_state_change_gated: the display is on while both the system and WindowServer (setAPIPowerState)
// want it on; each change goes to WindowServer as a power state notification, which CoreAnimation waits for before it swaps. The
// panel is not switched (no DPMS here): off only stops the copies' source.
IOReturn NativeGopDisplay::do_power_state_change_gated()
{
    bool on = at<uint32_t>(this, kSystemPowerState) && at<uint32_t>(this, kAPIPowerState);
    if (on == fDisplayOn)
        return kIOReturnSuccess;
    fDisplayOn = on;
    at<uint32_t>(this, kHardwarePowered) = on;
    IOLog("NativeGopDisplay: display %s\n", on ? "on" : "off");
    powerstate_notify(true, on);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay::program_hardware_gated (its slot 473): show the swap, then complete it. There is no cursor plane (no
// "CursorPlane" property), so WindowServer draws the cursor into the main surface. The screen is the largest surface of the swap.
void NativeGopDisplay::program_hardware_gated()
{
    IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kCurrentSwap);
    uint64_t &submitted = fSubmitTime[at<uint32_t>(swap, kSwapID) % 16];
    if (submitted) {
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - submitted, &waited);
        submitted = 0;
        fFenceWaits++;
        fFenceWaitNs += waited;
        if (waited > fFenceWaitMaxNs)
            fFenceWaitMaxNs = waited;
    }
    uint8_t layers = at<uint8_t>(swap, kSwapLayers);
    IOSurface *surface = nullptr;
    uint64_t area = 0, shape = layers;
    for (unsigned layer = 0; layer < kSwapLayerCount; layer++) {
        IOSurface *candidate = (layers >> layer & 1) ? at<IOSurface *>(swap, kSwapMainSurface + 8 * layer) : nullptr;
        if (!candidate)
            continue;
        uint64_t width = ioSurfaceWidth(candidate), height = ioSurfaceHeight(candidate);
        shape = shape * 31 + width * 65537 + height;
        if (width * height > area) {
            area = width * height;
            surface = candidate;
        }
    }
    if (shape != fLayerShape) {   // each new combination of layers
        fLayerShape = shape;
        IOLog("NativeGopDisplay: swap layers %#x:", layers);
        for (unsigned layer = 0; layer < kSwapLayerCount; layer++) {
            IOSurface *candidate = (layers >> layer & 1) ? at<IOSurface *>(swap, kSwapMainSurface + 8 * layer) : nullptr;
            if (candidate)
                IOLog(" %u: %llux%llu %#x", layer, ioSurfaceWidth(candidate), ioSurfaceHeight(candidate), ioSurfacePixelFormat(candidate));
        }
        IOLog("\n");
    }
    if (surface && area * 4 >= static_cast<uint64_t>(fVideo.v_width) * fVideo.v_height) {   // a quarter of the screen at least
        if (!fFrames++) {   // WindowServer's first frame: the verbose boot console stops drawing over the framebuffer
            PE_initialize_console(nullptr, kPEDisableScreen);
            IOLog("NativeGopDisplay: first frame (swap %u); boot console released from the screen\n", at<uint32_t>(swap, kSwapID));
        }
        uint64_t now = mach_absolute_time(), second = 0;
        fLastFrameTime = now;
        absolutetime_to_nanoseconds(now - fSecondStart, &second);
        if (second >= 1000000000) {
            if (fSecondFrames > fPeakFps)
                fPeakFps = fSecondFrames;
            fSecondStart = now;
            fSecondFrames = 0;
        }
        fSecondFrames++;
        uint32_t fence = 0;
        const bool fenced = gpuFence(&fence);
        requestCopy(surface, false, fenced, fence);
        if (surface != fShown) {
            reinterpret_cast<OSObject *>(surface)->retain();
            if (fShown)
                reinterpret_cast<OSObject *>(fShown)->release();
            fShown = surface;
        }
        fRefreshLeft = kRefreshTicks;
    }
    swap_complete_gated();
}

// 8-bit BGRA, 16-bit half-float RGBA ('RGhA': Core Animation's framebuffer on a virtual device, the ADT's vmm-present), and 'b3a8':
// two planes, 10-bit extended-range BGR in 32 bits and 8-bit alpha, what WindowServer draws with a deep framebuffer (P3 gamut).
static constexpr uint32_t kFormatBGRA = 'BGRA', kFormatRGhA = 'RGhA', kFormatB3A8 = 'b3a8';

// A half float's value in [0, 1] as a byte, in integer arithmetic (no floating point in the kernel).
static uint8_t halfToByte(uint16_t half)
{
    unsigned exponent = (half >> 10) & 31, mantissa = half & 1023;
    if (half & 0x8000 || (exponent == 31 && mantissa))   // negative or NaN
        return 0;
    if (exponent >= 15)                                  // 1.0 and above
        return 255;
    uint32_t scaled = (exponent ? 1024 + mantissa : mantissa) * 255;   // value * 255 = scaled * 2^(exponent - 25), subnormals 2^-24
    unsigned shift = exponent ? 25 - exponent : 24;
    return static_cast<uint8_t>((scaled + (1u << (shift - 1))) >> shift);
}

// The newest fence the GPU handed out (the swap path only: it also looks PianoA830's service up, once a second until found).
bool NativeGopDisplay::gpuFence(uint32_t *fence)
{
    if (!fGpuFences || !fGpuFenceSymbol)
        return false;
    if (!fGpu) {
        const uint64_t now = mach_absolute_time();
        if (now < fGpuLookupNext)
            return false;
        uint64_t second = 0;
        nanoseconds_to_absolutetime(1000000000ull, &second);
        fGpuLookupNext = now + second;
        OSDictionary *match = IOService::serviceMatching("PianoA830");
        IOService *gpu = match ? IOService::copyMatchingService(match) : nullptr;
        if (match)
            match->release();
        if (!gpu)
            return false;
        __atomic_store_n(&fGpu, gpu, __ATOMIC_RELEASE);
        IOLog("NativeGopDisplay: GPU fences from %s: a swap's copy waits for the GPU work handed out before it (at most %u ms)\n",
              gpu->getName(), kGpuFenceWaitMs);
    }
    uint32_t retired = 0;
    return fGpu->callPlatformFunction(fGpuFenceSymbol, false, fence, &retired, nullptr, nullptr) == kIOReturnSuccess;
}

// The copy thread: until the GPU retired fence (32-bit sequence numbers), bounded; a GPU that stops answering is not waited for.
void NativeGopDisplay::waitGpuFence(uint32_t fence)
{
    IOService *gpu = __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE);
    if (!gpu)
        return;
    const uint64_t start = mach_absolute_time();
    uint64_t waited = 0;
    for (;;) {
        uint32_t submitted = 0, retired = 0;
        if (gpu->callPlatformFunction(fGpuFenceSymbol, false, &submitted, &retired, nullptr, nullptr) != kIOReturnSuccess)
            break;
        if (static_cast<int32_t>(retired - fence) >= 0)
            break;
        absolutetime_to_nanoseconds(mach_absolute_time() - start, &waited);
        if (waited >= kGpuFenceWaitMs * 1000000ull) {
            fGpuTimeouts++;
            break;
        }
        IOSleep(1);
    }
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &waited);
    fGpuWaits++;
    fGpuWaitNs += waited;
    if (waited > fGpuWaitMaxNs)
        fGpuWaitMaxNs = waited;
}

void NativeGopDisplay::requestCopy(IOSurface *surface, bool refresh, bool fenced, uint32_t fence)
{
    reinterpret_cast<OSObject *>(surface)->retain();
    IOLockLock(fCopyLock);
    IOSurface *skipped = fCopyNext;
    if (skipped && refresh && !fCopyRefresh) {   // a swap's surface waiting is newer than the one on screen
        IOLockUnlock(fCopyLock);
        reinterpret_cast<OSObject *>(surface)->release();
        return;
    }
    fCopyNext = surface;
    fCopyRefresh = refresh;
    fCopyFenced = fenced;
    fCopyFence = fence;
    if (!skipped)
        fCopyRequestTime = mach_absolute_time();
    IOLockWakeup(fCopyLock, &fCopyNext, true);
    IOLockUnlock(fCopyLock);
    if (skipped) {
        fCopiesSkipped++;
        reinterpret_cast<OSObject *>(skipped)->release();
    }
}

void NativeGopDisplay::copyThreadMain(void *owner, wait_result_t)
{
    static_cast<NativeGopDisplay *>(owner)->copyLoop();
    thread_terminate(current_thread());
}

void NativeGopDisplay::copyLoop()
{
    IOLockLock(fCopyLock);
    while (!fCopyStop) {
        IOSurface *surface = fCopyNext;
        if (!surface) {
            IOLockSleep(fCopyLock, &fCopyNext, THREAD_UNINT);
            continue;
        }
        bool refresh = fCopyRefresh;
        const bool fenced = fCopyFenced;
        const uint32_t fence = fCopyFence;
        fCopyNext = nullptr;
        fCopyFenced = false;
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - fCopyRequestTime, &waited);
        IOLockUnlock(fCopyLock);
        if (waited > 20000000) {
            fWakeLate++;
            if (waited > fWakeMaxNs)
                fWakeMaxNs = waited;
        }
        if (fenced)
            waitGpuFence(fence);
        copySurface(surface, refresh);
        reinterpret_cast<OSObject *>(surface)->release();
        IOLockLock(fCopyLock);
    }
    fCopyThreadDone = true;
    IOLockWakeup(fCopyLock, &fCopyThreadDone, false);
    IOLockUnlock(fCopyLock);
}

// The kernel address of a surface's memory, mapped once for the next copies.
const uint8_t *NativeGopDisplay::surfaceBytes(IOSurface *surface, IOMemoryDescriptor *memory)
{
    SurfaceMap *slot = &fSurfaceMaps[0];
    for (SurfaceMap &entry : fSurfaceMaps) {
        if (entry.surface == surface && entry.map->getMemoryDescriptor() == memory) {
            entry.used = ++fSurfaceMapClock;
            return reinterpret_cast<const uint8_t *>(entry.map->getVirtualAddress());
        }
        if (entry.used < slot->used)
            slot = &entry;
    }
    if (slot->surface) {
        slot->map->release();
        reinterpret_cast<OSObject *>(slot->surface)->release();
        *slot = {};
    }
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    if (!map)
        return nullptr;
    reinterpret_cast<OSObject *>(surface)->retain();
    *slot = {surface, map, ++fSurfaceMapClock};
    return reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
}

void NativeGopDisplay::stopCopies()
{
    if (!fCopyLock)
        return;
    IOLockLock(fCopyLock);
    fCopyStop = true;
    IOLockWakeup(fCopyLock, &fCopyNext, true);
    while (!fCopyThreadDone)
        IOLockSleep(fCopyLock, &fCopyThreadDone, THREAD_UNINT);
    IOSurface *left = fCopyNext;
    fCopyNext = nullptr;
    IOLockUnlock(fCopyLock);
    if (left)
        reinterpret_cast<OSObject *>(left)->release();
    IOLockFree(fCopyLock);
    fCopyLock = nullptr;
}

void NativeGopDisplay::releaseSurfaceMaps()
{
    for (SurfaceMap &entry : fSurfaceMaps) {
        if (!entry.surface)
            continue;
        entry.map->release();
        reinterpret_cast<OSObject *>(entry.surface)->release();
        entry = {};
    }
}

void NativeGopDisplay::copySurface(IOSurface *surface, bool refresh)
{
    uint64_t width = ioSurfaceWidth(surface), height = ioSurfaceHeight(surface), rowBytes = ioSurfaceBytesPerRow(surface);
    uint64_t base = ioSurfaceBaseOffset(surface);
    uint32_t format = ioSurfacePixelFormat(surface);
    IOMemoryDescriptor *memory = ioSurfaceMemory(surface);
    const bool first = !fCopies && !refresh && fSurfaceFormat == 0;
    if (format != fSurfaceFormat) {
        IOLog("NativeGopDisplay: surface %llux%llu row %llu offset %llu format %#x memory %p\n", width, height, rowBytes, base, format, memory);
        for (uint32_t plane = 0, planes = ioSurfacePlaneCount(surface); plane < planes; plane++)
            IOLog("NativeGopDisplay:  plane %u: %llux%llu offset %llu row %llu element %llu\n", plane, ioSurfacePlaneWidth(surface, plane),
                  ioSurfacePlaneHeight(surface, plane), ioSurfacePlaneOffset(surface, plane), ioSurfacePlaneBytesPerRow(surface, plane),
                  ioSurfacePlaneBytesPerElement(surface, plane));
        fSurfaceFormat = format;
    }
    if (format == kFormatB3A8) {   // the colour plane; alpha is not shown
        if (ioSurfacePlaneCount(surface) < 1 || ioSurfacePlaneBytesPerElement(surface, 0) != 4)
            return;
        base = ioSurfacePlaneOffset(surface, 0);
        rowBytes = ioSurfacePlaneBytesPerRow(surface, 0);
    }
    uint64_t pixelBytes = format == kFormatRGhA ? 8 : 4;
    if (!memory || !fScanout || rowBytes < width * pixelBytes || (format != kFormatBGRA && format != kFormatRGhA && format != kFormatB3A8)) {
        static unsigned refused;
        if (refused++ < 8)
            IOLog("NativeGopDisplay: surface format %#x (row %llu for %llu pixels) is not copied\n", format, rowBytes, width);
        return;
    }
    uint64_t copyWidth = width < fVideo.v_width ? width : fVideo.v_width;
    uint64_t copyHeight = height < fVideo.v_height ? height : fVideo.v_height;
    if (!fLine) {
        fRowBytes = fVideo.v_width * 8;
        fRow = static_cast<uint16_t *>(IOMalloc(fRowBytes));
        fLine = static_cast<uint32_t *>(IOMalloc(fVideo.v_width * 4));
        fHalfToByte = static_cast<uint8_t *>(IOMalloc(16384));
        if (!fRow || !fLine || !fHalfToByte) {
            if (fRow)
                IOFree(fRow, fRowBytes);
            if (fLine)
                IOFree(fLine, fVideo.v_width * 4);
            if (fHalfToByte)
                IOFree(fHalfToByte, 16384);
            fRow = nullptr;
            fLine = nullptr;
            fHalfToByte = nullptr;
            IOLog("NativeGopDisplay: no memory for the row buffers\n");
            return;
        }
        // Indexed by the half float >> 2 (16 KiB): 8 bits of mantissa left, finer than a byte's step anywhere in [0, 1].
        for (uint32_t index = 0; index < 16384; index++)
            fHalfToByte[index] = halfToByte(static_cast<uint16_t>(index << 2 | 2));
        for (uint32_t value = 0; value < 1024; value++)   // extended range: 384 is 0.0, 894 is 1.0
            fXr10ToByte[value] = value <= 384 ? 0 : value >= 894 ? 255 : static_cast<uint8_t>((value - 384) * 255 / 510);
    }
    uint64_t began = mach_absolute_time(), mark = began, now;
    auto lap = [&](uint64_t &total) {
        now = mach_absolute_time();
        uint64_t ns;
        absolutetime_to_nanoseconds(now - mark, &ns);
        total += ns;
        mark = now;
    };
    if (memory->prepare() != kIOReturnSuccess)
        return;
    const uint8_t *bytes = surfaceBytes(surface, memory);
    lap(fPrepareNs);
    if (!bytes) {
        memory->complete();
        IOLog("NativeGopDisplay: cannot map a %llux%llu surface\n", width, height);
        return;
    }
    // Each new frame's statistics, and the thumbnail when one is due: the first new frame, then once every fThumbPeriod s, on a new
    // frame or on the refresh copy swap_begin_gated asked for. Both come from every kSampleStep-th pixel of every kSampleStep-th row of
    // the rows as written to the panel (after conversion), so they describe what the panel shows, in any source format. The sampling
    // time is counted as conversion.
    FrameStats stats = {};
    bool thumb = fThumbPeriod && (fThumbNext ? mach_absolute_time() >= fThumbNext : !refresh);
    if (thumb && !fThumbLuma) {
        fThumbLuma = static_cast<uint32_t *>(IOMalloc(kThumbCols * kThumbRows * sizeof(uint32_t)));
        fThumbCount = static_cast<uint32_t *>(IOMalloc(kThumbCols * kThumbRows * sizeof(uint32_t)));
        if (!fThumbLuma || !fThumbCount) {
            if (fThumbLuma)
                IOFree(fThumbLuma, kThumbCols * kThumbRows * sizeof(uint32_t));
            if (fThumbCount)
                IOFree(fThumbCount, kThumbCols * kThumbRows * sizeof(uint32_t));
            fThumbLuma = fThumbCount = nullptr;
            fThumbPeriod = 0;
            IOLog("NativeGopDisplay: no memory for the thumbnail; thumbnails off\n");
            thumb = false;
        }
    }
    if (thumb) {
        bzero(fThumbLuma, kThumbCols * kThumbRows * sizeof(uint32_t));
        bzero(fThumbCount, kThumbCols * kThumbRows * sizeof(uint32_t));
    }
    const bool sample = !refresh || thumb;
    for (uint64_t y = 0; y < copyHeight; y++) {
        uint8_t *line = fScanout + y * fVideo.v_rowBytes;
        const uint8_t *source = bytes + base + y * rowBytes;
        const bool sampled = sample && y % kSampleStep == 0;
        if (format == kFormatBGRA) {
            memcpy(fLine, source, copyWidth * 4);
            lap(fReadNs);
            if (sampled) {
                sampleRow(fLine, nullptr, 4, y, copyWidth, copyHeight, stats, thumb);
                lap(fConvertNs);
            }
            memcpy(line, fLine, copyWidth * 4);
            lap(fWriteNs);
            continue;
        }
        if (format == kFormatB3A8) {   // blue in the low bits, as BGR10A2
            memcpy(fLine, source, copyWidth * 4);
            lap(fReadNs);
            for (uint64_t x = 0; x < copyWidth; x++) {
                uint32_t bgr = fLine[x];
                fLine[x] = 0xff000000u | fXr10ToByte[bgr >> 20 & 1023] << 16 | fXr10ToByte[bgr >> 10 & 1023] << 8 | fXr10ToByte[bgr & 1023];
            }
            if (sampled)   // converted in place: the nonzero count is of the converted pixels
                sampleRow(fLine, nullptr, 4, y, copyWidth, copyHeight, stats, thumb);
            lap(fConvertNs);
            memcpy(line, fLine, copyWidth * 4);
            lap(fWriteNs);
            continue;
        }
        memcpy(fRow, source, copyWidth * 8);
        lap(fReadNs);
        const uint64_t *pixels = reinterpret_cast<const uint64_t *>(fRow);
        for (uint64_t x = 0; x < copyWidth; x++) {
            uint64_t rgba = pixels[x];
            fLine[x] = 0xff000000u | fHalfToByte[rgba >> 2 & 0x3fff] << 16 | fHalfToByte[rgba >> 18 & 0x3fff] << 8 |
                       fHalfToByte[rgba >> 34 & 0x3fff];
        }
        if (sampled)
            sampleRow(fLine, reinterpret_cast<const uint8_t *>(fRow), 8, y, copyWidth, copyHeight, stats, thumb);
        lap(fConvertNs);
        memcpy(line, fLine, copyWidth * 4);
        lap(fWriteNs);
    }
    memory->complete();
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    fCopies++;
    fCopyNs += ns;
    if (ns > fCopyMaxNs)
        fCopyMaxNs = ns;
    if (first)
        IOLog("NativeGopDisplay: first copy %llux%llu format %#x into the framebuffer in %llu us\n", copyWidth, copyHeight, format, ns / 1000);
    if (thumb) {
        logThumbnail(stats, copyWidth, copyHeight, format, refresh);
        uint64_t period;
        nanoseconds_to_absolutetime(static_cast<uint64_t>(fThumbPeriod) * 1000000000ULL, &period);
        fThumbNext = mach_absolute_time() + period;
        fThumbRequested = false;
        if (fThumbLimit && ++fThumbsTaken >= fThumbLimit) {
            fThumbPeriod = 0;
            IOLog("NativeGopDisplay: %u thumbnails taken, no more this boot (native-gop-display-thumbs)\n", fThumbsTaken);
        }
    }
    if (refresh)
        return;
    const uint64_t samples = stats.samples ? stats.samples : 1;
    fLastLitPerMille = stats.lit * 1000 / samples;
    fLastMean = (stats.red / samples) << 16 | (stats.green / samples) << 8 | stats.blue / samples;
    fLastCenter = stats.centre;
    if (format != kFormatBGRA && (fFrames <= 8 || fFrames % 50 == 0))
        IOLog("NativeGopDisplay: frame %llu: %llu of %llu sampled pixels non-black, %llu nonzero in the source, centre #%06x\n", fFrames,
              stats.lit, stats.samples, stats.raw, stats.centre);
}

// One sampled row (every kSampleStep-th pixel of LINE, 0xAARRGGBB as written to the panel) into STATS, and into the thumbnail's cells.
// SOURCE, when given, is the row before conversion (PIXELBYTES a pixel), for the count of nonzero source pixels; else LINE's own.
void NativeGopDisplay::sampleRow(const uint32_t *line, const uint8_t *source, unsigned pixelBytes, uint64_t y, uint64_t width,
                                 uint64_t height, FrameStats &stats, bool thumb)
{
    const uint64_t centreY = height / 2 / kSampleStep * kSampleStep, centreX = width / 2 / kSampleStep * kSampleStep;
    const uint64_t cellRow = y * kThumbRows / height;
    for (uint64_t x = 0; x < width; x += kSampleStep) {
        const uint32_t pixel = line[x];
        const uint32_t red = pixel >> 16 & 255, green = pixel >> 8 & 255, blue = pixel & 255;
        stats.samples++;
        stats.red += red;
        stats.green += green;
        stats.blue += blue;
        const uint32_t brightest = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
        stats.lit += brightest > 8;   // "non-black": any channel above 8 of 255
        if (source)
            stats.raw += pixelBytes == 8 ? *reinterpret_cast<const uint64_t *>(source + x * 8) != 0
                                         : *reinterpret_cast<const uint32_t *>(source + x * 4) != 0;
        else
            stats.raw += (pixel & 0xffffff) != 0;
        if (y == centreY && x == centreX)
            stats.centre = pixel & 0xffffff;
        if (thumb) {
            const uint64_t cell = cellRow * kThumbCols + x * kThumbCols / width;
            fThumbLuma[cell] += (54 * red + 183 * green + 19 * blue) >> 8;   // Rec. 709 luma weights, sum 256
            fThumbCount[cell]++;
        }
    }
}

// The thumbnail: a header with what the frame is, then kThumbRows lines of kThumbCols characters, each cell's mean luma (0..255) as
// one of " .:-=+*#%@" (dark to bright). The panel's cursor is not in it: only the main layer is copied.
void NativeGopDisplay::logThumbnail(const FrameStats &stats, uint64_t width, uint64_t height, uint32_t format, bool refresh)
{
    static const char kRamp[] = " .:-=+*#%@";
    const uint64_t samples = stats.samples ? stats.samples : 1, perMille = stats.lit * 1000 / samples;
    const uint64_t mean = (stats.red / samples) << 16 | (stats.green / samples) << 8 | stats.blue / samples;
    const char code[5] = {static_cast<char>(format >> 24), static_cast<char>(format >> 16), static_cast<char>(format >> 8),
                          static_cast<char>(format), 0};
    IOLog("NativeGopDisplay: thumb frame %llu%s: %llux%llu '%s', %llu.%llu%% non-black, mean #%06llx, centre #%06x, %ux%u cells of "
          "%llux%llu px, ramp \" .:-=+*#%%@\"\n", fFrames, refresh ? " (screen unchanged)" : "", width, height, code, perMille / 10,
          perMille % 10, mean, stats.centre, kThumbCols, kThumbRows, width / kThumbCols, height / kThumbRows);
    char text[kThumbCols + 1];
    for (unsigned row = 0; row < kThumbRows; row++) {
        for (unsigned column = 0; column < kThumbCols; column++) {
            const unsigned cell = row * kThumbCols + column;
            const uint32_t luma = fThumbCount[cell] ? fThumbLuma[cell] / fThumbCount[cell] : 0;
            text[column] = kRamp[luma * 10 / 256];
        }
        text[kThumbCols] = 0;
        IOLog("NativeGopDisplay: thumb %02u |%s|\n", row, text);
    }
}

static bool appendNumber(OSDictionary *dictionary, const char *key, uint64_t value, unsigned bits = 32)
{
    OSNumber *number = OSNumber::withNumber(value, bits);
    bool done = number && dictionary->setObject(key, number);
    OSSafeReleaseNULL(number);
    return done;
}

static bool appendString(OSDictionary *dictionary, const char *key, const char *value)
{
    OSString *string = OSString::withCString(value);
    bool done = string && dictionary->setObject(key, string);
    OSSafeReleaseNULL(string);
    return done;
}

static bool appendDictionary(OSDictionary *dictionary, const char *key, OSDictionary *value)
{
    bool done = value && dictionary->setObject(key, value);
    OSSafeReleaseNULL(value);
    return done;
}

static OSDictionary *point(uint32_t x, uint32_t y)
{
    OSDictionary *dictionary = OSDictionary::withCapacity(3);
    if (dictionary && !(appendNumber(dictionary, "X", x) && appendNumber(dictionary, "Y", y)))
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}

// AppleParavirtDisplay::createDisplayAttributes, with the display of the boot framebuffer: one timing (the GOP mode) with one
// 8-bit SDR color element. Chromaticities are sRGB's (16.16 fixed point, as a panel's EDID would give them).
bool NativeGopDisplay::createDisplayAttributes()
{
    uint32_t width = static_cast<uint32_t>(fVideo.v_width), height = static_cast<uint32_t>(fVideo.v_height);
    bool done = false;
    OSDictionary *attributes = OSDictionary::withCapacity(16), *product = OSDictionary::withCapacity(8);
    OSDictionary *chromaticity = OSDictionary::withCapacity(3), *whitePoint = point(20493, 21561);
    OSDictionary *transport = OSDictionary::withCapacity(2), *sourceLimits = OSDictionary::withCapacity(5);
    OSDictionary *scalingLimits = OSDictionary::withCapacity(2);
    OSArray *colors = OSArray::withCapacity(1), *timings = OSArray::withCapacity(1), *timingColors = OSArray::withCapacity(1);
    OSObject *color = ioavColorWithCapabilities(2, 8, 0, 0, 0, 16, false, nullptr, nullptr, nullptr, nullptr);
    // CVT reduced blanking as IOAVFamily's sCVTTimingData has it. Sync rates are 16.16 fixed point; the horizontal one is left 0.
    uint8_t timingData[80] = {};
    uint32_t hTotal = width + 160, vTotal = height + 23;
    at<uint32_t>(timingData, 4) = hTotal;
    at<uint32_t>(timingData, 8) = width;
    at<uint32_t>(timingData, 12) = 32;                   // horizontal sync width, back porch, front porch
    at<uint32_t>(timingData, 16) = 80;
    at<uint32_t>(timingData, 20) = 48;
    at<uint32_t>(timingData, 28) = 1;                    // horizontal sync polarity (positive)
    at<uint32_t>(timingData, 36) = vTotal;
    at<uint32_t>(timingData, 40) = height;
    at<uint32_t>(timingData, 44) = 6;                    // vertical sync width, back porch, front porch
    at<uint32_t>(timingData, 48) = 14;
    at<uint32_t>(timingData, 52) = 3;
    at<uint32_t>(timingData, 56) = fRefreshHz << 16;
    at<uint32_t>(timingData, 68) = hTotal * vTotal * fRefreshHz;   // pixel clock in Hz
    OSObject *timing = ioavTimingWithData(timingData, 2, 4);
    do {
        if (!attributes || !product || !chromaticity || !whitePoint || !transport || !sourceLimits || !scalingLimits || !colors ||
            !timings || !timingColors || !color || !timing)
            break;
        at<uint64_t>(color, kElementID) = kColorID;
        at<uint32_t>(color, kElementScore) = kColorID;
        at<uint64_t>(timing, kElementID) = kTimingID;
        at<uint32_t>(timing, kElementScore) = kTimingID;
        if (!colors->setObject(color) || !timingColors->setObject(color) || !timings->setObject(timing))
            break;
        ioavTimingSetColorElements(timing, timingColors);
        // The serial number is the size: WindowServer keeps the chosen mode per display identity.
        if (!(appendString(product, "ManufacturerID", "XMI") && appendNumber(product, "ProductID", 0x8750) &&
              appendNumber(product, "SerialNumber", width << 16 | height) && appendString(product, "ProductName", "Xiaomi Pad 8 Pro") &&
              appendNumber(product, "WeekOfManufacture", 1) && appendNumber(product, "YearOfManufacture", 2025)))
            break;
        if (!(appendDictionary(chromaticity, "Red", point(41943, 21627)) && appendDictionary(chromaticity, "Green", point(19661, 39322)) &&
              appendDictionary(chromaticity, "Blue", point(9830, 3932))))
            break;
        if (!appendNumber(whitePoint, "Gamma", 144179))   // 2.2
            break;
        if (!(attributes->setObject("SupportsPQEOTF", kOSBooleanFalse) && attributes->setObject("ProductAttributes", product) &&
              attributes->setObject("Chromaticity", chromaticity) && attributes->setObject("DefaultColorSpaceIsSRGB", kOSBooleanFalse) &&
              attributes->setObject("DefaultWhitePoint", whitePoint) && attributes->setObject("SupportsStandby", kOSBooleanTrue) &&
              appendNumber(attributes, "ContinuousFrequencySupport", 3) &&
              appendNumber(attributes, "MaxHorizontalImageSize", (fWidthMM + 5) / 10) &&
              appendNumber(attributes, "MaxVerticalImageSize", (fHeightMM + 5) / 10) &&
              appendNumber(attributes, "NativeFormatHorizontalPixels", width) && appendNumber(attributes, "NativeFormatVerticalPixels", height) &&
              appendNumber(attributes, "PreciseAspectRatio", (static_cast<uint64_t>(width) << 16) / height)))
            break;
        if (!(appendString(transport, "Upstream", "Other") && appendString(transport, "Downstream", "Other")))
            break;
        if (!(appendNumber(sourceLimits, "MaxSrcBufferWidth", 16384) && appendNumber(sourceLimits, "MaxSrcBufferHeight", 16384) &&
              appendNumber(sourceLimits, "MaxSrcRectWidth", 8192) && appendNumber(sourceLimits, "MaxSrcRectTotal", 0) &&
              appendNumber(sourceLimits, "IOMFBMaxCompressedSizeInBytes", 0)))
            break;
        if (!(appendNumber(scalingLimits, "RGBLayer_MinScaleFraction", 4) && appendNumber(scalingLimits, "RGBLayer_MaxScale", 4)))
            break;
        setProperty("IOMFBUUID", "4E474F50-4449-5350-0000-000000008750");
        // As DroidVMDisplay publishes its display (external, display index 0); proven with macOS 27's WindowServer in the VM.
        setProperty("external", true);
        setProperty("primary", 0ULL, 32);
        done = setProperty("DisplayAttributes", attributes) && setProperty("Transport", transport) &&
            setProperty("ColorElements", colors) && setProperty("TimingElements", timings) &&
            setProperty("IOMFBMaxSrcPixels", sourceLimits) && setProperty("IOMFBScalingLimits", scalingLimits);
    } while (false);
    OSSafeReleaseNULL(attributes);
    OSSafeReleaseNULL(product);
    OSSafeReleaseNULL(chromaticity);
    OSSafeReleaseNULL(whitePoint);
    OSSafeReleaseNULL(transport);
    OSSafeReleaseNULL(sourceLimits);
    OSSafeReleaseNULL(scalingLimits);
    OSSafeReleaseNULL(colors);
    OSSafeReleaseNULL(timings);
    OSSafeReleaseNULL(timingColors);
    OSSafeReleaseNULL(color);
    OSSafeReleaseNULL(timing);
    if (!done || set_digital_out_mode(kColorID, kTimingID) != kIOReturnSuccess) {
        IOLog("NativeGopDisplay: createDisplayAttributes failed (%s)\n", done ? "set_digital_out_mode" : "an element or a property");
        return false;
    }
    return true;
}

// The mode of a color and a timing element (the IDs published above); AppleParavirtDisplay's block takes the timing's active size.
IOReturn NativeGopDisplay::set_digital_out_mode(unsigned int colorID, unsigned int timingID)
{
    if (colorID != kColorID || timingID != kTimingID)
        return kIOReturnBadArgument;
    at<uint32_t>(this, kDisplayWidth) = static_cast<uint32_t>(fVideo.v_width);
    at<uint32_t>(this, kDisplayHeight) = static_cast<uint32_t>(fVideo.v_height);
    setProperty("DisplayWidth", fVideo.v_width, 32);
    setProperty("DisplayHeight", fVideo.v_height, 32);
    setProperty("DPTimingModeId", timingID, 32);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay: dots per inch from the physical width (IOMobileFramebufferLegacy's reads "dot-pitch" of the AppleARMIODevice,
// which this provider is not), in integer arithmetic.
IOReturn NativeGopDisplay::get_dot_pitch(unsigned int *pitch)
{
    *pitch = static_cast<unsigned int>((fVideo.v_width * 2546 + fWidthMM * 50) / (fWidthMM * 100));
    return kIOReturnSuccess;
}

// AppleParavirtDisplay's hooks with nothing to do on a CPU-copied scanout: the swap and its layers need no mapping, there is no
// gamma or color hardware (the family's defaults are unsupported, AppleParavirtDisplay accepts them).
IOReturn NativeGopDisplay::verify_swap(IOMFBSwapIORequest *)   // AppleParavirtDisplay returns 1 (IOMobileFramebufferLegacy: 0)
{
    if (!fSubmitted++)
        IOLog("NativeGopDisplay: first swap submitted\n");
    return 1;
}
IOReturn NativeGopDisplay::swap_layer_map(IOMFBSwapIORequest *, unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int, IOSurface *) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::set_display_device(unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::get_gamma_table(IOMFBGammaTable *) {return kIOReturnUnsupported;}
IOReturn NativeGopDisplay::set_gamma_table(IOMFBGammaTable *) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::set_color_remap_mode(DisplayColorRemapMode mode)
{
    fColorRemapMode = mode;
    return kIOReturnSuccess;
}
IOReturn NativeGopDisplay::get_color_remap_mode(DisplayColorRemapMode *mode) const
{
    *mode = fColorRemapMode;
    return kIOReturnSuccess;
}
IOReturn NativeGopDisplay::swap_set_color_matrix(IOMFBColorFixedMatrix *, IOMFBColorMatrixFunction, unsigned int) {return kIOReturnSuccess;}

// The slots IOMobileFramebufferLegacy leaves pure, as AppleParavirtDisplay implements them.
IOReturn NativeGopDisplay::set_notify_tag_mask_gated(IOMFB_NotificationType type, IOMFBNotifyRequest *request, unsigned int mask)
{
    if (type != 3)
        return kIOReturnBadArgument;
    at<uint32_t>(request, kNotifyMask) = mask;
    return kIOReturnSuccess;
}
IOReturn NativeGopDisplay::supported_frame_info(unsigned int, unsigned int *, char (*)[64], unsigned int *) {return kIOReturnUnsupported;}
IOReturn NativeGopDisplay::factory_portal(void *, task *) {return kIOReturnUnsupported;}
IOReturn NativeGopDisplay::executeFactoryConfigurations_gated() {return kIOReturnUnsupported;}
IOReturn NativeGopDisplay::set_frame_info_notification_state_gated(IOMFBNotifyRequest *, bool) {return kIOReturnSuccess;}

// AppleParavirtDisplay::notify_swap_done_gated: the family calls it as the current swap completes; the swap info goes to the
// clients (swap_info_notify_dispatch_gated, which calls do_swap_info_notify_gated per request) unless notifications are held.
IOReturn NativeGopDisplay::notify_swap_done_gated(unsigned int swapID, ::SwapInfoBlob *, unsigned int *, unsigned int)
{
    IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kCurrentSwap);
    if (at<uint32_t>(swap, kSwapDropReasons))
        return kIOReturnUnsupported;
    SwapInfo *info = &fSwapInfo[fSwapInfoWrite];
    *info = {swapID, {at<uint64_t>(this, kSwapInfoTime), 0, at<uint64_t>(swap, kSwapInfoA), at<uint32_t>(swap, kSwapInfoB), 0}};
    if (at<uint16_t>(this, kSwapInfoHeld)) {
        fSwapInfoWrite = (fSwapInfoWrite + 1) % kSwapInfoRing;
        return kIOReturnSuccess;
    }
    for (; fSwapInfoRead != fSwapInfoWrite; fSwapInfoRead = (fSwapInfoRead + 1) % kSwapInfoRing)
        iomfbSwapInfoDispatch(this, &fSwapInfo[fSwapInfoRead]);
    iomfbSwapInfoDispatch(this, info);
    return kIOReturnSuccess;
}

// AppleParavirtDisplay::do_swap_info_notify_gated: the request's async result is the mask, the swap ID and the words it asks for.
IOReturn NativeGopDisplay::do_swap_info_notify_gated(IOMFBNotifyRequest *request, const SwapInfoBlob *blob)
{
    const SwapInfo *info = reinterpret_cast<const SwapInfo *>(blob);
    uint32_t mask = at<uint32_t>(request, kNotifyMask);
    io_user_reference_t args[12] = {mask, info->swapID};
    unsigned count = 2;
    for (unsigned word = 0; word < 5; word++) {
        if (!(mask >> word & 1))
            continue;
        uint64_t value = word == 1 ? info->words[0] - at<uint64_t>(request, kNotifyLastTime) : info->words[word];
        unsigned need = mask >> 31 ? 2 : 1;
        if (count + need > 12) {
            IOLog("IOMFB Packer put: ran out of space\n");
            continue;
        }
        if (need == 2) {
            args[count++] = static_cast<uint32_t>(value);
            args[count++] = value >> 32;
        } else {
            args[count++] = value;
        }
    }
    at<uint64_t>(request, kNotifyLastTime) = info->words[0];
    return userClientSendAsyncResult(&at<io_user_reference_t>(request, kNotifyAsyncReference), kIOReturnSuccess, args, count,
                                     kIOUserNotifyOptionCanDrop);
}
IOReturn NativeGopDisplay::notify_crc_ready_gated(unsigned int) {return kIOReturnSuccess;}
bool NativeGopDisplay::are_timings_enabled() const {return false;}

// UniXNU-piano (2026-11-11): the display's nub, here so the native collection needs no DroidVMDisplay. IOMobileFramebuffer.framework
// counts the DisplayPortCount of the AppleParavirtGPU services (it matches the class name) when an /arm-io node is compatible
// "paravirtualizedgraphics,gpu" (make-piano-adt.py's /arm-io/gop-display). The same nub as DroidVMDisplay's.
class AppleParavirtGPU : public IOService {
    OSDeclareDefaultStructors(AppleParavirtGPU)
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(AppleParavirtGPU, IOService)

bool AppleParavirtGPU::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    setProperty("DisplayPortCount", 1ULL, 32);
    registerService();
    IOLog("NativeGopDisplay: AppleParavirtGPU nub on %s, 1 display port\n", provider->getName());
    return true;
}
