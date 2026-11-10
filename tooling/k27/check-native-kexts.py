#!/usr/bin/env python3
"""The native line's kext rule (docs/bringup-debugging.md "No DroidVM kexts natively", docs/PROJECT-MAP.md rule 8): no DroidVM kext
loads in a native piano boot. Every descriptor linked into the native kernel collection must be this repository's (resolved from
tooling/k27/kext/ into out/k27/kext/), its objects must be built from package/ into out/native-kexts/, and no bundle may be a
DroidVM one. Code taken over from the VM era is allowed only as this repository's own package (with UPSTREAM), never by linking the
archive's kext.

    check-native-kexts.py REPO DESCRIPTOR.json [...]          before the link: refuses the list
    check-native-kexts.py REPO --kc-log OUTPUT.log            after the link: refuses a collection with a DroidVM bundle in it
"""
import json
import re
import sys
from pathlib import Path

BAD = re.compile(r'droidvm', re.I)


def fail(why):
    sys.exit(f'native kext rule: {why} (docs/bringup-debugging.md, "No DroidVM kexts natively")')


repo = Path(sys.argv[1]).resolve()
args = sys.argv[2:]
if args[:1] == ['--kc-log']:
    text = Path(args[1]).read_text(errors='replace')
    linked = re.findall(r'^(\S+): linked', text, re.M)
    bad = [b for b in linked if BAD.search(b)]
    if bad:
        fail(f'the collection links {", ".join(bad)}')
    print(f'native kext rule: {len(linked)} native kexts linked, none from DroidVM')
    sys.exit(0)
for name in args:
    path = Path(name).resolve()
    if not path.is_relative_to(repo):
        fail(f'{path} is not a descriptor of this repository (tooling/k27/kext/)')
    spec = json.loads(path.read_text())
    bundle, objects = spec.get('bundle', ''), Path(spec.get('objects', ''))
    if BAD.search(bundle) or BAD.search(spec.get('name', '')) or any(BAD.search(p.get('IOClass', '')) for p in spec.get('personalities', {}).values()):
        fail(f'{path.name}: {bundle} is a DroidVM kext')
    if not objects.is_absolute() or not objects.resolve().is_relative_to(repo / 'out/native-kexts'):
        fail(f'{path.name}: its objects ({objects}) are not built from package/ into out/native-kexts')
print(f'native kext rule: {len(args)} descriptors, all this repository\'s')
