#!/usr/bin/env python3
"""The native kext rule (docs/bringup-debugging.md §5, docs/PROJECT-MAP.md rule 8): a native piano boot loads only this
repository's kexts. Every descriptor linked into the native kernel collection must be this repository's (resolved from
tooling/k27/kext/ into out/k27/kext/), its objects must be built from package/ into out/native-kexts/, and no bundle, kext name or
IOClass may be one of the VM-era paravirtual kexts (VM_ERA_* below: display, GPU, audio, Ethernet, input, video, virtio block),
which drove the hypervisor's virtual devices that piano does not have. Code taken over from the VM era is allowed only as this
repository's own package (with UPSTREAM), never by linking the VM-era kext. A descriptor without objects is a codeless bundle: only
personalities for classes of its com.apple.* libraries.

    check-native-kexts.py REPO DESCRIPTOR.json [...]          before the link: refuses the list
    check-native-kexts.py REPO --kc-log OUTPUT.log            after the link: refuses a collection that links a VM-era kext
    check-native-kexts.py --self-test                         the rule on synthetic descriptors: a VM-era bundle, name, IOClass,
                                                              a descriptor from elsewhere, foreign objects, a VM-era KC log must
                                                              be refused, this repository's kind of descriptor accepted
"""
import json
import re
import sys
import tempfile
from pathlib import Path

# The denylist (data). The VM-era kexts' bundle identifiers, and the patterns every one of them carries: their legacy name prefix
# (the earlier VM-based port's "DroidVM") and the virtio transport they sat on. Refused wherever they appear: bundle, kext name,
# IOClass, a codeless personality's owner, a bundle the collection links.
VM_ERA_BUNDLES = frozenset({
    'com.unixnu.driver.DroidVMAudio', 'com.unixnu.driver.DroidVMDisplay', 'com.unixnu.driver.DroidVMEthernet',
    'com.unixnu.driver.DroidVMFramebuffer', 'com.unixnu.driver.DroidVMGpu', 'com.unixnu.driver.DroidVMInput',
    'com.unixnu.driver.DroidVMVideo', 'com.unixnu.driver.NativeVirtioBlock',
})
VM_ERA_NAME = re.compile(r'droidvm|virtio', re.I)


class Refused(Exception):
    pass


def vm_era(value):
    return value in VM_ERA_BUNDLES or bool(VM_ERA_NAME.search(value))


def check_kc_log(text):
    """The bundles the collection links (the KC builder's '<bundle>: linked' lines); refuses a VM-era one."""
    linked = re.findall(r'^(\S+): linked', text, re.M)
    bad = [b for b in linked if vm_era(b)]
    if bad:
        raise Refused(f'the collection links {", ".join(bad)}')
    return linked


def check_descriptors(repo, names):
    for name in names:
        path = Path(name).resolve()
        if not path.is_relative_to(repo):
            raise Refused(f'{path} is not a descriptor of this repository (tooling/k27/kext/)')
        spec = json.loads(path.read_text())
        bundle, objects = spec.get('bundle', ''), Path(spec.get('objects', ''))
        if vm_era(bundle) or vm_era(spec.get('name', '')) or any(vm_era(p.get('IOClass', '')) for p in spec.get('personalities', {}).values()):
            raise Refused(f'{path.name}: {bundle} is a VM-era kext')
        if 'objects' not in spec:
            # A codeless bundle (kc-build.py: personalities for classes of the collection's own kexts, e.g. PianoUsbHostStack.json): no
            # code of its own, so nothing to build; its personalities must name a collection kext (com.apple.*), never a VM-era one or ours.
            for name_, personality in spec.get('personalities', {}).items():
                owner = personality.get('CFBundleIdentifier', '')
                if not owner.startswith('com.apple.') or vm_era(owner) or owner not in spec.get('libraries', {}):
                    raise Refused(f'{path.name}: codeless personality {name_!r} names {owner!r}, not one of its com.apple.* libraries')
            continue
        if not objects.is_absolute() or not objects.resolve().is_relative_to(repo / 'out/native-kexts'):
            raise Refused(f'{path.name}: its objects ({objects}) are not built from package/ into out/native-kexts')
    return len(names)


