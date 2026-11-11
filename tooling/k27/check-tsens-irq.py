#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check PianoThermal's uplow interrupt table (package/PianoThermal/TsensIrq.h, printed by tests/uplow_host --map) against the piano DT.

    check-tsens-irq.py PIANO.dtb MAP.txt

For every qcom,tsens-v2 node: its TM address, interrupt-names "uplow" then "critical", interrupts <0 SPI 4> each (GIC SPI, level
high), and the interrupt parent it inherits (its own interrupt-parent or its nearest ancestor's) being the arm,gic-v3 node; the map's
row for that TM address must name the same two SPIs. Also prints, for information, which PDC pin each uplow SPI is the output of
(qcom,pdc-ranges): the line passes through the PDC, which nothing here programs. Exit 1 on any difference (the DT is decompiled
with dtc; nothing is written).
"""
import re
import subprocess
import sys


def parse(dts):
    """The DTS as a list of nodes: (path, properties dict of raw values, parent index)."""
    nodes, stack = [], []
    for raw in dts.split('\n'):
        line = raw.strip()
        if not line or line.startswith('/dts-v1/') or line.startswith('//'):
            continue
        if line.endswith('{'):
            name = line[:-1].strip()
            name = name.split(':', 1)[1].strip() if ':' in name and not name.startswith('/') else name
            parent = stack[-1] if stack else None
            path = '/' if name == '/' else (nodes[parent][0].rstrip('/') + '/' + name if parent is not None else name)
            nodes.append((path, {}, parent))
            stack.append(len(nodes) - 1)
        elif line.startswith('};'):
            if stack:
                stack.pop()
        elif stack:
            m = re.match(r'([^\s=]+)\s*=\s*(.*);$', line)
            if m:
                nodes[stack[-1]][1][m.group(1)] = m.group(2)
            elif line.endswith(';'):
                nodes[stack[-1]][1][line[:-1]] = ''
    return nodes


def cells(value):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', value)]


def strings(value):
    return re.findall(r'"([^"]*)"', value)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', sys.argv[1]], check=True, capture_output=True, text=True).stdout
    nodes = parse(dts)
    gic = [cells(p['phandle'])[0] for _, p, _ in nodes if '"arm,gic-v3"' in p.get('compatible', '') and 'phandle' in p]
    pdc = [cells(p['qcom,pdc-ranges']) for _, p, _ in nodes if 'qcom,pdc-ranges' in p]
    rows = {}
    for line in open(sys.argv[2]):
        tm, uplow, critical = line.split()
        rows[int(tm, 16)] = (int(uplow), int(critical))
    errors, seen, infos = [], set(), []
    if len(gic) != 1:
        errors.append(f'{len(gic)} arm,gic-v3 nodes with a phandle')
    for i, (path, props, parent) in enumerate(nodes):
        if '"qcom,tsens-v2"' not in props.get('compatible', ''):
            continue
        tm = cells(props.get('reg', ''))[1] if len(cells(props.get('reg', ''))) > 1 else None
        names = strings(props.get('interrupt-names', ''))
        irqs = cells(props.get('interrupts', ''))
        j, ip = i, None
        while j is not None and ip is None:
            if 'interrupt-parent' in nodes[j][1]:
                ip = cells(nodes[j][1]['interrupt-parent'])[0]
            j = nodes[j][2]
        where = f'{path} (TM {tm:#x})' if tm is not None else path
        if tm not in rows:
            errors.append(f'{where}: no row in the map')
            continue
        seen.add(tm)
        if names != ['uplow', 'critical']:
            errors.append(f'{where}: interrupt-names {names}, expected uplow, critical')
        if len(irqs) != 6 or irqs[0] != 0 or irqs[3] != 0 or irqs[2] != 4 or irqs[5] != 4:
            errors.append(f'{where}: interrupts {irqs}: not two level-high GIC SPIs')
        elif (irqs[1], irqs[4]) != rows[tm]:
            errors.append(f'{where}: DT uplow/critical SPI {irqs[1]}/{irqs[4]}, map {rows[tm][0]}/{rows[tm][1]}')
        if gic and ip != gic[0]:
            errors.append(f'{where}: interrupt parent {ip}, not the GIC ({gic[0] if gic else "?"})')
        if pdc and len(irqs) == 6:
            r = pdc[0]
            for k in range(0, len(r) - 2, 3):
                pin, spi, count = r[k], r[k + 1], r[k + 2]
                if spi <= irqs[1] < spi + count:
                    infos.append(f'SPI {irqs[1]} = PDC pin {pin + irqs[1] - spi}')
    for tm in sorted(set(rows) - seen):
        errors.append(f'map row {tm:#x}: no qcom,tsens-v2 node at that TM address')
    for e in errors:
        print(f'check-tsens-irq: {e}', file=sys.stderr)
    if errors:
        sys.exit(1)
    print(f'check-tsens-irq: {len(seen)} controllers: uplow/critical SPIs, level high, GIC parent match the DT'
          + (f' ({", ".join(infos)}: the PDC passes them, not programmed here)' if infos else ''))


if __name__ == '__main__':
    main()
