#!/bin/bash
# scripts/pick-putty-commits.sh
#
# Cherry-pick PuTTY commits from 0.76 forward into KiTTY in batches.
#
# Strategy:
#   1. Walk 0.76..putty/main oldest-first, skipping merges and commits
#      that touch zero files KiTTY actually has.
#   2. For each remaining commit, format-patch it, rewrite paths so
#      `windows/`, `terminal/`, `crypto/`, `ssh/`, `proxy/`, `charset/`,
#      `unicode/`, `utils/`, top-level *.c/.h all map under
#      `0.76b_My_PuTTY/`.
#   3. `git am --3way --keep-cr` the rewritten patch.  If it fails for
#      any reason, abort and skip this commit, logging the SHA + reason.
#   4. Every BATCH_SIZE picked commits, run `./scripts/build-and-test.sh
#      both`.  If it fails, `git reset --hard` to before the batch and
#      record those SHAs as quarantined.
#   5. Persist progress (`last picked SHA`, skipped list, quarantine
#      list) to a state file so we can resume.
#
# Usage:
#   scripts/pick-putty-commits.sh              # process up to BATCH_LIMIT batches
#   scripts/pick-putty-commits.sh resume       # continue from last state
#   scripts/pick-putty-commits.sh status       # show progress
#   BATCH_SIZE=10 BATCH_LIMIT=1 ./scripts/...  # one batch of 10 then stop
#
# Output files (all under .pick-state/):
#   progress.txt        last picked PuTTY SHA
#   picked.log          one line per successful pick (KiTTY SHA <- PuTTY SHA)
#   skipped.log         one line per skipped commit + reason
#   quarantined.log     one line per commit that broke the build
#   batch-N.log         per-batch build+test stdout/stderr

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

STATE_DIR="$REPO/.pick-state"
mkdir -p "$STATE_DIR"
PROGRESS="$STATE_DIR/progress.txt"
PICKED_LOG="$STATE_DIR/picked.log"
SKIPPED_LOG="$STATE_DIR/skipped.log"
QUARANTINE_LOG="$STATE_DIR/quarantined.log"
touch "$PICKED_LOG" "$SKIPPED_LOG" "$QUARANTINE_LOG"

BATCH_SIZE="${BATCH_SIZE:-10}"
BATCH_LIMIT="${BATCH_LIMIT:-1000}"        # safety cap; default lots
REMOTE_REF="${REMOTE_REF:-putty/main}"

log()  { printf '\033[1;34m[pick]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[pick OK]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[pick WARN]\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31m[pick ERR]\033[0m %s\n' "$*" >&2; }

# --- status / resume entry points ---
if [ "${1:-}" = "status" ]; then
    echo "picked:      $(wc -l < "$PICKED_LOG")"
    echo "skipped:     $(wc -l < "$SKIPPED_LOG")"
    echo "quarantined: $(wc -l < "$QUARANTINE_LOG")"
    if [ -f "$PROGRESS" ]; then
        echo "last SHA:    $(cat "$PROGRESS")"
        echo "remaining:   $(git rev-list --count --no-merges "$(cat "$PROGRESS")..$REMOTE_REF")"
    fi
    exit 0
fi

# --- precheck ---
if ! git remote get-url putty >/dev/null 2>&1; then
    err "remote 'putty' not configured. Run:"
    err "  git remote add putty /path/to/putty"
    exit 1
fi
git fetch putty --tags >/dev/null 2>&1

# --- decide where to start ---
if [ -f "$PROGRESS" ]; then
    START="$(cat "$PROGRESS")"
    log "resuming after $START"
else
    START="0.76"
    log "starting fresh from $START"
fi