def self_test():
    """The rule against synthetic descriptors in a scratch repository: what must pass passes, what must be refused is refused."""
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp, 'repo').resolve()
        (repo / 'out/k27/kext').mkdir(parents=True)
        (repo / 'out/native-kexts/PianoGood').mkdir(parents=True)
        outside = Path(tmp, 'elsewhere').resolve()
        outside.mkdir()

        def descriptor(file, where=repo / 'out/k27/kext', **spec):
            path = where / file
            path.write_text(json.dumps(spec))
            return str(path)

        good = dict(name='PianoGood', bundle='com.unixnu.driver.PianoGood', objects=str(repo / 'out/native-kexts/PianoGood'),
                    personalities={'PianoGood': {'IOClass': 'PianoGood'}})
        codeless = dict(name='PianoStack', bundle='com.unixnu.driver.PianoStack', libraries={'com.apple.iokit.IOUSBHostFamily': '1.2'},
                        personalities={'xhci': {'CFBundleIdentifier': 'com.apple.iokit.IOUSBHostFamily', 'IOClass': 'AppleUSBXHCI'}})
        accepted = [descriptor('PianoGood.json', **good), descriptor('PianoStack.json', **codeless)]
        refused = {'a VM-era bundle': descriptor('A.json', **dict(good, bundle='com.unixnu.driver.DroidVMDisplay')),
                   'a VM-era bundle under a native name': descriptor('B.json', **dict(good, name='PianoBlock',
                                                                                      bundle='com.unixnu.driver.NativeVirtioBlock')),
                   'a VM-era kext name': descriptor('C.json', **dict(good, name='DroidVMGpu')),
                   'a VM-era IOClass': descriptor('D.json', **dict(good, personalities={'hid': {'IOClass': 'DroidVMHIDDevice'}})),
                   'a virtio IOClass': descriptor('E.json', **dict(good, personalities={'snd': {'IOClass': 'PianoVirtioSnd'}})),
                   'a descriptor from elsewhere': descriptor('PianoGood.json', where=outside, **good),
                   'objects from elsewhere': descriptor('F.json', **dict(good, objects=str(outside))),
                   'a codeless personality on a VM-era bundle': descriptor('G.json', **dict(codeless, libraries={
                       'com.unixnu.driver.DroidVMInput': '1'}, personalities={'p': {'CFBundleIdentifier': 'com.unixnu.driver.DroidVMInput'}}))}
        problems = []
        try:
            check_descriptors(repo, accepted)
        except Refused as e:
            problems.append(f'refused this repository\'s descriptors: {e}')
        for what, name in refused.items():
            try:
                check_descriptors(repo, accepted + [name])
                problems.append(f'accepted {what}')
            except Refused:
                pass
        for bundle in sorted(VM_ERA_BUNDLES):
            try:
                check_kc_log(f'com.unixnu.driver.PianoGood: linked, 1 pointers\n{bundle}: linked, header at 0x0, 1 pointers\n')
                problems.append(f'accepted a collection linking {bundle}')
            except Refused:
                pass
        try:
            if len(check_kc_log('com.unixnu.driver.PianoGood: linked, header at 0x0, 1 pointers\n')) != 1:
                problems.append('miscounted a native collection')
        except Refused as e:
            problems.append(f'refused a native collection: {e}')
    if problems:
        sys.exit('native kext rule self-test FAILED: ' + '; '.join(problems))
    print(f'native kext rule self-test: {len(refused)} VM-era or foreign descriptors and {len(VM_ERA_BUNDLES)} VM-era collections refused, '
          f'{len(accepted)} native descriptors accepted')


def main():
    if sys.argv[1:] == ['--self-test']:
        self_test()
        return
    repo = Path(sys.argv[1]).resolve()
    args = sys.argv[2:]
    try:
        if args[:1] == ['--kc-log']:
            linked = check_kc_log(Path(args[1]).read_text(errors='replace'))
            print(f'native kext rule: {len(linked)} native kexts linked, none from outside this repository')
            return
        count = check_descriptors(repo, args)
    except Refused as e:
        sys.exit(f'native kext rule: {e} (docs/bringup-debugging.md §5)')
    print(f'native kext rule: {count} descriptors, all this repository\'s')


if __name__ == '__main__':
    main()
