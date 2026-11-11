#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0061/0062's decisions (tooling/k27/piano-pm/piano_pm_policy.h: the idle state choice with its domains and
# latency holds, the PSCI and LLCC verdicts, the wake sources, the Type-C address, the wake calendar) and of 0064's
# (piano_s2idle_policy.h: the LLCC island read only at an ss3 decision, a driver's cancel of the suspend in progress), with and
# without the sanitizers, and the check that patches/k27/0061 and 0064 add those headers byte for byte under osfmk/arm64/.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/piano-pm
mkdir -p "$T"
S=$D/tooling/k27/piano-pm
for t in test_piano_pm_policy test_piano_s2idle_policy; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/$t.c" -o "$T/$t"
  "$T/$t"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
    "$S/$t.c" -o "$T/${t}_san"
  "$T/${t}_san"
done
for pair in 0061:piano_pm_policy.h 0064:piano_s2idle_policy.h; do
PATCH=$(ls "$D"/patches/k27/"${pair%%:*}"-*.patch)
python3 - "$PATCH" "$S/${pair#*:}" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
name = header.split('/')[-1]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ b/'):
        inside = line == '+++ b/osfmk/arm64/' + name
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
    raise SystemExit('test-piano-pm: %s does not add %s byte for byte' % (patch.split('/')[-1], name))
print('test-piano-pm: the %s patch carries %s unchanged (%d lines)' % (patch.split('/')[-1][:4], name, len(out)))
EOF
done
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
