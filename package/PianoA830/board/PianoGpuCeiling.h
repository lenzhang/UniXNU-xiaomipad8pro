// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU's external ceiling, one slot per caller (docs/policy.md §5.3, docs/drivers/gpu.md "Ceiling slots"):
// callPlatformFunction("piano-gpu-ceiling", false, (void *)(uintptr_t)kHz, (void *)(uintptr_t)client, ...) on the PianoA830 service.
// Client 0 (param2 null: every caller before the slots, PianoThermal's passive trip), 1 the power profile (PianoPolicy), 2 and 3 free
// for later callers; kHz 0 clears that caller's slot. The DCVS ceiling is the lowest non-zero slot (adreno830_dcvs_set_ceiling), so
// no caller can raise the GPU over another's limit, and clearing one leaves the others in force. The service publishes the number
// of slots as piano-gpu-ceiling-clients; a client id past it is refused (kIOReturnBadArgument) and changes nothing.
// Portable (no IOKit): tests/test_gpu_ceiling.cpp. The kext serialises set() with a lock (any caller thread).
#pragma once
#include <stdint.h>

namespace pianogpu {

constexpr unsigned kCeilingClients = 4;
enum : unsigned { kCeilingThermal = 0, kCeilingPolicy = 1 };

inline const char *ceilingClientName(unsigned client)
{
  switch (client) {
  case kCeilingThermal: return "thermal";
  case kCeilingPolicy: return "power profile";
  default: return "client";
  }
}

struct CeilingSlots {
  uint32_t khz[kCeilingClients] = {};
  uint32_t effective = 0;   // the lowest non-zero slot, 0 for none (what the DCVS has)

  // The caller's slot to `value`. False for an unknown client (nothing changes). *changed: the effective ceiling moved.
  bool set(unsigned client, uint32_t value, bool *changed)
  {
    if (changed) *changed = false;
    if (client >= kCeilingClients) return false;
    khz[client] = value;
    uint32_t lowest = 0;
    for (uint32_t k : khz)
      if (k && (!lowest || k < lowest)) lowest = k;
    if (changed) *changed = lowest != effective;
    effective = lowest;
    return true;
  }
  // The client whose slot holds the effective ceiling (the lowest id on a tie), kCeilingClients for none.
  unsigned holder() const
  {
    for (unsigned c = 0; c < kCeilingClients; ++c)
      if (effective && khz[c] == effective) return c;
    return kCeilingClients;
  }
};

}  // namespace pianogpu
