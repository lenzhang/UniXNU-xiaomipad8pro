#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check PianoThermal's TSENS sensor map (package/PianoThermal/Tsens.h, printed by its host test with --map) against the piano DT.

    check-thermal-map.py PIANO.dtb MAP.txt

For every qcom,tsens-v2 node: its TM address and #qcom,sensors; for every thermal zone on one of them (thermal-sensors = <&tsens N>):
the zone's name (minus "-thermal"), its passive trip and hysteresis, its hot and critical trips, all against the map's row for that
controller and sensor. Exit 1 on any difference (the DT is decompiled with dtc; nothing is written).
"""
import re
import subprocess
import sys


def parse(dts):
    lines = dts.split('\n')
    tsens = {}   # phandle -> (tm address, sensors)
    for i, line in enumerate(lines):
        if '"qcom,tsens-v2"' not in line:
            continue
        block = []
        for l in lines[i:i + 12]:
            block.append(l)
            if l.strip() == '};':
                break
        text = '\n'.join(block)
        reg = re.search(r'reg = <0x00 (0x[0-9a-f]+) ', text)
        count = re.search(r'#qcom,sensors = <(0x[0-9a-f]+)>', text)
        ph = re.search(r'phandle = <(0x[0-9a-f]+)>', text)
        if reg and count and ph:
            tsens[ph.group(1)] = (int(reg.group(1), 16), int(count.group(1), 16))
    zones = {}   # (tm, sensor) -> dict
    start = next(i for i, l in enumerate(lines) if l.strip() == 'thermal-zones {')
    depth, zone, trip = 0, None, None
    for line in lines[start:]:
        s = line.strip()
        if s.endswith('{'):
            depth += 1
            name = s[:-1].strip()
            if depth == 2:
                zone = {'name': name[:-len('-thermal')] if name.endswith('-thermal') else name, 'trips': []}
            elif depth == 4 and zone is not None:
                trip = {}
        elif s.startswith('}'):
            if depth == 4 and trip is not None and 'temperature' in trip:
                zone['trips'].append(trip)
            trip = None if depth == 4 else trip
            if depth == 2 and zone.get('sensor'):
                zones[zone['sensor']] = zone
            depth -= 1
            if depth == 0:
                break
        elif zone is not None:
            m = re.match(r'thermal-sensors = <(0x[0-9a-f]+) (0x[0-9a-f]+)>;', s)
            if m and m.group(1) in tsens:
                zone['sensor'] = (tsens[m.group(1)][0], int(m.group(2), 16))
            for key, pattern in (('temperature', r'temperature = <(0x[0-9a-f]+)>;'), ('hysteresis', r'hysteresis = <(0x[0-9a-f]+)>;')):
                m = re.match(pattern, s)
                if m and trip is not None:
                    trip[key] = int(m.group(1), 16)
            m = re.match(r'type = "(\w+)";', s)
            if m and trip is not None:
                trip['type'] = m.group(1)
    return tsens, zones


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', sys.argv[1]], check=True, capture_output=True, text=True).stdout
    tsens, zones = parse(dts)
    rows = {}
    for line in open(sys.argv[2]):
        tm, count, sensor, name, passive, hyst, hot, critical = line.split()
        rows[(int(tm, 16), int(sensor))] = (int(count), name, int(passive), int(hyst), int(hot), int(critical))
    errors = []
    counts = {tm: n for tm, n in tsens.values()}
    for (tm, sensor), (count, name, passive, hyst, hot, critical) in sorted(rows.items()):
        if counts.get(tm) != count:
            errors.append(f'{tm:#x}: {count} sensors in the map, {counts.get(tm)} in the DT')
        z = zones.get((tm, sensor))
        if not z:
            errors.append(f'{tm:#x} s{sensor} {name}: no thermal zone in the DT')
            continue
        trips = {t.get('type'): t for t in z['trips']}
        want = (z['name'], trips.get('passive', {}).get('temperature', 0) // 100, trips.get('passive', {}).get('hysteresis', 0) // 100,
                trips.get('hot', {}).get('temperature', 0) // 100, trips.get('critical', {}).get('temperature', 0) // 100)
        if want != (name, passive, hyst, hot, critical):
            errors.append(f'{tm:#x} s{sensor}: map {(name, passive, hyst, hot, critical)}, DT {want}')
    for key, z in zones.items():
        if key not in rows:
            errors.append(f'{key[0]:#x} s{key[1]} {z["name"]}: in the DT, not in the map')
    if len(counts) != 4:
        errors.append(f'{len(counts)} qcom,tsens-v2 nodes in the DT, the map has 4')
    if errors:
        print('check-thermal-map: ' + '\n  '.join(errors))
        sys.exit(1)
    print(f'check-thermal-map: {len(rows)} sensors on {len(counts)} controllers match the DT\'s thermal zones')


if __name__ == '__main__':
    main()
