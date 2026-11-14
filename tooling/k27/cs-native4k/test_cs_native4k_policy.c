/*
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * Host test of cs_native4k_policy.h (k27 0067): with 1 to 16 participants on real threads, chunk sizes from one block to 4 MiB,
 * buffers smaller than a chunk and signed ranges that end inside a block, the derived 4 KiB table equals the one-thread pass byte
 * for byte, every block is verified exactly once, and every page derived exactly once; a bad block anywhere, or a read error, fails
 * the pass (and the first error is the one kept); the helpers stop claiming after an error.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs_native4k_policy.h"

#define MAX_LIMIT (40u * 1024u * 1024u + 12345u)

static uint8_t *file;            /* the signed bytes */
static uint64_t *signature;      /* one digest per 16 KiB block */

static uint64_t
digest(const uint8_t *data, uint32_t length)   /* FNV-1a, a stand-in for SHA-256: the policy never looks inside */
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = 0; i < length; i++) {
		h = (h ^ data[i]) * 1099511628211ull;
	}
	return h ^ length;
}

struct context {
	uint64_t limit;
	uint64_t *table;              /* one slot per 4 KiB page */
	unsigned *verified, *derived; /* per block / per page, counted atomically */
	uint64_t bad_block;           /* this block's bytes differ from the signature (UINT64_MAX: none) */
	uint64_t failing_read;        /* a read covering this position fails with EIO (UINT64_MAX: none) */
	unsigned reads;
};

static int
port_read(void *c, uint8_t *buffer, uint32_t length, uint64_t position)
{
	struct context *ctx = c;
	__atomic_fetch_add(&ctx->reads, 1, __ATOMIC_RELAXED);
	assert(position + length <= ctx->limit && length > 0);
	if (ctx->failing_read >= position && ctx->failing_read < position + length) {
		return 5; /* EIO */
	}
	memcpy(buffer, file + position, length);
	if (ctx->bad_block != UINT64_MAX && ctx->bad_block * CS4K_BLOCK >= position && ctx->bad_block * CS4K_BLOCK < position + length) {
		buffer[ctx->bad_block * CS4K_BLOCK - position] ^= 1;
	}
	return 0;
}

static int
port_verify(void *c, uint64_t block, const uint8_t *data, uint32_t length)
{
	struct context *ctx = c;
	uint64_t expected_length = ctx->limit - block * CS4K_BLOCK < CS4K_BLOCK ? ctx->limit - block * CS4K_BLOCK : CS4K_BLOCK;
	assert(length == expected_length);
	__atomic_fetch_add(&ctx->verified[block], 1, __ATOMIC_RELAXED);
	return digest(data, length) == signature[block] ? 0 : 8; /* ENOEXEC */
}

static void
port_derive(void *c, uint64_t page, const uint8_t *data, uint32_t length)
{
	struct context *ctx = c;
	__atomic_fetch_add(&ctx->derived[page], 1, __ATOMIC_RELAXED);
	ctx->table[page] = digest(data, length);
}

static const struct cs4k_ops ops = { port_read, port_verify, port_derive };

struct pass {
	struct context *ctx;
	uint32_t chunk, buffer_bytes;
	uint64_t chunks, next;
	int error;
};

static void *
participant(void *p)
{
	struct pass *pass = p;
	uint8_t *buffer = malloc(pass->buffer_bytes);
	cs4k_participate(&ops, pass->ctx, pass->ctx->limit, pass->chunk, pass->chunks, &pass->next, &pass->error, buffer, pass->buffer_bytes);
	free(buffer);
	return NULL;
}

static int
run(struct context *ctx, uint32_t threads, uint32_t chunk, uint32_t buffer_bytes)
{
	struct pass pass = { ctx, chunk, buffer_bytes, cs4k_chunks(ctx->limit, chunk), 0, 0 };
	uint32_t n = cs4k_participants(threads, 0, pass.chunks);
	pthread_t helper[CS4K_MAX_THREADS];
	for (uint32_t i = 1; i < n; i++) {
		assert(pthread_create(&helper[i], NULL, participant, &pass) == 0);
	}
	participant(&pass);                       /* the caller takes part */
	for (uint32_t i = 1; i < n; i++) {
		pthread_join(helper[i], NULL);
	}
	return pass.error;
}

