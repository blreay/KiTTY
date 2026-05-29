# KiTTY 0.76 → PuTTY 0.84 Port Report

- Branch: `fork_init_test02_pick_putty` (2100c161, pushed)
- Base: `master` (cd0a5ca9)
- Span: 81 commits · 83 files · +6011 / −431 lines
- Verified: `./scripts/build-and-test.sh both` — 32-bit + 64-bit PASS

## Work breakdown (chronological)

| Phase | Commits | Outcome |
|---|---|---|
| 1. GCC 13 build unbreak | 2 | i686 + x86_64 cross builds compile clean |
| 2. Font fallback (MOD_PERSO) | 18 | New `winfont_fallback.{c,h}` (~815 LoC); INI `[FontFallback]`; logger; user list prepended to defaults |
| 3. URL underline / SSH segfault | 5 | Root cause: LP64 vs LLP64 ABI mismatch in vendored `libregex_64.a` |
| 4. Build infra | 6 | `_32.a` / `_64.a` rename + `LIBSUFFIX`; wine env + smoke-test scripts |
| 5. Bulk PuTTY pick | 1 | 33 trivially-applicable upstream commits (of 1186 scanned 0.76→0.84) |
| 6. PuTTY 0.77 → 0.84 release-by-release | 21 | 27 feature ports + 5 status docs |
| 7. Docs | 8 | CLAUDE.md, runbooks, postmortems, design specs |

## Security fixes (high priority)

