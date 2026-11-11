// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoGpuIdle.h (docs/power.md "Idle wakeups by driver"): the mailbox server's wait (the poll of before with
// piano-gpu-mailbox-poll=1; the doorbell trusted after enough rung requests and distrusted when they stop ringing; the display-off
// wait), and the DCVS screen rule's word.
#include "PianoGpuIdle.h"
#include <cstdio>
#include <cstdlib>

using namespace pianogpu;

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

int main()
{
  // The poll of before: 16 ms idle, 1 ms for a second after a request, whatever the display or the doorbell.
  MailboxPace old;
  old.poll = true;
  CHECK(old.waitMs(0, false) == 16 && old.waitMs(0, true) == 16);
  for (unsigned i = 0; i < 100; ++i) old.request(1000 + i, true);
  CHECK(!old.trusted());
  CHECK(old.waitMs(2000, false) == 1 && old.waitMs(2000, true) == 1);
  CHECK(old.waitMs(1000 + 99 + kMailboxActiveUs, false) == 16);

  // Default, no request yet: 16 ms lit, the dark wait with the display off.
  MailboxPace p;
  CHECK(p.waitMs(0, false) == 16 && p.waitMs(0, true) == kMailboxDarkMsDefault);
  // Requests without the doorbell: never trusted; 1 ms for a second after each (lit and dark), then 16 / 250.
  uint64_t t = 1000;
  for (unsigned i = 0; i < 50; ++i, t += 100000) CHECK(!p.request(t, false));
  CHECK(!p.trusted());
  CHECK(p.waitMs(t, false) == 1 && p.waitMs(t, true) == 1);
  CHECK(p.waitMs(t + kMailboxActiveUs, false) == 16 && p.waitMs(t + kMailboxActiveUs, true) == kMailboxDarkMsDefault);

  // Requests with the doorbell: trusted at the 8th, then a look every 100 ms lit (250 dark), active or not.
  MailboxPace d;
  t = 1000;
  for (unsigned i = 1; i < kTrustAfter; ++i, t += 100000) CHECK(!d.request(t, true) && !d.trusted());
  CHECK(d.waitMs(t, false) == 1);
  CHECK(d.request(t, true) && d.trusted());
  CHECK(d.waitMs(t, false) == kTrustedMs && d.waitMs(t + 10 * kMailboxActiveUs, false) == kTrustedMs);
  CHECK(d.waitMs(t, true) == kMailboxDarkMsDefault);
  // An occasional race (a request found just before its ring): still trusted while at most one in 64.
  for (unsigned i = 0; i < 64 * 2; ++i, t += 1000) d.request(t, true);
  CHECK(!d.request(t, false) && d.trusted());
  // The plugin stops ringing: distrusted within a few requests, the poll of before again.
  bool changed = false;
  for (unsigned i = 0; i < 8 && !changed; ++i, t += 1000) changed = d.request(t, false);
  CHECK(changed && !d.trusted() && d.waitMs(t, false) == 1);

  // piano-gpu-mailbox-dark-ms=0: the dark wait is the lit one (16 ms; trusted: 100 ms).
  MailboxPace z;
  z.darkMs = 0;
  CHECK(z.waitMs(0, true) == 16);
  for (unsigned i = 0; i < kTrustAfter; ++i) z.request(i, true);
  CHECK(z.trusted() && z.waitMs(10, true) == kTrustedMs);
  // A dark wait below the trusted one never shortens it.
  MailboxPace s;
  s.darkMs = 50;
  for (unsigned i = 0; i < kTrustAfter; ++i) s.request(i, true);
  CHECK(s.waitMs(10, true) == kTrustedMs);
  CHECK(clampDarkMs(5000) == kMailboxDarkMsMax && clampDarkMs(250) == 250);

  // The screen word: bit 0 off, the changes counted above it; a repeat changes nothing.
  uint32_t w = 0;
  w = screenWord(w, false);
  CHECK(w == 0);
  w = screenWord(w, true);
  CHECK(w == 3);
  CHECK(screenWord(w, true) == 3);
  w = screenWord(w, false);
  CHECK(w == 4);
  w = screenWord(w, true);
  CHECK(w == 7 && (w & 1u) && (w >> 1) == 3);

  std::printf("PASS test_gpu_idle: %u checks\n", checks);
  return 0;
}
