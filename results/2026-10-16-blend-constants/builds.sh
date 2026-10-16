#!/bin/bash
# Builds each break set of step3_breaks.py, restoring the trees after each, then both clean again. The broken titles go
# to the scratchpad, where make clean doesn't reach.
S=/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/3ba51ff7-78d2-4449-9ad4-c903b0892394/scratchpad
state() { (cd /mnt/z/vulkan4xenon && git diff | sha1sum) && (cd /mnt/z/Xbox-360-SDK && git diff | sha1sum); }
state >"$S/before3.sha"
for set in a b xsdk; do
	python3 "$S/step3_breaks.py" apply "$set" || exit 1
	if [ "$set" = xsdk ]; then
		(cd /mnt/z/Xbox-360-SDK && make -j32 >"$S/broken3_$set.txt" 2>&1) &&
			cp /mnt/z/Xbox-360-SDK/build/xsdk_test.xex "$S/xsdk_test_s3_broken.xex"
	else
		(cd /mnt/z/vulkan4xenon && make -j32 >"$S/broken3_$set.txt" 2>&1) &&
			cp /mnt/z/vulkan4xenon/build/vulkan4xenon_test.xex "$S/v4x_s3_broken_$set.xex"
	fi
	grep -E "error:" "$S/broken3_$set.txt"
	python3 "$S/step3_breaks.py" restore
done
state | diff - "$S/before3.sha" && echo trees-restored
(cd /mnt/z/Xbox-360-SDK && make clean >/dev/null 2>&1 && make -j32 >"$S/clean3_xsdk.txt" 2>&1) && echo xsdk-clean
(cd /mnt/z/vulkan4xenon && make clean >/dev/null && make -j32 >"$S/clean3_v4x.txt" 2>&1) && echo v4x-clean
ls -la --time-style=+%T "$S"/*_s3_*.xex /mnt/z/vulkan4xenon/build/vulkan4xenon_test.xex /mnt/z/Xbox-360-SDK/build/xsdk_test.xex
