#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0063's decisions (tooling/k27/piano-guard/piano_guard_policy.h: the watchdog's holders, the boot guard's health
# decision and tick schedule, piano-boot-guard-test, the milestone's name, machdep.piano_boot_guard), with and without the
# sanitizers, and the check that patches/k27/0063 adds that header byte for byte as osfmk/arm64/piano_guard_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/piano-guard
mkdir -p "$T"
S=$D/tooling/k27/piano-guard
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_guard_policy.c" -o "$T/test_piano_guard_policy"
"$T/test_piano_guard_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_guard_policy.c" -o "$T/test_piano_guard_policy_san"
"$T/test_piano_guard_policy_san"
PATCH=$(ls "$D"/patches/k27/0063-*.patch)
python3 - "$PATCH" "$S/piano_guard_policy.h" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ b/'):
        inside = line == '+++ b/osfmk/arm64/piano_guard_policy.h'
        continue
    if inside:
        if line.startswith('diff --git') or line.startswith('-- '):
            break
        if line.startswith('+'):
            out.append(line[1:])
        elif line.startswith('@@') or line == '':
            continue
        else:
            raise SystemExit('test-piano-guard: unexpected line in the header hunk: ' + line[:60])
want = open(header).read()
got = '\n'.join(out) + '\n'
if got != want:
    raise SystemExit('test-piano-guard: %s does not add %s byte for byte' % (patch.split('/')[-1], header.split('/')[-1]))
print('test-piano-guard: the 0063 patch carries piano_guard_policy.h unchanged (%d lines)' % len(out))
EOF
