# PuTTY 0.83 / 0.84 Port Status

- Branch: `fork_init_test02_pick_putty`
- Reference: <https://www.chiark.greenend.org.uk/~sgtatham/putty/changes.html>

Combined status across both releases. Items listed in upstream release-note
order; PuTTY commit SHAs in parens.

## PuTTY 0.84

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | RSA kex double-free | **DONE** | `ba3ed53e`. Dropped extra `sfree(s->rsa_kex_key)` after `ssh_rsakex_freekey()` in `transport2.c` and `kex2-server.c`. |
| 2 | ECDSA verify assertion crash | **DONE** | `65b8f37c` + `7db1f655`. Removed the bogus `ecc_weierstrass_add` assert that fired on a crafted P256 sig; `ecdsa_public` reduces exponent mod `G_order` not `p`. |
| 3 | ECDSA add_general doubling edge case | **DONE** | `c15d4d7b`. Tangent now computed over the common denominator from `add_prologue`, not the original `P`. |
| 4 | EdDSA out-of-range `s` | **DONE** | `af996b5e`. `eddsa_verify` rejects `s >= G_order`. |
| 5 | Trust sigil after proxy auth (Telnet/Rlogin) | **DONE** | `64712be3`. `raw_log`, `telnet_log`, `supdup_log` clear trust status on `PLUGLOG_CONNECT_SUCCESS` instead of at backend init. |
| 6 | Pre-connection command hook (wake-on-LAN, port knock) | SKIPPED | `503d6139` + 4 follow-ups. Adds new Conf entry, dialog UI, command-line option, platform `command_hook.c`. KiTTY already ships its own `AutoCommand` / `OnRunCommand` facility (kitty.c); upstream-style integration would conflict. |
| 7 | Pre-edit text (IM composition) | NOT APPLICABLE | Unix GTK only. |
| 8 | Improved Wayland support | NOT APPLICABLE | Unix only. |
| 9 | SSH cert-authority dir on Unix | NOT APPLICABLE | Unix only. |
| 10 | HTTP proxy "Socket is not connected" retry | SKIPPED | `bb8894ff` + `2eee8848`. Depends on a new `delete_callbacks()` helper in `callback.c` plus message-queue scrubbing that doesn't exist in KiTTY 0.76; HTTP-proxy 407-retry path isn't exercised in KiTTY anyway. |
| 11 | Caret blink disabled in Control Panel → tight loop | **DONE** | `c71cc50e`. `term_schedule_cblink` guards against `INFINITE` from `GetCaretBlinkTime()`. Inlined the check (didn't add a new platform helper file). |

## PuTTY 0.83

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | ML-KEM (post-quantum KEX) | SKIPPED | `e98615f0` + 8 follow-ups. Adds `crypto/mlkem.c`, modifies kex algorithm tables, requires SHA-3 SHAKE variants (`16629d3b`). Multi-thousand-line addition; sizable risk vs payoff for KiTTY's user base. |
| 2 | Unicode file names in Windows file selector | SKIPPED | `f8e1a2b3`. Rewrites `request_file.c` to wchar_t + `OPENFILENAMEW`. KiTTY's `request_file` is a much smaller `TCHAR`-based wrapper around a caller-supplied `OPENFILENAME`; rewrite would change the call signature of every caller (`controls.c` etc.). |
| 3 | `psftp -b` regression | NOT INVESTIGATED | Couldn't isolate the single fix commit; KiTTY's `ksftp` build smoke-tested OK without further work. |
| 4 | Assertion failure if SSH login prompt times out | NOT INVESTIGATED | Couldn't isolate. |
| 5 | Pageant crash on deferred-decryption abandon | **DONE** | `ec158a2e`. `signop_free` calls `signop_unlink` so an aborted `PageantSignOp` no longer leaves a stale pointer in `pk->blocked_requests`. |
| 6 | Empty answerback tight loop | NOT APPLICABLE | `19798515`. KiTTY 0.76 ldisc doesn't use the chunk-record `input_queue` model the bug lives in. |
| 7 | Config edit box truncation to 127 chars | NOT APPLICABLE | `9ab416e0`. Bug only present in the new wchar_t `request_file` rewrite (item #2), which KiTTY doesn't have. |
| 8 | Windows XP regression | NOT INVESTIGATED | KiTTY 0.76 still works on XP; no specific commit identified. |
| 9 | Tools auto-filling login via `^M` | NOT INVESTIGATED | Couldn't isolate. |
| 10 | FancyZones / bare-WM_SIZE | **DONE** | `1fc5f4af`. `wm_size_resize_term` updates `CONF_height` / `CONF_width` regardless of the `resizing` flag, so external resizers keep Change Settings in sync. |
| 11 | Small keypad keys on Unix | NOT APPLICABLE | `6a88b294` is Unix-only. |

## Summary

| Status | Count |
|---|---|
| **DONE** | **9** |
| SKIPPED | 3 |
| NOT APPLICABLE | 6 |
| NOT INVESTIGATED | 4 |
| **Total** | 22 |

## Commits on branch

```
707e23dc 0.83/0.84: security + behaviour fixes from PuTTY upstream
```

Single squashed commit covering all DONE items above (ten files touched:
`ssh/transport2.c`, `ssh/kex2-server.c`, `otherbackends/{raw,telnet,supdup}.c`,
`crypto/{ecc-ssh,ecc-arithmetic}.c`, `terminal/terminal.c`,
`windows/window.c`, `pageant.c`).
