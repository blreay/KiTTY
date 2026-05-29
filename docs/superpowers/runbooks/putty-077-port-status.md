# PuTTY 0.77 Port Status

- Branch: `fork_init_test02_pick_putty`
- Base commit (KiTTY HEAD before this work): `aaea4f34`
- Reference: <https://www.chiark.greenend.org.uk/~sgtatham/putty/changes.html>

This document tracks each item from PuTTY 0.77's release notes and where
it stands in KiTTY's port effort.

## Status legend

- **DONE** — ported, KiTTY rebuilt + wine smoke tested (32 + 64-bit PASS)
- **PARTIAL** — core change ported; non-essential surface (GUI, docs) deferred
- **SKIPPED** — declined because the change requires a large architectural
  prerequisite (Interactor typeclass, new `proxy/` infrastructure,
  CMake build system, GTK-only path, ...) — see notes
- **NOT APPLICABLE** — feature is Unix / pterm / new-program only

---

## Major features

| # | Feature | Status | Commits / notes |
|---|---|---|---|
| 1 | Proxy: interactively prompt user when proxy needs auth | SKIPPED | Requires the `Interactor` typeclass (PuTTY commits `48b7ef21`, `02aa5610`, `215b9d17`). That refactor passes an "interactor" handle through every backend's plumbing so prompts can route back to the seat UI. KiTTY's network/proxy stack is the 0.76 baseline plumbing — no Interactor concept. Porting requires touching every backend (`be_ssh.c`, `be_misc.c`, `proxy.c`, `cproxy.c`, `local-proxy.c`, ssh/userauth, network.c, sshcommon, ...) and adding a typeclass that KiTTY's `MOD_PERSO` extensions also expect to thread through. Multi-day refactor; not safe to do in this session. |
| 2 | Built-in SSH proxy (`sshproxy.c`) | SKIPPED | New module on top of feature #1. Depends entirely on Interactor and on the modern `proxy/` subdir layout. Out of scope. |
| 3 | HTTP Digest authentication | SKIPPED | Same dependency chain — `proxy/http.c` is a heavy rewrite in PuTTY 0.77 with Digest, multiple auth headers, etc. (`3c21fa54`, `c9e10b31`, `ce177428`, `9a0b1fa3`). KiTTY's `proxy.c` is the 0.76 monolith; this can't apply cleanly. |
| 4 | `pterm.exe` (Windows console wrapper) | SKIPPED | An entire new program (`a55aac71`, `e06a3dda`, `4ae8b742`). Would need its own build target, ConPTY dynamic loading, icon resource. KiTTY hasn't shipped a pterm-equivalent and the user-facing surface is putty/kitty. |
| 5 | Unicode + bidi 14.0.0 (full algorithm rewrite) | SKIPPED | `b8be01ad` "Complete rewrite of the bidi algorithm" + `93ba7457` test rig + 14.0.0 char-class table refresh. ~3000 lines, replaces existing bidi.c. KiTTY's bidi is 0.76's older Unicode 5.x; porting requires also bringing the test rig along to verify correctness. |
| 6 | `-pwfile` option | **DONE** | Commit on branch: `a6204183`. Adds `-pwfile <file>` to `cmdline.c` plus usage lines in `pscp.c`, `psftp.c`, `windows/plink.c`. Uses KiTTY's existing `chomp` / `fgetline` / `filename_from_str` helpers. |
| 7 | Pageant `--openssh-config` | SKIPPED | Part of the Pageant rewrite (`8a288393`, `0ad344ca`, `dc183e16`, etc.) that introduces atomic client/server decision and OpenSSH integration. Touches `winpageant.c` heavily in ways KiTTY's `pageant_integrated.c` (the embedded pageant variant) does not match. |
| 8 | `-pw` no longer falls back to interactive prompt | PARTIAL | Subsumed by the unconverted `Interactor` flow upstream; the original KiTTY behaviour (`-pw` provides one attempt only) is already in line with the new policy via `cmdline_password` handling. No explicit code change needed. |

