# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## What this repo is

KiTTY is a Windows-only fork of PuTTY 0.76 (SSH/telnet/serial terminal). The code is C, the target is **MinGW-w64 cross-compiled `.exe`** for Windows (both i686 and x86_64). There is no native Linux build.

All development happens on Linux via the MinGW cross toolchain and wine for runtime verification.

---

## Build (the only commands you need)

```bash
./build.sh cross       # 32-bit build → /builds/kitty.exe and friends
./build.sh cross64     # 64-bit build → /builds/kitty64.exe and friends
./build.sh clean       # make clean
```

`build.sh` is a thin wrapper around `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`. It enables `-j$(nproc)`. Outputs land in `/builds/` (mounted/symlinked per environment). Both targets can run in any order, any number of times — no manual cleanup between them. **Do not edit the `_64.a` / `_32.a` files directly; do not do `cp xxx_64.a xxx.a`** (see "Vendored libraries" below).

### What the build produces

| Output (under `/builds/`) | Source target | Notes |
|---|---|---|
| `kitty.exe`            | `cross` putty.exe + upx | UPX-compressed |
| `kitty_nocompress.exe` | `cross` putty.exe       | un-stripped image |
| `kitty_portable.exe`   | `cross` portable variant | |
| `klink.exe / kscp.exe / ksftp.exe / kageant.exe / kittygen.exe` | `cross` | |
| `kitty64.exe` and 64-bit siblings | `cross64` | |

### Build with debug symbols

To attach `gdb` to a running KiTTY, edit `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`:
```
CFLAGS = -Wall -O2 -g -std=gnu99 ...   # add -g
LDFLAGS =                              # remove -s
```
Then `./build.sh cross64`. **Use `/builds/kitty64_nocompress.exe`** — UPX destroys DWARF info on the compressed `kitty64.exe`. `-O0` will not link (a couple of `static` functions are referenced cross-TU, the optimizer hides it at `-O2`). Stick to `-O2 -g`.

---

## Verification & testing (wine on Linux)

Headless wine + Xvfb is the standard way to verify a build before sending it to Windows users.

### One-time environment setup

```bash
./scripts/setup-wine-debug-env.sh
```
Idempotent. Enables i386 multiarch, installs wine + wine32:i386 + Xvfb + xdotool + xpra + gdb + winedbg + puttygen + imagemagick + tesseract-ocr + openssh-server, initialises two wine prefixes under `~/wine_prefixes/wp32` and `~/wine_prefixes/wp64`, prints a readiness report. Re-run after any system upgrade.

### Smoke test (build + launch + verify shell)

```bash
./scripts/build-and-test.sh both         # 32 + 64
./scripts/build-and-test.sh cross        # just 32-bit
./scripts/build-and-test.sh cross64      # just 64-bit
```

For each requested arch the script:
1. Cleans + builds.
2. Generates an ed25519 SSH key + `.ppk` (one-time, cached at `~/.ssh/id_ed25519_kitty_test`).
3. Starts local `sshd` on port 22 if not already listening.
4. Launches `kitty.exe` / `kitty64.exe` under wine on Xvfb `:99`.
5. SSHes from KiTTY to `whoami@localhost`, auto-clicks the host-key dialog.
6. Types `uname -srm`, `echo 'hello-from-<arch>'`, `ls /etc/os-release` via `xdotool`.
7. Validates: screen-c PNG size, cropped-window pixel density vs screen-b, **tesseract OCR** must find `uname` / `Linux` / `hello-from-<arch>` / `os-release`.
8. Writes PASS/FAIL to `/tmp/kkit-<arch>/result.txt` plus three screenshots `screen-{a,b,c}.png` and `wine.log`.

This is the canonical "did I break anything?" check after touching renderer / SSH / URL hack / font fallback code. Always run before pushing.

### Manual launch (when the smoke script isn't enough)

```bash
# 32-bit
WINEPREFIX=~/wine_prefixes/wp32 DISPLAY=:99 WINEDEBUG=-all \
  wine /builds/kitty.exe admin@localhost -P 22 -i ~/.ssh/id_ed25519_kitty_test.ppk

# 64-bit
WINEPREFIX=~/wine_prefixes/wp64 DISPLAY=:99 WINEDEBUG=-all \
  wine /builds/kitty64.exe admin@localhost -P 22 -i ~/.ssh/id_ed25519_kitty_test.ppk
```

Snapshot the X server: `DISPLAY=:99 import -window root /tmp/k.png`. Drive the UI: `DISPLAY=:99 xdotool ...`. Wine 8+ uses a single `wine` launcher for both 32 and 64-bit — there is no `wine64` binary on Ubuntu 24.04.

