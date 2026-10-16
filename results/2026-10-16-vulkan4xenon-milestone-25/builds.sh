#!/bin/bash
# Builds xsdk_test once for each break set of xsdk_test_breaks.py, each in a copy of this xsdk made in the scratch
# folder, as xsdk_test_m25_broken_<set>.xex there: builds.sh <scratch> [set ...], every set when none is named. The
# tree itself is never touched. Run in WSL.
S=${1:?a scratch folder}
shift
SETS=${*:-a b c d e}
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=$(cd "$HERE/../.." && pwd)
for set in $SETS; do
	COPY="$S/xsdk_m25_$set"
	rm -rf "$COPY" && mkdir -p "$COPY" || exit 1
	# The reference sources and glslang are junctions to the main checkout, so they stay out of the copy.
	tar -C "$XSDK" --exclude=./build --exclude=./.git --exclude=./results --exclude=./local \
		--exclude=./xenos-toolchain/reference --exclude=./xenos-toolchain/tools/glslang -cf - . | tar -C "$COPY" -xf - ||
		exit 1
	python3 "$HERE/xsdk_test_breaks.py" "$COPY" "$set" || exit 1
	if make -C "$COPY" -j32 build/xsdk_test.xex >"$S/xsdk_m25_$set.txt" 2>&1; then
		cp "$COPY/build/xsdk_test.xex" "$S/xsdk_test_m25_broken_$set.xex" && echo "set $set built"
	else
		grep -E "error:" "$S/xsdk_m25_$set.txt"
		echo "set $set failed to build"
	fi
	rm -rf "$COPY"
done
ls -la "$S"/xsdk_test_m25_broken_*.xex
