#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check PianoThermal's board thermistor map (package/PianoThermal/AdcChannels.h, printed by its host test with --map) against the
piano DT (B6, docs/drivers/thermal.md section 12).

    check-adc-map.py PIANO.dtb MAP.txt

From the DT: the SPMI arbiter's qcom,ee; the "qcom,spmi-adc5-gen3" node's SDAMs (reg); each of its channels' reg, label,
qcom,ratiometric, qcom,pre-scaling, qcom,decimation, qcom,hw-settle-time and qcom,adc5-gen4; the thermal zone of each channel's
generic-adc-thermal consumer; the pins of the ADC node's pinctrl states (bias-high-impedance) with their PMIC's SID. All against the
map, both ways. Exit 1 on any difference (the DT is decompiled with dtc; nothing is written).
"""
import re
import subprocess
import sys


def tree(dts):
    root = {'name': '/', 'props': {}, 'children': [], 'parent': None}
    stack = [root]
    for raw in dts.split('\n'):
        s = raw.strip()
        if not s or s.startswith('/dts-v1/') or s.startswith('//') or s.startswith('/memreserve/'):
            continue
        if s.endswith('{'):
            node = {'name': s[:-1].strip(), 'props': {}, 'children': [], 'parent': stack[-1]}
            stack[-1]['children'].append(node)
            stack.append(node)
        elif s.startswith('};'):
            stack.pop()
        elif s.endswith(';'):
            body = s[:-1]
            if '=' in body:
                key, value = body.split('=', 1)
                stack[-1]['props'][key.strip()] = value.strip()
            else:
                stack[-1]['props'][body.strip()] = True
    return root


def walk(node):
    yield node
    for child in node['children']:
        yield from walk(child)


def cells(value):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', value)] if isinstance(value, str) else []


def text(value):
    m = re.match(r'"([^"]*)"', value) if isinstance(value, str) else None
    return m.group(1) if m else None


def strings(value):
    return re.findall(r'"([^"]*)"', value) if isinstance(value, str) else []


def from_dt(path):
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', path], capture_output=True, text=True, check=True).stdout
    root = tree(dts)
    nodes = list(walk(root))
    by_phandle = {cells(n['props']['phandle'])[0]: n for n in nodes if 'phandle' in n['props']}
    lines = set()
    adcs = [n for n in nodes if 'qcom,spmi-adc5-gen3' in strings(n['props'].get('compatible'))]
    if len(adcs) != 1:
        sys.exit(f'check-adc-map: {len(adcs)} qcom,spmi-adc5-gen3 nodes in the DT, expected 1')
    adc = adcs[0]
    arb = adc['parent']['parent']
    lines.add(f"ee {cells(arb['props'].get('qcom,ee', ''))[0] if cells(arb['props'].get('qcom,ee', '')) else '?'}")
    lines.add('sdam ' + ' '.join(hex(x) for x in cells(adc['props'].get('reg'))))
    adc_phandle = cells(adc['props'].get('phandle'))[0]
    zone_of = {}   # channel reg -> zone name
    for n in nodes:
        if 'generic-adc-thermal' not in strings(n['props'].get('compatible')):
            continue
        io = cells(n['props'].get('io-channels'))
        if len(io) == 2 and io[0] == adc_phandle and 'phandle' in n['props']:
            sensor = cells(n['props']['phandle'])[0]
            for z in nodes:
                if cells(z['props'].get('thermal-sensors')) == [sensor] and z['name'].endswith('-thermal'):
                    zone_of[io[1]] = z['name'][:-len('-thermal')]
    for ch in adc['children']:
        p = ch['props']
        reg = cells(p.get('reg'))[0]
        pre = cells(p.get('qcom,pre-scaling'))
        lines.add(f"channel {hex(reg)} label {text(p.get('label'))} zone {zone_of.get(reg, '?')} "
                  f"gen {4 if 'qcom,adc5-gen4' in p else 3} decimation {cells(p.get('qcom,decimation'))[0]} "
                  f"settle {cells(p.get('qcom,hw-settle-time'))[0]} ratiometric {1 if 'qcom,ratiometric' in p else 0} "
                  f"prescale {pre[0]}/{pre[1]}" if len(pre) == 2 else f"channel {hex(reg)} prescale ?")
    for ph in cells(adc['props'].get('pinctrl-0')):
        state = by_phandle.get(ph)
        if not state or 'bias-high-impedance' not in state['props']:
            lines.add(f'pin state {hex(ph)} not bias-high-impedance')
            continue
        pmic = state['parent']['parent']   # state -> gpio controller -> pmic@N
        sid = cells(pmic['props'].get('reg'))[0]
        for pin in strings(state['props'].get('pins')):
            lines.add(f'pin sid {sid} gpio {int(pin[4:])}')
    return lines


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dt = from_dt(sys.argv[1])
    ours = {l.strip() for l in open(sys.argv[2]) if l.strip()}
    missing, extra = sorted(dt - ours), sorted(ours - dt)
    for l in missing:
        print(f'check-adc-map: in the DT, not in AdcChannels.h: {l}')
    for l in extra:
        print(f'check-adc-map: in AdcChannels.h, not in the DT: {l}')
    if missing or extra:
        sys.exit(1)
    print(f"check-adc-map: {sum(l.startswith('channel ') for l in ours)} channels, {sum(l.startswith('pin ') for l in ours)} "
          'thermistor pins, the arbiter EE and the SDAMs match the DT')


main()
