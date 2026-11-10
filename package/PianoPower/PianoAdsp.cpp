// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano's audio DSP (ADSP, LPASS), which runs the charger firmware. UniXNU-sheng's AdspProbe.cpp (SM8550, hardware-verified there;
// UPSTREAM) with SM8750's addresses and three changes for the native k27 kernel collection: no interrupts (it has no SPI attach, so
// the IPCC doorbells and the RSC done bit are polled from the DSP thread), the secure calls through Sm8750Scm (Adreno830, MIT: the
// SM8750 wait-queue protocol, verified there for the GPU's PAS), and a console line at every stage.
//
// piano-power=1 runs only piano_adsp_survey: it reads what the boot stages left (the SMEM partition and items, the DSP's SMP2P state,
// the AOSS mailbox, the RSC id) and writes nothing. piano-power=2 runs piano_adsp_start: the DSP's images from modem_a, then the
// bring-up order of sheng: our SMP2P item, the doorbells, the AOSS load_state "adsp", the lcx/lmx proxy votes, the DTB image (PAS 36)
// and the DSP image (PAS 1), "ready" on SMP2P, then GLINK and the IPC router.
//
// Devicetree facts (piano's live DT, out/inputs/piano-boot.dtb): remoteproc@6800000 (qcom,sm8750-adsp-pas), firmware
// qcom/sm8750/piano/adsp.mdt and adsp_dtb.mdt (from modem_a image/, split into .bNN), carveouts adspslpi 0xb8200000 + 0x5780000 and
// q6-adsp-dtb 0x9ed80000 + 0x80000 (both outside XNU's RAM window at 0x8c0000000); proxy power domains lcx and lmx and the AOSS
// load_state "adsp"; SMEM 0x81d00000 + 0x200000 with TCSR mutex 3 (hwlock@1f40000, stride 0x1000); smp2p-adsp items 443 (in) and 429
// (out), remote pid 2, entries "master-kernel" (bit 0 stop) and "slave-kernel" (fatal 0, ready 1, handover 2, stop ack 3); IPCC
// mailbox@406000: LPASS client 3 with signal 0 GLINK and 2 SMP2P, AOP client 0 signal 0; GLINK items 478 (descriptor), 479 (our FIFO),
// 480 (the DSP's FIFO); AOSS message RAM 0xc300000 + 0x400; apps RSC drv-2 at 0x16520000, tcs-offset 0xd00, active TCSs 0-2
// (tcs-config); cmd-db 0x81c60000 + 0x20000. PAS ids 1 and 36 (0x24) as Linux's SM8750 ADSP resource and sheng.
//
// Design (sheng's): one kernel thread owns every step and all protocol state. The thread waits on an event with a deadline for each
// step (QMP 1 s per phase, RPMh 100 ms, DSP ready 5 s) and polls the doorbells every few milliseconds while it waits; afterwards it
// sleeps between doorbells (polled every 20 ms) and client sends. No lock is held across a wait. Clients (PianoAdsp.h) register
// channels and queue sends from any thread; their callbacks run on this thread. SMEM is read only inside the APPS partitions and the
// global one (other partitions may be closed to the HLOS: a whole-SMEM read on the 13T faulted).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "Sm8750Scm.h"
#include "PianoAdsp.h"
}

namespace {
// IPCC (the inter-processor doorbells; UniXNU-sheng QcomScm.h): a doorbell id is client << 16 | signal.
constexpr uint32_t kIpccSend = 0x0c, kIpccRecvId = 0x10, kIpccRecvEnable = 0x14, kIpccRecvClear = 0x1c, kIpccNone = 0xffffffffu;
constexpr uint32_t kClientAop = 0, kClientLpass = 3, kSignalGlinkQmp = 0, kSignalSmp2p = 2;
constexpr uint32_t ipccId(uint32_t client, uint32_t signal) { return client << 16 | (signal & 0xffff); }

constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x406000, kAoss = 0xc300000;
constexpr uint64_t kRscDrv = 0x16520000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kAdspBase = 0xb8200000, kAdspSize = 0x5780000, kDtbBase = 0x9ed80000, kDtbSize = 0x80000;
constexpr uint32_t kPasAdsp = 1, kPasDtb = 36, kRemotePid = 2, kItemIn = 443, kItemOut = 429, kItemCrash = 423;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvAop = 4, kEvRsc = 8, kEvSend = 16;
constexpr unsigned kClients = 4, kQueue = 8;
constexpr uint32_t kBringUpSliceMs = 2, kSteadySliceMs = 20;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}

