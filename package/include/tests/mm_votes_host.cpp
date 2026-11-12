// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the shared multimedia votes (package/include/PianoMmVotes.h): two simulated clients (PianoVideo, PianoCamera), each
// its own PianoMmVotes with its own tag, over one set of words and one model of apps DRV-2 (the vote it holds per resource):
//   - one user: the commands are exactly those PianoVideo's VideoRpmh.h sent before the helper (its railsUp/railsDown/bus rules,
//     kept here as the reference), over every sequence of up to six of its operations, with and without a display floor;
//   - two users: a release never drops the other's need (MXC back to the camera's level, not 0; MM1 stays on), the highest floor
//     wins, the maximum of the needs is voted, only what changes is sent, the last release votes MXC 0 and MM1 off;
//   - failures: a refused transaction leaves the words as they were (a retry sends again); a level no table entry reaches; a
//     resource without an address; the lock held past the bound (busy, nothing changed); a lost lock; bad arguments;
//   - real threads: both clients update in loops: never two transactions at once, the model always holds the votes the words say,
//     the lock free at the end;
//   - the cmd-db reader: a synthetic database with ARCs (a dummy one among them) and a BCM: addresses, level tables, the errors.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. mm_votes_host.cpp -o /tmp/mm_votes_host && /tmp/mm_votes_host
#include "PianoMmVotes.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the model
constexpr uint32_t kMxcAddr = 0x30010, kMmcxAddr = 0x30020, kMm1Addr = 0x50040;
// The rails' cmd-db tables (vlvl per hlvl): piano's mxc.lvl and mmcx.lvl have these shapes (off, ret, min_svs.., nom_l1, ...).
static const uint16_t kMxcTable[] = {0, 0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x150, 0x180};
static const uint16_t kMmcxTable[] = {0, 0x30, 0x38, 0x40, 0x60, 0x80, 0xc0, 0x100, 0x140, 0x180};

struct Drv2 {
  std::mutex m;
  std::map<uint32_t, uint32_t> held;   // the vote apps DRV-2 holds per address
  std::vector<std::vector<PianoMmCmd>> log;
  std::atomic<int> inside{0};
  std::atomic<int> overlaps{0};
  int failNext = 0;
};

struct Client {
  Drv2 *drv;
  bool noAddress[3] = {false, false, false};
  PianoMmVotesOps ops{};
  PianoMmVotes v{};
  std::vector<std::pair<uint32_t, uint32_t>> reports;
};

static uint32_t opAddress(void *c, unsigned r)
{
  Client *k = static_cast<Client *>(c);
  if (k->noAddress[r]) return 0;
  return r == PIANO_MM_MXC ? kMxcAddr : r == PIANO_MM_MMCX ? kMmcxAddr : kMm1Addr;
}
static int opLevel(void *, unsigned r, uint32_t vlvl, uint32_t *hlvl)
{
  PianoMmArc a{};
  const uint16_t *t = r == PIANO_MM_MXC ? kMxcTable : kMmcxTable;
  a.count = 10;
  for (unsigned i = 0; i < 10; ++i) a.vlvl[i] = t[i];
  return piano_mm_arc_level(&a, vlvl, hlvl);
}
static int opSend(void *c, const PianoMmCmd *cmds, unsigned n)
{
  Drv2 *d = static_cast<Client *>(c)->drv;
  if (d->inside.fetch_add(1)) d->overlaps++;
  int rc = 0;
  {
    std::lock_guard<std::mutex> g(d->m);
    if (d->failNext) {
      --d->failNext;
      rc = -60;
    } else {
      d->log.emplace_back(cmds, cmds + n);
      for (unsigned i = 0; i < n; ++i) d->held[cmds[i].addr] = cmds[i].data;
    }
  }
  std::this_thread::yield();
  d->inside.fetch_sub(1);
  return rc;
}
static void opSleep(void *, uint32_t) { std::this_thread::yield(); }
static void opReport(void *c, uint32_t e, uint32_t a, uint32_t) { static_cast<Client *>(c)->reports.push_back({e, a}); }

static void setup(Client &k, Drv2 &d, volatile uint32_t *words, volatile uint32_t *lock, uint32_t tag, unsigned client, uint32_t mmcxFloor)
{
  k.drv = &d;
  k.ops = {&k, opAddress, opLevel, opSend, opSleep, opReport};
  k.v = PianoMmVotes{};
  k.v.words = words;
  k.v.lock = lock;
  k.v.tag = tag;
  k.v.client = client;
  k.v.ops = &k.ops;
  k.v.floor[PIANO_MM_MMCX] = mmcxFloor;
  k.v.bound_ms = 50;
}

