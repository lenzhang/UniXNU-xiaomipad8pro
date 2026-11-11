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
//
// Bring-up steps (PianoAdspStage 3..18) are announced with "step N (...): begin" before they touch anything and "step N done" after,
// with a short sleep after the announcement so the console has the line before the access; every secure call and (with
// piano-power-trace, default on) every RSC/IPCC register write has a line before and after it, so a hang names its access.
// Boot arguments (read by piano_adsp_start): piano-power-delay=S (seconds before the bring-up, default 30: the root mount and the
// other drivers' bring-up are done by then), piano-power-stop=N (stop before step N; what the earlier steps did stays),
// piano-power-qmp=0 (no AOSS load_state), piano-power-rpmh=0 (no lcx/lmx proxy votes), piano-power-trace=0, piano-power-map=N
// (the carveout mapping: 0 write-combined = Normal non-cacheable as Linux's ioremap_wc, 1 Device-nGnRE, 2 Device-nGnRnE),
// piano-power-load-first=1 (an image's segments are written before its init_image and mem_setup).
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
#include "AdspHealth.h"
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
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvAop = 4, kEvSend = 16;
// Six channels: PianoPower's IPCRTR and PMIC_RTR_ADSP_APPS, then the clients' (PianoAdspContract.h); a queue of 16 messages each.
constexpr unsigned kClients = PIANO_ADSP_CHANNELS, kQueue = 16;
static_assert(kClients <= QCOM_GLINK_MAX_CHANNELS, "a GLINK channel per client slot");
constexpr uint32_t kSlowCallbackMs = 20;
constexpr uint32_t kBringUpSliceMs = 2, kSteadySliceMs = 20;