// ---- status for the owner's report ----
volatile int32_t gStage, gError, gSlaveBits = -1;
volatile uint32_t gDoorbells, gReadyMs, gAdspKib;
int64_t gScmX0;
uint64_t gScmX1;
char gText[96];

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoPower: adsp: %s\n", line);
}
void stage(int value) {gStage = value; say("stage %d: %s", value, piano_adsp_stage_name(value));}
void fail(int step, int error) {
    gError = error;
    gStage = -step;
    say("FAILED at stage %d (%s): error %d%s%s", step, piano_adsp_stage_name(step), error, gText[0] ? ", " : "", gText);
}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof gText - 1; ++at) gText[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof gText - 1; ++i) gText[at++] = text[i];
    gText[at] = 0;
}

Window gSmemW, gMutex, gIpcc, gAoss, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart;
bool gHavePart;
QcomSmp2p gP2p;
int gStopEntry = -1;
QcomQmp gQmp;
QcomGlink gGlink;
bool gGlinkStarted;
QcomRscLayout gLayout;
uint32_t gRscIrqBits;
volatile uint32_t gPending, gRscDone;
uint32_t gSliceMs = kBringUpSliceMs;
bool gDoorbellsOn;

struct Message {uint32_t length; uint8_t data[PIANO_ADSP_MAX_MESSAGE];};
}  // namespace

struct PianoAdspChannel {
    char name[QCOM_GLINK_NAME];
    uint32_t intent_size, intent_count;
    int initiator;
    PianoAdspClient client;
    void *context;
    uint8_t *storage;
    QcomGlinkChannel *glink;
    Message *queue;  // kQueue slots
    uint32_t head, count;
};

namespace {
PianoAdspChannel gClients[kClients];
uint32_t gClientCount;
IOLock *gClientLock;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

// ---- doorbells, polled: acknowledge, record ----
uint32_t pollHardware() {
    uint32_t bits = 0;
    if (gDoorbellsOn) {
        for (unsigned n = 0; n < 32; ++n) {
            const uint32_t id = rd(gIpcc, kIpccRecvId);
            if (id == kIpccNone) break;
            wr(gIpcc, kIpccRecvClear, id);
            if (id == ipccId(kClientLpass, kSignalSmp2p)) bits |= kEvSmp2p;
            else if (id == ipccId(kClientLpass, kSignalGlinkQmp)) bits |= kEvGlink;
            else if (id == ipccId(kClientAop, kSignalGlinkQmp)) bits |= kEvAop;
        }
        if (bits) gDoorbells = gDoorbells + 1;
    }
    if (gRscIrqBits) {
        const uint32_t status = rd(gRsc, kRscTcs + gLayout.irq_status) & gRscIrqBits;
        if (status) {
            wr(gRsc, kRscTcs + gLayout.irq_clear, status);
            __atomic_fetch_or(&gRscDone, status, __ATOMIC_RELEASE);
            bits |= kEvRsc;
        }
    }
    return bits;
}

// Waits for an event up to ms milliseconds, looking at the doorbells every slice; returns the event bits (0: the deadline passed).
uint32_t waitEvents(uint32_t ms) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    for (;;) {
        uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL) | pollHardware();
        if (bits) return bits;
        if (mach_absolute_time() >= deadline) return 0;
        assert_wait_timeout((event_t)&gPending, THREAD_UNINT, gSliceMs < ms ? gSliceMs : ms, kMillisecondScale);
        bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
        if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
        thread_block(THREAD_CONTINUE_NULL);
    }
}

