# KC builder patches for the k27 tree

The native kernel collection is built by the k27 tree's KC builder (`k27/kc-build27-piano.py` over `scripts/kc-build.py` and
`scripts/kc_native.py`), which this repository only reads (`tooling/kc/build-kc-k27.sh`). These two patches change how that builder
places the native kexts; they are applied by hand in the local k27 tree (`PIANO_K27_TREE`, `~/.config/piano/env`), never copied
here. Measurements and the reasoning: [docs/kc-space.md](../../docs/kc-space.md).

| Patch | File | What | Switch |
| --- | --- | --- | --- |
| 0001 | `scripts/kc-build.py` | native segments placed largest first, each into the smallest gap that holds it (a dry link gives the sizes first); the headroom left per segment kind in the log; a refusal names the kext | `KC_NATIVE_PLACEMENT=best-fit` (`tooling/kc/build-kc-k27.sh` sets it; unset: first fit as before) |
| 0002 | `scripts/kc_native.py` | a native kext's relocation-free `__DATA,__const` / `__DATA_CONST,__const` input sections (firmware images, number tables) go to its `__TEXT,__const` | `KC_NATIVE_CONST_TO_TEXT=1` (off unless set) |

Both are inert unless their switch is set: with neither set the builder's output is byte for byte what the unpatched builder writes
(checked: the same SHA-256 for main's collection). Neither changes what is linked, only where.

## Apply

```sh
. tooling/k27/k27-tree.sh                      # PIANO_K27_TREE from ~/.config/piano/env
git -C "$PIANO_K27_TREE" apply --check "$PWD"/patches/k27-kc/000[12]-*.patch && \
git -C "$PIANO_K27_TREE" apply "$PWD"/patches/k27-kc/000[12]-*.patch
grep -c KC_NATIVE_PLACEMENT "$PIANO_K27_TREE/scripts/kc-build.py"      # > 0: 0001 is in
```

0002 may be left out (`git apply .../0001-*.patch` alone). To take them out again: `git -C "$PIANO_K27_TREE" apply -R` with the same
files. Without 0001, `tooling/kc/build-kc-k27.sh` still works and says so ("note: the k27 tree's KC builder lacks
patches/k27-kc/0001"), and `PIANO_USBHOST_STACK=1 tooling/k27/build-native-image.sh` keeps leaving PianoTouch out (with 0001 it
links it).

## Check

```sh
tooling/k27/build-native-image.sh                        # the KC log ends the native link with "native placement (best-fit) ..."
python3 tooling/k27/kc-space.py out/k27/kc-k27-native.bin --simulate   # the gaps; the replay matches the build's addresses
```
