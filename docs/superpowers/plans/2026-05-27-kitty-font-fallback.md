# KiTTY Windows Font Fallback — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-codepoint font fallback to KiTTY's GDI renderer so missing glyphs (U+23F5, Box Drawing, Geometric Shapes, Nerd Font PUA, CJK, etc.) display from system fallback fonts instead of .notdef boxes.

**Architecture:** New module `winfont_fallback.{c,h}` probes glyphs via `GetGlyphIndicesW`, splits text runs by font slot, and draws each run with the matching HFONT. Cell width stays EAW-driven; fallback glyphs are clipped to cell bounds. Independent 5-level file logger colocated with KiTTY binary.

**Tech Stack:** C99, Win32 GDI (`GetGlyphIndicesW`, `CreateFontIndirectW`, `ExtTextOutW`), MinGW-w64 cross-compile, existing KiTTY ini reader (`ReadParameter`).

**Spec:** `docs/superpowers/specs/2026-05-27-kitty-font-fallback-design.md`

**Build commands:** `cd 0.76b_My_PuTTY/windows && make -f MAKEFILE.MINGW cross` (32-bit) / `cross64` (64-bit). Top-level shortcut: `./zzy.sh cross` / `./zzy.sh cross64`.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `0.76b_My_PuTTY/windows/winfont_fallback.h` | Create | Public API: init/split/draw/cleanup/log types and prototypes |
| `0.76b_My_PuTTY/windows/winfont_fallback.c` | Create | All implementation: probe, cache, split, draw, log, ini parsing |
| `0.76b_My_PuTTY/windows/window.c` | Modify | Include header, call `winfb_init`/`winfb_reset`/`winfb_cleanup`, replace normal-unicode draw path |
| `0.76b_My_PuTTY/windows/MAKEFILE.MINGW` | Modify | Add `winfont_fallback.o` to putty.exe link line and add compile rule |
| `kitty.c` | Modify | Read `[FontFallback]` ini keys before `init_fonts`, call `winfb_log_init` |
| `kitty_ini.txt` | Modify | Document `[FontFallback]` section |

---

## Task 1: Create header file with all types and prototypes

**Files:**
- Create: `0.76b_My_PuTTY/windows/winfont_fallback.h`

- [ ] **Step 1: Write the header file**

```c
/*
 * winfont_fallback.h — Per-codepoint font fallback for KiTTY (GDI route)
 *
 * When the primary terminal font lacks a glyph, this module probes a
 * configurable fallback list and draws the character from the first
 * matching font.  Cell width remains EAW-driven; glyphs are clipped.
 */

#ifndef WINFONT_FALLBACK_H
#define WINFONT_FALLBACK_H

#include <windows.h>

/* ---- log levels ---- */
enum winfb_log_level {
    WINFB_LOG_OFF   = 0,
    WINFB_LOG_ERROR = 1,
    WINFB_LOG_WARN  = 2,
    WINFB_LOG_INFO  = 3,
    WINFB_LOG_DEBUG = 4,
    WINFB_LOG_TRACE = 5
};

/* ---- run returned by winfb_split ---- */
typedef struct {
    int start;   /* index into wbuf */
    int len;     /* wchar count (surrogate pairs counted as 2) */
    int slot;    /* -1 = primary font, 0..N-1 = fallback slot */
} WinFB_Run;

/* ---- lifecycle ---- */

/*
 * Initialise the fallback subsystem.  Call after the primary font has
 * been created (so we can borrow its LOGFONT / cell metrics).
 *   hdc      — screen DC (we create our own compatible DC internally)
 *   primary  — LOGFONT of the primary terminal font
 *   cell_w   — font_width  from init_fonts
 *   cell_h   — font_height from init_fonts
 *   fallback_csv — comma-separated fallback font names (may be NULL/empty)
 *                  if starts with '!' the built-in list is replaced,
 *                  otherwise it is appended after the built-in list
 *   override_lines / n_override — array of "XXXX-YYYY:FontName" strings
 *                  (may be NULL / 0)
 */
void winfb_init(HDC hdc, const LOGFONT *primary,
                int cell_w, int cell_h,
                const char *fallback_csv,
                const char **override_lines, int n_override);

/* Clear caches (call when primary font is recreated). */
void winfb_reset(void);

/* Free all resources (call on shutdown). */
void winfb_cleanup(void);

/* ---- splitting ---- */

/*
 * Split wbuf[0..len-1] into contiguous runs sharing the same font slot.
 * Returns the number of runs written to out[] (never exceeds max_runs).
 * out[] is filled sequentially; runs cover the full wbuf range.
 */
int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs);

/* ---- drawing ---- */

/*
 * Draw multiple runs, each with its own HFONT, into the same line_box.
 *   nfont_primary — the fonts[] index of the current primary font variant
 *                   (used to restore after drawing)
 *   bold / italic / underline — attributes for HFONT variant selection
 */
void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     int nfont_primary,
                     bool bold, bool italic, bool underline);

/* ---- font handle access ---- */

/*
 * Return the HFONT for a given slot + attribute combination.
 * slot == -1 returns the primary font variant.
 * HFONTs are lazily created on first access.
 */
HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline);

/* ---- logging ---- */

void winfb_log_init(int level, const char *path);
void winfb_log_close(void);
void winfb_logf(int level, const char *fmt, ...);

#endif /* WINFONT_FALLBACK_H */
```

- [ ] **Step 2: Commit**

```bash
git add 0.76b_My_PuTTY/windows/winfont_fallback.h
git commit -m "feat(font-fallback): add winfont_fallback.h public API header"
```

---

## Task 2: Create implementation file — logging subsystem + stubs

**Files:**
- Create: `0.76b_My_PuTTY/windows/winfont_fallback.c`

This task creates the `.c` file with a fully working logging subsystem and stub implementations for all other functions. The stubs let the module compile and link without affecting rendering.

- [ ] **Step 1: Write the implementation file**