void ring(uint32_t client, uint32_t signal) {wr(gIpcc, kIpccSend, ipccId(client, signal));}
void kickAop(void *) {ring(kClientAop, kSignalGlinkQmp);}
void kickGlink(void *) {ring(kClientLpass, kSignalGlinkQmp);}
void kickSmp2p() {ring(kClientLpass, kSignalSmp2p);}

// ---- SMEM helpers (allocation under the TCSR hardware mutex) ----
bool lockSmem() {
    // The mutex is held only for the few writes of one allocation by whichever processor has it; it is tried a bounded number of times.
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return true;
        IODelay(10);
    }
    return false;
}
void unlockSmem() {wr(gMutex, 0, 0);}
int allocItem(uint16_t item, uint32_t size) {
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, item, size);
    unlockSmem();
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (!gHavePart || qcom_smem_find(&gSmem, &gPart, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}

void readCrashReason(char *out, size_t capacity) {
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    out[0] = 0;
    if (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gSmem, &global, kItemCrash, &at, &size) || !size)
        return;
    size_t n = 0;
    for (; n < capacity - 1 && n < size; ++n) {const char c = (char)gSmemW.va[at + n]; if (!c) break; out[n] = c >= 32 && c < 127 ? c : '.';}
    out[n] = 0;
}
void noteCrash() {
    char text[80];
    readCrashReason(text, sizeof text);
    if (text[0]) {note("crash: ", text); say("crash reason: %s", text);}
}

bool mapAll() {
    int which = 0;
    if (!mapWindow(gSmemW, kSmem, kSmemSize)) which = 1;
    else if (!mapWindow(gMutex, kMutex, 0x1000)) which = 2;
    else if (!mapWindow(gIpcc, kIpcc, 0x1000)) which = 3;
    else if (!mapWindow(gAoss, kAoss, 0x1000)) which = 4;
    else if (!mapWindow(gRsc, kRscDrv, 0x10000)) which = 5;
    if (which) {gError = which; return false;}
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    gLayout = qcom_rsc_layout(rd(gRsc, 0));
    return true;
}

// ---- the survey: reads only ----
void survey(PianoAdspSurvey *s) {
    *s = {};
    s->slave = -1;
    s->partition = qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart);
    gHavePart = s->partition == 0;
    if (gHavePart) {
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) s->items |= 1u << i;
    }
    QcomSmemPartition global;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global)) s->items |= 1u << 9;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) s->slave = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    gSlaveBits = s->slave;
    const uint32_t up = 0xffff;
    s->aoss = (rd(gAoss, 0) == 0x4d41494cu ? 1u : 0u) << 12 | (uint32_t)(rd(gAoss, 0xc) == up) << 8 | (uint32_t)(rd(gAoss, 0x24) == up) << 9 |
              (uint32_t)(rd(gAoss, 0x14) == up) << 10 | (uint32_t)(rd(gAoss, 0x2c) == up) << 11;
    s->rsc_id = rd(gRsc, 0);
    readCrashReason(s->crash, sizeof s->crash);
}

void service(uint32_t bits);

// ---- RPMh proxy votes ----
bool readCmdDb(uint8_t *copy) {
    Window db = {};
    if (!mapWindow(db, kCmdDb, kCmdDbSize)) return false;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);}
    unmap(db);
    return true;
}
QcomRpmhResource gLcx, gLmx;
int gTcs = -1;
uint32_t gVotes;  // lcx | lmx << 8 | (1 << 15: a request whose done bit was not seen)

