#!/bin/bash
# scripts/build-and-test.sh [cross|cross64|both]
#
# Smoke-tests one or both architectures of KiTTY:
#   1. Build the .exe via the existing ./build.sh
#   2. Launch it under wine + Xvfb
#   3. SSH to the local sshd (so the remote shell IS this very Ubuntu host)
#   4. Auto-accept the host-key dialog
#   5. Type three shell commands (uname / echo / ls) and inspect the screen
#   6. Compare output against expected substrings — pass / fail per arch
#
# Default: both architectures.
#
# Artefacts (per arch X):
#   /tmp/kkit-X/                      ephemeral test dir
#   /tmp/kkit-X/screen-{a,b,c}.png    screenshots (host-key, prompt, post-cmds)
#   /tmp/kkit-X/wine.log              wine + kitty stderr
#   /tmp/kkit-X/result.txt            PASS / FAIL summary
#
# Requirements: ./scripts/setup-wine-debug-env.sh must have run successfully
# (provides wine, wine32:i386, Xvfb, xdotool, puttygen, imagemagick).

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_NAME=$(basename "$0")

ACTION="${1:-both}"
case "$ACTION" in
    cross|cross64|both) ;;
    *) echo "usage: $0 [cross|cross64|both]" >&2; exit 2 ;;
esac

DISPLAY_NUM=99
SSH_USER="$(whoami)"
SSH_HOST="localhost"
SSH_PORT=22

log()  { printf '\033[1;34m[%s]\033[0m %s\n' "$SCRIPT_NAME" "$*"; }
ok()   { printf '\033[1;32m[%s OK]\033[0m %s\n' "$SCRIPT_NAME" "$*"; }
warn() { printf '\033[1;33m[%s WARN]\033[0m %s\n' "$SCRIPT_NAME" "$*" >&2; }
err()  { printf '\033[1;31m[%s ERR]\033[0m %s\n' "$SCRIPT_NAME" "$*" >&2; }

# -------- preflight --------
preflight() {
    log "Preflight checks"
    for c in wine Xvfb xdotool import puttygen ssh ssh-keygen; do
        command -v "$c" >/dev/null 2>&1 || {
            err "missing '$c' — run scripts/setup-wine-debug-env.sh first"
            exit 1
        }
    done
    # sshd listening?
    if ! ss -tnlp 2>/dev/null | grep -q ":$SSH_PORT "; then
        warn "no listener on :$SSH_PORT — attempting to start sshd"
        sudo -n /usr/sbin/sshd -p "$SSH_PORT" 2>/dev/null || {
            err "could not start sshd on :$SSH_PORT (need sudo)"
            exit 1
        }
        sleep 1
    fi
    # ssh self-loop works?
    if [ ! -f "$HOME/.ssh/id_ed25519_kitty_test" ]; then
        log "generating ed25519 SSH key for self-test"
        ssh-keygen -t ed25519 -f "$HOME/.ssh/id_ed25519_kitty_test" -N "" -q
        cat "$HOME/.ssh/id_ed25519_kitty_test.pub" >> "$HOME/.ssh/authorized_keys"
        chmod 600 "$HOME/.ssh/authorized_keys"
    fi
    if ! ssh -o StrictHostKeyChecking=no \
            -o BatchMode=yes \
            -o UserKnownHostsFile=/dev/null \
            -i "$HOME/.ssh/id_ed25519_kitty_test" \
            -p "$SSH_PORT" "$SSH_USER@$SSH_HOST" 'echo selftest' >/dev/null 2>&1; then
        err "ssh self-test failed: cannot ssh $SSH_USER@$SSH_HOST:$SSH_PORT"
        err "  the rest of the test cannot run."
        exit 1
    fi
    ok "preflight OK"
}

# -------- Xvfb (shared by both arch tests) --------
start_xvfb() {
    if [ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]; then
        log "Xvfb :$DISPLAY_NUM already running"
        return 0
    fi
    log "starting Xvfb :$DISPLAY_NUM"
    Xvfb ":$DISPLAY_NUM" -screen 0 1280x800x24 >/tmp/xvfb-test.log 2>&1 &
    XVFB_PID=$!
    sleep 2
    [ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ] || {
        err "Xvfb failed to start; see /tmp/xvfb-test.log"
        exit 1
    }
    ok "Xvfb up (PID $XVFB_PID)"
}

