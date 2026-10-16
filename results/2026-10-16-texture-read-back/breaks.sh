#!/bin/bash
# Builds xsdk_test with the texture read-back broken twice, then restores src/texture.c and rebuilds clean:
# uploads of rows past the first land on the first rows, and words read back come out one off.
S=/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/3ba51ff7-78d2-4449-9ad4-c903b0892394/scratchpad
cd /mnt/z/Xbox-360-SDK || exit 1
cp src/texture.c "$S/texture.c.orig"
python3 - <<'EOF'
import pathlib
path = pathlib.Path("src/texture.c")
text = path.read_text()
for old, new in [("place.y + first_row + y", "place.y + y"), ("out[i] = words[i];", "out[i] = words[i] ^ 1;")]:
    assert text.count(old) == 1, old
    text = text.replace(old, new)
path.write_text(text)
EOF
make -j32 >"$S/xsdk_broken.txt" 2>&1
echo "broken make exit $?"
grep -n -E "error|Error|\*\*\*" "$S/xsdk_broken.txt" | grep -v xenia_scan | head
cp build/xsdk_test.xex "$S/xsdk_test_rb_broken.xex"
cp "$S/texture.c.orig" src/texture.c
cmp -s src/texture.c "$S/texture.c.orig" && echo restored
make clean >/dev/null 2>&1
make -j32 >"$S/xsdk_clean.txt" 2>&1
echo "clean make exit $?"
ls -la --time-style=+%T build/xsdk_test*.xex build/libxsdk.a
