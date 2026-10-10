# Builds the SDK library and the test title. Run inside WSL Debian.

# The toolchain tools/toolchain/build.sh builds.
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
CXX := $(TOOLCHAIN)g++
AR := $(TOOLCHAIN)ar
OBJDUMP := $(TOOLCHAIN)objdump
PYTHON ?= python3

BUILD := build

WARNINGS := -Wall -Wextra -Werror
CFLAGS := -mcpu=cell -fno-pic -std=gnu17 -O2 -g $(WARNINGS) -Iinclude -MMD -MP
CXXFLAGS := -mcpu=cell -fno-pic -std=c++23 -O2 -g $(WARNINGS) -Iinclude -MMD -MP
ASFLAGS := -mcpu=cell -Wa,-mregnames -Iinclude -MMD -MP
LDFLAGS := -static -nostdlib -T src/xsdk.ld
LIBS := -Wl,--start-group $(BUILD)/libxsdk.a -lstdc++ -lc -lm -lgcc -Wl,--end-group

SDK_SOURCES := src/start.c src/log.c src/sbrk.c src/thread.c src/thread_entry.S src/file.c src/path.c src/dir.c \
	src/pthread.c src/lock.c src/reent.c src/time.c src/sysconf.c src/entropy.c src/exit.c src/gpu.c
IMPORT_NAMES := $(shell $(PYTHON) tools/gen_imports.py --list imports/imports.txt)
IMPORT_SOURCES := $(IMPORT_NAMES:%=$(BUILD)/imports/%.S)
SDK_OBJECTS := $(patsubst src/%,$(BUILD)/sdk/%.o,$(basename $(SDK_SOURCES))) $(IMPORT_NAMES:%=$(BUILD)/imports/%.o)

TEST_SOURCES := tests/main.c tests/test_image.c tests/test_threads.c tests/test_files.c tests/test_input.c \
	tests/test_audio.c tests/audio_entry.S tests/audio_frame.c tests/test_cpp_objects.cpp tests/test_cpp_threads.cpp \
	tests/test_cpp_atomics.cpp tests/test_cpp_exceptions.cpp tests/test_cpp_chrono.cpp tests/test_cpp_filesystem.cpp \
	tests/test_cpp_format.cpp tests/test_gpu.c
TEST_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(TEST_SOURCES)))
# A title for listening to the audio frame layout on the console.
AUDIO_CHECK_SOURCES := tests/audio_check.c tests/audio_check_entry.S tests/audio_frame.c
AUDIO_CHECK_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(AUDIO_CHECK_SOURCES)))
# Bring-up probes: titles that each answer one question on the console.
PROBES := probe_spin probe_print probe_cp probe_ring probe_interrupt probe_exports probe_engines probe_swap probe_display probe_guide

.PHONY: all clean
# Keep the ELFs and objects that pattern rules build on the way to a .xex.
.SECONDARY:
all: $(BUILD)/xsdk_test.xex $(BUILD)/audio_check.xex $(PROBES:%=$(BUILD)/%.xex)

$(BUILD)/%.xex: $(BUILD)/%.elf tools/elf2xex.py
	$(PYTHON) tools/elf2xex.py $< $@

$(BUILD)/xsdk_test.elf: $(BUILD)/sdk/crt0.o $(TEST_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/xsdk_test.map -o $@ $(BUILD)/sdk/crt0.o $(TEST_OBJECTS) $(LIBS)
	$(OBJDUMP) -d $@ | $(PYTHON) tools/xenia_scan.py

$(BUILD)/audio_check.elf: $(BUILD)/sdk/crt0.o $(AUDIO_CHECK_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/sdk/crt0.o $(AUDIO_CHECK_OBJECTS) $(LIBS)

$(BUILD)/probe_%.elf: $(BUILD)/sdk/crt0.o $(BUILD)/tests/probe_%.o $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -o $@ $(BUILD)/sdk/crt0.o $(BUILD)/tests/probe_$*.o $(LIBS)

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