stop_xvfb() {
    pkill -f "Xvfb :$DISPLAY_NUM" 2>/dev/null || true
    sleep 1
}

# -------- per-arch build --------
build_arch() {
    local arch="$1"        # "cross" or "cross64"
    log "[$arch] cleaning + building"
    ( cd "$REPO_ROOT/0.76b_My_PuTTY/windows" \
        && make -f MAKEFILE.MINGW clean ) >/dev/null 2>&1
    ( cd "$REPO_ROOT" && ./build.sh "$arch" ) >/tmp/build-$arch.log 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        err "[$arch] build.sh exited $rc; see /tmp/build-$arch.log"
        return 1
    fi
    # Verify undefined references count
    local undef_count
    undef_count=$(grep -c "undefined reference" /tmp/build-$arch.log 2>/dev/null | head -1)
    undef_count=${undef_count:-0}
    if [ "$undef_count" -gt 0 ] && [ "$arch" = "cross" ]; then
        # cross64 has expected fallback || undef refs; cross should be zero
        err "[$arch] build had $undef_count undefined-reference errors"
        return 1
    fi
    ok "[$arch] build OK"
}

# -------- per-arch run + smoke test --------
test_arch() {
    local arch="$1"                  # "cross" or "cross64"
    local exe wine_prefix label kitname

    if [ "$arch" = "cross" ]; then
        kitname="kitty.exe"
        wine_prefix="$HOME/wine_prefixes/wp32"
        label="32-bit"
    else
        kitname="kitty64.exe"
        wine_prefix="$HOME/wine_prefixes/wp64"
        label="64-bit"
    fi

    exe="/builds/$kitname"
    [ -f "$exe" ] || { err "[$arch] $exe not present after build"; return 1; }

    local work="/tmp/kkit-$arch"
    rm -rf "$work" && mkdir -p "$work"
    cp "$exe" "$work/kitty.exe"

    # PuTTY-format private key
    puttygen "$HOME/.ssh/id_ed25519_kitty_test" \
        -o "$work/id.ppk" -O private 2>/dev/null

    log "[$label] launching kitty under wine"
    pkill -f "kitty.exe" 2>/dev/null || true
    pkill wineserver 2>/dev/null || true
    sleep 1

    nohup env WINEPREFIX="$wine_prefix" DISPLAY=":$DISPLAY_NUM" WINEDEBUG=-all \
        wine "$work/kitty.exe" \
            "$SSH_USER@$SSH_HOST" -P "$SSH_PORT" -i "$work/id.ppk" \
        > "$work/wine.log" 2>&1 &
    sleep 6

    # The kitty.exe process (not the wine launcher)
    if ! pgrep -f "/tmp/kkit-$arch/kitty.exe" >/dev/null; then
        err "[$label] kitty.exe failed to start"
        tail -10 "$work/wine.log" >&2
        echo "FAIL: process did not start" > "$work/result.txt"
        return 1
    fi

    # Screen A: host-key dialog
    DISPLAY=":$DISPLAY_NUM" import -window root "$work/screen-a.png" 2>/dev/null

    # Click Accept (button at approx (360, 330) in default 1280x800 layout)
    DISPLAY=":$DISPLAY_NUM" xdotool mousemove 360 330 click 1 2>/dev/null
    sleep 4

    # Screen B: shell prompt
    DISPLAY=":$DISPLAY_NUM" import -window root "$work/screen-b.png" 2>/dev/null

    # Send 3 shell commands
    DISPLAY=":$DISPLAY_NUM" xdotool search --name "KiTTY" windowactivate 2>/dev/null || true
    sleep 1
    DISPLAY=":$DISPLAY_NUM" xdotool type --delay 50 "uname -srm"
    DISPLAY=":$DISPLAY_NUM" xdotool key Return
    sleep 1
    DISPLAY=":$DISPLAY_NUM" xdotool type --delay 50 "echo 'hello-from-$arch'"
    DISPLAY=":$DISPLAY_NUM" xdotool key Return
    sleep 1
    DISPLAY=":$DISPLAY_NUM" xdotool type --delay 50 "ls /etc/os-release"
    DISPLAY=":$DISPLAY_NUM" xdotool key Return
    sleep 2

    # Screen C: post-commands
    DISPLAY=":$DISPLAY_NUM" import -window root "$work/screen-c.png" 2>/dev/null

    # Verification strategy:
    #   1. screen-c must exist and contain real terminal content (size > 8KB,
    #      cropped-window mean pixel value > screen-b's by a small margin).
    #   2. If `tesseract` is available, OCR screen-c and look for the
    #      expected output strings.  This is the strong check.
    #
    # We do NOT require screen-a != screen-b — the host-key dialog may
    # already be cached in the wine prefix's registry from a previous run,
    # in which case the first paint IS the shell prompt.

    local size_a size_b size_c
    size_a=$(stat -c %s "$work/screen-a.png" 2>/dev/null || echo 0)
    size_b=$(stat -c %s "$work/screen-b.png" 2>/dev/null || echo 0)
    size_c=$(stat -c %s "$work/screen-c.png" 2>/dev/null || echo 0)

    local pass=1 reasons=""
    [ "$size_c" -lt 8000 ] && { pass=0; reasons="$reasons screen-c too empty ($size_c B);"; }

    # Cropped-mean pixel test (KiTTY window covers approx 660x390 top-left)
    local mean_b mean_c
    mean_b=$(identify -format '%[fx:mean]' "${work}/screen-b.png[660x390+0+0]" 2>/dev/null || echo 0)
    mean_c=$(identify -format '%[fx:mean]' "${work}/screen-c.png[660x390+0+0]" 2>/dev/null || echo 0)
    # screen-c should have a strictly greater mean (more lit pixels = commands rendered)
    awk -v b="$mean_b" -v c="$mean_c" 'BEGIN { exit !(c > b) }' \
        || { pass=0; reasons="$reasons screen-c not denser than screen-b (mean_b=$mean_b, mean_c=$mean_c);"; }

    # OCR check (if available)
    if command -v tesseract >/dev/null 2>&1; then
        local ocr
        ocr=$(tesseract "$work/screen-c.png" - 2>/dev/null)
        local needed=("uname" "Linux" "hello-from-$arch" "os-release")
        for w in "${needed[@]}"; do
            if ! grep -qF "$w" <<<"$ocr"; then
                pass=0
                reasons="$reasons OCR missing '$w';"
            fi
        done
    fi

    local hash_a hash_b hash_c
    hash_a=$(md5sum "$work/screen-a.png" 2>/dev/null | awk '{print $1}')
    hash_b=$(md5sum "$work/screen-b.png" 2>/dev/null | awk '{print $1}')
    hash_c=$(md5sum "$work/screen-c.png" 2>/dev/null | awk '{print $1}')

    {
        echo "=== KiTTY smoke test: $label ($arch) ==="
        echo "exe       : $exe"
        echo "kitty.exe : /tmp/kkit-$arch/kitty.exe"
        echo "wine pfx  : $wine_prefix"
        echo "screen-a  : $work/screen-a.png ($size_a bytes, md5 $hash_a)"
        echo "screen-b  : $work/screen-b.png ($size_b bytes, md5 $hash_b)"
        echo "screen-c  : $work/screen-c.png ($size_c bytes, md5 $hash_c, mean $mean_c)"
        echo "screen-b mean (cropped): $mean_b"
        if [ $pass -eq 1 ]; then
            echo "result    : PASS"
        else
            echo "result    : FAIL ($reasons)"
        fi
    } | tee "$work/result.txt"

    # cleanup
    pkill -f "/tmp/kkit-$arch/kitty.exe" 2>/dev/null || true
    pkill wineserver 2>/dev/null || true
    sleep 1

    [ $pass -eq 1 ]
}

# -------- main --------
preflight
start_xvfb

OVERALL_FAIL=0

run_one() {
    local arch="$1"
    build_arch "$arch" || { OVERALL_FAIL=$((OVERALL_FAIL+1)); return; }
    test_arch  "$arch" || { OVERALL_FAIL=$((OVERALL_FAIL+1)); return; }
}

case "$ACTION" in
    cross)   run_one cross ;;
    cross64) run_one cross64 ;;
    both)
        run_one cross
        run_one cross64
        ;;
esac

stop_xvfb

echo
log "=========================================="
if [ $OVERALL_FAIL -eq 0 ]; then
    ok  "All requested architectures passed smoke test."
    log "Screenshots in /tmp/kkit-*/screen-*.png"
    exit 0
else
    err "$OVERALL_FAIL test(s) failed. See /tmp/kkit-*/result.txt"
    exit 1
fi
