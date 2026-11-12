// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeGopDisplay: macOS 27's display on piano's panel without a display controller driver. An IOMobileFramebufferLegacy
// (IOMobileGraphicsFamily) whose scanout is the linear BGRA framebuffer the firmware left running: ABL's splash region, which
// UEFI's SimpleFbDxe exposes as the GOP and DarwinBoot hands to XNU as the boot console (boot_args Video, PE_state.video). The DPU
// pipeline ABL started keeps scanning that memory at the panel's refresh; this driver only writes it. No display register is touched.
//
// The CPU copy of each swap's main IOSurface into the boot framebuffer, with the family hooks AppleParavirtDisplay (vma2) overrides
// (provenance: UPSTREAM). There is no virtio-gpu scanout and no wait for a virtual GPU's 3D work; the defaults are the panel's.
//
// Since 2026-11-12 the copy is a composition (GopCompose.h, host-tested; docs/drivers/native-gop-display.md §5a): layer 0 (the frame)
// with layer 1 (WindowServer's cursor, 'BGRA' premultiplied) blended over it at the layer's origin, and a cursor-only swap redraws only
// the cursor's old and new rectangles. A swap that takes a surface off the screen while the copy thread still reads it completes only
// once that read is done (the family hands the surface back to WindowServer at the completion, and WindowServer draws into it at once:
// the flicker and white bands of 2026-11-11). Flip mode copies while a cursor is on the panel (no cursor pipe yet).
//
// Discovery: IOMobileFramebuffer.framework (iomfb_populate_all_display_infos) counts the DisplayPortCount of the AppleParavirtGPU
// services when an /arm-io node is compatible "paravirtualizedgraphics,gpu", and waits for that many framebuffers. The native ADT has
// such a node (tooling/adt/make-piano-adt.py, configs/k27-gop-display.toml) and this kext provides the nub, AppleParavirtGPU (end of
// this file), so no other display kext is linked natively. probe() takes the nub only on a node marked native-gop-display (that
// opt-in ADT's own node), not with boot-arg native-gop-display=0, and not when an /arm-io/virtio-gpu exists (a virtual machine's
// ADT, where another display driver owns the nub); the refusal strings below still name the VM-era display kext of that case. One
// display is published either way, unless PianoDp's D6 probe is armed (piano-dp=1 with piano-dp-fake=S or piano-dp-fake-boot=1,
// package/PianoDp/DpFake.h, docs/drivers/displayport.md D6): then the nub reports two display ports and a second instance of this
// class, matched by the personality NativeGopDisplayExternal, is a test display backed by a RAM surface, disconnected until its plug
// schedule connects it, that touches no hardware. Without those arguments its probe refuses silently and nothing of it runs.
//
// Every step logs "NativeGopDisplay: ..." (IOLog), so the pstore console mailbox of a headless boot tells how far it got; the second
// display's lines say "NativeGopDisplay-ext: ...".
#include "IOMobileFramebufferABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/device_tree.h>
#include "../PianoDpu/PianoDpuFlipClient.h"   // the zero-copy flip's request (native-gop-display-flip=1)
#include "../PianoDp/DpFake.h"                 // D6: the second, RAM-backed test display (piano-dp-fake)
#include "GopCompose.h"                        // the composition and the swap pipeline (portable, tests/gop_host.cpp)

using gopcompose::Job;
using gopcompose::Rect;

extern "C" task_t kernel_task;

// Every line of an instance names it: "NativeGopDisplay: " (the panel), "NativeGopDisplay-ext: " (D6's second display).
#define GOPLOG(fmt, ...) IOLog("NativeGopDisplay%s: " fmt, logTag(), ##__VA_ARGS__)

// macOS 27.0.1's (26A434, the k27 collection's) offsets, each read from AppleParavirtDisplay's code (provenance: UPSTREAM).
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
static constexpr unsigned kSwapLayerCount = gopcompose::kLayers;
// Layer 1's origin on the panel, two unaligned s32 (added 2026-11-12, read the same way from AppleParavirtDisplay's code, in the
// k27 collection kc27.bin with the k27 tree's scripts/kc-disasm.py): AppleParavirtDisplay::program_hardware_gated(bool)
// (0xfffffe000869d4f8) tests bit 1 of +984, then loads ldur w2, [x8, #97] and ldur w3, [x8, #101] (+0x28, +0x2c) and calls
// program_hardware_cursor_gated(IOSurface *, int, int) with layer 1's surface (+2952) and the two words; that function adds the
// surface's hot spot (IOSurface::getImageOriginAndExtents, words 4 and 5, checked to lie inside the glyph rectangle of words 0-3) to
// them and hands the sums to program_hardware_cursor_position_gated(bool, unsigned short, unsigned short), the host cursor's hot spot
// position. So the words are where the surface's pixel (0, 0) goes on the panel. A null layer-1 surface there hides the cursor.
static constexpr size_t kSwapLayer1X = 97;
static constexpr size_t kSwapLayer1Y = 101;
// The swap's dirty region (added 2026-11-12, read the same way from the family's own code in kc27.bin):
// IOMobileFramebufferLegacy::swap_submit(IOMFBSwapRec *, ...) copies 16 bytes of the client's record (+664) to the request's +417
// (str q0, [x9, #417]) and the low bit of rec +716 to +552; IOMobileFramebufferLegacy::getCurrentDirtyRegion_gated() (0xfffffe00094c8fec)
// returns current swap + 417 when +552 bit 0 is set. The 16 bytes are an IOMFBRect (x, y, width, height; GopCompose.h dirtyRegion).
// The layers' source and destination rectangles beside it: +17 + 16 i and +81 + 16 i (layer 1's destination is +97, the cursor's origin).
static constexpr size_t kSwapDirtyRegion = 417;
static constexpr size_t kSwapDirtyValid = 552;
static constexpr size_t kSwapLayerSourceRect = 17;
static constexpr size_t kSwapLayerDestRect = 81;
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
// A cursor surface's glyph rectangle (x, y, width, height) and hot spot (x, y) as six words, returned through x8: AppleParavirtDisplay
// zeroes 24 bytes and reads them so (program_hardware_cursor_gated +0xb8..+0x110). GopCompose.h's glyphRect checks them.
struct IOSurfaceOriginAndExtents {
    uint32_t words[6];
};
IOSurfaceOriginAndExtents ioSurfaceOriginAndExtents(IOSurface *) __asm("__ZN9IOSurface24getImageOriginAndExtentsEv");
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
    struct FrameStats;
    void sampleRow(const uint32_t *line, const uint8_t *source, unsigned pixelBytes, uint64_t y, uint64_t width, uint64_t height,
                   FrameStats &stats, bool thumb);
    void logThumbnail(const FrameStats &stats, uint64_t width, uint64_t height, uint32_t format, bool refresh);
    void copyLoop();
    static void copyThreadMain(void *, wait_result_t);
    const uint8_t *surfaceBytes(IOSurface *, IOMemoryDescriptor *, bool *cached);
    void releaseSurfaceMaps();
    void stopCopies();
    // The composition (GopCompose.h): the jobs the swaps and the refresh ticks hand the copy thread, what it draws.
    struct Mapped {                   // a surface opened for one job
        IOMemoryDescriptor *memory;   // prepared while its map is kept (SurfaceMap)
        bool cached;
    };
    bool openSource(IOSurface *, gopcompose::RowSource *, Mapped *, bool base);
    bool ensureBuffers();
    void runJob(const Job &, bool retired, bool covered, bool *flipped);
    bool overlayOf(IOSurface *, int32_t x, int32_t y, gopcompose::Overlay *);
    bool compose(const gopcompose::Plan &, const Job &, IOSurface *base, IOSurface *cursor, const gopcompose::Overlay &, const Rect &onPanel);
    void auditDamage(const gopcompose::Plan &, const gopcompose::RowSource &base, bool cached, const Rect &cursorNow);
    void queueRefresh(const Job &);
    static void retainJob(const Job &);
    static void releaseJob(const Job &);
    void noteLayers(const gopcompose::SwapLayers &, uint32_t swapID);
    static void jobDoneAction(OSObject *, IOInterruptEventSource *, int);
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
    gopcompose::Tables *fTables;   // the 'RGhA' and 'b3a8' conversions, made with the row buffers
    uint8_t *fRow;            // one source row ('RGhA': 8 bytes a pixel)
    uint32_t *fLine;          // one composed row, written to the scanout at once (it is write-combined)
    uint32_t *fOverlayRow;    // the cursor's part of a row
    uint64_t fRowBytes;
    IOTimerEventSource *fVBL;
    DisplayColorRemapMode fColorRemapMode;
    uint32_t fSurfaceFormat;
    bool fDisplayOn;
    uint64_t fFrames;
    // Each new combination of layers, and each new kind of layer (its index, size and format, or off), logged once.
    struct LayerKind {
        uint32_t layer, width, height, format;
        bool off;
    };
    static constexpr unsigned kLayerKinds = 32;
    LayerKind fKinds[kLayerKinds];
    unsigned fKindCount;
    uint32_t fMasksSeen;
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
    // The family's queue after each swap_begin_gated (a VBL tick, a submission, a deferred completion; GopCompose.h queueTick):
    // fStalled ("ticks stalled"), a swap queued while none is current, which nothing in progress will program (0 expected, as in every
    // log before 2026-11-12); fBehind (per period), a swap queued behind one that waits for its copy (native-gop-display-defer's
    // back-pressure: WindowServer swapping faster than the copies go). Until 2026-11-12 one counter was both: with the deferred
    // completions it climbed in every animation and looked like a stall (queue6's images, 114-400 a boot).
    uint64_t fSubmitted, fStalled, fBehind;
    uint64_t fSubmitTime[16], fFenceWaits, fFenceWaitNs, fFenceWaitMaxNs;   // swap submitted -> programmed (IOSurface fences)
    uint64_t fLastFrameTime, fSecondStart, fSecondFrames, fPeakFps;
    uint64_t fCopyRequestTime, fWakeMaxNs, fWakeLate, fVBLDue, fVBLLateMaxNs, fVBLLate;
    // The kernel mappings of the last surfaces copied (WindowServer swaps a few framebuffers in turn). Since 2026-11-12 a surface's
    // memory is prepared once, when it is mapped, and completed when its map is dropped (it was prepared and completed around every
    // job: the ~300 us "prepare" of each copy, which would be most of a small damage copy's cost).
    struct SurfaceMap {
        IOSurface *surface;   // retained while mapped
        IOMemoryMap *map;
        uint64_t used;
    };
    static void dropSurfaceMap(SurfaceMap &);
    static constexpr unsigned kSurfaceMaps = 8;   // the frames and the cursor's images
    SurfaceMap fSurfaceMaps[kSurfaceMaps];
    uint64_t fSurfaceMapClock;
    // The copies run on a thread of their own, so the command gate WindowServer swaps through is never held for a copy. The thread
    // draws the newest state asked for (GopCompose.h's slot: the jobs it had no time for are merged into the next). A swap completes
    // at once, unless the copy thread is reading a surface the swap takes off the screen (native-gop-display-defer, default 1): then
    // once that read is done, from the work loop (fJobDone). Flip mode: once the swap's own job is done.
    IOLock *fCopyLock;
    gopcompose::Slot fSlot;           // fCopyLock: the waiting job (its base and overlay retained)
    gopcompose::Busy fBusy;           // fCopyLock: the job the thread draws
    uint64_t fDoneSerial;             // fCopyLock: the last job done
    uint64_t fWaitSerial;             // fCopyLock: the job the waiting swap needs done (0: none waits)
    bool fCopyStop, fCopyThreadDone;
    uint64_t fCopiesSkipped;
    gopcompose::Pipeline fPipe;       // the work loop's: the layers (their surfaces retained), the refresh countdown
    gopcompose::PanelState fPanel;    // the copy thread's: what the panel shows
    uint32_t fDefer;                  // gopcompose::DeferMode
    uint32_t fRefreshPolicy;          // gopcompose::RefreshPolicy (native-gop-display-refresh)
    uint32_t fCursorMode;             // native-gop-display-cursor: 0 layer 1 not drawn, 1 its glyph rectangle, 2 the whole surface
    IOInterruptEventSource *fJobDone; // the copy thread -> the work loop: the job a swap waits for is done
    IOMFBSwapIORequest *fWaitingSwap; // the current swap while it waits
    volatile bool fLastFlipped;       // the job the swap waited for was flipped (flip mode)
    bool fFrameCovered;               // copy thread: the last frame's copy waited for its fence and saw it retired
    bool fNeedFenced;                 // copy thread: the fence the panel's state needs (the newest swap job's), when it has one
    uint32_t fNeedFence;
    volatile bool fUnfinished;        // copy thread -> work loop: a swap's job was left undrawn (its fence had not passed): retry
    volatile uint64_t fAbortSerial;   // work loop -> copy thread: the refresh with this serial stops at its next row
    uint64_t fUnfinishedJobs, fRetries, fRefreshAborts;
    uint64_t fDeferred, fDeferNs, fDeferMaxNs, fDeferStart;
    uint64_t fCursorJobs, fCursorNs, fCursorMaxNs, fCursorForced;
    int32_t fCursorX, fCursorY;       // copy thread: where the cursor was drawn last (its layer origin)
    uint32_t fCursorLogs;
    uint32_t fGlyphW, fGlyphH;        // copy thread: the cursor surface's size last logged
    Rect fGlyphLogged;
    // The GPU's fence rule (PianoA830, docs/drivers/gpu.md): WindowServer swaps a frame when its command buffer is scheduled, not when
    // the GPU finished it, and the family's IOSurface fences do not see the A830's work. So a swap records the newest fence the GPU
    // had handed out when it was programmed, and its copy first waits until the GPU retired that fence (at most fGpuFenceWaitMs, then
    // it copies anyway). PianoA830's registered service is found by class and asked with callPlatformFunction("native-adreno-fences"),
    // so neither kext links against the other; without it, or with boot-arg native-gop-display-gpu-fence=0, nothing waits.
    // Since 2026-11-12 a frame the GPU has not finished is never copied (50 ms before, then copied anyway: the login's white bands):
    // native-gop-display-gpu-fence-ms (default 1000, 10..10000) is only the hang guard; a job whose wait hits it is left undrawn (the
    // previous frame stays), its swap completes, and the ticks retry until the GPU retired the fence (GopCompose.h verdict).
    uint32_t fGpuFenceWaitMs;
    bool fGpuFences;
    IOService *fGpu;                  // retained once found (written once, by the swap path)
    const OSSymbol *fGpuFenceSymbol;
    uint64_t fGpuLookupNext;          // mach time of the next lookup while none was found
    uint64_t fGpuWaits, fGpuWaitNs, fGpuWaitMaxNs, fGpuTimeouts;
    // The display power for other drivers (docs/policy.md 5.6): every power change call publishes "piano-display-power" (boolean),
    // and a change of it is messaged to the interested clients (kIOMessageServicePropertyChange, the property's name), always.
    // The backlight callout (boot-arg piano-backlight=2 only; without it nothing more runs): each call is then also handed to
    // PianoBacklight (package/PianoBacklight/PianoBacklightClient.h), found by class like the GPU.
    bool fBacklightCallout;
    IOService *fBacklight;            // retained once found
    bool fPowerPublished, fPowerPublishedOn;   // what "piano-display-power" was last messaged as
    // The VBL tick while the display is off (2026-11-12, docs/power.md "Idle wakeups by driver"): it stops once nothing is queued,
    // waiting, refreshed or retried (a submitted swap calls swap_begin_gated anyway, the display's power-on starts it again), instead of
    // 60 wakeups a second for nothing. native-gop-display-vbl=0: the tick runs always, as before.
    bool fVblDarkStop, fVblStopped;
    uint32_t fVblStops;
    // native-gop-display-vbl=2 (the default with damage copies, 2026-11-12): the tick also stops while the display is on and nothing
    // moves (GopCompose.h vblIdleStops); a submitted swap, a deferred completion and the power-on call swap_begin_gated anyway.
    bool fVblIdleStop, fVblIdle;
    uint64_t fIdleStops, fTickRuns, fReportNext, fReportStart;
    // Damage copies (native-gop-display-damage, default 1, 2026-11-12; docs/drivers/native-gop-display.md §5c): a new frame whose
    // swap carries a dirty region (kSwapDirtyRegion) and whose GPU work was fence-covered recomposes only that region and the cursor's
    // rectangles, not 27 MB. After each such frame kAuditPoints panel pixels outside the region are compared with the new frame; one
    // that differs turns damage off for the boot and composes the whole panel at once. The whole panel is composed again at least every
    // native-gop-display-damage-full seconds (default 10, 0 never) while frames come. 0: every frame whole, the tick as before
    // (native-gop-display-vbl=1): the behaviour before 2026-11-12's damage copies.
    uint32_t fDamageMode, fDamageFullS;
    bool fDamageAuditOff;
    uint32_t fAuditState, fDirtyLogs;
    uint64_t fLastFullCompose;
    uint64_t fDirtyFrames, fPeriodFrames, fDirtyFramesAll;   // new frames whose swap named a region / new frames (this period), named (boot)
    uint64_t fDamageCopies, fDamageNs, fDamageMaxNs, fDamagePixels, fDamageNothing, fDamageRefreshes;
    uint64_t fWholeNoRegion, fWholeUncovered, fWholePeriodic, fWholeOther;
    uint64_t fAuditSamples, fAuditMismatches;
    bool fAuditFailedNow;                     // copy thread: the job's audit found a pixel the region did not name
    gopcompose::SwapSequence fSequence;       // the swaps programmed, by number: a gap makes the next frame whole
    uint64_t fSwapGaps, fShapeChanges;
    uint64_t fLastShape;                      // the last frame's surface: width, height, format, row bytes (another one: whole)
    int32_t fAuditX, fAuditY;
    uint32_t fAuditShown, fAuditWanted, fAuditBad, fAuditCount;
    void publishDisplayPower(bool on);
    void backlightCallout(bool on);
    // Zero-copy flips (boot-arg native-gop-display-flip=1, with PianoDpu's piano-dpu=2 piano-dpu-flip=4; docs/drivers/dpu.md §4.1;
    // without the argument nothing of this runs): the copy thread hands a swap's surface, after the GPU fence wait, to PianoDpu's
    // "PianoDpuFlip" (package/PianoDpu/PianoDpuFlipClient.h), found by class like the GPU; refused, it copies the surface as before
    // and asks for the boot framebuffer back. The swap completes only then (native-gop-display-defer=2: on the work loop, fJobDone):
    // the surface it replaces is free once the new one latched. A flipped swap needs no refresh copies (the DPU reads the surface every
    // frame), and the VBL timer runs only while refresh copies or a stalled queue need it. The DPU has no cursor pipe yet: while a
    // cursor is on the panel every frame is composed into the boot framebuffer (and that put back), flips resume once it hides.
    bool fFlipMode;
    // native-gop-display-builtin=1 (default 0): the panel published as a built-in display ("external" false, as a Mac's internal
    // panel), so System Settings and CGDisplayIsBuiltin call it built-in. Off by default: WindowServer treats a built-in display
    // differently (CoreBrightness's backlight service, docs/drivers/backlight.md section 5), not yet tried on the tablet.
    bool fBuiltin;
    IOService *fDpu;                  // retained once found
    const OSSymbol *fDpuFlipSymbol;
    uint64_t fDpuLookupNext;          // mach time of the next lookup while none was found
    uint64_t fFlips, fFlipCopies;
    bool dpuFlip(IOSurface *surface, PianoDpuFlipRequest *request);
    // The PianoDpu callouts (docs/drivers/dpu.md §4.3-4.5), each off without its boot argument (and then nothing of this runs):
    //   piano-dpu-pcc=1|2       swap_set_color_matrix / set_gamma_table: logged (their raw bytes, B1d-0) and handed to PianoDpu's
    //                           PCC (PianoDpuColorMatrix / PianoDpuGammaTable); 2 also lets PianoDpu read a whole 0xc0c-byte table
    //   piano-dpu-panel=1       each display power change handed to PianoDpu (PianoDpuPanel: the panel's light sleep)
    //   piano-dpu-refresh=auto  each frame shown handed to PianoDpu (PianoDpuSwap: its 60/120 Hz policy)
    // The service is found by class like the backlight's (no link dependency), retained once found, in fields of their own.
    static constexpr unsigned kColorFunctions = 16, kGammaWords = 771;
    uint32_t fDpuPcc;
    bool fDpuPanel, fDpuSwaps;
    IOService *fDpuSlices;            // retained once found (the flip's fDpu is the copy thread's)
    const OSSymbol *fDpuSwapSymbol;
    uint64_t fDpuSlicesLookupNext;
    uint32_t fColorCalls, fColorChanges, fColorSeen, fGammaCalls, fGammaChanges, fGammaHash;
    uint32_t fColorFunction[kColorFunctions];
    uint64_t fColorLast[kColorFunctions][9];
    IOService *dpuService(bool now);
    // The copy's cost (boot-arg native-gop-display-copy=BITS, default 0: nothing of this runs). A copy is ~20-39 ms for 3200x2136
    // BGRA, most of it reading the surface (uncached).
    //   bit 0: (since 2026-11-12 the default, native-gop-display-refresh=1) a frame whose copy waited for the GPU's fence and saw it
    //          retired needs no refresh copies but the last (tick 30); with native-gop-display-refresh=2 it brings that back
    //   bit 1: surfaces mapped cacheable, the rows each job reads cleaned and invalidated by VA (DC CIVAC) after the fence wait and
    //          before the read, so the read streams through the cache instead of single uncached loads
    static constexpr uint32_t kCopyFencedRefresh = 1, kCopyCachedRead = 2;
    uint32_t fCopyMode;
    bool fCachedMapRefused;
    uint64_t fRefreshSkipped, fInvalidateNs, fInvalidates;
    bool gpuFence(uint32_t *fence);
    bool waitGpuFence(uint32_t fence, bool *answered);
    bool gpuRetired(uint32_t fence);
    SwapInfo fSwapInfo[kSwapInfoRing];   // held while kSwapInfoHeld is set, sent in order afterwards
    // The frame on screen (fPipe's layer 0) is composed again on a few ticks after its swap (1, 2, 4, 8, 16, 30): a renderer that
    // swapped before its work was done (a GPU) is shown complete once it finishes. Only while the family keeps it on screen (it does
    // not hand a surface on a layer back to WindowServer); a frame whose fence wait saw the GPU done needs only the last.
    unsigned fSwapInfoRead, fSwapInfoWrite;
    // D6 (docs/drivers/displayport.md, package/PianoDp/DpFake.h): the second instance, matched by the personality whose
    // NativeGopDisplayRole is "piano-dp-fake". It has no framebuffer: a RAM surface stands in (its copies and thumbnails go there,
    // nothing scans it out), none of the panel's callouts run (backlight, PianoDpu), and it starts disconnected until the plug
    // schedule connects it (connected from the start with piano-dp-fake-boot=1). Its VBL timer stops while it is disconnected.
    bool fExternal;                   // this instance is D6's second display
    bool fPlugged;                    // its hot-plug state as last notified
    pianodp::FakeConfig fFake;
    uint32_t fPlugStep;               // the next step of the schedule
    IOBufferMemoryDescriptor *fRamSurface;
    IOTimerEventSource *fPlugTimer;
    const char *logTag() const {return fExternal ? "-ext" : "";}
    bool setupRamSurface();
    static void plugFired(OSObject *, IOTimerEventSource *);
};