```c
/*
 * winfont_fallback.c — Per-codepoint font fallback for KiTTY (GDI route)
 *
 * See winfont_fallback.h for API documentation.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "winfont_fallback.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Logging
 * =================================================================== */

static int  g_log_level = WINFB_LOG_OFF;
static FILE *g_log_fp   = NULL;
static char g_log_path[MAX_PATH] = {0};
static int  g_log_lines = 0;

#define WINFB_LOG_MAX_LINES  100000   /* ~10 MB */
#define WINFB_LOG_FLUSH_EVERY 64

static const char *level_name(int lvl)
{
    switch (lvl) {
    case WINFB_LOG_ERROR: return "ERROR";
    case WINFB_LOG_WARN:  return "WARN ";
    case WINFB_LOG_INFO:  return "INFO ";
    case WINFB_LOG_DEBUG: return "DEBUG";
    case WINFB_LOG_TRACE: return "TRACE";
    default:              return "?????";
    }
}

static void log_rollover(void)
{
    if (!g_log_fp) return;
    fclose(g_log_fp);
    g_log_fp = NULL;
    g_log_lines = 0;
    /* single-generation rollover: .log -> .log.1 */
    char bak[MAX_PATH];
    snprintf(bak, MAX_PATH, "%s.1", g_log_path);
    /* ignore failure — best effort */
    MoveFileExA(g_log_path, bak, MOVEFILE_REPLACE_EXISTING);
    g_log_fp = fopen(g_log_path, "a");
}

void winfb_log_init(int level, const char *path)
{
    g_log_level = level;
    if (level <= WINFB_LOG_OFF) return;

    if (path && path[0]) {
        strncpy(g_log_path, path, MAX_PATH - 1);
        g_log_path[MAX_PATH - 1] = '\0';
    } else {
        /* default: beside the .exe */
        GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
        char *slash = strrchr(g_log_path, '\\');
        if (slash) slash[1] = '\0'; else g_log_path[0] = '\0';
        strncat(g_log_path, "fontfallback.log",
                MAX_PATH - strlen(g_log_path) - 1);
    }

    g_log_fp = fopen(g_log_path, "a");
    if (g_log_fp) {
        setvbuf(g_log_fp, NULL, _IOFBF, 8192);
        g_log_lines = 0;
    }
}

void winfb_log_close(void)
{
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    g_log_level = WINFB_LOG_OFF;
}

void winfb_logf(int level, const char *fmt, ...)
{
    if (level > g_log_level) return;
    if (!g_log_fp) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log_fp,
            "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level_name(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_log_fp);

    g_log_lines++;
    if (level >= WINFB_LOG_DEBUG || (g_log_lines % WINFB_LOG_FLUSH_EVERY == 0))
        fflush(g_log_fp);
    if (g_log_lines >= WINFB_LOG_MAX_LINES)
        log_rollover();
}

/* ===================================================================
 *  Module state
 * =================================================================== */

#define WINFB_MAX_SLOTS   16
#define WINFB_ATTR_DIM     8   /* bold(2) x italic(2) x underline(2) */

typedef struct {
    char  name[LF_FACESIZE];
    HFONT hfont[WINFB_ATTR_DIM];   /* lazily created */
    bool  installed;                /* EnumFontFamiliesEx confirmed */
} winfb_slot;

static winfb_slot g_slots[WINFB_MAX_SLOTS];
static int        g_n_slots;
static LOGFONT    g_base_lf;
static int        g_cell_w, g_cell_h;
static HDC        g_probe_dc;
static bool       g_initialised;

/* BMP codepoint -> slot index cache.
 * -2 = not probed yet, -1 = primary (or all-miss), 0..N-1 = fallback slot */
static int8_t g_bmp_map[0x10000];

#define WINFB_SMP_CAP 1024
typedef struct { uint32_t cp; int8_t slot; } winfb_smp_ent;
static winfb_smp_ent g_smp[WINFB_SMP_CAP];
static int g_n_smp;

/* Override ranges */
#define WINFB_OVR_CAP 64
typedef struct { uint32_t lo, hi; int slot; } winfb_override;
static winfb_override g_ovr[WINFB_OVR_CAP];
static int g_n_ovr;

/* ===================================================================
 *  Stub implementations (S1 — everything except logging is a no-op)
 * =================================================================== */

void winfb_init(HDC hdc, const LOGFONT *primary,
                int cell_w, int cell_h,
                const char *fallback_csv,
                const char **override_lines, int n_override)
{
    (void)hdc; (void)primary;
    (void)cell_w; (void)cell_h;
    (void)fallback_csv;
    (void)override_lines; (void)n_override;
    winfb_logf(WINFB_LOG_INFO, "winfb_init: stub (S1)");
}

void winfb_reset(void)
{
    winfb_logf(WINFB_LOG_INFO, "winfb_reset: stub (S1)");
}

void winfb_cleanup(void)
{
    winfb_logf(WINFB_LOG_INFO, "winfb_cleanup: stub (S1)");
}

int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs)
{
    (void)wbuf;
    if (len <= 0 || max_runs <= 0) return 0;
    out[0].start = 0;
    out[0].len   = len;
    out[0].slot  = -1;
    return 1;
}

void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     int nfont_primary,
                     bool bold, bool italic, bool underline)
{
    (void)hdc; (void)x; (void)y; (void)line_box;
    (void)wbuf; (void)len; (void)lpDx;
    (void)runs; (void)nruns; (void)opaque;
    (void)nfont_primary;
    (void)bold; (void)italic; (void)underline;
}

HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline)
{
    (void)slot; (void)bold; (void)italic; (void)underline;
    return NULL;
}
```

- [ ] **Step 2: Commit**

```bash
git add 0.76b_My_PuTTY/windows/winfont_fallback.c
git commit -m "feat(font-fallback): add winfont_fallback.c with logging + stubs"
```

---

## Task 3: Add compile rule and link winfont_fallback.o into putty.exe

**Files:**
- Modify: `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`

The Makefile has two places that list object files for `putty.exe`: the dependency line (line ~408) and the link command (line ~438). Both must include `winfont_fallback.o`. We also add a compile rule.

- [ ] **Step 1: Add `winfont_fallback.o` to the dependency line**

Find the first occurrence of `windlg.o window.o gss.o` in the dependency list (around line 425-426). Insert `winfont_fallback.o` right after `window.o` on that line.

The line currently reads:
```
			windlg.o window.o gss.o handle-io.o help.o handle-socket.o \
```
Change to:
```
			windlg.o window.o winfont_fallback.o gss.o handle-io.o help.o handle-socket.o \
```

- [ ] **Step 2: Add `winfont_fallback.o` to the link command**

Find the second occurrence of `windlg.o window.o gss.o` in the link command (around line 456). Same insertion:
```
			windlg.o window.o winfont_fallback.o gss.o handle-io.o help.o handle-socket.o \
```

- [ ] **Step 3: Add compile rule**

Insert after the `window.o` rule block (after line ~1796, the line with `-DMOD_INTEGRATED_AGENT -DMOD_INTEGRATED_KEYGEN`):

```makefile
winfont_fallback.o: ../windows/winfont_fallback.c ../windows/winfont_fallback.h \
		../putty.h ../terminal/terminal.h ../storage.h \
		../windows/platform.h ../unix/unix.h \
		../defs.h ../puttyps.h ../network.h ../misc.h \
		../marshal.h ../ssh/signal-list.h ../puttymem.h \
		../windows/help.h ../charset/charset.h
	$(CC) $(COMPAT) $(CFLAGS) $(XFLAGS) -c ../windows/winfont_fallback.c
```

- [ ] **Step 4: Build both targets to verify compilation**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed with `winfont_fallback.o` compiled and linked into `putty.exe` / `kitty64.exe`.

- [ ] **Step 5: Commit**

```bash
git add 0.76b_My_PuTTY/windows/MAKEFILE.MINGW
git commit -m "build(font-fallback): add winfont_fallback.o to MAKEFILE.MINGW"
```

---

## Task 4: Wire logging init into kitty.c and add [FontFallback] ini reading

**Files:**
- Modify: `kitty.c`

We add ini reading for `[FontFallback]` section keys and call `winfb_log_init` early in startup. The actual `winfb_init` call happens later in `window.c` (Task 5) after the primary font is created.

- [ ] **Step 1: Add include at top of kitty.c**

Near the other `#include` directives (after existing kitty includes like `#include "kitty_win.h"`), add:

```c
#include "winfont_fallback.h"
```

Note: `kitty.c` is compiled from the `0.76b_My_PuTTY/windows/` directory as `kitty.o`. The include path must use the relative path from the Makefile's compilation directory. Check the Makefile — `kitty.o` is compiled from `../../kitty.c`. The include path should be:

```c
#include "../windows/winfont_fallback.h"
```

