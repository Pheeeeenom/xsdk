#!/bin/bash
# relink.sh <name> <pattern>: Dusklight's diagnostics build relinked from build.sh's -O2 build (~/build/unroll/base),
# its objects and libraries, but with the libraries whose paths match <pattern>, an awk regular expression, from the
# -funroll-loops build (~/build/unroll/unroll): "libxxhash" for dusklight_diag_unroll_xxhash_only.log,
# "libabsl_|libxxhash" for dusklight_diag_unroll_abseil_xxhash_only.log, "libvulkan4xenon|libxsdk" for
# dusklight_diag_unroll_sdk_only.log. The other two relinks swapped the other way: the -funroll-loops build's link with
# the -O2 build's libaurora_*.a (dusklight_diag_unroll_aurora_o2.log) and with all its libraries
# (dusklight_diag_unroll_game_only.log).
set -euo pipefail
NAME=$1
PATTERN=$2
B=$HOME/build/unroll/base
U=$HOME/build/unroll/unroll
OUT=$HOME/build/unroll/mixed
mkdir -p "$OUT"
cd "$B/dusklight-diag"
ninja -t commands dusklight | grep -- "-o dusklight " | sed -e "s/^: && //" -e "s/ && cd .*//" | tr " " "\n" |
	awk -v B="$B" -v U="$U" -v P="$PATTERN" '{
		if ($0 ~ /\.a$/ && $0 !~ /libSDL3/ && $0 ~ P) {
			if ($0 ~ /^\//) sub(B "/", U "/"); else $0 = U "/dusklight-diag/" $0
		}
		print
	}' | tr "\n" " " | sed "s# -o dusklight # -o $OUT/dusklight_diag_$NAME #" > "$OUT/link_$NAME.sh"
bash "$OUT/link_$NAME.sh"
python3 "$U/xsdk-diag/tools/elf2xex.py" "$OUT/dusklight_diag_$NAME" "$OUT/dusklight_diag_$NAME.xex"
