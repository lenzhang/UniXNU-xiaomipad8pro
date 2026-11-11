// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the secure-call lock protocol (package/include/PianoScmLock.h, docs/scm-lock.md):
//   - take/give: the word goes to the kext's tag and back; nested holds of one kext only count; the last give frees the word;
//   - waiting: a word held by another kext is waited for in 1 ms sleeps, reported at the first wait, every second and when it
//     comes free; the wait is recorded (contended, max_wait_ms);
//   - the bound: past bound_ms the waiter takes the word over (reported, counted), the old holder's give is then counted as lost
//     and leaves the word alone; an unbalanced give is counted and changes nothing; bad arguments are refused;
//   - real threads: two kexts (their own tags and hold mutexes, one shared word), two threads each, every thread doing sequences
//     with nested calls inside and lone calls: never are both kexts inside at once, inside one kext both threads may be, and the
//     word is free at the end.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. scm_lock_host.cpp -o /tmp/scm_lock_host && /tmp/scm_lock_host
#include "PianoScmLock.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------- a single-threaded model
struct Event {
  uint32_t event, other, waited;
};
struct Model {
  volatile uint32_t *word = nullptr;
  uint32_t slept = 0;                        // total ms slept
  int mutexDepth = 0, mutexMax = 0;
  std::vector<Event> events;
  std::function<void(uint32_t)> onSleep;     // what the other kext does meanwhile (by total ms slept)
  PianoScmLockOps ops{};
  PianoScmLock lock{};
  Model(volatile uint32_t *w, uint32_t tag, uint32_t bound = 0)
  {
    word = w;
    ops = {this,
           [](void *c, uint32_t ms) {
             auto *m = static_cast<Model *>(c);
             m->slept += ms;
             if (m->onSleep) m->onSleep(m->slept);
           },
           [](void *c) {
             auto *m = static_cast<Model *>(c);
             if (++m->mutexDepth > m->mutexMax) m->mutexMax = m->mutexDepth;
           },
           [](void *c) { --static_cast<Model *>(c)->mutexDepth; },
           [](void *c, uint32_t e, uint32_t o, uint32_t w) { static_cast<Model *>(c)->events.push_back({e, o, w}); }};
    lock = {w, tag, bound, &ops, 0, 0, 0, 0, 0, 0, 0};
  }
};

static void testTakeGive()
{
  volatile uint32_t word = 0;
  Model a(&word, PianoScmTagA830);
  CHECK(piano_scm_lock_take(&a.lock) == PianoScmLockOk && word == PianoScmTagA830 && a.lock.holds == 1);
  CHECK(piano_scm_lock_take(&a.lock) == PianoScmLockOk && a.lock.holds == 2);   // a call inside the kext's own sequence
  piano_scm_lock_give(&a.lock);
  CHECK(word == PianoScmTagA830 && a.lock.holds == 1);
  piano_scm_lock_give(&a.lock);
  CHECK(word == 0 && a.lock.holds == 0 && a.lock.takes == 1);
  CHECK(a.slept == 0 && a.events.empty() && a.mutexDepth == 0 && a.mutexMax == 1);
  CHECK(piano_scm_lock_owner(&a.lock) == 0);
  // Unbalanced: counted, the word untouched (another kext's hold stays).
  word = PianoScmTagPower;
  piano_scm_lock_give(&a.lock);
  CHECK(word == PianoScmTagPower && a.lock.unbalanced == 1 && a.events.size() == 1 && a.events[0].event == PianoScmLockUnbalanced);
  // Bad arguments.
  CHECK(piano_scm_lock_take(nullptr) == PianoScmLockArgument);
  PianoScmLock bad = a.lock;
  bad.tag = 0;
  CHECK(piano_scm_lock_take(&bad) == PianoScmLockArgument);
  bad = a.lock;
  bad.word = nullptr;
  CHECK(piano_scm_lock_take(&bad) == PianoScmLockArgument);
  PianoScmLockOps noSleep = a.ops;
  noSleep.sleep_ms = nullptr;
  bad = a.lock;
  bad.ops = &noSleep;
  CHECK(piano_scm_lock_take(&bad) == PianoScmLockArgument);
  piano_scm_lock_give(nullptr);   // no crash
}

