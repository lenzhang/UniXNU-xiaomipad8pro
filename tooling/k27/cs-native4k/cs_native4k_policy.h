/*
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * cs_native4k_policy.h: the work split of k27 patch 0067's cs_native4k_prepare (bsd/kern/ubc_subr.c), the authentication of a
 * 16 KiB-page code signature on the 4 KiB-page native kernel. Written for UniXNU-piano; the kernel carries this file unchanged as
 * bsd/kern/cs_native4k_policy.h (tooling/k27/test-cs-native4k.sh checks the copy and runs the host test).
 *
 * The signed range [0, limit) is cut into chunks of `chunk` bytes (a multiple of 16 KiB, 1 MiB by default). Every participant (the
 * caller and up to CS4K_MAX_THREADS - 1 helper threads) claims the next chunk with one atomic add and processes it alone: it reads the
 * chunk in pieces of its buffer (one read per piece), verifies each 16 KiB block against the signature, then derives the block's
 * 4 KiB page digests into their slots. Slots are indexed by page, so the order of chunks does not change the table. The first error
 * any participant meets is kept (later ones are dropped) and makes every participant stop claiming; the caller publishes nothing
 * unless no error was recorded, after every helper has finished. Nothing here allocates, locks or sleeps: the port supplies the
 * read, the two digests and the atomics' memory (plain uint64_t / int words, accessed only with these functions).
 */
#ifndef CS_NATIVE4K_POLICY_H
#define CS_NATIVE4K_POLICY_H

#include <stdint.h>
#include <stddef.h>

#define CS4K_BLOCK          16384u
#define CS4K_PAGE           4096u
#define CS4K_MAX_THREADS    16u
#define CS4K_DEFAULT_CHUNK  (1024u * 1024u)
#define CS4K_MAX_CHUNK      (16u * 1024u * 1024u)

/* The chunk (= the parallel path's read size) for cs-native4k-read-kib=K: whole 16 KiB blocks, 16 KiB .. 16 MiB; 0: 1 MiB. */
static inline uint32_t
cs4k_chunk_bytes(uint32_t read_kib)
{
	uint64_t bytes = read_kib ? (uint64_t)read_kib * 1024u : CS4K_DEFAULT_CHUNK;
	if (bytes < CS4K_BLOCK) {
		bytes = CS4K_BLOCK;
	}
	if (bytes > CS4K_MAX_CHUNK) {
		bytes = CS4K_MAX_CHUNK;
	}
	return (uint32_t)(bytes & ~(uint64_t)(CS4K_BLOCK - 1));
}

static inline uint64_t
cs4k_chunks(uint64_t limit, uint32_t chunk)
{
	return chunk ? (limit + chunk - 1) / chunk : 0;
}

/* Participants for cs-native4k-threads=T on ncpu CPUs: at least 1, at most T, CS4K_MAX_THREADS, ncpu (when known) and chunks. */
static inline uint32_t
cs4k_participants(uint32_t threads, uint32_t ncpu, uint64_t chunks)
{
	uint32_t n = threads ? threads : 1;
	if (n > CS4K_MAX_THREADS) {
		n = CS4K_MAX_THREADS;
	}
	if (ncpu && n > ncpu) {
		n = ncpu;
	}
	if (n > chunks) {
		n = (uint32_t)chunks;
	}
	return n ? n : 1;
}

/* Chunk i of [0, limit): [*offset, *offset + *length). */
static inline void
cs4k_chunk_range(uint64_t i, uint64_t limit, uint32_t chunk, uint64_t *offset, uint64_t *length)
{
	uint64_t start = i * (uint64_t)chunk;
	*offset = start < limit ? start : limit;
	*length = start < limit ? (limit - start < chunk ? limit - start : chunk) : 0;
}

/* The next chunk to process, or `chunks` when none is left or an error stopped the pass. */
static inline uint64_t
cs4k_claim(uint64_t *next, const int *error, uint64_t chunks)
{
	if (__atomic_load_n(error, __ATOMIC_ACQUIRE) != 0) {
		return chunks;
	}
	uint64_t i = __atomic_fetch_add(next, 1, __ATOMIC_RELAXED);
	return i < chunks ? i : chunks;
}

/* Keep the first error only. */
static inline void
cs4k_fail(int *error, int value)
{
	int none = 0;
	if (value != 0) {
		(void)__atomic_compare_exchange_n(error, &none, value, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}
}

/*
 * The port: read `length` bytes at signed-range offset `position` into `buffer` (0, or an errno: a short read is an error); verify
 * the 16 KiB block `block` (its `length` bytes: 16 KiB, less for the last) against the signature (0, or an errno); derive the digest
 * of 4 KiB page `page` (its `length` bytes) into its slot.
 */
struct cs4k_ops {
	int (*read)(void *context, uint8_t *buffer, uint32_t length, uint64_t position);
	int (*verify)(void *context, uint64_t block, const uint8_t *data, uint32_t length);
	void (*derive)(void *context, uint64_t page, const uint8_t *data, uint32_t length);
};

/*
 * One chunk [offset, offset + length) of [0, limit), through a buffer of `buffer_bytes` (a multiple of 16 KiB, at least one block):
 * pieces read in order, each piece's blocks verified and split in order. Stops at the first error, or before the next piece once
 * *error is set by another participant (then returns 0: that error is the pass's).
 */
static inline int
cs4k_process_chunk(const struct cs4k_ops *ops, void *context, uint64_t offset, uint64_t length, uint64_t limit, uint8_t *buffer,
    uint32_t buffer_bytes, const int *error)
{
	if (buffer_bytes < CS4K_BLOCK || (buffer_bytes % CS4K_BLOCK) || (offset % CS4K_BLOCK) || offset + length > limit) {
		return 22; /* EINVAL */
	}
	for (uint64_t done = 0; done < length;) {
		if (__atomic_load_n(error, __ATOMIC_ACQUIRE) != 0) {
			return 0;
		}
		uint64_t position = offset + done;
		uint32_t piece = (uint32_t)(length - done < buffer_bytes ? length - done : buffer_bytes);
		int status = ops->read(context, buffer, piece, position);
		if (status != 0) {
			return status;
		}
		for (uint32_t at = 0; at < piece; at += CS4K_BLOCK) {
			uint32_t block_bytes = piece - at < CS4K_BLOCK ? piece - at : CS4K_BLOCK;
			status = ops->verify(context, (position + at) / CS4K_BLOCK, buffer + at, block_bytes);
			if (status != 0) {
				return status;
			}
			for (uint32_t sub = 0; sub < block_bytes; sub += CS4K_PAGE) {
				uint32_t page_bytes = block_bytes - sub < CS4K_PAGE ? block_bytes - sub : CS4K_PAGE;
				ops->derive(context, (position + at + sub) / CS4K_PAGE, buffer + at + sub, page_bytes);
			}
		}
		done += piece;
	}
	return 0;
}

/* A participant's whole share: claim chunks until none is left or the pass stopped; its error goes to *error. */
static inline void
cs4k_participate(const struct cs4k_ops *ops, void *context, uint64_t limit, uint32_t chunk, uint64_t chunks, uint64_t *next,
    int *error, uint8_t *buffer, uint32_t buffer_bytes)
{
	for (uint64_t i; (i = cs4k_claim(next, error, chunks)) < chunks;) {
		uint64_t offset, length;
		cs4k_chunk_range(i, limit, chunk, &offset, &length);
		cs4k_fail(error, cs4k_process_chunk(ops, context, offset, length, limit, buffer, buffer_bytes, error));
	}
}

#endif /* CS_NATIVE4K_POLICY_H */
