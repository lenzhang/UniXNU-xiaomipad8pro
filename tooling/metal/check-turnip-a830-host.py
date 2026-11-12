#!/usr/bin/env python3
"""Offline ABI handshake of the native GPU path's Turnip (docs/drivers/gpu-userspace.md §4), on the build Mac, no tablet.

The very libvulkan_freedreno.dylib of out/turnip-a830/NativeAdrenoMetal.bundle (arm64e, built against Adreno830's NativeAdrenoABI.h
v2 by tooling/metal/build-turnip-a830.sh) drives a real Vulkan instance against the host copy of Adreno830's kernel stack:
Adreno830's tests/hostsim/host_gpu_service.cpp with HOSTSIM_GPU=a830 (its real GPU-context bring-up, VM, ring, submit and user-client
code over software SMMU and CP peers, the A830's device facts and the render server's slot-0 window), behind its IOKit shim
(tests/hostsim/host_iokit_shim.cpp, inserted with DYLD_INSERT_LIBRARIES). The shim answers as the tablet's kernel must: the service
the plugin names (TU_DARWIN_SERVICE=AGXAccelerator; the shim's iOS IOAcceleratorES is renamed in a generated copy), open type
0x4147 only, every ABI method through the NTNL tunnel of one admitted selector, CPU views through IOConnectMapMemory64. No GPU is
used and no shader runs: the software CP retires every submission at once.

    check-turnip-a830-host.py [--library DYLIB] [--out DIR] [--no-plugin-smoke]

Runs, each must pass:
  1. vulkan_smoke (Adreno830 tests/hostsim/vulkan_smoke.c): instance, the one physical device (Adreno 830, chip 0x44050001),
     memory types, vkCreateDevice, a 1 MiB host-visible BO mapped through the kernel's descriptor, a fill submitted and its fence
     retired, every kernel object gone after vkDestroyDevice. Twice: with the Mac's 16 KiB pages and with the tablet's 4 KiB
     (HOSTSIM_PAGE_SIZE=4096, so the GPU address layout is the tablet's).
  2. deferred_bo (tests/hostsim/deferred_bo.cpp): BOs freed while a fence is pending, and kernel refusals of each kind (busy,
     fence, free, lease, view and map-cleanup errors), with the BO cache off so that every free reaches the kernel. Two of the
     test's expectations are the 13T Turnip's policies, not the ABI's, and this Turnip (the VM-era build's, proven in the VM) has others:
     - its BO cache keeps a freed small BO (TU_DARWIN_BO_CACHE_SMALL_MB) instead of freeing it, so mode "normal" runs cache off;
     - its deferred-free queue grows without waiting for the GPU, where the 13T's waits at 256 entries: mode "pressure" must stop
       exactly at the test's "peer.forced" requirement (the 257th free did not wait), which is checked as such.
  3. plugin_smoke (tests/hostsim/plugin_smoke.cpp over AdrenoMetal's NativeTurnipSmoke.h): a fill, a compute pipeline and a render
     pass built (ir3 compiles them for the A830), submitted and retired. Skipped with --no-plugin-smoke or without glslangValidator.
  4. plugin_host (tooling/metal/turnip-a830/plugin_host.mm): the staged plugin executable itself, frontend and bridge on this Turnip:
     AIR pipelines, compute, a render into an IOSurface, a blit, then a desktop session's extras (a heap, 4x MSAA, a concurrent compute
     encoder, a shared event, 24 panel-sized IOSurfaces churned with no import left behind, an 8192x4096 texture), the per-object
     limit (maxBufferLength equal to it, one byte more refused before the kernel), 16384x16384 BGRA8 textures (own and over an
     IOSurface: made where the limit takes 1 GiB, else refused before the kernel) and 4096x2160 video frames sampled ('444v' as format
     503, '420v' as 500); with 16 KiB and 4 KiB pages, in a small slot's window (HOSTSIM_SLOT=4), and with the device's connection in
     another slot than the enumeration probe's (HOSTSIM_SLOT_REOPEN=5: Turnip 0006 takes the device's own window; without it every
     BoMap is BadArgument, run attend4). Then the same over the kernel as piano's kext builds it (-DADRENO830_MACOS, 2026-11-12: the
     per-object limit is the connection's window, the VM's 524288 table ranges) with the 16 GiB aperture (HOSTSIM_VA=16: the render
     server's 3.5 GiB window): plugin_host-macos (the 16384x16384 textures made: a 1 GiB BO and a 1 GiB import), -macos-scatter
     (HOSTSIM_SCATTER=1: every BO one page per span, as outside the BO pool on the tablet: the 1 GiB BO is 262144 table ranges),
     -macos-pool (HOSTSIM_POOL_MIB=1536: a BO pool as S3 reserves it, the 1 GiB BO one span of it) and -macos-slot4 (a small client's
     320 MiB window: maxBufferLength 320 MiB, the 16384x16384 textures refused before the kernel).
  5. abort_reporter (tooling/metal/turnip-a830/reporter_host.mm): the plugin's abort reporter on abort(), an uncaught exception and a
     bad access: the process dies with the same signal and says why on stderr first.
Sources are read from this repository's package/PianoA830/adreno830 (or ADRENO830_TREE), ADRENOMETAL_TREE and the kits below, never
changed; everything built goes to out/turnip-a830/hostsim."""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

