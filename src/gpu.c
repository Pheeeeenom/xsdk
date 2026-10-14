#include "xsdk/gpu.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "gpu_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	RING_BYTES = 0x8000,
	RING_DWORDS = RING_BYTES / 4,
	// log2 of RING_BYTES in 8-byte units (libxenon:drivers/xenos/xe.c:21, 366).
	RING_SIZE_LOG2 = 12,
	// libxenon's read pointer write-back block size (xe.c:981).
	RPTR_BLOCK_SIZE_LOG2 = 6,
	PAGE_BYTES = 0x10000,
	// What D3D passes first (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:196).
	ENGINES_FLAGS = 0x4F810000,
	CLOSE_TIMEOUT_MS = 1000,
	SET_UP_TIMEOUT_MS = 1000,
	// A NOP with one dword after it, the smallest packet that can fill the end of the ring.
	FILLER_DWORDS = 2,
	// Commands go into a stream, which the ring runs a segment at a time, as libxenon does
	// (libxenon:drivers/xenos/xe.c:21-23, 108-147).
	STREAM_BYTES = 0x400000,
	STREAM_DWORDS = STREAM_BYTES / 4,
	SEGMENT_DWORDS = 1024,
	// The stream is reused a block at a time, once the GPU is past a fence written after the block.
	STREAM_BLOCKS = 64,
	BLOCK_DWORDS = STREAM_DWORDS / STREAM_BLOCKS,
	FENCE_DWORDS = 4,
	SEGMENT_PACKET_DWORDS = 16,
	// Each segment starts on a 128-byte line, as libxenon's do (libxenon:drivers/xenos/xe.c:123).
	SEGMENT_ALIGN_DWORDS = 32,
	// A one-dword packet that does nothing (libxenon:drivers/xenos/xe.c:1217-1221).
	TYPE2_NOP = 0x80000000,
	RECTANGLE_FLOATS = 6,
	// Resolve sizes kept at once, so a frame resolving at a few sizes doesn't wait for the GPU.
	RECTANGLE_SLOTS = 8,
	RECTANGLE_STRIDE = 0x20,
	// Room for the largest packet a call reserves, as the ring has.
	OVERFLOW_SCRATCH_DWORDS = RING_DWORDS,
	// An indirect buffer's length is 20 bits (xenia:src/xenia/gpu/command_processor.cc:947-949).
	INDIRECT_BUFFER_MAX_DWORDS = 0xFFFFF,
	MICROSECONDS_PER_SECOND = 1000000,
	NANOSECONDS_PER_MICROSECOND = 1000,
	MICROSECONDS_PER_MILLISECOND = 1000,
	// Looks read memory the GPU shares: without a break they slowed a resolve 4%, every 5 us 0.1% (docs/HARDWARE.md).
	WAIT_LOOK_US = 5,
};

// Byte offsets in the write-back page.
enum {
	RPTR_OFFSET = 0x00,
	FENCE_OFFSET = 0x20,
	RECTANGLE_OFFSET = 0x40,
	// The stream's own fences, apart from the title's.
	BLOCK_FENCE_OFFSET = 0x200,
};

enum {
	// The cache-flush events wedge the GPU before render state is set; this one lands (docs/HARDWARE.md).
	PS_DONE_TS = 3,
	// An address's low bits pick a byte swap; 2 swaps each dword's bytes (xenia:src/xenia/gpu/xenos.h:193-197).
	SWAP_8IN32 = 2,
};

// Volatile, so every store stays 32 bits: a merged 64-bit one can fault here (docs/HARDWARE.md).
static volatile uint32_t *ring;
static volatile uint32_t *writeback;
static uint32_t writeback_physical;
static uint32_t write_index;
static uint32_t last_fence;
static XsdkGpuStats ring_stats;

// Write-combined, as recorded command buffers are, which the console runs (docs/HARDWARE.md).
static volatile uint32_t *stream;
static uint32_t stream_physical;
static uint32_t stream_write;
// Where the part of the stream the ring hasn't been given starts.
static uint32_t segment_start;
static uint32_t block;
static uint32_t block_fences[STREAM_BLOCKS];
static uint32_t last_block_fence;
// While xsdk_gpu_open sets the GPU up, packets go straight into the ring: the console never finished the set-up from
// the stream (docs/HARDWARE.md).
static bool direct;

