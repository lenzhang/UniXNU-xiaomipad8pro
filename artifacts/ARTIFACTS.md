# Artifacts of slice 1 (2026-11-10)

Built on the Mac by `tooling/build-slice1.sh` (SIGN=1) at ~19:56 SGT from the pins of `unixnu.lock`; files under `out/`
(ignored by git). Status of every image: **OFFLINE_CANDIDATE_NOT_DEVICE_TESTED**, `fastboot boot` only, never flash.
Verification logs: `logs/bootimg/verify-*.log` (all PASS, 0 failures).

Reproducibility: the RAM images are byte-identical when rebuilt from the same FD and ADT (checked 61 s apart). Two things
change per build by design or by tool: the ADT's `/chosen/random-seed` (256 fresh bytes, UniXNU's generator, kernel boot
entropy) and PcdDxe's CodeView GUID inside the FD (`logs/fd/reproducibility.txt`). So these hashes identify this build.

## Outputs

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | f74807d387e3b570039f4a5b4f5a8e5d22dda32fa89c7978e209b7d98b922f11 | pianoPkg FD, piano-uefi's bundled DXEs (other firmware build) |
| `fd/piano-uefi-B-own303.fd` | 3145728 | f88f5fcdbb99a96afad7ba43696e77b33463ab732737da5106f1ac239e190ead | pianoPkg FD, this tablet's own OS3.0.303 DXEs (UFSDxe unpatched) |
| `k27/kc-k27-native.bin` | 92782592 | e12a2ad3bc50027e0e6fb91de380e1b1d637a02a93d16260cdafaaf62cbc0af0 | k27 native-variant kernel collection (kernel 0054 native + the VM's kext set) |
| `adt/piano-k27.adt` | 14896 | f9b45c6560e05fac3c315f3cf6f219b39cad1a729cbd67c0b6d639657c18c695 | piano ADT (UniXNU generate, dts/piano.dts + configs/k27-adt.toml) |
| `ram/boot-piano-uefi-ram-A-bundled.img` | 11837440 | d326172ae388c191e40714c7e6ce3a4caade5544ce46fc132bb8d67b5fd60121 | M1 image, unsigned (as the Ubuntu RAM set) |
| `ram/boot-piano-uefi-ram-B-own303.img` | 11837440 | b8e01cfa00c525462a5dbf39baafe02793a345bacbf8ffb8ae32cba6e240989b | M1 image, unsigned |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | 77a79f9a57e23ed715b3ea82d50f9485356008f953dba05b21281b6791a33656 | M2 image, unsigned |
| `ram/boot-piano-macos-ram-B-own303.img` | 203726848 | b7d36b2b2224d62a252e2e7c0c8c02c17de1ed7b41b7f66f7be6fc90be479ec3 | M2 image, unsigned |
| `ram/boot-piano-uefi-ram-A-bundled-signed.img` | 100663296 | 7754b8cbfa593aafb6068c8c97827770e10d6fbf45072e6ce7913b6b06e728e6 | AVB footer, partition_size = boot (96 MiB) |
| `ram/boot-piano-uefi-ram-B-own303-signed.img` | 100663296 | 4d03f3d46d112b38a3e1159d26a93b1195063d0358421163875254714cea0c08 | AVB footer, partition_size = boot |
| `ram/boot-piano-macos-ram-A-bundled-signed.img` | 205824000 | f3efb4c6d93763f9fe29890514f640976ac68df1ebef42a50697847fab8763d2 | AVB footer, partition_size 205824000 (> boot: RAM only) |
| `ram/boot-piano-macos-ram-B-own303-signed.img` | 205824000 | 977e55ee62513ae23959af6f654d60167c0300902be10ef139cb6b86e6a360b7 | AVB footer, partition_size 205824000 |
| `keys/piano-boot-signing.avbpubkey` | | 6b38a68872db145dbee63091b487a669f913574c38a8be4a8883a0019aef78b5 | public key; AVB sha1 f5151930dc69791637b2a48505cb175f5f78a522 = the key of this tablet's boot_a |
| `keys/piano-boot-signing.pub.pem` | | 12b38cc79502bff4b2436cb13541f52b37184b2ec131aec666692dc50aebad3b | same, PEM (verify_image) |

Signing: SHA256_RSA4096, partition `boot`, rollback index 1769904000, the three `com.android.build.boot.*` properties of
`boot-profile.json` (OS3.0.303 fingerprint), exactly as `build-persistent-boot.py` signs the installed set; done on
the private build host, where the private key stays.

## Inputs

| File | SHA-256 | From |
|---|---|---|
| `inputs/piano-boot.dtb` (147436 B) | 50636effb5f54306ec0ab9a3061cd163d7f003d8797e04585a33205918856840 | v1.1.0 release `piano-boot.dtb` = the DTB of the Ubuntu RAM images (maintenance-manifest.json) |
| `inputs/ubuntu-boot-ram-maintenance.img` | 54001a50a13fe80c3bd8e926161608b71d91e57e2d6be6b83cdc2bc62b5f6fbc | v1.1.0 release, the reference v2 container |
| `inputs/kernel.macho.trustcache` (446804 B) | 2b87b2241090c90508f26c8ca33129f7540922481a102359e99cf0adf00d10d6 | the VM accept boot disk |
| `inputs/scratch-data.img` (83967024 B) | cb50ceac16394c9f1da7f141ce0bb0b831b5ffac6bc9eeb56a9af99b1c147c11 | the VM accept boot disk (Data seed; = tablet data-store.seed) |
| AOSP `mkbootimg.py` | 37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c | v1.1.0 host-tools (the Ubuntu RAM set's) |
| `avbtool.py` 1.3.0 | e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b | xiaomipad-8pro-mainline `tools/aosp-avb.json` pin |
| k27 kernel `kernel-k27-0054-native.macho` | 20a5b4fdcf54b6ac6e8f1878856b6049fc800591d6936c204545e15c53ebbfae | macOS-DroidVM el1/native-variant 11ac316, `K27_VARIANT=native` |
| k27 kernel `kernel-k27-0054-vm.macho` | a8de3ece88b0477aa01e4bdfbdd8132b3ab04a5e4754ee212a63b3fbb6b74cf6 | same tree, `K27_VARIANT=vm` (not deployed) |
| tablet `uefi_a` (OS3.0.303, BOOT.MXF.2.5.1-00265) | cee5757c034be1d5d656ace60d5f43fd242c19c511bbf52907655d2edf333a00 | 2026-11-10 backup `sde/1-uefi_a.img.zst`; source of FD B's DXEs (kept outside every repo) |
| piano hardware DTB `adt/piano-k27.hw.dtb` | 3e8ec34e612242bb93182713c77b8209890407b6497fc71042fac16e5ad5760d | dtc of dts/piano.dts + sm8750.dtsi |

## Item 4 test disk (VM only, on the tablet and the private build host, not in out/)

| File | SHA-256 |
|---|---|
| the private build host `~/droidvm/work/macos27/boot-native-test/boot.img` = tablet `~/vm/images/macos27/boot-native-test.img` | f27e87daf43eb1f7f855ccccd2213a005a0bd79da34e0c928f875678e65f0311 |
| its ADT (8 CPUs EEEEEEPP, psci hvc) | `logs/adt/vm-test-8cpu-adt.txt` |
| 8-vCPU crosvm FDT `crosvm-piano-gpu-net-snd-media-8192m-8cpu-1600x1068.dtb` | synthesized from the 6-vCPU dump; matched by run 1110-201218's own generated.dtb |
| accept disk backup `~/vm/images/macos27/boot.img.accept-20261110` | dcbbcc9b72d9b379303b76e4c78d6c979961f9e2210a4c03c676d6a04c2a61bd |

## Persistent boot (2026-11-11, docs/persistent-boot.md)

Built offline by `tooling/persist/build-all.sh` (`ESP_PROFILE=base`, `ESP_UTC=1794350126`) in an agent worktree; files under `out/`
(ignored by git). Status: **OFFLINE_BUILD_NOT_DEVICE_TESTED**; the `-signed` images do not exist yet (`tooling/persist/sign.sh`
makes them on the signing host). The FD build is not bit-reproducible: everything below is tied to this FD.

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | 6d15f0f830e5ea4d7d456d1ffd239a62c996c51c5e91649bedd29df334f26802 | FD A + `firmware/patches/unixnu/0001` (RAM payload first; patch sha256 1be90f66…) |
| `persistent/kernel-field.bin` | 3293356 | a1df7f145cf2d851fa2cdb3a8f8d3986bc9b4cf67d1660492e0f673c4da485ff | BootShim + that FD + boot DTB (the same field as the RAM images below) |
| `persistent/boot-piano-macos-persistent-v4.img` | 3301376 | 605c3a60254efb3620a1a4e8843d2effb4ffadcff831efd67631bec81201f6c5 | boot_a image, v4, unsigned |
| `persistent/boot-piano-esp-test-v2.img` | 3452928 | 04ed0d00483978e22bc67aed48558fc758148bc85c07a6d386d03c33fd5d11a6 | RAM test of the flashed path (v2, 4096-byte zero ramdisk); signed = the v2 fallback for boot_a |
| `persistent/esp-base-diag.img` | 1073741824 | a7a2afefc6af46feb519861a8e2a8fd5c726517dfc28cdf7937cd39d9121f760 | ESP, FAT32 4 KiB sectors, default `macos-diag` (reset to fastboot after 240 s) |
| `persistent/esp-base-macos.img` | 1073741824 | f5f4c6de3d61bed5b8337c2c964199c76e87445370c271a9a53ce99a9f301dd7 | ESP, default `macos` (daily use) |
| `persistent/gpt-head.bin` | 24576 | 8c3ab20f60226441096d4c61ea2671db605b4347163cd556e4080135eaab450e | LUN 0 LBA 0-5, new primary GPT (100 bytes differ from new-primary.bin: gpt-diff.txt) |
| `persistent/gpt-tail.bin` | 20480 | 39da467b3c6f99100583c5a0f42350a1d3c8e014be508f1fc53941414e7aa543 | LUN 0 LBA 124552187-124552191, new backup GPT |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | c7f40f62500e36180a43883a79554f0a8b58037d18f91f1254e36e4d70b5a11a | dev RAM image with that FD (K27_ARGS, reset 45 s): `ram-precedence` |
| `ram-dump/boot-piano-macos-ram-A-bundled.img` | 203726848 | 0b129dfa8eda049b5210a572ad09eeea119f47979852d7988c01f5c1d2474b33 | the loop's dump image with that FD (reset 1 s, data-persist=0) |

ESP inputs: KC 415bbea57f0f6699… (8 native kexts), ADT bafbdaac91f0118f… (vmm-present 1), trust cache 2b87b2241090c905…, seed
cb50ceac16394c9f… (seed identity 469ed4401f5b5241…, 3145728 pages, store window 3145825 blocks). ESP partition GUID
3F8B0988-D851-5539-AE47-6E53C621D288.

ESP profile `gpu` (built 07:56 from a copy of the main checkout's 07:44 build, the one that showed the login screen on the A830 at
07:45: KC 68fd4487e36bff02… with PianoA830, ADT f441d8b108478d24… vmm-present 0, Turnip-staged trust cache 97fb617d9c274035… and
seed df5579ddb4a1d9ec… (seed identity aced5bf9794b42f3…), `ESP_UTC=1794354270`):

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `persistent/esp-gpu-diag.img` | 1073741824 | 97fecadfb84cc91ff5d64c6c7d99e9906de907e3c593d03c7cb80734716a5b18 | default `macos-diag` (session 1, RAM tests) |
| `persistent/esp-gpu-macos.img` | 1073741824 | 1120f7d86ce688f9e122f863f59f71e50a5865e17227fa999824e2d331970615 | default `macos` (session 3, `esp-final`) |
| `ram-precedence/boot-piano-macos-ram-A-bundled.img` | 203726848 | 7f8c2d6194d8c8c11f005572d107de6dbb495122ff14638c4e8557458faa4846 | `ram-precedence`'s dev image: patched FD, base inputs, reset 45 s, data-persist=0 (cannot reset the installed store) |
