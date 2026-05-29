#!/bin/bash
#
# Repo-relative path, regardless of where the script is invoked from.
REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_ROOT/0.76b_My_PuTTY/windows"

act=${1:-cross}
export MAKEFLAGS="-j$(nproc)"

# All vendored .a libraries now live as either xxx_32.a or xxx_64.a — both
# committed to the repo. MAKEFILE.MINGW selects the right one via the
# LIBSUFFIX variable (set by the cross / cross64 targets). No more cp
# trickery, no more restore step here.

case $act in
cross64)
echo "build 64bit"
make -f MAKEFILE.MINGW cross64
ls -alrt /builds
;;
cross)
echo "build 32bit"
make -f MAKEFILE.MINGW cross
ls -alrt /builds
;;
clean)
echo "clean"
make -f MAKEFILE.MINGW clean
;;
esac