typedef struct {
	uint32_t width;
	uint32_t height;
	// The fence after which no queued resolve reads the corners.
	uint32_t fence;
} Rectangle;

static Rectangle rectangles[RECTANGLE_SLOTS];

// The buffer being recorded into, and where packets past its end go to be dropped.
static XsdkCommandBuffer *recording;
static bool recording_overflowed;
static uint32_t overflow_scratch[OVERFLOW_SCRATCH_DWORDS];

// How long a wait looks at its fence before it sleeps; none until set, as a 2 ms spin cost a title's thread sharing
// its core 5% of its frame (docs/HARDWARE.md).
static uint32_t wait_spin_us;

// VdShutdownEngines calls it (docs/HARDWARE.md); the SDK has nothing to clean up there.
static void engines_shut_down(void)
{
}

static volatile uint32_t *allocate(uint32_t size_bytes, uint32_t protect)
{
	const uint32_t rounded = (size_bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const memory =
		MmAllocatePhysicalMemoryEx(0, rounded, PAGE_READWRITE | protect | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!memory)
		xsdk_log_write("gpu: MmAllocatePhysicalMemoryEx(%u bytes) failed", (unsigned)rounded);
	return memory;
}

static void free_memory(void)
{
	xsdk_gpu_free(stream);
	xsdk_gpu_free(writeback);
	xsdk_gpu_free(ring);
	stream = NULL;
	writeback = NULL;
	ring = NULL;
}

// Dwords the ring can take before it reaches what the GPU hasn't read.
static uint32_t free_dwords(void)
{
	return (writeback[RPTR_OFFSET / 4] - write_index - 1) & (RING_DWORDS - 1);
}

static void kick(void)
{
	// Drain the write-combined stores before the GPU reads them (libxenon:drivers/xenos/xe.c:97-106).
	__asm__ volatile("sync" ::: "memory");
	gpu_registers[CP_RB_WPTR] = write_index;
	ring_stats.kicks++;
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

// Spins, as the GPU frees room far sooner than a 1 ms sleep returns.
static void wait_for_room(uint32_t dword_count)
{
	if (free_dwords() >= dword_count)
		return;
	const uint64_t start = now_us();
	ring_stats.waits++;
	kick();
	while (free_dwords() < dword_count) {
	}
	ring_stats.waited_us += now_us() - start;
}

// Room in the ring itself, which runs the stream's segments and recorded buffers.
static volatile uint32_t *ring_reserve(uint32_t dword_count)
{
	// Keep room at the end for a NOP, so the ring can always wrap.
	if (write_index + dword_count > RING_DWORDS - FILLER_DWORDS) {
		const uint32_t filler_dwords = RING_DWORDS - write_index;
		wait_for_room(filler_dwords);
		ring[write_index] = xsdk_gpu_type3(PM4_NOP, filler_dwords - 1);
		write_index = 0;
	}
	wait_for_room(dword_count);
	volatile uint32_t *const packets = &ring[write_index];
	write_index += dword_count;
	return packets;
}

// The ring runs the stream written since the last segment, once the vertex cache drops that memory, as libxenon's
// does (libxenon:drivers/xenos/xe.c:83-95, 108-115); then it's kicked, with whatever else it holds.
static void submit_segment(void)
{
	const uint32_t count = stream_write - segment_start;
	if (!count) {
		kick();
		return;
	}
	const uint32_t address = stream_physical + segment_start * 4;
	uint32_t dwords[SEGMENT_PACKET_DWORDS];
	GpuPackets packets = { .dwords = dwords, .count = 0, .capacity = SEGMENT_PACKET_DWORDS };
	xsdk_gpu_put_coherency(&packets, COHER_VERTEX_CACHE, address, count * 4);
	gpu_put_type3(&packets, PM4_INDIRECT_BUFFER, 2);
	gpu_put(&packets, address);
	gpu_put(&packets, count);
	volatile uint32_t *const space = ring_reserve(packets.count);
	for (uint32_t i = 0; i < packets.count; i++)
		space[i] = packets.dwords[i];
	stream_write = (stream_write + SEGMENT_ALIGN_DWORDS - 1) & ~(uint32_t)(SEGMENT_ALIGN_DWORDS - 1);
	segment_start = stream_write;
	kick();
}

static bool block_free(uint32_t index)
{
	return (int32_t)(writeback[BLOCK_FENCE_OFFSET / 4] - block_fences[index]) >= 0;
}

// Pads the block out, so no packet spans two.
static void pad_block(void)
{
	const uint32_t left = (block + 1) * BLOCK_DWORDS - stream_write;
	if (left == 1)
		stream[stream_write] = TYPE2_NOP;
	else if (left > 1)
		stream[stream_write] = xsdk_gpu_type3(PM4_NOP, left - 1);
	stream_write += left;
	ring_stats.dwords += left;
}

// Moves into the stream's next block once the GPU is past what it last held, and fences the block left behind.
static void next_block(void)
{
	pad_block();
	const uint32_t left = block;
	block = (block + 1) % STREAM_BLOCKS;
	if (!block) {
		submit_segment();
		stream_write = 0;
		segment_start = 0;
	}
	if (!block_free(block)) {
		const uint64_t start = now_us();
		ring_stats.waits++;
		submit_segment();
		while (!block_free(block)) {
		}
		ring_stats.waited_us += now_us() - start;
	}
	volatile uint32_t *const packet = &stream[stream_write];
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_SHD, 3);
	packet[1] = PS_DONE_TS;
	packet[2] = (writeback_physical + BLOCK_FENCE_OFFSET) | SWAP_8IN32;
	packet[3] = ++last_block_fence;
	block_fences[left] = last_block_fence;
	stream_write += FENCE_DWORDS;
	ring_stats.dwords += FENCE_DWORDS;
}

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes)
{
	return allocate(size_bytes, PAGE_WRITECOMBINE);
}

