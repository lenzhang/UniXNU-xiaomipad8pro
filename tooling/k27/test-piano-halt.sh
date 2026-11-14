#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0069's decisions (tooling/k27/piano-halt/piano_halt_policy.h: PE_halt_restart's PSCI call per PEHaltRestart
# type, the restart word that stops the boot guard's restart-reason write once a restart has begun), with and without the
# sanitizers (thread sanitizer for the race), and the check that patches/k27/0069 adds that header byte for byte as
# osfmk/arm64/piano_halt_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/piano-halt
mkdir -p "$T"
S=$D/tooling/k27/piano-halt
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy" -lpthread
"$T/test_piano_halt_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy_san" -lpthread
"$T/test_piano_halt_policy_san"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=thread -I "$S" \
  "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy_tsan" -lpthread
"$T/test_piano_halt_policy_tsan"
PATCH=$(ls "$D"/patches/k27/0069-*.patch)
python3 - "$PATCH" "$S/piano_halt_policy.h" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ b/'):
        inside = line == '+++ b/osfmk/arm64/piano_halt_policy.h'
        continue
    if inside:
        if line.startswith('diff --git') or line.startswith('-- '):
            break
        if line.startswith('+'):
            out.append(line[1:])
        elif line.startswith('@@') or line == '':
            continue
        else:
            raise SystemExit('test-piano-halt: unexpected line in the header hunk: ' + line[:60])
want = open(header).read()
got = '\n'.join(out) + '\n'
if got != want:
    raise SystemExit('test-piano-halt: %s does not add %s byte for byte' % (patch.split('/')[-1], header.split('/')[-1]))
print('test-piano-halt: the 0069 patch carries piano_halt_policy.h unchanged (%d lines)' % len(out))
EOF
