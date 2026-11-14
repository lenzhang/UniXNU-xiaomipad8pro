#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0067's work split (tooling/k27/cs-native4k/cs_native4k_policy.h: chunks, claiming, the first error, the 4 KiB table
# equal to the one-thread pass with 1-16 threads), with and without the sanitizers, and the check that patches/k27/0067 adds that
# header byte for byte as bsd/kern/cs_native4k_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$D/out/test/cs-native4k
mkdir -p "$T"
S=$D/tooling/k27/cs-native4k
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy" -lpthread
"$T/test_cs_native4k_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy_san" -lpthread
"$T/test_cs_native4k_policy_san"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=thread -I "$S" \
  "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy_tsan" -lpthread
"$T/test_cs_native4k_policy_tsan"
PATCH=$(ls "$D"/patches/k27/0067-*.patch)
python3 - "$PATCH" "$S/cs_native4k_policy.h" <<'EOF'
import sys
patch, header = sys.argv[1], sys.argv[2]
lines, inside, out = open(patch).read().split('\n'), False, []
for line in lines:
    if line.startswith('+++ '):
        inside = line == '+++ b/bsd/kern/cs_native4k_policy.h'
        continue
    if inside:
        if line.startswith('diff --git') or line.startswith('-- '):
            break
        if line.startswith('+'):
            out.append(line[1:])
        elif line.startswith('@@') or line == '':
            continue
        else:
            raise SystemExit('test-cs-native4k: unexpected line in the header hunk: ' + line[:60])
want = open(header).read()
got = '\n'.join(out) + '\n'
if got != want:
    raise SystemExit('test-cs-native4k: %s does not add %s byte for byte' % (patch.split('/')[-1], header.split('/')[-1]))
print('test-cs-native4k: the 0067 patch carries cs_native4k_policy.h unchanged (%d lines)' % len(out))
EOF