### gdb-attach a running KiTTY

The wine process running the PE binary is a real Linux process. With a debug build (`-O2 -g`):

```bash
KPID=$(pgrep -f "/builds/kitty64_nocompress.exe" | head -1)
gdb -p $KPID
(gdb) file /builds/kitty64_nocompress.exe        # loads DWARF
(gdb) info functions urlhack
(gdb) b winfont_fallback.c:382
(gdb) print urlhack_rx.re_nsub
(gdb) x/64bx &urlhack_rx                          # raw bytes — see "ABI traps" below
```

When inspecting structs that come from / go into the vendored `.a` libraries, **always print raw bytes** in addition to typed `print`. KiTTY's header may disagree with what the library actually wrote.

Full debugging walkthrough: `docs/superpowers/runbooks/wine-debug-kitty.md`.

---

## High-level architecture

### The two source trees

- **`0.76_My_PuTTY/`** — historical PuTTY 0.76 baseline. Not built by default.
- **`0.76b_My_PuTTY/`** — the active fork. Everything new and all current builds live here.
  - `windows/` — Windows GUI, font / palette / DPI / message loop. Entry: `window.c` (`do_text_internal`, `init_fonts`, `wintw_*` callbacks).
  - `terminal/` — terminal emulator (escape sequences, scrollback, selection, URL toggle state machine in `do_paint`).
  - `ssh/`, `crypto/`, `proxy/`, `charset/`, `utils/` — PuTTY core.
  - `MAKEFILE.MINGW` — the only build file. Hand-written; do not regenerate.

### KiTTY-specific extension modules at repo root

Each adds Windows-only features behind `#ifdef MOD_PERSO` (and friends — `MOD_HYPERLINK`, `MOD_BACKGROUNDIMAGE`, `MOD_RUTTY`, `MOD_TUTTY`, `MOD_ZMODEM`, `MOD_FAR2L`, `MOD_PORTABLE`, ...).

- `kitty.c` (~9K lines) — KiTTY's main glue: ini reading (`ReadParameter` / `WriteParameter`), auto-command, transparency, tray, hyperlink launch.
- `kitty_commun.c`, `kitty_crypt.c`, `kitty_image.c` (background image), `kitty_launcher.c`, `kitty_proxy.c`, `kitty_puttytray.c`, `kitty_registry.c`, `kitty_savedump.c`, `kitty_settings.c`, `kitty_store.c`, `kitty_tools.c`, `kitty_win.c`.

### Subdirectory native modules

- `url/urlhack.{c,h}` — URL auto-detection in terminal text via regex.
- `adb/`, `blocnote/`, `cthelper/`, `far2l/`, `rutty/`, `zmodem/` — optional sub-features, each pulled in by its `MOD_*` flag.

### Vendored libraries (the .a files)

Each of these directories ships **two** static archives, one per architecture:

```
base64/{base64_32.a, base64_64.a}
bcrypt/{bcrypt_32.a, bcrypt_64.a}
blocnote/{notepad_32.a, notepad_64.a}
jpeg/{libjpeg_32.a, libjpeg_64.a}
md5/{MD5check_32.a, MD5check_64.a}
mini/{mini_32.a, mini_64.a}
regex/{libregex_32.a, libregex_64.a}
```

`MAKEFILE.MINGW` picks the right one via the `LIBSUFFIX` variable: `cross:` sets `LIBSUFFIX=_32`, `cross64:` sets `LIBSUFFIX=_64`, both pass it to nested `make -e ... LIBSUFFIX=...`. **No bare `xxx.a` files exist in the repo** (deliberate — `.gitignore` enforces). Do not create them; do not `cp xxx_64.a xxx.a` "to make things work" — that's exactly the trap commit `5703c85` undid.

`regex/regex.h` is the only **header** vendored alongside; it has a `#if defined(_WIN64)` block that conditionally widens `unsigned long` to `uint64_t` and `regoff_t` to `ssize_t` so the application's struct layouts match the GNULIB-built library on 64-bit (without this, the URL underline / SSH segfault bugs reappear — see "ABI traps").

### Generated headers (not committed)

- `kitty_ini.h` — generated by `MAKEFILE.MINGW` from `kitty_ini.txt` via `sed` at build time.
- `kitty_help.h` — generated from `docs/pages/CommandLine.md` likewise.

Both are listed in `.gitignore`. Edit the `.txt` / `.md` source, not the `.h`.

### The font fallback subsystem (`MOD_PERSO`)

