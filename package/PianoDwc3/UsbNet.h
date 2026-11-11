// SPDX-License-Identifier: BSD-2-Clause-Patent
// Between PianoDwc3.cpp (the USB thread and the gadget core) and PianoUsbNet.cpp (the IOEthernetController; IONetworkingFamily's
// headers stay in that file): the frame queues both ways, the link state, the counters. piano-usb-net=1 only. Kernel only.
#pragma once
#include <stdint.h>
#include "Ncm.h"

class IOService;

namespace pianonet {

// The network side's own counts (the NTB counts are the gadget core's ncm::NetStats). Frames are never logged, only counted.
struct Counters {
  uint64_t sent, sentBytes, sendDrops, sendRefused;            // from macOS to the Mac: queued; queue full or link down; bad length
  uint64_t received, receivedBytes, receiveDrops, notUp;       // from the Mac to macOS: delivered; no mbuf; interface not up
};

// From PianoDwc3::start, before the USB thread: the queues and the lock (false: no memory, no network function).
bool init(const pianousb::ncm::NetStats *gadget);
// A thread that waits for IOBSD, publishes the controller under owner, then hands received frames and link changes to macOS.
bool startThread(IOService *owner);

// The USB thread (KernelHal), never blocking for long: copies under a short lock.
unsigned txNext(uint8_t *dst, unsigned room);           // the next frame for the Mac (padded to 60 bytes), 0 for none
unsigned rxSpace();
bool rxFrame(const uint8_t *src, unsigned length);      // false: dropped (no room, or the interface is not up)
void rxDone();
void link(bool up, uint32_t bitsPerSecond);

// The monitor's `usb` command: one line.
void status(char *line, unsigned size);

}  // namespace pianonet
