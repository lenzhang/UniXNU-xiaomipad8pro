# k27 patches for native piano

k27 = xnu-12377.121.6 + the macOS-DroidVM series 0001-0052 (the macOS 27.0.1 VM kernel). These patches on top make
the same kernel boot natively on piano at EL1 (all inert in the VM unless their boot argument is set):

| Patch | What |
| --- | --- |
| 0053 | One kernel for the VM and the HLOS: the PSCI conduit comes from the ADT's `/psci` method; `XNU_VM_GUEST=0` (K27_VARIANT=native) only changes the fallback to SMC |
| 0054 | SM8750 MPIDRs: Affinity2 in the reset handler's and `ml_get_cpu_number`'s CPU lookups (prime cores 0x10000/0x10100) |
| 0055 | `pstore-console=<phys>`: the console mirrored into a RAM mailbox that survives a reset (11 copies, majority on read; `tooling/native/xnu-log.py`) |
| 0056 | `piano-reset-after=<s>`: PSCI reset after N seconds, the Gunyah watchdog (32 s bite) as the backstop |
| 0057 | `piano-reset-to=bootloader`: the PMIC SDAM restart reason (SPMI arbiter v7, SID 0 byte 0x48 = 2) set at boot, so every reset lands in ABL fastboot |
| 0058 | pstore-console header stored 11 times too ("DBGR": signature, then 11 {start, size} pairs; the reader takes their majority): one decayed bit of the single size word had cut the log's tail |
| 0059 | On `!BSD` (IOBSD matching not finished after 30 s), every busy IOService with its clients and their state goes to the console: who holds the root up |

Build: `K27_VARIANT=native build-k27-piano.sh` in the macOS-DroidVM tree (see `docs/bringup-debugging.md` §4).