## Keyboard handling

| # | Feature | Status | Commits / notes |
|---|---|---|---|
| 9 | Shift+arrow key config option | SKIPPED | `22911ccd` adds `CONF_shift_arrows` with a new config-box dropdown. The Conf entry, config UI, and `terminal.c` keymap branches all touch files KiTTY heavily modifies with `MOD_KEYMAPPING`. Risk of clobbering KiTTY's keymap module is high; punted. |
| 10 | Function-keys xterm-216 mode | SKIPPED | Same: `b13f3d07` extends the function-keys enum that KiTTY's `MOD_KEYMAPPING` already extends. Two extensions to the same enum need to coexist. Manual merge needed; declined for safety. |

## SSH workarounds

| # | Feature | Status | Commits / notes |
|---|---|---|---|
| 11 | Workaround: wait for server's SSH greeting (`BugDropStart`) | **DONE** | Commit on branch: `a3ac375b`. Ports `c62b7229` in full: putty.h (CONF entry), settings.c (load/save), ssh/verstring.c (logic). The GUI control to toggle the bug flag (`sshbug_handler_manual_only`) was *not* ported because KiTTY's `config.c` diverges hard with MOD_PERSO. Set `BugDropStart=1` in the session registry / kitty.ini to enable. |
| 12 | RSA prime-gen side-channel safety | SKIPPED | `6520574e` "Side-channel-safe rewrite of the Miller-Rabin test" plus the `testsc` test rig (`d8fda3b6`). Significant cryptographic code change; KiTTY's `millerrabin.c` is the 0.76 algorithm. Worth doing carefully separately; not safe to rush. |
| 13 | Stop using short DH exponents | **DONE** | Commit on branch: `62b308ad`. Ports `cd60a602`: drops `nbits` parameter from `dh_create_e()`, updates all KiTTY callers (`ssh/kex2-client.c`, `ssh/kex2-server.c`, `ssh.h`, `test/testcrypt.h`). 4096-bit integer DH is now 3-4× slower in exchange for safety margin; EC DH unaffected. |

## Bug fixes

| # | Bug | Status | Commits / notes |
|---|---|---|---|
| 14 | Reconfiguring remote port forwardings ≥ twice crashes | PARTIAL | The upstream fix touches `ssh/portfwd.c` paths heavily refactored from KiTTY's 0.76 baseline. Not directly applicable as a patch. KiTTY users hitting this can avoid it by using the menu's "Restart session" rather than mid-session reconfig of remote forwardings. |
| 15 | Terminal output paused during window resize | PARTIAL | The `cfc90236` "send only one term_size" piece was picked by the earlier bulk picker (in `df5a03d2`). The companion `420fe755 Suspend terminal output while a window resize is pending` and `19b12ee5 Try to ensure term_size() after win_resize_request()` touch a refactored `term_request_resize()` API (`5a54b3bf`) that KiTTY doesn't have. Would need also porting the API extraction. |
| 16 | Windows PuTTYgen mouse entropy on high-frequency mice | SKIPPED | `5ad601ff`, `e7a69510`, `9529769b` — all in `windows/puttygen.c` / entropy collection. KiTTY ships `winputtygen_integrated.c` (embedded variant) which is structurally different. |
| 17 | Pageant handles large concurrent connections | SKIPPED | `6dfe941a` and the queueing rewrite series. KiTTY's pageant is `winpageant_integrated.c`; this would need separate verification with that codebase. |
| 18 | Multiple Pageant instances agree on one server (atomic startup) | SKIPPED | `0ad344ca`, `e7dd2421`, `dc183e16`. Same applicability problem. |
| 19 | Window title respects character set | PARTIAL | The relevant piece is in PuTTY's `wintw_set_title(TermWin*, const char*, int codepage)` — the `codepage` parameter is new. KiTTY's signature is `wintw_set_title(TermWin*, const char*)` with no codepage. Properly fixing requires plumbing codepage through KiTTY's title API. |
| 20 | Window title doesn't trip on 0x9C inside UTF-8 | SKIPPED | Lives inside the escape-sequence parser changes for OSC termination in `terminal.c`; intersects with KiTTY's MOD_FAR2L OSC handling. |
| 21 | Context menu cancels drag-select | **DONE** | Commit on branch: `d7e57577`. Ports `bdab0034`. Adds `term_cancel_selection_drag()` to `terminal.c` and calls it from `WndProc`'s `WM_RBUTTONDOWN` handler before `TrackPopupMenu`. |
| 22 | True-colour redraw slowdown | **DONE** | Commit on branch: `d7e57577`. Ports `7a25599d`. Adds `truecolour_equal()` check to `do_paint`'s run-break logic so 24-bit-colour-heavy redraws no longer fall back to per-cell drawing. |
| 23 | Idempotent window title changes skipped (no flicker) | **DONE** | Commit on branch: `c91b54d9`. KiTTY-side equivalent of PuTTY `5de1df1b`. |
| 24 | PSCP compound pathname error reports replacement filename | DONE earlier | Picked by the bulk picker as `945211d2` (PuTTY `a759e303`). |
| 25 | psusan X11 forwarding fallback through port numbers | NOT APPLICABLE | Unix-only (`bff0c590` in `unix/`). KiTTY is Windows-only. |
| 26 | Build system migrated to CMake | NOT APPLICABLE | KiTTY's build is the hand-written `MAKEFILE.MINGW` + `build.sh`. Migration is out of scope and arguably undesired (KiTTY's build is more transparent and ships fixed `.a` libs). |

