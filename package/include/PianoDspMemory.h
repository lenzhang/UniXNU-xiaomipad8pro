/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Low DRAM the audio DSP can address, split once, here (docs/drivers/sensors.md §5, "DSP-addressable memory"). The DSP takes 32-bit
 * addresses (the context bank in bits 32 and up), and every page XNU owns lies above 4 GiB (0x8c0000000 and up), so the buffers the
 * DSP reads and writes through a bypassed SMMU stream must be physical memory below 4 GiB that nobody else uses. Facts:
 *   - cdsp@9d200000, 25 MiB, no-map, in the live DT's reserved-memory (docs/hyp-evidence/memory-map-running-dt.txt);
 *   - its only user is the compute DSP's loader, remoteproc@32300000 (qcom,sm8750-cdsp-pas), which is status = "disabled" on piano:
 *     Ubuntu never started the CDSP, natively nothing does, and nothing in this repository maps the range;
 *   - piano's UEFI keeps it inside PIL_Reserved (0x89e00000 + 0x19700000, pianoPkg MemoryMapLib.c), so neither UEFI nor DarwinBoot
 *     allocates there; its neighbour cvp@9cb00000 (the same kind of carveout) is the native console mailbox, written from XNU.
 * Whether the DSP's accesses through the bypassed stream reach it is shown by the first attach (sensors.md §8, S2).
 * One owner: this header. A driver uses only its own slice and changes the split only here. */
#ifndef PIANO_DSP_MEMORY_H
#define PIANO_DSP_MEMORY_H

#define PIANO_DSP_MEMORY_BASE 0x9d200000ull      /* the CDSP carveout */
#define PIANO_DSP_MEMORY_BYTES 0x1900000ull      /* 25 MiB */

#define PIANO_DSP_MEMORY_SENSORS 0x9d200000ull   /* PianoSensors: the FastRPC arena (SSR_ARENA_BYTES), the piano-sensors-rpc-pa default */
#define PIANO_DSP_MEMORY_SENSORS_BYTES 0x800000ull
#define PIANO_DSP_MEMORY_AUDIO 0x9da00000ull     /* PianoAudio: its DMA buffers when it needs them (audio.md §10.4), not used yet */
#define PIANO_DSP_MEMORY_AUDIO_BYTES 0x1000000ull
#define PIANO_DSP_MEMORY_SPARE 0x9ea00000ull     /* unassigned */
#define PIANO_DSP_MEMORY_SPARE_BYTES 0x100000ull

#endif