D = Path(__file__).resolve().parents[2]
H = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research'
# The kernel stack and hostsim: this repository's Adreno830 copy (package/PianoA830/adreno830, the code PianoA830 builds, upstream paths:
# no ADRENO830_MACOS here; its hostsim has HOSTSIM_SLOT_REOPEN), else ADRENO830_TREE (e.g. unixnu-research/Adreno830 at 1a13210).
A830 = Path(os.environ.get('ADRENO830_TREE', D / 'package/PianoA830/adreno830' if (D / 'package/PianoA830/adreno830').is_dir()
                           else H / 'Adreno830'))
METAL = Path(os.environ.get('ADRENOMETAL_TREE', H / 'AdrenoMetal'))
SIM = A830 / 'tests/hostsim'
# The kits Adreno830 1a13210 is written against: ArmSmmu4K e923329 (ArmSmmu4KLiveArena still {mapping, capacity_pages}, with
# ArmSmmu4KLiveMapContextBatchExclusive and ArmSmmu4KTlbPageLimit; the next commit, 48dbd6c, replaced the arena's pool by page
# callbacks, which the standalone checkout in unixnu-research has) and AdrenoPM4 282606b (UniXNU components.toml's pin of that
# week). Fetched once as GitHub archives into out/turnip-a830/kits; ARMSMMU4K_TREE / ADRENOPM4_TREE name other checkouts.
KITS_PINNED = {'ArmSmmu4K': 'e923329e74f18b11e63c155e470c970ebad3b6e2', 'AdrenoPM4': '282606b528b2595bc4274ea7490b62b323f35a5f'}
ARCH = ['-target', 'arm64e-apple-macos14.0']   # the bundle's Turnip is arm64e: so is every image of the test process
CORE = ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm', 'NativeAdrenoRing', 'NativeAdrenoSubmit',
        'NativeAdrenoUserClient', 'NativeAdrenoTimer', 'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext',
        'NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory']


def kit(name):
    """The kit's source directory: KIT_TREE from the environment, else the pinned commit's GitHub archive under out/."""
    override = os.environ.get(f'{name.upper()}_TREE')
    if override:
        return Path(override)
    commit = KITS_PINNED[name]
    where = D / 'out/turnip-a830/kits' / f'{name}-{commit}'
    if not where.is_dir():
        where.parent.mkdir(parents=True, exist_ok=True)
        archive = where.parent / f'{name}-{commit}.tar.gz'
        urllib.request.urlretrieve(f'https://github.com/UniXNU/{name}/archive/{commit}.tar.gz', archive)
        with tarfile.open(archive) as tar:
            tar.extractall(where.parent, filter='data')
    return where


SMMU, PM4 = kit('ArmSmmu4K'), kit('AdrenoPM4')
KITS = [SMMU / f'{s}.c' for s in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams', 'ArmSmmu4KMmio']] \
    + [PM4 / 'AdrenoPM4.c']


def run(command, **kw):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kw)
    if result.returncode:
        raise SystemExit(f'{" ".join(str(c) for c in command)[:300]}\n{(result.stdout + result.stderr)[-4000:]}')
    return result