- `0.76b_My_PuTTY/windows/winfont_fallback.{c,h}` — module designed in `docs/superpowers/specs/2026-05-27-kitty-font-fallback-design.md`, implemented per `docs/superpowers/plans/2026-05-27-kitty-font-fallback.md`.
- Probes for missing glyphs via `GetGlyphIndicesW`, splits text into per-font runs, draws each run with the matching `HFONT` clipped to the cell.
- Hooked into `do_text_internal` (single integration point in `window.c`).
- Configured via `[FontFallback]` in `kitty.ini` (`Log=info|debug|trace`, `Fallback=Font1,Font2,...`, `Override=E000-F8FF:FontName;...`).
- Has its own file logger (default OFF) at `<exe-dir>/fontfallback.log` with single-generation 10MB rollover.
- **Known limitation**: SMP codepoints (emoji ≥ U+1F600) cannot fallback-probe via `GetGlyphIndicesW` (surrogate halves never have glyphs). Use `Override=` to force-assign. Full list of limitations in `tests/font_fallback_test.txt`.

---

## ABI traps (historical pain — read before debugging 64-bit-only bugs)

The `regex/libregex_64.a` library was built with **glibc-style** types (`unsigned long = 8 bytes`, `regoff_t = ssize_t`). MinGW-w64 / Win64 is **LLP64** (`unsigned long = 4 bytes`, `int = 4 bytes`). Without the `#if defined(_WIN64)` block in `regex/regex.h`, the application reads `re_nsub` from the wrong byte offset and `rm_eo` from the high half of `rm_so` (always 0). The URL-finding loop then mis-advances `text_pos` by 1 byte at a time, fills `link_regions[]` with hundreds of duplicates, and the terminal toggle state machine paints `ATTR_UNDER` on every cell after the first URL. On some Ubuntu MOTDs the same trampling crashes the binary.

**If a bug only reproduces on 64-bit and not 32-bit, the first hypothesis is ABI / type-size mismatch with a vendored library.** Use `gdb` + `x/64bx` + `x86_64-w64-mingw32-objdump --dwarf=info` to compare what KiTTY's headers think the struct layout is vs what the `.a`'s DWARF says.

Full forensic write-up: `docs/superpowers/postmortems/2026-05-29-regex-abi-mismatch-real-rootcause.md`.

---

## Where to write docs

`docs/superpowers/` is the dev / design knowledge directory (separate from the user-facing `docs/pages/` website content):

```
docs/superpowers/specs/         design specs for features
docs/superpowers/plans/         implementation plans (task lists)
docs/superpowers/runbooks/      "how to do X" operational guides
docs/superpowers/postmortems/   bug forensics with date + root cause
```

When you fix a non-trivial bug, especially a 64-bit-only one, write a postmortem with:
- the symptom (in the user's words),
- the wrong hypotheses you tried first,
- how you actually found the cause (with the gdb / objdump output),
- the fix,
- a "lessons" section that warns the next reader.

---

## Things that look like bugs but aren't

- `./build.sh cross64`'s console output contains `undefined reference to ...` lines (jpeg / regex / notepad symbols). These come from the `make putty.exe || gcc -o kitty64.exe ...` fallback path that the cross64 recipe uses when the in-Makefile link line doesn't directly match the 64-bit set. With the LIBSUFFIX refactor (`5703c85`) the left side now succeeds and the fallback is rarely triggered, but the recipe is tolerant of both outcomes. Look at `/builds/kitty64.exe`'s timestamp for the real signal.
- `MD5check_64.a` is actually i386 content. md5 only exports ~10 symbols and MinGW-w64's ld accepts the size mismatch silently. Pre-existing. Don't "fix" it unless something stops working.
- `[FontFallback]` log shows `fallback slot N: "Segoe UI Symbol" NOT installed, skipping` under wine — that's normal, wine doesn't have Windows system fonts; fallback list ends up empty and font fallback becomes a no-op. Behaviour on a real Windows install is different.

---

## When in doubt

- **Build something?** `./build.sh cross` and/or `./build.sh cross64`.
- **Verify it actually works?** `./scripts/build-and-test.sh both`.
- **Debug 64-bit-only weirdness?** Build with `-O2 -g`, launch the `_nocompress.exe` under wine, `gdb -p $(pgrep kitty)`, `file /builds/kitty64_nocompress.exe` inside gdb.
- **Looking for context on a past fix?** `docs/superpowers/postmortems/`.
- **Setting up a fresh box?** `./scripts/setup-wine-debug-env.sh`.
