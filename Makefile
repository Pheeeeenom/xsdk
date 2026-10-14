# Builds the SDK library and the test title. Run inside WSL Debian.

# The toolchain tools/toolchain/build.sh builds.
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
CXX := $(TOOLCHAIN)g++
AR := $(TOOLCHAIN)ar
OBJDUMP := $(TOOLCHAIN)objdump
PYTHON ?= python3
HOST_CC ?= gcc

BUILD := build

WARNINGS := -Wall -Wextra -Werror
CFLAGS := -mcpu=cell -fno-pic -std=gnu17 -O2 -g $(WARNINGS) -Iinclude -MMD -MP
CXXFLAGS := -mcpu=cell -fno-pic -std=c++23 -O2 -g $(WARNINGS) -Iinclude -MMD -MP
HOST_CFLAGS := -std=gnu17 -O2 -g $(WARNINGS) -fPIC -Iinclude -MMD -MP
ASFLAGS := -mcpu=cell -Wa,-mregnames -Iinclude -MMD -MP
LDFLAGS := -static -nostdlib -T src/xsdk.ld
LIBS := -Wl,--start-group $(BUILD)/libxsdk.a -lstdc++ -lc -lm -lgcc -Wl,--end-group

SDK_SOURCES := src/start.c src/log.c src/sbrk.c src/thread.c src/file.c src/path.c src/dir.c \
	src/pthread.c src/lock.c src/reent.c src/time.c src/sysconf.c src/entropy.c src/exit.c src/gpu.c \
	src/gpu_setup.c src/gpu_resolve.c src/gpu_draw.c src/gpu_constants.c src/display.c src/shader.c src/microcode.c \
	src/texture.c src/texture_layout.c src/tiled.c src/present.c src/splash.c src/copy.c src/socket.c src/netdb.c
# xost's compiler, in the SDK for titles and built for the PC as libxsc.so for the shader tools.
XSC_SOURCES := src/xsc_memory.c src/xsc_spirv.c src/xsc_resolve.c src/xsc_unroll.c src/xsc_encode.c src/xsc_container.c \
	src/xsc_program.c src/xsc_optimize.c src/xsc_lower.c src/xsc_lower_block.c src/xsc_lower_matrix.c \
	src/xsc_lower_glsl.c src/xsc_lower_stage.c src/xsc_lower_vertex.c src/xsc_lower_fragment.c src/xsc_lower_loop.c \
	src/xsc_compile.c
SDK_SOURCES += $(XSC_SOURCES)
IMPORT_NAMES := $(shell $(PYTHON) tools/gen_imports.py --list imports/imports.txt)
IMPORT_SOURCES := $(IMPORT_NAMES:%=$(BUILD)/imports/%.S)
SDK_OBJECTS := $(patsubst src/%,$(BUILD)/sdk/%.o,$(basename $(SDK_SOURCES))) $(IMPORT_NAMES:%=$(BUILD)/imports/%.o)

TEST_SOURCES := tests/main.c tests/test_image.c tests/test_memory.c tests/test_threads.c tests/test_placement.c \
	tests/test_pthread.c tests/test_clocks.c tests/test_kernel_entry.c tests/kernel_entry_check.S tests/test_files.c \
	tests/test_input.c tests/test_audio.c tests/audio_frame.c tests/test_cpp_objects.cpp tests/test_cpp_threads.cpp \
	tests/test_cpp_atomics.cpp tests/test_cpp_exceptions.cpp tests/test_cpp_unwind.cpp tests/test_fp_align.cpp \
	tests/test_cpp_chrono.cpp tests/test_cpp_filesystem.cpp \
	tests/test_cpp_format.cpp tests/test_gpu.c tests/test_present.c tests/test_triangle.c tests/test_texture.c \
	tests/test_render.c tests/test_pacing.c tests/bar_scene.c tests/test_scale.c tests/test_constants.c \
	tests/test_indexed.c tests/test_depth.c tests/test_raster.c tests/test_sampling.c tests/test_anisotropy.c \
	tests/test_formats.c tests/test_vertex.c tests/test_replay.c tests/test_registers.c tests/test_copy.c \
	tests/test_microcode.c tests/test_multisample.c tests/test_hiz.c tests/test_net.c tests/test_interrupts.c \
	tests/test_xsc.c
TEST_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(TEST_SOURCES)))
# A title for listening to the audio frame layout on the console.
AUDIO_CHECK_SOURCES := tests/audio_check.c tests/audio_frame.c
AUDIO_CHECK_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(AUDIO_CHECK_SOURCES)))
# Milestone 9's 10-minute pacing run, too long for the test title.
PACING_SOURCES := tests/pacing.c tests/bar_scene.c
PACING_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(PACING_SOURCES)))
# Bring-up probes: titles that each answer one question on the console.
PROBES := probe_spin probe_print probe_cp probe_ring probe_interrupt probe_exports probe_engines probe_swap \
	probe_display probe_guide probe_resolve probe_triangle probe_vblank probe_720p probe_constants probe_mip_tail \
	probe_draw_after_resolve probe_pixel_rate probe_unroll probe_hiz probe_net probe_texel_rate
