# Builds the SDK library and the test title. Run inside WSL Debian.

TOOLCHAIN ?= /usr/local/xenon/bin/xenon-
CC := $(TOOLCHAIN)gcc
AR := $(TOOLCHAIN)ar
OBJDUMP := $(TOOLCHAIN)objdump
PYTHON ?= python3

BUILD := build

CFLAGS := -m32 -mcpu=cell -fno-pic -std=gnu17 -O2 -g -Wall -Wextra -Werror -Iinclude -MMD -MP
ASFLAGS := -m32 -mcpu=cell -Wa,-mregnames -Iinclude -MMD -MP
# The driver doesn't search newlib's 32-bit library folder by itself.
LDFLAGS := -m32 -static -nostdlib -T src/xsdk.ld -L$(dir $(TOOLCHAIN))../xenon/lib/32
LIBS := -Wl,--start-group $(BUILD)/libxsdk.a -lc -lm -lgcc -Wl,--end-group

SDK_SOURCES := src/start.c src/log.c src/sbrk.c src/thread.c src/thread_entry.S src/malloc_lock.c
IMPORT_NAMES := $(shell $(PYTHON) tools/gen_imports.py --list imports/imports.txt)
IMPORT_SOURCES := $(IMPORT_NAMES:%=$(BUILD)/imports/%.S)
SDK_OBJECTS := $(patsubst src/%,$(BUILD)/sdk/%.o,$(basename $(SDK_SOURCES))) $(IMPORT_NAMES:%=$(BUILD)/imports/%.o)

TEST_SOURCES := tests/main.c tests/test_image.c tests/test_threads.c
TEST_OBJECTS := $(patsubst tests/%,$(BUILD)/tests/%.o,$(basename $(TEST_SOURCES)))
# Bring-up probes: titles that each answer one question on the console.
PROBES := probe_spin probe_print

.PHONY: all clean
# Keep the ELFs and objects that pattern rules build on the way to a .xex.
.SECONDARY:
all: $(BUILD)/xsdk_test.xex $(PROBES:%=$(BUILD)/%.xex)

$(BUILD)/%.xex: $(BUILD)/%.elf tools/elf2xex.py
	$(PYTHON) tools/elf2xex.py $< $@

$(BUILD)/xsdk_test.elf: $(BUILD)/sdk/crt0.o $(TEST_OBJECTS) $(BUILD)/libxsdk.a src/xsdk.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/xsdk_test.map -o $@ $(BUILD)/sdk/crt0.o $(TEST_OBJECTS) $(LIBS)
	$(OBJDUMP) -d $@ | $(PYTHON) tools/xenia_scan.py

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

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
