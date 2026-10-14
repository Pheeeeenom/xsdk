#!/bin/bash
# build.sh <name> "<extra flags>": how milestone 35's titles were built, in WSL. Copies of xsdk and vulkan4xenon, plain
# and with the diagnostics' counters (diagnostic/), and Dusklight's normal and diagnostics builds, into
# ~/build/unroll/<name>, cleaned and built in full with 32 jobs, every C and C++ compile given <extra flags>: "" for the
# toolchain's -O2, -funroll-loops for the other. XW, VW, DW and DDW are xsdk at the milestone, vulkan4xenon 950ffd4,
# Dusklight 7e4b828248 (Aurora 1844958), and that Dusklight with diagnostic/aurora-diag.patch and dusklight-diag.patch.
set -euo pipefail
: "${XW:?xsdk}" "${VW:?vulkan4xenon}" "${DW:?Dusklight}" "${DDW:?Dusklight with the diagnostics}"
NAME=$1
EXTRA=$2
TC=$HOME/xsdk-gcc
B=$HOME/build/unroll/$NAME
P=$(cd "$(dirname "$0")" && pwd)/diagnostic
LOGS=$B/logs
PREFIX=$TC/bin/powerpc-xsdk-elf-
CC="${PREFIX}gcc${EXTRA:+ $EXTRA}"
CXX="${PREFIX}g++${EXTRA:+ $EXTRA}"
FLAGS="-mcpu=cell -fno-pic${EXTRA:+ $EXTRA}"
SDL=/home/xenon/build/m23/sdl
mkdir -p "$LOGS"

# copy_tree <source> <dest>: a worktree's files, without its builds.
copy_tree() {
	rm -rf "$2"
	mkdir -p "$2"
	tar -C "$1" --exclude=./.git --exclude='./build*' -cf - . | tar -xf - -C "$2"
}

copy_tree "$XW" "$B/xsdk"
make -C "$B/xsdk" -j32 TOOLCHAIN="$PREFIX" CC="$CC" CXX="$CXX" > "$LOGS/xsdk.log" 2>&1
copy_tree "$XW" "$B/xsdk-diag"
python3 -I "$P/patch_xsdk_rectangles.py" "$B/xsdk-diag"
make -C "$B/xsdk-diag" -j32 TOOLCHAIN="$PREFIX" CC="$CC" CXX="$CXX" build/libxsdk.a > "$LOGS/xsdk-diag.log" 2>&1

copy_tree "$VW" "$B/v4x"
make -C "$B/v4x" -j32 XSDK="$B/xsdk" TOOLCHAIN="$PREFIX" CC="$CC" > "$LOGS/v4x.log" 2>&1
copy_tree "$VW" "$B/v4x-diag"
python3 -I "$P/patch_v4x.py" "$B/v4x-diag"
make -C "$B/v4x-diag" -j32 XSDK="$B/xsdk-diag" TOOLCHAIN="$PREFIX" CC="$CC" build/libvulkan4xenon.a \
	> "$LOGS/v4x-diag.log" 2>&1

# dusklight <source> <build dir> <xsdk> <vulkan4xenon> <cmake options...>
dusklight() {
	local source=$1 dir=$2 xsdk=$3 v4x=$4
	shift 4
	cmake -S "$source" -B "$dir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$xsdk/cmake/xsdk.cmake" -DXSDK_TOOLCHAIN="$TC" \
		-DVULKAN4XENON_DIR="$v4x" -DSDL3_DIR="$SDL" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS="$FLAGS" "$@" \
		> "$LOGS/$(basename "$dir")-configure.log" 2>&1
	ninja -C "$dir" -t clean > /dev/null
	ninja -C "$dir" -j32 dusklight > "$LOGS/$(basename "$dir").log" 2>&1
}

dusklight "$DW" "$B/dusklight" "$B/xsdk" "$B/v4x" -DCMAKE_CXX_FLAGS="$FLAGS"
dusklight "$DDW" "$B/dusklight-diag" "$B/xsdk-diag" "$B/v4x-diag" -DAURORA_ENABLE_DIAGNOSTICS=ON \
	-DCMAKE_CXX_FLAGS="$FLAGS -DDUSK_XENON_DIAGNOSTICS"