void xsdk_gpu_free(volatile uint32_t *memory)
{
	if (memory)
		MmFreePhysicalMemory(0, (void *)memory);
}

uint32_t xsdk_gpu_address(volatile const uint32_t *memory)
{
	return MmGetPhysicalAddress((const void *)memory);
}

NTSTATUS xsdk_gpu_open(void)
{
	assert(!ring);
	// Write-combined, as titles allocate the ring (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:235-238).
	ring = allocate(RING_BYTES, PAGE_WRITECOMBINE);
	// Uncached, so the CPU sees what the GPU writes back.
	writeback = allocate(PAGE_BYTES, PAGE_NOCACHE);
	// 32-bit stores into write-combined memory outran cached memory flushed with dcbst (docs/HARDWARE.md).
	stream = allocate(STREAM_BYTES, PAGE_WRITECOMBINE);
	if (!ring || !writeback || !stream) {
		free_memory();
		return STATUS_NO_MEMORY;
	}
	writeback_physical = MmGetPhysicalAddress((const void *)writeback);
	writeback[RPTR_OFFSET / 4] = 0;
	writeback[FENCE_OFFSET / 4] = 0;
	writeback[BLOCK_FENCE_OFFSET / 4] = 0;
	write_index = 0;
	last_fence = 0;
	stream_physical = MmGetPhysicalAddress((const void *)stream);
	stream_write = 0;
	segment_start = 0;
	block = 0;
	memset(block_fences, 0, sizeof block_fences);
	last_block_fence = 0;
	memset(&ring_stats, 0, sizeof ring_stats);
	memset(rectangles, 0, sizeof rectangles);
	xsdk_gpu_hiz_drop();

	// No microcode: the kernel keeps the command processor's own (docs/HARDWARE.md).
	const uint32_t engines = VdInitializeEngines(ENGINES_FLAGS, (void *)engines_shut_down, NULL, NULL, NULL);
	if (!engines) {
		xsdk_log_write("gpu: VdInitializeEngines failed: 0x%08X", (unsigned)engines);
		free_memory();
		return STATUS_UNSUCCESSFUL;
	}
	VdInitializeRingBuffer(MmGetPhysicalAddress((const void *)ring), RING_SIZE_LOG2);
	VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_OFFSET, RPTR_BLOCK_SIZE_LOG2);

	// Without libxenon's set-up, only every third resolve writes (docs/HARDWARE.md).
	direct = true;
	xsdk_gpu_set_up();
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), SET_UP_TIMEOUT_MS);
	direct = false;
	if (!NT_SUCCESS(status))
		xsdk_log_write("gpu: the set-up didn't finish: 0x%08X", (unsigned)status);
	return status;
}