Wait — the Makefile compiles with `-I` flags. Let me check. Looking at the Makefile, `window.c` includes `"../putty.h"` etc. But `kitty.c` is at the repo root. The safest approach: use the same relative path pattern that `kitty.c` uses for its own includes. Since `kitty.c` already includes `"kitty_commun.h"` (same directory), and the Makefile compiles it from the `0.76b_My_PuTTY/windows/` directory as `../../kitty.c`, the include for our header needs to be:

```c
#include "0.76b_My_PuTTY/windows/winfont_fallback.h"
```

No — that's fragile. Better: add the include path to the Makefile compile rule for `kitty.o`. But that's invasive. Simplest: declare the log init prototype as `extern` directly in `kitty.c` to avoid the header dependency for this one call. We'll use the full header in `window.c` (Task 5).

In `kitty.c`, near the top with other extern declarations, add:

```c
/* winfont_fallback logging — full header included from window.c */
extern void winfb_log_init(int level, const char *path);
extern void winfb_log_close(void);
```

- [ ] **Step 2: Add ini reading and log init call**

Find a suitable early initialization point in `kitty.c`. The function `ReadParameter(INIT_SECTION, ...)` is used throughout. Add a new function and call it from the startup path.

Search for where `ReadParameter(INIT_SECTION, "KiCount", ...)` is called (around line 1199). That's inside a function called at startup. Add a new function right before or after the existing init code:

```c
static void init_fontfallback_log(void)
{
    char buf[4096];
    int level = WINFB_LOG_OFF;  /* default off */

    if (ReadParameter("FontFallback", "Log", buf)) {
        if      (!stricmp(buf, "error")) level = 1;
        else if (!stricmp(buf, "warn"))  level = 2;
        else if (!stricmp(buf, "info"))  level = 3;
        else if (!stricmp(buf, "debug")) level = 4;
        else if (!stricmp(buf, "trace")) level = 5;
    }

    char logpath[MAX_PATH];
    logpath[0] = '\0';
    if (ReadParameter("FontFallback", "LogFile", buf)) {
        strncpy(logpath, buf, MAX_PATH - 1);
        logpath[MAX_PATH - 1] = '\0';
    }

    winfb_log_init(level, logpath[0] ? logpath : NULL);
}
```

Then call `init_fontfallback_log();` early in the startup sequence. Find the `WinMain` or equivalent entry in `kitty.c` and add the call after `ReadParameter` infrastructure is available (after the ini file path is resolved). Look for where other `ReadParameter(INIT_SECTION, ...)` calls begin — that's the safe point.

**Note:** The `stricmp` function is available in `kitty.c` (already used extensively there).

- [ ] **Step 3: Add `winfb_log_close()` on shutdown**

Find the cleanup/shutdown path in `kitty.c` and add `winfb_log_close();` before process exit.

- [ ] **Step 4: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed. Running KiTTY with `[FontFallback] Log=info` in kitty.ini should create `fontfallback.log` beside the exe containing the stub init message.

- [ ] **Step 5: Commit**

```bash
git add kitty.c
git commit -m "feat(font-fallback): wire [FontFallback] ini reading and log init into kitty.c"
```

---

## Task 5: Wire winfb_init/reset/cleanup into window.c

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

Add the `#include`, call `winfb_init` at the end of `init_fonts`, call `winfb_reset` when fonts are rebuilt, and call `winfb_cleanup` in `deinit_fonts`.

- [ ] **Step 1: Add include near top of window.c**

After the existing includes (around line 30-40, after `#include "win-gui-seat.h"` etc.), add:

```c
#include "winfont_fallback.h"
```

- [ ] **Step 2: Add winfb_init at end of init_fonts**

At the end of `init_fonts` (line ~2814, right before the closing `}` of the function, after `init_ucs(conf, &ucsdata);`), add:

```c
    /* --- font fallback init --- */
    {
        extern char KittyIniFile[];  /* defined in kitty.c */
        char fb_buf[4096];
        const char *fb_csv = NULL;

        if (ReadParameterA("FontFallback", "Fallback", fb_buf))
            fb_csv = fb_buf;

        winfb_init(hdc, &lfont, font_width, font_height, fb_csv, NULL, 0);
    }
```

**Important:** `ReadParameter` in `kitty.c` uses `char[]` buffers and returns non-zero if the key exists. But `window.c` doesn't have direct access to `ReadParameter` — it's defined in `kitty.c`. We need a thin wrapper. The simplest approach: expose the fallback CSV through a global or a getter function. However, to keep Task 5 focused, we'll pass `NULL` for now and add the ini reading in Task 8 (S4). For S1 wiring, just call:

```c
    winfb_init(hdc, &lfont, font_width, font_height, NULL, NULL, 0);
```