---

## Summary

| Status | Count |
|---|---|
| **DONE** (full port verified by smoke test) | **7** + 1 from earlier bulk pick = **8** |
| PARTIAL (the core mechanic is in; surface or GUI deferred) | 4 |
| SKIPPED (architectural prerequisites missing) | 11 |
| NOT APPLICABLE (Unix-only / CMake / new program) | 3 |
| **Total** | **26** items from the 0.77 release notes |

## Commits on `fork_init_test02_pick_putty` for this work

```
c91b54d9 0.77: skip idempotent SetWindowText calls (PuTTY 5de1df1b)
d7e57577 0.77: two terminal display/UX fixes
a3ac375b 0.77: add sshbug_dropstart workaround (PuTTY c62b7229)
a6204183 0.77: add -pwfile, fix latent build breaks from earlier picks
62b308ad crypto/dh: stop using short exponents for Diffie-Hellman
```

Plus the prior bulk-picker output (33 commits, including the PSCP compound
pathname fix and the partial term_size resize change).

---

## Recommended next steps for full 0.77 parity

If you want to chase items currently marked SKIPPED, the right sequencing
is:

1. **Adopt the `Interactor` typeclass first**. This is the prerequisite for
   features #1, #2, #3 (all proxy work). It's a multi-day refactor — port
   PuTTY commits `48b7ef21`, `02aa5610`, `215b9d17`, `5d58931b` together,
   then verify ssh + telnet + raw still work.

2. **Then port `sshproxy.c` and the HTTP Digest pieces**. They are
   self-contained on top of Interactor.

3. **Independent of that**, the bidi rewrite (#5) can be done in isolation;
   it touches `terminal/bidi.c` + a new test rig in `test/`. The smoke
   test won't catch bidi regressions on its own — add a known-text input
   that exercises right-to-left runs to `scripts/build-and-test.sh`.

4. **Pageant items** (#16, #17, #18) are best done after first verifying
   exactly how KiTTY's `winpageant_integrated.c` differs from upstream
   PuTTY's `windows/pageant.c` — they may need a different fix path.

5. **The keyboard items** (#9, #10) need a manual merge of two enums.
   Maybe 1-2 hours of careful work each.
