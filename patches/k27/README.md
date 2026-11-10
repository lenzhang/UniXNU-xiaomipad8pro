# k27 patches for native piano

k27 = xnu-12377.121.6 + the macOS-DroidVM series 0001-0052 (the macOS 27.0.1 VM kernel). These four patches on top make
the same kernel boot natively on piano at EL1 (all inert in the VM unless their boot argument is set):

| Patch | What |
| --- | --- |
| 0053 | One kernel for the VM and the HLOS: the PSCI conduit comes from the ADT's `/psci` method; `XNU_VM_GUEST=0` (K27_VARIANT=native) only changes the fallback to SMC |
| 0054 | SM8750 MPIDRs: Affinity2 in the reset handler's and `ml_get_cpu_number`'s CPU lookups (prime cores 0x10000/0x10100) |
| 0055 | `pstore-console=<phys>`: the console mirrored into a RAM mailbox that survives a reset (11 copies, majority on read; `tooling/native/xnu-log.py`) |
| 0056 | `piano-reset-after=<s>`: PSCI reset after N seconds, the Gunyah watchdog (32 s bite) as the backstop |

Build: `K27_VARIANT=native build-k27-piano.sh` in the macOS-DroidVM tree (see `docs/bringup-debugging.md` §4).
