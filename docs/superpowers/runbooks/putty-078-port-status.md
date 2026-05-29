# PuTTY 0.78 Port Status

- Branch: `fork_init_test02_pick_putty`
- Reference: <https://www.chiark.greenend.org.uk/~sgtatham/putty/changes.html>

## Status legend
Same as `putty-077-port-status.md`.

## 12 release-note items

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Windows installer scope = per-user (workaround) | NOT APPLICABLE | KiTTY has no MSI installer. |
| 2 | OpenSSH certificates (user + host) | SKIPPED | Large new subsystem (`utils/cert-expr.c`, `keygen/opensshcert.c`, multiple userauth changes). Depends on PuTTY's modern keygen + crypto/ layout that KiTTY hasn't adopted. |
| 3 | SSH proxy: shell command / subsystem modes | SKIPPED | Builds on the 0.77 SshProxy + Interactor infrastructure that KiTTY hasn't ported. |
| 4 | K-I auth helper plugin system | SKIPPED | `15f097f3` and `1f32a16d` factor out k-i code into a new plugin protocol with subprocess spawning. Substantial new feature. |
| 5 | NTRU Prime PQ key exchange | SKIPPED | Whole new `crypto/ntru.c` module (~1500 lines) plus algorithm registration. Pure-add but needs `crypto/` subdir restructuring. |
| 6 | AES-GCM (OpenSSH style) | SKIPPED | New cipher implementation (`crypto/aesgcm-*.c`), KEX layer changes, RFC 5647 vs OpenSSH protocol IDs. Multi-day. |
| 7 | More DH forms (group16/18, GSS-ECDH) | SKIPPED | KiTTY doesn't yet have GSS-ECDH at all; adding group16/18 alone is feasible but requires the larger group definitions and changes through kex2-{client,server}.c that depend on the PuTTY 0.77+ crypto/ layout. |
| 8 | SSH configuration panels reorganised (Credentials) | SKIPPED | Pure UI rework of windows/controls.c + config.c around the Auth/Credentials split. KiTTY's config.c has MOD_PERSO mods that overlap here; safe merge would require manual review. |
| 9 | 32-bit Windows build runs on Windows XP again | NOT INVESTIGATED | Comes from build/compiler flag tweaks. KiTTY's MinGW-w64 build targets `_WIN32_WINNT=0x0501` already (XP) — likely already works. |
| 10 | Window title with ISO-8859 charset | PARTIAL | Upstream fix is inside the codepage-aware `wintw_set_title(TermWin*, const char*, int codepage)` rewrite that KiTTY hasn't picked. KiTTY's title path is char* throughout, so the symptom may not even apply. |
| 11 | OSC crash from certain escape sequences | **DONE** | Commit `6d4aebf7`. Ports `a7106d8e` (init osc_strlen) + `de66b031` (accept BEL/ESC\\/0x9C/UTF-8 0xC2 0x9C as OSC terminators in SEEN_OSC and SEEN_OSC_W). Added new termstate `OSC_MAYBE_ST_UTF8`. |
| 12 | `-pwfile`/`-pw` server-only + plink anti-spoof | **DONE** | Commit `07d9f435`. Ports `bdb3ac9f` — `cmdline_get_passwd_input()` now also requires `p->to_server`, and the antispoof prompt in `ssh/connection2.c` is correctly marked `to_server=false`. |
| – | Duplicated 'w' shortcut on Auth panel | **DONE** | Commit `5a200f30`. Ports `3bb7e6ba` — removes the hardcoded 'w' accelerator on Browse buttons so two file selectors on the same panel don't assertion-fail. |

## Summary

| Status | Count |
|---|---|
| **DONE** (full port verified by smoke test) | **3** |
| PARTIAL | 1 |
| SKIPPED (large feature / architectural prerequisite missing) | 7 |
| NOT APPLICABLE / NOT INVESTIGATED | 2 |
| **Total** | 12 + 1 (incidental) = 13 |

## Commits on branch for 0.78 work

```
5a200f30 0.78: drop hardcoded 'w' shortcut on Browse buttons
07d9f435 0.78: restrict -pwfile / -pw to server prompts only
6d4aebf7 0.78: OSC parsing — terminate early + initialise osc_strlen
```
