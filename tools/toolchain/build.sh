#!/bin/bash
# Builds the SDK's cross toolchain (binutils, GCC for C and C++, newlib) for 32-bit PowerPC. Run inside WSL Debian.
set -euo pipefail

TARGET=powerpc-xsdk-elf
PREFIX="${PREFIX:-$HOME/xsdk-gcc}"
WORK="${WORK:-$HOME/xsdk-gcc-work}"
JOBS="${JOBS:-$(nproc)}"
HERE="$(cd "$(dirname "$0")" && pwd)"

BINUTILS=binutils-2.47
GCC=gcc-15.3.0
NEWLIB=newlib-4.6.0.20260123

# Each source: archive, URL, SHA-256 of the archive as first downloaded on 2026-10-09.
SOURCES=(
	"$BINUTILS.tar.xz https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.xz 154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff"
	"$GCC.tar.xz https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz fa59c1beef8995f27c4d71c1df227587189315d3e6faff1bb4306e61b0c530eb"
	"$NEWLIB.tar.gz https://sourceware.org/pub/newlib/$NEWLIB.tar.gz 6ff27e3bf022666f43f7802255be680eeff722ac181b1725d21e2e8318604ee3"
)

# Options every compile gets, the SDK's and titles' included.
SPECS=(
	-D__XSDK__                                  # turns on newlib's POSIX thread and dirent declarations (newlib-xsdk.patch)
	-D__DYNAMIC_REENT__                         # each thread gets its own errno and stdio state through __getreent
	# Whole 64-bit registers: the console's calling convention (-mcall-xsdk, the target's default) passes and keeps
	# them, and 64-bit atomics go inline with ldarx/stdcx.; libatomic doesn't build for *-elf.
	-mpowerpc64
	# Only CR0 and CR1 to allocate, so GCC rarely copies fields with mcrf, which Xenia skips; CR0 alone breaks GCC.
	-ffixed-cr2 -ffixed-cr3 -ffixed-cr4 -ffixed-cr5 -ffixed-cr6 -ffixed-cr7
	'%{!ftree-vectorize:-fno-tree-vectorize}'   # avoids AltiVec ops Xenon lacks (libxenon's gcc-9.2.0.diff)
	# Xenia misreads where tail-calling or split functions end (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204).
	-fno-optimize-sibling-calls -fno-reorder-blocks-and-partition
	# GCC 15.3's RTL PRE miscompiles 32-bit code with -mpowerpc64: it drops a copy and passes a pointer as 0.
	-fno-gcse
)

COMMON=(--target="$TARGET" --prefix="$PREFIX" --disable-nls --disable-multilib)
# Constructors go in .init_array, which start.c runs; a cross GCC otherwise falls back to .ctors.
GCC_COMMON=("${COMMON[@]}" --with-cpu=cell --with-newlib --disable-shared --disable-tls --with-specs="${SPECS[*]}"
	--enable-initfini-array
	--disable-libssp --disable-libquadmath --disable-libgomp --disable-libitm --disable-libsanitizer --disable-libvtv)

export PATH="$PREFIX/bin:$PATH"
mkdir -p "$WORK/src" "$WORK/build" "$WORK/logs"

step() {
	echo "== $1 ($(date +%H:%M:%S))"
}

# unpack <source> [patch]: unpacks a source tree and patches it, afresh whenever the patch changes.
unpack() {
	local name=$1 patch=${2:-} stamp="$1/.xsdk-patch" want=none
	[ -z "$patch" ] || want=$(sha256sum <"$HERE/$patch" | cut -d' ' -f1)
	if [ -d "$name" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$want" ]; then
		return
	fi
	rm -rf "$name"
	tar xf "$name".tar.*
	[ -z "$patch" ] || patch -d "$name" -p1 <"$HERE/$patch" >/dev/null
	echo "$want" >"$stamp"
}

fetch() {
	cd "$WORK/src"
	for source in "${SOURCES[@]}"; do
		read -r archive url sha256 <<<"$source"
		[ -f "$archive" ] || curl -sSfL -o "$archive" "$url"
		echo "$sha256  $archive" | sha256sum -c --quiet
	done
	unpack "$BINUTILS" binutils-xsdk.patch
	unpack "$NEWLIB" newlib-xsdk.patch
	unpack "$GCC" gcc-xsdk.patch
}

# Builds a stage unless its stamp exists; delete $WORK/build and $PREFIX after changing a patch or an option.
configure_and_make() {
	local name=$1 source=$2 targets=$3 installs=$4
	shift 5
	local build="$WORK/build/$name"
	if [ -f "$build.done" ]; then
		step "$name: already built"
		return
	fi
	step "$name"
	mkdir -p "$build"
	cd "$build"
	"$source/configure" "$@" >"$WORK/logs/$name-configure.log" 2>&1
	make -j"$JOBS" $targets >"$WORK/logs/$name-make.log" 2>&1
	make $installs >"$WORK/logs/$name-install.log" 2>&1
	touch "$build.done"
}

fetch
configure_and_make binutils "$WORK/src/$BINUTILS" all install -- "${COMMON[@]}" --disable-werror
configure_and_make gcc-stage1 "$WORK/src/$GCC" "all-gcc all-target-libgcc" "install-gcc install-target-libgcc" -- \
	"${GCC_COMMON[@]}" --enable-languages=c --without-headers --disable-threads --disable-libatomic
configure_and_make newlib "$WORK/src/$NEWLIB" all-target-newlib install-target-newlib -- "${COMMON[@]}" \
	--enable-newlib-io-long-long --enable-newlib-io-long-double --enable-newlib-io-c99-formats \
	--enable-newlib-retargetable-locking --disable-newlib-supplied-syscalls
configure_and_make gcc "$WORK/src/$GCC" all install -- "${GCC_COMMON[@]}" --enable-languages=c,c++ --enable-threads=posix

# Objects built for the console's calling convention link together, and the linker refuses one built for another with
# them (EF_PPC_XSDK, binutils-xsdk.patch).
step "check: the linker refuses objects of another calling convention"
check="$WORK/build/check"
rm -rf "$check"
mkdir -p "$check"
cd "$check"
echo 'int other(int x) { return x + 1; }' >other.c
echo 'int other(int x); int main(void) { return other(1); }' >main.c
"$TARGET-gcc" -c main.c -o main.o
"$TARGET-gcc" -c other.c -o other.o
"$TARGET-gcc" -mcall-sysv -c other.c -o other_sysv.o
"$TARGET-gcc" -nostdlib -Wl,-e,main -o same.elf main.o other.o
if "$TARGET-gcc" -nostdlib -Wl,-e,main -o mixed.elf main.o other_sysv.o 2>mixed.log; then
	echo "linked an object built with -mcall-sysv with -mcall-xsdk ones" >&2
	exit 1
fi
grep -q "calling convention" mixed.log

# A float read from any byte goes through the integer registers: the console faults on a float register load at an
# address not aligned to its size, and -mcheck-fpr-alignment makes any such load an error (SCOPE.md, milestone 23).
step "check: no float register load at memory not known to be aligned"
printf '#include <string.h>\nfloat read_float(const unsigned char *p) { float f; memcpy(&f, p, 4); return f; }\n' >float.c
"$TARGET-gcc" -O2 -mcheck-fpr-alignment -c float.c -o float.o
step "done: $PREFIX/bin/$TARGET-gcc"
