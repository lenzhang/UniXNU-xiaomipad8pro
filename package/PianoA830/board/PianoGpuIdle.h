// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoA830's idle wakeups, the portable part (docs/power.md "Idle wakeups by driver"; host test board/tests/test_gpu_idle.cpp).
//
// The render server's mailbox (NativeAdrenoPublish.cpp nativeAdrenoMailboxPoller): WindowServer's Metal plugin writes a request
// into a shared page; the kernel's server thread finds it by polling, every 1 ms for a second after each request, else every 16 ms
// (before 2026-11-12). At the login window WindowServer asks about ten times a second, so the poll never left its 1 ms mode: about
// 1000 wakeups a second while the display is on. The plugin may also ring the mailbox's doorbell (the marker 0x7db0), which wakes the
// server at once. MailboxPace decides the wait:
//   - piano-gpu-mailbox-poll=1: the poll of before, exactly (1 ms after a request, else 16 ms), whatever the doorbell and the display;
//   - else, while the requests come with the doorbell (at least kTrustAfter of them rung and at most one in kTrustRatio not): a look
//     every kTrustedMs only, the doorbell does the rest; a request found without a ring counts against it, so a plugin that stops
//     ringing is polled as before within a few requests;
//   - else the poll of before, but with the display off (NativeGopDisplay's piano-display-power, which the board follows)
//     piano-gpu-mailbox-dark-ms (default 250, 0 keeps 16) when no request came for a second.
// The DCVS screen rule's word (pagani_screen_state: bit 0 the display off, the count of changes above it): ScreenWord.
#ifndef PIANO_GPU_IDLE_H
#define PIANO_GPU_IDLE_H

#include <stdint.h>

namespace pianogpu {

constexpr uint32_t kMailboxActiveMs = 1, kMailboxIdleMs = 16, kMailboxActiveUs = 1000000;
constexpr uint32_t kMailboxDarkMsDefault = 250, kMailboxDarkMsMax = 1000, kTrustedMs = 100;
constexpr uint32_t kTrustAfter = 8, kTrustRatio = 64;

struct MailboxPace {
  bool poll = false;                       // piano-gpu-mailbox-poll=1
  uint32_t darkMs = kMailboxDarkMsDefault; // piano-gpu-mailbox-dark-ms
  uint64_t activeUntilUs = 0;
  uint32_t rung = 0, unrung = 0;           // requests found with a doorbell since the last look, and without one

  bool trusted() const { return !poll && rung >= kTrustAfter && uint64_t(unrung) * kTrustRatio <= rung; }

  // A request found at NOWUS; RANG: the doorbell rang since the server's previous look. True when trusted() changed.
  bool request(uint64_t nowUs, bool rang)
  {
    const bool before = trusted();
    if (rang) ++rung;
    else ++unrung;
    activeUntilUs = nowUs + kMailboxActiveUs;
    return trusted() != before;
  }

  // The server's next wait in ms (the doorbell ends it early).
  uint32_t waitMs(uint64_t nowUs, bool dark) const
  {
    const bool active = nowUs < activeUntilUs;
    if (poll) return active ? kMailboxActiveMs : kMailboxIdleMs;
    const uint32_t darkWait = dark && darkMs ? darkMs : 0;
    if (trusted()) return darkWait > kTrustedMs ? darkWait : kTrustedMs;
    if (active) return kMailboxActiveMs;
    return darkWait ? darkWait : kMailboxIdleMs;
  }
};

inline uint32_t clampDarkMs(uint32_t ms) { return ms > kMailboxDarkMsMax ? kMailboxDarkMsMax : ms; }

// pagani_screen_state after the display's state became OFF (true) or on: bit 0 the state, a change counted above it.
inline uint32_t screenWord(uint32_t before, bool off)
{
  if (((before & 1u) != 0) == off) return before;
  return ((((before >> 1) + 1u) << 1) & ~1u) | (off ? 1u : 0u);
}

}  // namespace pianogpu

#endif
