"""patch_v4x.py <vulkan4xenon>: diagnostics in the layer's replay (src/queue.c). Temporary, never committed.

- Each texture upload's wait for the GPU and its CPU copy (step 5's second run): v4x_diag_take_uploads.
- Where the replay's time goes, per frame (perf step 1, 2026-10-13): v4x_diag_take_replay fills REPLAY_COUNTERS
  counters, counts and time-base ticks (49.875 MHz on the console), and zeroes them. See the enum below.
"""
import sys
from pathlib import Path

p = Path(sys.argv[1]) / "src/queue.c"
raw = p.read_bytes().decode()
crlf = "\r\n" in raw
s = raw.replace("\r\n", "\n")


def sub(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)


sub("""#include <string.h>
""", """#include <string.h>
#include <time.h>
""")

# The replay's counters, after the block buffer.
sub("""static float block[V4X_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));
""", """static float block[V4X_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));

// Perf step 1's diagnostics: what the replay does and where its time goes.
enum {
	DIAG_COMMANDS,
	DIAG_DRAWS,
	DIAG_DRAW_PACKETS,
	DIAG_PIPELINE_APPLIES,
	DIAG_VIEWPORT_APPLIES,
	DIAG_UNIFORMS,
	DIAG_UNIFORM_LOADS,
	DIAG_FALLBACK_FIRST,
	DIAG_FALLBACK_ADDRESS,
	DIAG_FALLBACK_RANGE,
	DIAG_FALLBACK_WORDS,
	DIAG_PUSH_WRITES,
	DIAG_PUSH_REGISTERS,
	DIAG_LITERALS,
	DIAG_TEXTURE_SETS,
	DIAG_TEXTURE_SKIPS,
	DIAG_STREAM_SETS,
	DIAG_TICKS_REPLAY,
	DIAG_TICKS_BEGIN_PASS,
	DIAG_TICKS_END_PASS,
	DIAG_TICKS_PIPELINE,
	DIAG_TICKS_FALLBACKS,
	DIAG_TICKS_LOADS,
	DIAG_TICKS_PUSH,
	DIAG_TICKS_TEXTURES,
	DIAG_TICKS_STREAMS,
	DIAG_TICKS_PACKETS,
	DIAG_TICKS_UPLOADS,
	DIAG_TICKS_OTHER,
	DIAG_TICKS_INVALIDATE,
	REPLAY_COUNTERS = 32,
};
static uint64_t diag_replay[REPLAY_COUNTERS];

static inline uint64_t diag_ticks(void)
{
	return __builtin_ppc_get_timebase();
}

void v4x_diag_take_replay(uint64_t *counters);
void v4x_diag_take_replay(uint64_t *counters)
{
	memcpy(counters, diag_replay, sizeof diag_replay);
	memset(diag_replay, 0, sizeof diag_replay);
}

static bool diag_tiny_scissor;
// Every draw's scissor cut to this many quarters of its width and height, about what that resolution would draw; 0 is
// off.
static uint32_t diag_scissor_quarters;

void v4x_diag_set_tiny_scissor(bool tiny);
void v4x_diag_set_tiny_scissor(bool tiny)
{
	diag_tiny_scissor = tiny;
}

void v4x_diag_set_scissor_quarters(uint32_t quarters);
void v4x_diag_set_scissor_quarters(uint32_t quarters)
{
	diag_scissor_quarters = quarters;
}
""")

sub("""static void write_literals(const V4xShader *shader, bool vertex)
{
""", """static void write_literals(const V4xShader *shader, bool vertex)
{
	diag_replay[DIAG_LITERALS] += shader->literal_count;
""")

# Every draw scissored to one pixel while set: the vertex work stays, the pixel work goes.
sub("""	const VkRect2D *const s = pipeline->dynamic_scissor ? &replay->scissor : &pipeline->scissor;
	xsdk_gpu_set_scissor((uint32_t)s->offset.x, (uint32_t)s->offset.y, s->extent.width, s->extent.height);
}""", """	const VkRect2D *const s = pipeline->dynamic_scissor ? &replay->scissor : &pipeline->scissor;
	if (diag_tiny_scissor)
		xsdk_gpu_set_scissor(0, 0, 1, 1);
	else if (diag_scissor_quarters)
		xsdk_gpu_set_scissor((uint32_t)s->offset.x, (uint32_t)s->offset.y, s->extent.width * diag_scissor_quarters / 4,
			s->extent.height * diag_scissor_quarters / 4);
	else
		xsdk_gpu_set_scissor((uint32_t)s->offset.x, (uint32_t)s->offset.y, s->extent.width, s->extent.height);
}""")