| CVE / class | Commit | Note |
|---|---|---|
| **CVE-2023-48795** Terrapin | `3d6ab81e` | `kex-strict-{c,s}-v00@openssh.com` + BPP seq-num reset; PuTTY `244be541` |
| **CVE-2024-31497** P521 ECDSA nonce bias | `b55be85a` | `dss_gen_k` proto_k extended 64→128 bytes (hot-fix vs upstream's full RFC 6979) |
| RSA kex double-free | `707e23dc` | Drop extra `sfree` after `ssh_rsakex_freekey`; PuTTY `ba3ed53e` |
| ECDSA verify crash (crafted P256) | `707e23dc` | Remove bogus `ecc_weierstrass_add` assert; PuTTY `65b8f37c` / `7db1f655` |
| ECDSA `add_general` doubling | `707e23dc` | Tangent over common denom; PuTTY `c15d4d7b` |
| EdDSA out-of-range `s` | `707e23dc` | RFC 8032 check; PuTTY `af996b5e` |
| Telnet/Rlogin trust sigil after proxy auth | `707e23dc` | Defer to `PLUGLOG_CONNECT_SUCCESS`; PuTTY `64712be3` |
| RSA SHA-2 sig padding (RFC 8332) | `3b8242d4` | Pad to modulus length; PuTTY `a5bcf3d3` |
| DH short-exponent | `62b308ad` | Use full-length DH exponents |
| 64-bit URL underline / SSH segfault | `c1ff448d` `5c0a369e` | Regex ABI: KiTTY hdr LLP64 vs lib LP64 → wrong `re_nsub` offset, `rm_eo=0` |

## Feature ports per release

| Release | Done | Skipped | N/A | Total |
|---|---|---|---|---|
| 0.77 | 5 (+8 bulk) | 7 | 6 | 26 |
| 0.78 | 3 | 4 | 5 | 12 |
| 0.79 | 3 (+1 bulk) | 6 | 4 | 14 |
| 0.80–0.82 | 6 | 1 | 0 | 7 |
| 0.83–0.84 | 9 | 3 | 6 | 22 |

Per-release detail: `docs/superpowers/runbooks/putty-{077,078,079,080-082,083-084}-port-status.md`.

## Files changed (top by churn)

```
+1662  docs/superpowers/plans/2026-05-27-kitty-font-fallback.md (impl plan)
+ 713  0.76b_My_PuTTY/windows/winfont_fallback.c                (new module)
+ 376  docs/superpowers/specs/...kitty-font-fallback-design.md
+ 390  docs/superpowers/runbooks/wine-debug-kitty.md
+ 302/-83  0.76b_My_PuTTY/terminal/terminal.c
+ 281  docs/superpowers/postmortems/...regex-abi-mismatch...
+ 298  scripts/build-and-test.sh
+ 250  scripts/pick-putty-commits.sh
+ 218/-32  0.76b_My_PuTTY/windows/window.c
+ 184  scripts/setup-wine-debug-env.sh
+ 214  CLAUDE.md
+ 160  docs/superpowers/postmortems/...urlhack-64bit-underline.md
+ 102  0.76b_My_PuTTY/windows/winfont_fallback.h
```

## KiTTY-specific additions (not from PuTTY)

1. **Font fallback** — Probe missing glyphs via `GetGlyphIndicesW`, split text into per-font runs, draw each clipped to the cell. Config in `[FontFallback]` (`Log=`, `Fallback=`, `Override=`). Logger writes beside the EXE.
2. **`build.sh` cross / cross64** — Wrapper around `MAKEFILE.MINGW` with `-j$(nproc)`, single source of truth.
3. **Vendored libs `_32.a` / `_64.a`** — `LIBSUFFIX` selects per arch; eliminates the `cp xxx_64.a xxx.a` trap.
4. **`scripts/setup-wine-debug-env.sh`** — Installs wine + Xvfb + xdotool + tesseract + gdb + sshd + puttygen + per-arch wine prefixes.
5. **`scripts/build-and-test.sh both|cross|cross64`** — clean → build → wine launch → SSH localhost → xdotool drive → OCR verify → PASS/FAIL.
6. **CLAUDE.md** — Onboarding for future sessions: build, verify, debug 64-bit weirdness via `gdb -p $(pgrep kitty)`.

## Verification

```
=== KiTTY smoke test: 32-bit (cross) ===   result: PASS
=== KiTTY smoke test: 64-bit (cross64) === result: PASS
```

Both arches: clean build → wine launch → host-key dialog → ssh `whoami@localhost` → `uname -srm` / `echo hello-from-<arch>` / `ls /etc/os-release` → tesseract OCR matches `uname` / `Linux` / `hello` / `os-release`.

Artifacts in `/builds/`: `kitty.exe`, `kitty_nocompress.exe`, `kitty_portable.exe`, `klink.exe`, `kscp.exe`, `ksftp.exe`, `kageant.exe`, `kittygen.exe` × {32, 64} = 14 binaries.

## Items deliberately skipped (and why)

| Item | Reason |
|---|---|
| Pre-connection command hook (0.84) | KiTTY already has `AutoCommand` / `OnRunCommand` — risk of conflict |
| ML-KEM post-quantum kex (0.83) | Multi-thousand-LoC addition; needs SHAKE variants |
| Unicode `request_file` rewrite (0.83) | KiTTY's `request_file` has different signature; every caller would change |
| HTTP-proxy stale-netevent (0.84) | Depends on `delete_callbacks()` helper not in KiTTY 0.76 |
| Alt+Fn xterm-216 (0.82) | KiTTY's keyboard layer lacks `consumed_alt` parameter |
| OpenSSH cert auth (0.77) | Not ported in KiTTY at all |
| HTTP digest qop list (0.84) | KiTTY 0.76 lacks HTTP digest path |
| Banner-on-close (0.79) | Needs `final_output` slot in `PacketProtocolLayerVtable` |
| Pre-edit / Wayland (0.84) | Unix only |

## Where to look next

- **Anything 64-bit weird:** suspect ABI mismatch with a vendored `.a` (see regex postmortem)
- **Adding features:** read `CLAUDE.md` first, then port-status runbooks for context
- **Debugging:** `docs/superpowers/runbooks/wine-debug-kitty.md` — full gdb-under-wine walkthrough
- **Continuing PuTTY picks:** `scripts/pick-putty-commits.sh` with path-rewrite for KiTTY layout