struct Knobs {uint32_t delay, stop, qmp, rpmh, trace, map, loadFirst;};
Knobs gKnobs = {30, 0, 1, 1, 1, 0, 0};
int gCurrent;   // the step begun last

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
volatile uint32_t gDoorbells, gReadyMs, gAdspKib, gRestarts;
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
void stage(int value) {gStage = value; say("step %d done: %s", value, piano_adsp_stage_name(value));}
void fail(int step, int error) {
    gError = error;
    gStage = -step;
    say("FAILED at step %d (%s): error %d%s%s", step, piano_adsp_stage_name(step), error, gText[0] ? ", " : "", gText);
}
// A step that touches hardware is announced before it starts; the sleep lets the console take the line first. false: piano-power-stop
// asks to stop before this step (what the earlier steps did stays: votes, doorbells, SMP2P item).
bool begin(int step, const char *detail) {
    if (gKnobs.stop && step >= (int)gKnobs.stop) {
        say("step %d (%s) not started: piano-power-stop=%u; the earlier steps stay as they are", step, piano_adsp_stage_name(step),
            gKnobs.stop);
        return false;
    }
    gCurrent = step;
    say("step %d (%s): begin%s%s", step, piano_adsp_stage_name(step), detail[0] ? ": " : "", detail);
    IOSleep(20);
    return true;
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
volatile uint32_t gPending;
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
// Health (piano_adsp_health): the crash reason of the last fatal report (under gClientLock), the link generation, the quiet bits.
char gCrash[80];
AdspLinkTrack gLinkTrack;
volatile uint32_t gLinkGeneration, gQuiet, gSlowCallbacks;
thread_t gThread;

// The client lock, made once whoever comes first (a client kext may register before PianoPower starts the DSP).
IOLock *clientLock() {
    IOLock *lock = __atomic_load_n(&gClientLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOLock *fresh = IOLockAlloc(), *expected = nullptr;
    if (!fresh) return nullptr;
    if (__atomic_compare_exchange_n(&gClientLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    IOLockFree(fresh);
    return expected;
}
// PianoPower's own channels: the router (PianoQrtr.c) and the charger (PianoPower.cpp).
bool ownChannel(const char *name) {return !strcmp(name, "IPCRTR") || !strcmp(name, "PMIC_RTR_ADSP_APPS");}
uint64_t nowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
void linkChanged() {__atomic_store_n(&gLinkGeneration, gLinkTrack.generation, __ATOMIC_RELEASE);}

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

void ipccWrite(uint32_t at, uint32_t value, const char *what) {
    if (gKnobs.trace) say("ipcc: write %s (+%#x) = %#x", what, at, value);
    wr(gIpcc, at, value);
    if (gKnobs.trace) say("ipcc: written %s", what);
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
    if (IOLock *lock = clientLock()) {   // the health's own copy (gText is shared with the open: notes)
        IOLockLock(lock);
        strlcpy(gCrash, text, sizeof gCrash);
        IOLockUnlock(lock);
    }
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
bool gVoted;

// apps_rsc (DT rsc@16500000, qcom,drv-id 2): the HLOS's DRV is drv-2 at 0x16520000 with its TCS block at +0xd00 (qcom,tcs-offset), done
// interrupt SPI 5 (level); tcs-config lists the active TCSs first (3: TCS 0-2), then sleep (3-4) and wake (5-6), no control TCS. One
// active-only request as Linux's rpmh-rsc sends it: an idle active TCS (DRV_STATUS non-zero), its commands (MSGID with the response
// bit: the RSC waits for RPMh's acknowledgement before it reports completion; address; data), WAIT_FOR_CMPL and CMD_ENABLE, the TCS's
// IRQ_ENABLE bit (its IRQ_STATUS bit is what completion sets), then CONTROL: trigger off, AMC mode off, AMC mode on, AMC mode + trigger.
// Completion is polled every millisecond for at most 100 ms: the IRQ_STATUS bit and each command's CMD_STATUS (bit 8 issued, bit 16
// completed). A completed TCS is reclaimed (CMD_ENABLE 0, IRQ_CLEAR, CONTROL off); one that did not complete is left alone (touching
// a TCS in flight is what the hardware forbids). IRQ_ENABLE is put back as it was either way, so no later completion raises SPI 5.
constexpr unsigned kActiveTcs = 3;
constexpr uint32_t kAmcEnable = 1u << 16, kAmcTrigger = 1u << 24, kMsgidWriteResp = 8u | 1u << 8 | 1u << 16;
constexpr uint32_t kCmdIssued = 1u << 8, kCmdCompleted = 1u << 16;
bool gTcsStuck[kActiveTcs];

// The DRV-2 owner word of PianoA830 (package/PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h §4.3.4): one 32-bit word, 0 = free,
// any other value = its owner's tag. The A830 driver holds it around its own TCS transactions, so the installed system (piano-gpu=2)
// and this driver cannot program the same TCS at once. PianoA830 is listed before PianoPower in configs/k27-native-kexts.txt, so the
// KC linker (which has no weak imports) resolves this to A830's single copy; PianoPower.json names PianoA830 as a library. The
// protocol is A830's: compare-exchange 0 for our tag, 10 us between tries, at most 2 s, and store 0 to release.
extern "C" volatile uint32_t sm8750_rpmh_drv2_owner;
constexpr uint32_t kDrv2Tag = 0x504f5752;   // "POWR"
constexpr unsigned kDrv2Steps = 200000, kDrv2StepUs = 10;

// Takes the lock; *waits gets the 10 us waits. false: someone held it for 2 s (then nothing is programmed).
bool drv2Acquire(uint32_t *waits) {
    for (uint32_t n = 0;; ++n) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&sm8750_rpmh_drv2_owner, &expected, kDrv2Tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            *waits = n;
            return true;
        }
        if (n == kDrv2Steps) {*waits = n; return false;}
        IODelay(kDrv2StepUs);
    }
}
void drv2Release() {__atomic_store_n(&sm8750_rpmh_drv2_owner, 0, __ATOMIC_RELEASE);}

uint32_t rscRead(uint64_t at) {return rd(gRsc, kRscTcs + at);}
void rscWrite(uint64_t at, uint32_t value, const char *what) {
    if (gKnobs.trace) say("rsc: write %s (+%#llx) = %#x", what, (unsigned long long)(kRscTcs + at), value);
    wr(gRsc, kRscTcs + at, value);
    if (gKnobs.trace) say("rsc: written %s, reads %#x", what, rd(gRsc, kRscTcs + at));
}

int voteLocked(uint32_t lcx, uint32_t lmx, const char *why) {
    const QcomRscLayout &l = gLayout;
    const uint32_t id = rd(gRsc, 0), major = (id >> 16) & 0xff;
    say("rsc: %s: lcx index %u, lmx index %u; DRV id %#x (version %u.%u: %s register layout)", why, lcx, lmx, id, major, (id >> 8) & 0xff,
        major >= 3 ? "v3" : "v2");
    static const unsigned kOrder[kActiveTcs] = {1, 2, 0};
    int tcs = -1;
    for (unsigned t = 0; t < kActiveTcs; ++t) {
        const uint64_t base = (uint64_t)l.tcs_stride * t;
        const uint32_t status = rscRead(base + l.status), control = rscRead(base + l.control), enable = rscRead(base + l.cmd_enable);
        say("rsc: TCS %u: DRV_STATUS %#x (%s), CONTROL %#x, CMD_ENABLE %#x%s", t, status, status ? "idle" : "busy", control, enable,
            gTcsStuck[t] ? ", left in flight earlier" : "");
    }
    for (unsigned k = 0; k < kActiveTcs && tcs < 0; ++k) {
        const unsigned t = kOrder[k];
        if (!gTcsStuck[t] && rscRead((uint64_t)l.tcs_stride * t + l.status)) tcs = (int)t;
    }
    if (tcs < 0) {say("rsc: no idle active TCS: no vote sent"); return -16;}
    const uint64_t base = (uint64_t)l.tcs_stride * (unsigned)tcs, cmdStatus = l.cmd_data + 4;
    const uint32_t bit = 1u << tcs, irqEnable = rscRead(0), irqStatus = rscRead(l.irq_status);
    say("rsc: using TCS %d; IRQ_ENABLE %#x, IRQ_STATUS %#x", tcs, irqEnable, irqStatus);
    if (irqStatus & bit) rscWrite(l.irq_clear, bit, "IRQ_CLEAR (stale completion)");
    const uint32_t addresses[2] = {gLcx.address, gLmx.address}, values[2] = {lcx, lmx};
    static const char *const kNames[2][3] = {{"cmd 0 MSGID", "cmd 0 ADDR (lcx.lvl)", "cmd 0 DATA"},
                                             {"cmd 1 MSGID", "cmd 1 ADDR (lmx.lvl)", "cmd 1 DATA"}};
    for (unsigned c = 0; c < 2; ++c) {
        const uint64_t cmd = base + (uint64_t)l.cmd_stride * c;
        rscWrite(cmd + l.cmd_msgid, kMsgidWriteResp, kNames[c][0]);
        rscWrite(cmd + l.cmd_addr, addresses[c], kNames[c][1]);
        rscWrite(cmd + l.cmd_data, values[c], kNames[c][2]);
    }
    rscWrite(base + l.wait_for_cmpl, 3, "WAIT_FOR_CMPL");
    rscWrite(base + l.cmd_enable, 3, "CMD_ENABLE");
    rscWrite(0, irqEnable | bit, "IRQ_ENABLE");
    const uint32_t control = rscRead(base + l.control);
    rscWrite(base + l.control, control & ~kAmcTrigger, "CONTROL (trigger off)");
    rscWrite(base + l.control, control & ~(kAmcTrigger | kAmcEnable), "CONTROL (AMC mode off)");
    rscWrite(base + l.control, kAmcEnable, "CONTROL (AMC mode on)");
    rscWrite(base + l.control, kAmcEnable | kAmcTrigger, "CONTROL (AMC mode on + trigger)");
    uint32_t irq = 0, s0 = 0, s1 = 0;
    unsigned waited = 0;
    for (;; ++waited) {
        irq = rscRead(l.irq_status) & bit;
        s0 = rscRead(base + cmdStatus);
        s1 = rscRead(base + l.cmd_stride + cmdStatus);
        if ((irq && (s0 & kCmdCompleted) && (s1 & kCmdCompleted)) || waited >= 100) break;
        IOSleep(1);
    }
    const bool completed = irq || ((s0 & kCmdCompleted) && (s1 & kCmdCompleted));
    say("rsc: %s after %u ms: IRQ_STATUS bit %s, cmd 0 status %#x (%s), cmd 1 status %#x (%s)", completed ? "completed" : "NOT completed",
        waited, irq ? "set" : "clear", s0, s0 & kCmdCompleted ? "completed" : s0 & kCmdIssued ? "issued" : "not issued", s1,
        s1 & kCmdCompleted ? "completed" : s1 & kCmdIssued ? "issued" : "not issued");
    if (completed) {
        rscWrite(base + l.cmd_enable, 0, "CMD_ENABLE (reclaim)");
        rscWrite(l.irq_clear, bit, "IRQ_CLEAR");
        const uint32_t now = rscRead(base + l.control);
        rscWrite(base + l.control, now & ~kAmcTrigger, "CONTROL (trigger off)");
        rscWrite(base + l.control, 0, "CONTROL (AMC mode off)");
    } else {
        gTcsStuck[tcs] = true;
        say("rsc: TCS %d left as it is (in flight); not used again", tcs);
    }
    rscWrite(0, irqEnable, "IRQ_ENABLE (as it was)");
    say("rails: lcx index %u, lmx index %u%s", lcx, lmx, completed ? "" : " (completion not seen)");
    return completed ? 0 : -60;
}

// Every vote of ours runs with PianoA830's DRV-2 owner word held, so a GPU vote cannot be programming the same TCS.
int vote(uint32_t lcx, uint32_t lmx, const char *why) {
    uint32_t waits = 0;
    const uint32_t held = __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE);
    if (held) say("rsc: the DRV-2 lock is held by %#x; waiting", held);
    if (!drv2Acquire(&waits)) {
        say("rsc: the DRV-2 lock stayed held by %#x for 2 s: no vote sent", __atomic_load_n(&sm8750_rpmh_drv2_owner, __ATOMIC_ACQUIRE));
        return -16;
    }
    if (waits) say("rsc: DRV-2 lock taken after %u waits of %u us", waits, kDrv2StepUs);
    const int rc = voteLocked(lcx, lmx, why);
    drv2Release();
    return rc;
}

// ---- secure world calls (Sm8750Scm: INTERRUPTED, WAITQ_SLEEP polled, EBUSY) ----
Sm8750Scm gScm;
IOLock *gScmLock;
uint64_t nowUs(void *) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
void sleepUs(void *, uint32_t us) {if (us >= 1000) IOSleep(us / 1000); else IODelay(us);}
void scmLock(void *) {IOLockLock(gScmLock);}
void scmUnlock(void *) {IOLockUnlock(gScmLock);}
const Sm8750ScmOps gScmOps = {nullptr, Sm8750ScmSmcInstruction, nowUs, sleepUs, scmLock, scmUnlock, nullptr, nullptr};
// A secure call with a line before it (and a moment for the console) and one after it: a call the secure world does not return from
// leaves the first line last in the log.
template <typename F> bool secure(const char *what, F call) {
    say("smc: %s: calling", what);
    IOSleep(20);
    Sm8750SmcResult r = {};
    const uint64_t t0 = nowUs(nullptr);
    const Sm8750ScmStatus status = call(&r);
    const uint64_t us = nowUs(nullptr) - t0;
    gScmX0 = r.x0;
    gScmX1 = r.x1;
    say("smc: %s: %s (status %d, x0 %lld, x1 %#llx, %u SMCs: %u interrupted, %u wait-queue, %u busy; %llu us)", what,
        status == Sm8750ScmOk ? "ok" : "FAILED", (int)status, (long long)r.x0, (unsigned long long)r.x1, r.calls, r.interrupted, r.waitq,
        r.busy, (unsigned long long)us);
    return status == Sm8750ScmOk;
}
void shutdownPas(uint32_t pas) {
    const uint64_t args[4] = {pas, 0, 0, 0};
    char what[32];
    snprintf(what, sizeof what, "PAS %u shutdown", pas);
    secure(what, [&](Sm8750SmcResult *out) {
        return Sm8750ScmCall(&gScm, SM8750_SCM_FID(0, Sm8750ScmSvcPil, 0x06), SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, out);
    });
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
// The metadata (ELF and program headers, then the hash segment) in physically contiguous memory, cleaned to the point of coherency.
int prepareMetadata(Image &image, uint64_t *phys) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
                                                                 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    *phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    say("metadata: %llu bytes at %#llx (%s image, cleaned)", (unsigned long long)bytes, (unsigned long long)*phys,
        image.mdt.relocatable ? "a relocatable" : "a fixed");
    return *phys && !(*phys & 0xfff) ? 0 : -14;
}
IOOptionBits carveoutCache() {
    return gKnobs.map == 1 ? kIOMapPostedWrite : gKnobs.map == 2 ? kIOMapInhibitCache : kIOMapWriteCombineCache;
}
const char *carveoutCacheName() {
    return gKnobs.map == 1 ? "Device-nGnRE" : gKnobs.map == 2 ? "Device-nGnRnE" : "write-combined (Normal non-cacheable)";
}
// One aligned 8-byte read of the carveout's first word, then the same value written back (nothing changes), each announced before
// and logged after: a hang names the access and the moment (before the first secure call, or after mem_setup).
bool probeCarveout(const char *when, uint64_t base) {
    Window page = {};
    if (!mapWindow(page, base, 0x1000, carveoutCache())) {say("probe (%s): %#llx does not map", when, (unsigned long long)base); return false;}
    volatile uint64_t *word = reinterpret_cast<volatile uint64_t *>(page.va);
    say("probe (%s): 8-byte read at %#llx (%s): begin", when, (unsigned long long)base, carveoutCacheName());
    IOSleep(20);
    const uint64_t value = *word;
    say("probe (%s): read %#llx; writing it back: begin", when, (unsigned long long)value);
    IOSleep(20);
    *word = value;
    __asm__ volatile("dsb sy" ::: "memory");
    say("probe (%s): written, reads back %#llx", when, (unsigned long long)*word);
    unmap(page);
    return true;
}
// Stores of 8 aligned bytes (bytes at the ends), no memcpy/bzero: the copy works on every mapping type and uses no DC ZVA.
void copyOut(volatile uint8_t *to, const uint8_t *from, uint64_t n) {
    while (n && (reinterpret_cast<uintptr_t>(to) & 7)) {*to++ = *from++; --n;}
    for (; n >= 8; n -= 8, to += 8, from += 8) {
        uint64_t v;
        memcpy(&v, from, 8);
        *reinterpret_cast<volatile uint64_t *>(to) = v;
    }
    while (n--) *to++ = *from++;
}
void zeroOut(volatile uint8_t *to, uint64_t n) {
    while (n && (reinterpret_cast<uintptr_t>(to) & 7)) {*to++ = 0; --n;}
    for (; n >= 8; n -= 8, to += 8) *reinterpret_cast<volatile uint64_t *>(to) = 0;
    while (n--) *to++ = 0;
}
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    Window region = {};
    // A relocatable image is mapped only as far as mem_setup declared it (its span), a fixed one as the whole carveout.
    const uint64_t span = image.mdt.relocatable ? qcom_mdt_span(&image.mdt) : size;
    if (span < size) size = span;
    say("carveout %#llx + %#llx: mapping %s", (unsigned long long)base, (unsigned long long)size, carveoutCacheName());
    if (!mapWindow(region, base, size, carveoutCache())) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    unsigned written = 0;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        if (gKnobs.trace && (written < 3 || i + 1 == image.mdt.count))
            say("segment %u: %llu bytes from the file, %llu zero, at +%#llx", i, (unsigned long long)s.file_size,
                (unsigned long long)(s.memory_size > s.file_size ? s.memory_size - s.file_size : 0), (unsigned long long)at);
        const bool verbose = gKnobs.trace && (written < 3 || i + 1 == image.mdt.count);
        volatile uint8_t *to = region.va + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (piano_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            if (verbose && !done) say("segment %u: %zu bytes read from the file; storing them", i, n);
            copyOut(to + done, bounce, n);
            if (verbose && !done) say("segment %u: first %zu bytes stored", i, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) zeroOut(to + s.file_size, s.memory_size - s.file_size);
        written++;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    say("carveout: %u segments written, unmapped (%d)", written, rc);
    return rc;
}

// One image: metadata, init_image, mem_setup (relocatable images), segments, auth_and_reset, each its own step (firstStep .. +4).
// Returns 0 when started, 1 when piano-power-stop ended it, negative on a failure.
int startImage(Image &image, const char *name, uint32_t pas, uint64_t base, uint64_t size, int firstStep) {
    char what[64];
    uint64_t phys = 0;
    int rc;
    if (!begin(firstStep, name)) return 1;
    if ((rc = prepareMetadata(image, &phys))) return rc;
    probeCarveout("before init_image", base);
    stage(firstStep);
    if (gKnobs.loadFirst) {   // piano-power-load-first: the segments before init_image and mem_setup
        if (!begin(firstStep + 3, name)) return 1;
        if ((rc = loadSegments(image, base, size))) return rc;
        stage(firstStep + 3);
    }
    if (!begin(firstStep + 1, name)) return 1;
    snprintf(what, sizeof what, "PAS init_image (%s, PAS %u)", name, pas);
    if (!secure(what, [&](Sm8750SmcResult *r) {return Sm8750ScmPasInitImage(&gScm, pas, phys, r);})) return -1;
    stage(firstStep + 1);
    if (image.mdt.relocatable) {
        if (!begin(firstStep + 2, name)) return 1;
        const uint64_t span = qcom_mdt_span(&image.mdt);
        snprintf(what, sizeof what, "PAS mem_setup (%s, %#llx + %#llx)", name, (unsigned long long)base, (unsigned long long)span);
        if (!secure(what, [&](Sm8750SmcResult *r) {return Sm8750ScmPasMemSetup(&gScm, pas, base, span, r);})) return -2;
        stage(firstStep + 2);
    } else {
        say("step %d (%s): not needed, a fixed image", firstStep + 2, piano_adsp_stage_name(firstStep + 2));
    }
    if (!gKnobs.loadFirst) {
        if (!begin(firstStep + 3, name)) return 1;
        probeCarveout("after mem_setup", base);
        if ((rc = loadSegments(image, base, size))) return rc;
        stage(firstStep + 3);
    }
    if (!begin(firstStep + 4, name)) return 1;
    snprintf(what, sizeof what, "PAS auth_and_reset (%s, PAS %u)", name, pas);
    if (!secure(what, [&](Sm8750SmcResult *r) {return Sm8750ScmPasAuthAndReset(&gScm, pas, r);})) return -3;
    stage(firstStep + 4);
    return 0;
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
uint32_t gSlaveChanges, gHandoverAgain;
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    // The piano ADSP raises handover again after every power collapse (kernel.md §2.5, K:9adeb3213d24), so the bits may change often:
    // the first 16 changes are logged, then every 64th; fatal and ready changes always are (below).
    if (value != gSlave && (++gSlaveChanges <= 16 || gSlaveChanges % 64 == 0)) say("slave-kernel bits %#x (change %u)", value, gSlaveChanges);
    gSlave = value;
    gSlaveBits = (int32_t)value;
    if ((changed & value & kFatal)) {
        // Every DSP crash puts the charger back on its defaults (Ubuntu #3: a sensor-framework loop restarted the ADSP about 8
        // times a minute; natively nothing serves the sensors' files). Nothing here restarts it: the count and the reason are logged.
        gRestarts = gRestarts + 1;
        say("the DSP reports a FATAL error (%u since the start)", gRestarts);
        noteCrash();
    }
    if ((changed & ~value & kReady) && gStage >= PianoAdspReady) say("the DSP is no longer ready (slave-kernel %#x)", value);
    if ((changed & value & kHandover) && gHandedOver && ++gHandoverAgain <= 3)
        say("handover raised again (%u; a DSP power collapse): ignored, it is one-shot per DSP boot", gHandoverAgain);
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        say("handover: the DSP holds its own votes now");
        if (gVoted) {vote(0, 0, "proxy votes off at handover"); gVoted = false;}
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
    if ((gGlink.broken != 0) != (gLinkTrack.broken != 0)) {
        adsp_link_broken(&gLinkTrack, gGlink.broken != 0);
        linkChanged();
        if (gGlink.broken) say("GLINK: the transport is broken (link generation %u)", gLinkTrack.generation);
    }
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
    piano_qrtr_poll();   // server changes a client's open asked for (PianoQrtr.c)
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
// No lock is held across these: a client's callback may queue sends (gClientLock, taken and dropped inside) and nothing else waits.
void timed(uint64_t since, const char *name) {
    const uint64_t ms = nowMs() - since;
    if (ms > kSlowCallbackMs) piano_adsp_slow_callback(name, (uint32_t)ms);
}
void clientState(void *context, QcomGlinkChannel *, int up) {
    PianoAdspChannel *c = static_cast<PianoAdspChannel *>(context);
    adsp_link_channel(&gLinkTrack, (unsigned)(c - gClients), up);
    linkChanged();
    say("channel %s %s%s", c->name, up ? "up" : "down", up ? "" : " (link generation moves)");
    if (c->client.state) {
        const uint64_t t = nowMs();
        c->client.state(c->context, up);
        timed(t, c->name);
    }
}
void clientReceive(void *context, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    PianoAdspChannel *c = static_cast<PianoAdspChannel *>(context);
    if (c->client.receive) {
        const uint64_t t = nowMs();
        c->client.receive(c->context, data, length);
        timed(t, c->name);
    }
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
// One message per channel per round, so a busy channel (a client's) never holds the charger's behind it in the shared FIFO.
void pumpSends() {
    uint32_t blocked = 0;
    for (bool progress = true; progress;) {
        progress = false;
        for (unsigned i = 0; i < gClientCount; ++i) {
            PianoAdspChannel &c = gClients[i];
            if ((blocked >> i) & 1 || !c.glink || !c.glink->up) continue;
            IOLockLock(gClientLock);
            Message *m = c.count ? &c.queue[c.head] : nullptr;
            IOLockUnlock(gClientLock);
            if (!m) continue;
            // Only this thread takes messages off the queue, so m stays valid while the lock is dropped for the send.
            if (qcom_glink_send(&gGlink, c.glink, m->data, m->length) != QcomGlinkOk) {blocked |= 1u << i; continue;}
            IOLockLock(gClientLock);
            c.head = (c.head + 1) % kQueue;
            c.count--;
            IOLockUnlock(gClientLock);
            progress = true;
        }
    }
}

// ---- the bring-up ----
void enableDoorbells() {
    const uint32_t enable[3] = {ipccId(kClientLpass, kSignalSmp2p), ipccId(kClientLpass, kSignalGlinkQmp), ipccId(kClientAop, kSignalGlinkQmp)};
    static const char *const kNames[3] = {"RECV_ENABLE LPASS SMP2P", "RECV_ENABLE LPASS GLINK", "RECV_ENABLE AOP QMP"};
    for (unsigned i = 0; i < 3; ++i) ipccWrite(kIpccRecvEnable, enable[i], kNames[i]);
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

void dropVotes() {
    if (gVoted) {vote(0, 0, "proxy votes off after a failure"); gVoted = false;}
}

bool start() {
    char dir[80] = "";
    const bool chosen = PE_parse_boot_argn("piano-power-fw", dir, sizeof dir) && dir[0];
    char path[112];
    Image &dtb = gDtbImage, &adsp = gAdspImage;
    if (!begin(PianoAdspFirmware, chosen ? dir : "modem_a (else modem_b) image/ through NativeUfs, read only")) return false;
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
    if (!begin(PianoAdspSmp2p, "SMEM item 429 under TCSR mutex 3 (0x1f43000)")) {release(dtb); release(adsp); return false;}
    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(PianoAdspSmp2p, rc); release(dtb); release(adsp); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    gStopEntry = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    stage(PianoAdspSmp2p);
    if (!begin(PianoAdspDoorbells, "IPCC 0x406000: receive LPASS SMP2P/GLINK and AOP, ring LPASS SMP2P")) {
        release(dtb); release(adsp); return false;
    }
    enableDoorbells();
    ipccWrite(kIpccSend, ipccId(kClientLpass, kSignalSmp2p), "SEND LPASS SMP2P");
    stage(PianoAdspDoorbells);

    if (gKnobs.qmp) {
        if (!begin(PianoAdspLoadState, "AOSS QMP 0xc300000 (polled): open, then {class: image, res: load_state, name: adsp, val: on}")) {
            release(dtb); release(adsp); return false;
        }
        qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
        for (int phase = 0; phase < 4 && qcom_qmp_step(&gQmp) > 0 && gQmp.state != QcomQmpOpen; ++phase) {
            waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;});
            say("qmp: state %d after phase %d", gQmp.state, phase);
        }
        if (gQmp.state != QcomQmpOpen) {fail(PianoAdspLoadState, gQmp.state); release(dtb); release(adsp); return false;}
        say("qmp: open; sending load_state");
        qcom_qmp_send(&gQmp, "{class: image, res: load_state, name: adsp, val: on}");
        if (!waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen;})) {
            qcom_qmp_abandon(&gQmp);
            fail(PianoAdspLoadState, -60); release(dtb); release(adsp); return false;
        }
        say("qmp: the AOP took the message");
        stage(PianoAdspLoadState);
    } else {
        say("step %d (%s) skipped: piano-power-qmp=0", PianoAdspLoadState, piano_adsp_stage_name(PianoAdspLoadState));
    }

    if (gKnobs.rpmh) {
        if (!begin(PianoAdspRails, "cmd-db lcx.lvl/lmx.lvl, highest level, apps RSC drv-2 active TCS")) {release(dtb); release(adsp); return false;}
        // A copy of the command database, held only while the levels are looked up.
        struct CmdDbCopy {uint8_t *bytes = static_cast<uint8_t *>(IOMallocData(kCmdDbSize)); ~CmdDbCopy() {if (bytes) IOFreeData(bytes, kCmdDbSize);}} copy;
        uint8_t *const db = copy.bytes;
        uint32_t lcx = 0, lmx = 0;
        if (!db || !readCmdDb(db) || qcom_cmd_db_find(db, kCmdDbSize, "lcx.lvl", &gLcx) || qcom_cmd_db_find(db, kCmdDbSize, "lmx.lvl", &gLmx) ||
            qcom_rpmh_level_index(&gLcx, 0xffffffffu, &lcx) || qcom_rpmh_level_index(&gLmx, 0xffffffffu, &lmx)) {
            fail(PianoAdspRails, -2); release(dtb); release(adsp); return false;
        }
        say("cmd-db: lcx.lvl at %#x (%u levels, top %u), lmx.lvl at %#x (%u levels, top %u)", gLcx.address, gLcx.level_count,
            gLcx.level_count ? gLcx.levels[gLcx.level_count - 1] : 0, gLmx.address, gLmx.level_count,
            gLmx.level_count ? gLmx.levels[gLmx.level_count - 1] : 0);
        gVoted = true;   // a vote that did not complete may still take effect: it is taken back at handover or on a failure
        if ((rc = vote(lcx, lmx, "proxy votes for the DSP start"))) {
            fail(PianoAdspRails, rc); dropVotes(); release(dtb); release(adsp); return false;
        }
        stage(PianoAdspRails);
    } else {
        say("step %d (%s) skipped: piano-power-rpmh=0 (the rails stay where the boot stages and the AOP left them)", PianoAdspRails,
            piano_adsp_stage_name(PianoAdspRails));
    }

    // The DSP's devicetree image first, as its own authenticated image, then the DSP image.
    rc = startImage(dtb, "DTB", kPasDtb, kDtbBase, kDtbSize, PianoAdspDtbMeta);
    if (rc) {
        if (rc < 0) {
            const int step = gCurrent;
            fail(step, rc);
            if (step > PianoAdspDtbInit) shutdownDsp(true, false);
            dropVotes();
        }
        release(dtb); release(adsp); return false;
    }
    rc = startImage(adsp, "DSP", kPasAdsp, kAdspBase, kAdspSize, PianoAdspMeta);
    if (rc) {
        if (rc < 0) {
            const int step = gCurrent;
            fail(step, rc);
            shutdownDsp(true, step > PianoAdspInit);
            dropVotes();
        }
        release(dtb); release(adsp); return false;
    }
    if (!begin(PianoAdspReady, "SMP2P slave-kernel ready (bit 1) or fatal (bit 0), at most 5 s")) {release(dtb); release(adsp); return false;}
    const uint64_t began = mach_absolute_time();
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
        if (!gHandedOver) dropVotes();
        return false;
    }
    say("ready after %u ms", gReadyMs);
    stage(PianoAdspReady);
    return true;
}