// Votes both rails to the given ARC index pair through active TCS 1 and waits up to 100 ms for its done bit (polled). The active TCSs
// are 0 to 2 (tcs-config lists the active type first); nothing else in the native kernel sends through this RSC. A request whose done
// bit is not seen still counts (bit 15 of the votes word), as on sheng.
int vote(uint32_t lcx, uint32_t lmx) {
    if (gTcs < 0) {
        gTcs = 1;
        wr(gRsc, kRscTcs + 0, rd(gRsc, kRscTcs + 0) | (1u << gTcs));  // IRQ_ENABLE for our TCS: the done bit latches in the status
        gRscIrqBits = 1u << gTcs;
    }
    const uint32_t addresses[2] = {gLcx.address, gLmx.address}, values[2] = {lcx, lmx};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&gLayout, (unsigned)gTcs, addresses, values, 2, writes, 16);
    if (!n) return -22;
    __atomic_store_n(&gRscDone, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    uint64_t deadline;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) && mach_absolute_time() < deadline) service(waitEvents(20));
    const bool done = (__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) != 0;
    const unsigned m = qcom_rpmh_finish(&gLayout, (unsigned)gTcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    gVotes = lcx | lmx << 8 | (done ? 0 : 1u << 15);
    say("rails: lcx index %u, lmx index %u%s", lcx, lmx, done ? "" : " (done bit not seen)");
    return 0;
}

// ---- secure world calls (Sm8750Scm: INTERRUPTED, WAITQ_SLEEP polled, EBUSY) ----
Sm8750Scm gScm;
IOLock *gScmLock;
uint64_t nowUs(void *) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
void sleepUs(void *, uint32_t us) {if (us >= 1000) IOSleep(us / 1000); else IODelay(us);}
void scmLock(void *) {IOLockLock(gScmLock);}
void scmUnlock(void *) {IOLockUnlock(gScmLock);}
const Sm8750ScmOps gScmOps = {nullptr, Sm8750ScmSmcInstruction, nowUs, sleepUs, scmLock, scmUnlock, nullptr, nullptr};
bool scmDone(Sm8750ScmStatus status, const Sm8750SmcResult &r, const char *what) {
    gScmX0 = r.x0;
    gScmX1 = r.x1;
    if (status == Sm8750ScmOk) return true;
    say("%s: status %d x0 %lld x1 %#llx (calls %u, waitq %u, busy %u)", what, (int)status, (long long)r.x0, (unsigned long long)r.x1,
        r.calls, r.waitq, r.busy);
    return false;
}
void shutdownPas(uint32_t pas) {
    const uint64_t args[4] = {pas, 0, 0, 0};
    Sm8750SmcResult r = {};
    const Sm8750ScmStatus s = Sm8750ScmCall(&gScm, SM8750_SCM_FID(0, Sm8750ScmSvcPil, 0x06), SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, &r);
    say("PAS %u shut down: status %d x0 %lld", pas, (int)s, (long long)r.x0);
}

