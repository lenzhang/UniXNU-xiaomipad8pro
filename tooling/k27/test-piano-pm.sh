#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0061/0062's decisions (tooling/k27/piano-pm/piano_pm_policy.h: the idle state choice with its domains and
# latency holds, the PSCI and LLCC verdicts, the wake sources, the Type-C address, the wake calendar), with and without the
# sanitizers, and the check that patches/k27/0061 adds that header byte for byte as osfmk/arm64/piano_pm_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/piano-pm
mkdir -p "$T"
S=$D/tooling/k27/piano-pm
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_pm_policy.c" -o "$T/test_piano_pm_policy"
"$T/test_piano_pm_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_pm_policy.c" -o "$T/test_piano_pm_policy_san"
"$T/test_piano_pm_policy_san"
PATCH=$(ls "$D"/patches/k27/0061-*.patch)
python3 - "$PATCH" "$S/piano_pm_policy.h" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ b/'):
        inside = line == '+++ b/osfmk/arm64/piano_pm_policy.h'
        continue
    if inside:
        if line.startswith('diff --git') or line.startswith('-- '):
            break
        if line.startswith('+'):
            out.append(line[1:])
        elif line.startswith('@@') or line == '':
            continue
        else:
            raise SystemExit('test-piano-pm: unexpected line in the header hunk: ' + line[:60])
want = open(header).read()
got = '\n'.join(out) + '\n'
if got != want:
    raise SystemExit('test-piano-pm: %s does not add %s byte for byte' % (patch.split('/')[-1], header.split('/')[-1]))
print('test-piano-pm: the 0061 patch carries piano_pm_policy.h unchanged (%d lines)' % len(out))
EOF
# --kext: package/include/PianoPowerContract.h compiled as a kext (the k27 objects' recipe, -Werror), and with K27_KERNEL set
# (a 0062 kernel) its imports resolved against that kernel.
if [ "${1:-}" = --kext ]; then
  python3 "$D/tooling/k27/build-native-kext.py" PianoPowerContractCheck "$S/contract_check.cpp" --out "$T/contract-check" \
    2>&1 | grep -v -e CCC_OVERRIDE -e 'Adding argument' || true
  [ -f "$T/contract-check/contract_check.o" ] || { echo "test-piano-pm: PianoPowerContract.h does not compile as a kext" >&2; exit 1; }
  if [ -n "${K27_KERNEL:-}" ]; then
    printf '["%s"]\n' "$T/contract-check/contract_check.o" > "$T/contract-check/objects.json"
    python3 "$D/tooling/k27/check-kext-imports.py" "$T/contract-check/objects.json" "$K27_KERNEL"
  fi
fi
