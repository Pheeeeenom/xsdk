#!/bin/bash
# Builds breaks.py's break into xsdk_test_m22_broken.xex in a fresh copy of xsdk, leaving the tree alone. XSDK names
# the tree and WORK the folder for the copy and the title.
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=${XSDK:-/mnt/z/Xbox-360-SDK}
WORK=${WORK:-$HERE/work}
copy="$WORK/copy"
rm -rf "$copy" && mkdir -p "$copy" "$WORK/out" || exit 1
tar -C "$XSDK" --exclude=./.git --exclude=./build --exclude=./xenos-toolchain/reference \
	--exclude=./xenos-toolchain/tools/glslang -cf - . | tar -C "$copy" -xf - || exit 1
python3 "$HERE/breaks.py" "$copy" || exit 1
(cd "$copy" && make -j32 build/xsdk_test.xex >"$WORK/xsdk.txt" 2>&1) || { echo "xsdk failed"; exit 1; }
cp "$copy/build/xsdk_test.xex" "$WORK/out/xsdk_test_m22_broken.xex"
rm -rf "$copy"
ls -la "$WORK/out"