void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- a firmware image: parsed from its file, metadata handed over, segments written into the carveout ----
struct Image {  // plain data, zero at start: the two live in static storage (a few KiB of header table each, kept off the stack)
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
};
Image gDtbImage, gAdspImage;
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return piano_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) piano_fw_close(image.file);
    if (image.meta) image.meta->release();
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = piano_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    return rc ? 1000 - rc : 0;
}
// init_image with the metadata in physically contiguous memory, then the memory setup of a relocatable image.
int authenticateHeader(Image &image, uint32_t pas, uint64_t base) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
                                                                 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    Sm8750SmcResult r = {};
    if (!scmDone(Sm8750ScmPasInitImage(&gScm, pas, phys, &r), r, "PAS init_image")) return -1;
    if (image.mdt.relocatable && !scmDone(Sm8750ScmPasMemSetup(&gScm, pas, base, qcom_mdt_span(&image.mdt), &r), r, "PAS mem_setup"))
        return -2;
    return 0;
}
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    Window region = {};
    if (!mapWindow(region, base, size, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (piano_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    if (value != gSlave) say("slave-kernel bits %#x", value);
    gSlave = value;
    gSlaveBits = (int32_t)value;
    if ((changed & value & kFatal)) noteCrash();
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        say("handover: the DSP holds its own votes now");
        vote(0, 0);
    }
}

void applyClients();
void pumpSends();
uint32_t gUnknownSeen;
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) {
            qcom_glink_set_rx(&gGlink, rx, size);
            say("GLINK: the DSP's FIFO is there (%u bytes)", size);
        }
    }
    const bool agreed = gGlink.version_agreed;
    qcom_glink_poll(&gGlink);
    pumpSends();
    if (!agreed && gGlink.version_agreed) say("GLINK: version agreed (features %#x)", gGlink.features);
    if (gGlink.unknown_opens != gUnknownSeen) {
        gUnknownSeen = gGlink.unknown_opens;
        if (strncmp(gText, "crash: ", 7)) note("open: ", gGlink.unknown_names);
    }
    if (gGlink.version_agreed && gStage >= PianoAdspReady && gStage < PianoAdspGlink) stage(PianoAdspGlink);
    uint32_t up = 0;
    for (unsigned i = 0; i < gClientCount; ++i) if (gClients[i].glink && gClients[i].glink->up) up |= 1u << i;
    if (up && gStage >= PianoAdspGlink && gStage < PianoAdspChannelUp) stage(PianoAdspChannelUp);
}
void service(uint32_t bits) {
    if (bits & kEvAop) qcom_qmp_step(&gQmp);
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (bits & (kEvGlink | kEvSend)) {applyClients(); serviceGlink();}
}
// Runs events until done() holds or ms pass; returns done().
template <typename F> bool waitUntil(uint32_t ms, F done) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!done()) {
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) return done();
        uint64_t left = 0;
        absolutetime_to_nanoseconds(deadline - now, &left);
        service(waitEvents((uint32_t)(left / 1000000) + 1));
    }
    return true;
}

// ---- client glue (DSP thread side) ----
void clientState(void *context, QcomGlinkChannel *, int up) {
    PianoAdspChannel *c = static_cast<PianoAdspChannel *>(context);
    say("channel %s %s", c->name, up ? "up" : "down");
    if (c->client.state) c->client.state(c->context, up);
}
void clientReceive(void *context, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    PianoAdspChannel *c = static_cast<PianoAdspChannel *>(context);
    if (c->client.receive) c->client.receive(c->context, data, length);
}
void applyClients() {
    if (!gGlinkStarted) return;
    const QcomGlinkClient glue = {clientState, clientReceive};
    IOLockLock(gClientLock);
    for (unsigned i = 0; i < gClientCount; ++i) {
        PianoAdspChannel &c = gClients[i];
        if (!c.glink) c.glink = qcom_glink_channel(&gGlink, c.name, c.intent_size, c.intent_count, c.storage, &glue, &c, c.initiator);
    }
    IOLockUnlock(gClientLock);
}
void pumpSends() {
    for (unsigned i = 0; i < gClientCount; ++i) {
        PianoAdspChannel &c = gClients[i];
        if (!c.glink || !c.glink->up) continue;
        for (;;) {
            IOLockLock(gClientLock);
            if (!c.count) {IOLockUnlock(gClientLock); break;}
            Message *m = &c.queue[c.head];
            IOLockUnlock(gClientLock);
            // Only this thread takes messages off the queue, so m stays valid while the lock is dropped for the send.
            const int rc = qcom_glink_send(&gGlink, c.glink, m->data, m->length);
            if (rc != QcomGlinkOk) break;
            IOLockLock(gClientLock);
            c.head = (c.head + 1) % kQueue;
            c.count--;
            IOLockUnlock(gClientLock);
        }
    }
}

// ---- the bring-up ----
void enableDoorbells() {
    const uint32_t enable[3] = {ipccId(kClientLpass, kSignalSmp2p), ipccId(kClientLpass, kSignalGlinkQmp), ipccId(kClientAop, kSignalGlinkQmp)};
    for (uint32_t id : enable) wr(gIpcc, kIpccRecvEnable, id);
    gDoorbellsOn = true;
}