bool startGlink() {
    say("GLINK: SMEM items 478 (descriptor) and 479 (our FIFO, %u bytes); the DSP's FIFO is item 480", kGlinkFifo);
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
    __atomic_store_n(&gThread, current_thread(), __ATOMIC_RELEASE);
    if (!gStage) {
        if (!mapAll()) {fail(PianoAdspMapped, gError); return;}
        stage(PianoAdspMapped);
        PianoAdspSurvey s;
        survey(&s);
    }
    stage(PianoAdspSurveyed);
    if (gKnobs.delay) {
        say("waiting %u s before the bring-up (piano-power-delay)", gKnobs.delay);
        IOSleep(gKnobs.delay * 1000);
    }
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
    IOLock *lock = clientLock();
    if (!lock || !name || !name[0] || strlen(name) >= QCOM_GLINK_NAME || !client || !intent_size || intent_size > 0x10000 ||
        !intent_count || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS)
        return nullptr;
    uint8_t *storage = static_cast<uint8_t *>(IOMallocData((vm_size_t)intent_size * intent_count));
    Message *queue = static_cast<Message *>(IOMallocData(sizeof(Message) * kQueue));
    IOLockLock(lock);
    // One owner per channel name (a second registration of IPCRTR or PMIC_RTR_ADSP_APPS is refused), and two of the six slots stay
    // PianoPower's own whatever registers first: a client kext may start before PianoPower's level 2.
    bool taken = false;
    unsigned others = 0;
    for (unsigned i = 0; i < gClientCount; ++i) {
        taken |= !strncmp(gClients[i].name, name, sizeof gClients[i].name);
        others += !ownChannel(gClients[i].name);
    }
    const bool room = adsp_channel_room(gClientCount, others, ownChannel(name), kClients);
    PianoAdspChannel *c = !taken && room && storage && queue ? &gClients[gClientCount] : nullptr;
    if (c) {
        bzero(c, sizeof *c);
        strlcpy(c->name, name, sizeof c->name);
        c->intent_size = intent_size; c->intent_count = intent_count; c->initiator = initiator;
        c->client = *client; c->context = context; c->storage = storage; c->queue = queue;
        __atomic_store_n(&gClientCount, gClientCount + 1, __ATOMIC_RELEASE);
    }
    IOLockUnlock(lock);
    if (!c) {
        if (storage) IOFreeData(storage, (vm_size_t)intent_size * intent_count);
        if (queue) IOFreeData(queue, sizeof(Message) * kQueue);
        say("channel %s: not registered (%s)", name, taken ? "the name is taken" : "no slot or no memory");
        return nullptr;
    }
    wake(kEvSend);
    return c;
}

