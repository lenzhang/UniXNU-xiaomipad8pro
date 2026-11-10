#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The PianoA830 ABI handshake on the Mac, against Adreno830's host simulator (gpu-native-plan.md step 3). No device, no GPU.

    check-a830-abi-host.py [--turnip LIBVULKAN_FREEDRENO.dylib] [--vulkan-include DIR] [--expect-turnip pass|fail]

Builds, from package/PianoA830 (UniXNU scripts/check-turnip-darwin-host.py's recipe with Adreno830's sources): the host copy of the
kernel stack (tests/hostsim/host_gpu_service.cpp over the real VM, ring, submit and user-client code with software SMMU/CP peers;
HOSTSIM_GPU=a830 gives the A830's DeviceInfo) inside an IOKit shim dylib (tests/hostsim/host_iokit_shim.cpp, DYLD_INSERT_LIBRARIES
with dyld interposing). Then:
  1. package/PianoA830/abi/abi_handshake.cpp, the darwin KMD's calls written against package/PianoA830/abi/NativeAdrenoABI.h (v2 plus
     the v1 compat name): open 0x4147, tunnel probe, Query v2, DeviceInfo, queue, BO alloc/map/CPU lease, one Submit, WaitFence,
     timestamp/status, and a v1-sized Submit refused. Must PASS.
  2. With --turnip: Adreno830's tests/hostsim/vulkan_smoke.c drives that Turnip build (vkCreateInstance, the physical device,
     vkCreateDevice, memory, a submission) over the same stack. A Turnip built against the VM's ABI v1 must fail at Query
     (--expect-turnip fail, the default when the dylib is the VM's); one rebuilt with -Dtu-darwin-abi-dir=package/PianoA830/abi must
     pass (--expect-turnip pass).
Outputs in out/native-kexts/PianoA830/hostsim.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
P = REPO / 'package/PianoA830'
A, SIM, SMMU, PM4 = P / 'adreno830', P / 'adreno830/tests/hostsim', P / 'kits/ArmSmmu4K', P / 'kits/AdrenoPM4'
VM_TURNIP = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/UniXNU-turnip/build/mesa-tu-macos/src/freedreno/vulkan/libvulkan_freedreno.dylib'
VK_INCLUDE = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/UniXNU-turnip/.cache/mesa-work/include'


def sh(*command, **kw):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kw)
    if result.returncode:
        sys.exit(f'check-a830-abi-host: {" ".join(map(str, command[:3]))} ...:\n{(result.stdout + result.stderr)[-3000:]}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--turnip', type=Path, help=f'a macOS libvulkan_freedreno.dylib (e.g. the VM build {VM_TURNIP})')
    parser.add_argument('--vulkan-include', type=Path, default=VK_INCLUDE)
    parser.add_argument('--expect-turnip', choices=('pass', 'fail'))
    args = parser.parse_args()
    out = REPO / 'out/native-kexts/PianoA830/hostsim'
    includes = out / 'host-include'
    for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h']:
        path = includes / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "XnuMemoryFixture.h"\n')
    (includes / 'kern/clock.h').write_text('#include "Adreno830HostClock.h"\n')
    flags = ['-I', includes, '-I', A / 'tests', '-I', A, '-I', A / 'platform/sm8750', '-I', SMMU, '-I', PM4]
    objects = []
    for source in [*(SMMU / f'{s}.c' for s in ('ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams', 'ArmSmmu4KMmio')),
                   PM4 / 'AdrenoPM4.c']:
        obj = out / (source.stem + '.o')
        sh('xcrun', 'clang', '-std=c11', '-O1', '-fPIC', '-c', source, '-o', obj)
        objects.append(obj)
    for source in [SIM / 'host_gpu_service.cpp', *(A / f'{s}.cpp' for s in (
            'NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm', 'NativeAdrenoRing', 'NativeAdrenoSubmit', 'NativeAdrenoUserClient',
            'NativeAdrenoTimer', 'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext', 'NativeAdrenoGmuContext'))]:
        obj = out / (source.stem + '.o')
        sh('xcrun', 'clang++', '-std=c++23', '-O1', '-fPIC', '-DADRENO830_HOST_TEST', *flags, '-c', source, '-o', obj)
        objects.append(obj)
    shim_obj = out / 'host_iokit_shim.o'
    sh('xcrun', 'clang++', '-std=c++17', '-O1', '-fPIC', '-c', SIM / 'host_iokit_shim.cpp', '-o', shim_obj)
    shim = out / 'libhostiokit.dylib'
    sh('xcrun', 'clang++', '-dynamiclib', '-o', shim, shim_obj, *objects, '-framework', 'IOKit', '-framework', 'CoreFoundation')
    env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(shim), HOSTSIM_GPU='a830', MESA_LOG='')
    env.pop('TU_DARWIN_SERVICE', None)
    handshake = out / 'abi_handshake'
    sh('xcrun', 'clang++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I', P / 'abi', P / 'abi/abi_handshake.cpp', '-o', handshake,
       '-framework', 'IOKit', '-framework', 'CoreFoundation')
    result = subprocess.run([str(handshake)], env=env, capture_output=True, text=True, timeout=120)
    (out / 'abi_handshake.log').write_text(result.stdout + result.stderr)
    print((result.stdout + result.stderr).strip())
    if result.returncode or 'PASS' not in result.stdout:
        sys.exit('check-a830-abi-host: the v2 handshake failed')
    if not args.turnip:
        return 0
    archs = subprocess.run(['lipo', '-archs', str(args.turnip)], capture_output=True, text=True).stdout.split()
    if 'arm64' not in archs:
        # A third-party arm64e image does not load into an arm64 host process (and the VM's has a modified ad-hoc signature):
        # the device build is not the host build. Turnip for this check: an arm64 macOS build of the same source.
        print(f'vulkan_smoke: SKIP: {args.turnip} is {"/".join(archs) or "unreadable"}, not arm64; build an arm64 host Turnip '
              '(-Dtu-darwin-abi-dir=package/PianoA830/abi) for this check')
        return 0
    smoke = out / 'vulkan_smoke'
    sh('xcrun', 'clang', '-std=c11', '-O1', '-I', args.vulkan_include, SIM / 'vulkan_smoke.c', '-o', smoke)
    run = subprocess.run([str(smoke), str(args.turnip)], env=dict(env, TU_DEBUG='startup'), capture_output=True, text=True, timeout=300)
    text = (run.stdout + run.stderr).strip()
    (out / 'vulkan_smoke.log').write_text(text + '\n')
    passed = run.returncode == 0 and 'PASS' in run.stdout
    expect = args.expect_turnip or ('fail' if args.turnip.resolve() == VM_TURNIP.resolve() else 'pass')
    print(f'vulkan_smoke with {args.turnip}: {"PASS" if passed else "FAIL"} (expected {expect})')
    print('\n'.join(text.splitlines()[-12:]))
    return 0 if passed == (expect == 'pass') else 1


if __name__ == '__main__':
    sys.exit(main())
