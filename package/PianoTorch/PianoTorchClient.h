// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PianoTorch service's interface, header only (no link dependency): the names other code uses to reach it.
//
// From macOS (an administrator, IORegistryEntrySetCFProperties on the service of class "PianoTorch"; tooling/native/piano-torch):
//   "piano-torch-set"      a number: the lamp current in mA (0 = off; clamped to piano-torch-max-ma), or a boolean (true = on at
//                          piano-torch-ma, false = off)
//   "piano-torch-seconds"  optional, with an on: the auto-off time in seconds (default piano-torch-timeout, at most 1800)
// Published on the service: "piano-torch" (the level), "piano-torch-survey" (level 1's summary line), "piano-torch-state" (one line:
// "on 50 mA (2 x 25 mA, itarget 4), off in 287 s", "off (the time limit)", or "unavailable: <why>"), "piano-torch-lit" (boolean),
// "piano-torch-ma" (the lamp current now, 0 when off).
//
// From another driver (IOService::callPlatformFunction on the service, found by class name; never blocks on the PMIC):
//   "PianoTorchSet"     param1 = mA (uintptr_t; 0 = off), param2 = seconds (0 = the default), param3 = the caller's name (a
//                       static C string, logged)
//   "PianoTorchToggle"  param1 = the caller's name: on at piano-torch-ma when off, else off
//   "PianoTorchStatus"  param1 = char buffer, param2 = its size (uintptr_t): the state line (copied under the service's lock)
// The USB console's hook (PianoDwc3 is not changed by this driver; docs/drivers/torch.md section 5 has the ~15 lines it would add):
// a monitor command "torch [on [mA] [s] | off | status]" that calls PianoTorchSet / PianoTorchStatus as above.
#ifndef PIANO_TORCH_CLIENT_H
#define PIANO_TORCH_CLIENT_H

namespace pianotorch {

constexpr const char *kServiceClass = "PianoTorch";
constexpr const char *kFunctionSet = "PianoTorchSet";
constexpr const char *kFunctionToggle = "PianoTorchToggle";
constexpr const char *kFunctionStatus = "PianoTorchStatus";
constexpr const char *kPropertySet = "piano-torch-set";
constexpr const char *kPropertySeconds = "piano-torch-seconds";
constexpr const char *kPropertyState = "piano-torch-state";
constexpr const char *kPropertyLit = "piano-torch-lit";
constexpr const char *kPropertyMa = "piano-torch-ma";
constexpr const char *kPropertySurvey = "piano-torch-survey";

}  // namespace pianotorch
#endif
