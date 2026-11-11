#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare PianoThermal's board thermistor curve (package/PianoThermal/Ntc.h: Steinhart-Hart from the part's R25, B25/50 and
B25/85) with the vendor resistance-temperature data Linux tabulates for the same part (adcmap7_100k, "NTCG104EF104", in
drivers/iio/adc/qcom-vadc-common.c of a Linux tree). The table is read from that tree at run time; nothing of it is kept.

    check-ntc-curve.py LINUX_TREE ADC_HOST

ADC_HOST is the built host test (out/native-kexts/PianoThermal/host-tests/adc_host), asked with --curve for each resistance. The
table's temperatures are compared both as degrees C * 1024 and as milli-degrees C (what Linux reports). Exit 1 when the curve is more
than 0.6 C from the table read as degrees C * 1024 anywhere in -40..127 C (docs/drivers/thermal.md 12.3).
"""
import re
import subprocess
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = (Path(sys.argv[1]) / 'drivers/iio/adc/qcom-vadc-common.c').read_text()
    start = src.index('adcmap7_100k[] = {')
    table = [(int(r), int(t)) for r, t in re.findall(r'\{\s*(\d+),\s*(-?\d+)\s*\}', src[start:src.index('};', start)])]
    out = subprocess.run([sys.argv[2], '--curve'] + [str(r) for r, _ in table], capture_output=True, text=True, check=True).stdout
    ours = {int(a): int(b) for a, b in (l.split() for l in out.split('\n') if l.strip())}
    for name, scale in (('degrees C * 1024', 1024.0), ('milli-degrees C (as Linux reports)', 1000.0)):
        worst = {}
        for r, t in table:
            c = t / scale
            band = '-40..0 C' if c < 0 else '0..60 C' if c <= 60 else '60..127 C'
            e = ours[r] / 1000.0 - c
            if abs(e) > abs(worst.get(band, 0.0)):
                worst[band] = e
        print(f'table read as {name}: largest difference ' + ', '.join(f'{b} {worst[b]:+.3f} C' for b in ('-40..0 C', '0..60 C', '60..127 C')))
        if scale == 1024.0:
            bad = max(abs(v) for v in worst.values()) > 0.6
    print(f'{len(table)} points; at 100 kOhm the table says {dict(table)[100000]}, the curve {ours[100000]} milli-C')
    sys.exit(1 if bad else 0)


main()