// The firmware comes from the device's own firmware partition ("modem:", modem_a then modem_b, read in place; piano keeps adsp.mdt and
// adsp_dtb.mdt with their .bNN pieces in image/), or with piano-power-fw=DIR from DIR/adsp.mbn and DIR/adsp_dtb.mbn on a mounted volume.
// The UFS driver may not be up when this thread starts: a few tries with growing pauses (about two minutes in all), not a poll.
int openPair(Image &dtb, Image &adsp, const char *source, char *path, size_t size) {
    const bool modem = !strcmp(source, "modem:");
    snprintf(path, size, modem ? "%sadsp_dtb" : "%s/adsp_dtb.mbn", source);
    int rc = openImage(dtb, path);
    if (rc) return rc;
    snprintf(path, size, modem ? "%sadsp" : "%s/adsp.mbn", source);
    if ((rc = openImage(adsp, path))) release(dtb);
    return rc;
}
int openFirmware(Image &dtb, Image &adsp, char *path, size_t size, const char *chosen) {
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    const char *source = chosen ? chosen : "modem:";
    for (unsigned i = 0;; ++i) {
        const int rc = openPair(dtb, adsp, source, path, size);
        if (!rc) return 0;
        if (rc != ENOENT && rc != ENXIO && rc != EIO) return rc;  // found but unusable
        if (i == sizeof pauses / sizeof pauses[0]) return rc;
        if (!i) say("firmware not readable yet (%s: %d); trying again for about two minutes", path, rc);
        IOSleep(pauses[i] * 1000);
    }
}

void shutdownDsp(bool dtb, bool adsp) {
    if (adsp) shutdownPas(kPasAdsp);
    if (dtb) shutdownPas(kPasDtb);
}