def stale(out, *inputs):
    return not out.exists() or any(Path(i).stat().st_mtime > out.stat().st_mtime for i in inputs)


def compile_one(source, obj, flags, extra=()):
    if stale(obj, source, *extra):
        compiler = 'clang' if source.suffix == '.c' else 'clang++'
        std = '-std=c11' if source.suffix == '.c' else '-std=c++23'
        run(['xcrun', compiler, *ARCH, std, '-O1', '-fPIC', *flags, '-c', source, '-o', obj])
    return obj


def spirv_words(path):
    data = path.read_bytes()
    return struct.unpack('<%dI' % (len(data) // 4), data)


def smoke_shaders(out):
    """NativeTurnipShaders.h as UniXNU's gen-turnip-smoke-shaders.py writes it, with glslangValidator (glslc is not installed here)."""
    shaders = sorted((METAL / 'shaders').glob('smoke_*'))
    if not shaders or not shutil.which('glslangValidator'):
        return None
    lines = ['// Generated by tooling/metal/check-turnip-a830-host.py from AdrenoMetal shaders/smoke_*; do not edit.', '#pragma once',
             '#include <stdint.h>']
    for source in shaders:
        spv = out / (source.name + '.spv')
        run(['glslangValidator', '--target-env', 'vulkan1.1', '-Os', '-o', spv, source])
        if shutil.which('spirv-val'):
            run(['spirv-val', '--target-env', 'vulkan1.1', spv])
        words = spirv_words(spv)
        lines.append('static const uint32_t kSpv_%s[] = {' % source.name.replace('.', '_'))
        lines += ['    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',' for i in range(0, len(words), 8)]
        lines.append('};')
    header = out / 'NativeTurnipShaders.h'
    header.write_text('\n'.join(lines) + '\n')
    return header


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--library', type=Path, default=D / 'out/turnip-a830/NativeAdrenoMetal.bundle/Contents/MacOS/libvulkan_freedreno.dylib')
    p.add_argument('--out', type=Path, default=D / 'out/turnip-a830/hostsim')
    p.add_argument('--no-plugin-smoke', action='store_true')
    a = p.parse_args()
    library = a.library.resolve()
    if not library.is_file():
        raise SystemExit(f'{library}: build it first (tooling/metal/build-turnip-a830.sh)')
    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    vulkan = D / 'out/turnip-a830/mesa/include'   # Khronos headers as Mesa ships them
    abi = D / 'out/turnip-a830/abi'               # the ABI Turnip was built with (the kernel header + compat)

    # Host doubles of the kernel headers, as Adreno830's tests/run-host-tests.py makes them.
    includes = out / 'host-include'
    for name in ('IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h'):
        (includes / name).parent.mkdir(parents=True, exist_ok=True)
        (includes / name).write_text('#include "XnuMemoryFixture.h"\n')
    (includes / 'kern/clock.h').write_text('#include "Adreno830HostClock.h"\n')
    kernel_flags = ['-DADRENO830_HOST_TEST', '-I', includes, '-I', A830 / 'tests', '-I', A830, '-I', SMMU, '-I', PM4,
                    '-I', A830 / 'platform/sm8750']
    kits = [compile_one(s, out / f'{s.stem}.o', []) for s in KITS]
    headers = sorted(A830.glob('*.h'))
    objects = kits + [compile_one(A830 / f'{n}.cpp', out / f'{n}.o', kernel_flags, headers) for n in CORE]
    objects.append(compile_one(SIM / 'host_gpu_service.cpp', out / 'host_gpu_service.o', kernel_flags, headers))
    # The same kernel core as piano's kext builds it (-DADRENO830_MACOS: macOS's per-object limit, window table and VM capacity), into
    # out/turnip-a830/hostsim/macos, for the macOS runs of section 4.
    macos_out = out / 'macos'
    macos_out.mkdir(exist_ok=True)
    macos_flags = kernel_flags + ['-DADRENO830_MACOS']
    macos_objects = kits + [compile_one(A830 / f'{n}.cpp', macos_out / f'{n}.o', macos_flags, headers) for n in CORE]
    macos_objects.append(compile_one(SIM / 'host_gpu_service.cpp', macos_out / 'host_gpu_service.o', macos_flags, headers))
    # The shim, answering the macOS plugin's service name as the iOS one: a generated copy, the source stays as it is.
    shim_source = out / 'host_iokit_shim.cpp'
    text = (SIM / 'host_iokit_shim.cpp').read_text()
    renamed = text.replace('if(!strcmp(name,"IOAcceleratorES"))',
                           'if(!strcmp(name,"IOAcceleratorES") || !strcmp(name,"AGXAccelerator"))')
    if renamed == text:
        raise SystemExit(f'{SIM}/host_iokit_shim.cpp: the IOAcceleratorES match is not where it was; update this check')
    if not shim_source.exists() or shim_source.read_text() != renamed:
        shim_source.write_text(renamed)
    shim_obj = out / 'host_iokit_shim.o'
    if stale(shim_obj, shim_source):
        run(['xcrun', 'clang++', *ARCH, '-std=c++17', '-O1', '-fPIC', '-c', shim_source, '-o', shim_obj])
    shim = out / 'libhostiokit.dylib'
    if stale(shim, shim_obj, *objects):
        run(['xcrun', 'clang++', *ARCH, '-dynamiclib', '-o', shim, shim_obj, *objects, '-framework', 'IOKit',
             '-framework', 'CoreFoundation'])
    macos_shim = macos_out / 'libhostiokit.dylib'
    if stale(macos_shim, shim_obj, *macos_objects):
        run(['xcrun', 'clang++', *ARCH, '-dynamiclib', '-o', macos_shim, shim_obj, *macos_objects, '-framework', 'IOKit',
             '-framework', 'CoreFoundation'])
    smoke = out / 'vulkan_smoke'
    if stale(smoke, SIM / 'vulkan_smoke.c'):
        run(['xcrun', 'clang', *ARCH, '-std=c11', '-O1', '-I', vulkan, SIM / 'vulkan_smoke.c', '-o', smoke])
    deferred = out / 'deferred_bo'
    if stale(deferred, SIM / 'deferred_bo.cpp', abi / 'NativeAdrenoABI.v2.h'):
        run(['xcrun', 'clang++', *ARCH, '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-I', vulkan, '-I', abi, SIM / 'deferred_bo.cpp', '-o', deferred])

    env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(shim), HOSTSIM_GPU='a830', TU_DARWIN_SERVICE='AGXAccelerator', MESA_LOG='')
    for key in [k for k in env if k.startswith(('TU_DEBUG', 'TU_DARWIN_VIRTGPU', 'HOSTSIM_STALL', 'HOSTSIM_FAIL'))]:
        env.pop(key)
    failures = []

    def check(label, command, extra_env, verdict):
        result = subprocess.run([str(c) for c in command], env=dict(env, **extra_env), capture_output=True, text=True, timeout=600)
        log = out / f'{label}.log'
        log.write_text(result.stdout + result.stderr)
        ok, line = verdict(result)
        print(f'{"PASS" if ok else "FAIL"} {label}: {line}')
        if not ok:
            failures.append(f'{label}: exit {result.returncode}, see {log}\n{(result.stdout + result.stderr)[-2500:]}')

    def smoke_verdict(result):
        lines = result.stdout.splitlines()
        device = next((l for l in lines if l.startswith('device: ')), 'no device line')
        ok = result.returncode == 0 and 'PASS' in lines and 'Adreno' in device and '830' in device
        return ok, f'{device}; ' + '; '.join(l for l in lines if l.startswith(('fill-buffer', 'device destroyed')))

    check('vulkan_smoke-16k', [smoke, library], {}, smoke_verdict)
    check('vulkan_smoke-4k', [smoke, library], {'HOSTSIM_PAGE_SIZE': '4096'}, smoke_verdict)
    for mode in ['normal', 'busy', 'fence-error', 'free-error', 'lease-error', 'view-error', 'map-cleanup-error']:
        def deferred_verdict(result):
            lines = [l for l in result.stdout.splitlines() if l.startswith('PASS:')]
            return result.returncode == 0 and bool(lines), lines[0] if lines else 'no PASS line'
        check(f'deferred_bo-{mode}', [deferred, library, mode], {'TU_DARWIN_RETIRE_TRACE': '1', 'TU_DARWIN_BO_CACHE': '0'},
              deferred_verdict)

    def growth_verdict(result):
        # 256 frees under a pending fence: none reached the kernel (the test's own checks up to there); the 257th did not wait for
        # the GPU either, the one requirement of the 13T's bounded queue this Turnip does not have.
        stopped = [l for l in result.stderr.splitlines() if l.startswith('FAIL line')]
        ok = result.returncode == 1 and len(stopped) == 1 and stopped[0].endswith('peer.forced.load()')
        return ok, ('257 frees queued under a pending fence, none reached the kernel, the last without a GPU wait (the queue grows: '
                    'the VM-era Turnip\'s policy)' if ok else (stopped[0] if stopped else f'exit {result.returncode}'))
    check('deferred_bo-pressure', [deferred, library, 'pressure'], {'TU_DARWIN_RETIRE_TRACE': '1', 'TU_DARWIN_BO_CACHE': '0'},
          growth_verdict)

    # Turnip 0009 (2026-11-12): a BoAlloc without memory waits for the other processes' caches and is tried again (made after a wait
    # or two, refused after the whole series, refused at once within the next second), its line on stderr; a cached BO goes back to
    # the kernel by the idle trim with no further call.
    def reclaim_verdict(result):
        lines = [l for l in result.stdout.splitlines() if l.startswith('PASS:')]
        made = [l for l in result.stderr.splitlines() if 'a BoAlloc found no memory' in l and ' made after ' in l]
        none = [l for l in result.stderr.splitlines() if 'a BoAlloc found no memory' in l and 'still none after 5 waits' in l]
        ok = result.returncode == 0 and bool(lines) and bool(made) and bool(none)
        return ok, (f'{lines[0]}; stderr: "{made[0].split("NativeAdreno: ", 1)[-1]}" and the series that failed' if ok
                    else (lines[0] if lines else 'no PASS line') + ('' if made else '; no "made after N waits" line') +
                    ('' if none else '; no "still none" line'))
    check('deferred_bo-no-memory', [deferred, library, 'no-memory'], {}, reclaim_verdict)
    check('deferred_bo-idle-trim', [deferred, library, 'idle-trim'], {'TU_DARWIN_BO_CACHE_AGE_MS': '200'}, deferred_verdict)

    if not a.no_plugin_smoke:
        header = smoke_shaders(out)
        smoke_h = METAL / 'NativeTurnipSmoke.h'
        if header is None or not smoke_h.is_file():
            print('SKIP plugin_smoke: no glslangValidator or no AdrenoMetal NativeTurnipSmoke.h')
        else:
            plugin = out / 'plugin_smoke'
            if stale(plugin, SIM / 'plugin_smoke.cpp', smoke_h, header):
                run(['xcrun', 'clang++', *ARCH, '-std=c++17', '-O1', '-I', METAL, '-I', vulkan, '-I', out, SIM / 'plugin_smoke.cpp',
                     '-o', plugin])

            def plugin_verdict(result):
                line = next((l for l in result.stdout.splitlines() if l.startswith('stage ')), 'no result line')
                return result.returncode == 0, line
            check('plugin_smoke', [plugin, library], {}, plugin_verdict)

    # 4. The plugin binary itself (tooling/metal/turnip-a830/plugin_host.mm): frontend and bridge over this Turnip.
    executable = library.parent / 'NativeAdrenoMetal'
    if executable.is_file():
        host = out / 'plugin_host'
        source = D / 'tooling/metal/turnip-a830/plugin_host.mm'
        if stale(host, source):
            run(['xcrun', 'clang++', *ARCH, '-fobjc-arc', '-std=c++17', '-O1', '-x', 'objective-c++', source, '-o', host,
                 '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface', '-framework', 'CoreVideo'])
        cache = out / 'm2v-cache'
        shutil.rmtree(cache, ignore_errors=True)
        cache.mkdir()

        def plugin_host_verdict(result):
            lines = result.stdout.splitlines()
            device = next((l for l in lines if l.startswith('device: ')), 'no device')
            verdict = next((l for l in lines if l.startswith(('PASS', 'FAILED', 'FAIL'))), 'no verdict')
            return result.returncode == 0 and verdict.startswith('PASS'), f'{device}; {verdict}'
        # -slot4: the same as a process in a small slot (HOSTSIM_SLOT=4, the hostsim's 124 MiB app window, smaller than piano's 448 MiB
        # ones): the panel-sized IOSurface churn then needs every import's GPU range back before the next one.
        # -reslot: the enumeration probe in slot 4, the device's connection in slot 5 (HOSTSIM_SLOT_REOPEN), another window: Turnip
        # 0006 takes the device's own (without it every BoMap fell outside the window, BadArgument, run attend4).
        for label, extra in [('plugin_host-16k', {}), ('plugin_host-4k', {'HOSTSIM_PAGE_SIZE': '4096'}),
                             ('plugin_host-slot4', {'HOSTSIM_PAGE_SIZE': '4096', 'HOSTSIM_SLOT': '4'}),
                             ('plugin_host-reslot', {'HOSTSIM_PAGE_SIZE': '4096', 'HOSTSIM_SLOT': '4', 'HOSTSIM_SLOT_REOPEN': '5'})]:
            check(label, [host, executable, library, cache], extra, plugin_host_verdict)
        # The kernel as piano's kext builds it (2026-11-12): the per-object limit is the window, and the 16 GiB aperture's windows.
        macos = {'DYLD_INSERT_LIBRARIES': str(macos_shim), 'HOSTSIM_PAGE_SIZE': '4096', 'HOSTSIM_VA': '16'}
        for label, extra in [('plugin_host-macos', {}), ('plugin_host-macos-scatter', {'HOSTSIM_SCATTER': '1'}),
                             ('plugin_host-macos-pool', {'HOSTSIM_POOL_MIB': '1536'}), ('plugin_host-macos-slot4', {'HOSTSIM_SLOT': '4'})]:
            check(label, [host, executable, library, cache], dict(macos, **extra), plugin_host_verdict)

        # 5. The plugin's abort reporter (tooling/metal/turnip-a830/abort_reporter.mm): a process with it dies as it would have (the same
        #    signal) and says why on stderr first: the reason, the crash-reporter message, the stack.
        reporter_source = D / 'tooling/metal/turnip-a830/reporter_host.mm'
        if (D / 'tooling/metal/turnip-a830/abort_reporter.mm').is_file() and reporter_source.is_file():
            reporter = out / 'reporter_host'
            if stale(reporter, reporter_source):
                run(['xcrun', 'clang++', *ARCH, '-fobjc-arc', '-std=c++17', '-O1', '-x', 'objective-c++', reporter_source, '-o', reporter,
                     '-framework', 'Foundation'])
            import signal
            for mode, number, wanted in [('abort', signal.SIGABRT, 'crash message (reporter_host): reporter check'),
                                         ('exception', signal.SIGABRT, 'an uncaught exception'),
                                         ('fault', signal.SIGSEGV, 'SIGSEGV')]:
                def reporter_verdict(result, number=number, wanted=wanted):
                    lines = result.stderr.splitlines()
                    died = result.returncode in (-number, -signal.SIGBUS) if number == signal.SIGSEGV else result.returncode == -number
                    ok = died and any('abort reporter installed' in l for l in lines) and any(wanted in l for l in lines) and \
                        any(': #0 ' in l for l in lines)
                    return ok, f'exit {result.returncode}; ' + '; '.join(l.split(': ', 2)[-1] for l in lines if wanted in l)[:150]
                check(f'abort_reporter-{mode}', [reporter, executable, mode], {'PIANO_METAL_REPORT': '2'}, reporter_verdict)
    else:
        print(f'SKIP plugin_host: no {executable}')

    for failure in failures:
        print('\n' + failure, file=sys.stderr)
    if failures:
        raise SystemExit(f'{len(failures)} check(s) failed')
    print(f'PASS: Turnip ({library.name}, arm64e) on Adreno830 ABI v2 over the host kernel stack: AGXAccelerator, open type 0x4147, '
          'NTNL tunnel; no GPU')


if __name__ == '__main__':
    main()
