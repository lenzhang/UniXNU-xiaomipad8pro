// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6: a board thermistor's resistance as a temperature, integer only (the kext uses no floating point). Portable: the
// kext and tests/adc_host.cpp include it. docs/drivers/thermal.md section 12.
//
// The part. Every one of piano's eleven board channels is a 100 kOhm NTC to ground on a 100 kOhm pull-up inside the PMIC, read as a
// ratio of the ADC reference (DT: qcom,ratiometric on the *_100K_PU channels; docs/ubuntu-history/kernel.md 2.8). Linux names the
// curve of these channels "NTCG104EF104" (TDK's NTCG series, 100 kOhm, 1 %). Its published parameters, the only inputs here:
//   R25 = 100 kOhm at 25 C, B25/50 = 4250 K, B25/85 = 4311 K.
// A B value fixes a second point of the curve: R(T) = R25 * exp(B * (1/T - 1/T25)), T in kelvin; so the three numbers give three
// exact points, (25 C, 100000 Ohm), (50 C, 33194.6 Ohm), (85 C, 8871.7 Ohm).
//
// The model: Steinhart-Hart, 1/T = a + b * ln(R) + c * ln(R)^3, the textbook three-parameter NTC curve, solved through those three
// points: a = 8.58857e-4, b = 2.05943e-4, c = 8.13564e-8 (1/K, R in ohms). kA, kB, kC below are a, b, c * 2^50, rounded;
// tests/adc_host.cpp solves the 3 x 3 system again in double precision from the three parameters and checks them (`adc_host --ntc`
// prints the derivation). Evaluation: ln(R) in Q32 fixed point (lnQ32: R = 2^k * m, ln m = 2 atanh((m - 1)/(m + 1)) by its series),
// the polynomial in 128-bit products, one 64-bit division for T; the result is within 0.001 C of the same formula in double over
// 1 kOhm .. 10 MOhm (tested).
//
// How good the model is: against the vendor's resistance-temperature data of the part (as tabulated in Linux's
// drivers/iio/adc/qcom-vadc-common.c adcmap7_100k, read as data, not copied; tooling/k27/check-ntc-curve.py repeats the comparison
// from a Linux tree at run time) it is within 0.01 C from 0 to 60 C, 0.15 C from -40 to 0 C and 0.5 C from 60 to 127 C, provided
// that table's temperatures are read as degrees C * 1024 rather than milli-degrees: its 100 kOhm entry sits at "25600", so read as
// milli-degrees the part would not meet its own R25. Linux returns those numbers as milli-degrees, so Linux (Ubuntu's v0.7.0
// readings included) reports these channels 2.4 % high in degrees C: 25.6 C at 25 C, 51.2 C at 50 C. Divide a Linux reading by
// 1.024 to compare it with this one.
#ifndef PIANO_THERMAL_NTC_H
#define PIANO_THERMAL_NTC_H
#include <stdint.h>

namespace pianothermal::ntc {

// The part's published parameters (the derivation's inputs; tests/adc_host.cpp derives kA, kB, kC from them).
constexpr uint32_t kR25Ohms = 100000, kB2550 = 4250, kB2585 = 4311;
// Steinhart-Hart coefficients, per kelvin, Q50 (a * 2^50, b * 2^50, c * 2^50).
constexpr int64_t kA = 966987170627, kB = 231871433742, kC = 91599127;
constexpr int64_t kLn2Q32 = 2977044472;            // ln 2 * 2^32, rounded
constexpr int32_t kZeroCelsiusMilliK = 273150;

// ln(x) * 2^32 for x >= 1 (x = 0: 0; the callers never ask).
constexpr int64_t lnQ32(uint64_t x)
{
  if (!x) return 0;
  const unsigned k = 63u - unsigned(__builtin_clzll(x));       // floor(log2 x)
  const uint64_t m = k <= 32 ? x << (32 - k) : x >> (k - 32);   // x / 2^k in [1, 2), Q32
  constexpr uint64_t one = 1ull << 32;
  const uint64_t z = ((m - one) << 32) / (m + one);             // (m - 1)/(m + 1) in [0, 1/3), Q32
  const uint64_t z2 = (z * z) >> 32;
  uint64_t term = z, sum = 0;
  for (uint64_t n = 1; term && n < 64; n += 2) {                // atanh z = z + z^3/3 + z^5/5 + ...
    sum += term / n;
    term = (term * z2) >> 32;
  }
  return int64_t(k) * kLn2Q32 + int64_t(2 * sum);
}

// 1/T in kelvin^-1, Q50, of a resistance in ohms.
constexpr int64_t inverseKelvinQ50(uint64_t ohms)
{
  const int64_t l = lnQ32(ohms);
  const __int128 l2 = (__int128(l) * l) >> 32;
  const __int128 l3 = (l2 * l) >> 32;
  return int64_t(__int128(kA) + ((__int128(kB) * l) >> 32) + ((__int128(kC) * l3) >> 32));
}

// The temperature of a resistance, milli-degrees C. ohms in 1 .. 2^40 (the callers clamp; outside the part's range the number is
// the model's extrapolation, which the plausibility check rejects).
constexpr int32_t milliC(uint64_t ohms)
{
  if (ohms < 1) ohms = 1;
  if (ohms > (1ull << 40)) ohms = 1ull << 40;
  const uint64_t inv = uint64_t(inverseKelvinQ50(ohms));
  const uint64_t milliK = ((1000ull << 50) + inv / 2) / inv;
  return int32_t(int64_t(milliK) - kZeroCelsiusMilliK);
}

// A ratiometric code as the thermistor's resistance: code = fullScale * R / (R + Rpu), so R = Rpu * code / (fullScale - code),
// rounded. code must be in 1 .. fullScale - 1 (0 is a short, fullScale and above an open input: the caller's checks).
constexpr uint64_t ohmsOf(uint32_t code, uint32_t fullScale, uint32_t pullUpOhms)
{
  const uint64_t d = uint64_t(fullScale) - code;
  return (uint64_t(pullUpOhms) * code + d / 2) / d;
}

static_assert(milliC(kR25Ohms) > 24999 && milliC(kR25Ohms) < 25001, "R25 is 25 C");
static_assert(milliC(400000) < 0 && milliC(2500) > 120000, "an NTC: colder at higher resistance");

}  // namespace pianothermal::ntc
#endif