bool start() {
    char dir[80] = "";
    const bool chosen = PE_parse_boot_argn("piano-power-fw", dir, sizeof dir) && dir[0];
    char path[112];
    Image &dtb = gDtbImage, &adsp = gAdspImage;
    int rc = openFirmware(dtb, adsp, path, sizeof path, chosen ? dir : nullptr);
    if (rc) {note("fw: ", path); fail(PianoAdspFirmware, rc); return false;}
    gAdspKib = (uint32_t)(adsp.size >> 10);
    say("firmware: %s (%u KiB, %u segments; DTB image %u segments)", path, gAdspKib, adsp.mdt.count, dtb.mdt.count);
    stage(PianoAdspFirmware);
    if (gSlaveBits > 0) {  // running already: left alone
        note("", "the DSP is running already");
        fail(PianoAdspFirmware, -114); release(dtb); release(adsp); return false;
    }

    // Our SMP2P item, before the DSP starts (it looks for it at boot).
    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(PianoAdspSmp2p, rc); release(dtb); release(adsp); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    gStopEntry = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    stage(PianoAdspSmp2p);
    enableDoorbells();
    kickSmp2p();
    stage(PianoAdspDoorbells);

    qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
    for (int phase = 0; phase < 4 && qcom_qmp_step(&gQmp) > 0 && gQmp.state != QcomQmpOpen; ++phase)
        waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;});
    if (gQmp.state != QcomQmpOpen) {fail(PianoAdspLoadState, gQmp.state); release(dtb); release(adsp); return false;}
    qcom_qmp_send(&gQmp, "{class: image, res: load_state, name: adsp, val: on}");
    if (!waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen;})) {
        qcom_qmp_abandon(&gQmp);
        fail(PianoAdspLoadState, -60); release(dtb); release(adsp); return false;
    }
    stage(PianoAdspLoadState);

    // A copy of the command database, held only while the levels are looked up.
    struct CmdDbCopy {uint8_t *bytes = static_cast<uint8_t *>(IOMallocData(kCmdDbSize)); ~CmdDbCopy() {if (bytes) IOFreeData(bytes, kCmdDbSize);}} copy;
    uint8_t *const db = copy.bytes;
    uint32_t lcx = 0, lmx = 0;
    if (!db || !readCmdDb(db) || qcom_cmd_db_find(db, kCmdDbSize, "lcx.lvl", &gLcx) || qcom_cmd_db_find(db, kCmdDbSize, "lmx.lvl", &gLmx) ||
        qcom_rpmh_level_index(&gLcx, 0xffffffffu, &lcx) || qcom_rpmh_level_index(&gLmx, 0xffffffffu, &lmx)) {
        fail(PianoAdspRails, -2); release(dtb); release(adsp); return false;
    }
    if ((rc = vote(lcx, lmx))) {fail(PianoAdspRails, rc); release(dtb); release(adsp); return false;}
    stage(PianoAdspRails);

    // The DSP's devicetree image first, as its own authenticated image, then the DSP image.
    Sm8750SmcResult r = {};
    if ((rc = authenticateHeader(dtb, kPasDtb, kDtbBase)) || (rc = loadSegments(dtb, kDtbBase, kDtbSize)) ||
        (rc = scmDone(Sm8750ScmPasAuthAndReset(&gScm, kPasDtb, &r), r, "PAS auth_and_reset (DTB)") ? 0 : -3)) {
        fail(PianoAdspDtb, rc); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(PianoAdspDtb);
    if ((rc = authenticateHeader(adsp, kPasAdsp, kAdspBase)) || (rc = loadSegments(adsp, kAdspBase, kAdspSize))) {
        fail(PianoAdspLoaded, rc); shutdownDsp(true, false); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(PianoAdspLoaded);
    const uint64_t began = mach_absolute_time();
    if (!scmDone(Sm8750ScmPasAuthAndReset(&gScm, kPasAdsp, &r), r, "PAS auth_and_reset (DSP)")) {
        fail(PianoAdspStarted, -3); shutdownDsp(true, false); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(PianoAdspStarted);
    const bool ready = waitUntil(5000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    gReadyMs = (uint32_t)(ns / 1000000);
    release(dtb);
    release(adsp);  // the metadata may go once the images are authenticated
    if (!ready || (gSlave & kFatal)) {
        noteCrash();
        fail(PianoAdspReady, ready ? -5 : -60);
        shutdownDsp(true, true);
        if (!gHandedOver) vote(0, 0);
        return false;
    }
    say("ready after %u ms", gReadyMs);
    stage(PianoAdspReady);
    return true;
}

bool startGlink() {
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(PianoAdspGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    // Descriptor words: our FIFO's tail and head, then the DSP's FIFO's tail and head.
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    gGlinkStarted = true;
    say("GLINK: our FIFO %u bytes, version sent", size);
    applyClients();
    serviceGlink();
    return true;
}

void adspThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "piano-adsp");
    if (!gStage) {
        if (!mapAll()) {fail(PianoAdspMapped, gError); return;}
        stage(PianoAdspMapped);
        PianoAdspSurvey s;
        survey(&s);
    }
    stage(PianoAdspSurveyed);
    if (!start() || !startGlink()) return;
    gSliceMs = kSteadySliceMs;
    // From here on the thread only answers doorbells and client sends; while a send waits for an intent or FIFO room it looks again
    // every second.
    for (;;) {
        bool pending = false;
        for (unsigned i = 0; i < gClientCount; ++i) pending |= gClients[i].count != 0;
        service(waitEvents(pending ? 1000 : 3600u * 1000u) | (pending ? kEvSend : 0));
    }
}

bool gStarted;
}  // namespace

extern "C" int piano_adsp_survey(PianoAdspSurvey *out) {
    if (!mapAll()) {*out = {}; out->mapped = gError; out->slave = -1; return -1;}
    if (!gStage) gStage = PianoAdspMapped;
    survey(out);
    return 0;
}

extern "C" PianoAdspChannel *piano_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                                const PianoAdspClient *client, void *context) {
    if (!gClientLock) gClientLock = IOLockAlloc();
    if (!gClientLock || !name || !client || !intent_size || !intent_count || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS) return nullptr;
    uint8_t *storage = static_cast<uint8_t *>(IOMallocData((vm_size_t)intent_size * intent_count));
    Message *queue = static_cast<Message *>(IOMallocData(sizeof(Message) * kQueue));
    IOLockLock(gClientLock);
    PianoAdspChannel *c = gClientCount < kClients && storage && queue ? &gClients[gClientCount] : nullptr;
    if (c) {
        bzero(c, sizeof *c);
        strlcpy(c->name, name, sizeof c->name);
        c->intent_size = intent_size; c->intent_count = intent_count; c->initiator = initiator;
        c->client = *client; c->context = context; c->storage = storage; c->queue = queue;
        __atomic_store_n(&gClientCount, gClientCount + 1, __ATOMIC_RELEASE);
    }
    IOLockUnlock(gClientLock);
    if (!c) {
        if (storage) IOFreeData(storage, (vm_size_t)intent_size * intent_count);
        if (queue) IOFreeData(queue, sizeof(Message) * kQueue);
        return nullptr;
    }
    wake(kEvSend);
    return c;
}

