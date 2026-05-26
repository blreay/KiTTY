#!/bin/bash
#
cd 0.76b_My_PuTTY/windows 

act=${1:-build}

case $act in
build)
echo "build"
make -f MAKEFILE.MINGW cross64
ls -alrt /builds
;;
clean)
echo "clean"
make -f MAKEFILE.MINGW clean
;;
esac

