#include "microcode_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"

enum {
	PAGE_BYTES = 0x10000,
	// libxenon gives a shader's microcode this alignment (libxenon:drivers/xenos/xe.c:943).
	BLOCK_BYTES = 0x100,
	PAGE_BLOCKS = PAGE_BYTES / BLOCK_BYTES,
	MAP_BITS = 32,
	MAP_WORDS = PAGE_BLOCKS / MAP_BITS,
	// 16384 blocks; microcode past them, or past a page, gets pages of its own.
	MAX_PAGES = 64,
};

typedef struct {
	volatile uint32_t *memory;
	// A bit a block, set while the block holds microcode.
	uint32_t used[MAP_WORDS];
	uint32_t used_count;
} Page;

static Page pages[MAX_PAGES];
// Aurora loads shaders from more than one thread.
static RTL_CRITICAL_SECTION pages_lock;

void xsdk_microcode_init(void)
{
	RtlInitializeCriticalSection(&pages_lock);
}

static uint32_t blocks_for(uint32_t size_bytes)
{
	return (size_bytes + BLOCK_BYTES - 1) / BLOCK_BYTES;
}

static bool block_used(const Page *page, uint32_t block)
{
	return page->used[block / MAP_BITS] >> (block % MAP_BITS) & 1;
}

static void mark(Page *page, uint32_t first, uint32_t count, bool used)
{
	for (uint32_t block = first; block < first + count; block++) {
		const uint32_t bit = 1u << (block % MAP_BITS);
		page->used[block / MAP_BITS] = used ? page->used[block / MAP_BITS] | bit : page->used[block / MAP_BITS] & ~bit;
	}
	page->used_count = used ? page->used_count + count : page->used_count - count;
}

// The first block of the first run of count free blocks, or PAGE_BLOCKS if none.
static uint32_t find_run(const Page *page, uint32_t count)
{
	uint32_t run = 0;
	for (uint32_t block = 0; block < PAGE_BLOCKS; block++) {
		run = block_used(page, block) ? 0 : run + 1;
		if (run == count)
			return block + 1 - count;
	}
	return PAGE_BLOCKS;
}

// Under pages_lock. *full: every record holds a page, and none has the room.
static volatile uint32_t *take_blocks(uint32_t count, bool *full)
{
	Page *empty = NULL;
	for (Page *page = pages; page < pages + MAX_PAGES; page++) {
		if (!page->memory) {
			empty = empty ? empty : page;
			continue;
		}
		const uint32_t first = find_run(page, count);
		if (first < PAGE_BLOCKS) {
			mark(page, first, count, true);
			return page->memory + first * BLOCK_BYTES / 4;
		}
	}
	*full = !empty;
	if (!empty)
		return NULL;
	empty->memory = xsdk_gpu_alloc(PAGE_BYTES);
	if (!empty->memory)
		return NULL;
	mark(empty, 0, count, true);
	return empty->memory;
}

volatile uint32_t *xsdk_microcode_alloc(uint32_t size_bytes)
{
	const uint32_t count = blocks_for(size_bytes);
	if (count == 0 || count > PAGE_BLOCKS)
		return xsdk_gpu_alloc(size_bytes);
	bool full = false;
	RtlEnterCriticalSection(&pages_lock);
	volatile uint32_t *const memory = take_blocks(count, &full);
	RtlLeaveCriticalSection(&pages_lock);
	return full ? xsdk_gpu_alloc(size_bytes) : memory;
}

void xsdk_microcode_free(volatile uint32_t *memory, uint32_t size_bytes)
{
	if (!memory)
		return;
	const uintptr_t base = (uintptr_t)memory & ~(uintptr_t)(PAGE_BYTES - 1);
	bool pooled = false;
	volatile uint32_t *emptied = NULL;
	RtlEnterCriticalSection(&pages_lock);
	for (Page *page = pages; page < pages + MAX_PAGES; page++) {
		if ((uintptr_t)page->memory != base || !page->memory)
			continue;
		pooled = true;
		mark(page, (uint32_t)((uintptr_t)memory - base) / BLOCK_BYTES, blocks_for(size_bytes), false);
		if (page->used_count == 0) {
			emptied = page->memory;
			*page = (Page){ .memory = NULL };
		}
		break;
	}
	RtlLeaveCriticalSection(&pages_lock);
	// Microcode past a page, or past the pool, had pages of its own.
	xsdk_gpu_free(pooled ? emptied : memory);
}
