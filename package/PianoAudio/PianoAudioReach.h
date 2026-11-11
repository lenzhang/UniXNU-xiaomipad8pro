/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio DSP's graph manager (APM) and resource manager (PRM) commands for piano's speaker and microphone paths, as GPR packets.
 * UniXNU-sheng package/ShengAudioReach.{h,c} (UPSTREAM: the packet writer, the graph-open layout, the graph commands, the memory map,
 * the buffer commands, the PRM requests and the answer parser, hardware-verified on SM8550) with piano's graphs instead of sheng's:
 *   speaker device subgraph 0x4005:  [LOG 0x6050] -> [MFC 0x6051] -> [TDM sink 0x6052] (LPAIF secondary TDM, 4 x 32-bit slots)
 *   playback stream subgraph 0x4001: [WR_SH 0x6001] -> [PCM DEC 0x6002] -> [PCM CNV 0x6003] -> [LOG 0x6004] -> device LOG 0x6050
 *   mic device subgraph 0x4008:      [codec DMA source 0x6080 (VA TX0)] -> [LOG 0x6081]
 *   capture stream subgraph 0x4003:  device LOG 0x6081 -> [LOG 0x6020] -> [MFC 0x6021] -> [PCM CNV 0x6022] -> [PCM ENC 0x6023] -> [RD_SH 0x6024]
 * (xiaomipad-8pro-mainline device/audio/Xiaomi-Pad-8-Pro.m4 and linux-msm audioreach-topology, BSD-3-Clause: the graphs Ubuntu plays
 * and records with on piano). Module, parameter and payload layouts are the AudioReach API's (as Linux's q6apm/audioreach implement it:
 * GPL-2.0, read as a specification only). Each packet stays within one GLINK intent (GPR_MAX_PACKET). Pure encoding: no I/O, no waits. */
#ifndef PIANO_AUDIOREACH_H
#define PIANO_AUDIOREACH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AR_APM_IID 1u
#define AR_OP_GRAPH_OPEN 0x01001000u
#define AR_OP_GRAPH_PREPARE 0x01001001u
#define AR_OP_GRAPH_START 0x01001002u
#define AR_OP_GRAPH_STOP 0x01001003u
#define AR_OP_GRAPH_CLOSE 0x01001004u
#define AR_OP_SET_CFG 0x01001006u
#define AR_OP_MEM_MAP 0x0100100Cu
#define AR_OP_MEM_UNMAP 0x0100100Du
#define AR_OP_MEM_MAP_DONE 0x02001001u          /* payload {handle} */
#define AR_OP_WRITE 0x0400100Au                 /* DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 */
#define AR_OP_WRITE_DONE 0x05001004u
#define AR_OP_READ 0x0400100Bu                  /* DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2 */
#define AR_OP_READ_DONE 0x05001005u
#define AR_OP_EOS_RENDERED 0x05001001u

/* GPR ports of the application side. Ubuntu on piano (Linux gpr/q6apm) sends APM commands from port 1 and PRM requests from port 2
 * (the DT's service numbers) and data commands from a dynamic port (the first is 0x10000000); sheng used one port, 0x2001, for all. */
#define AR_PORT_APM 1u
#define AR_PORT_PRM 2u
#define AR_PORT_DATA 0x10000000u

/* Module ids. */
#define AR_MOD_WR_SHMEM 0x07001000u
#define AR_MOD_RD_SHMEM 0x07001001u
#define AR_MOD_PCM_CNV 0x07001003u
#define AR_MOD_PCM_ENC 0x07001004u
#define AR_MOD_PCM_DEC 0x07001005u
#define AR_MOD_TDM_SINK 0x0700100Eu
#define AR_MOD_MFC 0x07001015u
#define AR_MOD_LOG 0x0700101Au
#define AR_MOD_CODEC_DMA_SOURCE 0x07001024u

/* piano's subgraphs and the instance ids the topology gives their modules. */
#define AR_SG_PLAY 0x4001u
#define AR_SG_CAPTURE 0x4003u
#define AR_SG_SPEAKER 0x4005u
#define AR_SG_MIC 0x4008u
#define AR_IID_PLAY_SHMEM 0x6001u
#define AR_IID_PLAY_DEC 0x6002u
#define AR_IID_PLAY_CNV 0x6003u
#define AR_IID_PLAY_LOG 0x6004u
#define AR_IID_SPK_LOG 0x6050u
#define AR_IID_SPK_MFC 0x6051u
#define AR_IID_SPK_TDM 0x6052u
#define AR_IID_CAP_LOG 0x6020u
#define AR_IID_CAP_MFC 0x6021u
#define AR_IID_CAP_CNV 0x6022u
#define AR_IID_CAP_ENC 0x6023u
#define AR_IID_CAP_SHMEM 0x6024u
#define AR_IID_MIC_DMA 0x6080u
#define AR_IID_MIC_LOG 0x6081u
#define AR_LOG_CODE_DEVICE 0x19abu   /* the topology's logger codes (tap point 1, mode 0) */
#define AR_LOG_CODE_STREAM 0x19afu

/* PCM channel types (the AudioReach API's): the four TDM slots carry FL, FR, LS, RS = top left, top right, bottom left, bottom right
 * (landscape, front camera on top), the order the MFC in front of the TDM sink routes by. */
#define AR_CH_FL 1u
#define AR_CH_FR 2u
#define AR_CH_LS 4u
#define AR_CH_RS 5u

typedef struct {
    uint32_t rate;           /* Hz */
    uint16_t bits;           /* 16 (streams) or 32 (the TDM slots) */
    uint16_t channels;       /* 1, 2 or 4 */
} ArFormat;

