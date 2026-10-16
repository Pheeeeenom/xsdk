#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "tests.h"

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// MmQueryStatistics counts 4 KB pages (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:496).
enum {
	PAGE_BYTES = 4096,
	GROWTH_BYTES = 16 * 1024 * 1024,
};

static NTSTATUS query(MM_QUERY_STATISTICS_RESULT *statistics, uint32_t size_bytes)
{
	*statistics = (MM_QUERY_STATISTICS_RESULT){ .size = size_bytes };
	return MmQueryStatistics(statistics);
}

static void log_section(const char *name, const MM_QUERY_STATISTICS_SECTION *section)
{
	xsdk_log_write("memory: %s: %u pages free; %u MB virtual, %u MB reserved", name, (unsigned)section->available_pages,
		(unsigned)(section->total_virtual_memory_bytes >> 20),
		(unsigned)(section->reserved_virtual_memory_bytes >> 20));
	xsdk_log_write("memory: %s pages: %u physical, %u pool, %u stack, %u image, %u heap", name,
		(unsigned)section->physical_pages, (unsigned)section->pool_pages, (unsigned)section->stack_pages,
		(unsigned)section->image_pages, (unsigned)section->heap_pages);
	xsdk_log_write("memory: %s pages: %u virtual, %u page table, %u cache", name, (unsigned)section->virtual_pages,
		(unsigned)section->page_table_pages, (unsigned)section->cache_pages);
}

static void log_statistics(const MM_QUERY_STATISTICS_RESULT *statistics)
{
	xsdk_log_write("memory: %u pages in all, %u the kernel's, highest page 0x%X",
		(unsigned)statistics->total_physical_pages, (unsigned)statistics->kernel_pages,
		(unsigned)statistics->highest_physical_page);
	log_section("title", &statistics->title);
	log_section("system", &statistics->system);
	unsigned free_mb = statistics->title.available_pages / (1048576 / PAGE_BYTES);
	xsdk_log_write("memory: %u MB free for the title", free_mb);
}

// Grows the heap, touching every page, and returns the free pages the kernel then reports.
static uint32_t available_after_growth(void)
{
	volatile uint8_t *grown = sbrk(GROWTH_BYTES);
	if (grown == (void *)-1) {
		xsdk_log_write("memory: sbrk(%u) failed", (unsigned)GROWTH_BYTES);
		return 0;
	}
	for (size_t offset = 0; offset < GROWTH_BYTES; offset += PAGE_BYTES)
		grown[offset] = 1;
	MM_QUERY_STATISTICS_RESULT statistics;
	NTSTATUS status = query(&statistics, sizeof(statistics));
	(void)sbrk(-GROWTH_BYTES);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("memory: MmQueryStatistics failed: 0x%08X", (unsigned)status);
		return 0;
	}
	return statistics.title.available_pages;
}

void test_memory(void)
{
	MM_QUERY_STATISTICS_RESULT statistics;
	check("MmQueryStatistics: a wrong size refused", !NT_SUCCESS(query(&statistics, sizeof(statistics) - 4)));

	NTSTATUS status = query(&statistics, sizeof(statistics));
	check("MmQueryStatistics: answers", NT_SUCCESS(status) && statistics.size == sizeof(statistics));
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("memory: MmQueryStatistics failed: 0x%08X", (unsigned)status);
		return;
	}
	log_statistics(&statistics);
	check("MmQueryStatistics: the title's free pages lie within the total",
		statistics.title.available_pages > 0 && statistics.title.available_pages <= statistics.total_physical_pages);
	check("sysconf: the physical pages and their size are the console's",
		sysconf(_SC_PAGESIZE) == PAGE_BYTES && sysconf(_SC_PHYS_PAGES) == (long)statistics.total_physical_pages);
	// sbrk reserved the title's free memory at the heap's first growth, before main, and free memory has only shrunk.
	check("sbrk: the heap's reservation covers the title's free memory",
		statistics.title.reserved_virtual_memory_bytes >= statistics.title.available_pages * PAGE_BYTES);

	uint32_t before = statistics.title.available_pages;
	uint32_t after = available_after_growth();
	xsdk_log_write("memory: %u pages free before %u MB of heap growth, %u after", (unsigned)before,
		(unsigned)(GROWTH_BYTES >> 20), (unsigned)after);
	check("MmQueryStatistics: heap growth takes free pages",
		after != 0 && after < before && before - after >= GROWTH_BYTES / PAGE_BYTES);
}
