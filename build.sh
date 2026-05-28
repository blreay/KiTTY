#!/bin/bash
#
cd 0.76b_My_PuTTY/windows

act=${1:-cross}

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

