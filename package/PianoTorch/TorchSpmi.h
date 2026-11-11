// SPDX-License-Identifier: BSD-2-Clause-Patent
// The flash module's SPMI port: reads over EE 0's observer channel of its APID (package/PianoPower/SpmiObserver.h, included, not
// copied), writes over its owner channel (TorchCore.h spmiWriteByte), as the Bus TorchCore.h's Controller takes. The kext and the
// host test use this same adapter, so the test exercises the arbiter protocol the kext speaks.
#ifndef PIANO_TORCH_SPMI_H
#define PIANO_TORCH_SPMI_H
#include "SpmiObserver.h"
#include "TorchCore.h"

namespace pianotorch {

struct SpmiPort {
  const pianopower::Io *io = nullptr;   // core, cnfg, observer windows (reads; the observer's command is its only store)
  int apid = -1;                        // the flash module's APID
  const ChnlIo *chnl = nullptr;         // its owner channel; null at level 1 (no write path exists then)
  unsigned reads = 0, writes = 0, readFailures = 0, writeFailures = 0;
  uint32_t lastWriteStatus = 0;
};

inline bool portRead(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes)
{
  SpmiPort &p = *static_cast<SpmiPort *>(ctx);
  ++p.reads;
  const bool ok = p.io && pianopower::spmiRead(*p.io, p.apid, reg, value, bytes) == pianopower::ReadResult::ok;
  if (!ok) ++p.readFailures;
  return ok;
}

inline bool portWrite(void *ctx, uint8_t reg, uint8_t value)
{
  SpmiPort &p = *static_cast<SpmiPort *>(ctx);
  if (!p.chnl) return false;
  ++p.writes;
  const bool ok = spmiWriteByte(*p.chnl, reg, value, &p.lastWriteStatus) == WriteResult::ok;
  if (!ok) ++p.writeFailures;
  return ok;
}

inline Bus portBus(SpmiPort &p)
{
  Bus b;
  b.ctx = &p;
  b.read = portRead;
  b.write = p.chnl ? portWrite : nullptr;
  return b;
}

}  // namespace pianotorch
#endif