/* The TDM sink's interface (PARAM_ID_TDM_INTF_CFG): which LPAIF interface and the frame. */
typedef struct {
    uint32_t lpaif_type;     /* 0 LPAIF */
    uint32_t intf_idx;       /* 1 secondary */
    uint16_t sync_src;       /* 1 internal (the SoC drives bit clock and frame sync) */
    uint16_t data_out_enable;
    uint32_t slot_mask;      /* 0xf */
    uint16_t slots, slot_width;
    uint16_t sync_mode;      /* 1 long sync (the stock ACDB has entries with 1 and 0: the first value to revisit on clock errors) */
    uint16_t invert_sync, data_delay;
} ArTdm;
/* piano's frame: 48 kHz, 4 x 32-bit slots, mask 0xf, internal long sync, data delay 1, data out enabled (Ubuntu's topology from the
 * stock ACDB). The bit clock is 48000 * 4 * 32 = 6.144 MHz. */
void ar_piano_tdm(ArTdm *t);
#define AR_TDM_BIT_CLOCK 6144000u

/* The default channel map of n channels (1: FL; 2: FL FR; 4: FL FR LS RS); returns 0 for any other count. */
unsigned ar_channel_map(unsigned channels, uint8_t map[4]);

/* Each builder writes one complete GPR packet into out (capacity bytes) and returns its size, or 0 when it does not fit.
 * port: the GPR port the answers come back to; token: echoed in the answer. */
uint32_t ar_open_speaker_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
uint32_t ar_config_speaker_logger(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* f: the slots' format (48000, 32, 4); the MFC's output channel map is the default map of f->channels. */
uint32_t ar_config_speaker_mfc(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f);
/* The TDM interface, the hardware endpoint's media format (f) and its frame size factor, in one packet. */
uint32_t ar_config_speaker_tdm(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArTdm *t, const ArFormat *f);
uint32_t ar_open_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* The decoder's (interleaved) and the converter's (deinterleaved unpacked) output formats. */
uint32_t ar_config_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f);
uint32_t ar_config_playback_logger(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
uint32_t ar_config_playback_shmem(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f);
uint32_t ar_open_mic_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* channel_mask: VA TX0's codec DMA channels in use (bit 0 first channel). */
uint32_t ar_config_mic_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f, uint32_t channel_mask);
uint32_t ar_config_mic_logger(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
uint32_t ar_open_capture_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* The MFC's, converter's and encoder's output formats (all interleaved). */
uint32_t ar_config_capture_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f);
uint32_t ar_config_capture_logger(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token);
/* opcode: AR_OP_GRAPH_PREPARE, _START, _STOP or _CLOSE for the one subgraph sg. */
uint32_t ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t opcode, uint32_t sg);
/* Maps one physically contiguous region (rounded up to 4 KiB) for the DSP; the answer is AR_OP_MEM_MAP_DONE with the handle.
 * address: a DSP address (ar_dsp_address). */
uint32_t ar_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t bytes);
uint32_t ar_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t handle);
/* One buffer for the playback shared memory endpoint; the answer is AR_OP_WRITE_DONE with the same token. */
uint32_t ar_write(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes);
/* One empty buffer for the capture shared memory endpoint; the answer is AR_OP_READ_DONE. */
uint32_t ar_read(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes);

/* The DSP takes a buffer as its 32-bit physical address with the SMMU stream's index in bits 32 and up (Linux q6apm-dai: the low four
 * bits of the stream id, 1 for 0x1001). Returns 0 and the address, or -1 when phys does not fit in 32 bits (piano's XNU memory is all
 * above 4 GiB: such a buffer is not DSP-addressable without a translating bank). */
int ar_dsp_address(uint64_t phys, uint32_t stream_id, uint64_t *out);

/* The DSP's power and resource manager (PRM, module 2): an LPASS clock at hz (0 releases it; attribute 1 "couple no", root 0), and a
 * hardware block vote (1 LPASS core macros, 2 digital codec). The answer is AR_OP_PRM_REQUEST_DONE / _RELEASE_DONE. */
#define AR_PRM_IID 2u
#define AR_OP_PRM_REQUEST 0x0100100Fu
#define AR_OP_PRM_RELEASE 0x01001010u
#define AR_OP_PRM_REQUEST_DONE 0x02001002u
#define AR_OP_PRM_RELEASE_DONE 0x02001003u
#define AR_CLK_SEC_TDM_IBIT 0x202u   /* the secondary TDM's internal bit clock: PRM's id (the DT's index is 37 = 0x25) */
#define AR_CLK_VA_CORE_MCLK 0x307u   /* sheng's SM8550 value; piano's: needs-boot (level 3) */
#define AR_HW_MACRO 1u
#define AR_HW_DCODEC 2u
uint32_t ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz);
uint32_t ar_prm_clock_attr(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz, uint32_t attribute);
uint32_t ar_prm_vote(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t block, int enable);

/* Answers. */
typedef struct {
    uint32_t opcode, token, src_port, dst_port, src_domain;
    uint32_t result_opcode, status;  /* basic result: the command and its status (0 success); framework state: 1 ready; PRM: status */
    uint32_t handle;                 /* AR_OP_MEM_MAP_DONE */
    uint64_t address;                /* write/read done: the buffer */
    uint32_t data_bytes, offset;     /* read done: bytes filled, starting offset bytes into the buffer */
} ArAnswer;
enum {ArUnknown = 0, ArBasicResult = 1, ArMapped = 2, ArWriteDone = 3, ArReadDone = 4, ArFrameworkState = 5, ArPrmDone = 6, ArBad = -1};
int ar_parse(const uint8_t *in, uint32_t length, ArAnswer *a);

#ifdef __cplusplus
}
#endif
#endif
