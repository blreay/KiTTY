#!/bin/bash
# scripts/setup-wine-debug-env.sh
#
# Install and configure everything needed to run + debug KiTTY's Windows
# binaries (kitty.exe / kitty64.exe) on a Linux box, headless.
#
# Idempotent: safe to run repeatedly.
#
# Requirements:
#   - Ubuntu 22.04 / 24.04 (or other Debian-derived with multiarch)
#   - sudo (the script will use `sudo -n` only; pre-configure passwordless
#     sudo or run as a user already in sudoers NOPASSWD for apt-get)
#
# Steps:
#   1. Enable i386 multiarch (needed for wine32)
#   2. apt-get install: wine, wine64, wine32:i386, Xvfb, xdotool,
#      xpra, gdb, putty-tools, imagemagick, openssh-server, file
#   3. Initialise two wine prefixes under ~/wine_prefixes:
#        wp32 (WINEARCH=win32) for 32-bit kitty.exe
#        wp64 (WINEARCH=win64) for 64-bit kitty64.exe
#   4. Print a readiness report (versions + path checks)

set -u

SCRIPT_NAME=$(basename "$0")
HOME_DIR="${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}"
PREFIX_ROOT="$HOME_DIR/wine_prefixes"

log() { printf '\033[1;34m[%s]\033[0m %s\n' "$SCRIPT_NAME" "$*"; }
warn() { printf '\033[1;33m[%s WARN]\033[0m %s\n' "$SCRIPT_NAME" "$*" >&2; }
err() { printf '\033[1;31m[%s ERR]\033[0m %s\n' "$SCRIPT_NAME" "$*" >&2; }

# ------- 1. multiarch (for wine32:i386) -------
log "Step 1/4: enable i386 multiarch"
if dpkg --print-foreign-architectures 2>/dev/null | grep -q '^i386$'; then
    log "  i386 already enabled, skipping dpkg --add-architecture"
else
    log "  enabling i386 architecture (sudo required)"
    sudo -n dpkg --add-architecture i386 || {
        err "  could not run 'sudo dpkg --add-architecture i386'."
        err "  Ensure passwordless sudo is configured, or run this script as root."
        exit 1
    }
fi

# ------- 2. install packages -------
log "Step 2/4: apt-get update + install packages"

# Packages: any arch-suffixed name needs i386; the rest are amd64.
PACKAGES=(
    wine
    wine64
    wine32:i386
    xvfb
    xdotool
    xpra
    gdb
    putty-tools          # provides puttygen for OpenSSH -> .ppk conversion
    imagemagick          # provides `import` for headless screenshots
    tesseract-ocr        # OCR engine for screen-content validation
    openssh-server       # local sshd to SSH against from KiTTY
    file                 # arch checks
    # winedbg is bundled inside `wine` package as /usr/bin/winedbg
)

# Run apt-get update only if cache > 1h old (cheap idempotency)
APT_CACHE_MTIME=$(stat -c %Y /var/cache/apt/pkgcache.bin 2>/dev/null || echo 0)
NOW=$(date +%s)
if [ $((NOW - APT_CACHE_MTIME)) -gt 3600 ]; then
    log "  apt-get update"
    sudo -n apt-get update -qq || { err "apt-get update failed"; exit 1; }
else
    log "  apt cache is fresh, skipping update"
fi

log "  apt-get install ${PACKAGES[*]}"
sudo -n apt-get install -y "${PACKAGES[@]}" >/tmp/setup-wine-apt.log 2>&1 || {
    err "  apt-get install failed; see /tmp/setup-wine-apt.log"
    tail -20 /tmp/setup-wine-apt.log >&2
    exit 1
}

# ------- 3. wine prefixes -------
log "Step 3/4: initialise wine prefixes under $PREFIX_ROOT"
mkdir -p "$PREFIX_ROOT"

init_prefix() {
    local prefix="$1" arch="$2"
    if [ -f "$prefix/system.reg" ]; then
        log "  $prefix already initialised ($arch), skipping wineboot"
        return 0
    fi
    log "  creating $prefix ($arch)"
    WINEPREFIX="$prefix" WINEARCH="$arch" WINEDEBUG=-all \
        wineboot -i >/tmp/setup-wine-boot-${arch}.log 2>&1 || {
        warn "wineboot for $arch reported errors, but prefix may still be usable"
        tail -10 /tmp/setup-wine-boot-${arch}.log >&2
    }
}

