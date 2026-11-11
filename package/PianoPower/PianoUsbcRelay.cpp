// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB-C relay's kernel glue (UsbcRelay.h; the contract is package/include/PianoUsbcContract.h): the piano_usbc_* functions a client
// kext calls from its threads, and the three hooks PianoPower's own thread calls (step after each pass, receive before PianoMca sees a
// message, pending in its wait). One spin lock guards the core; callbacks run on PianoPower's thread with it dropped, counted in busy
// so a detach can wait them out. Nothing here touches the charger owner or the DSP: requests go out through piano_adsp_send on the
// channel PianoPower passes in.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
extern "C" {
#include "PianoAdsp.h"
#include "UsbcRelay.h"
}

namespace {
UsbcRelay gRelay;                 // zero: no client (usbc_relay_attach sets what a client needs)
IOSimpleLock *volatile gLock;
const void *volatile gWake;       // PianoPower's wait event (set by the first step)
thread_t volatile gOwner;         // PianoPower's thread (set by the first step)

// The lock, made by whoever needs it first (a client may attach before PianoPower's first step).
IOSimpleLock *lock()
{
  IOSimpleLock *l = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
  if (l) return l;
  IOSimpleLock *fresh = IOSimpleLockAlloc();
  if (!fresh) return nullptr;
  IOSimpleLock *expected = nullptr;
  if (__atomic_compare_exchange_n(&gLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
  IOSimpleLockFree(fresh);
  return expected;
}

void wakeOwner()
{
  if (const void *w = __atomic_load_n(&gWake, __ATOMIC_ACQUIRE)) thread_wakeup((event_t)w);
}

struct Held {
  IOSimpleLock *l;
  Held() : l(lock()) { if (l) IOSimpleLockLock(l); }
  ~Held() { if (l) IOSimpleLockUnlock(l); }
};
}  // namespace

extern "C" int piano_usbc_attach(const PianoUsbcClient *client, void *context)
{
  int r;
  {
    Held h;
    if (!h.l) return -2;
    r = usbc_relay_attach(&gRelay, client, context);
  }
  if (!r) wakeOwner();   // a ready owner tells the client at its next step
  return r;
}

extern "C" void piano_usbc_detach(void)
{
  {
    Held h;
    if (!h.l || !gRelay.attached) return;
    gRelay.closing = 1;   // no new callback starts
  }
  // A callback in progress returns first, unless this is that callback (PianoPower's thread).
  if (current_thread() != __atomic_load_n(&gOwner, __ATOMIC_ACQUIRE))
    for (;;) {
      uint32_t busy;
      {
        Held h;
        busy = gRelay.busy;
      }
      if (!busy) break;
      IOSleep(1);
    }
  Held h;
  usbc_relay_detach(&gRelay);
}

extern "C" int piano_usbc_send(const void *data, uint32_t length)
{
  int r;
  {
    Held h;
    if (!h.l) return -4;
    r = usbc_relay_queue(&gRelay, static_cast<const uint8_t *>(data), length);
  }
  if (!r) wakeOwner();
  return r;
}

extern "C" void piano_usbc_status(PianoUsbcStatus *out)
{
  if (!out) return;
  Held h;
  usbc_relay_status(&gRelay, out);
}

// ---------------------------------------------------------------------------------------------------- PianoPower's hooks
extern "C" void piano_usbc_relay_step(PianoAdspChannel *channel, int ready, uint32_t generation, const void *wake)
{
  __atomic_store_n(&gWake, wake, __ATOMIC_RELEASE);
  __atomic_store_n(&gOwner, current_thread(), __ATOMIC_RELEASE);
  PianoUsbcClient client = {};
  void *context = nullptr;
  bool tell = false;
  uint32_t told = 0;
  {
    Held h;
    if (!h.l) return;
    gRelay.kick = 0;
    if (usbc_relay_link(&gRelay, ready, generation)) {
      tell = true;
      client = gRelay.client;
      context = gRelay.context;
      told = gRelay.generation;
      gRelay.busy++;
    }
  }
  if (tell) {
    if (client.state) client.state(context, ready != 0, told);
    Held h;
    usbc_relay_told(&gRelay);
    gRelay.busy--;
  }
  if (!channel) return;
  // The queue, in order; a full channel queue keeps the request for the next pass (PianoPower's thread wakes at least every 5 s while
  // the charger is linked).
  for (unsigned n = 0; n < PIANO_USBC_QUEUE; ++n) {
    uint8_t out[PIANO_USBC_MAX_MESSAGE];
    uint32_t length = 0;
    {
      Held h;
      const uint8_t *data = nullptr;
      length = usbc_relay_peek(&gRelay, &data);
      if (length) memcpy(out, data, length);
    }
    if (!length) break;
    const int r = piano_adsp_send(channel, out, length);
    Held h;
    if (r) {
      gRelay.send_failures++;
      break;
    }
    usbc_relay_pop(&gRelay);
  }
}

extern "C" int piano_usbc_relay_receive(const uint8_t *data, uint32_t length)
{
  PianoUsbcClient client = {};
  void *context = nullptr;
  {
    Held h;
    if (!h.l) return 0;
    if (usbc_relay_classify(&gRelay, data, length) == USBC_MINE_NONE) return 0;
    client = gRelay.client;
    context = gRelay.context;
    gRelay.busy++;
    gRelay.delivered++;
  }
  client.ucsi(context, data, length);
  Held h;
  gRelay.busy--;
  return 1;
}

extern "C" int piano_usbc_relay_pending(void)
{
  Held h;
  return h.l && gRelay.kick;
}

extern "C" int piano_usbc_relay_attached(void)
{
  Held h;
  return h.l && gRelay.attached && !gRelay.closing;
}
