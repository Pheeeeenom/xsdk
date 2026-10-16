#!/bin/bash
# Builds each break set of xsdk_test_breaks.py in a copy of xsdk, as xsdk_test_broken_<set>.xex in OUT, broken/
# beside this by default; the working tree isn't touched, and the copy leaves out xost's reference and glslang.
# Run in WSL: builds.sh [sets], all three by default.
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$HERE/broken}
mkdir -p "$OUT" || exit 1
for set in ${@:-a b c}; do
	copy="$OUT/xsdk_$set"
	rm -rf "$copy"
	mkdir -p "$copy"
	(cd "$XSDK" && tar --exclude=./.git --exclude=./build --exclude=./results --exclude=./xenos-toolchain/reference \
		--exclude=./xenos-toolchain/tools/glslang -cf - .) | (cd "$copy" && tar -xf -) || exit 1
	python3 "$HERE/xsdk_test_breaks.py" "$copy" "$set" || exit 1
	(cd "$copy" && make -j32 build/xsdk_test.xex >"$OUT/broken_$set.txt" 2>&1) &&
		cp "$copy/build/xsdk_test.xex" "$OUT/xsdk_test_broken_$set.xex"
	grep -E "error:" "$OUT/broken_$set.txt"
	rm -rf "$copy"
done
ls -la "$OUT"/*.xex