# --- helper: should we even try this commit? ---
# Returns 0 if commit touches at least one file KiTTY has under 0.76b_My_PuTTY/
should_attempt() {
    local sha="$1" files f
    files=$(git show --name-only --format='' "$sha" 2>/dev/null | grep -v '^$')
    [ -z "$files" ] && return 1
    while IFS= read -r f; do
        # Skip obviously KiTTY-irrelevant files
        case "$f" in
            CMakeLists.txt|*/CMakeLists.txt|cmake/*|cmake.h.in) continue ;;
            doc/*|*.md|.gitignore|.gitattributes|Buildscr*|licence.pl|mkfiles.pl) continue ;;
            unix/*|macosx/*|test/*) continue ;;
        esac
        if [ -e "0.76b_My_PuTTY/$f" ]; then
            return 0
        fi
    done <<<"$files"
    return 1
}

# --- helper: cherry-pick one commit with path rewriting ---
# Returns 0 success, 1 fail/skip.  Writes to picked/skipped logs.
attempt_pick() {
    local sha="$1" subject
    subject=$(git log --format=%s -1 "$sha" | head -c 80)

    # 1. Generate patch
    local patch="$STATE_DIR/.patch-$sha"
    if ! git format-patch -1 --stdout --no-stat "$sha" > "$patch" 2>/dev/null; then
        echo "$sha format-patch-failed $subject" >> "$SKIPPED_LOG"
        rm -f "$patch"
        return 1
    fi

    # 2. Rewrite paths: everything PuTTY references at top-level OR
    #    under windows/, terminal/, crypto/, ssh/, proxy/, charset/,
    #    unicode/, utils/, keygen/, otherbackends/, contrib/, icons/,
    #    stubs/ goes under 0.76b_My_PuTTY/.  Skip files KiTTY doesn't have.
    # Path rewriting done entirely in python below (more reliable than
    # alternating sed expressions).

    # Top-level .c / .h files (e.g. callback.c, defs.h) also live under 0.76b_My_PuTTY/.
    # Rewrite if the bare-name file exists there.
    # We do this with python for safety on the path manipulation.
    python3 - "$patch" "$REPO/0.76b_My_PuTTY" <<'PYEOF'
import sys, re, os
patch_path, kdir = sys.argv[1], sys.argv[2]
with open(patch_path) as f:
    text = f.read()

def rewrite(prefix, rest):
    # rest is something like "a/callback.c" or "b/foo/bar.c"
    side, rel = rest[0], rest[2:]
    # If already prefixed with 0.76b_My_PuTTY/, skip
    if rel.startswith('0.76b_My_PuTTY/'):
        return prefix + rest
    # Only rewrite if the bare-name OR top dir exists in 0.76b_My_PuTTY/
    candidate = os.path.join(kdir, rel)
    if os.path.exists(candidate):
        return prefix + side + '/0.76b_My_PuTTY/' + rel
    return prefix + rest

def fix_line(m):
    return rewrite(m.group(1), m.group(2))

text = re.sub(r'(^--- )([ab]/\S+)', fix_line, text, flags=re.M)
text = re.sub(r'(^\+\+\+ )([ab]/\S+)', fix_line, text, flags=re.M)
text = re.sub(r'(^diff --git )(\S+ \S+)', lambda m: m.group(1) + ' '.join(
    rewrite('', s).lstrip() for s in m.group(2).split()
), text, flags=re.M)
with open(patch_path, 'w') as f:
    f.write(text)
PYEOF

    # 3. Apply via git am (3-way merge if hunks need context).
    if git am --3way --keep-cr --whitespace=nowarn "$patch" >/tmp/pick-am.log 2>&1; then
        local new_sha
        new_sha=$(git rev-parse HEAD)
        echo "$new_sha <- $sha $subject" >> "$PICKED_LOG"
        rm -f "$patch"
        return 0
    fi

    # Apply failed.  Abort and skip.
    git am --abort 2>/dev/null
    local reason
    reason=$(tail -1 /tmp/pick-am.log | tr -d '\r' | head -c 100)
    echo "$sha am-failed: $reason | $subject" >> "$SKIPPED_LOG"
    rm -f "$patch"
    return 1
}

# --- main loop ---
batch_count=0
in_batch=0
batch_start_sha=$(git rev-parse HEAD)

COMMITS=$(git rev-list --reverse --no-merges "$START..$REMOTE_REF")

run_smoke_test() {
    local batch_idx="$1"
    local logfile="$STATE_DIR/batch-$batch_idx.log"
    log "smoke test (batch $batch_idx) -> $logfile"
    if ! "$REPO/scripts/build-and-test.sh" both > "$logfile" 2>&1; then
        return 1
    fi
    grep -q "All requested architectures passed" "$logfile"
}

for sha in $COMMITS; do
    # Always update progress so we can resume from any point
    echo "$sha" > "$PROGRESS"

    if ! should_attempt "$sha"; then
        local_subject=$(git log --format=%s -1 "$sha" | head -c 80)
        echo "$sha not-applicable | $local_subject" >> "$SKIPPED_LOG"
        continue
    fi

    log "trying $sha: $(git log --format=%s -1 "$sha" | head -c 60)"
    if attempt_pick "$sha"; then
        ok "picked $sha"
        in_batch=$((in_batch+1))
    fi

    if [ $in_batch -ge $BATCH_SIZE ]; then
        batch_count=$((batch_count+1))
        log "===== batch $batch_count complete ($in_batch picks); running smoke test ====="
        if run_smoke_test "$batch_count"; then
            ok "batch $batch_count passed smoke test"
            batch_start_sha=$(git rev-parse HEAD)
        else
            warn "batch $batch_count broke the build; reverting and quarantining"
            # Record the picks in this batch as quarantined, then reset
            git log --format='%H %s' "$batch_start_sha..HEAD" >> "$QUARANTINE_LOG"
            git reset --hard "$batch_start_sha"
            warn "reset to $batch_start_sha"
        fi
        in_batch=0

        if [ $batch_count -ge $BATCH_LIMIT ]; then
            log "BATCH_LIMIT=$BATCH_LIMIT reached, stopping"
            break
        fi
    fi
done

# Trailing partial batch
if [ $in_batch -gt 0 ]; then
    batch_count=$((batch_count+1))
    log "===== trailing batch $batch_count ($in_batch picks); smoke test ====="
    if run_smoke_test "$batch_count"; then
        ok "trailing batch $batch_count passed"
    else
        warn "trailing batch broke build; reverting"
        git log --format='%H %s' "$batch_start_sha..HEAD" >> "$QUARANTINE_LOG"
        git reset --hard "$batch_start_sha"
    fi
fi

echo
log "DONE"
echo "  total picked:      $(wc -l < "$PICKED_LOG")"
echo "  total skipped:     $(wc -l < "$SKIPPED_LOG")"
echo "  total quarantined: $(wc -l < "$QUARANTINE_LOG")"
echo "  batches run:       $batch_count"
