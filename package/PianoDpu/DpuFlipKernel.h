// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1b in the kernel (DpuFlip.cpp), as PianoDpu.cpp calls it: two entry points, nothing else shared.
#pragma once
#include <IOKit/IOReturn.h>
#include "DpuSurvey.h"

class IOService;
class OSSymbol;

namespace pianodpu {

// piano-dpu=2: B1b's ladder (piano-dpu-flip=N, docs/drivers/dpu.md section 8) on the calling thread, after the survey s and m
// (smmuLevel: piano-dpu-smmu, 3 lets a bank the boot loader translates be walked and taken over). Step 4 does not return while
// it serves: the thread then watches the surface on the panel (piano-dpu-watch) and logs the summaries.
void flipRun(IOService *owner, const Survey &s, const SmmuSurvey &m, uint32_t smmuLevel);

// PianoDpu::callPlatformFunction: true when name is "PianoDpuFlip" (PianoDpuFlipClient.h), with *result set.
bool flipFunction(const OSSymbol *name, void *request, IOReturn *result);

}  // namespace pianodpu
