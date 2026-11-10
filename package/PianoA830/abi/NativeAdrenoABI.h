/* SPDX-License-Identifier: MIT */
/* PianoA830's user-client ABI as Turnip's darwin KMD includes it: point Mesa's -Dtu-darwin-abi-dir at this directory
 * (package/PianoA830/abi) to build Turnip for the native A830. It is Adreno830's NativeAdrenoABI.h v2, included unchanged (the kernel
 * side is built from that same file), plus the one name the Droid-VM darwin KMD (UniXNU-turnip patches/mesa/turnip-darwin-kmd.patch,
 * tu_knl_darwin.cc) uses that v2 does not have: NativeAdrenoParamsOwnedCoherent, a Params flag of the VM's v1 ABI (BOs IO-coherent for
 * the GPU, so map them write-back). PianoA830 never sets it: its BOs are write-combined for the CPU unless DeviceInfo.flags carries
 * NativeAdrenoDeviceCachedCoherent (S8's coherence self-test), which the KMD already reads.
 * What changes for a Turnip built against v1 (the VM's, NativeAdreno 805c7e4): NativeAdrenoAbiVersion 1 -> 2 (Query refuses a mismatch),
 * NativeAdrenoAbiMaxBuffers 16 -> 128 and so sizeof(NativeAdrenoAbiSubmit) 416 -> 3104 (the kernel checks every record's exact size),
 * the Params flags OwnedCoherent (2) and PoolPressure (4) gone. A v1 binary cannot talk to PianoA830; rebuild it against this header. */
#ifndef PIANO_A830_NATIVE_ADRENO_ABI_H
#define PIANO_A830_NATIVE_ADRENO_ABI_H
#include "../adreno830/NativeAdrenoABI.h"
enum {NativeAdrenoParamsOwnedCoherent = 2};   /* v1 compatibility: never reported by PianoA830 */
#ifdef __cplusplus
static_assert(NativeAdrenoAbiVersion == 2 && NativeAdrenoAbiMaxBuffers == 128, "PianoA830 speaks NativeAdreno ABI v2");
#else
_Static_assert(NativeAdrenoAbiVersion == 2 && NativeAdrenoAbiMaxBuffers == 128, "PianoA830 speaks NativeAdreno ABI v2");
#endif
#endif