NTSTATUS xsdk_gpu_close(void)
{
	if (!ring)
		return STATUS_SUCCESS;
	xsdk_gpu_put_back();
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), CLOSE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		// VdShutdownEngines never returns while the GPU is wedged, and the GPU may still read the ring.
		xsdk_log_write("gpu: the GPU didn't finish: 0x%08X; keeping it and its memory", (unsigned)status);
		ring = NULL;
		writeback = NULL;
		stream = NULL;
		return status;
	}
	VdShutdownEngines();
	free_memory();
	return STATUS_SUCCESS;
}

static volatile uint32_t *reserve_recorded(uint32_t dword_count)
{
	assert(dword_count <= OVERFLOW_SCRATCH_DWORDS);
	if (recording->count + dword_count > recording->capacity_dwords) {
		recording_overflowed = true;
		return overflow_scratch;
	}
	volatile uint32_t *const packets = &recording->dwords[recording->count];
	recording->count += dword_count;
	return packets;
}

volatile uint32_t *xsdk_gpu_reserve(uint32_t dword_count)
{
	if (recording)
		return reserve_recorded(dword_count);
	if (direct)
		return ring_reserve(dword_count);
	assert(stream && dword_count <= OVERFLOW_SCRATCH_DWORDS);
	// Everything reserved before this call has been written, so it can run.
	if (stream_write - segment_start >= SEGMENT_DWORDS)
		submit_segment();
	if (stream_write + dword_count > (block + 1) * BLOCK_DWORDS)
		next_block();
	volatile uint32_t *const packets = &stream[stream_write];
	stream_write += dword_count;
	ring_stats.dwords += dword_count;
	return packets;
}

void xsdk_gpu_write(const GpuPackets *packets)
{
	volatile uint32_t *const space = xsdk_gpu_reserve(packets->count);
	for (uint32_t i = 0; i < packets->count; i++)
		space[i] = packets->dwords[i];
}

static bool fence_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

// The slot for this size, else the one used longest ago, after the GPU is done with it.
static uint32_t rectangle_slot(uint32_t width, uint32_t height)
{
	uint32_t oldest = 0;
	for (uint32_t i = 0; i < RECTANGLE_SLOTS; i++) {
		if (rectangles[i].width == width && rectangles[i].height == height)
			return i;
		if (fence_before(rectangles[i].fence, rectangles[oldest].fence))
			oldest = i;
	}
	Rectangle *const rectangle = &rectangles[oldest];
	if (rectangle->width) {
		if (fence_before(last_fence, rectangle->fence))
			(void)xsdk_gpu_fence();
		(void)xsdk_gpu_wait(rectangle->fence, CLOSE_TIMEOUT_MS);
	}
	// libxenon's three corners, half a pixel up and left (libxenon:drivers/xenos/xe.c:1074-1081).
	const float corners[RECTANGLE_FLOATS] = { -0.5f, -0.5f, width - 0.5f, 0, width - 0.5f, height - 0.5f };
	for (uint32_t i = 0; i < RECTANGLE_FLOATS; i++)
		writeback[(RECTANGLE_OFFSET + oldest * RECTANGLE_STRIDE) / 4 + i] = float_bits(corners[i]);
	rectangle->width = width;
	rectangle->height = height;
	return oldest;
}

uint32_t xsdk_gpu_rectangle(uint32_t width, uint32_t height)
{
	// A recorded resolve could outlive its slot.
	assert(!recording);
	const uint32_t slot = rectangle_slot(width, height);
	// The resolve about to use it is queued before the next fence.
	rectangles[slot].fence = last_fence + 1;
	return writeback_physical + RECTANGLE_OFFSET + slot * RECTANGLE_STRIDE;
}