extern "C" int piano_adsp_send_keep(PianoAdspChannel *c, const void *data, uint32_t length, uint32_t keep_free) {
    if (!c || !data || !length) return -2;
    if (length > PIANO_ADSP_MAX_MESSAGE) return -1;
    IOLock *lock = clientLock();
    if (!lock) return -1;
    IOLockLock(lock);
    if (c->count + keep_free >= kQueue) {IOLockUnlock(lock); return -1;}
    Message &m = c->queue[(c->head + c->count) % kQueue];
    memcpy(m.data, data, length);
    m.length = length;
    c->count++;
    IOLockUnlock(lock);
    wake(kEvSend);
    return 0;
}

extern "C" int piano_adsp_send(PianoAdspChannel *c, const void *data, uint32_t length) {
    return piano_adsp_send_keep(c, data, length, 0);
}

extern "C" int piano_adsp_on_thread(void) {
    return current_thread() == __atomic_load_n(&gThread, __ATOMIC_ACQUIRE);
}

extern "C" void piano_adsp_wake(void) {wake(kEvSend);}

extern "C" uint64_t piano_adsp_now_ms(void) {return nowMs();}

extern "C" void piano_adsp_slow_callback(const char *who, uint32_t ms) {
    const uint32_t n = __atomic_add_fetch(&gSlowCallbacks, 1u, __ATOMIC_RELAXED);
    if (n <= 8 || (n & (n - 1)) == 0)
        say("%s: a client callback took %u ms on the DSP thread (the charger waited; %u so far)", who, ms, n);
}

