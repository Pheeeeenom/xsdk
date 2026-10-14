"""patch_xsdk_rectangles.py <xsdk>: perf step 1's diagnostics in xsdk's resolve rectangles (src/gpu.c, rectangle_slot).
Temporary, never committed; put src/gpu.c back with git checkout and rebuild libxsdk after.

xsdk_diag_take_rectangles fills 4 counters (hits, evictions, waits for the GPU, ticks waited at 49.875 MHz) and up to
16 distinct sizes resolved since the last call (width << 16 | height), and zeroes them; it returns the number of sizes.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1]) / "src/gpu.c"
raw = p.read_bytes().decode()
crlf = "\r\n" in raw
s = raw.replace("\r\n", "\n")


def sub(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)


sub("""// The slot for this size, else the one used longest ago, after the GPU is done with it.
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
""", """static uint64_t diag_rectangles[4];
static uint32_t diag_sizes[16];
static uint32_t diag_size_count;

uint32_t xsdk_diag_take_rectangles(uint64_t *counters, uint32_t *sizes);
uint32_t xsdk_diag_take_rectangles(uint64_t *counters, uint32_t *sizes)
{
	memcpy(counters, diag_rectangles, sizeof diag_rectangles);
	memcpy(sizes, diag_sizes, sizeof diag_sizes);
	const uint32_t count = diag_size_count;
	memset(diag_rectangles, 0, sizeof diag_rectangles);
	diag_size_count = 0;
	return count;
}

static void diag_note_size(uint32_t width, uint32_t height)
{
	const uint32_t size = width << 16 | height;
	for (uint32_t i = 0; i < diag_size_count; i++) {
		if (diag_sizes[i] == size)
			return;
	}
	if (diag_size_count < 16)
		diag_sizes[diag_size_count++] = size;
}

// The slot for this size, else the one used longest ago, after the GPU is done with it.
static uint32_t rectangle_slot(uint32_t width, uint32_t height)
{
	diag_note_size(width, height);
	uint32_t oldest = 0;
	for (uint32_t i = 0; i < RECTANGLE_SLOTS; i++) {
		if (rectangles[i].width == width && rectangles[i].height == height) {
			diag_rectangles[0]++;
			return i;
		}
		if (fence_before(rectangles[i].fence, rectangles[oldest].fence))
			oldest = i;
	}
	diag_rectangles[1]++;
	Rectangle *const rectangle = &rectangles[oldest];
	if (rectangle->width) {
		if (fence_before(last_fence, rectangle->fence))
			(void)xsdk_gpu_fence();
		if (!xsdk_gpu_fence_done(rectangle->fence)) {
			const uint64_t diag_start = __builtin_ppc_get_timebase();
			(void)xsdk_gpu_wait(rectangle->fence, CLOSE_TIMEOUT_MS);
			diag_rectangles[2]++;
			diag_rectangles[3] += __builtin_ppc_get_timebase() - diag_start;
		}
	}
""")

p.write_bytes((s.replace("\n", "\r\n") if crlf else s).encode())
print("patched", "crlf" if crlf else "lf")