// ------------------------------------------------------------------------- PianoVideo's rules before the helper (the reference)
constexpr uint32_t kVlvlLowSvs = 0x40, kVlvlSvs = 0x80, kVlvlNomL1 = 0x140;
struct OldVotes {
  uint32_t floorVlvl;
  bool mxcSent = false, mmcxSent = false, mm1On = false;
  uint32_t mxcHlvl = 0, mmcxHlvl = 0;
};
static uint32_t lvl(unsigned r, uint32_t vlvl)
{
  uint32_t h = 0;
  opLevel(nullptr, r, vlvl, &h);
  return h;
}
static std::vector<PianoMmCmd> oldRailsUp(OldVotes &v)
{
  const uint32_t mxc = lvl(PIANO_MM_MXC, kVlvlSvs), mmcx = lvl(PIANO_MM_MMCX, std::max(kVlvlLowSvs, v.floorVlvl));
  std::vector<PianoMmCmd> c;
  if (!v.mxcSent || v.mxcHlvl != mxc) c.push_back({kMxcAddr, mxc});
  if (!v.mmcxSent || v.mmcxHlvl != mmcx) c.push_back({kMmcxAddr, mmcx});
  if (c.empty()) return c;
  v.mxcSent = v.mmcxSent = true;
  v.mxcHlvl = mxc;
  v.mmcxHlvl = mmcx;
  return c;
}
static std::vector<PianoMmCmd> oldRailsDown(OldVotes &v)
{
  const uint32_t mmcx = v.floorVlvl ? lvl(PIANO_MM_MMCX, v.floorVlvl) : 0;
  std::vector<PianoMmCmd> c;
  if (v.mxcSent && v.mxcHlvl != 0) c.push_back({kMxcAddr, 0});
  if (v.mmcxSent && v.mmcxHlvl != mmcx) c.push_back({kMmcxAddr, mmcx});
  if (c.empty()) return c;
  if (v.mxcSent) v.mxcHlvl = 0;
  if (v.mmcxSent) v.mmcxHlvl = mmcx;
  return c;
}
static std::vector<PianoMmCmd> oldBus(OldVotes &v, bool on)
{
  if (v.mm1On == on) return {};
  v.mm1On = on;
  return {{kMm1Addr, on ? PIANO_MM_BCM_ON : PIANO_MM_BCM_OFF}};
}
static bool sameCmds(const std::vector<PianoMmCmd> &a, const std::vector<PianoMmCmd> &b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].addr != b[i].addr || a[i].data != b[i].data) return false;
  return true;
}

// ------------------------------------------------------------------------------------------------------------------ tests
static void testOneUser()
{
  // Every sequence of up to six operations (rails up, rails down, bus on, bus off), floors NOM_L1 and 0.
  unsigned sequences = 0;
  for (uint32_t floor : {kVlvlNomL1, 0u, 0x30u})
    for (unsigned len = 1; len <= 6; ++len)
      for (unsigned code = 0; code < (1u << (2 * len)); ++code) {
        volatile uint32_t words[PIANO_MM_WORDS] = {};
        volatile uint32_t lock = 0;
        Drv2 d;
        Client video;
        setup(video, d, words, &lock, PianoMmTagVideo, PIANO_MM_VIDEO, floor);
        OldVotes old{floor};
        bool same = true;
        for (unsigned i = 0; i < len; ++i) {
          const unsigned op = (code >> (2 * i)) & 3u;
          std::vector<PianoMmCmd> want;
          const size_t before = d.log.size();
          int rc = 0;
          if (op == 0) {
            want = oldRailsUp(old);
            rc = piano_mm_votes_rails(&video.v, kVlvlSvs, kVlvlLowSvs);
          } else if (op == 1) {
            want = oldRailsDown(old);
            rc = piano_mm_votes_rails(&video.v, 0, 0);
          } else {
            want = oldBus(old, op == 2);
            rc = piano_mm_votes_bus(&video.v, op == 2);
          }
          const std::vector<PianoMmCmd> got = d.log.size() > before ? d.log.back() : std::vector<PianoMmCmd>{};
          same = same && rc == 0 && d.log.size() <= before + 1 && sameCmds(got, want);
        }
        CHECK(same);
        CHECK(lock == 0);
        ++sequences;
      }
  CHECK(sequences == 3 * (4 + 16 + 64 + 256 + 1024 + 4096));
}

