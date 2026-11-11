#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of tooling/k27/gen-touch-calibration.py: the synthetic calibration (tests/synthetic-calibration.json, made up) loads
and its header carries it; every kind of invalid file is refused with a reason and gives the linear-map header; with
PIANO_TOUCH_CALIBRATION (environment or ~/.config/piano/env) the unit's file loads too. Only properties are checked and printed:
never a value of the unit's maps.
    python3 calibration_gen_test.py
"""
import copy
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gen', HERE.parents[2] / 'tooling/k27/gen-touch-calibration.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
failures = 0


def check(condition, what):
    global failures
    if not condition:
        print('FAIL', what)
        failures += 1


synthetic = json.loads((HERE / 'synthetic-calibration.json').read_text())
with tempfile.TemporaryDirectory() as tmp:
    def write(doc):
        path = Path(tmp) / 'c.json'
        path.write_text(json.dumps(doc))
        return path

    cal = gen.load(HERE / 'synthetic-calibration.json')
    check(cal['row_map'] == synthetic['calibration']['row_map'] and cal['project_vendor_id'] == 26 and cal['invert_y'] == -1,
          'synthetic loads')
    text = gen.header(cal, '7186ba60', 'kSynthetic', 'test')
    check('kSyntheticCalibrationPresent = true' in text and ', 21360, 32000, 2700, 600, 4350, 26, -1};' in text, 'synthetic header')
    check('kUnitCalibrationPresent = false' in gen.header(None, 'none', 'kUnit', 'test'), 'fallback header')
    # This unit's Y orientation from the build (PIANO_TOUCH_INVERT_Y): its own constant, with or without a calibration.
    check('kSyntheticInvertYOverride = -1;' in text, 'no override by default')
    check('kUnitInvertYOverride = 1;' in gen.header(None, 'none', 'kUnit', 'test', 1), 'override without a calibration')
    check('kSyntheticInvertYOverride = 0;' in gen.header(cal, '7186ba60', 'kSynthetic', 'test', 0), 'override 0')
    check(gen.invert_override('none') == (-1, '') and gen.invert_override('1')[0] == 1 and gen.invert_override('0')[0] == 0,
          'explicit overrides')
    import os
    saved = os.environ.get('PIANO_TOUCH_INVERT_Y')
    for value, want in (('1', 1), ('true', 1), ('0', 0), ('no', 0), ('', -1), ('maybe', -1)):
        os.environ['PIANO_TOUCH_INVERT_Y'] = value
        check(gen.invert_override('env')[0] == want, f'PIANO_TOUCH_INVERT_Y={value!r}')
    if saved is None:
        del os.environ['PIANO_TOUCH_INVERT_Y']
    else:
        os.environ['PIANO_TOUCH_INVERT_Y'] = saved

    def bad(mutate, why):
        doc = copy.deepcopy(synthetic)
        mutate(doc)
        try:
            gen.load(write(doc))
            check(False, f'invalid accepted: {why}')
        except ValueError:
            pass

    bad(lambda d: d.update(schema=2), 'schema 2')
    bad(lambda d: d.update(rows=41), '41 rows')
    bad(lambda d: d.update(units_per_pixel=1), 'units 1')
    bad(lambda d: d['calibration']['row_map'].pop(), 'row map too short')
    bad(lambda d: d['calibration']['row_map'].__setitem__(5, d['calibration']['row_map'][4]), 'row map not increasing')
    bad(lambda d: d['calibration']['column_map'].__setitem__(59, 3200), 'column beyond its maximum')
    bad(lambda d: d['calibration'].update(edge_size=10680), 'edge too wide')
    bad(lambda d: d['calibration'].update(edge_base=0), 'edge base 0')
    bad(lambda d: d['calibration'].pop('edge_gain'), 'edge gain missing')
    bad(lambda d: d['calibration'].update(row_maximum=40000), 'maximum beyond int16')
    bad(lambda d: d['calibration']['row_map'].__setitem__(0, -1), 'negative position')
    bad(lambda d: d['calibration']['row_map'].__setitem__(0, 1.5), 'non-integer position')
    bad(lambda d: d.update(invert_y=1), 'invert_y not a boolean')
    doc = copy.deepcopy(synthetic)
    doc['invert_y'] = True
    check(gen.load(write(doc))['invert_y'] == 1, 'invert_y override')
    (Path(tmp) / 'broken.json').write_text('{not json')
    try:
        gen.load(Path(tmp) / 'broken.json')
        check(False, 'broken JSON accepted')
    except ValueError:
        pass

real = gen.configured_path()
if real and real.is_file():
    try:
        unit = gen.load(real)
        steps = [b - a for m in (unit['row_map'], unit['column_map']) for a, b in zip(m, m[1:])]
        check(unit['project_vendor_id'] in (4, 26) and 40 <= min(steps) and max(steps) <= 70 and
              (unit['row_maximum'], unit['column_maximum']) == (21360, 32000), 'unit calibration properties')
        print(f'unit calibration: valid (panel {gen.PANELS.get(unit["project_vendor_id"], "unknown")})')
    except ValueError as error:
        check(False, f'unit calibration invalid: {error}')
else:
    print('unit calibration: not configured (PIANO_TOUCH_CALIBRATION): its test skipped')
if failures:
    print(f'calibration generator tests: {failures} FAILED')
    sys.exit(1)
print('PASS: calibration generator (synthetic, 13 invalid kinds, fallback header, the Y override)')