extern "C" void piano_adsp_quiet_set(uint32_t bits, int on) {
    if (on) __atomic_fetch_or(&gQuiet, bits, __ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&gQuiet, ~bits, __ATOMIC_ACQ_REL);
}

extern "C" uint32_t piano_adsp_quiet(void) {return __atomic_load_n(&gQuiet, __ATOMIC_ACQUIRE);}

extern "C" void piano_adsp_health(PianoAdspHealth *out) {
    if (!out) return;
    bzero(out, sizeof *out);
    out->version = PIANO_ADSP_HEALTH_VERSION;
    out->stage = gStage;
    const uint32_t slave = gSlaveBits < 0 ? 0 : (uint32_t)gSlaveBits;
    out->fatal = (slave & kFatal) ? 1 : 0;
    out->fatal_reports = gRestarts;
    out->link_generation = __atomic_load_n(&gLinkGeneration, __ATOMIC_ACQUIRE);
    out->dsp_up = adsp_health_dsp_up(gStage, slave, gGlinkStarted && gGlink.version_agreed && !gGlink.broken, piano_qrtr_link_up());
    out->charger_pd = piano_qrtr_charger_pd(&out->charger_generation);
    out->quiet = piano_adsp_quiet();
    if (IOLock *lock = clientLock()) {
        IOLockLock(lock);
        strlcpy(out->crash, gCrash, sizeof out->crash);
        IOLockUnlock(lock);
    }
}

