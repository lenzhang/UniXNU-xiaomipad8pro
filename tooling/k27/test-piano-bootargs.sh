#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0066's string handling (tooling/k27/piano-bootargs/piano_bootargs_policy.h: the set a selector names, the merge
# of DarwinBoot's line and the ADT's tail, kern.bootargs' clip), with and without the sanitizers; the check that patches/k27/0066
# adds that header byte for byte as pexpert/arm/piano_bootargs_policy.h; and the host side (tooling/adt/boot_args.py: the split, the
# ADT property, the kernel's merge redone) against the same numbers.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/piano-bootargs
mkdir -p "$T"
S=$D/tooling/k27/piano-bootargs
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_bootargs_policy.c" -o "$T/test_piano_bootargs_policy"
"$T/test_piano_bootargs_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_bootargs_policy.c" -o "$T/test_piano_bootargs_policy_san"
"$T/test_piano_bootargs_policy_san"
PATCH=$(ls "$D"/patches/k27/0066-*.patch)
python3 - "$PATCH" "$S/piano_bootargs_policy.h" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ b/'):
        inside = line == '+++ b/pexpert/arm/piano_bootargs_policy.h'
        continue
    if inside:
        if line.startswith('diff --git') or line.startswith('-- '):
            break
        if line.startswith('+'):
            out.append(line[1:])
        elif line.startswith('@@') or line == '':
            continue
        else:
            raise SystemExit('test-piano-bootargs: unexpected line in the header hunk: ' + line[:60])
want = open(header).read()
got = '\n'.join(out) + '\n'
if got != want:
    raise SystemExit('test-piano-bootargs: %s does not add %s byte for byte' % (patch.split('/')[-1], header.split('/')[-1]))
print('test-piano-bootargs: the 0066 patch carries piano_bootargs_policy.h unchanged (%d lines)' % len(out))
EOF
python3 "$D/tooling/adt/boot_args.py" --self-test
