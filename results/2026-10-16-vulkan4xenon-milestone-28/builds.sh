#!/bin/bash
# Builds vulkan4xenon milestone 28's probes, in WSL: xsdk cleaned and built in full, then tests/probe_occlusion.c once a
# try, into build/probe_occlusion_<try>.xex. The Makefile's OCCLUSION_TRIES lists the tries in the order to run them on
# the console, one at a time (vulkan4xenon:docs/ROADMAP.md, milestone 28). Every try but registers can hang the GPU;
# one that does logs where and leaves the GPU hung, so the console restarts before the next.
# Run as: bash results/2026-10-16-vulkan4xenon-milestone-28/builds.sh
cd "$(dirname "$0")/../.." || exit 1
log=$(mktemp)
make clean >/dev/null
make -j32 >"$log" 2>&1 && make -j32 occlusion_tries >>"$log" 2>&1
status=$?
grep -E 'warning:|error:|\*\*\*' "$log"
rm "$log"
[ $status -eq 0 ] || exit $status
ls -la --time-style=+%T build/probe_occlusion_*.xex