static void testTwoUsers()
{
  volatile uint32_t words[PIANO_MM_WORDS] = {};
  volatile uint32_t lock = 0;
  Drv2 d;
  Client video, camera;
  setup(video, d, words, &lock, PianoMmTagVideo, PIANO_MM_VIDEO, kVlvlNomL1);
  setup(camera, d, words, &lock, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
  // The camera first: MXC low_svs, MMCX at the floor, MM1 on.
  CHECK(piano_mm_votes_rails(&camera.v, kVlvlLowSvs, kVlvlLowSvs) == 0);
  CHECK(d.held[kMxcAddr] == lvl(PIANO_MM_MXC, kVlvlLowSvs) && d.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
  CHECK(piano_mm_votes_bus(&camera.v, 1) == 0 && d.held[kMm1Addr] == PIANO_MM_BCM_ON);
  const size_t n0 = d.log.size();
  // The video core: MXC raised to svs; MMCX unchanged (one command); MM1 already on (nothing sent).
  CHECK(piano_mm_votes_rails(&video.v, kVlvlSvs, kVlvlLowSvs) == 0);
  CHECK(d.log.size() == n0 + 1 && d.log.back().size() == 1 && d.log.back()[0].addr == kMxcAddr);
  CHECK(d.held[kMxcAddr] == lvl(PIANO_MM_MXC, kVlvlSvs) && video.v.others[PIANO_MM_MXC] == kVlvlLowSvs);
  CHECK(piano_mm_votes_bus(&video.v, 1) == 0 && d.log.size() == n0 + 1 && video.v.unchanged == 1);
  // The video core powers off: MXC back to the camera's low_svs (not 0); MM1 stays on; MMCX stays at the floor.
  CHECK(piano_mm_votes_bus(&video.v, 0) == 0 && d.held[kMm1Addr] == PIANO_MM_BCM_ON && d.log.size() == n0 + 1);
  CHECK(piano_mm_votes_rails(&video.v, 0, 0) == 0 && d.held[kMxcAddr] == lvl(PIANO_MM_MXC, kVlvlLowSvs));
  CHECK(d.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
  CHECK(piano_mm_holders(&camera.v, PIANO_MM_MXC) == 1u << PIANO_MM_CAMERA);
  // The camera's release: MXC 0, MM1 off, MMCX at the floor (unchanged: nothing sent for it).
  const size_t n1 = d.log.size();
  CHECK(piano_mm_votes_bus(&camera.v, 0) == 0 && d.held[kMm1Addr] == PIANO_MM_BCM_OFF);
  CHECK(piano_mm_votes_rails(&camera.v, 0, 0) == 0 && d.held[kMxcAddr] == 0 && d.log.size() == n1 + 2 && d.log.back().size() == 1);
  CHECK(d.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
  CHECK(piano_mm_holders(&camera.v, PIANO_MM_MXC) == 0 && piano_mm_holders(&camera.v, PIANO_MM_MM1) == 0);
  CHECK(lock == 0 && words[PIANO_MM_TRANSACTIONS] == d.log.size());

  // The highest floor wins: the video core with piano-video-mmcx-floor=0 never takes MMCX below the camera's floor.
  {
    volatile uint32_t w2[PIANO_MM_WORDS] = {};
    volatile uint32_t l2 = 0;
    Drv2 e;
    Client v2, c2;
    setup(v2, e, w2, &l2, PianoMmTagVideo, PIANO_MM_VIDEO, 0);
    setup(c2, e, w2, &l2, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
    CHECK(piano_mm_votes_rails(&c2.v, kVlvlLowSvs, kVlvlLowSvs) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
    CHECK(piano_mm_votes_rails(&c2.v, 0, 0) == 0);
    CHECK(piano_mm_votes_rails(&v2.v, kVlvlSvs, kVlvlLowSvs) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
    CHECK(piano_mm_votes_rails(&v2.v, 0, 0) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1) && e.held[kMxcAddr] == 0);
    CHECK(w2[piano_mm_floor_index(PIANO_MM_MMCX)] == kVlvlNomL1);
  }
  // PianoCamera's actual needs (MXC low_svs, no MMCX need: the boot display's vote covers it): MMCX is never sent by the camera,
  // so the boot's DRV-2 MMCX vote stays exactly; the video core's later MMCX vote still honours the camera's declared floor.
  {
    volatile uint32_t w4[PIANO_MM_WORDS] = {};
    volatile uint32_t l4 = 0;
    Drv2 e;
    Client v4, c4;
    setup(v4, e, w4, &l4, PianoMmTagVideo, PIANO_MM_VIDEO, 0);
    setup(c4, e, w4, &l4, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
    CHECK(piano_mm_votes_rails(&c4.v, kVlvlLowSvs, 0) == 0 && e.log.size() == 1 && e.log[0].size() == 1 && e.log[0][0].addr == kMxcAddr);
    CHECK(piano_mm_votes_rails(&c4.v, 0, 0) == 0 && e.log.size() == 2 && e.log[1].size() == 1 && e.held[kMxcAddr] == 0 && !e.held.count(kMmcxAddr));
    CHECK(piano_mm_votes_rails(&v4.v, kVlvlSvs, kVlvlLowSvs) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
  }
  // A need above the floor: the maximum is voted, and the release goes back to the floor.
  {
    volatile uint32_t w3[PIANO_MM_WORDS] = {};
    volatile uint32_t l3 = 0;
    Drv2 e;
    Client v3, c3;
    setup(v3, e, w3, &l3, PianoMmTagVideo, PIANO_MM_VIDEO, kVlvlNomL1);
    setup(c3, e, w3, &l3, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
    CHECK(piano_mm_votes_rails(&c3.v, 0, 0x180) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, 0x180));
    CHECK(piano_mm_votes_rails(&v3.v, kVlvlSvs, kVlvlLowSvs) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, 0x180));
    CHECK(piano_mm_votes_rails(&c3.v, 0, 0) == 0 && e.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
    CHECK(e.held[kMxcAddr] == lvl(PIANO_MM_MXC, kVlvlSvs));
    // MM1 with bandwidth needs: x summed, y the maximum.
    const uint32_t a[PIANO_MM_RESOURCES] = {0, 0, PIANO_MM_BCM_COMMIT | PIANO_MM_BCM_VALID | 3u << 14 | 5u};
    const uint32_t b[PIANO_MM_RESOURCES] = {0, 0, PIANO_MM_BCM_COMMIT | PIANO_MM_BCM_VALID | 4u << 14 | 2u};
    CHECK(piano_mm_votes_update(&c3.v, 1u << PIANO_MM_MM1, a) == 0 && piano_mm_votes_update(&v3.v, 1u << PIANO_MM_MM1, b) == 0);
    CHECK(e.held[kMm1Addr] == (PIANO_MM_BCM_COMMIT | PIANO_MM_BCM_VALID | 7u << 14 | 5u));
  }
}

static void testFailures()
{
  volatile uint32_t words[PIANO_MM_WORDS] = {};
  volatile uint32_t lock = 0;
  Drv2 d;
  Client video, camera;
  setup(video, d, words, &lock, PianoMmTagVideo, PIANO_MM_VIDEO, kVlvlNomL1);
  setup(camera, d, words, &lock, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
  // A refused transaction: the words as they were, nothing recorded as sent; the retry sends.
  d.failNext = 1;
  CHECK(piano_mm_votes_rails(&camera.v, kVlvlLowSvs, kVlvlLowSvs) == PianoMmSend);
  CHECK(words[piano_mm_need_index(PIANO_MM_MXC, PIANO_MM_CAMERA)] == 0 && words[PIANO_MM_SENT_VALID] == 0 && d.log.empty());
  CHECK(words[PIANO_MM_LAST_ERROR] == uint32_t(PianoMmSend) && camera.v.failed == 1 && lock == 0);
  // After a failure that never sent, a release sends nothing (nothing of ours is held).
  CHECK(piano_mm_votes_rails(&camera.v, 0, 0) == 0 && d.log.empty());
  CHECK(piano_mm_votes_rails(&camera.v, kVlvlLowSvs, kVlvlLowSvs) == 0 && d.log.size() == 1 && d.log[0].size() == 2);
  // A failed release keeps the need (and the vote): the next release retries.
  d.failNext = 1;
  CHECK(piano_mm_votes_rails(&camera.v, 0, 0) == PianoMmSend && words[piano_mm_need_index(PIANO_MM_MXC, PIANO_MM_CAMERA)] == kVlvlLowSvs);
  CHECK(piano_mm_votes_rails(&camera.v, 0, 0) == 0 && d.held[kMxcAddr] == 0);
  // No level reaches the need: nothing sent, the needs unchanged.
  const size_t n = d.log.size();
  CHECK(piano_mm_votes_rails(&video.v, 0x400, kVlvlLowSvs) == PianoMmNoLevel && d.log.size() == n);
  CHECK(words[piano_mm_need_index(PIANO_MM_MXC, PIANO_MM_VIDEO)] == 0);
  // A resource without a cmd-db address.
  video.noAddress[PIANO_MM_MM1] = true;
  CHECK(piano_mm_votes_bus(&video.v, 1) == PianoMmNoResource && d.log.size() == n);
  video.noAddress[PIANO_MM_MM1] = false;
  // The lock held by another kext past the bound: busy, nothing changed.
  lock = 0x12345678;
  CHECK(piano_mm_votes_bus(&video.v, 1) == PianoMmBusy && video.v.busy == 1 && d.log.size() == n && lock == 0x12345678);
  bool waited = false;
  for (auto &[e, a] : video.reports) waited = waited || (e == PianoMmLockWaiting && a == 0x12345678);
  CHECK(waited);
  lock = 0;
  // A lock taken from under us (someone stored over our tag): reported as lost.
  {
    PianoMmVotesOps ops = video.ops;
    ops.send = [](void *c, const PianoMmCmd *cmds, unsigned k) {
      *static_cast<Client *>(c)->v.lock = 0x99;
      return opSend(c, cmds, k);
    };
    video.v.ops = &ops;
    CHECK(piano_mm_votes_bus(&video.v, 1) == 0 && video.v.lost == 1 && lock == 0x99);
    video.v.ops = &video.ops;
    lock = 0;
  }
  // Bad arguments.
  const uint32_t none[PIANO_MM_RESOURCES] = {0, 0, 0}, bad[PIANO_MM_RESOURCES] = {0, 0, 0x80000001u};
  CHECK(piano_mm_votes_update(&video.v, 0, none) == PianoMmArgument);
  CHECK(piano_mm_votes_update(&video.v, 8, none) == PianoMmArgument);
  CHECK(piano_mm_votes_update(&video.v, 1u << PIANO_MM_MM1, bad) == PianoMmArgument);
  CHECK(piano_mm_votes_update(nullptr, 1, none) == PianoMmArgument);
  PianoMmVotes broken = video.v;
  broken.client = PIANO_MM_CLIENTS;
  CHECK(piano_mm_votes_update(&broken, 1, none) == PianoMmArgument);
  broken = video.v;
  broken.tag = 0;
  CHECK(piano_mm_votes_update(&broken, 1, none) == PianoMmArgument);
}

static void testThreads()
{
  volatile uint32_t words[PIANO_MM_WORDS] = {};
  volatile uint32_t lock = 0;
  Drv2 d;
  Client video, camera;
  setup(video, d, words, &lock, PianoMmTagVideo, PIANO_MM_VIDEO, kVlvlNomL1);
  setup(camera, d, words, &lock, PianoMmTagCamera, PIANO_MM_CAMERA, kVlvlNomL1);
  video.v.bound_ms = camera.v.bound_ms = 1000000;
  std::atomic<int> errors{0};
  auto loop = [&](Client *k, uint32_t mxc, unsigned seed) {
    for (unsigned i = 0; i < 3000; ++i) {
      const unsigned op = (i * 7 + seed) % 4;
      int rc = op == 0 ? piano_mm_votes_rails(&k->v, mxc, kVlvlLowSvs) : op == 1 ? piano_mm_votes_rails(&k->v, 0, 0)
                                                                      : piano_mm_votes_bus(&k->v, op == 2);
      if (rc) errors++;
    }
    piano_mm_votes_rails(&k->v, 0, 0);
    piano_mm_votes_bus(&k->v, 0);
  };
  std::thread a(loop, &video, kVlvlSvs, 1u), b(loop, &camera, kVlvlLowSvs, 2u);
  a.join();
  b.join();
  CHECK(errors == 0 && d.overlaps == 0 && lock == 0);
  // Both released: MXC 0, MM1 off, MMCX at the floor; and what DRV-2 holds is what the words say was sent.
  CHECK(d.held[kMxcAddr] == 0 && d.held[kMm1Addr] == PIANO_MM_BCM_OFF && d.held[kMmcxAddr] == lvl(PIANO_MM_MMCX, kVlvlNomL1));
  CHECK(words[piano_mm_sent_index(PIANO_MM_MXC)] == d.held[kMxcAddr] && words[piano_mm_sent_index(PIANO_MM_MM1)] == d.held[kMm1Addr]);
  CHECK(words[PIANO_MM_UPDATES] == video.v.updates + camera.v.updates && words[PIANO_MM_TRANSACTIONS] == d.log.size());
}

// A synthetic cmd-db: slave 3 (ARC) with mxc.lvl, mmcx.lvl and a dummy ARC; slave 5 (BCM) with MM1.
static std::vector<uint8_t> cmdDb()
{
  std::vector<uint8_t> db(4096, 0);
  auto w16 = [&](size_t at, uint32_t v) {
    db[at] = uint8_t(v);
    db[at + 1] = uint8_t(v >> 8);
  };
  w16(4, 0x30db);
  w16(6, 0x0c03);
  // slave records at 8: id, table offset, data offset, count (offsets from byte 144)
  w16(8, 3), w16(10, 0), w16(12, 512), w16(14, 3);
  w16(24, 5), w16(26, 256), w16(28, 1024), w16(30, 1);
  auto entry = [&](size_t at, const char *name, uint32_t addr, uint32_t auxLen, uint32_t auxOff) {
    for (unsigned i = 0; i < 8 && name[i]; ++i) db[at + i] = uint8_t(name[i]);
    w16(at + 16, addr & 0xffff);
    w16(at + 18, addr >> 16);
    w16(at + 20, auxLen);
    w16(at + 22, auxOff);
  };
  entry(144 + 0, "mxc.lvl", kMxcAddr, 20, 0);
  entry(144 + 24, "mmcx.lvl", kMmcxAddr, 32, 32);
  entry(144 + 48, "dummy.lv", 0x30030, 2, 64);
  entry(144 + 256, "MM1", kMm1Addr, 0, 0);
  for (unsigned i = 0; i < 10; ++i) w16(144 + 512 + 2 * i, kMxcTable[i]);
  for (unsigned i = 0; i < 10; ++i) w16(144 + 512 + 32 + 2 * i, kMmcxTable[i]);   // 16 slots, the last 6 zero padding
  return db;
}

static void testCmdDb()
{
  const std::vector<uint8_t> db = cmdDb();
  PianoMmArc mxc{}, mmcx{}, dummy{};
  CHECK(piano_mm_cmd_db_arc(db.data(), uint32_t(db.size()), "mxc.lvl", &mxc) == 0 && mxc.addr == kMxcAddr && mxc.count == 10);
  CHECK(piano_mm_cmd_db_arc(db.data(), uint32_t(db.size()), "mmcx.lvl", &mmcx) == 0 && mmcx.count == 10 && mmcx.vlvl[8] == 0x140);
  CHECK(piano_mm_cmd_db_arc(db.data(), uint32_t(db.size()), "dummy.lv", &dummy) == -3);
  uint32_t a = 0, h = 0;
  CHECK(piano_mm_cmd_db(db.data(), uint32_t(db.size()), "MM1", PIANO_MM_SLAVE_BCM, &a, nullptr, nullptr) == 0 && a == kMm1Addr);
  CHECK(piano_mm_cmd_db(db.data(), uint32_t(db.size()), "MM1", PIANO_MM_SLAVE_ARC, &a, nullptr, nullptr) == -3);
  CHECK(piano_mm_cmd_db(db.data(), uint32_t(db.size()), "MM0", PIANO_MM_SLAVE_BCM, &a, nullptr, nullptr) == -2);
  CHECK(piano_mm_cmd_db_arc(db.data(), 100, "mxc.lvl", &mxc) == -1);
  CHECK(piano_mm_arc_level(&mmcx, 0x140, &h) == 0 && h == 8);
  CHECK(piano_mm_arc_level(&mmcx, 0x41, &h) == 0 && h == 4);
  CHECK(piano_mm_arc_level(&mmcx, 0, &h) == 0 && h == 0);
  CHECK(piano_mm_arc_level(&mmcx, 0x181, &h) == -3);
}

int main()
{
  testOneUser();
  testTwoUsers();
  testFailures();
  testThreads();
  testCmdDb();
  if (failures) {
    std::printf("mm_votes_host: %d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("mm_votes_host: all checks passed\n");
  return 0;
}
