# PuTTY 0.79 Port Status

- Branch: `fork_init_test02_pick_putty`
- Reference: <https://www.chiark.greenend.org.uk/~sgtatham/putty/changes.html>

## 15 release-note items

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Installer scope → per-machine | NOT APPLICABLE | No MSI installer in KiTTY |
| 2 | Terminal mouse tracking: non-drag movements (any-event) | SKIPPED | PuTTY commit `0112167f`. KiTTY already has a partial MBT_NOTHING / any-event hack under MOD_PERSO; merging cleanly with the upstream `xterm any-event` mode requires care |
| 3 | Horizontal scroll events | **DONE** | Commit `e25d51ef` ports `1526b563`. New MBT_WHEEL_LEFT/RIGHT, WM_MOUSEHWHEEL handling, no fall-through to vertical scrollback |
| 4 | Certificate auth OpenSSH 7.7 backward-compat | SKIPPED | OpenSSH cert support itself wasn't ported in 0.78. Nothing to fix |
| 5 | Raw protocol ^D twice → assertion | NOT INVESTIGATED | Probably small but I didn't find the specific commit |
| 6 | Terminal hang on resize when window non-resizable | SKIPPED | Requires the term_request_resize() API extraction that wasn't ported in 0.77 |
| 7 | GTK resize assertion in KDE | NOT APPLICABLE | Unix-only |
| 8 | GTK resize assertion when maximised | NOT APPLICABLE | Unix-only |
| 9 | Unix bell-overload Unix-only fix | NOT APPLICABLE | Unix-only (`7e8be5a2`) |
| 10 | SSH userauth banner shown on close | SKIPPED | Requires a new `final_output` slot in PacketProtocolLayerVtable that KiTTY's `ssh/ppl.h` doesn't have. Adding it cleanly means touching every layer's vtable + the Ssh-side teardown plumbing |
| 11 | PSFTP 'close' returns success | **DONE** earlier | Already in via bulk-pick (PuTTY `6370782d` → KiTTY `ae7b7a87`) |
| 12 | Cert: detached RSA with PPK | NOT APPLICABLE | Bug is in detached-cert code; KiTTY has no detached-cert subsystem |
| 13 | Pageant OpenSSH config with space in user name | SKIPPED | KiTTY uses integrated Pageant variant, OpenSSH-config feature itself not ported |
| 14 | Local-line-edit `^U` no longer self-inserts | **DONE** | Commit `0b6cab93` ports `1405659d` |
| 15 | Backspace respects LATTR_WRAPPED2 | **DONE** | Commit `0b6cab93` ports `05276bda` |

## Summary

| Status | Count |
|---|---|
| **DONE** | **3** (+ 1 already done in bulk) |
| SKIPPED | 6 |
| NOT APPLICABLE | 4 |
| NOT INVESTIGATED | 1 |
| **Total** | 15 |

## Commits on branch for 0.79 work

```
e25d51ef 0.79: support horizontal scroll events in mouse tracking
0b6cab93 0.79: ^U just erases the line; backspace respects LATTR_WRAPPED2
```