sub("""	if (resource->first % CONSTANT_GROUP || address % LOAD_ALIGNMENT_BYTES ||
		descriptor->range_bytes < resource->count * 16)
		return false;
""", """	if (resource->first % CONSTANT_GROUP) {
		diag_replay[DIAG_FALLBACK_FIRST]++;
		return false;
	}
	if (address % LOAD_ALIGNMENT_BYTES) {
		diag_replay[DIAG_FALLBACK_ADDRESS]++;
		return false;
	}
	if (descriptor->range_bytes < resource->count * 16) {
		diag_replay[DIAG_FALLBACK_RANGE]++;
		return false;
	}
	diag_replay[DIAG_UNIFORM_LOADS]++;
""")

sub("""	if (pointed->texture == descriptor->texture && pointed->sampler == descriptor->sampler)
		return;
	xsdk_gpu_set_texture(""", """	if (pointed->texture == descriptor->texture && pointed->sampler == descriptor->sampler) {
		diag_replay[DIAG_TEXTURE_SKIPS]++;
		return;
	}
	diag_replay[DIAG_TEXTURE_SETS]++;
	xsdk_gpu_set_texture(""")

sub("""static void apply_resources(const Pipeline *pipeline, Replay *replay)
{
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		const bool vertex = pipeline->resources[i].vertex;
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			write_constants(vertex, resource->first, replay->push, resource->count);
			continue;
		}
		if (!replay->sets[resource->set])
			continue;
		if (resource->kind == V4X_RESOURCE_UNIFORM_BUFFER) {
			if (!load_uniform_buffer(replay, resource, vertex)) {
				read_uniform_buffer(replay, resource);
				write_constants(vertex, resource->first, block, resource->count);
			}
			continue;
		}
		point_texture(replay, resource->first, &replay->sets[resource->set]->descriptors[resource->binding]);
	}
}""", """static void apply_resources(const Pipeline *pipeline, Replay *replay)
{
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		const bool vertex = pipeline->resources[i].vertex;
		const uint64_t diag_start = diag_ticks();
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			write_constants(vertex, resource->first, replay->push, resource->count);
			diag_replay[DIAG_PUSH_WRITES]++;
			diag_replay[DIAG_PUSH_REGISTERS] += resource->count;
			diag_replay[DIAG_TICKS_PUSH] += diag_ticks() - diag_start;
			continue;
		}
		if (!replay->sets[resource->set])
			continue;
		if (resource->kind == V4X_RESOURCE_UNIFORM_BUFFER) {
			diag_replay[DIAG_UNIFORMS]++;
			if (!load_uniform_buffer(replay, resource, vertex)) {
				read_uniform_buffer(replay, resource);
				write_constants(vertex, resource->first, block, resource->count);
				diag_replay[DIAG_FALLBACK_WORDS] += resource->count * 4;
				diag_replay[DIAG_TICKS_FALLBACKS] += diag_ticks() - diag_start;
			} else {
				diag_replay[DIAG_TICKS_LOADS] += diag_ticks() - diag_start;
			}
			continue;
		}
		point_texture(replay, resource->first, &replay->sets[resource->set]->descriptors[resource->binding]);
		diag_replay[DIAG_TICKS_TEXTURES] += diag_ticks() - diag_start;
	}
}""")

# Uploads, as step 5's second run had them.
sub("""static void copy_buffer_to_image(VkQueue queue, const Command *command)
{
	wait_for_image(queue, command->copy.image);
	xsdk_texture_upload(&command->copy.image->texture, command->copy.level,
		(const void *)(uintptr_t)command->copy.source, command->copy.row_bytes);
""", """// Step 5's second diagnostics run: uploads, their waits for the GPU and their CPU copies.
static uint64_t diag_uploads, diag_wait_ns, diag_copy_ns;

static uint64_t diag_now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

// Since the last call: uploads replayed, and the nanoseconds waiting for the GPU and copying texels.
uint64_t v4x_diag_take_uploads(uint64_t *wait_ns, uint64_t *copy_ns);
uint64_t v4x_diag_take_uploads(uint64_t *wait_ns, uint64_t *copy_ns)
{
	const uint64_t uploads = diag_uploads;
	*wait_ns = diag_wait_ns;
	*copy_ns = diag_copy_ns;
	diag_uploads = diag_wait_ns = diag_copy_ns = 0;
	return uploads;
}

static void copy_buffer_to_image(VkQueue queue, const Command *command)
{
	const uint64_t diag_start = diag_now_ns();
	wait_for_image(queue, command->copy.image);
	const uint64_t diag_waited = diag_now_ns();
	xsdk_texture_upload(&command->copy.image->texture, command->copy.level,
		(const void *)(uintptr_t)command->copy.source, command->copy.row_bytes);
	diag_uploads++;
	diag_wait_ns += diag_waited - diag_start;
	diag_copy_ns += diag_now_ns() - diag_waited;
""")

