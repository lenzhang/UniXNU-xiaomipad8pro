// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of piano's graph manager commands (PianoAudioReach.c): packet sizes, parameter block layout and the fields the DSP reads,
// checked against the layout the DSP's reference client produces for the graphs Ubuntu runs on piano (Xiaomi-Pad-8-Pro.m4), and a
// few packets compared byte for byte with hand-assembled ones. The harness (blocks(), CHECK) is UniXNU-sheng's tests/audioreach_host.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../PianoAudioReach.h"
#include "../PianoGpr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}

/* Walks the parameter blocks of an APM command; returns the count and checks each stays inside the payload. Blocks are 8-byte sized
 * except a packet's last, which may be exact (the reference client sizes the logger's 12 bytes and the MFC's to 4). */
static unsigned blocks(const uint8_t *pkt, uint32_t n, uint32_t *iid, uint32_t *id, uint32_t *offset, uint32_t *size, unsigned max)
{
    GprPacket p;
    CHECK(gpr_decode(pkt, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    CHECK(p.payload_bytes >= 16 && le32(p.payload + 12) == p.payload_bytes - 16);   /* in-band, size matches */
    unsigned count = 0;
    for (uint32_t at = 16; at < p.payload_bytes; ) {
        CHECK(at + 16 <= p.payload_bytes);
        const uint32_t s = le32(p.payload + at + 8);
        CHECK(at + 16 + s <= p.payload_bytes && le32(p.payload + at + 12) == 0);
        CHECK(s % 8 == 0 || at + 16 + s == p.payload_bytes);
        CHECK(count < max);
        iid[count] = le32(p.payload + at); id[count] = le32(p.payload + at + 4); offset[count] = 24 + at + 16; size[count] = s;
        ++count; at += 16 + s;
    }
    return count;
}

/* The connection list (block 4 of a graph open): link i's source, its port, destination and its port. */
static void link_at(const uint8_t *b, uint32_t off, unsigned i, uint32_t from, uint32_t to)
{
    const uint8_t *l = b + off + 4 + 16 * i;
    CHECK(le32(l) == from && le32(l + 4) == 1 && le32(l + 8) == to && le32(l + 12) == 2);
}

int main(void)
{
    uint8_t b[GPR_MAX_PACKET];
    uint32_t iid[8], id[8], off[8], sz[8];
    const ArFormat stream4 = {48000, 16, 4}, slots = {48000, 32, 4}, mic = {48000, 16, 2};
    GprPacket p;

    /* ---- the speaker device graph: LOG -> MFC -> TDM sink; subgraph TX, playback, low latency; container at the stream position. */
    uint32_t n = ar_open_speaker_device(b, sizeof b, AR_PORT_APM, 7);
    CHECK(n && gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_GRAPH_OPEN && p.dst_port == AR_APM_IID && p.src_port == 1 && p.token == 7);
    CHECK(p.dst_domain == GPR_DOMAIN_ADSP && p.src_domain == GPR_DOMAIN_APPS);
    CHECK(blocks(b, n, iid, id, off, sz, 8) == 5);
    CHECK(id[0] == 0x08001001 && id[1] == 0x08001000 && id[2] == 0x08001002 && id[3] == 0x08001003 && id[4] == 0x08001004);
    for (int i = 0; i < 5; ++i) CHECK(iid[i] == AR_APM_IID);
    CHECK(le32(b + off[0]) == 1 && le32(b + off[0] + 4) == AR_SG_SPEAKER && le32(b + off[0] + 8) == 3);
    CHECK(le32(b + off[0] + 12) == 0x0800100E && le32(b + off[0] + 20) == 2 && le32(b + off[0] + 32) == 1 && le32(b + off[0] + 44) == 1);
    CHECK(le32(b + off[1] + 4) == AR_SG_SPEAKER && le32(b + off[1] + 24) == 0x0B001001 && le32(b + off[1] + 36) == 1);   /* GC, stream */
    CHECK(le32(b + off[1] + 48) == 8192 && le32(b + off[1] + 60) == 2);
    CHECK(le32(b + off[2] + 12) == 3 && le32(b + off[2] + 16) == AR_MOD_LOG && le32(b + off[2] + 20) == AR_IID_SPK_LOG);
    CHECK(le32(b + off[2] + 24) == AR_MOD_MFC && le32(b + off[2] + 28) == AR_IID_SPK_MFC);
    CHECK(le32(b + off[2] + 32) == 0x0700100E && le32(b + off[2] + 36) == AR_IID_SPK_TDM);
    CHECK(le32(b + off[3]) == 3);
    CHECK(le32(b + off[3] + 4 + 48) == AR_IID_SPK_TDM && le32(b + off[3] + 4 + 64) == 1 && le32(b + off[3] + 4 + 68) == 0);   /* sink: 1 in, 0 out */
    CHECK(le32(b + off[4]) == 2);
    link_at(b, off[4], 0, AR_IID_SPK_LOG, AR_IID_SPK_MFC);
    link_at(b, off[4], 1, AR_IID_SPK_MFC, AR_IID_SPK_TDM);

    /* ---- the playback stream graph: WR_SH (no input, output port 1) -> DEC -> CNV -> LOG, and the link into the device's logger. */
    n = ar_open_playback_stream(b, sizeof b, AR_PORT_APM, 8);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 5);
    CHECK(le32(b + off[0] + 4) == AR_SG_PLAY && le32(b + off[0] + 32) == 2);     /* direction RX */
    CHECK(le32(b + off[2] + 12) == 4 && le32(b + off[2] + 16) == AR_MOD_WR_SHMEM && le32(b + off[2] + 40) == AR_MOD_LOG);
    CHECK(le32(b + off[3]) == 4 && le32(b + off[3] + 4) == AR_IID_PLAY_SHMEM && le32(b + off[3] + 20) == 0 && le32(b + off[3] + 24) == 1);
    CHECK(le32(b + off[4]) == 4);
    link_at(b, off[4], 0, AR_IID_PLAY_SHMEM, AR_IID_PLAY_DEC);
    link_at(b, off[4], 1, AR_IID_PLAY_DEC, AR_IID_PLAY_CNV);
    link_at(b, off[4], 2, AR_IID_PLAY_CNV, AR_IID_PLAY_LOG);
    link_at(b, off[4], 3, AR_IID_PLAY_LOG, AR_IID_SPK_LOG);

    /* ---- the TDM sink: interface, hardware endpoint format, frame size factor. */
    ArTdm t;
    ar_piano_tdm(&t);
    CHECK(t.lpaif_type == 0 && t.intf_idx == 1 && t.sync_src == 1 && t.data_out_enable == 1 && t.slot_mask == 0xf && t.slots == 4);
    CHECK(t.slot_width == 32 && t.sync_mode == 1 && t.invert_sync == 0 && t.data_delay == 1);
    CHECK(AR_TDM_BIT_CLOCK == 48000u * 4u * 32u);
    n = ar_config_speaker_tdm(b, sizeof b, AR_PORT_APM, 9, &t, &slots);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 3);
    CHECK(iid[0] == AR_IID_SPK_TDM && id[0] == 0x0800101B && sz[0] == 32);
    CHECK(le32(b + off[0]) == 0 && le32(b + off[0] + 4) == 1 && le16(b + off[0] + 8) == 1 && le16(b + off[0] + 10) == 1);
    CHECK(le32(b + off[0] + 12) == 0xf && le16(b + off[0] + 16) == 4 && le16(b + off[0] + 18) == 32 && le16(b + off[0] + 20) == 1);
    CHECK(le16(b + off[0] + 22) == 0 && le16(b + off[0] + 24) == 1 && le16(b + off[0] + 26) == 0 && le32(b + off[0] + 28) == 0);
    CHECK(iid[1] == AR_IID_SPK_TDM && id[1] == 0x08001017 && sz[1] == 16 && le32(b + off[1]) == 48000 && le16(b + off[1] + 4) == 32 &&
          le16(b + off[1] + 6) == 4 && le32(b + off[1] + 8) == 1);
    CHECK(id[2] == 0x08001018 && sz[2] == 8 && le32(b + off[2]) == 1);
    CHECK(ar_config_speaker_tdm(b, sizeof b, AR_PORT_APM, 9, &t, &stream4) == 0);   /* 16-bit data does not fit 32-bit slots */
    ArTdm three = t; three.slots = 3;
    CHECK(ar_config_speaker_tdm(b, sizeof b, AR_PORT_APM, 9, &three, &slots) == 0);

    /* ---- the MFC: the slots' format and channel types FL FR LS RS (top left, top right, bottom left, bottom right). */
    n = ar_config_speaker_mfc(b, sizeof b, AR_PORT_APM, 10, &slots);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 1 && iid[0] == AR_IID_SPK_MFC && id[0] == 0x08001024 && sz[0] == 16);
    CHECK(le32(b + off[0]) == 48000 && le16(b + off[0] + 4) == 32 && le16(b + off[0] + 6) == 4);
    CHECK(le16(b + off[0] + 8) == 1 && le16(b + off[0] + 10) == 2 && le16(b + off[0] + 12) == 4 && le16(b + off[0] + 14) == 5);
    const ArFormat bad = {48000, 24, 4}, three_ch = {48000, 16, 3};
    CHECK(ar_config_speaker_mfc(b, sizeof b, AR_PORT_APM, 10, &bad) == 0 && ar_config_speaker_mfc(b, sizeof b, AR_PORT_APM, 10, &three_ch) == 0);

    /* ---- the loggers: exact 12-byte bodies, the topology's codes, tap point 1, mode 0. */
    n = ar_config_speaker_logger(b, sizeof b, AR_PORT_APM, 11);
    CHECK(n == 24 + 16 + 16 + 12 && blocks(b, n, iid, id, off, sz, 8) == 1 && iid[0] == AR_IID_SPK_LOG && id[0] == 0x08001031 && sz[0] == 12);
    CHECK(le32(b + off[0]) == 0x19ab && le32(b + off[0] + 4) == 1 && le32(b + off[0] + 8) == 0);
    n = ar_config_playback_logger(b, sizeof b, AR_PORT_APM, 12);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 1 && iid[0] == AR_IID_PLAY_LOG && le32(b + off[0]) == 0x19af);
    n = ar_config_mic_logger(b, sizeof b, AR_PORT_APM, 12);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 1 && iid[0] == AR_IID_MIC_LOG && le32(b + off[0]) == 0x19ab);
    n = ar_config_capture_logger(b, sizeof b, AR_PORT_APM, 12);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 1 && iid[0] == AR_IID_CAP_LOG && le32(b + off[0]) == 0x19af);

    /* ---- decoder and converter output formats of the four-channel stream. */
    n = ar_config_playback_stream(b, sizeof b, AR_PORT_APM, 13, &stream4);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 2);
    CHECK(iid[0] == AR_IID_PLAY_DEC && iid[1] == AR_IID_PLAY_CNV && id[0] == 0x08001008 && sz[0] == 32);
    CHECK(le32(b + off[0]) == 1 && le32(b + off[0] + 4) == 0x09001000 && le32(b + off[0] + 8) == 20);
    CHECK(le16(b + off[0] + 12) == 16 && le16(b + off[0] + 14) == 1 && le16(b + off[0] + 16) == 16 && le16(b + off[0] + 18) == 15);
    CHECK(le16(b + off[0] + 20) == 1 && le16(b + off[0] + 22) == 1 && le16(b + off[0] + 26) == 4);
    CHECK(b[off[0] + 28] == 1 && b[off[0] + 29] == 2 && b[off[0] + 30] == 4 && b[off[0] + 31] == 5);
    CHECK(le16(b + off[1] + 22) == 3 && le16(b + off[1] + 26) == 4 && b[off[1] + 30] == 4);

    /* ---- the shared memory endpoint's media format goes to the endpoint's own port. */
    n = ar_config_playback_shmem(b, sizeof b, AR_PORT_APM, 14, &stream4);
    CHECK(n && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && blocks(b, n, iid, id, off, sz, 8) == 1);
    CHECK(id[0] == 0x0800100C && sz[0] == 32 && le32(b + off[0] + 8) == 36 && le32(b + off[0] + 12) == 48000);
    CHECK(le16(b + off[0] + 26) == 4 && b[off[0] + 28] == 1 && b[off[0] + 29] == 2 && b[off[0] + 30] == 4 && b[off[0] + 31] == 5);

    /* ---- the microphones: codec DMA source on VA TX0 (LPAIF type 3, index 1) -> LOG, then the capture stream's five modules. */
    n = ar_open_mic_device(b, sizeof b, AR_PORT_APM, 15);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 5 && le32(b + off[0] + 4) == AR_SG_MIC && le32(b + off[1] + 36) == 4);   /* global device */
    CHECK(le32(b + off[2] + 16) == AR_MOD_CODEC_DMA_SOURCE && le32(b + off[3] + 4 + 16) == 0 && le32(b + off[3] + 4 + 20) == 1);
    link_at(b, off[4], 0, AR_IID_MIC_DMA, AR_IID_MIC_LOG);
    n = ar_open_capture_stream(b, sizeof b, AR_PORT_APM, 16);
    CHECK(n && n <= GPR_MAX_PACKET && blocks(b, n, iid, id, off, sz, 8) == 5 && le32(b + off[2] + 12) == 5 && le32(b + off[4]) == 5);
    link_at(b, off[4], 0, AR_IID_MIC_LOG, AR_IID_CAP_LOG);
    link_at(b, off[4], 4, AR_IID_CAP_ENC, AR_IID_CAP_SHMEM);
    n = ar_config_mic_device(b, sizeof b, AR_PORT_APM, 17, &mic, 0x3);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 4 && id[2] == 0x08001063 && le32(b + off[2]) == 3 && le32(b + off[2] + 4) == 1 &&
          le32(b + off[2] + 8) == 3 && id[3] == 0x08001176);
    CHECK(ar_config_mic_device(b, sizeof b, AR_PORT_APM, 17, &mic, 0) == 0);
    n = ar_config_capture_stream(b, sizeof b, AR_PORT_APM, 18, &mic);
    CHECK(n && blocks(b, n, iid, id, off, sz, 8) == 3 && iid[0] == AR_IID_CAP_MFC && sz[0] == 16 && iid[2] == AR_IID_CAP_ENC);
    CHECK(le16(b + off[0] + 6) == 2 && le16(b + off[0] + 8) == 1 && le16(b + off[0] + 10) == 2);

    /* ---- graph commands list one subgraph: GRAPH_START of the speaker device from port 1, token 7, byte for byte. */
    n = ar_graph_command(b, sizeof b, AR_PORT_APM, 7, AR_OP_GRAPH_START, AR_SG_SPEAKER);
    const uint8_t start[64] = {0x60, 0x40, 0, 0, 2, 3, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0x02, 0x10, 0x00, 0x01,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0, 0, 0,
                               1, 0, 0, 0, 0x05, 0x10, 0x00, 0x08, 8, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0x05, 0x40, 0, 0};
    CHECK(n == 64 && !memcmp(b, start, 64));

    /* ---- the PRM: the secondary TDM's bit clock at 6.144 MHz, couple-no, default root; its release; byte for byte. */
    n = ar_prm_clock(b, sizeof b, AR_PORT_PRM, 0x21, AR_CLK_SEC_TDM_IBIT, AR_TDM_BIT_CLOCK);
    const uint8_t clk[76] = {0x60, 0x4c, 0, 0, 2, 3, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 0x21, 0, 0, 0, 0x0f, 0x10, 0x00, 0x01,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 36, 0, 0, 0,
                             2, 0, 0, 0, 0x2c, 0x10, 0x00, 0x08, 20, 0, 0, 0, 0, 0, 0, 0,
                             1, 0, 0, 0, 0x02, 0x02, 0, 0, 0x00, 0xc0, 0x5d, 0x00, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(n == 76 && !memcmp(b, clk, 76));
    n = ar_prm_clock(b, sizeof b, AR_PORT_PRM, 0x22, AR_CLK_SEC_TDM_IBIT, 0);
    CHECK(n == 24 + 16 + 16 + 8 && gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_PRM_RELEASE && le32(b + 60) == 0x202);
    n = ar_prm_vote(b, sizeof b, AR_PORT_PRM, 0x23, AR_HW_MACRO, 1);
    CHECK(n == 24 + 16 + 20 && le32(b + 44) == 0x08001032 && le32(b + 48) == 4 && le32(b + 56) == 1);

    /* ---- the memory map, buffers and the DSP address: a 32-bit physical address with the stream index above it. */
    uint64_t a = 0;
    CHECK(ar_dsp_address(0x9f000000ull, 0x1001, &a) == 0 && a == 0x19f000000ull);
    CHECK(ar_dsp_address(0x8c0000000ull, 0x1001, &a) == -1);   /* XNU's memory on piano: refused */
    CHECK(ar_dsp_address(0xfffff000ull, 0x1041, &a) == 0 && a == 0x1fffff000ull);
    n = ar_map(b, sizeof b, AR_PORT_APM, 13, 0x19f000000ull, 5000);
    CHECK(n == 24 + 20 + 16 && gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_MEM_MAP);
    CHECK(le16(b + 24) == 3 && le16(b + 26) == 1 && le32(b + 28) == 0 && le32(b + 32) == 0x9f000000 && le32(b + 36) == 1 && le32(b + 40) == 8192);
    CHECK(ar_map(b, sizeof b, AR_PORT_APM, 13, 0x19f000000ull, 0) == 0);
    n = ar_unmap(b, sizeof b, AR_PORT_APM, 14, 0x55);
    CHECK(n == 24 + 4 + 16 && le32(b + 24) == 0x55);
    n = ar_write(b, sizeof b, AR_PORT_DATA, 14, 0x19f000000ull, 0x55, 3840);
    CHECK(n == 24 + 44 && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && p.src_port == AR_PORT_DATA);
    CHECK(le32(b + 24) == 0x9f000000 && le32(b + 28) == 1 && le32(b + 32) == 0x55 && le32(b + 36) == 3840);
    n = ar_read(b, sizeof b, AR_PORT_DATA, 19, 0x19f001000ull, 0x56, 1920);
    CHECK(n == 24 + 32 && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_CAP_SHMEM);

    /* ---- every builder refuses a buffer that is too small, and stays within one intent. */
    CHECK(ar_open_playback_stream(b, 100, 1, 1) == 0 && ar_open_capture_stream(b, 400, 1, 1) == 0 && ar_config_speaker_tdm(b, 60, 1, 1, &t, &slots) == 0);
    CHECK(ar_open_capture_stream(b, sizeof b, 1, 1) <= GPR_MAX_PACKET && ar_open_playback_stream(b, sizeof b, 1, 1) <= GPR_MAX_PACKET);
    uint8_t m4[4];
    CHECK(ar_channel_map(1, m4) == 1 && m4[0] == 1 && ar_channel_map(2, m4) == 2 && m4[1] == 2 && ar_channel_map(3, m4) == 0);

    /* ---- answers. */
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = 1, .dst_port = 1, .token = 5, .opcode = GPR_OP_BASIC_RESULT};
    uint32_t words[10] = {AR_OP_GRAPH_OPEN, 0};
    ArAnswer ans;
    n = gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(ar_parse(b, n, &ans) == ArBasicResult && ans.result_opcode == AR_OP_GRAPH_OPEN && ans.status == 0 && ans.token == 5 &&
          ans.dst_port == 1 && ans.src_domain == GPR_DOMAIN_ADSP);
    r.opcode = AR_OP_MEM_MAP_DONE; words[0] = 0x1234;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &ans) == ArMapped && ans.handle == 0x1234);
    r.opcode = AR_OP_WRITE_DONE; words[0] = 0x9f000000; words[1] = 1; words[2] = 0x55; words[3] = 0;
    n = gpr_encode(b, sizeof b, &r, words, 32);
    CHECK(ar_parse(b, n, &ans) == ArWriteDone && ans.address == 0x19f000000ull && ans.handle == 0x55 && ans.status == 0);
    r.opcode = AR_OP_READ_DONE; words[0] = 0; words[1] = 0xa000; words[2] = 0; words[3] = 0x56; words[4] = 1920;
    n = gpr_encode(b, sizeof b, &r, words, 40);
    CHECK(ar_parse(b, n, &ans) == ArReadDone && ans.address == 0xa000 && ans.data_bytes == 1920);
    CHECK(ar_parse(b, 10, &ans) == ArBad);
    r.opcode = GPR_OP_APM_SPF_STATE; words[0] = 1;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &ans) == ArFrameworkState && ans.status == 1);
    r.opcode = AR_OP_PRM_REQUEST_DONE; words[0] = AR_OP_PRM_REQUEST; words[1] = 0;
    n = gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(ar_parse(b, n, &ans) == ArPrmDone && ans.status == 0);
    r.opcode = 0x12345678;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &ans) == ArUnknown && ans.opcode == 0x12345678);

    printf("PASS audioreach: %u checks\n", checks);
    return 0;
}