extern "C" int piano_adsp_send(PianoAdspChannel *c, const void *data, uint32_t length) {
    if (!c || !data || !length) return -2;
    if (length > PIANO_ADSP_MAX_MESSAGE) return -1;
    IOLockLock(gClientLock);
    if (c->count == kQueue) {IOLockUnlock(gClientLock); return -1;}
    Message &m = c->queue[(c->head + c->count) % kQueue];
    memcpy(m.data, data, length);
    m.length = length;
    c->count++;
    IOLockUnlock(gClientLock);
    wake(kEvSend);
    return 0;
}

extern "C" void piano_adsp_start(void) {
    if (gStarted) return;
    gStarted = true;
    if (!gClientLock) gClientLock = IOLockAlloc();
    gScmLock = IOLockAlloc();
    if (!gClientLock || !gScmLock) {fail(PianoAdspMapped, -12); return;}
    Sm8750ScmInit(&gScm, &gScmOps);
    gScm.budget_us = 30u * 1000u * 1000u;  // a large image's authentication takes longer than the default second
    piano_qrtr_start();                    // the router link registers before the DSP can open it
    thread_t thread = nullptr;
    if (kernel_thread_start(adspThread, nullptr, &thread) != KERN_SUCCESS) {fail(PianoAdspMapped, -7); return;}
    thread_deallocate(thread);
}

extern "C" void piano_adsp_status(PianoAdspStatus *out) {
    *out = {};
    out->stage = gStage;
    out->error = gError;
    out->slave = gSlaveBits;
    out->doorbells = gDoorbells;
    out->glink_in = gGlink.rx_messages;
    out->glink_out = gGlink.tx_messages;
    for (unsigned i = 0; i < gClientCount; ++i) if (gClients[i].glink && gClients[i].glink->up) out->channels_up |= 1u << i;
    out->version_agreed = gGlink.version_agreed;
    out->broken = gGlink.broken;
    out->unknown_opens = gGlink.unknown_opens;
    out->ready_ms = gReadyMs;
    out->adsp_kib = gAdspKib;
    out->scm_x0 = gScmX0;
    out->scm_x1 = gScmX1;
    piano_qrtr_counters(&out->qrtr_stage, &out->qrtr_requests, &out->qrtr_answers, &out->qrtr_services);
    strlcpy(out->text, gText, sizeof out->text);
}

extern "C" const char *piano_adsp_stage_name(int stage) {
    static const char *const names[] = {"none", "windows mapped", "surveyed", "firmware opened", "SMP2P item", "doorbells",
                                        "AOSS load_state", "lcx/lmx votes", "DTB image started", "DSP image loaded", "DSP started",
                                        "DSP ready", "GLINK version agreed", "a channel up"};
    const int s = stage < 0 ? -stage : stage;
    return s < (int)(sizeof names / sizeof names[0]) ? names[s] : "?";
}