sub("""static void prepare_draw(Replay *replay)
{
	const Pipeline *const pipeline = replay->pipeline;
	if (replay->pipeline_dirty)
		apply_pipeline(pipeline, replay);
	else if (replay->viewport_dirty)
		apply_viewport(pipeline, replay);
""", """static void prepare_draw(Replay *replay)
{
	const Pipeline *const pipeline = replay->pipeline;
	const uint64_t diag_start = diag_ticks();
	diag_replay[DIAG_DRAWS]++;
	if (replay->pipeline_dirty) {
		apply_pipeline(pipeline, replay);
		diag_replay[DIAG_PIPELINE_APPLIES]++;
	} else if (replay->viewport_dirty) {
		apply_viewport(pipeline, replay);
		diag_replay[DIAG_VIEWPORT_APPLIES]++;
	}
	diag_replay[DIAG_TICKS_PIPELINE] += diag_ticks() - diag_start;
""")

sub("""		if (replay->pointed_streams[s].address == stream.address &&
			replay->pointed_streams[s].size_bytes == stream.size_bytes)
			continue;
		xsdk_gpu_set_vertices(""", """		if (replay->pointed_streams[s].address == stream.address &&
			replay->pointed_streams[s].size_bytes == stream.size_bytes)
			continue;
		diag_replay[DIAG_STREAM_SETS]++;
		xsdk_gpu_set_vertices(""")

sub("""	for (uint32_t done = 0; done < vertex_count; done += chunk) {
		point_streams(replay, (int32_t)(first_vertex + done));
		const uint32_t left = vertex_count - done;
		xsdk_gpu_draw(replay->pipeline->primitive, left < chunk ? left : chunk);
	}""", """	for (uint32_t done = 0; done < vertex_count; done += chunk) {
		const uint64_t diag_start = diag_ticks();
		point_streams(replay, (int32_t)(first_vertex + done));
		const uint64_t diag_pointed = diag_ticks();
		const uint32_t left = vertex_count - done;
		xsdk_gpu_draw(replay->pipeline->primitive, left < chunk ? left : chunk);
		diag_replay[DIAG_DRAW_PACKETS]++;
		diag_replay[DIAG_TICKS_STREAMS] += diag_pointed - diag_start;
		diag_replay[DIAG_TICKS_PACKETS] += diag_ticks() - diag_pointed;
	}""")

sub("""	prepare_draw(replay);
	point_streams(replay, command->draw_indexed.vertex_offset);
	const uint32_t index_bytes""", """	prepare_draw(replay);
	const uint64_t diag_start = diag_ticks();
	point_streams(replay, command->draw_indexed.vertex_offset);
	diag_replay[DIAG_TICKS_STREAMS] += diag_ticks() - diag_start;
	const uint32_t index_bytes""")

sub("""		const uint32_t address = replay->index_address + (command->draw_indexed.first_index + done) * index_bytes;
		const uint32_t left = count - done;
		xsdk_gpu_draw_indexed(replay->pipeline->primitive, address, left < chunk ? left : chunk, replay->index_32_bit);
""", """		const uint32_t address = replay->index_address + (command->draw_indexed.first_index + done) * index_bytes;
		const uint32_t left = count - done;
		const uint64_t diag_packet = diag_ticks();
		xsdk_gpu_draw_indexed(replay->pipeline->primitive, address, left < chunk ? left : chunk, replay->index_32_bit);
		diag_replay[DIAG_DRAW_PACKETS]++;
		diag_replay[DIAG_TICKS_PACKETS] += diag_ticks() - diag_packet;
""")

# Each command's time, by kind; draws' time is split above.
sub("""static void replay_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	const Command *const command = &buffer->commands[index];
	switch (command->type) {""", """static void replay_command_diag(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index);

static void replay_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	const Command *const command = &buffer->commands[index];
	const uint64_t diag_start = diag_ticks();
	replay_command_diag(queue, buffer, replay, index);
	const uint64_t diag_took = diag_ticks() - diag_start;
	diag_replay[DIAG_COMMANDS]++;
	diag_replay[DIAG_TICKS_REPLAY] += diag_took;
	switch (command->type) {
	case COMMAND_BEGIN_RENDER_PASS:
		diag_replay[DIAG_TICKS_BEGIN_PASS] += diag_took;
		break;
	case COMMAND_END_RENDER_PASS:
		diag_replay[DIAG_TICKS_END_PASS] += diag_took;
		break;
	case COMMAND_COPY_BUFFER_TO_IMAGE:
		diag_replay[DIAG_TICKS_UPLOADS] += diag_took;
		break;
	case COMMAND_DRAW:
	case COMMAND_DRAW_INDEXED:
		break;
	default:
		diag_replay[DIAG_TICKS_OTHER] += diag_took;
		break;
	}
}

static void replay_command_diag(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	const Command *const command = &buffer->commands[index];
	switch (command->type) {""")

sub("""	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
""", """	const uint64_t diag_start = diag_ticks();
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
	diag_replay[DIAG_TICKS_INVALIDATE] += diag_ticks() - diag_start;
""")

p.write_bytes((s.replace("\n", "\r\n") if crlf else s).encode())
print("patched", "crlf" if crlf else "lf")
