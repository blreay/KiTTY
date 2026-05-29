# PuTTY 0.80 / 0.81 / 0.82 Port Status

- Branch: `fork_init_test02_pick_putty`
- Reference: <https://www.chiark.greenend.org.uk/~sgtatham/putty/changes.html>

Coverage of the three release-note clusters that motivated this batch
of cherry-picks. Items are grouped by upstream release.

## PuTTY 0.80

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Strict-kex / Terrapin (CVE-2023-48795) | **DONE** | PuTTY `244be541`. Ports kex-strict-{c,s}-v00@openssh.com signalling, BPP sequence-number reset on NEWKEYS, pre-NEWKEYS extraneous-packet rejection. `ssh2_bpp_new_{in,out}going_crypto` grew `reset_sequence_number` arg; transport2 plumbs `s->strict_kex`, `s->seen_non_kexinit`, `s->enabled_*_crypto`. |
| 2 | Scrollback reset rationalisation | **DONE** | PuTTY `73b41feb`. Replaced flag-based `seen_disp_event` with eager scroll/cblink reset inside `seen_disp_event()`. `term_seen_key_event` reads `scroll_on_key` directly. KiTTY-specific MOD_PERSO DECSCUSR site updated. |

## PuTTY 0.81

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | CVE-2024-31497 — biased DSA/ECDSA nonces on P521 | **DONE** | PuTTY `c193fe98` (full RFC 6979 rewrite). KiTTY took the minimal hot-fix path: extend `proto_k` from 64 bytes to 128 bytes via two SHA-512 hashes with different prefixes, so the mod-(q-2) reduction is meaningful for q < 2^1024. Same security property as RFC 6979, far smaller diff. See `0.76b_My_PuTTY/crypto/dsa.c`. |

## PuTTY 0.82

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Pad RSA SHA-2 signature blobs | **DONE** | PuTTY `a5bcf3d3`. `rsa2_sign` now pads to modulus byte length when `flags != 0` (rsa-sha2-256 / rsa-sha2-512). Legacy ssh-rsa unchanged. |
| 2 | Xterm-216+ Alt-Fn fix | SKIPPED | PuTTY `89c88253`. KiTTY's `format_function_key` / `format_arrow_key` family lack the `consumed_alt` out-parameter that this fix manipulates. Porting would require the full upstream refactor of the keyboard layer. |
| 3 | Respect `window_border` on maximise | **DONE** | PuTTY `31ab5b8e`. `reset_window()`'s zoomed branch now subtracts `2*window_border` before computing font cell dimensions; both the resize-by-font and resize-by-term code paths updated. |
| 4 | `wrapnext` only in auto-wrapping mode | **DONE** | PuTTY `3c3c1792`. In DECAWM-off mode, `term_display_graphic_char` no longer arms `wrapnext`, so backspace at the rightmost column really moves the cursor back. `term_reconfig` and the DECAWM toggle case clear `wrapnext` / `alt_wnext` on transition to non-wrap. Atari `ESC w` autowrap-off also clears `wrapnext`. |

## Summary

| Status | Count |
|---|---|
| **DONE** | **6** |
| SKIPPED | 1 |
| **Total** | 7 |

## Commits on branch for 0.80 / 0.81 / 0.82 work

```
b55be85a fix(dsa): CVE-2024-31497 - extend proto_k to 1024 bits to defeat P521 nonce bias
3d6ab81e 0.80/0.82: anti-Terrapin strict-kex, wrapnext gate, maximize border
3b8242d4 0.80/0.82: RSA SHA-2 padding, scrollback reset rationalisation
```
