#!/bin/bash
# Builds each break set of xsdk_test_breaks.py, or the sets named, into a copy of xsdk, as xsdk_test_broken_<set>.xex
# in $OUT (build/breaks by default). The tree itself is never changed. Run it in WSL.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$REPO/build/breaks}
SETS=${*:-$(python3 "$HERE/xsdk_test_breaks.py" --list)}
mkdir -p "$OUT"
for set in $SETS; do
	copy=$(mktemp -d)
	cp -r "$REPO"/{src,include,imports,tests,tools,assets,Makefile} "$copy"
	python3 "$HERE/xsdk_test_breaks.py" "$copy" "$set"
	if make -C "$copy" -j32 build/xsdk_test.xex >"$OUT/broken_$set.txt" 2>&1; then
		cp "$copy/build/xsdk_test.xex" "$OUT/xsdk_test_broken_$set.xex"
	else
		grep -E "error" "$OUT/broken_$set.txt"
	fi
	rm -rf "$copy"
done
ls -la "$OUT"/*.xex