init_prefix "$PREFIX_ROOT/wp32" "win32"
init_prefix "$PREFIX_ROOT/wp64" "win64"

# ------- 4. readiness report -------
log "Step 4/4: environment readiness report"

check_cmd() {
    local cmd="$1" expected_in="$2"
    local path
    path=$(command -v "$cmd" 2>/dev/null || true)
    if [ -n "$path" ]; then
        printf '  \033[32m✓\033[0m %-20s -> %s\n' "$cmd" "$path"
        return 0
    else
        printf '  \033[31m✗\033[0m %-20s (expected from package: %s)\n' "$cmd" "$expected_in"
        return 1
    fi
}

check_path() {
    local path="$1" label="$2"
    if [ -e "$path" ]; then
        printf '  \033[32m✓\033[0m %-30s %s\n' "$label" "$path"
        return 0
    else
        printf '  \033[31m✗\033[0m %-30s %s (missing)\n' "$label" "$path"
        return 1
    fi
}

FAIL=0
echo
echo "=== Commands ==="
check_cmd wine        "wine"           || FAIL=$((FAIL+1))
# Note: on wine >= 8, /usr/bin/wine itself is a unified launcher that handles
# both 32 and 64-bit PE binaries; the legacy `wine64` symlink is no longer
# shipped. We still want the wine64 *package* installed (it pulls 64-bit
# libwine), but don't require the wine64 binary symlink.
if command -v wine64 >/dev/null 2>&1; then
    printf '  \033[32m✓\033[0m %-20s -> %s\n' "wine64" "$(command -v wine64)"
else
    printf '  \033[33m·\033[0m %-20s (not present; wine 8+ has unified launcher, OK)\n' "wine64"
fi
check_cmd Xvfb        "xvfb"           || FAIL=$((FAIL+1))
check_cmd xdotool     "xdotool"        || FAIL=$((FAIL+1))
check_cmd xpra        "xpra"           || FAIL=$((FAIL+1))
check_cmd gdb         "gdb"            || FAIL=$((FAIL+1))
check_cmd winedbg     "wine"           || FAIL=$((FAIL+1))
check_cmd puttygen    "putty-tools"    || FAIL=$((FAIL+1))
check_cmd import      "imagemagick"    || FAIL=$((FAIL+1))
check_cmd tesseract   "tesseract-ocr"  || FAIL=$((FAIL+1))
check_cmd sshd        "openssh-server" || FAIL=$((FAIL+1))
check_cmd file        "file"           || FAIL=$((FAIL+1))

echo
echo "=== Wine prefixes ==="
check_path "$PREFIX_ROOT/wp32/system.reg" "32-bit prefix initialised" || FAIL=$((FAIL+1))
check_path "$PREFIX_ROOT/wp64/system.reg" "64-bit prefix initialised" || FAIL=$((FAIL+1))

echo
echo "=== Versions ==="
wine     --version 2>/dev/null | sed 's/^/  wine     : /'
gdb      --version 2>/dev/null | head -1 | sed 's/^/  gdb      : /'
xpra     --version 2>/dev/null | head -1 | sed 's/^/  xpra     : /'
xdotool  --version 2>/dev/null | sed 's/^/  xdotool  : /'
puttygen --version 2>/dev/null | head -1 | sed 's/^/  puttygen : /'
Xvfb     -version  2>&1 | head -1 | sed 's/^/  Xvfb     : /'

echo
echo "=== Multiarch ==="
printf '  enabled architectures: '
dpkg --print-foreign-architectures 2>/dev/null | tr '\n' ' '
echo
echo "  primary architecture: $(dpkg --print-architecture)"

echo
if [ "$FAIL" -eq 0 ]; then
    log "\033[1;32mAll checks passed — environment ready.\033[0m"
    log "Next: $(dirname "$0")/build-and-test.sh [cross|cross64|both]"
    exit 0
else
    err "$FAIL check(s) failed — see above. Re-run after fixing."
    exit 1
fi
