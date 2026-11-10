#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate package/PianoA830/board/PianoGpuInputs.h, the device-tree constants of piano's A830 board (Adreno830Board).

Derived from UniXNU-Pagani_SM8750 scripts/prepare-pagani-gpu-inputs.py (25b1109, BSD-2-Clause-Patent; its FDT reader, the KGSL
level-bin extraction and the header layout), for piano's two device trees instead of the 13T's DTS and runtime DT:

(A) piano's boot DT, out/inputs/piano-boot.dtb (the DT ABL hands Ubuntu's mainline kernel on this tablet; decompiled with dtc into
    out/native-kexts/PianoA830/inputs/piano-boot.dts for the record). Every value the driver needs that this DT carries comes from
    it: the GPU base, CX_MISC (its "cx_mem"), GPU_CC, GCC, the GPU SMMU and its 25 context-bank interrupts, the GPU and GMU
    interrupts, the stream IDs, the speed-bin fuse cell, cmd-db, SMEM, the ZAP carve-out (zap-shader memory-region), the LLCC
    windows, the chip id (compatible "qcom,adreno-44050001") and the SoC id (qcom,msm-id).
(B) The rest is KGSL board policy that only piano's stock (Android) DT has: the RSCC, CX_DBGC and GMU_AO windows, the GMU window
    at KGSL's base (the driver's GMU offsets are relative to 0x3d37000; the mainline DT names 0x3d6c000, the CX part inside it),
    the KGSL window sizes, cx_host_irq, the six power-level bins with their bus min/max votes, the DDR/CNOC bus tables, the GMU
    frequency table, the apps RSC DRV-2, AOSS QMP, IPCC and TSENS windows, the SCM wait-queue interrupt, ACTLR and UBWC. They
    are read from piano's live DT (unixnu-research/Unixnu-SM8750-Piano/inputs/piano-live.dtb, /sys/firmware/fdt of the stock OS)
    and recorded, normalised, in package/PianoA830/board/piano-gpu-inputs.json ("runtime_dt", pinned by the SHA-256 of the
    values), so the header is regenerated and --check'ed without that file.

Cross-checks: every (B) value the boot DT also carries must agree (bases, sizes contained, interrupts, SIDs, the speed-bin cell),
and every KGSL level of bin 0 must be one of the boot DT's GPU OPPs (frequency, RPMh level, ACD value, and the OPP's peak bandwidth
= the KGSL bus level's entry of the DDR table); the two GMU OPPs must be the GMU frequency table. --compare PAGANI_HEADER lists
what differs from the 13T's generated header (the plan's pass criterion: only digests and the SoC id).

The DTs are hardware descriptions; no Linux driver code is read or used (Linux is a specification only).
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = REPO / 'package/PianoA830/board'
OUTPUT = HERE / 'PianoGpuInputs.h'
PINS = HERE / 'piano-gpu-inputs.json'
BOOT_DTB = REPO / 'out/inputs/piano-boot.dtb'
LIVE_DTB = Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research/Unixnu-SM8750-Piano/inputs/piano-live.dtb'
MAX_LEVELS, MAX_SKUS, BINS = 16, 4, 6
LLCC_NAMES = ['llcc0_base', 'llcc1_base', 'llcc2_base', 'llcc3_base', 'llcc_broadcast_or_base', 'llcc_broadcast_and_base']
# The mainline DT's names for the same six LLCC windows (in this order).
LLCC_MAINLINE = ['llcc0_base', 'llcc1_base', 'llcc2_base', 'llcc3_base', 'llcc_broadcast_base', 'llcc_broadcast_and_base']


def fail(message):
    raise SystemExit('gen-piano-gpu-inputs: ' + message)


class Node:
    def __init__(self, name, parent):
        self.name, self.parent, self.props, self.children = name, parent, {}, {}

    def path(self):
        return '/' if self.parent is None else (self.parent.path().rstrip('/') + '/' + self.name)

    def cells(self, key, default):
        raw = self.props.get(key)
        return struct.unpack('>I', raw)[0] if raw else default

    def u32s(self, key):
        raw = self.props.get(key)
        if raw is None:
            fail(f'{self.path()}: no {key}')
        return list(struct.unpack(f'>{len(raw) // 4}I', raw))

    def u32(self, key, default=None):
        if key not in self.props:
            if default is None:
                fail(f'{self.path()}: no {key}')
            return default
        values = self.u32s(key)
        if len(values) != 1:
            fail(f'{self.path()}: {key} is not one cell')
        return values[0]

    def u64(self, key):
        values = self.u32s(key)
        if len(values) != 2:
            fail(f'{self.path()}: {key} is not two cells')
        return values[0] << 32 | values[1]

    def strings(self, key):
        raw = self.props.get(key, b'')
        return [s.decode() for s in raw.rstrip(b'\0').split(b'\0')] if raw else []

    def reg(self):
        acells, scells = self.parent.cells('#address-cells', 2), self.parent.cells('#size-cells', 1)
        values, step, result = self.u32s('reg'), acells + scells, []
        if len(values) % step:
            fail(f'{self.path()}: reg is not a multiple of {step} cells')
        number = lambda cells: sum(v << (32 * (len(cells) - 1 - k)) for k, v in enumerate(cells))
        for i in range(0, len(values), step):
            result.append((number(values[i:i + acells]), number(values[i + acells:i + step])))
        return result

    def named_reg(self, name):
        names = self.strings('reg-names')
        if name not in names:
            fail(f'{self.path()}: no reg named {name}')
        return self.reg()[names.index(name)]

    def interrupts(self):
        values = self.u32s('interrupts')
        if len(values) % 3:
            fail(f'{self.path()}: interrupts are not GIC triplets')
        result = []
        for i in range(0, len(values), 3):
            if values[i] != 0:
                fail(f'{self.path()}: interrupt {i // 3} is not an SPI')
            result.append((values[i + 1] + 32, values[i + 2]))
        return result

    def named_interrupt(self, name):
        names = self.strings('interrupt-names')
        if name not in names:
            fail(f'{self.path()}: no interrupt named {name}')
        return self.interrupts()[names.index(name)][0]

    def walk(self):
        yield self
        for child in self.children.values():
            yield from child.walk()


