#!/bin/bash
# Builds each set of breaks.py, or those named, into a copy of xsdk, as <out>/xsdk_test_broken_<set>.xex:
# builds.sh <out> [set...]. The repository isn't touched. Run in WSL.
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=$(cd "$HERE/../.." && pwd)
OUT=${1:?builds.sh <out> [set...]}
shift
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SETS=${*:-$(python3 "$HERE/breaks.py" list)}
for set in $SETS; do
	work="$OUT/copy_$set"
	rm -rf "$work"
	mkdir -p "$work"
	# The sources alone: no build output, history, results or the toolchain's read-only reference clones.
	tar -C "$XSDK" --exclude=./build --exclude=./.git --exclude=./results --exclude=./local \
		--exclude=./xenos-toolchain/reference --exclude=./xenos-toolchain/tools/glslang -cf - . | tar -C "$work" -xf -
	python3 "$HERE/breaks.py" apply "$set" "$work" || exit 1
	if ! make -C "$work" -j32 build/xsdk_test.xex >"$OUT/build_$set.txt" 2>&1; then
		echo "$set: xsdk didn't build ($OUT/build_$set.txt)"
		exit 1
	fi
	cp "$work/build/xsdk_test.xex" "$OUT/xsdk_test_broken_$set.xex"
	rm -rf "$work"
	echo "$set: $OUT/xsdk_test_broken_$set.xex"
done