static struct context
fresh(uint64_t limit)
{
	uint64_t pages = (limit + CS4K_PAGE - 1) / CS4K_PAGE, blocks = (limit + CS4K_BLOCK - 1) / CS4K_BLOCK;
	struct context ctx = { limit, calloc(pages, 8), calloc(blocks, sizeof(unsigned)), calloc(pages, sizeof(unsigned)), UINT64_MAX,
		               UINT64_MAX, 0 };
	return ctx;
}

static void
release(struct context *ctx)
{
	free(ctx->table);
	free(ctx->verified);
	free(ctx->derived);
}

int
main(void)
{
	file = malloc(MAX_LIMIT);
	uint64_t seed = 7;
	for (uint32_t i = 0; i < MAX_LIMIT; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		file[i] = (uint8_t)(seed >> 56);
	}
	/* the policy's numbers */
	assert(cs4k_chunk_bytes(0) == 1024u * 1024u && cs4k_chunk_bytes(1) == CS4K_BLOCK && cs4k_chunk_bytes(17) == CS4K_BLOCK);
	assert(cs4k_chunk_bytes(33) == 2 * CS4K_BLOCK && cs4k_chunk_bytes(1u << 30) == CS4K_MAX_CHUNK);
	assert(cs4k_participants(0, 8, 100) == 1 && cs4k_participants(6, 8, 100) == 6 && cs4k_participants(12, 8, 100) == 8);
	assert(cs4k_participants(40, 0, 100) == CS4K_MAX_THREADS && cs4k_participants(6, 8, 2) == 2 && cs4k_participants(6, 8, 0) == 1);
	uint64_t o, l;
	cs4k_chunk_range(2, 3 * 1048576 + 5, 1048576, &o, &l);
	assert(o == 2 * 1048576 && l == 1048576);
	cs4k_chunk_range(3, 3 * 1048576 + 5, 1048576, &o, &l);
	assert(o == 3 * 1048576 && l == 5);
	cs4k_chunk_range(4, 3 * 1048576 + 5, 1048576, &o, &l);
	assert(l == 0);

	const uint64_t limits[] = { 1, 4095, 4096, 4097, 16383, 16384, 16385, 1048576, 1048576 + 4096 + 7, 5 * 1048576 + 16384 * 3 + 100,
		                    MAX_LIMIT };
	const uint32_t chunks[] = { CS4K_BLOCK, 3 * CS4K_BLOCK, 1048576, 4 * 1048576 };
	const uint32_t threads[] = { 1, 2, 3, 6, 8, 16 };
	unsigned passes = 0;
	for (unsigned li = 0; li < sizeof(limits) / sizeof(limits[0]); li++) {
		uint64_t limit = limits[li], blocks = (limit + CS4K_BLOCK - 1) / CS4K_BLOCK, pages = (limit + CS4K_PAGE - 1) / CS4K_PAGE;
		signature = malloc(blocks * 8);
		for (uint64_t b = 0; b < blocks; b++) {
			uint64_t len = limit - b * CS4K_BLOCK < CS4K_BLOCK ? limit - b * CS4K_BLOCK : CS4K_BLOCK;
			signature[b] = digest(file + b * CS4K_BLOCK, (uint32_t)len);
		}
		struct context reference = fresh(limit);   /* the one-participant, one-block-at-a-time pass: today's order */
		assert(run(&reference, 1, CS4K_BLOCK, CS4K_BLOCK) == 0);
		for (unsigned ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
			for (unsigned ti = 0; ti < sizeof(threads) / sizeof(threads[0]); ti++) {
				for (unsigned bi = 0; bi < 2; bi++) {
					uint32_t chunk = chunks[ci], buffer_bytes = bi ? CS4K_BLOCK : chunk;   /* also a buffer of one block */
					struct context ctx = fresh(limit);
					assert(run(&ctx, threads[ti], chunk, buffer_bytes) == 0);
					assert(memcmp(ctx.table, reference.table, pages * 8) == 0);
					for (uint64_t b = 0; b < blocks; b++) {
						assert(ctx.verified[b] == 1);
					}
					for (uint64_t p = 0; p < pages; p++) {
						assert(ctx.derived[p] == 1);
					}
					release(&ctx);
					passes++;
				}
			}
			/* a bad block (first, middle, last) or a read error fails the pass, whatever the threads */
			const uint64_t bad[] = { 0, blocks / 2, blocks - 1 };
			for (unsigned k = 0; k < 3; k++) {
				for (unsigned ti = 0; ti < sizeof(threads) / sizeof(threads[0]); ti++) {
					struct context ctx = fresh(limit);
					ctx.bad_block = bad[k];
					assert(run(&ctx, threads[ti], chunks[ci], chunks[ci]) == 8);
					assert(ctx.verified[bad[k]] == 1);
					release(&ctx);
					ctx = fresh(limit);
					ctx.failing_read = bad[k] * CS4K_BLOCK;
					assert(run(&ctx, threads[ti], chunks[ci], chunks[ci]) == 5);
					assert(ctx.verified[bad[k]] == 0);
					release(&ctx);
					passes += 2;
				}
			}
		}
		/* after an error the others stop: with a bad first block and one participant, nothing after the first chunk is read */
		if (blocks > 2 * 64) {
			struct context ctx = fresh(limit);
			ctx.bad_block = 0;
			assert(run(&ctx, 1, 1048576, 1048576) == 8 && ctx.reads == 1);
			release(&ctx);
		}
		release(&reference);
		free(signature);
	}
	/* bad arguments: a buffer that is not whole blocks, a chunk start inside a block, beyond the range */
	struct context ctx = fresh(3 * CS4K_BLOCK);
	int none = 0;
	uint8_t buffer[CS4K_BLOCK];
	assert(cs4k_process_chunk(&ops, &ctx, 0, CS4K_BLOCK, 3 * CS4K_BLOCK, buffer, 4096, &none) == 22);
	assert(cs4k_process_chunk(&ops, &ctx, 4096, CS4K_BLOCK, 3 * CS4K_BLOCK, buffer, CS4K_BLOCK, &none) == 22);
	assert(cs4k_process_chunk(&ops, &ctx, 2 * CS4K_BLOCK, 2 * CS4K_BLOCK, 3 * CS4K_BLOCK, buffer, CS4K_BLOCK, &none) == 22);
	release(&ctx);
	/* the first error is kept */
	int error = 0;
	cs4k_fail(&error, 0);
	assert(error == 0);
	cs4k_fail(&error, 8);
	cs4k_fail(&error, 5);
	assert(error == 8);
	/* cs-native4k-skip: only with the skip on and no table; whole pages before codeLimit; NX for the one that crosses it */
	int nx = -1;
	const uint64_t code_limit = 5 * CS4K_BLOCK + 100;   /* ends 100 bytes into page 20 */
	assert(!cs4k_skip_accepts(0, 0, 0, code_limit, &nx) && nx == -1);
	assert(!cs4k_skip_accepts(1, 1, 0, code_limit, &nx) && nx == -1);
	assert(cs4k_skip_accepts(1, 0, 0, code_limit, &nx) && nx == 0);
	assert(cs4k_skip_accepts(1, 0, 19 * CS4K_PAGE, code_limit, &nx) && nx == 0);
	assert(cs4k_skip_accepts(1, 0, 20 * CS4K_PAGE, code_limit, &nx) && nx == 1);
	nx = -1;
	assert(!cs4k_skip_accepts(1, 0, 21 * CS4K_PAGE, code_limit, &nx) && nx == -1);   /* past codeLimit: answered as without */
	assert(!cs4k_skip_accepts(1, 0, 4096 + 512, code_limit, &nx) && nx == -1);       /* not a page boundary */
	assert(cs4k_skip_accepts(1, 0, 3 * CS4K_PAGE, 4 * CS4K_PAGE, &nx) && nx == 0);   /* a range ending on a page */
	assert(!cs4k_skip_accepts(1, 0, 4 * CS4K_PAGE, 4 * CS4K_PAGE, &nx));
	printf("PASS: cs_native4k_policy.h: cs-native4k-skip accepts whole pages before codeLimit of a signature without a table "
	    "(NX for the crossing page), nothing else\n");
	printf("PASS: cs_native4k_policy.h: %u passes (1-16 participants, chunks 16 KiB-4 MiB, block buffers, ranges ending inside a block): "
	    "the same 4 KiB table as one thread, every block verified and every page derived once; bad blocks and read errors fail the "
	    "pass with the first error; claiming stops after an error\n", passes);
	free(file);
	return 0;
}