extern "C" void piano_adsp_start(void) {
    if (gStarted) return;
    gStarted = true;
    gScmLock = IOLockAlloc();
    if (!clientLock() || !gScmLock) {fail(PianoAdspMapped, -12); return;}
    PE_parse_boot_argn("piano-power-delay", &gKnobs.delay, sizeof gKnobs.delay);
    PE_parse_boot_argn("piano-power-stop", &gKnobs.stop, sizeof gKnobs.stop);
    PE_parse_boot_argn("piano-power-qmp", &gKnobs.qmp, sizeof gKnobs.qmp);
    PE_parse_boot_argn("piano-power-rpmh", &gKnobs.rpmh, sizeof gKnobs.rpmh);
    PE_parse_boot_argn("piano-power-trace", &gKnobs.trace, sizeof gKnobs.trace);
    PE_parse_boot_argn("piano-power-map", &gKnobs.map, sizeof gKnobs.map);
    PE_parse_boot_argn("piano-power-load-first", &gKnobs.loadFirst, sizeof gKnobs.loadFirst);
    if (gKnobs.delay > 600) gKnobs.delay = 600;
    if (gKnobs.map > 2) gKnobs.map = 0;
    say("bring-up: delay %u s, stop before step %u%s, AOSS load_state %s, lcx/lmx votes %s, trace %s, carveouts %s, segments %s",
        gKnobs.delay, gKnobs.stop, gKnobs.stop ? "" : " (none)", gKnobs.qmp ? "on" : "off", gKnobs.rpmh ? "on" : "off",
        gKnobs.trace ? "on" : "off", carveoutCacheName(), gKnobs.loadFirst ? "before init_image (load-first)" : "after mem_setup");
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
    out->fatal = gRestarts;
    out->adsp_kib = gAdspKib;
    out->scm_x0 = gScmX0;
    out->scm_x1 = gScmX1;
    piano_qrtr_counters(&out->qrtr_stage, &out->qrtr_requests, &out->qrtr_answers, &out->qrtr_services);
    strlcpy(out->text, gText, sizeof out->text);
}

extern "C" const char *piano_adsp_stage_name(int stage) {
    static const char *const names[] = {"none", "windows mapped", "surveyed", "firmware opened", "SMP2P item", "doorbells",
                                        "AOSS load_state", "lcx/lmx votes", "DTB metadata", "DTB PAS init_image", "DTB PAS mem_setup",
                                        "DTB segments", "DTB PAS auth_and_reset", "DSP metadata", "DSP PAS init_image",
                                        "DSP PAS mem_setup", "DSP segments", "DSP PAS auth_and_reset", "DSP ready",
                                        "GLINK version agreed", "a channel up"};
    const int s = stage < 0 ? -stage : stage;
    return s < (int)(sizeof names / sizeof names[0]) ? names[s] : "?";
}
