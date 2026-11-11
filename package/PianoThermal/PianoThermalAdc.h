// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6's hook (PianoThermalAdc.cpp): the three calls PianoThermal.cpp makes. docs/drivers/thermal.md section 12.
#ifndef PIANO_THERMAL_ADC_HOOK_H
#define PIANO_THERMAL_ADC_HOOK_H
#include <stdint.h>

class IOService;

namespace pianothermal::adc {
// piano-thermal-adc (0 off, the default; 1 the board thermistors; 2 the same with every round's raw codes).
uint32_t hookLevel();
// Starts the ADC thread (level >= 1): its first round after delayS, then every periodS; svc gets the IORegistry properties.
void hookStart(IOService *svc, uint32_t level, uint32_t periodS, uint32_t delayS);
// callPlatformFunction("piano-thermal-board-temps", param1 = struct piano_board_temps *): true when filled.
bool hookBoardTemps(void *param1);
}  // namespace pianothermal::adc
#endif
