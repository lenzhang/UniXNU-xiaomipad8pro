# The AppleUSBXHCI ABI of one kernel collection (data only)

`abi-26C5035r-mac17j.json` is what `PianoXhciHost` (`package/PianoXhciHost`, path **A2** of `docs/drivers/usb-host.md` §5) needs in
order to be a direct subclass of the generic Apple xHCI driver of **one** kernel collection: the class's 427 virtual slots with the
PAC diversity and the symbol each slot must reach, the instance size `0xf88`, synthetic declarations for the private slots, and the
three slots the subclass implements. It holds **numbers and names only**: no Apple code, no Apple bytes, no Apple field layout.
Nothing generated from it is committed either — the declaration header `OriginalUsbXhciABI.h` is a build output under
`out/native-kexts/PianoXhciHost/`.

| Field | Value |
|---|---|
| Class / fileset | `AppleUSBXHCI` / `com.apple.driver.usb.AppleUSBXHCI` 1.2 |
| Kernel collection | macOS **27.3** build **26C5035r**, `kernelcache.release.mac17j`, our input `out/inputs/k273/kc/kc273-mac17j.bin` |
| Collection SHA-256 | `d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9` |
| Slots / instance size | 427 / `0xf88`; IOService prefix 272 slots; no pure virtual slot |
| Overridden slots | **292** desired power state (`XHCI_DESIRED_POWER`, diversity `0x4c77`), **419** 64-bit register read (`XHCI_READ64`, `0xe26f`), **421** 64-bit register write (`XHCI_WRITE64`, `0x6089`) |

## Provenance

The slot roles, the synthetic declaration names and the original bindings come from the reviewed fixture
`scripts/usb-xhci-26C5035r.json` of **UniXNU-sheng** (`unixnu-research/UniXNU-sheng`, `main`
`00ba90984e831e4c6ce30ec3d8a56e6434e9f1af`, SHA-256 `c6d7ad3855b213b794b11f5a6efc4922cc957cf6285e000ad69dac7ab661946b`,
BSD-2-Clause-Patent). That fixture pins the **same** collection SHA-256 as our 27.3 input, so it is not merely a fixture of a similar
27.3 build: it is the fixture of this exact collection, and reusing it needs no new binary analysis
(`private/host-checks/20261114-xhci/REPORT.md`, offline analysis of 2026-11-14, owner's approval of path A2 the same day).

Everything in this file was then **re-derived from our own input collection** and compared, by
`tooling/k27/xhci-abi.py check-kc out/inputs/k273/kc/kc273-mac17j.bin`: all 427 slot targets (as symbol names), all 427 PAC
diversities, the signing key (IA) and address diversification of every entry, and the instance size the class's `OSMetaClass`
constructor passes. The `slots` column with the symbol names is ours, from that read.

## How it is used, and what refuses a change

- `tooling/k27/xhci-abi.py header --out DIR` generates `OriginalUsbXhciABI.h` at build time (`build-native-piano-xhcihost.sh`).
- `xhci-abi.py check-kc KC` refuses a collection whose SHA-256, vtable or instance size is not the recorded one. The build runs it
  against `KC_BASE` **before** compiling, so a new macOS seed stops the A2 build instead of producing a collection that links and
  then traps (the collection builder's own vtable check compares inherited targets only: not diversities, not the slot count, not
  the instance size).
- `xhci-abi.py check-object OBJECT.o` refuses a compiled subclass whose vtable is not exactly the original's 427 slots, whose
  entries are not IA + address-diversified with the original diversities, or which implements any slot other than 292, 419 and 421.
- `xhci-abi.py self-test` (also run by the build) proves on synthetic objects that those gates refuse a changed diversity, an added
  virtual method, a stolen slot, a missing slot, another signing key, an unsigned pointer, a foreign binding, and a changed
  collection.

**Never carry the slot numbers to another collection.** A new seed or a new macOS build needs a fixture rebuilt for it; the gate
exists to make that failure loud.