OSDefineMetaClassAndStructors(NativeGopDisplay, IOMobileFramebufferLegacy)

static IOPMPowerState gPowerStates[2] = {   // AppleParavirtDisplay's gPowerState
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

// The ADT node under the nub (the AppleParavirtGPU nub is the provider, its provider the /arm-io node).
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

// D6's boot arguments as one configuration (package/PianoDp/DpFake.h): armed only with piano-dp >= 1 and piano-dp-fake=S or
// piano-dp-fake-boot=1.
static pianodp::FakeConfig fakeFromBootArgs()
{
    pianodp::FakeArgs a;
    PE_parse_boot_argn("piano-dp", &a.dp, sizeof(a.dp));
    PE_parse_boot_argn("piano-dp-fake", &a.plug, sizeof(a.plug));
    PE_parse_boot_argn("piano-dp-fake-boot", &a.boot, sizeof(a.boot));
    PE_parse_boot_argn("piano-dp-fake-unplug", &a.unplug, sizeof(a.unplug));
    PE_parse_boot_argn("piano-dp-fake-cycles", &a.cycles, sizeof(a.cycles));
    PE_parse_boot_argn("piano-dp-fake-primary", &a.primary, sizeof(a.primary));
    char mode[32] = {}, mm[32] = {};
    if (PE_parse_boot_arg_str("piano-dp-fake-mode", mode, sizeof(mode)))
        a.mode = mode;
    if (PE_parse_boot_arg_str("piano-dp-fake-mm", mm, sizeof(mm)))
        a.mm = mm;
    return pianodp::fakeConfig(a);
}

// Whether this display takes the nub: the ADT node's native-gop-display marker (configs/k27-gop-display.toml, only in the ADT made with
// make-piano-adt.py --gop-display) says yes, boot-arg native-gop-display overrides it (0: leave the nub to another display driver,
// 1: take it), and never when an /arm-io/virtio-gpu exists.
IOService *NativeGopDisplay::probe(IOService *provider, SInt32 *score)
{
    // D6's personality (NativeGopDisplayExternal) is probed on every boot: silent and refused without its arguments.
    OSString *role = OSDynamicCast(OSString, getProperty("NativeGopDisplayRole"));
    fExternal = role && role->isEqualTo("piano-dp-fake");
    if (fExternal) {
        fFake = fakeFromBootArgs();
        if (!fFake.armed)
            return nullptr;
    }
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
    GOPLOG("probe on %s (ADT %s): %s\n", provider ? provider->getName() : "?", node ? node->getName() : "?",
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
        GOPLOG("boot framebuffer %#llx %lux%lu row %lu depth %lu\n", fVideoPhys, fVideo.v_width, fVideo.v_height,
              fVideo.v_rowBytes, fVideo.v_depth);
        if (described && (base != fVideoPhys || width != fVideo.v_width || height != fVideo.v_height || stride != fVideo.v_rowBytes))
            GOPLOG("the ADT describes %#llx %llux%llu row %llu instead; the boot framebuffer stands\n", base, width,
                  height, stride);
    } else if (described) {
        GOPLOG("no console info; the ADT's framebuffer %#llx %llux%llu row %llu\n", base, width, height, stride);
        fVideoPhys = base;
        fVideo.v_baseAddr = base;
        fVideo.v_width = width;
        fVideo.v_height = height;
        fVideo.v_rowBytes = stride;
        fVideo.v_depth = 32;
    } else {
        GOPLOG("no boot framebuffer (no console info, no ADT framebuffer-* description)\n");
        return false;
    }
    if (fVideo.v_depth != 32 || !fVideo.v_width || !fVideo.v_height || fVideo.v_width > 16384 || fVideo.v_height > 16384 ||
        fVideo.v_rowBytes < fVideo.v_width * 4) {
        GOPLOG("unusable framebuffer: depth %lu, %lux%lu row %lu (needs 32-bit linear)\n", fVideo.v_depth,
              fVideo.v_width, fVideo.v_height, fVideo.v_rowBytes);
        return false;
    }
    if (size && fVideo.v_rowBytes * fVideo.v_height > size)
        GOPLOG("the framebuffer (%lu bytes) is larger than the ADT's region (%llu bytes)\n",
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
    GOPLOG("start on %s (ADT %s)\n", provider->getName(), node ? node->getName() : "?");
    uint64_t number = 0;
    fRefreshHz = adtNumber(node, "refresh-hz", &number) ? static_cast<uint32_t>(number) : 60;
    PE_parse_boot_argn("native-gop-display-hz", &fRefreshHz, sizeof(fRefreshHz));
    if (fRefreshHz < 30 || fRefreshHz > 240)
        fRefreshHz = 60;
    uint32_t builtin = 0;
    PE_parse_boot_argn("native-gop-display-builtin", &builtin, sizeof(builtin));
    fBuiltin = builtin == 1 && !fExternal;   // D6's second display stays external
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
    // native-gop-display-refresh: 0 none, 1 (default) all six after a frame no fence covered and only the last after one it did,
    // 2 all six after every frame (the default before 2026-11-12; native-gop-display-copy bit 0 brings it back to 1).
    fRefreshPolicy = gopcompose::kRefreshUncovered;
    PE_parse_boot_argn("native-gop-display-refresh", &fRefreshPolicy, sizeof(fRefreshPolicy));
    if (fRefreshPolicy > gopcompose::kRefreshAlways)
        fRefreshPolicy = gopcompose::kRefreshUncovered;
    // native-gop-display-defer: 0 a swap completes at once (before 2026-11-12), 1 at once unless the copy thread reads a surface it
    // takes off the screen, 2 (default) once its own job is done (flip mode's rule): a surface goes back to WindowServer only after
    // the copy of the frame that replaced it.
    fDefer = gopcompose::kDeferAlways;
    PE_parse_boot_argn("native-gop-display-defer", &fDefer, sizeof(fDefer));
    if (fDefer > gopcompose::kDeferAlways)
        fDefer = gopcompose::kDeferAlways;
    // native-gop-display-cursor: 0 layer 1 (the cursor) not drawn (before 2026-11-12), 1 (default) its glyph rectangle blended at the
    // layer's origin, 2 the whole cursor surface (should IOSurface's extents ever be wrong).
    fCursorMode = 1;
    PE_parse_boot_argn("native-gop-display-cursor", &fCursorMode, sizeof(fCursorMode));
    fGpuFenceWaitMs = 1000;
    PE_parse_boot_argn("native-gop-display-gpu-fence-ms", &fGpuFenceWaitMs, sizeof(fGpuFenceWaitMs));
    if (fGpuFenceWaitMs < 10 || fGpuFenceWaitMs > 10000)
        fGpuFenceWaitMs = 1000;
    fThumbPeriod = 30;
    PE_parse_boot_argn("native-gop-display-thumb", &fThumbPeriod, sizeof(fThumbPeriod));
    fThumbLimit = 3;   // the console ring is 7 MiB and a thumbnail is about 3 KB: a few say what the panel showed
    PE_parse_boot_argn("native-gop-display-thumbs", &fThumbLimit, sizeof(fThumbLimit));
    unsigned gpuFence = 1;
    PE_parse_boot_argn("native-gop-display-gpu-fence", &gpuFence, sizeof(gpuFence));
    fGpuFences = gpuFence != 0;
    fGpuFenceSymbol = OSSymbol::withCStringNoCopy("native-adreno-fences");
    PE_parse_boot_argn("native-gop-display-copy", &fCopyMode, sizeof(fCopyMode));
    if (fExternal) {   // D6: its own timing and size; nothing scans the RAM surface, so no refresh copies; none of the panel's callouts
        fRefreshHz = fFake.hz;
        fWidthMM = fFake.widthMm;
        fHeightMM = fFake.heightMm;
        fRefreshPolicy = gopcompose::kRefreshOff;
        GOPLOG("D6 (piano-dp-fake): a second display %ux%u@%u, %ux%u mm, primary %u, %s; RAM-backed, no hardware touched%s%s\n",
               fFake.width, fFake.height, fFake.hz, fFake.widthMm, fFake.heightMm, fFake.primary,
               fFake.atBoot ? "connected from its start" : "disconnected until its plug timer", *fFake.note ? "; " : "", fFake.note);
        // Other drivers find the panel's display by class (PianoBacklight, PianoKeyboard, PianoPolicy, PianoSensors, PianoTouch);
        // with two instances that lookup may return this one. It publishes no display power, and D6's images run none of them.
        GOPLOG("D6: drivers that look NativeGopDisplay up by class may find this instance: test it without them\n");
    }
    // The display power's capability for its readers (docs/policy.md 5.6), before the service registers: each change of
    // piano-display-power is messaged, so PianoPolicy (and any other reader) needs no polling. D6's instance publishes neither.
    if (!fExternal)
        setProperty("piano-display-power-messages", true);
    // Damage copies (2026-11-12, §5c): native-gop-display-damage=0 brings back every frame copied whole and, unless
    // native-gop-display-vbl says otherwise, the 60 Hz tick while the display is on.
    fDamageMode = 1;
    PE_parse_boot_argn("native-gop-display-damage", &fDamageMode, sizeof(fDamageMode));
    if (fDamageMode > 1)
        fDamageMode = 1;
    if (fExternal)   // D6's RAM surface: nothing scans it, nothing to save
        fDamageMode = 0;
    fPipe.damage = fDamageMode != 0;
    fDamageFullS = 10;
    PE_parse_boot_argn("native-gop-display-damage-full", &fDamageFullS, sizeof(fDamageFullS));
    if (fDamageFullS > 3600)
        fDamageFullS = 10;
    fAuditState = 0x9e3779b9u;
    unsigned vbl = fDamageMode ? 2 : 1;
    PE_parse_boot_argn("native-gop-display-vbl", &vbl, sizeof(vbl));
    fVblDarkStop = vbl != 0 && !fExternal;
    fVblIdleStop = vbl >= 2 && !fExternal;
    unsigned backlight = 0;
    PE_parse_boot_argn("piano-backlight", &backlight, sizeof(backlight));
    fBacklightCallout = backlight >= 2 && !fExternal;
    if (!fExternal)
        PE_parse_boot_argn("piano-dpu-pcc", &fDpuPcc, sizeof(fDpuPcc));
    unsigned dpuPanel = 0;
    PE_parse_boot_argn("piano-dpu-panel", &dpuPanel, sizeof(dpuPanel));
    fDpuPanel = dpuPanel != 0 && !fExternal;
    char dpuRefresh[16] = {};
    fDpuSwaps = !fExternal && PE_parse_boot_arg_str("piano-dpu-refresh", dpuRefresh, sizeof(dpuRefresh)) && !strcmp(dpuRefresh, "auto");
    if (fDpuPcc || fDpuPanel || fDpuSwaps) {
        fDpuSwapSymbol = OSSymbol::withCStringNoCopy("PianoDpuSwap");
        GOPLOG("PianoDpu callouts:%s%s%s\n", fDpuPcc ? " colour matrices and gamma tables (piano-dpu-pcc)" : "",
              fDpuPanel ? " display power (piano-dpu-panel)" : "", fDpuSwaps ? " frames shown (piano-dpu-refresh=auto)" : "");
    }
    if ((fCopyMode & kCopyFencedRefresh) && fRefreshPolicy == gopcompose::kRefreshAlways)
        fRefreshPolicy = gopcompose::kRefreshUncovered;
    if (fCopyMode)
        GOPLOG("copy mode %#x:%s%s (native-gop-display-copy)\n", fCopyMode,
              fCopyMode & kCopyFencedRefresh ? " refresh copies only when no GPU fence covered the frame (and the last one)" : "",
              fCopyMode & kCopyCachedRead ? " cached surface reads with DC CIVAC first" : "");
    // Flip mode is on by default where PianoDpu serves flips (piano-dpu=2 with piano-dpu-flip=4 or more, 2026-11-12): the ladder's
    // last step needs no argument of this driver; native-gop-display-flip=0 keeps the copy, =1 asks PianoDpu whatever its level.
    unsigned dpuLevel = 0, dpuFlip = 1;
    PE_parse_boot_argn("piano-dpu", &dpuLevel, sizeof(dpuLevel));
    PE_parse_boot_argn("piano-dpu-flip", &dpuFlip, sizeof(dpuFlip));
    unsigned flip = dpuLevel >= 2 && dpuFlip >= 4 ? 1 : 0;
    PE_parse_boot_argn("native-gop-display-flip", &flip, sizeof(flip));
    fFlipMode = flip != 0 && !fExternal;
    if (fFlipMode) {
        fDpuFlipSymbol = OSSymbol::withCString(kPianoDpuFlipFunction);
        fFlipMode = fDpuFlipSymbol != nullptr;
        fDefer = gopcompose::kDeferAlways;
        GOPLOG("flip mode (native-gop-display-flip): each swap's surface to PianoDpu's flip (piano-dpu=2 "
              "piano-dpu-flip=4), copied when refused or while a cursor is on the panel; a swap completes once on the panel\n");
    }
    static const char *const deferNames[3] = {"at once (native-gop-display-defer=0)",
                                              "at once unless the copy thread reads a surface it takes off the screen",
                                              "once its frame or cursor is on the panel"};
    static const char *const refreshNames[3] = {"none", "after frames no GPU fence covered (the last only after covered ones)",
                                                "after every frame"};
    GOPLOG("composition: layer 0 the frame, layer 1 the cursor %s; a swap completes %s; refresh copies %s; a frame is copied only "
           "once the GPU retired its fence (hang guard %u ms)\n",
           fCursorMode == 0 ? "not drawn (native-gop-display-cursor=0)" : fCursorMode == 2 ? "blended, its whole surface" :
                                                                                            "blended (its glyph rectangle, premultiplied alpha)",
           deferNames[fDefer], refreshNames[fRefreshPolicy], fGpuFenceWaitMs);
    if (fDamageMode)
        GOPLOG("damage: a fence-covered frame whose swap names its dirty region recomposes only that region and the cursor's (its "
               "refreshes what was drawn since the last whole one); %u panel pixels outside it are checked after each, one that "
               "differs turns damage off for the boot; the whole panel again every %u s while frames come%s; the VBL tick %s "
               "(native-gop-display-damage=0: every frame whole, as before)\n", gopcompose::kAuditPoints, fDamageFullS,
               fDamageFullS ? "" : " (0: never)", fVblIdleStop ? "stops while nothing moves" : "runs while the display is on");
    else if (!fExternal)
        GOPLOG("damage copies off (native-gop-display-damage=0): every frame composed whole; the VBL tick %s\n",
               fVblIdleStop ? "stops while nothing moves (native-gop-display-vbl=2)" : "runs while the display is on");
    if (fExternal) {
        if (!setupRamSurface()) {
            provider->setProperty("DisplayPortCount", 1ULL, 32);   // WindowServer then waits for the panel alone
            GOPLOG("not published: the nub reports 1 display port again\n");
            return false;
        }
    } else {
        if (!readFramebuffer())
            return false;
        IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(fVideoPhys, fVideo.v_rowBytes * fVideo.v_height,
                                                                             kIODirectionInOut);
        if (memory) {
            fScanoutMap = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
            memory->release();
        }
        if (!fScanoutMap) {
            GOPLOG("cannot map the framebuffer at %#llx\n", fVideoPhys);
            return false;
        }
        fScanout = reinterpret_cast<uint8_t *>(fScanoutMap->getVirtualAddress());
        GOPLOG("framebuffer mapped write-combined at %p (%lu bytes)\n", fScanout, fVideo.v_rowBytes * fVideo.v_height);
    }
    // The panel until WindowServer's first frame (GopCompose.h startPlan): a verbose boot's text console keeps the framebuffer as it
    // drew it (it repaints only what changes, so a colour under it stays around the text, crossed by lines: the bluish background of
    // 2026-11-12), a graphics boot's is cleared to black; native-gop-display-fill=0xRRGGBB paints a colour, -bars=1 the colour bars.
    // The boot console stops drawing at WindowServer's first frame.
    unsigned bars = 0;
    PE_parse_boot_argn("native-gop-display-bars", &bars, sizeof(bars));
    uint32_t fillRgb = 0;
    const bool fillGiven = PE_parse_boot_argn("native-gop-display-fill", &fillRgb, sizeof(fillRgb));
    int verbose = 0;
    uint32_t vcText = 0;
    PE_parse_boot_argn("-v", &verbose, sizeof(verbose));
    PE_parse_boot_argn("vc-text", &vcText, sizeof(vcText));
    const bool textConsole = !fExternal && gopcompose::textConsole(fVideo.v_display, verbose != 0, vcText);
    const gopcompose::StartPlan plan = gopcompose::startPlan(bars != 0, fillGiven, fillRgb, textConsole);
    if (plan.kind == gopcompose::StartFill::Bars) {
        drawBars();
    } else if (plan.kind != gopcompose::StartFill::Keep) {
        for (unsigned long y = 0; y < fVideo.v_height; y++) {
            auto *row = reinterpret_cast<volatile uint32_t *>(fScanout + y * fVideo.v_rowBytes);
            for (unsigned long x = 0; x < fVideo.v_width; x++)
                row[x] = plan.pixel;
        }
    }
    switch (plan.kind) {
    case gopcompose::StartFill::Bars:
        GOPLOG("framebuffer shows the colour bars\n");
        break;
    case gopcompose::StartFill::Colour:
        GOPLOG("framebuffer filled with #%06x (native-gop-display-fill)%s\n", plan.pixel & 0xffffff,
               textConsole ? "; the verbose console's text is drawn over it, around which the colour stays" : "");
        break;
    case gopcompose::StartFill::Keep:
        GOPLOG("framebuffer left to the boot console's text (%s) until WindowServer's first frame\n",
               verbose ? "-v" : vcText ? "vc-text" : "no graphics mode");
        break;
    case gopcompose::StartFill::Black:
        GOPLOG("framebuffer cleared to black\n");
        break;
    }
    fCopyLock = IOLockAlloc();
    thread_t copier = nullptr;
    if (!fCopyLock || kernel_thread_start(copyThreadMain, this, &copier) != KERN_SUCCESS) {
        GOPLOG("no copy thread\n");
        if (fCopyLock)
            IOLockFree(fCopyLock);
        fCopyLock = nullptr;
        OSSafeReleaseNULL(fScanoutMap);
        OSSafeReleaseNULL(fRamSurface);
        fScanout = nullptr;
        if (fExternal)
            provider->setProperty("DisplayPortCount", 1ULL, 32);
        return false;
    }
    thread_deallocate(copier);
    GOPLOG("copy thread started\n");
    // IOMobileFramebufferLegacy::start waits for IOSurfaceRoot, calls start_hardware, creates the default surface and registers.
    if (!IOMobileFramebufferLegacy::start(provider)) {
        GOPLOG("IOMobileFramebufferLegacy::start failed\n");
        stopCopies();
        OSSafeReleaseNULL(fScanoutMap);
        OSSafeReleaseNULL(fRamSurface);
        fScanout = nullptr;
        if (fExternal)
            provider->setProperty("DisplayPortCount", 1ULL, 32);
        return false;
    }
    GOPLOG("started, display %ux%u at %u Hz, %ux%u mm, %s copies, thumbnail %s%u s (at most %u, 0: no limit)\n",
          display_width(), display_height(), fRefreshHz, fWidthMM, fHeightMM, fRefreshPolicy ? "with refresh" : "no refresh",
          fThumbPeriod ? "every " : "off: ", fThumbPeriod, fThumbLimit);
    return true;
}

void NativeGopDisplay::stop(IOService *provider)
{
    GOPLOG("stop\n");
    IOMobileFramebufferLegacy::stop(provider);
    stopCopies();
    releaseSurfaceMaps();
    if (fTables)
        IOFree(fTables, sizeof(gopcompose::Tables));
    if (fRow)
        IOFree(fRow, fRowBytes);
    if (fLine)
        IOFree(fLine, fVideo.v_width * 4);
    if (fOverlayRow)
        IOFree(fOverlayRow, fVideo.v_width * 4);
    fTables = nullptr;
    fRow = nullptr;
    fLine = nullptr;
    fOverlayRow = nullptr;
    if (fThumbLuma)
        IOFree(fThumbLuma, kThumbCols * kThumbRows * sizeof(uint32_t));
    if (fThumbCount)
        IOFree(fThumbCount, kThumbCols * kThumbRows * sizeof(uint32_t));
    fThumbLuma = fThumbCount = nullptr;
    for (const void *&surface : fPipe.layers.surface) {   // the layers' references
        if (surface)
            reinterpret_cast<const OSObject *>(surface)->release();
        surface = nullptr;
    }
    OSSafeReleaseNULL(fScanoutMap);
    OSSafeReleaseNULL(fRamSurface);
    fScanout = nullptr;
    OSSafeReleaseNULL(fPlugTimer);
    OSSafeReleaseNULL(fGpu);
    OSSafeReleaseNULL(fGpuFenceSymbol);
    OSSafeReleaseNULL(fDpu);
    OSSafeReleaseNULL(fDpuFlipSymbol);
    OSSafeReleaseNULL(fJobDone);
    OSSafeReleaseNULL(fDpuSlices);
    OSSafeReleaseNULL(fDpuSwapSymbol);
}

// Called by IOMobileFramebufferLegacy::start before it creates the default surface and registers the service (it tests bit 0).
bool NativeGopDisplay::start_hardware()
{
    if (!createDisplayAttributes())
        return false;
    GOPLOG("display attributes published (one %lux%lu@%u timing, 8-bit SDR sRGB)\n", fVideo.v_width, fVideo.v_height,
          fRefreshHz);
    fVBL = IOTimerEventSource::timerEventSource(this, vblFired);
    if (!fVBL || getWorkLoop()->addEventSource(fVBL) != kIOReturnSuccess) {   // the work loop of the family's command gate
        GOPLOG("Failed to create VBL timer\n");
        OSSafeReleaseNULL(fVBL);
        return false;
    }
    // The completions of the swaps that wait for the copy thread, on the same work loop.
    fJobDone = IOInterruptEventSource::interruptEventSource(this, jobDoneAction);
    if (!fJobDone || getWorkLoop()->addEventSource(fJobDone) != kIOReturnSuccess) {
        OSSafeReleaseNULL(fJobDone);
        fFlipMode = false;
        fDefer = gopcompose::kDeferNever;
        GOPLOG("no work-loop source for the deferred completions: every swap completes at once, flip mode off\n");
    }
    if (fExternal) {   // D6: the plug schedule's timer, on the same work loop
        fPlugTimer = IOTimerEventSource::timerEventSource(this, plugFired);
        if (!fPlugTimer || getWorkLoop()->addEventSource(fPlugTimer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(fPlugTimer);
            GOPLOG("no plug timer: the second display stays as it starts\n");
        }
    }
    fVBL->enable();
    at<uint8_t>(this, kHardwareStarted) = 1;
    PMinit();
    getPlatform()->PMRegisterDevice(getProvider(), this);   // the provider is no power-managed AppleARMIODevice here
    registerPowerDriver(this, gPowerStates, 2);
    if (fExternal) {   // D6: disconnected (or connected with piano-dp-fake-boot=1) until the schedule's first step
        fPlugged = fFake.atBoot;
        hotPlug_notify(fPlugged ? 1 : 0);
        const pianodp::PlugStep first = pianodp::plugStep(fFake, 0);
        if (fPlugTimer && first.valid)
            fPlugTimer->setTimeoutMS(first.atS * 1000);
        if (first.valid)
            GOPLOG("hardware started (VBL timer %u Hz while connected, power states registered, hot plug %u; %s in %u s)\n", fRefreshHz,
                   fPlugged ? 1u : 0u, first.connect ? "plugged" : "unplugged", first.atS);
        else
            GOPLOG("hardware started (VBL timer %u Hz, power states registered, hot plug %u; no change scheduled)\n", fRefreshHz,
                   fPlugged ? 1u : 0u);
        return true;
    }
    // The panel is always there: report it plugged in (the digital out state IOMobileFramebufferGetDigitalOutState answers). Unplugged
    // (0), WindowServer kept the display disabled and composited a virtual one as the main display.
    hotPlug_notify(1);
    GOPLOG("hardware started (VBL timer %u Hz, power states registered, hot plug 1)\n", fRefreshHz);
    return true;
}

IOReturn NativeGopDisplay::stop_hardware_gated()
{
    if (fVBL) {
        fVBL->cancelTimeout();
        getWorkLoop()->removeEventSource(fVBL);
        OSSafeReleaseNULL(fVBL);
    }
    if (fJobDone)   // released in stop(), once the copy thread (which fires it) has ended
        getWorkLoop()->removeEventSource(fJobDone);
    if (fPlugTimer) {   // released in stop()
        fPlugTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(fPlugTimer);
    }
    return kIOReturnSuccess;
}

// D6: the second display's frame store, RAM only this kext writes (its copies and thumbnails) and nothing scans out.
bool NativeGopDisplay::setupRamSurface()
{
    memset(&fVideo, 0, sizeof(fVideo));
    fVideo.v_width = fFake.width;
    fVideo.v_height = fFake.height;
    fVideo.v_rowBytes = fFake.width * 4;
    fVideo.v_depth = 32;
    fVideoPhys = 0;
    const uint64_t bytes = static_cast<uint64_t>(fVideo.v_rowBytes) * fVideo.v_height;
    fRamSurface = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, 4096);
    fScanout = fRamSurface ? static_cast<uint8_t *>(fRamSurface->getBytesNoCopy()) : nullptr;
    if (!fScanout) {
        OSSafeReleaseNULL(fRamSurface);
        GOPLOG("no RAM surface of %llu bytes\n", bytes);
        return false;
    }
    GOPLOG("RAM surface %lux%lu row %lu (%llu bytes): frames are copied there and never scanned out\n", fVideo.v_width,
           fVideo.v_height, fVideo.v_rowBytes, bytes);
    return true;
}

// D6, on the work loop: the plug schedule's next step (hot plug 1 or 0), then the timer for the one after it.
void NativeGopDisplay::plugFired(OSObject *owner, IOTimerEventSource *timer)
{
    auto *display = static_cast<NativeGopDisplay *>(owner);
    const pianodp::PlugStep step = pianodp::plugStep(display->fFake, display->fPlugStep);
    if (!step.valid)
        return;
    display->fPlugStep++;
    display->fPlugged = step.connect;
    display->hotPlug_notify(step.connect ? 1 : 0);
    const pianodp::PlugStep next = pianodp::plugStep(display->fFake, display->fPlugStep);
    IOLog("NativeGopDisplay-ext: hot plug %u, %u s after the start (piano-dp-fake: connection %u of %u)%s\n", step.connect ? 1u : 0u,
          step.atS, step.plugNumber, display->fFake.cycles, next.valid ? "" : "; the schedule is done");
    if (next.valid)
        timer->setTimeoutMS((next.atS - step.atS) * 1000);
    if (step.connect)
        display->swap_begin_gated();   // the VBL ticks again (they stop while disconnected)
}

void NativeGopDisplay::vblFired(OSObject *owner, IOTimerEventSource *)
{
    auto *display = static_cast<NativeGopDisplay *>(owner);
    display->fTickRuns++;
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
    // The refresh countdown of the frame on screen (none while a swap waits for the copy thread; the copy thread skips the ones a
    // fence-covered frame does not need, GopCompose.h refreshNeeded).
    Job refresh;
    if (fFrames == frames && fRefreshPolicy && fPipe.refreshJob(&refresh))
        queueRefresh(refresh);
    // A swap's job left undrawn (its GPU fence had not passed): the whole panel again from the current state, once the GPU is done.
    if (__atomic_load_n(&fUnfinished, __ATOMIC_ACQUIRE) && fPipe.retryJob(true, &refresh)) {
        fRetries++;
        queueRefresh(refresh);
    }
    // A thumbnail that is due while no new frame comes: one refresh of the frame on screen takes it.
    if (fThumbPeriod && fThumbNext && !fThumbRequested && mach_absolute_time() >= fThumbNext && fPipe.forcedRefreshJob(&refresh)) {
        fThumbRequested = true;
        queueRefresh(refresh);
    }
    switch (gopcompose::queueTick(at<IOMFBSwapIORequest *>(this, kSwapQueueFirst) != nullptr,
                                  at<IOMFBSwapIORequest *>(this, kCurrentSwap) != nullptr)) {
    case gopcompose::QueueTick::Behind:
        fBehind++;
        break;
    case gopcompose::QueueTick::Stalled:
        fStalled++;
        break;
    case gopcompose::QueueTick::Idle:
        break;
    }
    // About every 10 s while swaps arrive (by the clock since 2026-11-12: the tick stops while nothing moves, so counting its calls no
    // longer measures time; a period that spans such a stop is longer, and its length is logged).
    fTicks++;
    const uint64_t reportNow = mach_absolute_time();
    uint64_t tenSeconds = 0;
    nanoseconds_to_absolutetime(10000000000ull, &tenSeconds);
    if (!fReportNext) {
        fReportStart = reportNow;
        fReportNext = reportNow + tenSeconds;
    }
    if (reportNow >= fReportNext && fSubmitted + fFrames + fDropped + fStalled != fReported) {
        fReported = fSubmitted + fFrames + fDropped + fStalled;
        uint64_t periodNs = 0;
        absolutetime_to_nanoseconds(reportNow - fReportStart, &periodNs);
        fReportStart = reportNow;
        fReportNext = reportNow + tenSeconds;
        GOPLOG("period %llu ms: %llu VBL ticks, %llu times the tick stopped while nothing moved%s\n", periodNs / 1000000, fTickRuns,
              fIdleStops, fVblIdleStop || fFlipMode ? "" : " (native-gop-display-vbl < 2: it runs while the display is on)");
        fTickRuns = fIdleStops = 0;
        if (fDamageMode) {
            const uint64_t frames = fDamageCopies + fDamageNothing + fWholeNoRegion + fWholeUncovered + fWholePeriodic + fWholeOther;
            GOPLOG("damage: %llu of %llu frames from their dirty regions, mean %llu.%llu%% of the panel, %llu us mean, %llu us max "
                  "(%llu with nothing of the frame to draw, %llu refreshes of what was drawn); whole: %llu without a region, %llu not "
                  "fence-covered, %llu periodic, %llu other (first, after a flip or an undrawn frame, damage off); %llu of %llu new "
                  "frames named a region; audit %llu points, %llu differed%s\n",
                  fDamageCopies, frames, fDamageCopies ? fDamagePixels * 1000 / fDamageCopies / (fVideo.v_width * fVideo.v_height) / 10 : 0,
                  fDamageCopies ? fDamagePixels * 1000 / fDamageCopies / (fVideo.v_width * fVideo.v_height) % 10 : 0,
                  fDamageCopies ? fDamageNs / fDamageCopies / 1000 : 0, fDamageMaxNs / 1000, fDamageNothing, fDamageRefreshes,
                  fWholeNoRegion, fWholeUncovered, fWholePeriodic, fWholeOther, fDirtyFrames, fPeriodFrames,
                  fAuditSamples, fAuditMismatches, fDamageAuditOff ? " (damage off for this boot)" : "");
            fDamageCopies = fDamageNs = fDamageMaxNs = fDamagePixels = fDamageNothing = fDamageRefreshes = 0;
            fWholeNoRegion = fWholeUncovered = fWholePeriodic = fWholeOther = fDirtyFrames = fPeriodFrames = 0;
            fAuditSamples = fAuditMismatches = 0;
        }
        GOPLOG("%llu swaps submitted, %llu shown, %llu dropped, %llu ticks stalled (current %p), last frame %llu.%llu%% "
              "non-black mean #%06llx centre #%06llx; %llu copies mean %llu us (prepare %llu read %llu convert %llu write %llu) max %llu us, "
              "%llu waits released\n",
              fSubmitted, fFrames, fDropped, fStalled, at<IOMFBSwapIORequest *>(this, kCurrentSwap), fLastLitPerMille / 10,
              fLastLitPerMille % 10, fLastMean, fLastCenter,
              fCopies, fCopies ? fCopyNs / fCopies / 1000 : 0, fCopies ? fPrepareNs / fCopies / 1000 : 0,
              fCopies ? fReadNs / fCopies / 1000 : 0, fCopies ? fConvertNs / fCopies / 1000 : 0, fCopies ? fWriteNs / fCopies / 1000 : 0,
              fCopyMaxNs / 1000, fWaitReleased);
        if (fCopiesSkipped)
            GOPLOG("%llu copies skipped for newer ones\n", fCopiesSkipped);
        GOPLOG("at most %llu frames in a second\n", fPeakFps);
        if (fFlipMode)
            GOPLOG("flip mode: %llu swaps flipped (no copy, no refresh copies), %llu copied (PianoDpu refused them, is not serving, "
                  "or a cursor was on the panel: %llu)\n", fFlips, fFlipCopies, fCursorForced);
        if (fCursorJobs)
            GOPLOG("cursor: %llu updates of its old and new rectangles, mean %llu us max %llu us, last at (%d, %d)\n", fCursorJobs,
                  fCursorNs / fCursorJobs / 1000, fCursorMaxNs / 1000, fCursorX, fCursorY);
        if (fDeferred || fBehind)
            GOPLOG("%llu swaps completed once the copy thread was done (native-gop-display-defer=%u), mean %llu us max %llu us after "
                  "they were programmed; %llu times a newer swap was queued behind the one waiting (not stalls)\n", fDeferred, fDefer,
                  fDeferred ? fDeferNs / fDeferred / 1000 : 0, fDeferMaxNs / 1000, fBehind);
        if (fRefreshSkipped)
            GOPLOG("%llu refresh copies not needed (their frame's fence wait saw the GPU done)\n", fRefreshSkipped);
        if (fUnfinishedJobs || fRetries || fRefreshAborts)
            GOPLOG("%llu jobs left undrawn (the GPU had not finished their frame within %u ms: the previous one stayed), %llu tick "
                  "retries, %llu refresh copies cut short by a swap\n", fUnfinishedJobs, fGpuFenceWaitMs, fRetries, fRefreshAborts);
        fUnfinishedJobs = fRetries = fRefreshAborts = 0;
        fCursorJobs = fCursorNs = fCursorMaxNs = fDeferred = fDeferNs = fDeferMaxNs = fBehind = 0;
        if (fFenceWaits)
            GOPLOG("%llu swaps from submit to programmed mean %llu us max %llu us\n", fFenceWaits,
                  fFenceWaitNs / fFenceWaits / 1000, fFenceWaitMaxNs / 1000);
        GOPLOG("scheduling: %llu copy wakeups over 20 ms (longest %llu ms), %llu VBL ticks over 20 ms late (latest by %llu ms)\n",
              fWakeLate, fWakeMaxNs / 1000000, fVBLLate, fVBLLateMaxNs / 1000000);
        if (fGpuWaits)
            GOPLOG("%llu copies waited for the GPU's fence, mean %llu us max %llu us, %llu gave up after %u ms\n", fGpuWaits,
                  fGpuWaitNs / fGpuWaits / 1000, fGpuWaitMaxNs / 1000, fGpuTimeouts, fGpuFenceWaitMs);
        if (fCopyMode) {   // what the copy mode saved: refresh copies not made (at this period's mean copy time), the reads' cost
            const uint64_t mean = fCopies ? fCopyNs / fCopies : 0;
            GOPLOG("copy mode %#x: %llu refresh copies skipped (about %llu ms of copying saved, mean copy %llu us); "
                  "%llu cache invalidations mean %llu us%s\n", fCopyMode, fRefreshSkipped, fRefreshSkipped * mean / 1000000, mean / 1000,
                  fInvalidates, fInvalidates ? fInvalidateNs / fInvalidates / 1000 : 0,
                  fCachedMapRefused ? " (a cacheable mapping was refused: those surfaces read uncached)" : "");
            fInvalidates = fInvalidateNs = 0;
        }
        fRefreshSkipped = 0;
        fGpuWaits = fGpuWaitNs = fGpuWaitMaxNs = fGpuTimeouts = 0;
        fFenceWaits = fFenceWaitNs = fFenceWaitMaxNs = 0;
        fPeakFps = 0;
        fWakeLate = fWakeMaxNs = fVBLLate = fVBLLateMaxNs = 0;
        fCopies = fCopyNs = fCopyMaxNs = fPrepareNs = fReadNs = fConvertNs = fWriteNs = 0;
    }
    // D6: no tick while the second display is disconnected and nothing is queued (a submitted swap calls this anyway).
    if (fExternal && !fPlugged && !at<IOMFBSwapIORequest *>(this, kSwapQueueFirst)) {
        fVBLDue = 0;
        return kIOReturnSuccess;
    }
    // Flip mode, and native-gop-display-vbl=2 (the default with damage copies): no tick while nothing needs one (a deferred swap
    // completes from jobDoneAction, a ready one makes the family call this, swap_queue_finalize_gated).
    if (gopcompose::vblIdleStops(fFlipMode || fVblIdleStop, fPipe.refreshLeft, __atomic_load_n(&fUnfinished, __ATOMIC_ACQUIRE),
                                 fWaitingSwap != nullptr, at<IOMFBSwapIORequest *>(this, kSwapQueueFirst) != nullptr)) {
        if (!fVblIdle)
            fIdleStops++;
        fVblIdle = true;
        fVBLDue = 0;
        return kIOReturnSuccess;
    }
    fVblIdle = false;
    // The display off: no tick while nothing needs one (the panel is dark; a submitted swap calls this anyway, the power-on restarts it).
    if (gopcompose::vblStops(fVblDarkStop, fPowerPublished && !fPowerPublishedOn, fPipe.refreshLeft,
                             __atomic_load_n(&fUnfinished, __ATOMIC_ACQUIRE), fWaitingSwap != nullptr,
                             at<IOMFBSwapIORequest *>(this, kSwapQueueFirst) != nullptr)) {
        if (!fVblStopped && (++fVblStops <= 4 || fVblStops % 64 == 0))
            GOPLOG("display off: the VBL tick stopped, nothing queued or to refresh (stop %u; native-gop-display-vbl=0 keeps it)\n", fVblStops);
        fVblStopped = true;
        fVBLDue = 0;
        return kIOReturnSuccess;
    }
    fVblStopped = false;
    uint64_t period;
    nanoseconds_to_absolutetime(1000000000ULL / fRefreshHz, &period);
    fVBLDue = mach_absolute_time() + period;
    return fVBL->setTimeoutUS(1000000 / fRefreshHz);
}

// CoreAnimation asks swap_wait, without blocking, whether its last swaps are done before it composes the next frame. The family
// answers kIOReturnBusy for the last completed swap as long as no later one replaces it (a display controller is still scanning it
// out). Here the panel shows a copy, so it is done: told it is busy, WindowServer updated at about 10 frames a second however little
// it drew (measured with the VM-era display kext). The surfaces themselves stay on their layers (the family hands one back only when a
// later swap replaces it, and such a swap waits while the copy thread reads it: GopCompose.h admitSwap).
IOReturn NativeGopDisplay::swap_wait(IOMobileFramebufferUserClient *client, unsigned int swapID, unsigned int options, unsigned int timeout)
{
    IOReturn result = IOMobileFramebufferLegacy::swap_wait(client, swapID, options, timeout);
    if (result == kIOReturnBusy && (options & 0x7fffffffu) == 0 && swapID == at<uint32_t>(this, kLastCompletedSwap)) {
        fWaitReleased++;
        return kIOReturnSuccess;
    }
    if (result != kIOReturnSuccess && (++fWaitFailures <= 20 || fWaitFailures % 1000 == 0))
        GOPLOG("swap_wait(%u, 0x%x, %u) = 0x%x (%llu failures): last %u power %u/%u powered %u started 0x%x current %p queue %p\n",
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
    if (!fExternal)   // the panel's power only: D6's second display publishes none (its readers mean the panel)
        publishDisplayPower(on);
    if (fBacklightCallout)
        backlightCallout(on);
    if (on == fDisplayOn)
        return kIOReturnSuccess;
    fDisplayOn = on;
    at<uint32_t>(this, kHardwarePowered) = on;
    GOPLOG("display %s\n", on ? "on" : "off");
    if (fDpuPanel) {   // the panel's light sleep (PianoDpu's thread; the backlight's request above goes first)
        IOService *dpu = dpuService(true);
        if (dpu)
            dpu->callPlatformFunction("PianoDpuPanel", false, reinterpret_cast<void *>(uintptr_t(on)), nullptr, nullptr, nullptr);
        else
            GOPLOG("display %s: no PianoDpu service (piano-dpu-panel needs piano-dpu=1)\n", on ? "on" : "off");
    }
    powerstate_notify(true, on);
    if (on && (fVblStopped || fVblIdle))   // the tick stopped while the display was off (or nothing moved): again from now
        swap_begin_gated();
    return kIOReturnSuccess;
}

// The PianoDpu service (piano-dpu-pcc / -panel / -refresh=auto only), looked up at once (now) or at most once a second (the swap
// path), retained once found.
IOService *NativeGopDisplay::dpuService(bool now)
{
    IOService *found = __atomic_load_n(&fDpuSlices, __ATOMIC_ACQUIRE);
    if (found)
        return found;
    const uint64_t t = mach_absolute_time();
    if (!now && t < fDpuSlicesLookupNext)
        return nullptr;
    uint64_t second = 0;
    nanoseconds_to_absolutetime(1000000000ull, &second);
    fDpuSlicesLookupNext = t + second;
    OSDictionary *match = IOService::serviceMatching("PianoDpu");
    IOService *dpu = match ? IOService::copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (!dpu)
        return nullptr;
    if (!__atomic_compare_exchange_n(&fDpuSlices, &found, dpu, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {   // another caller was first
        dpu->release();
        return found;
    }
    GOPLOG("PianoDpu service found: %s\n", dpu->getName());
    return dpu;
}

// Every call: "piano-display-power" (PianoPolicy, PianoBacklight, PianoKeyboard read it), and on a change of it a property-change
// message to the interested clients. Their handlers run here, inside the framebuffer's gated power change: they only record it.
void NativeGopDisplay::publishDisplayPower(bool on)
{
    setProperty("piano-display-power", on);
    if (fPowerPublished && on == fPowerPublishedOn)
        return;
    fPowerPublished = true;
    fPowerPublishedOn = on;
    messageClients(kIOMessageServicePropertyChange, const_cast<char *>("piano-display-power"));
}

// piano-backlight=2: every call, with both power states as they are (PianoBacklight logs their raw values, so an intermediate state
// WindowServer might use for dimming shows up), only records a request there: nothing waits for the backlight's I2C.
void NativeGopDisplay::backlightCallout(bool on)
{
    if (!fBacklight) {
        OSDictionary *match = IOService::serviceMatching("PianoBacklight");
        IOService *backlight = match ? IOService::copyMatchingService(match) : nullptr;
        if (match)
            match->release();
        if (!backlight) {
            GOPLOG("display %s: no PianoBacklight service yet (it reads piano-display-power when it starts)\n",
                  on ? "on" : "off");
            return;
        }
        fBacklight = backlight;
    }
    const uintptr_t system = at<uint32_t>(this, kSystemPowerState), api = at<uint32_t>(this, kAPIPowerState);
    fBacklight->callPlatformFunction("PianoBacklightDisplay", false, reinterpret_cast<void *>(uintptr_t(on)),
                                     reinterpret_cast<void *>(system), reinterpret_cast<void *>(api), nullptr);
}

// AppleParavirtDisplay::program_hardware_gated (its slot 473): show the swap, then complete it. Every layer the swap hands over goes into
// the composition (GopCompose.h, since 2026-11-12): layer 0 the frame at (0, 0), layer 1 WindowServer's cursor at its origin (it swaps
// the cursor as a layer of its own, also alone: 'swap layers 0x2: 1: 128x128'), layers 2 and 3 only logged (no position is known for
// them; none was seen with a surface). The swap completes at once, unless the copy thread is reading a surface it takes off the screen
// (native-gop-display-defer=1) or, in flip mode, until its own job is done: then jobDoneAction completes it.
void NativeGopDisplay::program_hardware_gated()
{
    IOMFBSwapIORequest *swap = at<IOMFBSwapIORequest *>(this, kCurrentSwap);
    const uint32_t swapID = at<uint32_t>(swap, kSwapID);
    uint64_t &submitted = fSubmitTime[swapID % 16];
    if (submitted) {
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - submitted, &waited);
        submitted = 0;
        fFenceWaits++;
        fFenceWaitNs += waited;
        if (waited > fFenceWaitMaxNs)
            fFenceWaitMaxNs = waited;
    }
    gopcompose::SwapLayers layers;
    layers.mask = at<uint8_t>(swap, kSwapLayers) & ((1u << kSwapLayerCount) - 1);
    for (unsigned layer = 0; layer < kSwapLayerCount; layer++)
        layers.surface[layer] = (layers.mask >> layer & 1) ? at<IOSurface *>(swap, kSwapMainSurface + 8 * layer) : nullptr;
    if (layers.mask & 2) {   // unaligned words
        memcpy(&layers.x1, reinterpret_cast<const uint8_t *>(swap) + kSwapLayer1X, sizeof(layers.x1));
        memcpy(&layers.y1, reinterpret_cast<const uint8_t *>(swap) + kSwapLayer1Y, sizeof(layers.y1));
    }
    fSequence.next(swapID);
    if ((layers.mask & 1) && layers.surface[0]) {   // a new frame: what of it changed, if the swap says (unaligned words)
        uint32_t dirty[4];
        memcpy(dirty, reinterpret_cast<const uint8_t *>(swap) + kSwapDirtyRegion, sizeof(dirty));
        const bool valid = at<uint8_t>(swap, kSwapDirtyValid) & 1;
        layers.dirty = gopcompose::dirtyRegion(valid, dirty, uint32_t(fVideo.v_width), uint32_t(fVideo.v_height));
        fPeriodFrames++;
        if (layers.dirty.known) {
            fDirtyFrames++;
            fDirtyFramesAll++;
        }
        // Another size, format or row length than the frame before (a display mode change): its region is about another picture.
        IOSurface *frame = static_cast<IOSurface *>(const_cast<void *>(layers.surface[0]));
        const uint64_t shape = (ioSurfaceWidth(frame) & 0xffff) << 48 | (ioSurfaceHeight(frame) & 0xffff) << 32 |
                               uint64_t(ioSurfacePixelFormat(frame) ^ uint32_t(ioSurfaceBytesPerRow(frame)));
        if (shape != fLastShape && fFrames && layers.dirty.known) {
            layers.dirty.known = false;
            if (++fShapeChanges <= 4)
                GOPLOG("damage: swap %u's frame is a %llux%llu surface of another shape than the last one: composed whole\n", swapID,
                       ioSurfaceWidth(frame), ioSurfaceHeight(frame));
        }
        fLastShape = shape;
        // A swap that never reached this driver since the last frame (dropped, aborted, cancelled) took its change along: whole.
        if (fSequence.takeGap() && layers.dirty.known && fFrames) {
            layers.dirty.known = false;
            if (++fSwapGaps <= 4 || fSwapGaps % 100 == 0)
                GOPLOG("damage: swap %u does not follow the last one programmed (%llu such gaps): its frame is composed whole\n", swapID,
                       fSwapGaps);
        }
        // The first regions, with the layer's own rectangles beside them (what WindowServer means by them is read from these lines).
        if (fDamageMode && (valid || fDirtyLogs < 2) && fDirtyLogs < 12) {
            fDirtyLogs++;
            uint32_t source[4], dest[4];
            memcpy(source, reinterpret_cast<const uint8_t *>(swap) + kSwapLayerSourceRect, sizeof(source));
            memcpy(dest, reinterpret_cast<const uint8_t *>(swap) + kSwapLayerDestRect, sizeof(dest));
            GOPLOG("dirty region of swap %u: valid %u words %u %u %u %u -> %s %d,%d %dx%d; layer 0 source %u,%u %ux%u dest %u,%u %ux%u\n",
                   swapID, valid ? 1u : 0u, dirty[0], dirty[1], dirty[2], dirty[3],
                   layers.dirty.known ? (layers.dirty.rect.empty() ? "nothing changed:" : "rect") : "none (the frame is composed whole):",
                   layers.dirty.rect.x, layers.dirty.rect.y, layers.dirty.rect.w, layers.dirty.rect.h, source[0], source[1], source[2],
                   source[3], dest[0], dest[1], dest[2], dest[3]);
        }
        if (fDamageMode && fFrames == 99 && !fDirtyFramesAll)
            GOPLOG("damage: none of WindowServer's first 100 frames named a dirty region: every frame is composed whole\n");
    }
    noteLayers(layers, swapID);
    if (!fCursorMode) {   // native-gop-display-cursor=0: layer 1 is neither drawn nor held
        layers.mask &= ~2u;
        layers.surface[1] = nullptr;
    }
    uint32_t fence = 0;
    const bool fenced = gpuFence(&fence);
    gopcompose::LayerChange change;
    const Job job = fPipe.swapJob(layers, fenced, fence, fFlipMode, &change);
    for (const void *taken : change.taken)   // the layers' own references
        if (taken)
            reinterpret_cast<const OSObject *>(taken)->retain();
    if (change.base) {
        if (!fFrames++) {   // WindowServer's first frame: the verbose boot console stops drawing over the framebuffer
            if (!fExternal)
                PE_initialize_console(nullptr, kPEDisableScreen);
            GOPLOG("first frame (swap %u)%s\n", swapID,
                   fExternal ? ": WindowServer draws on the second display" : "; boot console released from the screen");
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
        if (fDpuSwaps && fDpuSwapSymbol) {   // PianoDpu's refresh policy: a frame is shown
            IOService *dpu = dpuService(false);
            if (dpu)
                dpu->callPlatformFunction(fDpuSwapSymbol, false, nullptr, nullptr, nullptr, nullptr);
        }
    }
    // The job and the completion rule in one step under the lock: the copy thread cannot take a job that reads a surface this swap
    // releases between the check and the offer (the offer merges such a job into this one, which reads only what stays on screen).
    retainJob(job);
    IOLockLock(fCopyLock);
    const gopcompose::Admission admission = gopcompose::admitSwap(fSlot, fBusy, job, change, fDefer);
    const bool wait = admission.waitSerial && fDoneSerial < admission.waitSerial;
    if (wait)
        fWaitSerial = admission.waitSerial;
    if (wait && admission.abortBusy)   // the refresh in the way stops at its next row
        __atomic_store_n(&fAbortSerial, fBusy.serial, __ATOMIC_RELEASE);
    if (admission.hadJob) {
        if (admission.offer == gopcompose::Offer::Taken)
            fCopyRequestTime = mach_absolute_time();
        IOLockWakeup(fCopyLock, &fSlot, true);
    }
    IOLockUnlock(fCopyLock);
    if (admission.hadJob && admission.offer == gopcompose::Offer::Merged) {
        if (!admission.displaced.refresh)
            fCopiesSkipped++;   // a swap's job the copy thread had no time for: this newer state replaces it
        releaseJob(admission.displaced);
    }
    for (const void *released : change.released)   // still held by any job that reads them
        if (released)
            reinterpret_cast<const OSObject *>(released)->release();
    if (wait) {
        fPipe.pending = true;
        fPipe.waitSerial = admission.waitSerial;
        fWaitingSwap = swap;
        fDeferStart = mach_absolute_time();
        return;
    }
    swap_complete_gated();
}

// PianoDpu's "PianoDpuFlip" (the copy thread only): surface on the panel, or with nullptr the boot framebuffer back. True once the
// hardware latched it. PianoDpu's service is looked up once a second until found (it registers only with piano-dpu=2).
bool NativeGopDisplay::dpuFlip(IOSurface *surface, PianoDpuFlipRequest *request)
{
    if (!fDpu) {
        const uint64_t now = mach_absolute_time();
        if (now < fDpuLookupNext)
            return false;
        uint64_t second = 0;
        nanoseconds_to_absolutetime(1000000000ull, &second);
        fDpuLookupNext = now + second;
        OSDictionary *match = IOService::serviceMatching(kPianoDpuServiceClass);
        IOService *dpu = match ? IOService::copyMatchingService(match) : nullptr;
        OSSafeReleaseNULL(match);
        if (!dpu)
            return false;
        fDpu = dpu;
        GOPLOG("flips through %s: a 'BGRA' swap is shown by the DPU itself, the others are copied\n", dpu->getName());
    }
    *request = {};
    request->size = sizeof(*request);
    request->version = kPianoDpuFlipVersion;
    if (surface) {
        request->surface = surface;
        request->memory = ioSurfaceMemory(surface);
        request->offset = ioSurfaceBaseOffset(surface);
        request->rowBytes = ioSurfaceBytesPerRow(surface);
        request->width = ioSurfaceWidth(surface);
        request->height = ioSurfaceHeight(surface);
        request->format = ioSurfacePixelFormat(surface);
        request->planes = ioSurfacePlaneCount(surface);
        request->copyUs = fCopies ? static_cast<uint32_t>(fCopyNs / fCopies / 1000) : 0;
    }
    if (fDpu->callPlatformFunction(fDpuFlipSymbol, false, request, nullptr, nullptr, nullptr) != kIOReturnSuccess)
        return false;
    static unsigned refusals;
    if (surface && request->result != kPianoDpuFlipShown && refusals++ < 8)
        GOPLOG("flip refused (%s): %llux%llu row %llu format %#x; copied\n", pianoDpuFlipResultName(request->result),
              request->width, request->height, request->rowBytes, request->format);
    return request->result == kPianoDpuFlipShown;
}

// On the work loop: the job a waiting swap needs is done (its read of the surfaces the swap takes off the screen is over; in flip mode
// the swap's own frame is on the panel): complete the swap, then take the next queued one.
void NativeGopDisplay::jobDoneAction(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *display = static_cast<NativeGopDisplay *>(owner);
    IOMFBSwapIORequest *swap = display->fWaitingSwap;
    if (!swap)
        return;
    IOLockLock(display->fCopyLock);
    const bool done = display->fDoneSerial >= display->fWaitSerial;
    if (done)
        display->fWaitSerial = 0;
    IOLockUnlock(display->fCopyLock);
    if (!done)
        return;
    display->fWaitingSwap = nullptr;
    uint64_t waited = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - display->fDeferStart, &waited);
    display->fDeferred++;
    display->fDeferNs += waited;
    if (waited > display->fDeferMaxNs)
        display->fDeferMaxNs = waited;
    display->fPipe.swapDone(__atomic_load_n(&display->fLastFlipped, __ATOMIC_ACQUIRE));
    if (at<IOMFBSwapIORequest *>(display, kCurrentSwap) == swap)
        display->swap_complete_gated();
    display->swap_begin_gated();
}

void NativeGopDisplay::retainJob(const Job &job)
{
    if (job.base)
        reinterpret_cast<const OSObject *>(job.base)->retain();
    if (job.overlay)
        reinterpret_cast<const OSObject *>(job.overlay)->retain();
}

void NativeGopDisplay::releaseJob(const Job &job)
{
    if (job.base)
        reinterpret_cast<const OSObject *>(job.base)->release();
    if (job.overlay)
        reinterpret_cast<const OSObject *>(job.overlay)->release();
}

// A refresh job (the work loop): dropped when a swap's job waits (that one draws a newer state anyway).
void NativeGopDisplay::queueRefresh(const Job &job)
{
    retainJob(job);
    Job displaced;
    IOLockLock(fCopyLock);
    const gopcompose::Offer result = gopcompose::offer(fSlot, job, &displaced);
    if (result == gopcompose::Offer::Taken)
        fCopyRequestTime = mach_absolute_time();
    if (result != gopcompose::Offer::Dropped)
        IOLockWakeup(fCopyLock, &fSlot, true);
    IOLockUnlock(fCopyLock);
    if (result == gopcompose::Offer::Dropped)
        releaseJob(job);
    else if (result == gopcompose::Offer::Merged)
        releaseJob(displaced);
}

static void fourcc(uint32_t format, char code[5])
{
    for (unsigned i = 0; i < 4; i++) {
        const char c = static_cast<char>(format >> (24 - 8 * i));
        code[i] = c >= 32 && c < 127 ? c : '?';
    }
    code[4] = 0;
}

// Each new combination of layers and each new kind of layer (its index, size and format, or off), once a boot (the first kLayerKinds).
void NativeGopDisplay::noteLayers(const gopcompose::SwapLayers &swap, uint32_t swapID)
{
    if (!(fMasksSeen >> swap.mask & 1)) {
        fMasksSeen |= 1u << swap.mask;
        GOPLOG("swap layers %#x first seen (swap %u)\n", swap.mask, swapID);
    }
    for (unsigned layer = 0; layer < kSwapLayerCount; layer++) {
        if (!(swap.mask >> layer & 1))
            continue;
        IOSurface *surface = static_cast<IOSurface *>(const_cast<void *>(swap.surface[layer]));
        LayerKind kind = {layer, 0, 0, 0, !surface};
        if (surface) {
            kind.width = static_cast<uint32_t>(ioSurfaceWidth(surface));
            kind.height = static_cast<uint32_t>(ioSurfaceHeight(surface));
            kind.format = ioSurfacePixelFormat(surface);
        }
        bool seen = false;
        for (unsigned i = 0; i < fKindCount && i < kLayerKinds && !seen; i++)
            seen = fKinds[i].layer == kind.layer && fKinds[i].off == kind.off && fKinds[i].width == kind.width &&
                   fKinds[i].height == kind.height && fKinds[i].format == kind.format;
        if (seen)
            continue;
        if (fKindCount >= kLayerKinds) {
            if (fKindCount++ == kLayerKinds)
                GOPLOG("more than %u kinds of layers: the others are not logged\n", kLayerKinds);
            continue;
        }
        fKinds[fKindCount++] = kind;
        char code[5];
        fourcc(kind.format, code);
        if (kind.off)
            GOPLOG("layer %u off (swap %u)%s\n", layer, swapID,
                   layer == 0 ? ": no frame, the panel shows black (the cursor still drawn)" : layer == 1 ? ": the cursor hidden" : "");
        else if (layer == 0)
            GOPLOG("layer 0: %ux%u '%s' (swap %u): the frame, composed at (0, 0)\n", kind.width, kind.height, code, swapID);
        else if (layer == 1)
            GOPLOG("layer 1: %ux%u '%s' at (%d, %d) (swap %u): the cursor, %s\n", kind.width, kind.height, code, swap.x1, swap.y1, swapID,
                   !fCursorMode ? "not drawn (native-gop-display-cursor=0)"
                   : kind.format != gopcompose::kFormatBGRA ? "not drawn (only 'BGRA' is blended)"
                                                            : "blended over the frame at that origin (premultiplied alpha)");
        else
            GOPLOG("layer %u: %ux%u '%s' (swap %u): not composed (no position is known for layers 2 and 3)\n", layer, kind.width,
                   kind.height, code, swapID);
    }
}

// Layer 1's geometry for a job (the copy thread): its glyph rectangle (IOSurface's origin and extents; the whole surface with
// native-gop-display-cursor=2 or when the words are not consistent) with the surface's pixel (0, 0) at the layer's origin. False: not
// drawn (none, the cursor off, not 'BGRA').
bool NativeGopDisplay::overlayOf(IOSurface *surface, int32_t x, int32_t y, gopcompose::Overlay *o)
{
    *o = gopcompose::Overlay();
    if (!surface || !fCursorMode)
        return false;
    const uint64_t width = ioSurfaceWidth(surface), height = ioSurfaceHeight(surface);
    if (ioSurfacePixelFormat(surface) != gopcompose::kFormatBGRA || !width || !height || width > 16384 || height > 16384)
        return false;
    IOSurfaceOriginAndExtents extents = {};
    if (fCursorMode == 1)
        extents = ioSurfaceOriginAndExtents(surface);
    o->visible = true;
    o->x = x;
    o->y = y;
    o->glyph = gopcompose::glyphRect(uint32_t(width), uint32_t(height), fCursorMode == 1 ? extents.words : nullptr);
    gopcompose::hotSpot(extents.words, &o->hotX, &o->hotY);
    if ((width != fGlyphW || height != fGlyphH || !(o->glyph == fGlyphLogged)) && fCursorLogs < 8) {   // the first few shapes
        fCursorLogs++;
        fGlyphW = uint32_t(width);
        fGlyphH = uint32_t(height);
        fGlyphLogged = o->glyph;
        GOPLOG("cursor: surface %llux%llu, glyph %d,%d %dx%d, hot spot (%d, %d) (origin-and-extents words %u %u %u %u %u %u); its "
               "pixel (0, 0) at (%d, %d) on the panel\n", width, height, o->glyph.x, o->glyph.y, o->glyph.w, o->glyph.h, o->hotX, o->hotY,
               extents.words[0], extents.words[1], extents.words[2], extents.words[3], extents.words[4], extents.words[5], x, y);
    }
    return true;
}

// One job on the copy thread, after its fence wait: whether it is drawn at all (GopCompose.h verdict: never a frame the GPU has not
// finished), the plan of what the panel shows (planJob), then a flip, a composition, or both (a refused flip is composed and the boot
// framebuffer put back).
void NativeGopDisplay::runJob(const Job &job, bool retired, bool covered, bool *flipped)
{
    *flipped = false;
    if (job.kind == Job::kFull && !job.refresh)
        fFrameCovered = covered;
    if (!job.refresh) {   // the newest swap job's fence: what the panel's state needs
        fNeedFenced = job.fenced;
        fNeedFence = job.fence;
    }
    const bool panelFinished = !fNeedFenced || (job.refresh ? gpuRetired(fNeedFence) : retired);
    switch (gopcompose::verdict(job, retired, fRefreshPolicy, fFrameCovered, panelFinished)) {
    case gopcompose::Verdict::NotNeeded:
        fRefreshSkipped++;   // the GPU had finished the frame before its copy read it; the last refresh still runs
        return;
    case gopcompose::Verdict::Unfinished:   // the previous composition stays on the panel; the ticks retry (swap_begin_gated)
        if (!job.refresh) {
            fPanel.current = false;   // the next job composes the whole panel
            fUnfinishedJobs++;
            if (fPanel.flipped) {     // flip mode: the swap completes, so the surface the DPU scans must not stay on the panel
                PianoDpuFlipRequest request;
                dpuFlip(nullptr, &request);
                fPanel.flipped = false;
                fPanel.cursor = Rect();
                GOPLOG("flip mode: the boot framebuffer (an older frame) is back until the GPU finishes the frame\n");
            }
            static unsigned undrawn;
            if (++undrawn <= 8 || undrawn % 100 == 0)
                GOPLOG("the GPU had not finished %s after %u ms (fence %u): not copied, the previous frame stays; retried at the next "
                       "ticks (%u so far)\n", job.kind == Job::kFull ? "a frame" : "the frame under a cursor move", fGpuFenceWaitMs,
                       job.fence, undrawn);
        }
        __atomic_store_n(&fUnfinished, true, __ATOMIC_RELEASE);
        return;
    case gopcompose::Verdict::Draw:
        break;
    }
    IOSurface *base = static_cast<IOSurface *>(const_cast<void *>(job.base));
    IOSurface *cursor = static_cast<IOSurface *>(const_cast<void *>(job.overlay));
    gopcompose::Overlay o;
    overlayOf(cursor, job.x1, job.y1, &o);
    const uint32_t width = static_cast<uint32_t>(fVideo.v_width), height = static_cast<uint32_t>(fVideo.v_height);
    const Rect onPanel = gopcompose::overlayOnPanel(o, width, height), whole = gopcompose::panelRect(width, height);
    // Damage (§5c): only for a frame the GPU's fence covered (a refresh: the frame on screen was), never once an audit failed, and not
    // when the periodic whole composition is due.
    const uint64_t jobStart = mach_absolute_time();
    bool fullDue = false;
    if (fDamageFullS && fLastFullCompose) {
        uint64_t period = 0;
        nanoseconds_to_absolutetime(uint64_t(fDamageFullS) * 1000000000ull, &period);
        fullDue = jobStart - fLastFullCompose >= period;
    }
    const bool frameCovered = job.refresh ? fFrameCovered : covered;
    gopcompose::Plan plan = gopcompose::planJob(fPanel, job, onPanel, whole,
                                                gopcompose::damageAllowed(fDamageMode && !fDamageAuditOff, frameCovered, fullDue));
    PianoDpuFlipRequest request;
    if (plan.tryFlip && dpuFlip(base, &request)) {
        *flipped = true;
        fFlips++;
    }
    if (fDamageMode && !*flipped && job.kind == Job::kFull && !job.refresh) {   // why a frame was composed whole, or that none of it was
        if (plan.full)
            ++*(job.frameAll ? &fWholeNoRegion : !frameCovered ? &fWholeUncovered : fullDue ? &fWholePeriodic : &fWholeOther);
        else if (plan.damage && !plan.count)
            fDamageNothing++;
    }
    if (!*flipped && plan.count && compose(plan, job, base, cursor, o, onPanel)) {   // a refresh a swap cut short
        fRefreshAborts++;
        gopcompose::abortedJob(fPanel, plan, onPanel);
        return;
    }
    if (fAuditFailedNow) {   // the region left out a pixel that changed: damage off, the whole panel now
        fAuditFailedNow = false;
        fDamageAuditOff = true;
        GOPLOG("damage: a frame's dirty region %d,%d %dx%d left out a change: panel (%d, %d) showed #%08x, the frame has #%08x (%u of "
               "%u points differ); damage copies off for this boot, the whole panel composed now (the cursor %d,%d %dx%d)\n",
               plan.frameRect.x, plan.frameRect.y, plan.frameRect.w, plan.frameRect.h, fAuditX, fAuditY, fAuditShown, fAuditWanted,
               fAuditBad, fAuditCount, onPanel.x, onPanel.y, onPanel.w, onPanel.h);
        plan = gopcompose::planJob(fPanel, job, onPanel, whole, false);
        compose(plan, job, base, cursor, o, onPanel);
    }
    if (!*flipped && plan.full && plan.count)
        fLastFullCompose = jobStart;
    if (!*flipped && plan.count) {
        if (plan.bootBack)   // the copy never shows half done: the boot framebuffer goes back on the panel only now
            dpuFlip(nullptr, &request);
        if (job.flip && (plan.full || plan.damage) && !job.refresh) {
            fFlipCopies++;
            if (plan.cursorForced && !fCursorForced++)
                GOPLOG("flip mode: a cursor is on the panel: frames are composed into the boot framebuffer (the DPU has no cursor "
                       "pipe yet); flips resume once it hides\n");
        }
    }
    if (!onPanel.empty()) {
        fCursorX = o.x;
        fCursorY = o.y;
    }
    if (*flipped || (plan.count && plan.full))   // the panel shows the whole current state again
        __atomic_store_n(&fUnfinished, false, __ATOMIC_RELEASE);
    gopcompose::commitJob(fPanel, plan, *flipped, onPanel);
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
        GOPLOG("GPU fences from %s: a swap's copy waits for the GPU work handed out before it (at most %u ms)\n",
              gpu->getName(), fGpuFenceWaitMs);
    }
    uint32_t retired = 0;
    return fGpu->callPlatformFunction(fGpuFenceSymbol, false, fence, &retired, nullptr, nullptr) == kIOReturnSuccess;
}

// The copy thread: until the GPU retired fence (32-bit sequence numbers), at most fGpuFenceWaitMs (the hang guard). True when the GPU
// retired it; *answered false when the GPU does not answer (no fence to go by: the job is drawn as without fences).
bool NativeGopDisplay::waitGpuFence(uint32_t fence, bool *answered)
{
    *answered = false;
    IOService *gpu = __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE);
    if (!gpu)
        return false;
    const uint64_t start = mach_absolute_time();
    uint64_t waited = 0;
    bool retiredInTime = false;
    for (;;) {
        uint32_t submitted = 0, retired = 0;
        if (gpu->callPlatformFunction(fGpuFenceSymbol, false, &submitted, &retired, nullptr, nullptr) != kIOReturnSuccess)
            break;
        *answered = true;
        if (static_cast<int32_t>(retired - fence) >= 0) {
            retiredInTime = true;
            break;
        }
        absolutetime_to_nanoseconds(mach_absolute_time() - start, &waited);
        if (waited >= fGpuFenceWaitMs * 1000000ull) {
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
    return retiredInTime;
}

// The copy thread, without waiting: whether the GPU retired fence (true when it does not answer).
bool NativeGopDisplay::gpuRetired(uint32_t fence)
{
    IOService *gpu = __atomic_load_n(&fGpu, __ATOMIC_ACQUIRE);
    uint32_t submitted = 0, retired = 0;
    if (!gpu || gpu->callPlatformFunction(fGpuFenceSymbol, false, &submitted, &retired, nullptr, nullptr) != kIOReturnSuccess)
        return true;
    return static_cast<int32_t>(retired - fence) >= 0;
}

void NativeGopDisplay::copyThreadMain(void *owner, wait_result_t)
{
    static_cast<NativeGopDisplay *>(owner)->copyLoop();
    thread_terminate(current_thread());
}

// The copy thread: takes the waiting job (busy with it under the lock, so a swap that takes its surfaces off the screen waits), waits
// for its GPU fence, draws it, and when a swap waits for it tells the work loop.
void NativeGopDisplay::copyLoop()
{
    IOLockLock(fCopyLock);
    while (!fCopyStop) {
        Job job;
        if (!gopcompose::takeJob(fSlot, fBusy, &job)) {
            IOLockSleep(fCopyLock, &fSlot, THREAD_UNINT);
            continue;
        }
        uint64_t waited = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - fCopyRequestTime, &waited);
        IOLockUnlock(fCopyLock);
        if (waited > 20000000) {
            fWakeLate++;
            if (waited > fWakeMaxNs)
                fWakeMaxNs = waited;
        }
        bool answered = false;   // a GPU that does not answer has no fence to go by: drawn as without fences
        const bool retired = !job.fenced || waitGpuFence(job.fence, &answered) || !answered;
        const bool covered = job.fenced && answered && retired;
        bool flipped = false;
        runJob(job, retired, covered, &flipped);
        __atomic_store_n(&fLastFlipped, flipped, __ATOMIC_RELEASE);
        IOLockLock(fCopyLock);
        uint64_t done = 0;
        const bool signal = gopcompose::finishJob(fBusy, &done, fWaitSerial);
        fDoneSerial = done;
        IOLockUnlock(fCopyLock);
        releaseJob(job);
        if (signal && fJobDone)
            fJobDone->interruptOccurred(nullptr, nullptr, 0);
        IOLockLock(fCopyLock);
    }
    fCopyThreadDone = true;
    IOLockWakeup(fCopyLock, &fCopyThreadDone, false);
    IOLockUnlock(fCopyLock);
}

// The kernel address of a surface's memory, mapped once for the next jobs. *cached: the mapping is cacheable (copy mode bit 1).
const uint8_t *NativeGopDisplay::surfaceBytes(IOSurface *surface, IOMemoryDescriptor *memory, bool *cached)
{
    SurfaceMap *slot = &fSurfaceMaps[0];
    for (SurfaceMap &entry : fSurfaceMaps) {
        if (entry.surface == surface && entry.map->getMemoryDescriptor() == memory) {
            entry.used = ++fSurfaceMapClock;
            *cached = (entry.map->getMapOptions() & kIOMapCacheMask) == kIOMapCopybackCache;
            return reinterpret_cast<const uint8_t *>(entry.map->getVirtualAddress());
        }
        if (entry.used < slot->used)
            slot = &entry;
    }
    if (slot->surface)
        dropSurfaceMap(*slot);
    if (memory->prepare() != kIOReturnSuccess)
        return nullptr;
    IOMemoryMap *map = nullptr;
    if (fCopyMode & kCopyCachedRead) {   // native-gop-display-copy bit 1: cacheable, invalidated before each read (compose)
        map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapCopybackCache);
        if (!map && !fCachedMapRefused) {
            fCachedMapRefused = true;
            GOPLOG("a cacheable mapping of a %llux%llu surface was refused: it is read uncached\n", ioSurfaceWidth(surface),
                  ioSurfaceHeight(surface));
        }
    }
    if (!map)
        map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    if (!map) {
        memory->complete();
        return nullptr;
    }
    reinterpret_cast<OSObject *>(surface)->retain();
    *slot = {surface, map, ++fSurfaceMapClock};
    *cached = (map->getMapOptions() & kIOMapCacheMask) == kIOMapCopybackCache;
    return reinterpret_cast<const uint8_t *>(map->getVirtualAddress());
}

// Copy mode bit 1: before a cacheable read of memory others write (the GPU through the SMMU, not snooping the CPU's caches), every
// cache line of the range is cleaned and invalidated to the point of coherency by VA (DC CIVAC: a read-permission operation, so a
// read-only mapping takes it; this kernel is not built IO-coherent, so IOKit's incoherent-IO flush would be a set/way flush of the
// whole cache above its bulk size). Lines this alias never dirtied are only dropped; the next loads fetch what memory holds. It runs
// after the job's fence wait, over exactly the rows (or row spans) the job reads; one barrier after the last range.
static void invalidateForRead(const uint8_t *start, uint64_t length, bool barrier = true)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uint64_t line = 4ull << ((ctr >> 16) & 15);   // DminLine: the smallest data cache line
    const uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
    for (uintptr_t p = reinterpret_cast<uintptr_t>(start) & ~(line - 1); p < end; p += line)
        __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
    if (barrier)
        __asm__ volatile("dsb sy" ::: "memory");
}

// The rows [y0, y1) and columns [x0, x1) of a source (its own coordinates, clipped to it) that a composition reads.
static void invalidateRows(const gopcompose::RowSource &source, int64_t x0, int64_t x1, int64_t y0, int64_t y1)
{
    x0 = gopcompose::max64(x0, 0);
    y0 = gopcompose::max64(y0, 0);
    x1 = gopcompose::min64(x1, source.width);
    y1 = gopcompose::min64(y1, source.height);
    if (x1 <= x0 || y1 <= y0)
        return;
    const unsigned pixelBytes = gopcompose::bytesPerPixel(source.format);
    if (x0 == 0 && uint64_t(x1) == source.width) {   // whole rows: one range
        invalidateForRead(source.bytes + uint64_t(y0) * source.rowBytes,
                          uint64_t(y1 - y0 - 1) * source.rowBytes + uint64_t(x1) * pixelBytes, false);
        return;
    }
    for (int64_t y = y0; y < y1; y++)
        invalidateForRead(source.bytes + uint64_t(y) * source.rowBytes + uint64_t(x0) * pixelBytes, uint64_t(x1 - x0) * pixelBytes, false);
}

void NativeGopDisplay::stopCopies()
{
    if (!fCopyLock)
        return;
    IOLockLock(fCopyLock);
    fCopyStop = true;
    IOLockWakeup(fCopyLock, &fSlot, true);
    while (!fCopyThreadDone)
        IOLockSleep(fCopyLock, &fCopyThreadDone, THREAD_UNINT);
    const bool left = fSlot.busy;
    const Job job = fSlot.job;
    fSlot = gopcompose::Slot();
    IOLockUnlock(fCopyLock);
    if (left)
        releaseJob(job);
    IOLockFree(fCopyLock);
    fCopyLock = nullptr;
}

// A kept map let go: its memory's prepare() completed (the map's descriptor is the one prepared), the map and the surface released.
void NativeGopDisplay::dropSurfaceMap(SurfaceMap &entry)
{
    if (IOMemoryDescriptor *memory = entry.map->getMemoryDescriptor())
        memory->complete();
    entry.map->release();
    reinterpret_cast<OSObject *>(entry.surface)->release();
    entry = {};
}

void NativeGopDisplay::releaseSurfaceMaps()
{
    for (SurfaceMap &entry : fSurfaceMaps)
        if (entry.surface)
            dropSurfaceMap(entry);
}

// The row buffers (the panel's width) and the conversion tables, once.
bool NativeGopDisplay::ensureBuffers()
{
    if (fLine)
        return true;
    fRowBytes = fVideo.v_width * 8;
    fRow = static_cast<uint8_t *>(IOMalloc(fRowBytes));
    fOverlayRow = static_cast<uint32_t *>(IOMalloc(fVideo.v_width * 4));
    fTables = static_cast<gopcompose::Tables *>(IOMalloc(sizeof(gopcompose::Tables)));
    uint32_t *line = static_cast<uint32_t *>(IOMalloc(fVideo.v_width * 4));
    if (!fRow || !fOverlayRow || !fTables || !line) {
        if (fRow)
            IOFree(fRow, fRowBytes);
        if (fOverlayRow)
            IOFree(fOverlayRow, fVideo.v_width * 4);
        if (fTables)
            IOFree(fTables, sizeof(gopcompose::Tables));
        if (line)
            IOFree(line, fVideo.v_width * 4);
        fRow = nullptr;
        fOverlayRow = nullptr;
        fTables = nullptr;
        GOPLOG("no memory for the row buffers\n");
        return false;
    }
    fTables->init();
    fLine = line;
    return true;
}

// A surface opened for one job: mapped (kept for the next jobs), prepared, its rows checked against its memory. base: layer 0 ('BGRA',
// 'RGhA', or 'b3a8''s colour plane), else layer 1 ('BGRA').
bool NativeGopDisplay::openSource(IOSurface *surface, gopcompose::RowSource *source, Mapped *mapped, bool base)
{
    const uint64_t width = ioSurfaceWidth(surface), height = ioSurfaceHeight(surface);
    uint64_t rowBytes = ioSurfaceBytesPerRow(surface), offset = ioSurfaceBaseOffset(surface);
    const uint32_t format = ioSurfacePixelFormat(surface);
    IOMemoryDescriptor *memory = ioSurfaceMemory(surface);
    if (base && format != fSurfaceFormat) {
        GOPLOG("surface %llux%llu row %llu offset %llu format %#x memory %p\n", width, height, rowBytes, offset, format, memory);
        for (uint32_t plane = 0, planes = ioSurfacePlaneCount(surface); plane < planes; plane++)
            GOPLOG(" plane %u: %llux%llu offset %llu row %llu element %llu\n", plane, ioSurfacePlaneWidth(surface, plane),
                  ioSurfacePlaneHeight(surface, plane), ioSurfacePlaneOffset(surface, plane), ioSurfacePlaneBytesPerRow(surface, plane),
                  ioSurfacePlaneBytesPerElement(surface, plane));
        fSurfaceFormat = format;
    }
    bool usable = base ? gopcompose::baseFormat(format) : format == gopcompose::kFormatBGRA;
    if (usable && format == gopcompose::kFormatB3A8) {   // the colour plane; alpha is not shown
        usable = ioSurfacePlaneCount(surface) >= 1 && ioSurfacePlaneBytesPerElement(surface, 0) == 4;
        if (usable) {
            offset = ioSurfacePlaneOffset(surface, 0);
            rowBytes = ioSurfacePlaneBytesPerRow(surface, 0);
        }
    }
    const uint64_t pixelBytes = gopcompose::bytesPerPixel(format);
    if (!memory || !usable || !width || !height || width > 16384 || height > 16384 || rowBytes < width * pixelBytes ||
        offset + (height - 1) * rowBytes + width * pixelBytes > memory->getLength()) {
        static unsigned refused;
        if (refused++ < 8)
            GOPLOG("%s surface %llux%llu format %#x row %llu offset %llu (memory %llu bytes) is not read\n", base ? "a frame" : "a cursor",
                  width, height, format, rowBytes, offset, memory ? static_cast<uint64_t>(memory->getLength()) : 0ull);
        return false;
    }
    bool cached = false;
    const uint8_t *bytes = surfaceBytes(surface, memory, &cached);   // mapped and prepared once, kept for the next jobs
    if (!bytes) {
        GOPLOG("cannot map a %llux%llu surface\n", width, height);
        return false;
    }
    *source = {bytes + offset, rowBytes, static_cast<uint32_t>(width), static_cast<uint32_t>(height), format};
    *mapped = {memory, cached};
    return true;
}

// A plan's rectangles into the boot framebuffer (the copy thread): per row, layer 0's part read in one run and converted (black where
// it has no pixel), layer 1's glyph blended over it, the row written at once (the framebuffer is write-combined). A plan for the whole
// panel is a frame: its time, statistics and thumbnail are the periodic line's; a cursor plan redraws the cursor's old and new
// rectangles only (the cursor line's time). A refresh stops at the next row when a swap asks (fAbortSerial): true then.
bool NativeGopDisplay::compose(const gopcompose::Plan &plan, const Job &job, IOSurface *base, IOSurface *cursor,
                               const gopcompose::Overlay &o, const Rect &onPanel)
{
    if (!fScanout || !plan.count || !ensureBuffers())
        return false;
    const bool full = plan.full, refresh = job.refresh;
    uint64_t partialLaps = 0;
    uint64_t &prepareNs = full ? fPrepareNs : partialLaps, &readNs = full ? fReadNs : partialLaps;
    uint64_t &convertNs = full ? fConvertNs : partialLaps, &writeNs = full ? fWriteNs : partialLaps;
    const uint64_t began = mach_absolute_time();
    uint64_t mark = began, now;
    auto lap = [&](uint64_t &total) {
        now = mach_absolute_time();
        uint64_t ns;
        absolutetime_to_nanoseconds(now - mark, &ns);
        total += ns;
        mark = now;
    };
    const bool first = full && !refresh && base && !fCopies && fSurfaceFormat == 0;
    gopcompose::RowSource baseRows, cursorRows;
    Mapped baseMap = {}, cursorMap = {};
    const bool haveBase = base && openSource(base, &baseRows, &baseMap, true);
    const bool haveCursor = cursor && o.visible && !onPanel.empty() && openSource(cursor, &cursorRows, &cursorMap, false);
    lap(prepareNs);
    if (baseMap.cached || cursorMap.cached) {   // native-gop-display-copy bit 1: the rows about to be read, from memory
        for (unsigned i = 0; i < plan.count; i++) {
            const Rect &r = plan.rect[i];
            if (haveBase && baseMap.cached)
                invalidateRows(baseRows, r.x, r.right(), r.y, r.bottom());
            const Rect c = gopcompose::intersect(r, onPanel);
            if (haveCursor && cursorMap.cached && !c.empty())
                invalidateRows(cursorRows, int64_t(c.x) - o.x, c.right() - o.x, int64_t(c.y) - o.y, c.bottom() - o.y);
        }
        __asm__ volatile("dsb sy" ::: "memory");
        lap(fInvalidateNs);
        fInvalidates++;
    }
    // Each new frame's statistics, and the thumbnail when one is due: the first new frame, then once every fThumbPeriod s, on a new
    // frame or on the refresh swap_begin_gated asked for. Both come from every kSampleStep-th pixel of every kSampleStep-th row as
    // written to the panel (after conversion and the cursor), so they describe what the panel shows. The sampling time is counted as
    // conversion.
    FrameStats stats = {};
    bool thumb = full && fThumbPeriod && (fThumbNext ? mach_absolute_time() >= fThumbNext : !refresh);
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
            GOPLOG("no memory for the thumbnail; thumbnails off\n");
            thumb = false;
        }
    }
    if (thumb) {
        bzero(fThumbLuma, kThumbCols * kThumbRows * sizeof(uint32_t));
        bzero(fThumbCount, kThumbCols * kThumbRows * sizeof(uint32_t));
    }
    const bool sample = full && (!refresh || thumb);
    const uint64_t width = fVideo.v_width, height = fVideo.v_height;
    const bool halfFloat = haveBase && baseRows.format == gopcompose::kFormatRGhA;
    bool aborted = false;
    for (unsigned i = 0; i < plan.count && !aborted; i++) {
        const Rect &r = plan.rect[i];
        const int32_t x0 = r.x, x1 = static_cast<int32_t>(r.right());
        for (int32_t y = r.y; y < r.bottom(); y++) {
            if (refresh && __atomic_load_n(&fAbortSerial, __ATOMIC_ACQUIRE) == job.serial) {
                aborted = true;
                break;
            }
            const uint32_t count = gopcompose::readBaseSpan(fLine, fRow, y, x0, x1, haveBase ? &baseRows : nullptr);
            lap(readNs);
            gopcompose::convertBaseSpan(fLine, fRow, x0, x1, count, haveBase ? &baseRows : nullptr, *fTables);
            if (haveCursor)
                gopcompose::blendOverlayRow(fLine, fOverlayRow, y, x0, x1, &cursorRows, o, onPanel);
            if (sample && y % kSampleStep == 0)
                sampleRow(fLine, halfFloat ? fRow : nullptr, halfFloat ? 8 : 4, static_cast<uint64_t>(y), width, height, stats, thumb);
            lap(convertNs);
            memcpy(fScanout + static_cast<uint64_t>(y) * fVideo.v_rowBytes + static_cast<uint64_t>(x0) * 4, fLine + x0,
                   static_cast<size_t>(x1 - x0) * 4);
            lap(writeNs);
        }
    }
    if (plan.damage && !refresh && !aborted && haveBase)   // a frame from its region: what it left out must still be on the panel
        auditDamage(plan, baseRows, baseMap.cached, onPanel);
    if (aborted)   // no statistics, no thumbnail (it is taken again)
        return true;
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    if (plan.damage) {   // a frame (or its refresh) from its damage: the damage line's figures
        if (refresh) {
            fDamageRefreshes++;
            return false;
        }
        fDamageCopies++;
        fDamageNs += ns;
        if (ns > fDamageMaxNs)
            fDamageMaxNs = ns;
        fDamagePixels += uint64_t(gopcompose::planArea(plan));
        return false;
    }
    if (!full) {
        fCursorJobs++;
        fCursorNs += ns;
        if (ns > fCursorMaxNs)
            fCursorMaxNs = ns;
        return false;
    }
    fCopies++;
    fCopyNs += ns;
    if (ns > fCopyMaxNs)
        fCopyMaxNs = ns;
    const uint32_t format = haveBase ? baseRows.format : 0;
    if (first && haveBase)
        GOPLOG("first copy %llux%llu format %#x into the framebuffer in %llu us\n", width, height, format, ns / 1000);
    if (thumb) {
        logThumbnail(stats, width, height, format, refresh);
        uint64_t period;
        nanoseconds_to_absolutetime(static_cast<uint64_t>(fThumbPeriod) * 1000000000ULL, &period);
        fThumbNext = mach_absolute_time() + period;
        fThumbRequested = false;
        if (fThumbLimit && ++fThumbsTaken >= fThumbLimit) {
            fThumbPeriod = 0;
            GOPLOG("%u thumbnails taken, no more this boot (native-gop-display-thumbs)\n", fThumbsTaken);
        }
    }
    if (refresh)
        return false;
    const uint64_t samples = stats.samples ? stats.samples : 1;
    fLastLitPerMille = stats.lit * 1000 / samples;
    fLastMean = (stats.red / samples) << 16 | (stats.green / samples) << 8 | stats.blue / samples;
    fLastCenter = stats.centre;
    if (haveBase && format != gopcompose::kFormatBGRA && (fFrames <= 8 || fFrames % 50 == 0))
        GOPLOG("frame %llu: %llu of %llu sampled pixels non-black, %llu nonzero in the source, centre #%06x\n", fFrames,
              stats.lit, stats.samples, stats.raw, stats.centre);
    return false;
}