static void testWait()
{
  volatile uint32_t word = PianoScmTagPower;   // the DSP's PAS sequence holds it
  Model v(&word, PianoScmTagVideo);
  v.onSleep = [&](uint32_t ms) {
    if (ms == 2500) word = 0;   // PianoPower gives it back after 2.5 s
  };
  CHECK(piano_scm_lock_take(&v.lock) == PianoScmLockOk);
  CHECK(word == PianoScmTagVideo && v.slept == 2500 && v.lock.contended == 1 && v.lock.max_wait_ms == 2500);
  // Reported: the first wait, at 1 s and 2 s, and the end.
  CHECK(v.events.size() == 4);
  if (v.events.size() == 4) {
    CHECK(v.events[0].event == PianoScmLockWaiting && v.events[0].other == PianoScmTagPower && v.events[0].waited == 0);
    CHECK(v.events[1].event == PianoScmLockStillWaiting && v.events[1].waited == 1000);
    CHECK(v.events[2].event == PianoScmLockStillWaiting && v.events[2].waited == 2000);
    CHECK(v.events[3].event == PianoScmLockGot && v.events[3].other == PianoScmTagPower && v.events[3].waited == 2500);
  }
  CHECK(v.mutexDepth == 1 || v.mutexDepth == 0);
  CHECK(v.mutexDepth == 0);   // the hold mutex is back
  piano_scm_lock_give(&v.lock);
  CHECK(word == 0);
  // A word that changes owner while we wait is still waited for (another kext took it in between).
  word = PianoScmTagA830;
  Model p(&word, PianoScmTagPower);
  p.onSleep = [&](uint32_t ms) {
    if (ms == 10) word = PianoScmTagVideo;
    if (ms == 20) word = 0;
  };
  CHECK(piano_scm_lock_take(&p.lock) == PianoScmLockOk && p.slept == 20 && word == PianoScmTagPower);
  CHECK(!p.events.empty() && p.events.back().event == PianoScmLockGot && p.events.back().other == PianoScmTagVideo);
  piano_scm_lock_give(&p.lock);
}

static void testBound()
{
  volatile uint32_t word = PianoScmTagA830;   // leaked: never given back
  Model p(&word, PianoScmTagPower, 50);
  CHECK(piano_scm_lock_take(&p.lock) == PianoScmLockTakenOver);
  CHECK(word == PianoScmTagPower && p.slept == 50 && p.lock.taken_over == 1 && p.lock.holds == 1);
  CHECK(!p.events.empty() && p.events.back().event == PianoScmLockTookOver && p.events.back().other == PianoScmTagA830 &&
        p.events.back().waited == 50);
  // The old holder (A830, one hold) gives back: counted as lost, our hold stays.
  Model a(&word, PianoScmTagA830);
  a.lock.holds = 1;
  piano_scm_lock_give(&a.lock);
  CHECK(word == PianoScmTagPower && a.lock.lost == 1 && a.lock.holds == 0);
  CHECK(!a.events.empty() && a.events.back().event == PianoScmLockLost && a.events.back().other == PianoScmTagPower);
  piano_scm_lock_give(&p.lock);
  CHECK(word == 0);
  // The default bound is 30 s.
  word = PianoScmTagVideo;
  Model d(&word, PianoScmTagA830);
  CHECK(piano_scm_lock_take(&d.lock) == PianoScmLockTakenOver && d.slept == PianoScmLockDefaultBoundMs);
  unsigned still = 0;
  for (const Event &e : d.events) still += e.event == PianoScmLockStillWaiting;
  CHECK(still == PianoScmLockDefaultBoundMs / PianoScmLockReportMs - 1);
  piano_scm_lock_give(&d.lock);
  CHECK(word == 0);
}

