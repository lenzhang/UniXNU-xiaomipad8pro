# Packaging on a macOS runner (draft)

`package.yml` builds, on a GitHub `macos-26` runner, the artifacts the build section of
[`README.md`](../README.md) produces by hand: the firmware FD, the 27.3 kernel-collection inputs, and
— when the k27 tree is available — the RAM images. It exists so an image does not have to live only
on one machine, and so every artifact comes with the SHA-256 of its inputs.

**It is a draft.** Every stage is guarded and reports its own outcome; the stages past ② have not
been run. The three inputs below are the ones a public runner cannot reconstruct by itself.

## Stages

| # | Stage | Input | Artifact |
| --- | --- | --- | --- |
| ① | `scripts/fetch-pins.py --lock unixnu.lock` | the lockfile's `[pin."name"]` tables (public repositories, exact commits) | `unixnu-research/` |
| ② | `tooling/unixnu.sh build firmware` | ①, plus the EDK2 feed pins | the FD and its SHA-256 |
| ③ | IPSW extraction | macOS 27.3 IPSW (`vars.PIANO_IPSW_URL`) and its AEA keys | `out/inputs/k273/kc/kc273-mac17j.bin` |
| ④ | `tooling/k273/prepare-k27-tree.sh` | the k27 tree (`vars.PIANO_K27_TREE_URL`), unknown 1 | the relinked kernel |
| ⑤ | `tooling/k273/build-k273-image.sh` | ③, ④ and the root image (unknown 3) | `out/ram/*.img` |

Prove them in that order. ① and ② need nothing private and are the first thing worth reviewing.

## Unknown 1: the k27 tree is a local branch

`unixnu.lock` records it as such:

```toml
[pin."k27"]
repo = "local: the k27 tree (PIANO_K27_TREE, tooling/k27/k27-tree.sh), branch el1/native-variant"
xnu = "xnu-12377.121.6 (ac9718fb…) + patches/xnu-12377 0001-0054"
```

`prepare-k27-tree.sh` reads an existing tree (it clones it with `cp -c`, applies `patches/k273-kc`
and `patches/k273`, and relinks). A runner therefore needs the tree published as a tarball, a pinned
fork, or a CI cache seeded by hand; reconstructing it from `apple-oss-distributions/xnu` plus the
baseline series is the alternative, and the one most likely to drift.

## Unknown 2: the IPSW extraction invocation

`tooling/k273/ipsw-members.py` and `decompress-kernelcache.py` are the tools; the workflow's call is
a sketch (blacktop's `ipsw` for the download and the AEA keys). Check it against
[`k273-line.md`](k273-line.md) before trusting its output. The IPSW is about 15 GB: cache it, or the
run costs more than the build.

## Unknown 3: the root image

`build-k273-image.sh` with `K273_ROOT=k273` wants `out/inputs/k273/root/` — `root.json`,
`scratch-data.img` and `kernel.macho.trustcache`, with a 20.9 GB root image. It is not in the
repository and should not be re-downloaded per run; an Actions cache or a private artifact is the
only sane carrier.

## Repository settings

| Kind | Name | Use |
| --- | --- | --- |
| variable | `PIANO_IPSW_URL` | the macOS 27.3 IPSW; unset skips ③ |
| variable | `PIANO_K27_TREE_URL` | the k27 tree tarball; unset skips ④ and ⑤ |
| secret | `BOOT_SIGNING_KEY` | `private/boot-signing.pem` for the FD/payload signature |
| secret | `UNIXNU_UNIQUE_CHIP_ID` | the platform seed, if the packaging step needs one (`UniXNU-sheng`'s workflow passes it) |

Nothing that the repository rules forbid enters the tree: vendor firmware, IPSW-derived payloads,
keys and per-unit calibration stay in the run's artifacts or in Actions secrets.
