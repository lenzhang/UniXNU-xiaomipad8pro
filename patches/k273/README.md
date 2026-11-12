# patches/k273: the 27.3 line's kernel patches

Applied by `tooling/k273/prepare-k27-tree.sh` to the xnu source of the 27.3 line's k27 tree clone (`out/k273/k27-tree/out/k27/xnu`),
on top of the k27 tree's series (xnu-12377 + 0001-0052 + `patches/k27` 0053-...). The configured k27 tree is never changed.

| Patch | What | Source |
|---|---|---|
| 0001-darwin26-syscalls.patch | The Darwin 26 system calls the k27 tree leaves as `nosys` (SIGSYS): pipe2 (148), dup3 (149), fchflagsat (214), getumask (215), aio_readv/aio_writev (246/247, ENOTSUP), fsctlat (249), guarded_ftruncate_np (352) | UniXNU `patches/xnu-12377/darwin26-syscalls.patch` at UniXNU 6ae5f831 (2026-11-09), unchanged; BSD-2-Clause-Patent (UniXNU contributors). It is in the series of UniXNU's macos-27.3-26C5035r profile, which runs this userland on xnu-12377. |

UniXNU's other Darwin 26 / macOS patches of that profile (darwin26-kpi, macos-darwin26-kpi, ...) do not apply to the k27 tree, and
the 27.3 collection's 48 kept kexts reference no kernel symbol the k27 kernel lacks (docs/k273-line.md §1), so none is carried.