// After a frame composed from its damage (the copy thread, its surface still prepared): kAuditPoints panel pixels outside the plan's
// rectangles and the cursor's old and new ones (GopCompose.h auditPoints) must show what the new frame has there, since its dirty
// region said nothing changed there. Each point's base pixel is read and converted as compose() does and compared with the boot
// framebuffer (one uncached load each way: some 20 us for the lot; copy mode bit 1 invalidates the pixel's line first). A difference
// sets fAuditFailedNow (runJob turns damage off).
void NativeGopDisplay::auditDamage(const gopcompose::Plan &plan, const gopcompose::RowSource &base, bool cached, const Rect &cursorNow)
{
    Rect exclude[5];
    unsigned excluded = 0;
    for (unsigned i = 0; i < plan.count && i < 3; i++)
        exclude[excluded++] = plan.rect[i];
    exclude[excluded++] = fPanel.cursor;   // not committed yet: where the cursor was drawn before this job
    exclude[excluded++] = cursorNow;
    const gopcompose::AuditPoints points = gopcompose::auditPoints(fAuditState, uint32_t(fVideo.v_width), uint32_t(fVideo.v_height),
                                                                  exclude, excluded);
    unsigned bad = 0;
    for (unsigned i = 0; i < points.count; i++) {
        const int32_t x = points.x[i], y = points.y[i];
        if (cached && uint32_t(x) < base.width && uint32_t(y) < base.height)   // copy mode bit 1: this pixel from memory, as the rows were
            invalidateForRead(base.bytes + uint64_t(y) * base.rowBytes + uint64_t(x) * gopcompose::bytesPerPixel(base.format),
                              gopcompose::bytesPerPixel(base.format));
        const uint32_t count = gopcompose::readBaseSpan(fLine, fRow, y, x, x + 1, &base);
        gopcompose::convertBaseSpan(fLine, fRow, x, x + 1, count, &base, *fTables);
        const uint32_t wanted = fLine[x];
        const uint32_t shown = reinterpret_cast<const volatile uint32_t *>(fScanout + uint64_t(y) * fVideo.v_rowBytes)[x];
        if (shown != wanted && !bad++) {
            fAuditX = x;
            fAuditY = y;
            fAuditShown = shown;
            fAuditWanted = wanted;
        }
    }
    fAuditSamples += points.count;
    fAuditMismatches += bad;
    if (bad) {
        fAuditFailedNow = true;
        fAuditBad = bad;
        fAuditCount = points.count;
    }
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
// one of " .:-=+*#%@" (dark to bright), as composed: the cursor is in it (since 2026-11-12).
void NativeGopDisplay::logThumbnail(const FrameStats &stats, uint64_t width, uint64_t height, uint32_t format, bool refresh)
{
    static const char kRamp[] = " .:-=+*#%@";
    const uint64_t samples = stats.samples ? stats.samples : 1, perMille = stats.lit * 1000 / samples;
    const uint64_t mean = (stats.red / samples) << 16 | (stats.green / samples) << 8 | stats.blue / samples;
    const char code[5] = {static_cast<char>(format >> 24), static_cast<char>(format >> 16), static_cast<char>(format >> 8),
                          static_cast<char>(format), 0};
    GOPLOG("thumb frame %llu%s: %llux%llu '%s', %llu.%llu%% non-black, mean #%06llx, centre #%06x, %ux%u cells of "
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
        GOPLOG("thumb %02u |%s|\n", row, text);
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
        // The serial number is the size: WindowServer keeps the chosen mode per display identity. LegacyManufacturerID is the EDID
        // vendor code of the three letters ("XMI": 24 << 10 | 13 << 5 | 9 = 0x61a9), what a panel's EDID gives IOMFB and what
        // CGDisplayVendorNumber returns (0 without it).
        if (!(appendString(product, "ManufacturerID", "XMI") && appendNumber(product, "LegacyManufacturerID", 0x61a9) &&
              appendNumber(product, "ProductID", fExternal ? pianodp::kFakeProductId : 0x8750) &&
              appendNumber(product, "SerialNumber", width << 16 | height) &&
              appendString(product, "ProductName", fExternal ? pianodp::kFakeProductName : "Xiaomi Pad 8 Pro") &&
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
        setProperty("IOMFBUUID", fExternal ? pianodp::kFakeUuid : "4E474F50-4449-5350-0000-000000008750");
        // External, display index 0: the attributes macOS 27's WindowServer accepted on the VM-era display (UPSTREAM). D6's display:
        // external too, "primary" as piano-dp-fake-primary says (default 0, the panel's value; what WindowServer makes of it is D6's
        // question). native-gop-display-builtin=1: the panel built-in instead (fBuiltin).
        setProperty("external", !fBuiltin);
        setProperty("primary", fExternal ? static_cast<unsigned long long>(fFake.primary) : 0ULL, 32);
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
        GOPLOG("createDisplayAttributes failed (%s)\n", done ? "set_digital_out_mode" : "an element or a property");
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
        GOPLOG("first swap submitted\n");
    return 1;
}
IOReturn NativeGopDisplay::swap_layer_map(IOMFBSwapIORequest *, unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int, IOSurface *) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::set_display_device(unsigned int) {return kIOReturnSuccess;}
IOReturn NativeGopDisplay::get_gamma_table(IOMFBGammaTable *) {return kIOReturnUnsupported;}

// piano-dpu-pcc: the gamma table's first 16 words logged when it changes (its layout is what B1d-0 is for) and the table handed to
// PianoDpu; with piano-dpu-pcc=2 PianoDpu reads 771 words (3 x 257, the 0xc0c-byte layout DpuPcc.h assumes) and fits a diagonal.
// Without the argument this is the stub it was (success, nothing done).
IOReturn NativeGopDisplay::set_gamma_table(IOMFBGammaTable *table)
{
    if (!fDpuPcc || !table)
        return kIOReturnSuccess;
    const uint32_t *w = reinterpret_cast<const uint32_t *>(table);
    const unsigned words = fDpuPcc >= 2 ? kGammaWords : 16;
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < words; i++)
        hash = (hash ^ w[i]) * 16777619u;
    fGammaCalls++;
    if (fGammaChanges && hash == fGammaHash)
        return kIOReturnSuccess;
    fGammaHash = hash;
    if (++fGammaChanges <= 8 || fGammaChanges % 100 == 0)
        GOPLOG("set_gamma_table #%u (call %u): %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x%s\n",
              fGammaChanges, fGammaCalls, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14],
              w[15], fDpuPcc >= 2 ? " (771 words handed over)" : "");
    IOService *dpu = dpuService(true);
    if (dpu)
        dpu->callPlatformFunction("PianoDpuGammaTable", false, const_cast<uint32_t *>(w), reinterpret_cast<void *>(uintptr_t(fDpuPcc >= 2 ? kGammaWords : 0)),
                                  nullptr, nullptr);
    return kIOReturnSuccess;
}
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
// piano-dpu-pcc: each new matrix of a function logged as its nine 64-bit words (taken to be S31.32, DpuPcc.h; B1d-0 checks that) and
// handed to PianoDpu's PCC; the same matrix again (it may come with every swap) is only counted. Without the argument this is the
// stub it was.
IOReturn NativeGopDisplay::swap_set_color_matrix(IOMFBColorFixedMatrix *matrix, IOMFBColorMatrixFunction function, unsigned int arg)
{
    if (!fDpuPcc || !matrix)
        return kIOReturnSuccess;
    uint64_t raw[9];
    memcpy(raw, matrix, sizeof(raw));
    const uint32_t f = static_cast<uint32_t>(function), slot = f % kColorFunctions;
    fColorCalls++;
    if ((fColorSeen >> slot & 1) && fColorFunction[slot] == f && !memcmp(fColorLast[slot], raw, sizeof(raw)))
        return kIOReturnSuccess;
    fColorSeen |= 1u << slot;
    fColorFunction[slot] = f;
    memcpy(fColorLast[slot], raw, sizeof(raw));
    if (++fColorChanges <= 16 || fColorChanges % 100 == 0)
        GOPLOG("swap_set_color_matrix #%u (call %u): function %u arg %#x: %016llx %016llx %016llx | %016llx %016llx %016llx | "
              "%016llx %016llx %016llx\n", fColorChanges, fColorCalls, f, arg, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
              raw[8]);
    IOService *dpu = dpuService(true);
    if (dpu)
        dpu->callPlatformFunction("PianoDpuColorMatrix", false, raw, reinterpret_cast<void *>(uintptr_t(f)), reinterpret_cast<void *>(uintptr_t(arg)),
                                  nullptr);
    return kIOReturnSuccess;
}

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

// UniXNU-piano (2026-11-11): the display's nub, here so the native collection needs no other display kext. IOMobileFramebuffer.framework
// counts the DisplayPortCount of the AppleParavirtGPU services (it matches the class name) when an /arm-io node is compatible
// "paravirtualizedgraphics,gpu" (make-piano-adt.py's /arm-io/gop-display). One display port, as the VM-era nub (UPSTREAM); two with
// D6's arguments.
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
    // D6 (piano-dp-fake, DpFake.h): a second display port for NativeGopDisplay's second instance, so WindowServer waits for both.
    const bool fake = fakeFromBootArgs().armed;
    setProperty("DisplayPortCount", fake ? 2ULL : 1ULL, 32);
    registerService();
    IOLog("NativeGopDisplay: AppleParavirtGPU nub on %s, %s\n", provider->getName(),
          fake ? "2 display ports (piano-dp-fake: the second is D6's RAM-backed test display)" : "1 display port");
    return true;
}
