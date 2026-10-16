#!/bin/bash
# Builds xsdk_test once for each of breaks.py's breaks, or for those named, to xsdk_test_m21_<break>.xex in OUT, each
# from a copy of XSDK made there and deleted after; XSDK is only read. tests/xsc_shaders.h stays as the unbroken
# compiler made it, so the console's compiler, broken, gives other containers for the shaders the break touches.
XSDK=${XSDK:-/mnt/z/Xbox-360-SDK}
OUT=${OUT:?OUT names a folder outside the tree for the copies and the titles}
BREAKS="$(dirname "$(readlink -f "$0")")/breaks.py"
mkdir -p "$OUT" || exit 1
for name in ${*:-$(python3 "$BREAKS" list)}; do
	copy="$OUT/xsdk-m21-$name"
	log="$OUT/xsdk-m21-$name.txt"
	rm -rf "$copy" && mkdir -p "$copy" || exit 1
	tar -C "$XSDK" -cf - --exclude=build assets cmake imports include src tests tools xenos-toolchain Makefile |
		tar -C "$copy" -mxf - || exit 1
	python3 "$BREAKS" apply "$name" "$copy" || exit 1
	(cd "$copy" && make -j32 build/xsdk_test.xex) >"$log" 2>&1 &&
		cp "$copy/build/xsdk_test.xex" "$OUT/xsdk_test_m21_$name.xex" || echo "$name: the build failed, see $log"
	grep -E "error:" "$log"
	rm -rf "$copy"
done
ls -la --time-style=+%T "$OUT"/xsdk_test_m21_*.xex