// ---------------------------------------------------------------------------------------------------------------- threads
struct Kext {
  std::mutex mutex;
  PianoScmLockOps ops{};
  PianoScmLock lock{};
  std::atomic<int> inside{0};
};

static void testThreads()
{
  volatile uint32_t word = 0;
  Kext kexts[2];
  const uint32_t tags[2] = {PianoScmTagA830, PianoScmTagPower};
  for (unsigned i = 0; i < 2; ++i) {
    kexts[i].ops = {&kexts[i],
                    [](void *, uint32_t ms) { std::this_thread::sleep_for(std::chrono::microseconds(100 * ms)); },
                    [](void *c) { static_cast<Kext *>(c)->mutex.lock(); },
                    [](void *c) { static_cast<Kext *>(c)->mutex.unlock(); },
                    nullptr};
    kexts[i].lock = {&word, tags[i], 0, &kexts[i].ops, 0, 0, 0, 0, 0, 0, 0};
  }
  std::atomic<int> overlaps{0}, sameKext{0}, notOwner{0};
  auto enter = [&](unsigned k) {
    piano_scm_lock_take(&kexts[k].lock);
    if (word != tags[k]) notOwner++;
    if (kexts[k].inside.fetch_add(1) > 0) sameKext++;   // allowed: the kext's other thread
    if (kexts[1 - k].inside.load()) overlaps++;
  };
  auto leave = [&](unsigned k) {
    kexts[k].inside.fetch_sub(1);
    piano_scm_lock_give(&kexts[k].lock);
  };
  auto pause = [] { std::this_thread::sleep_for(std::chrono::microseconds(20)); };
  auto worker = [&](unsigned k, unsigned seed) {
    for (unsigned i = 0; i < 300; ++i) {
      if ((i + seed) % 3 == 0) {   // a sequence: one take around three nested calls
        enter(k);
        for (unsigned c = 0; c < 3; ++c) {
          enter(k);
          pause();
          leave(k);
        }
        leave(k);
      } else {                      // a lone call
        enter(k);
        pause();
        leave(k);
      }
      pause();                      // outside: the other kext's turn
    }
  };
  // One thread per kext (every take contends with the other kext), then two per kext (calls of one kext overlap inside it).
  for (unsigned perKext : {1u, 2u}) {
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < 2; ++k)
      for (unsigned t = 0; t < perKext; ++t) threads.emplace_back(worker, k, t);
    for (auto &t : threads) t.join();
  }
  CHECK(kexts[0].lock.contended + kexts[1].lock.contended > 0);
  CHECK(overlaps == 0);
  CHECK(notOwner == 0);
  CHECK(word == 0 && kexts[0].lock.holds == 0 && kexts[1].lock.holds == 0);
  CHECK(kexts[0].lock.taken_over == 0 && kexts[1].lock.taken_over == 0 && kexts[0].lock.lost == 0 && kexts[1].lock.lost == 0);
  CHECK(kexts[0].lock.unbalanced == 0 && kexts[1].lock.unbalanced == 0);
  std::printf("  threads: %u + %u takes, %u + %u contended, max wait %u/%u ms, %d nested in-kext overlaps (allowed)\n",
              kexts[0].lock.takes, kexts[1].lock.takes, kexts[0].lock.contended, kexts[1].lock.contended, kexts[0].lock.max_wait_ms,
              kexts[1].lock.max_wait_ms, sameKext.load());
}

int main()
{
  testTakeGive();
  testWait();
  testBound();
  testThreads();
  if (failures) {
    std::printf("PianoScmLock host tests: %d failed\n", failures);
    return 1;
  }
  std::printf("PianoScmLock host tests: all passed\n");
  return 0;
}