# GCC's -O2 for PowerPC, which unrolls small loops only (gcc:common/config/rs6000/rs6000-common.cc:39-46).
UNROLL_O2_FLAGS := -munroll-only-small-loops -fno-rename-registers
UNROLL_LOOPS_OBJECTS := $(BUILD)/tests/unroll_loops_o2.o $(BUILD)/tests/unroll_loops_unrolled.o

.PHONY: all clean cmake_check
# Keep the ELFs and objects that pattern rules build on the way to a .xex.
.SECONDARY:
all: $(BUILD)/xsdk_test.xex $(BUILD)/host/libxsc.so $(BUILD)/audio_check.xex $(BUILD)/pacing_test.xex $(PROBES:%=$(BUILD)/%.xex)

# A title built through cmake/xsdk.cmake, as CMake projects such as Dusklight build; needs CMake and Ninja.
cmake_check: $(BUILD)/libxsdk.a
	cmake -S tests/cmake -B $(BUILD)/cmake_check -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/xsdk.cmake
	cmake --build $(BUILD)/cmake_check

$(BUILD)/%.xex: $(BUILD)/%.elf tools/elf2xex.py
	$(PYTHON) tools/elf2xex.py $< $@

$(BUILD)/xsdk_test.elf: $(TEST_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/xsdk_test.map -o $@ $(TEST_OBJECTS) $(LIBS)
	$(OBJDUMP) -d $@ | $(PYTHON) tools/xenia_scan.py

$(BUILD)/audio_check.elf: $(AUDIO_CHECK_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(AUDIO_CHECK_OBJECTS) $(LIBS)

$(BUILD)/pacing_test.elf: $(PACING_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(PACING_OBJECTS) $(LIBS)

$(BUILD)/probe_%.elf: $(BUILD)/tests/probe_%.o $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/tests/probe_$*.o $(LIBS)

$(BUILD)/probe_unroll.elf: $(BUILD)/tests/probe_unroll.o $(UNROLL_LOOPS_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/tests/probe_unroll.o $(UNROLL_LOOPS_OBJECTS) $(LIBS)

# probe_unroll's loops built both ways, its copy loop kept a loop rather than a call to memcpy.
$(BUILD)/tests/unroll_loops_o2.o: tests/unroll_loops.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(UNROLL_O2_FLAGS) -fno-tree-loop-distribute-patterns -DUNROLL_LOOPS=unroll_loops_o2 -c $< -o $@

$(BUILD)/tests/unroll_loops_unrolled.o: tests/unroll_loops.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -funroll-loops -fno-tree-loop-distribute-patterns -DUNROLL_LOOPS=unroll_loops_unrolled -c $< -o $@

$(BUILD)/libxsdk.a: $(SDK_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(IMPORT_SOURCES) &: imports/imports.txt tools/gen_imports.py
	$(PYTHON) tools/gen_imports.py imports/imports.txt $(BUILD)/imports

$(BUILD)/imports/%.o: $(BUILD)/imports/%.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/sdk/%.o: src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/sdk/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# memcpy itself, so GCC mustn't turn its loops back into calls to memcpy.
$(BUILD)/sdk/copy.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns

# xost folds constants in doubles, as Python does; a fused multiply-add rounds once and would differ.
$(XSC_SOURCES:src/%.c=$(BUILD)/sdk/%.o): CFLAGS += -ffp-contract=off

# The console probe of xost's compiler, from the shader pack Aurora's tools/build_glsl_pack.py wrote for Dusklight;
# outside all, as the pack is Dusklight's. probe_xsc_break has the fetch layout off, so it must fail.
XSC_PACK ?= ../dusklight-xenon/platforms/xenon/glsl_pack.c
XSC_REFUSED := xenos-toolchain/xsc/testdata/vk_frag_coord_depth.frag.spv \
	xenos-toolchain/xsc/testdata/vk_local_partial.frag.spv

$(BUILD)/probe/probe_xsc_programs.h: $(XSC_PACK) tools/gen_xsc_probe.py $(BUILD)/host/libxsc.so
	$(PYTHON) tools/gen_xsc_probe.py $(XSC_PACK) $@ $(XSC_REFUSED)

$(BUILD)/tests/probe_xsc.o: CFLAGS += -I$(BUILD)/probe
$(BUILD)/tests/probe_xsc.o: $(BUILD)/probe/probe_xsc_programs.h

$(BUILD)/tests/probe_xsc_break.o: tests/probe_xsc.c $(BUILD)/probe/probe_xsc_programs.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(BUILD)/probe -DXSC_PROBE_BREAK -c $< -o $@

$(BUILD)/host/libxsc.so: $(XSC_SOURCES:src/%.c=$(BUILD)/host/%.o)
	$(HOST_CC) -shared -o $@ $^

$(BUILD)/host/%.o: src/%.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -ffp-contract=off -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
