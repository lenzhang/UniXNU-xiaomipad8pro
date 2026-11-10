/* SPDX-License-Identifier: MIT */
/* What Turnip's darwin backend (Droid-VM Mesa cecc0e9, written against the VM-era NativeAdreno 805c7e4 ABI v1 and Adreno740's v2)
 * names and Adreno830's NativeAdrenoABI.h v2 has not got. tooling/metal/build-turnip-a830.sh includes this after the kernel's header
 * only when that header lacks the name.
 *
 * NativeAdrenoParamsOwnedCoherent: Adreno740 v2's Query flag for IO-coherent BOs (the CPU view is then write-back). Adreno830 never
 * sets it (its NativeAdrenoAbiParams.flags is NativeAdrenoParamsHostImport or zero), so every CPU view of a BO stays write-combined
 * (tu_knl_darwin.cc darwin_bo_map: kIOMapWriteCombineCache), which is what the kernel's non-coherent GPU mapping needs. */
#ifndef NATIVE_ADRENO_ABI_COMPAT_H
#define NATIVE_ADRENO_ABI_COMPAT_H
enum {
    NativeAdrenoParamsOwnedCoherent = 2
};
#endif