uint32_t xsdk_gpu_fence(void)
{
	assert(!recording);
	const uint32_t fence = last_fence + 1;
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_SHD, 3);
	packet[1] = PS_DONE_TS;
	packet[2] = (writeback_physical + FENCE_OFFSET) | SWAP_8IN32;
	packet[3] = fence;
	last_fence = fence;
	submit_segment();
	return fence;
}

bool xsdk_gpu_fence_done(uint32_t fence)
{
	return (int32_t)(writeback[FENCE_OFFSET / 4] - fence) >= 0;
}

void xsdk_gpu_stats(XsdkGpuStats *stats)
{
	*stats = ring_stats;
}

void xsdk_gpu_set_wait_spin(uint32_t microseconds)
{
	wait_spin_us = microseconds;
}

NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms)
{
	const uint64_t timeout_us = (uint64_t)timeout_ms * MICROSECONDS_PER_MILLISECOND;
	uint64_t now = now_us();
	const uint64_t spin_end = now + (timeout_us < wait_spin_us ? timeout_us : wait_spin_us);
	while (now < spin_end) {
		if (xsdk_gpu_fence_done(fence))
			return STATUS_SUCCESS;
		const uint64_t next = now + WAIT_LOOK_US;
		while ((now = now_us()) < next) {
		}
	}
	for (uint32_t waited_ms = 0;; waited_ms++) {
		if (xsdk_gpu_fence_done(fence))
			return STATUS_SUCCESS;
		if (waited_ms == timeout_ms)
			return STATUS_IO_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
}

NTSTATUS xsdk_gpu_command_buffer_create(XsdkCommandBuffer *buffer, uint32_t capacity_dwords)
{
	if (!capacity_dwords || capacity_dwords > INDIRECT_BUFFER_MAX_DWORDS) {
		xsdk_log_write("gpu: command buffer size not from 1 to %u dwords", (unsigned)INDIRECT_BUFFER_MAX_DWORDS);
		return STATUS_INVALID_PARAMETER;
	}
	volatile uint32_t *const dwords = xsdk_gpu_alloc(capacity_dwords * 4);
	if (!dwords)
		return STATUS_NO_MEMORY;
	*buffer = (XsdkCommandBuffer){
		.dwords = dwords, .address = xsdk_gpu_address(dwords), .capacity_dwords = capacity_dwords, .count = 0
	};
	return STATUS_SUCCESS;
}

void xsdk_gpu_command_buffer_free(XsdkCommandBuffer *buffer)
{
	assert(recording != buffer);
	xsdk_gpu_free(buffer->dwords);
	*buffer = (XsdkCommandBuffer){ .dwords = NULL };
}

bool xsdk_gpu_recording(void)
{
	return recording != NULL;
}

void xsdk_gpu_record_begin(XsdkCommandBuffer *buffer)
{
	assert(!recording);
	buffer->count = 0;
	recording = buffer;
	recording_overflowed = false;
	// Recorded draws may run when the hierarchical buffer holds another target's depth, so they leave it off.
	xsdk_gpu_hiz_drop();
}

NTSTATUS xsdk_gpu_record_end(void)
{
	assert(recording);
	// Drain the write-combined stores before the GPU reads them (libxenon:drivers/xenos/xe.c:97-106).
	__asm__ volatile("sync" ::: "memory");
	XsdkCommandBuffer *const buffer = recording;
	recording = NULL;
	if (!recording_overflowed)
		return STATUS_SUCCESS;
	xsdk_log_write("gpu: the recording outgrew its %u-dword command buffer", (unsigned)buffer->capacity_dwords);
	buffer->count = 0;
	return STATUS_BUFFER_TOO_SMALL;
}

// The ring runs the buffer's packets, then carries on (xenia:src/xenia/gpu/command_processor.cc:943-953).
void xsdk_gpu_execute(const XsdkCommandBuffer *buffer)
{
	assert(!recording);
	if (!buffer->count)
		return;
	// The hierarchical buffer doesn't follow what it draws.
	xsdk_gpu_hiz_drop();
	// From the ring, after what the stream holds: the console hasn't run a buffer from inside another
	// (docs/HARDWARE.md).
	submit_segment();
	volatile uint32_t *const packet = ring_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_INDIRECT_BUFFER, 2);
	packet[1] = buffer->address;
	packet[2] = buffer->count;
}