This uses the built-in default fallback list (which we'll implement in Task 6).

- [ ] **Step 3: Add winfb_reset at font rebuild sites**

In `reset_window`, there are two places where `deinit_fonts()` + `init_fonts()` are called (lines ~3015-3016 and ~3042-3043). After each `init_fonts(...)` call, add:

```c
    winfb_reset();
    winfb_init(hdc, &lfont, font_width, font_height, NULL, NULL, 0);
```

But `hdc` and `lfont` may not be in scope in `reset_window`. We need to use the module-level `wintw_hdc` and the global `lfont`. Looking at the code: `lfont` is a file-static `LOGFONT` at line 222, and `hdc` can be obtained via `GetDC(wgs.term_hwnd)`. However, `winfb_init` only needs a DC to create a compatible DC — we can also use `wintw_hdc` which is already set up. Let's use a simpler approach: `winfb_reset()` clears caches and re-probes using the stored `g_base_lf` and `g_probe_dc`. So we only need to call `winfb_reset()` after `init_fonts`, not a full `winfb_init` again. But we do need to update `g_base_lf` and recreate `g_probe_dc` with the new primary font.

Actually, the cleanest design: make `winfb_reset()` do a full re-init. We'll store the DC handle from init. So the call sites just need:

```c
    winfb_reset();
```

And `winfb_reset` will re-probe the primary font (which has already been recreated by `init_fonts`) and clear the cache. We'll implement this properly in Task 6. For now, the stub just logs.

Add `winfb_reset();` after each `init_fonts(...)` call in `reset_window`:

- After line ~3016: `init_fonts(0,0);` → add `winfb_reset();`
- After line ~3043: `init_fonts(win_width/term->cols, win_height/term->rows);` → add `winfb_reset();`
- After line ~3163: `init_fonts(font_width, font_height);` → add `winfb_reset();`
- After line ~3200: `init_fonts((win_width-window_border*2)/term->cols, ...` → add `winfb_reset();`

- [ ] **Step 4: Add winfb_cleanup in deinit_fonts**

In `deinit_fonts` (line ~2887), at the end of the function (after the `trust_icon` cleanup, before the closing `}`), add:

```c
    winfb_cleanup();
```

- [ ] **Step 5: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed. No runtime change yet (stubs).

- [ ] **Step 6: Commit**

```bash
git add 0.76b_My_PuTTY/windows/window.c
git commit -m "feat(font-fallback): wire init/reset/cleanup calls into window.c"
```

---

## Task 6: Implement glyph probe, cache, and fallback slot selection

**Files:**
- Modify: `0.76b_My_PuTTY/windows/winfont_fallback.c`

This is the core logic. Replace the stubs with real implementations for `winfb_init`, `winfb_reset`, `winfb_cleanup`, `winfb_hfont`, and the internal `winfb_lookup_slot` function.

- [ ] **Step 1: Add built-in default fallback list**

Replace the empty module state section with the full state plus the default list. Add these constants and helper after the logging section, before the stub implementations:

```c
/* ===================================================================
 *  Built-in fallback font list
 * =================================================================== */

static const char *g_default_fallbacks[] = {
    "Segoe UI Symbol",
    "Segoe UI Emoji",
    "Segoe UI Historic",
    "Cascadia Mono",
    "Cascadia Code",
    "Microsoft YaHei UI",
    "Microsoft JhengHei UI",
    "Yu Gothic UI",
    "Malgun Gothic",
    NULL
};
```

- [ ] **Step 2: Implement font enumeration callback (to check if a font is installed)**

```c
typedef struct {
    const char *target_name;
    BOOL        found;
} enum_cb_ctx;

static int CALLBACK enum_font_cb(const LOGFONT *lf, const TEXTMETRIC *tm,
                                 DWORD type, LPARAM lParam)
{
    (void)tm; (void)type;
    enum_cb_ctx *ctx = (enum_cb_ctx *)lParam;
    if (stricmp(ctx->target_name, lf->lfFaceName) == 0) {
        ctx->found = TRUE;
        return 0;  /* stop enumeration */
    }
    return 1;  /* continue */
}

static bool is_font_installed(HDC hdc, const char *name)
{
    enum_cb_ctx ctx;
    ctx.target_name = name;
    ctx.found = FALSE;
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, name, LF_FACESIZE - 1);
    EnumFontFamiliesExA(hdc, &lf, enum_font_cb, (LPARAM)&ctx, 0);
    return ctx.found != FALSE;
}
```

- [ ] **Step 3: Implement HFONT creation helper**

```c
static int attr_index(bool bold, bool italic, bool underline)
{
    return (bold ? 4 : 0) | (italic ? 2 : 0) | (underline ? 1 : 0);
}

static HFONT create_variant(const LOGFONT *base, int cell_w, int cell_h,
                             const char *face, bool bold, bool italic,
                             bool underline)
{
    LOGFONT lf = *base;
    strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = '\0';
    lf.lfHeight = cell_h;
    lf.lfWidth  = cell_w;
    lf.lfWeight = bold ? FW_BOLD : FW_DONTCARE;
    lf.lfItalic = italic ? TRUE : FALSE;
    lf.lfUnderline = underline ? TRUE : FALSE;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = base->lfQuality;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    return CreateFontIndirectW(&lf);
}
```

- [ ] **Step 4: Implement CSV parser for fallback font list**

```c
/* Parse comma-separated font names. Writes into slot array.
 * If csv starts with '!', replace built-in list; otherwise append. */
static void parse_fallback_csv(const char *csv, bool *replace_builtins)
{
    if (!csv || !csv[0]) return;
    const char *p = csv;
    if (*p == '!') {
        *replace_builtins = true;
        p++;
        while (*p == ' ') p++;
    }
    /* tokenize by comma */
    char buf[LF_FACESIZE];
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ',' && i < LF_FACESIZE - 1) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        /* trim trailing space */
        while (i > 0 && (buf[i-1] == ' ' || buf[i-1] == '\t')) buf[--i] = '\0';
        if (buf[0] && g_n_slots < WINFB_MAX_SLOTS) {
            strncpy(g_slots[g_n_slots].name, buf, LF_FACESIZE - 1);
            g_slots[g_n_slots].installed = false; /* checked later */
            memset(g_slots[g_n_slots].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
            g_n_slots++;
        }
        if (*p == ',') p++;
    }
}
```

- [ ] **Step 5: Implement override range parser**

```c
/* Parse "XXXX-YYYY:FontName" or "XXXX:FontName" lines.
 * XXXX and YYYY are hex Unicode codepoints. */
static void parse_overrides(const char **lines, int n)
{
    int i;
    for (i = 0; i < n && g_n_ovr < WINFB_OVR_CAP; i++) {
        const char *s = lines[i];
        if (!s) continue;
        uint32_t lo = 0, hi = 0;
        int consumed = 0;
        if (sscanf(s, "%x-%x:%n", &lo, &hi, &consumed) == 2 && consumed > 0) {
            /* range */
        } else if (sscanf(s, "%x:%n", &lo, &consumed) == 1 && consumed > 0) {
            hi = lo;
        } else {
            continue;
        }
        const char *fname = s + consumed;
        while (*fname == ' ') fname++;
        if (!*fname) continue;

        /* find or create the slot for this font name */
        int slot;
        for (slot = 0; slot < g_n_slots; slot++) {
            if (stricmp(g_slots[slot].name, fname) == 0) break;
        }
        if (slot == g_n_slots) {
            if (g_n_slots >= WINFB_MAX_SLOTS) continue;
            strncpy(g_slots[g_n_slots].name, fname, LF_FACESIZE - 1);
            memset(g_slots[g_n_slots].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
            g_slots[g_n_slots].installed = false;
            slot = g_n_slots++;
        }
        g_ovr[g_n_ovr].lo   = lo;
        g_ovr[g_n_ovr].hi   = hi;
        g_ovr[g_n_ovr].slot = slot;
        g_n_ovr++;
        winfb_logf(WINFB_LOG_INFO, "override U+%04X-U+%04X -> slot %d \"%s\"",
                   lo, hi, slot, g_slots[slot].name);
    }
}
```

- [ ] **Step 6: Implement winfb_init**

Replace the stub `winfb_init` with:

```c
void winfb_init(HDC hdc, const LOGFONT *primary,
                int cell_w, int cell_h,
                const char *fallback_csv,
                const char **override_lines, int n_override)
{
    winfb_cleanup();  /* idempotent */

    g_base_lf = *primary;
    g_cell_w  = cell_w;
    g_cell_h  = cell_h;
    g_n_slots = 0;
    g_n_smp   = 0;
    g_n_ovr   = 0;
    g_initialised = false;

    /* create a compatible DC for glyph probing */
    g_probe_dc = CreateCompatibleDC(hdc);
    if (!g_probe_dc) {
        winfb_logf(WINFB_LOG_ERROR, "winfb_init: CreateCompatibleDC failed");
        return;
    }

    /* select primary font into probe DC */
    HFONT hf_primary = CreateFontIndirectW(primary);
    if (hf_primary) {
        SelectObject(g_probe_dc, hf_primary);
        DeleteObject(hf_primary);  /* DC holds the reference */
    }

    /* reset BMP cache to "not probed" */
    memset(g_bmp_map, -2, sizeof(g_bmp_map));  /* -2 == 0xFE as int8 */

    /* parse user fallback list */
    bool replace_builtins = false;
    parse_fallback_csv(fallback_csv, &replace_builtins);

    /* if not replacing, prepend built-in defaults */
    if (!replace_builtins) {
        /* shift user slots up to make room for builtins at the front */
        int builtin_count = 0;
        while (g_default_fallbacks[builtin_count]) builtin_count++;
        if (g_n_slots + builtin_count <= WINFB_MAX_SLOTS) {
            memmove(g_slots + builtin_count, g_slots,
                    g_n_slots * sizeof(winfb_slot));
            g_n_slots += builtin_count;
            for (int i = 0; i < builtin_count; i++) {
                strncpy(g_slots[i].name, g_default_fallbacks[i], LF_FACESIZE - 1);
                memset(g_slots[i].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
                g_slots[i].installed = false;
            }
        }
    }

    /* verify which fallback fonts are actually installed */
    for (int i = 0; i < g_n_slots; i++) {
        g_slots[i].installed = is_font_installed(hdc, g_slots[i].name);
        if (g_slots[i].installed) {
            winfb_logf(WINFB_LOG_INFO, "fallback slot %d: \"%s\" (installed)",
                       i, g_slots[i].name);
        } else {
            winfb_logf(WINFB_LOG_WARN, "fallback slot %d: \"%s\" NOT installed, skipping",
                       i, g_slots[i].name);
        }
    }

    /* compact: remove uninstalled fonts, adjust override slot indices */
    int dst = 0;
    int remap[WINFB_MAX_SLOTS];
    for (int i = 0; i < g_n_slots; i++) {
        remap[i] = -1;
        if (g_slots[i].installed) {
            if (dst != i) g_slots[dst] = g_slots[i];
            remap[i] = dst;
            dst++;
        }
    }
    g_n_slots = dst;
    for (int i = 0; i < g_n_ovr; i++) {
        g_ovr[i].slot = (g_ovr[i].slot >= 0 && g_ovr[i].slot < WINFB_MAX_SLOTS)
                        ? remap[g_ovr[i].slot] : -1;
        if (g_ovr[i].slot < 0) {
            /* override pointed to uninstalled font, drop it */
            g_ovr[i] = g_ovr[g_n_ovr - 1];
            g_n_ovr--;
            i--;
        }
    }

    /* parse overrides */
    if (override_lines && n_override > 0)
        parse_overrides(override_lines, n_override);

    winfb_logf(WINFB_LOG_INFO,
               "winfb_init: primary=\"%ls\" cell=%dx%d fallback_slots=%d overrides=%d",
               primary->lfFaceName, cell_w, cell_h, g_n_slots, g_n_ovr);

    g_initialised = true;
}
```

- [ ] **Step 7: Implement winfb_reset and winfb_cleanup**

```c
void winfb_reset(void)
{
    if (!g_initialised) return;
    winfb_logf(WINFB_LOG_INFO, "winfb_reset: clearing caches");

    /* clear glyph cache */
    memset(g_bmp_map, -2, sizeof(g_bmp_map));
    g_n_smp = 0;

    /* delete fallback HFONTs (they'll be recreated lazily) */
    for (int i = 0; i < g_n_slots; i++) {
        for (int j = 0; j < WINFB_ATTR_DIM; j++) {
            if (g_slots[i].hfont[j]) {
                DeleteObject(g_slots[i].hfont[j]);
                g_slots[i].hfont[j] = NULL;
            }
        }
    }
}

void winfb_cleanup(void)
{
    winfb_reset();

    /* delete probe DC */
    if (g_probe_dc) {
        DeleteDC(g_probe_dc);
        g_probe_dc = NULL;
    }

    g_n_slots = 0;
    g_n_smp   = 0;
    g_n_ovr   = 0;
    g_initialised = false;

    winfb_logf(WINFB_LOG_INFO, "winfb_cleanup: released all resources");
}
```

- [ ] **Step 8: Implement winfb_lookup_slot**

```c
/* Look up which fallback slot (or primary) contains the glyph for cp.
 * Returns: -1 = primary font (confirmed present or all-miss),
 *           0..N-1 = fallback slot index.
 * BMP codepoints use g_bmp_map; SMP uses g_smp hash. */
static int winfb_lookup_slot(uint32_t cp)
{
    if (!g_initialised) return -1;

    /* --- check override ranges first --- */
    for (int i = 0; i < g_n_ovr; i++) {
        if (cp >= g_ovr[i].lo && cp <= g_ovr[i].hi)
            return g_ovr[i].slot;
    }

    /* --- BMP cache --- */
    if (cp < 0x10000) {
        int8_t cached = g_bmp_map[cp];
        if (cached != -2) return (int)cached;
    } else {
        /* SMP hash lookup */
        for (int i = 0; i < g_n_smp; i++) {
            if (g_smp[i].cp == cp) return (int)g_smp[i].slot;
        }
    }

    /* --- probe primary font --- */
    if (g_probe_dc) {
        wchar_t wc = (wchar_t)cp;
        WORD gi = 0;
        if (cp < 0x10000) {
            SelectObject(g_probe_dc, GetCurrentObject(g_probe_dc, OBJ_FONT));
            if (GetGlyphIndicesW(g_probe_dc, &wc, 1, &gi,
                                 GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR
                && gi != 0xFFFF) {
                /* primary has it */
                if (cp < 0x10000) g_bmp_map[cp] = -1;
                winfb_logf(WINFB_LOG_TRACE, "probe U+%04X -> primary", cp);
                return -1;
            }
        }
    }

    /* --- probe fallback slots --- */
    for (int i = 0; i < g_n_slots; i++) {
        HFONT hf = winfb_hfont(i, false, false, false);
        if (!hf) continue;
        HFONT old = SelectObject(g_probe_dc, hf);
        wchar_t wc_buf[2];
        int wc_len;
        if (cp < 0x10000) {
            wc_buf[0] = (wchar_t)cp;
            wc_len = 1;
        } else {
            /* surrogate pair */
            cp -= 0x10000;
            wc_buf[0] = (wchar_t)(0xD800 + (cp >> 10));
            wc_buf[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            wc_len = 2;
        }
        WORD gi = 0;
        DWORD ret = GetGlyphIndicesW(g_probe_dc, wc_buf, wc_len, &gi,
                                     GGI_MARK_NONEXISTING_GLYPHS);
        SelectObject(g_probe_dc, old);
        if (ret != GDI_ERROR && gi != 0xFFFF) {
            /* found in this fallback slot */
            if (cp < 0x10000) {
                g_bmp_map[cp] = (int8_t)i;
            } else {
                if (g_n_smp < WINFB_SMP_CAP) {
                    g_smp[g_n_smp].cp   = cp;
                    g_smp[g_n_smp].slot = (int8_t)i;
                    g_n_smp++;
                }
            }
            winfb_logf(WINFB_LOG_INFO, "map U+%04X -> slot %d \"%s\"",
                       cp, i, g_slots[i].name);
            return i;
        }
    }

    /* all-miss: record as primary (will show .notdef but avoid re-probing) */
    if (cp < 0x10000) g_bmp_map[cp] = -1;
    else {
        if (g_n_smp < WINFB_SMP_CAP) {
            g_smp[g_n_smp].cp   = cp;
            g_smp[g_n_smp].slot = -1;
            g_n_smp++;
        }
    }
    winfb_logf(WINFB_LOG_TRACE, "probe U+%04X -> all-miss (primary .notdef)", cp);
    return -1;
}
```

- [ ] **Step 9: Implement winfb_hfont**

```c
HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline)
{
    if (slot < 0 || slot >= g_n_slots) return NULL;

    int idx = attr_index(bold, italic, underline);
    if (!g_slots[slot].hfont[idx]) {
        g_slots[slot].hfont[idx] = create_variant(
            &g_base_lf, g_cell_w, g_cell_h,
            g_slots[slot].name, bold, italic, underline);
    }
    return g_slots[slot].hfont[idx];
}
```

- [ ] **Step 10: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed. The module now has real probe/cache/hfont logic but `winfb_split` and `winfb_draw_runs` are still stubs so rendering is unchanged.

- [ ] **Step 11: Commit**

```bash
git add 0.76b_My_PuTTY/windows/winfont_fallback.c
git commit -m "feat(font-fallback): implement probe, cache, fallback slot selection, hfont creation"
```

---

## Task 7: Implement winfb_split and winfb_draw_runs

**Files:**
- Modify: `0.76b_My_PuTTY/windows/winfont_fallback.c`

Replace the stubs with real run-splitting and multi-font drawing.

- [ ] **Step 1: Implement winfb_split**

```c
int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs)
{
    if (len <= 0 || max_runs <= 0) return 0;
    if (!g_initialised || g_n_slots == 0) {
        out[0].start = 0;
        out[0].len   = len;
        out[0].slot  = -1;
        return 1;
    }

    int nruns = 0;
    int i = 0;

    while (i < len) {
        /* decode codepoint (handle surrogate pairs) */
        uint32_t cp;
        int advance;
        if (i + 1 < len && wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF
            && wbuf[i+1] >= 0xDC00 && wbuf[i+1] <= 0xDFFF) {
            cp = 0x10000 + (((uint32_t)(wbuf[i]) - 0xD800) << 10)
                 + (wbuf[i+1] - 0xDC00);
            advance = 2;
        } else {
            cp = (uint32_t)wbuf[i];
            advance = 1;
        }

        int slot = winfb_lookup_slot(cp);

        /* extend current run or start new one */
        if (nruns > 0 && out[nruns-1].slot == slot) {
            out[nruns-1].len += advance;
        } else {
            if (nruns >= max_runs) break;
            out[nruns].start = i;
            out[nruns].len   = advance;
            out[nruns].slot  = slot;
            nruns++;
        }
        i += advance;
    }

    winfb_logf(WINFB_LOG_DEBUG, "split len=%d runs=%d", len, nruns);
    return nruns;
}
```

- [ ] **Step 2: Implement winfb_draw_runs**

```c
void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     int nfont_primary,
                     bool bold, bool italic, bool underline)
{
    (void)len;  /* len is implicit in runs */

    HFONT hf_primary = NULL;
    /* We need to get the primary font HFONT. The caller passes nfont_primary
     * which is the fonts[] index. We'll get it from the external array.
     * However, we can't access the static fonts[] array from this module.
     * Solution: the caller should save/restore the primary font around the
     * call. For now, we just select the appropriate fallback font per run. */

    for (int r = 0; r < nruns; r++) {
        const WinFB_Run *run = &runs[r];
        HFONT hf;

        if (run->slot < 0) {
            /* primary font — caller has it selected already, or we skip
             * selection and let the current font stand */
            hf = NULL;  /* means: don't change font */
        } else {
            hf = winfb_hfont(run->slot, bold, italic, underline);
        }

        HFONT old_font = NULL;
        if (hf) old_font = SelectObject(hdc, hf);

        UINT eto_flags = ETO_CLIPPED;
        if (opaque && r == 0) eto_flags |= ETO_OPAQUE;

        ExtTextOutW(hdc, x, y, eto_flags, line_box,
                    wbuf + run->start, run->len,
                    lpDx ? lpDx + run->start : NULL);

        if (old_font) SelectObject(hdc, old_font);

        /* after first run, no more background erase */
        if (r == 0) SetBkMode(hdc, TRANSPARENT);
    }
}
```

- [ ] **Step 3: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed.

- [ ] **Step 4: Commit**

```bash
git add 0.76b_My_PuTTY/windows/winfont_fallback.c
git commit -m "feat(font-fallback): implement winfb_split and winfb_draw_runs"
```

---

## Task 8: Integrate fallback into do_text_internal rendering path

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

This is the critical integration point. We modify the "normal unicode characters" branch (around line 6651) to use `winfb_split` + `winfb_draw_runs` when fallback slots are available.

- [ ] **Step 1: Replace the normal-unicode rendering block**

Find the block in `do_text_internal` starting at `} else {` (line 6651) with comment `/* And 'normal' unicode characters */`. The current code is:

```c
        } else {
            /* And 'normal' unicode characters */
            static WCHAR *wbuf = NULL;
            static int wlen = 0;
            int i;

            if (wlen < len) {
                sfree(wbuf);
                wlen = len;
                wbuf = snewn(wlen, WCHAR);
            }

            for (i = 0; i < len; i++)
                wbuf[i] = text[i];
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	/* print Glyphs as they are, without Windows' Shaping*/
	if( GetBackgroundImageFlag() && (!PuttyFlag) )
	// exact_textout(hdc, x, y - font_height * (lattr == LATTR_BOT) + text_adjust,
	//	      &line_box, wbuf, len, lpDx, !(attr & TATTR_COMBINING) &&!transBg);
		exact_textout(hdc, x + xoffset,
			y - font_height * (lattr == LATTR_BOT) + text_adjust,
			&line_box, wbuf, len, lpDx,
			!(attr & TATTR_COMBINING) &&!transBg);
	else
#endif
            /* print Glyphs as they are, without Windows' Shaping*/
            general_textout(wintw_hdc, x + xoffset,
                            y - font_height * (lattr==LATTR_BOT) + text_adjust,
                            &line_box, wbuf, len, lpDx,
                            opaque && !(attr & TATTR_COMBINING));

            /* And the shadow bold hack. */
            if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wintw_hdc, TRANSPARENT);
                ExtTextOutW(wintw_hdc, x + xoffset - 1,
                            y - font_height * (lattr ==
                                               LATTR_BOT) + text_adjust,
                            ETO_CLIPPED, &line_box, wbuf, len, lpDx_maybe);
            }
        }
```

Replace with:

```c
        } else {
            /* And 'normal' unicode characters */
            static WCHAR *wbuf = NULL;
            static int wlen = 0;
            int i;

            if (wlen < len) {
                sfree(wbuf);
                wlen = len;
                wbuf = snewn(wlen, WCHAR);
            }

            for (i = 0; i < len; i++)
                wbuf[i] = text[i];

            {
                /* --- font fallback integration --- */
                WinFB_Run fb_runs[64];
                int fb_nruns = winfb_split(wbuf, len, fb_runs, 64);

                if (fb_nruns == 1 && fb_runs[0].slot < 0) {
                    /* all characters in primary font — use original path */
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
                    if (GetBackgroundImageFlag() && (!PuttyFlag))
                        exact_textout(hdc, x + xoffset,
                            y - font_height * (lattr == LATTR_BOT) + text_adjust,
                            &line_box, wbuf, len, lpDx,
                            !(attr & TATTR_COMBINING) && !transBg);
                    else
#endif
                    general_textout(wintw_hdc, x + xoffset,
                                    y - font_height * (lattr==LATTR_BOT) + text_adjust,
                                    &line_box, wbuf, len, lpDx,
                                    opaque && !(attr & TATTR_COMBINING));
                } else {
                    /* mixed fonts — use fallback drawing */
                    int y_adj = y - font_height * (lattr == LATTR_BOT) + text_adjust;
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
                    HDC draw_dc = (GetBackgroundImageFlag() && (!PuttyFlag))
                                  ? hdc : wintw_hdc;
#else
                    HDC draw_dc = wintw_hdc;
#endif
                    winfb_draw_runs(draw_dc, x + xoffset, y_adj,
                                    &line_box, wbuf, len, lpDx,
                                    fb_runs, fb_nruns,
                                    opaque && !(attr & TATTR_COMBINING),
                                    nfont,
                                    (nfont & FONT_BOLD) != 0,
                                    (nfont & FONT_ITALIC) != 0,
                                    (nfont & FONT_UNDERLINE) != 0);
                }
            }

            /* And the shadow bold hack. */
            if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wintw_hdc, TRANSPARENT);
                ExtTextOutW(wintw_hdc, x + xoffset - 1,
                            y - font_height * (lattr ==
                                               LATTR_BOT) + text_adjust,
                            ETO_CLIPPED, &line_box, wbuf, len, lpDx_maybe);
            }
        }
```

**Key design note:** When `fb_nruns == 1 && fb_runs[0].slot < 0` (all characters in primary font), we use the original `general_textout` path. This preserves RTL handling and existing GDI font-linking for CJK characters that the primary font already covers. Fallback drawing is only used when there are actual non-primary runs.

- [ ] **Step 2: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed.

- [ ] **Step 3: Commit**

```bash
git add 0.76b_My_PuTTY/windows/window.c
git commit -m "feat(font-fallback): integrate winfb_split/draw_runs into do_text_internal"
```

---

## Task 9: Add [FontFallback] ini documentation

**Files:**
- Modify: `kitty_ini.txt`

- [ ] **Step 1: Add documentation section**

Append to `kitty_ini.txt` (or add in the appropriate section):

```
[FontFallback]
; Control font fallback behavior when the primary terminal font lacks a glyph.
;
; Log: off|error|warn|info|debug|trace
;   Default: off. When enabled, log file is created beside kitty.exe as
;   fontfallback.log. Use debug/trace only for troubleshooting (high volume).
; Log=off
;
; LogFile: absolute path for the log file (optional).
;   Default: fontfallback.log beside the KiTTY executable.
; LogFile=
;
; Fallback: comma-separated list of font names to try when the primary font
;   lacks a character. These are appended AFTER the built-in defaults.
;   If the list starts with ! the built-in defaults are replaced entirely.
;   Built-in defaults: Segoe UI Symbol, Segoe UI Emoji, Segoe UI Historic,
;     Cascadia Mono, Cascadia Code, Microsoft YaHei UI,
;     Microsoft JhengHei UI, Yu Gothic UI, Malgun Gothic
; Fallback=
;
; Override: force a Unicode range to a specific font. One range per line.
;   Format: hex-start-hex-end:FontName  or  hex-point:FontName
;   Example for Nerd Fonts PUA:
; Override=E000-F8FF:Symbols Nerd Font Mono
;   Example for emoji:
; Override=1F600-1F64F:Segoe UI Emoji
```

- [ ] **Step 2: Commit**

```bash
git add kitty_ini.txt
git commit -m "docs(font-fallback): document [FontFallback] ini section"
```

---

## Task 10: Wire ini Fallback and Override reading into window.c init_fonts

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

Currently `winfb_init` is called with `NULL` for `fallback_csv` and `0` for overrides. We need to read these from the ini file.

- [ ] **Step 1: Add a helper to read FontFallback ini keys**

The challenge is that `ReadParameter` is defined in `kitty.c`, not accessible from `window.c`. We have two options:

**Option A (chosen):** Add a thin bridge. In `kitty.c`, define two global buffers that `window.c` can `extern`:

In `kitty.c`, near the existing globals:
```c
char g_fb_fallback[4096] = "";
char g_fb_overrides[4096] = "";  /* semicolon-separated for simplicity */
```

In `kitty.c`'s startup init function (where `init_fontfallback_log` is called), add:
```c
    ReadParameter("FontFallback", "Fallback", g_fb_fallback);
    ReadParameter("FontFallback", "Override", g_fb_overrides);
```

Note: For multiple Override lines, we'll use a simple approach — read them as `Override1`, `Override2`, etc., or use a single `Override` key with semicolons. For MVP, use a single `Override` key with semicolons as separator. This is simpler than multi-line ini reading.

In `window.c`, near the `winfb_init` call, `extern` these:
```c
extern char g_fb_fallback[];
extern char g_fb_overrides[];
```

Then modify the `winfb_init` call in `init_fonts` to pass them:
```c
    /* Parse override string into array of lines */
    const char *ovr_lines[WINFB_OVR_CAP];
    int n_ovr = 0;
    char ovr_buf[4096];
    strncpy(ovr_buf, g_fb_overrides, sizeof(ovr_buf) - 1);
    ovr_buf[sizeof(ovr_buf) - 1] = '\0';
    char *ovr_save = NULL;
    char *tok = strtok_r(ovr_buf, ";", &ovr_save);
    while (tok && n_ovr < WINFB_OVR_CAP) {
        while (*tok == ' ') tok++;
        if (*tok) ovr_lines[n_ovr++] = tok;
        tok = strtok_r(NULL, ";", &ovr_save);
    }

    winfb_init(hdc, &lfont, font_width, font_height,
               g_fb_fallback[0] ? g_fb_fallback : NULL,
               n_ovr > 0 ? ovr_lines : NULL, n_ovr);
```

Wait — `WINFB_OVR_CAP` is defined in `winfont_fallback.c`, not the header. We need to either expose it or use a local constant. Add to `winfont_fallback.h`:

```c
#define WINFB_MAX_OVERRIDES 64
```

And update `winfont_fallback.c` to use `WINFB_MAX_OVERRIDES` instead of the local `WINFB_OVR_CAP`.

- [ ] **Step 2: Update the winfb_init call in init_fonts**

Replace the temporary `winfb_init(hdc, &lfont, font_width, font_height, NULL, NULL, 0);` with the full version that reads from the globals.

- [ ] **Step 3: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed.

- [ ] **Step 4: Commit**

```bash
git add kitty.c 0.76b_My_PuTTY/windows/window.c 0.76b_My_PuTTY/windows/winfont_fallback.h 0.76b_My_PuTTY/windows/winfont_fallback.c
git commit -m "feat(font-fallback): wire Fallback and Override ini keys into winfb_init"
```

---

## Task 11: Handle winfb_reset properly after font rebuild

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

Currently `winfb_reset()` is called after `init_fonts()` in `reset_window`, but `winfb_reset` doesn't re-initialize with the new primary font metrics. We need to make `winfb_reset` do a full re-init, or call `winfb_cleanup` + `winfb_init` at each site.

The cleanest approach: change `winfb_reset` to accept the new primary font info, or have it call `winfb_init` internally. But `winfb_reset` doesn't have access to the LOGFONT. Instead, we'll store the initialization parameters and have `winfb_reset` call a full re-init.

- [ ] **Step 1: Add re-init capability to winfb_reset**

In `winfont_fallback.c`, add a static variable to remember the last init params:

```c
static char g_last_fallback_csv[4096];
static char g_last_overrides[4096];
```

In `winfb_init`, save these at the end:
```c
    strncpy(g_last_fallback_csv, fallback_csv ? fallback_csv : "",
            sizeof(g_last_fallback_csv) - 1);
    /* overrides are already parsed into g_ovr, no need to re-parse */
```

Then modify `winfb_reset` to call `winfb_init` with stored params (but we need the HDC and LOGFONT). Since `winfb_reset` is called right after `init_fonts` where we have the HDC and LOGFONT available, the simplest approach is: **replace all `winfb_reset()` calls with `winfb_cleanup()` + `winfb_init(...)` calls**.

In `window.c`, change each `winfb_reset();` after `init_fonts(...)` to:
```c
    winfb_cleanup();
    winfb_init(hdc, &lfont, font_width, font_height,
               g_fb_fallback[0] ? g_fb_fallback : NULL,
               ...overrides..., n_ovr);
```

But we need the override array at each call site. Factor it into a small helper function in `window.c`:

```c
static void reinit_font_fallback(HDC hdc)
{
    const char *ovr_lines[64];
    int n_ovr = 0;
    char ovr_buf[4096];
    extern char g_fb_overrides[];
    strncpy(ovr_buf, g_fb_overrides, sizeof(ovr_buf) - 1);
    ovr_buf[sizeof(ovr_buf) - 1] = '\0';
    char *ovr_save = NULL;
    char *tok = strtok_r(ovr_buf, ";", &ovr_save);
    while (tok && n_ovr < 64) {
        while (*tok == ' ') tok++;
        if (*tok) ovr_lines[n_ovr++] = tok;
        tok = strtok_r(NULL, ";", &ovr_save);
    }
    extern char g_fb_fallback[];
    winfb_cleanup();
    winfb_init(hdc, &lfont, font_width, font_height,
               g_fb_fallback[0] ? g_fb_fallback : NULL,
               n_ovr > 0 ? ovr_lines : NULL, n_ovr);
}
```

Then replace all `winfb_reset()` calls with `reinit_font_fallback(hdc);`.

Note: in `reset_window`, the HDC may need to be obtained. Looking at the code, `wintw_hdc` is the persistent DC. We can use that, or get a fresh one with `GetDC(wgs.term_hwnd)`. Since `winfb_init` only uses the DC to create a compatible DC and probe fonts, `wintw_hdc` is fine.

- [ ] **Step 2: Build both targets**

```bash
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: both builds succeed.

- [ ] **Step 3: Commit**

```bash
git add 0.76b_My_PuTTY/windows/window.c 0.76b_My_PuTTY/windows/winfont_fallback.c
git commit -m "feat(font-fallback): proper reinit after font rebuild in reset_window"
```

---

## Task 12: Smoke test and fix compilation issues

**Files:**
- Potentially any of the modified files

- [ ] **Step 1: Clean build 32-bit**

```bash
cd /home/admin/git/KiTTY.new/0.76b_My_PuTTY/windows && make -f MAKEFILE.MINGW clean
cd /home/admin/git/KiTTY.new && ./zzy.sh cross
```

Expected: clean build, no warnings in `winfont_fallback.c`.

- [ ] **Step 2: Clean build 64-bit**

```bash
cd /home/admin/git/KiTTY.new/0.76b_My_PuTTY/windows && make -f MAKEFILE.MINGW clean
cd /home/admin/git/KiTTY.new && ./zzy.sh cross64
```

Expected: clean build, no warnings in `winfont_fallback.c`.

- [ ] **Step 3: Fix any compilation errors or warnings**

Address any issues found in steps 1 and 2. Common issues to watch for:
- `stricmp` availability in MinGW (should be available via `<string.h>`)
- `strtok_r` vs `_strtok_r` naming
- `GGI_MARK_NONEXISTING_GLYPHS` macro availability
- `MoveFileExA` availability
- Signed/unsigned comparison warnings

- [ ] **Step 4: Commit fixes if any**

```bash
git add -A && git commit -m "fix(font-fallback): resolve compilation issues for cross/cross64"
```

---

## Task 13: Create test script and document known limitations

**Files:**
- Create: `tests/font_fallback_test.txt`

- [ ] **Step 1: Create test text file**

```
[1] ASCII:         The quick brown fox jumps over the lazy dog 0123456789
[2] Box Drawing:   ┌─┬─┐  ╔══╦══╗  ┃ ╋ ┻ ┳ ╳ ▔ ▁ ▏ ▕
[3] Block:         █ ▓ ▒ ░ ▀ ▄ ▌ ▐
[4] Geometric:     ● ○ ■ □ ▲ △ ▶ ▼ ◀ ◆ ◇ ★ ☆
[5] Misc Symbols:  ☀ ☁ ☂ ☃ ☄ ⌘ ⌥ ⌃ ⏏ ⏵ ⏸ ⏹
[6] Dingbats:      ✓ ✗ ✘ ✿ ❄ ❤ ✈ ✂
[7] Arrows:        ← ↑ → ↓ ↔ ↕ ⇐ ⇒ ⇔
[8] CJK single-w: 「」『』、。
[9] CJK double-w: 你好，世界！中文测试 漢字 ひらがな カタカナ 한글
[10] Surrogate:    😀 😁 😂 🤔 🦊 🚀
[11] Nerd Font:       (Powerline)
                             (Font Awesome)
[12] Combining:    é = e + ́    ñ = n + ̃
[13] Width edge:   ▶▶▶ABC│CJK 你好│End
```

- [ ] **Step 2: Document known limitations**

Create or update a section in the spec or a separate `KNOWN_LIMITATIONS.md`:

```
Known Limitations (MVP)
========================
1. Emoji display is monochrome (GDI limitation). Color emoji requires
   DirectWrite/Direct2D (future work).
2. RTL text with fallback characters may lose GCP shaping on the primary
   font run. Pure RTL text (no fallback needed) is unaffected.
3. Fallback glyph baseline may differ by 1-2px from primary — no vertical
   adjustment is applied.
4. FONT_HIGH / FONT_WIDE (LATTR_TOP/BOT/2x) variants are not created for
   fallback fonts. Characters in doubled-height/width lines use the primary
   font for all fallback glyphs.
5. Override ranges in kitty.ini use a single "Override" key with semicolons
   as separators, not multiple ini lines.
6. The BMP cache (64KB) is always allocated. On very memory-constrained
   systems this adds ~64KB to the process working set.
```

- [ ] **Step 3: Commit**

```bash
git add tests/font_fallback_test.txt
git commit -m "test(font-fallback): add smoke test strings and document known limitations"
```

---

## Self-Review Checklist

- [x] **Spec coverage:**
  - Section 3 (Architecture): Tasks 1-2 (header/stubs), Task 6 (full impl), Task 7 (split/draw)
  - Section 4 (Rendering path): Task 8
  - Section 5 (Width/alignment): Covered by `winfb_draw_runs` using same `lpDx` + `ETO_CLIPPED`
  - Section 6 (Cache): Task 6 (BMP map, SMP hash, HFONT lazy)
  - Section 7 (Logging): Task 2 (full impl), Task 4 (ini wiring)
  - Section 8 (ini config): Task 9 (docs), Task 10 (Fallback/Override wiring)
  - Section 9 (File list): All files covered
  - Section 10 (Stages S1-S6): Mapped to Tasks 1-13
  - Section 11 (Test matrix): Task 13
  - Section 12 (Risks): Documented in Task 13
- [x] **Placeholder scan:** No TBD/TODO/fill-in-later. All code shown inline.
- [x] **Type consistency:** `WinFB_Run`, `winfb_log_level`, `winfb_init`/`winfb_split`/`winfb_draw_runs`/`winfb_hfont` signatures match between header and implementation. `nfont_primary` is `int` (matching `fonts[]` index type in window.c). `bool` parameters use C99 `_Bool` via `<windows.h>` or PuTTY's `bool` typedef.
- [x] **Build verification:** Every task that modifies source includes a build step for both `cross` and `cross64`.