def read_fdt(data):
    magic, total, structure, strings = struct.unpack_from('>4I', data)
    if magic != 0xd00dfeed or total > len(data):
        fail('not a flattened device tree')
    at, root, node = structure, None, None
    while True:
        token = struct.unpack_from('>I', data, at)[0]
        at += 4
        if token == 1:
            end = data.index(b'\0', at)
            name = data[at:end].decode()
            at = (end + 4) & ~3
            child = Node(name, node)
            if node is None:
                root = child
            else:
                node.children[name] = child
            node = child
        elif token == 2:
            node = node.parent
        elif token == 3:
            size, offset = struct.unpack_from('>II', data, at)
            at += 8
            key = data[strings + offset:data.index(b'\0', strings + offset)].decode()
            node.props[key] = data[at:at + size]
            at = (at + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return root
        else:
            fail(f'bad FDT token {token}')


def find(root, path):
    node = root
    for part in path.strip('/').split('/'):
        if part not in node.children:
            fail(f'node {path} not found (at {node.path()})')
        node = node.children[part]
    return node


def by_phandle(root, handle):
    for node in root.walk():
        if node.props.get('phandle') == struct.pack('>I', handle):
            return node
    fail(f'phandle {handle:#x} not found')


def sids(node, smmu_handle, cells=3):
    specifier = node.u32s('iommus')
    if len(specifier) % cells:
        fail(f'{node.path()}: iommus is not <&smmu SID MASK> specifiers')
    result = []
    for i in range(0, len(specifier), cells):
        if specifier[i] != smmu_handle or specifier[i + 2] != 0:
            fail(f'{node.path()}: iommus entry {i // cells} is not <&gpu_smmu SID 0>')
        result.append(specifier[i + 1])
    return result


def dtc_record(dtb, out):
    """The boot DT as text next to the build (dtc), and a check that dtc reads it as a valid tree."""
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', '-o', str(out), str(dtb)], check=True)


def boot_values(boot):
    """(A) from piano's boot DT: name -> (value, note)."""
    values = {}

    def put(name, value, note):
        values[name] = (value, note)

    soc = find(boot, '/soc@0')
    gpu, gmu = find(boot, '/soc@0/gpu@3d00000'), find(boot, '/soc@0/gmu@3d6c000')
    smmu, qfprom = find(boot, '/soc@0/iommu@3da0000'), find(boot, '/soc@0/efuse@221c8000')
    for node in (gpu, smmu):
        if node.strings('status') not in ([], ['okay']):
            fail(f'{node.path()} is disabled in the boot DT')
    compatible = [c for c in gpu.strings('compatible') if re.fullmatch(r'qcom,adreno-[0-9a-f]{8}', c)]
    if len(compatible) != 1:
        fail('gpu@3d00000: no qcom,adreno-<chip id> compatible')
    put('CHIP_ID', int(compatible[0].rsplit('-', 1)[1], 16), f'boot DT gpu compatible "{compatible[0]}"')
    put('GPU_BASE', gpu.named_reg('kgsl_3d0_reg_memory'), 'boot DT gpu reg kgsl_3d0_reg_memory')
    put('WINDOW_CX_MISC', gpu.named_reg('cx_mem'), 'boot DT gpu reg cx_mem (KGSL cx_misc)')
    put('GMU_CX', gmu.named_reg('gmu'), 'boot DT gmu@3d6c000 reg gmu')
    put('WINDOW_GPUCC', find(boot, '/soc@0/clock-controller@3d90000').reg()[0], 'boot DT gpucc reg')
    put('WINDOW_GCC', find(boot, '/soc@0/clock-controller@100000').reg()[0], 'boot DT gcc reg')
    put('WINDOW_SMMU', smmu.reg()[0], 'boot DT adreno smmu reg')
    for key, path in (('CMD_DB', '/reserved-memory/aop-cmd-db@81c60000'), ('SMEM', '/reserved-memory/smem@81d00000')):
        node = find(boot, path)
        if 'no-map' not in node.props:
            fail(f'{path} is not no-map')
        put(f'WINDOW_{key}', node.reg()[0], f'boot DT {path} reg, no-map')
    zap = by_phandle(boot, gpu.children['zap-shader'].u32('memory-region'))
    if 'no-map' not in zap.props:
        fail(f'{zap.path()} is not no-map')
    put('WINDOW_ZAP', zap.reg()[0], f'boot DT gpu zap-shader memory-region {zap.path()}, no-map')
    put('ZAP_FIRMWARE', gpu.children['zap-shader'].strings('firmware-name')[0], 'boot DT zap-shader firmware-name')
    put('WINDOW_QFPROM', qfprom.reg()[0], 'boot DT qfprom reg')
    llcc = find(boot, '/soc@0/system-cache-controller@24800000')
    put('WINDOW_LLCC', [llcc.named_reg(name) for name in LLCC_MAINLINE], 'boot DT llcc reg ' + ', '.join(LLCC_MAINLINE))
    put('IRQ_GPU', gpu.interrupts()[0][0], 'boot DT gpu interrupt (INTID)')
    for key, name in (('HFI', 'hfi'), ('GMU', 'gmu')):
        put(f'IRQ_{key}', gmu.named_interrupt(name), f'boot DT gmu interrupt {name} (INTID)')
    if smmu.u32('#global-interrupts') != 1 or smmu.u32('#iommu-cells') != 2:
        fail('adreno smmu: expected one global interrupt and #iommu-cells = 2')
    irqs = [intid for intid, _ in smmu.interrupts()]
    put('IRQ_SMMU_GLOBAL', irqs[0], 'boot DT adreno smmu global interrupt (INTID)')
    put('IRQ_SMMU_CB', irqs[1:5], 'boot DT adreno smmu context banks 0..3 (INTID)')
    put('SMMU_IRQS_ALL', irqs, 'boot DT adreno smmu interrupts')
    handle = smmu.u32('phandle')
    gpu_sids = sids(gpu, handle)
    if len(gpu_sids) != 2:
        fail('gpu iommus: expected the user and LPAC streams')
    put('SID_GPU', gpu_sids[0], 'boot DT gpu iommus 0')
    put('SID_LPAC', gpu_sids[1], 'boot DT gpu iommus 1')
    gmu_sids = sids(gmu, handle)
    if len(gmu_sids) != 1:
        fail('gmu iommus: expected one stream')
    put('SID_GMU', gmu_sids[0], 'boot DT gmu iommus')
    names, cells = gpu.strings('nvmem-cell-names'), gpu.u32s('nvmem-cells')
    if names != ['speed_bin'] or len(cells) != 1:
        fail('gpu: nvmem-cells is not the speed_bin cell')
    fuse = by_phandle(boot, cells[0])
    if fuse.parent is not qfprom:
        fail('gpu speed_bin cell is not in the qfprom node')
    bits = fuse.u32s('bits')
    if bits[0] != 0:
        fail('gpu speed-bin cell does not start at bit 0')
    put('QFPROM_SPEEDBIN_OFFSET', fuse.reg()[0][0], 'boot DT qfprom gpu-speed-bin reg')
    put('SPEEDBIN_BITS', bits[1], 'boot DT qfprom gpu-speed-bin bits')
    msm = boot.u32s('qcom,msm-id')
    socs = sorted({value & 0xffff for value in msm[0::2]})
    if len(socs) != 1:
        fail(f'boot DT qcom,msm-id names {socs}, not one SoC')
    put('SOC_ID', socs[0], f'boot DT qcom,msm-id {msm[0]:#x} (the DT ABL picked for this unit)')
    # The OPP tables, for the cross-check with the KGSL levels.
    opps = []
    for opp in by_phandle(boot, gpu.u32('operating-points-v2')).children.values():
        opps.append({'hz': opp.u64('opp-hz'), 'level': opp.u32('opp-level'), 'acd': opp.u32('qcom,opp-acd-level', 0),
                     'kbps': opp.u32('opp-peak-kBps'), 'hw': opp.u32('opp-supported-hw', 0)})
    gmu_opps = [(opp.u64('opp-hz'), opp.u32('opp-level')) for opp in by_phandle(boot, gmu.u32('operating-points-v2')).children.values()]
    put('OPPS', opps, 'boot DT gpu opp-table')
    put('GMU_OPPS', gmu_opps, 'boot DT gmu opp-table')
    _ = soc
    return values


LEVEL_PROPS = {'reg', 'qcom,gpu-freq', 'qcom,level', 'qcom,bus-min', 'qcom,bus-freq', 'qcom,bus-max', 'qcom,acd-level'}
BIN_PROPS = {'qcom,speed-bin', 'qcom,sku-codes', 'qcom,initial-pwrlevel', '#address-cells', '#size-cells'}


def level_bins(gpu, ddr_levels):
    """qcom,gpu-pwrlevel-bins in DT order; levels ascending, initial as the DT index (0 = fastest), as KGSL reads it."""
    result = []
    for node in gpu.children['qcom,gpu-pwrlevel-bins'].children.values():
        if not node.name.startswith('qcom,gpu-pwrlevels-'):
            continue
        extra = set(node.props) - BIN_PROPS - {'name'}
        if extra:
            fail(f'{node.path()}: unhandled properties {sorted(extra)}')
        levels = {}
        for level in node.children.values():
            extra = set(level.props) - LEVEL_PROPS - {'name'}
            if extra:
                fail(f'{level.path()}: unhandled properties {sorted(extra)}')
            freq = level.u32('qcom,gpu-freq')
            if freq == 0:
                continue
            if freq % 1000:
                fail(f'{level.path()}: frequency is not a whole kHz')
            bus = level.u32('qcom,bus-freq')
            entry = {'khz': freq // 1000, 'vlvl': level.u32('qcom,level'), 'bus_min': level.u32('qcom,bus-min', bus),
                     'bus': bus, 'bus_max': level.u32('qcom,bus-max', bus), 'acd': level.u32('qcom,acd-level', 0)}
            index = level.u32('reg')
            if index in levels:
                fail(f'{level.path()}: duplicate reg {index}')
            if not entry['bus_min'] <= entry['bus'] <= entry['bus_max'] < ddr_levels:
                fail(f'{level.path()}: bus votes out of order or past qcom,bus-table-ddr')
            levels[index] = entry
        count = len(levels)
        if sorted(levels) != list(range(count)) or not 0 < count <= MAX_LEVELS:
            fail(f'{node.path()}: level indices are not 0..{count - 1} or more than {MAX_LEVELS}')
        ordered = [levels[i] for i in range(count)]
        if any(b['khz'] > a['khz'] for a, b in zip(ordered, ordered[1:])):
            fail(f'{node.path()}: levels are not in descending frequency order')
        skus = node.u32s('qcom,sku-codes') if 'qcom,sku-codes' in node.props else []
        if len(skus) > MAX_SKUS:
            fail(f'{node.path()}: {len(skus)} sku codes')
        initial = node.u32('qcom,initial-pwrlevel')
        if initial >= count:
            fail(f'{node.path()}: bad qcom,initial-pwrlevel')
        speed = node.u32('qcom,speed-bin') if 'qcom,speed-bin' in node.props else None
        if speed is None and not skus:
            fail(f'{node.path()}: neither qcom,speed-bin nor qcom,sku-codes')
        result.append({'name': node.name, 'speed_bin': speed, 'skus': skus, 'initial': initial, 'levels': list(reversed(ordered))})
    if len(result) != BINS:
        fail(f'expected {BINS} power-level bins, found {len(result)}')
    return result


def common_levels(bins):
    count = min(len(b['levels']) for b in bins)
    common = []
    for i in range(count):
        first = bins[0]['levels'][i]
        if any((b['levels'][i]['khz'], b['levels'][i]['vlvl'], b['levels'][i]['acd']) !=
               (first['khz'], first['vlvl'], first['acd']) for b in bins):
            break
        common.append({key: (min(b['levels'][i][key] for b in bins) if key.startswith('bus') else first[key]) for key in first})
    return common


def runtime_values(live):
    """(B) from piano's live (stock) DT, as plain JSON values."""
    gpu, gmu = find(live, '/soc/qcom,kgsl-3d0@3d00000'), find(live, '/soc/qcom,gmu@3d37000')
    smmu = find(live, '/soc/kgsl-smmu@3da0000')
    zones = find(live, '/soc/thermal-zones')
    sensors = {zones.children[f'gpuss-{i}'].u32s('thermal-sensors')[0] for i in range(8)}
    if len(sensors) != 1:
        fail('the GPU thermal zones gpuss-0..7 do not share one TSENS block')
    tsens = by_phandle(live, sensors.pop())
    actlr = smmu.u32s('qcom,actlr')
    if len(actlr) != 3:
        fail('kgsl-smmu qcom,actlr is not one <SID MASK VALUE> triplet')
    table = gmu.u32s('qcom,gmu-freq-table')
    if len(table) != 4:
        fail('qcom,gmu-freq-table is not two <Hz vlvl> pairs')
    ddr = gpu.u32s('qcom,bus-table-ddr')
    msm = live.u32s('qcom,msm-id')
    fuse = find(live, '/soc/qfprom@221c8000/gpu_speed_bin@138')

    def sid(node):
        specifier = node.u32s('iommus')
        if len(specifier) != 3 or specifier[0] != smmu.u32('phandle'):
            fail(f'{node.path()}: iommus is not one <&kgsl_smmu SID MASK> specifier')
        return specifier[1]

    llcc = find(live, '/soc/cache-controller@24800000')
    return {
        'WINDOW_GPU': list(gpu.named_reg('kgsl_3d0_reg_memory')), 'WINDOW_RSCC': list(gpu.named_reg('rscc')),
        'WINDOW_CX_DBGC': list(gpu.named_reg('cx_dbgc')), 'WINDOW_CX_MISC': list(gpu.named_reg('cx_misc')),
        'WINDOW_GMU': list(gmu.named_reg('gmu')), 'WINDOW_GMU_AO': list(gmu.named_reg('gmu_ao_blk_dec0')),
        'WINDOW_GPUCC': list(find(live, '/soc/clock-controller@3d90000').reg()[0]),
        'WINDOW_GCC': list(find(live, '/soc/clock-controller@100000').reg()[0]),
        'WINDOW_SMMU': list(smmu.reg()[0]),
        'WINDOW_CMD_DB': list(find(live, '/reserved-memory/aop_cmd_db_region@81c60000').reg()[0]),
        'WINDOW_SMEM': list(find(live, '/reserved-memory/smem_region@81d00000').reg()[0]),
        'WINDOW_ZAP': list(find(live, '/reserved-memory/gpu_microcode_region@9b09a000').reg()[0]),
        'WINDOW_QFPROM': list(find(live, '/soc/qfprom@221c8000').reg()[0]),
        'WINDOW_LLCC': [list(llcc.named_reg(name)) for name in LLCC_NAMES],
        'CHIP_ID': gpu.u32('qcom,chipid'),
        'IRQ_GPU': gpu.named_interrupt('kgsl_3d0_irq'), 'IRQ_CX_HOST': gpu.named_interrupt('cx_host_irq'),
        'IRQ_HFI': gmu.named_interrupt('hfi'), 'IRQ_GMU': gmu.named_interrupt('gmu'),
        'SMMU_IRQS_ALL': [i for i, _ in smmu.interrupts()],
        'SID_GPU': sid(find(live, '/soc/qcom,kgsl-iommu@3da0000/gfx3d_user')),
        'SID_LPAC': sid(find(live, '/soc/qcom,kgsl-iommu@3da0000/gfx3d_lpac')),
        'SID_GMU': sid(gmu),
        'QFPROM_SPEEDBIN_OFFSET': fuse.reg()[0][0], 'SPEEDBIN_BITS': fuse.u32s('bits')[1],
        'WINDOW_RSC_DRV2': list(find(live, '/soc/rsc@16500000').named_reg('drv-2')),
        'WINDOW_AOSS_MSGRAM': list(find(live, '/soc/power-controller@c300000').reg()[0]),
        'WINDOW_IPCC': list(find(live, '/soc/qcom,ipcc@406000').reg()[0]),
        'WINDOW_TSENS_TM': list(tsens.reg()[0]),
        'TSENS_NAME': tsens.name,
        'IRQ_SCM_WAITQ': find(live, '/firmware/qcom_scm').interrupts()[0][0],
        'SMMU_ACTLR_DT': actlr[2],
        'UBWC_MODE': gpu.u32('qcom,ubwc-mode'),
        'MIN_ACCESS_LENGTH': gpu.u32('qcom,min-access-length'),
        'DDR_KBPS': ddr,
        'CNOC_LEVELS': gpu.u32s('qcom,bus-table-cnoc'),
        'GMU_KHZ': [table[0] // 1000, table[2] // 1000],
        'GMU_VLVL': [table[1], table[3]],
        'SOC_IDS': sorted({value & 0xffff for value in msm[0::2]}),
        'BINS': level_bins(gpu, len(ddr)),
    }


def cross_check(boot, runtime):
    """Every value both DTs carry agrees; the KGSL bin-0 levels are the boot DT's OPPs."""
    def same(name, a, b):
        if list(a) != list(b) if isinstance(a, (list, tuple)) else a != b:
            fail(f'{name}: boot DT {a} differs from the live DT {b}')
    for name in ('CHIP_ID', 'WINDOW_CX_MISC', 'WINDOW_GPUCC', 'WINDOW_GCC', 'WINDOW_SMMU', 'WINDOW_CMD_DB', 'WINDOW_SMEM', 'WINDOW_ZAP',
                 'WINDOW_QFPROM', 'IRQ_GPU', 'IRQ_HFI', 'IRQ_GMU', 'SMMU_IRQS_ALL', 'SID_GPU', 'SID_LPAC', 'SID_GMU',
                 'QFPROM_SPEEDBIN_OFFSET', 'SPEEDBIN_BITS'):
        same(name, boot[name][0], runtime[name])
    same('WINDOW_LLCC', [list(w) for w in boot['WINDOW_LLCC'][0]], runtime['WINDOW_LLCC'])
    base, size = boot['GPU_BASE'][0]
    if base != runtime['WINDOW_GPU'][0] or size < runtime['WINDOW_GPU'][1]:
        fail(f'GPU window: boot DT {base:#x}+{size:#x} does not cover the KGSL window {runtime["WINDOW_GPU"]}')
    cx, cxsize = boot['GMU_CX'][0]
    gmu, gmusize = runtime['WINDOW_GMU']
    if not (gmu <= cx and cx + cxsize <= gmu + gmusize):
        fail(f'GMU: the boot DT window {cx:#x}+{cxsize:#x} is not inside the KGSL GMU window {gmu:#x}+{gmusize:#x}')
    if boot['SOC_ID'][0] not in runtime['SOC_IDS']:
        fail(f'SoC id {boot["SOC_ID"][0]} of the boot DT is not one of the live DT qcom,msm-id SoCs {runtime["SOC_IDS"]}')
    opps = boot['OPPS'][0]
    for level in runtime['BINS'][0]['levels']:
        kbps = runtime['DDR_KBPS'][level['bus']]
        match = [o for o in opps if o['hz'] == level['khz'] * 1000 and o['level'] == level['vlvl'] and o['acd'] == level['acd']
                 and o['kbps'] == kbps]
        if not match:
            fail(f'KGSL bin-0 level {level} has no boot DT OPP with {level["khz"]} kHz, level {level["vlvl"]:#x}, '
                 f'ACD {level["acd"]:#x} and {kbps} kBps')
    gmu_opps = sorted(boot['GMU_OPPS'][0])
    if [(khz * 1000, vlvl) for khz, vlvl in zip(runtime['GMU_KHZ'], runtime['GMU_VLVL'])] != gmu_opps:
        fail(f'GMU OPPs {gmu_opps} differ from the KGSL GMU frequency table')


def digest_of(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def c_levels(levels):
    return '{' + ', '.join(f'{{{l["khz"]}u, {l["vlvl"]:#x}u, {l["bus_min"]}u, {l["bus"]}u, {l["bus_max"]}u, 0u, {l["acd"]:#010x}u}}'
                           for l in levels) + '}'


def window(value):
    return f'{{{value[0]:#x}ULL, {value[1]:#x}ULL}}'


def number(name, value):
    hexadecimal = name.startswith(('CHIP_ID', 'SMMU_ACTLR', 'QFPROM_SPEEDBIN_OFFSET', 'GMU_VLVL'))
    return f'{value:#x}u' if hexadecimal else f'{value}u'


def render(boot, runtime, digest, runtime_digest, boot_pin):
    lines = ['// SPDX-License-Identifier: BSD-2-Clause-Patent',
             '// Generated by tooling/k27/gen-piano-gpu-inputs.py; do not edit (re-run it after a DT or recording change).',
             f'// (A) piano\'s boot DT out/inputs/piano-boot.dtb (sha256 {boot_pin}); (B) KGSL policy of piano\'s live (stock) DT,',
             f'// recorded in package/PianoA830/board/piano-gpu-inputs.json (normalised JSON sha256 {runtime_digest}).',
             '// Interrupts are GIC INTIDs (SPI + 32). Level tables are ascending (GMU level = index + 1, level 0 is off);',
             '// PIANO_GPU_BIN_INITIAL is qcom,initial-pwrlevel, the DT index (0 = fastest), as KGSL reads it.',
             '#pragma once',
             f'#define PIANO_GPU_INPUTS_DIGEST "{digest}"',
             f'#define PIANO_GPU_RUNTIME_DT_DIGEST "{runtime_digest}"']
    b = {k: v[0] for k, v in boot.items()}
    windows = [  # name, value, note: (A) wherever the boot DT has the value, else (B)
        ('WINDOW_GPU', runtime['WINDOW_GPU'], f'live DT kgsl_3d0_reg_memory (the boot DT has {b["GPU_BASE"][0]:#x}+{b["GPU_BASE"][1]:#x})'),
        ('WINDOW_RSCC', runtime['WINDOW_RSCC'], 'live DT gpu reg rscc'),
        ('WINDOW_CX_DBGC', runtime['WINDOW_CX_DBGC'], 'live DT gpu reg cx_dbgc'),
        ('WINDOW_CX_MISC', b['WINDOW_CX_MISC'], boot['WINDOW_CX_MISC'][1]),
        ('WINDOW_GMU', runtime['WINDOW_GMU'], f'live DT gmu reg gmu (KGSL base; the boot DT\'s {b["GMU_CX"][0]:#x}+{b["GMU_CX"][1]:#x} is inside)'),
        ('WINDOW_GMU_AO', runtime['WINDOW_GMU_AO'], 'live DT gmu reg gmu_ao_blk_dec0'),
        ('WINDOW_GPUCC', b['WINDOW_GPUCC'], boot['WINDOW_GPUCC'][1]),
        ('WINDOW_GCC', b['WINDOW_GCC'], boot['WINDOW_GCC'][1]),
        ('WINDOW_SMMU', b['WINDOW_SMMU'], boot['WINDOW_SMMU'][1]),
        ('WINDOW_CMD_DB', b['WINDOW_CMD_DB'], boot['WINDOW_CMD_DB'][1]),
        ('WINDOW_SMEM', b['WINDOW_SMEM'], boot['WINDOW_SMEM'][1]),
        ('WINDOW_ZAP', b['WINDOW_ZAP'], boot['WINDOW_ZAP'][1]),
        ('WINDOW_QFPROM', b['WINDOW_QFPROM'], boot['WINDOW_QFPROM'][1]),
    ]
    for name, value, note in windows:
        lines.append(f'#define PIANO_GPU_{name} {window(value)}  // {note}')
    lines.append(f'#define PIANO_GPU_WINDOW_LLCC_COUNT {len(b["WINDOW_LLCC"])}u')
    lines.append(f'#define PIANO_GPU_WINDOW_LLCC {{{", ".join(window(v) for v in b["WINDOW_LLCC"])}}}  // {boot["WINDOW_LLCC"][1]}')
    lines.append(f'#define PIANO_GPU_CHIP_ID {number("CHIP_ID", b["CHIP_ID"])}  // {boot["CHIP_ID"][1]}')
    lines.append(f'#define PIANO_GPU_IRQ_GPU {b["IRQ_GPU"]}u  // {boot["IRQ_GPU"][1]}')
    lines.append(f'#define PIANO_GPU_IRQ_CX_HOST {runtime["IRQ_CX_HOST"]}u  // live DT gpu interrupt cx_host_irq (INTID)')
    lines.append(f'#define PIANO_GPU_IRQ_HFI {b["IRQ_HFI"]}u  // {boot["IRQ_HFI"][1]}')
    lines.append(f'#define PIANO_GPU_IRQ_GMU {b["IRQ_GMU"]}u  // {boot["IRQ_GMU"][1]}')
    lines.append(f'#define PIANO_GPU_IRQ_SMMU_GLOBAL {b["IRQ_SMMU_GLOBAL"]}u  // {boot["IRQ_SMMU_GLOBAL"][1]}')
    lines.append(f'#define PIANO_GPU_IRQ_SMMU_CB_COUNT {len(b["IRQ_SMMU_CB"])}u')
    lines.append(f'#define PIANO_GPU_IRQ_SMMU_CB {{{", ".join(f"{v}u" for v in b["IRQ_SMMU_CB"])}}}  // {boot["IRQ_SMMU_CB"][1]}')
    for name in ('SID_GPU', 'SID_LPAC', 'SID_GMU'):
        lines.append(f'#define PIANO_GPU_{name} {b[name]}u  // {boot[name][1]}')
    for name in ('QFPROM_SPEEDBIN_OFFSET', 'SPEEDBIN_BITS'):
        lines.append(f'#define PIANO_GPU_{name} {number(name, b[name])}  // {boot[name][1]}')
    lines.append(f'#define PIANO_GPU_SOC_ID {b["SOC_ID"]}u  // {boot["SOC_ID"][1]}; live DT qcom,msm-id SoCs '
                 f'{"/".join(map(str, runtime["SOC_IDS"]))}')
    lines.append(f'#define PIANO_GPU_SOC_IDS {{{", ".join(f"{v}u" for v in runtime["SOC_IDS"])}}}  // live DT qcom,msm-id SoCs')
    lines.append(f'#define PIANO_GPU_SOC_IDS_TEXT "{"/".join(map(str, runtime["SOC_IDS"]))}"')
    lines.append(f'#define PIANO_GPU_ZAP_FIRMWARE "{b["ZAP_FIRMWARE"]}"  // {boot["ZAP_FIRMWARE"][1]}')
    for name in ('WINDOW_RSC_DRV2', 'WINDOW_AOSS_MSGRAM', 'WINDOW_IPCC', 'WINDOW_TSENS_TM'):
        note = {'WINDOW_RSC_DRV2': 'live DT apps_rsc reg drv-2', 'WINDOW_AOSS_MSGRAM': 'live DT aoss_qmp reg (message RAM)',
                'WINDOW_IPCC': 'live DT ipcc_mproc reg',
                'WINDOW_TSENS_TM': f'live DT {runtime["TSENS_NAME"]} TM region (sensors of gpuss-0..7)'}[name]
        lines.append(f'#define PIANO_GPU_{name} {window(runtime[name])}  // {note}')
    for name, note in (('IRQ_SCM_WAITQ', 'live DT qcom_scm interrupt (INTID, edge)'),
                       ('SMMU_ACTLR_DT', 'live DT kgsl-smmu qcom,actlr value (KGSL); the 13T keeps only 0x303 under Gunyah'),
                       ('UBWC_MODE', 'live DT qcom,ubwc-mode'), ('MIN_ACCESS_LENGTH', 'live DT qcom,min-access-length')):
        lines.append(f'#define PIANO_GPU_{name} {number(name, runtime[name])}  // {note}')
    for name, note in (('DDR_KBPS', 'live DT qcom,bus-table-ddr'), ('CNOC_LEVELS', 'live DT qcom,bus-table-cnoc'),
                       ('GMU_KHZ', 'live DT qcom,gmu-freq-table (Hz / 1000) = the boot DT GMU OPPs'),
                       ('GMU_VLVL', 'live DT qcom,gmu-freq-table levels = the boot DT GMU opp-level')):
        value = runtime[name]
        lines.append(f'#define PIANO_GPU_{name}_COUNT {len(value)}u')
        lines.append(f'#define PIANO_GPU_{name} {{{", ".join(number(name, v) for v in value)}}}  // {note}')
    bins = runtime['BINS']
    lines.append(f'#define PIANO_GPU_BIN_COUNT {len(bins)}u  // live DT qcom,gpu-pwrlevel-bins, DT order (bin 0 = the boot DT OPPs)')
    for i, bin_ in enumerate(bins):
        lines.append(f'#define PIANO_GPU_BIN{i}_COUNT {len(bin_["levels"])}u  // {bin_["name"]}')
        lines.append(f'#define PIANO_GPU_BIN{i}_LEVELS {c_levels(bin_["levels"])}')
    lines.append(f'#define PIANO_GPU_BIN_SPEED {{{", ".join(f"{(x["speed_bin"] or 0):#x}u" for x in bins)}}}  // qcom,speed-bin (0: SKU bin)')
    lines.append(f'#define PIANO_GPU_BIN_SKU_COUNT {{{", ".join(f"{len(x["skus"])}u" for x in bins)}}}  // qcom,sku-codes count')
    lines.append('#define PIANO_GPU_BIN_SKU {' + ', '.join(
        '{' + ', '.join(f'{v}u' for v in (x['skus'] + [0] * MAX_SKUS)[:MAX_SKUS]) + '}' for x in bins) + '}  // qcom,sku-codes')
    lines.append(f'#define PIANO_GPU_BIN_INITIAL {{{", ".join(f"{x["initial"]}u" for x in bins)}}}  // qcom,initial-pwrlevel')
    common = common_levels(bins)
    lines.append(f'#define PIANO_GPU_COMMON_LEVEL_COUNT {len(common)}u  // the <= {common[-1]["khz"] // 1000} MHz levels every bin shares')
    lines.append(f'#define PIANO_GPU_COMMON_LEVELS {c_levels(common)}')
    return '\n'.join(lines) + '\n'


def compare(text, pagani):
    """The defines of this header against the 13T's PaganiGpuInputs.h (names mapped), comments ignored."""
    def defines(source, prefix):
        out = {}
        for line in source.splitlines():
            m = re.match(rf'#define {prefix}(\w+) (.*?)(\s*//.*)?$', line)
            if m:
                out[m.group(1)] = m.group(2).strip()
        return out
    ours, theirs = defines(text, 'PIANO_GPU_'), defines(Path(pagani).read_text(), 'PAGANI_GPU_')
    rows = []
    for name in sorted(set(ours) | set(theirs)):
        if ours.get(name) != theirs.get(name):
            rows.append(f'  {name}: piano {ours.get(name, "-")} | 13T {theirs.get(name, "-")}')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot-dtb', type=Path, default=BOOT_DTB)
    parser.add_argument('--live-dtb', type=Path, help=f'piano\'s live DT (pinned): re-extract and compare (B) (e.g. {LIVE_DTB})')
    parser.add_argument('--record', action='store_true', help='with --live-dtb: record (B) and the DT pins (after checking them)')
    parser.add_argument('--check', action='store_true', help='compare with the committed header instead of writing it')
    parser.add_argument('--compare', type=Path, help="the 13T's PaganiGpuInputs.h: list the defines that differ")
    parser.add_argument('--dts-out', type=Path, default=REPO / 'out/native-kexts/PianoA830/inputs/piano-boot.dts')
    args = parser.parse_args()
    pins = json.loads(PINS.read_text()) if PINS.is_file() else {}
    data = args.boot_dtb.read_bytes()
    boot_pin = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    if pins.get('boot_dtb') and pins['boot_dtb'] != boot_pin and not args.record:
        fail(f'{args.boot_dtb} is not the pinned boot DT ({pins["boot_dtb"]["sha256"]}); check it, then --record')
    dtc_record(args.boot_dtb, args.dts_out)
    boot = boot_values(read_fdt(data))
    recorded = pins.get('runtime_dt', {})
    if args.live_dtb:
        live_data = args.live_dtb.read_bytes()
        live_pin = {'bytes': len(live_data), 'sha256': hashlib.sha256(live_data).hexdigest()}
        if pins.get('live_dtb') and pins['live_dtb'] != live_pin and not args.record:
            fail(f'{args.live_dtb} is not the pinned live DT ({pins["live_dtb"]["sha256"]})')
        runtime = runtime_values(read_fdt(live_data))
        cross_check(boot, runtime)
        if args.record:
            pins['boot_dtb'], pins['live_dtb'] = boot_pin, live_pin
            pins['runtime_dt'] = {'comment': "KGSL policy values of piano's live DT (normalised; sha256 of this values object as "
                                             'compact sorted JSON)', 'sha256': digest_of(runtime), 'values': runtime}
            PINS.write_text(json.dumps(pins, indent=2, ensure_ascii=False) + '\n')
            recorded = pins['runtime_dt']
        elif runtime != recorded.get('values'):
            fail('the live DT values differ from the recorded ones; run with --record after checking them')
    elif args.record:
        fail('--record needs --live-dtb')
    runtime = recorded.get('values')
    if runtime is None:
        fail(f'no recorded live DT values; run with --live-dtb {LIVE_DTB} --record')
    runtime_digest = digest_of(runtime)
    if runtime_digest != recorded.get('sha256'):
        fail(f'the recorded live DT values do not match their pin {recorded.get("sha256")}')
    cross_check(boot, runtime)
    plain = {k: v for k, (v, _) in boot.items()}
    digest = digest_of({'boot': plain, 'runtime': runtime})
    text = render(boot, runtime, digest, runtime_digest, boot_pin['sha256'])
    if args.compare:
        rows = compare(text, args.compare)
        print(f'{len(rows)} defines differ from {args.compare.name}:' if rows else f'no define differs from {args.compare.name}')
        print('\n'.join(rows))
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_text() != text:
            fail(f'{OUTPUT} is stale; run without --check')
        print(f'PASS: {OUTPUT.relative_to(REPO)} matches the boot DT and the recorded live DT values')
        return
    OUTPUT.write_text(text)
    print(f'wrote {OUTPUT.relative_to(REPO)} (digest {digest[:16]})')


if __name__ == '__main__':
    main()
