#!/bin/bash
#
# Repo-relative path, regardless of where the script is invoked from.
REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_ROOT/0.76b_My_PuTTY/windows"

act=${1:-cross}
export MAKEFLAGS="-j$(nproc)"

# The cross64 target inside MAKEFILE.MINGW does:
#     cp mini/mini_64.a    mini/mini.a
#     cp bcrypt/bcrypt_64.a bcrypt/bcrypt.a
#     cp base64/base64_64.a base64/base64.a
# This silently leaves the 32-bit-named archives holding x86_64 objects, so
# the next `cross` build links them as wrong-architecture archives. GNU ld
# does NOT fail loudly on this -- it just leaves a wagonload of "undefined
# reference to readINI / writeINI / bcrypt_string_base64 / ..." errors that
# look unrelated to the .a files.
#
# Restore the matching architecture's archives before each build.
# The 64-bit "*_64.a" files committed in git are authoritative; the
# 32-bit ".a" filenames live in the same trees but were unfortunately
# overwritten with 64-bit content by an earlier `cross64` build that
# got committed (see commit fead573). The last commit that had genuine
# 32-bit content in those filenames is 035070c, so we restore from there.
PRISTINE_32BIT_LIBS_COMMIT="035070c"

restore_libs() {
    local arch="$1"   # "32" or "64"
    case "$arch" in
        32)
            for lib in mini/mini.a bcrypt/bcrypt.a base64/base64.a; do
                if ! git -C "$REPO_ROOT" show "${PRISTINE_32BIT_LIBS_COMMIT}:${lib}" \
                        > "$REPO_ROOT/${lib}" 2>/dev/null; then
                    echo "warn: could not restore 32-bit ${lib} from ${PRISTINE_32BIT_LIBS_COMMIT}"
                fi
            done
            ;;
        64)
            cp "$REPO_ROOT/mini/mini_64.a"     "$REPO_ROOT/mini/mini.a"
            cp "$REPO_ROOT/bcrypt/bcrypt_64.a" "$REPO_ROOT/bcrypt/bcrypt.a"
            cp "$REPO_ROOT/base64/base64_64.a" "$REPO_ROOT/base64/base64.a"
            ;;
    esac
}

case $act in
cross64)
echo "build 64bit"
restore_libs 64
make -f MAKEFILE.MINGW cross64
ls -alrt /builds
;;
cross)
echo "build 32bit"
restore_libs 32
make -f MAKEFILE.MINGW cross
ls -alrt /builds
;;
clean)
echo "clean"
make -f MAKEFILE.MINGW clean
;;
esac
