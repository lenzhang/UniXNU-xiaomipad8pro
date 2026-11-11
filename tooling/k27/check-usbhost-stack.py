#!/usr/bin/env python3
"""Apple's USB host stack in a native collection must be inert without piano-usbhost=6 (docs/drivers/usb-host.md section 5.2, H6).

    check-usbhost-stack.py KC.bin

Reads the linked collection's __PRELINK_INFO (kext names, libraries and personalities only). The stack's kexts are those only the
codeless bundle com.unixnu.driver.PianoUsbHostStack pulls in (its libraries' closure minus every other bundle's). Refused:
  - the codeless personality (AppleUSBXHCI on PianoUsbHostNub) missing;
  - a stack personality on IOResources that is not behind the gate resource PianoUsbHostStack (PianoUsbHost publishes it at level 6
    only), unless it waits for a resource the stack itself publishes;
  - a stack personality on a provider that exists at boot (IOPlatformExpertDevice, IOPlatformDevice, IOService, AppleARMIODevice,
    IODTNub) without a name match (name-matched ones are listed: the ADT must not have such a node).
Exit 0 when the stack is gated, 1 otherwise. With no PianoUsbHostStack bundle in the collection: nothing to check (exit 0).
"""
import plistlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(os.environ.get('K27_TREE', Path.home() / 'Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM')) / 'scripts'))
import fileset_kc  # noqa: E402

GATE = 'PianoUsbHostStack'
BUNDLE = 'com.unixnu.driver.PianoUsbHostStack'
# Resources the stack's own kexts publish once loaded (so a personality waiting for one cannot match before the stack loads).
STACK_RESOURCES = {'com.apple.iokit.SCSISubsystemGlobals'}
BOOT_PROVIDERS = {'IOPlatformExpertDevice', 'IOPlatformDevice', 'IOService', 'AppleARMIODevice', 'IODTNub'}


def main():
    kc = fileset_kc.Kernelcache(Path(sys.argv[1]).read_bytes())
    prelink = next(s for s in kc.segments if s['name'] == '__PRELINK_INFO')
    section = prelink['sections'][0]
    info = plistlib.loads(bytes(kc.data[section['fileoff']:section['fileoff'] + section['size']]).split(b'\0')[0])['_PrelinkInfoDictionary']
    by_id = {e['CFBundleIdentifier']: e for e in info}
    if BUNDLE not in by_id:
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

    stack = closure(by_id[BUNDLE].get('OSBundleLibraries', {}))
    others = closure([lib for e in info if e['CFBundleIdentifier'] != BUNDLE and e['CFBundleIdentifier'] not in stack
                      for lib in e.get('OSBundleLibraries', {})] +
                     [e['CFBundleIdentifier'] for e in info if e['CFBundleIdentifier'] != BUNDLE and e['CFBundleIdentifier'] not in stack])
    only = sorted(stack - others)
    problems, named = [], []
    codeless = by_id[BUNDLE].get('IOKitPersonalities', {})
    if not any(p.get('IOClass') == 'AppleUSBXHCI' and p.get('IOProviderClass') == 'PianoUsbHostNub' for p in codeless.values()):
        problems.append(f'{BUNDLE}: no AppleUSBXHCI personality on PianoUsbHostNub')
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
