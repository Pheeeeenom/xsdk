#!/bin/bash
# Builds each break set of breaks.py in a copy of xsdk, never xsdk itself, and keeps each broken title as
# xsdk_test_broken_<set>.xex in OUT. Run in WSL: builds.sh 2a ...
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$HERE/broken}
COPY="$OUT/copy"
mkdir -p "$OUT" || exit 1
for set in "$@"; do
	rm -rf "$COPY" && mkdir -p "$COPY" || exit 1
	# The build needs neither xost's Python nor the reference clones.
	tar -C "$XSDK" --exclude=./build --exclude=./results --exclude=./.git --exclude=./local \
		--exclude=./xenos-toolchain -cf - . | tar -C "$COPY" -xf - || exit 1
	python3 "$HERE/breaks.py" "$set" "$COPY" || exit 1
	if make -C "$COPY" -j32 build/xsdk_test.xex >"$OUT/build_$set.txt" 2>&1; then
		cp "$COPY/build/xsdk_test.xex" "$OUT/xsdk_test_broken_$set.xex"
	else
		echo "set $set didn't build:"
		grep -E "error:" "$OUT/build_$set.txt"
	fi
done
rm -rf "$COPY"
ls -la "$OUT"/*.xex
