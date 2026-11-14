#!/usr/bin/env python3
"""Apple's USB host stack in a native collection must be inert without piano-usbhost=6 (docs/drivers/usb-host.md section 5.2, H6).

    check-usbhost-stack.py KC.bin

Reads the linked collection's __PRELINK_INFO (kext names, libraries and personalities only). The stack's kexts are those only its
own bundles pull in (the closure of the stack bundle's and PianoXhciHost's libraries, minus every other bundle's). Refused:
  - no automatic client for the nub at all, or **more than one** (path A1's codeless AppleUSBXHCI personality and path A2's
    PianoXhciHost personality must never both be in one collection: two clients would compete for one nub);
  - a stack personality on IOResources that is not behind the gate resource PianoUsbHostStack (PianoUsbHost publishes it at level 6
    only), unless it waits for a resource the stack itself publishes;
  - a stack personality on a provider that exists at boot (IOPlatformExpertDevice, IOPlatformDevice, IOService, AppleARMIODevice,
    IODTNub) without a name match (name-matched ones are listed: the ADT must not have such a node).
Exit 0 when the stack is gated, 1 otherwise. With none of the stack bundles in the collection: nothing to check (exit 0).

Path A2 (PianoXhciHost, docs/drivers/usb-host.md §5.4) differs in one way that is printed, not refused: our own subclass names
AppleUSBXHCI and IOUSBHostFamily as libraries, so those two kexts (and their own dependencies) are **loaded at boot** instead of
on demand. They still match nothing: the generic AppleUSBXHCI has no personality of its own, the family's boot personalities stay
behind the gate resource, and the subclass's only personality waits for PianoUsbHostNub, which only piano-usbhost=6 creates.
"""
import plistlib
import sys
from pathlib import Path

from k27_tree import k27_tree  # noqa: E402  (tooling/k27/k27_tree.py: PIANO_K27_TREE)
sys.path.insert(0, str(k27_tree() / 'scripts'))
import fileset_kc  # noqa: E402

GATE = 'PianoUsbHostStack'
# The bundles that carry the stack: path A1's codeless one, path A2's codeless one and A2's own subclass.
BUNDLES = ('com.unixnu.driver.PianoUsbHostStack', 'com.unixnu.driver.PianoUsbHostStackA2', 'com.unixnu.driver.PianoXhciHost')
A2_BUNDLE = 'com.unixnu.driver.PianoXhciHost'
NUB = 'PianoUsbHostNub'
# Resources the stack's own kexts publish once loaded (so a personality waiting for one cannot match before the stack loads).
STACK_RESOURCES = {'com.apple.iokit.SCSISubsystemGlobals'}
BOOT_PROVIDERS = {'IOPlatformExpertDevice', 'IOPlatformDevice', 'IOService', 'AppleARMIODevice', 'IODTNub'}


def main():
    kc = fileset_kc.Kernelcache(Path(sys.argv[1]).read_bytes())
    prelink = next(s for s in kc.segments if s['name'] == '__PRELINK_INFO')
    section = prelink['sections'][0]
    info = plistlib.loads(bytes(kc.data[section['fileoff']:section['fileoff'] + section['size']]).split(b'\0')[0])['_PrelinkInfoDictionary']
    by_id = {e['CFBundleIdentifier']: e for e in info}
    ours = [b for b in BUNDLES if b in by_id]
    if not ours:
        print('usbhost stack: not in this collection (nothing to check)')
        return 0

    def closure(roots):
        seen, todo = set(), list(roots)
        while todo:
            name = todo.pop()
            if name in seen or name not in by_id:
                continue
            seen.add(name)
            todo.extend(by_id[name].get('OSBundleLibraries', {}))
        return seen

    stack = closure([lib for b in ours for lib in by_id[b].get('OSBundleLibraries', {})])
    others = closure([lib for e in info if e['CFBundleIdentifier'] not in ours and e['CFBundleIdentifier'] not in stack
                      for lib in e.get('OSBundleLibraries', {})] +
                     [e['CFBundleIdentifier'] for e in info if e['CFBundleIdentifier'] not in ours and e['CFBundleIdentifier'] not in stack])
    only = sorted(stack - others)
    problems, named = [], []
    # Exactly one automatic client for the nub: A1's codeless AppleUSBXHCI personality, or A2's own PianoXhciHost one.
    clients = [f'{bundle} "{name}" ({p.get("IOClass")})' for bundle in by_id
               for name, p in (by_id[bundle].get('IOKitPersonalities') or {}).items() if p.get('IOProviderClass') == NUB]
    if not clients:
        problems.append(f'no personality on {NUB}: nothing would attach to the nub (A1\'s codeless AppleUSBXHCI or A2\'s PianoXhciHost)')
    elif len(clients) > 1:
        problems.append(f'{len(clients)} personalities on {NUB} ({"; ".join(clients)}): two automatic clients would compete for one nub')
    else:
        print(f'usbhost stack: the nub\'s only client is {clients[0]}')
    if A2_BUNDLE in by_id:
        print(f'usbhost stack: path A2: {A2_BUNDLE} links AppleUSBXHCI and IOUSBHostFamily, so those load at boot; they match '
              f'nothing without the gate resource {GATE} and the nub')
    gated = 0
    for name in only:
        for pname, p in (by_id[name].get('IOKitPersonalities') or {}).items():
            provider, resource = p.get('IOProviderClass'), p.get('IOResourceMatch')
            if resource == GATE:
                gated += 1
                continue
            if provider == 'IOResources':
                if resource in STACK_RESOURCES:
                    continue
                problems.append(f'{name} "{pname}": IOResources personality not behind {GATE} (IOResourceMatch {resource!r})')
            elif provider in BOOT_PROVIDERS:
                if p.get('IONameMatch') or p.get('IOPropertyMatch') or p.get('IOParentMatch'):
                    named.append(f'{name} "{pname}": {provider} {p.get("IONameMatch") or p.get("IOPropertyMatch") or p.get("IOParentMatch")}')
                else:
                    problems.append(f'{name} "{pname}": matches any {provider}')
    print(f'usbhost stack: {len(only)} kexts only the stack pulls in, {gated} personalities behind {GATE}')
    for line in named:
        print(f'  name-matched (the ADT must not have such a node): {line}')
    for line in problems:
        print(f'  NOT INERT: {line}')
    return 1 if problems else 0


sys.exit(main())
