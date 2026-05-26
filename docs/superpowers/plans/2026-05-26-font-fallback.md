# Font Fallback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a font fallback mechanism to KiTTY that automatically uses system fonts for PUA (Nerd Font) and Box Drawing characters missing from the primary font, without breaking the cell-based rendering model.

**Architecture:** DirectWrite's `IDWriteFontFallback::MapCharacters()` identifies the best system font per codepoint; the result is converted to `HFONT` via `IDWriteGdiInterop::ConvertFontToLOGFONT()`; actual drawing continues through the existing GDI `ExtTextOutW` path. A 4096-slot hash cache avoids per-frame DWrite queries. The new module is a `.cpp` file with `extern "C"` exports, linked into the existing C build.

**Tech Stack:** C (existing), C++11 (new module only), DirectWrite (`dwrite.h` / `-ldwrite`, available in MinGW-w64), GDI32, MinGW-w64 cross-compiler.

**Build command used throughout this plan:**
```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -15
```

**Spec:** `docs/superpowers/specs/2026-05-26-font-fallback-design.md`

---

## File Map

| File | Action | Purpose |
|---|---|---|
| `kitty_fontfallback.h` | **Create** | Public C API: `KffResult`, `kff_init/lookup/char_width/deinit` |
| `kitty_fontfallback.cpp` | **Create** | DWrite init, HFONT pool, hash cache, `kff_lookup()` |
| `0.76b_My_PuTTY/windows/MAKEFILE.MINGW` | **Modify** | Add CXX rule, `kitty_fontfallback.o`, `-ldwrite` |
| `0.76b_My_PuTTY/windows/window.c` | **Modify** | Hook `init_fonts`/`deinit_fonts`; segment loop in `do_text_internal` |
| `0.76b_My_PuTTY/terminal/terminal.c` | **Modify** | `term_char_width()` PUA branch: query fallback font width |

---

## Task 1: Build Scaffolding — Stub Files + Makefile

**Files:**
- Create: `kitty_fontfallback.h`
- Create: `kitty_fontfallback.cpp`
- Modify: `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`

- [ ] **Step 1.1 — Create the public header stub**

Create `/home/admin/git/KiTTY.font.fallback.worktree/kitty_fontfallback.h`:

```c
#ifndef KITTY_FONTFALLBACK_H
#define KITTY_FONTFALLBACK_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HFONT hfont;    /* NULL = use primary font */
    int   glyph_px; /* actual glyph pixel width; 0 = unknown */
} KffResult;

/* Call at end of init_fonts(), after fonts[FONT_NORMAL] is ready. */
void kff_init(HFONT primary_hfont, const LOGFONT *primary_lf, int quality);

/* Return fallback info for a Unicode codepoint (BMP or supplementary).
   Returns {NULL, 0} if the primary font already has the glyph. */
KffResult kff_lookup(unsigned int codepoint);

/* Return cell width (1 or 2) derived from fallback font metrics.
   Returns 0 if codepoint is not in any fallback font. */
int kff_char_width(unsigned int codepoint, int font_width);

/* Call at end of deinit_fonts(). Releases all DWrite COM objects and HFONTs. */
void kff_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* KITTY_FONTFALLBACK_H */
```

- [ ] **Step 1.2 — Create the minimal stub .cpp**

Create `/home/admin/git/KiTTY.font.fallback.worktree/kitty_fontfallback.cpp`:

```cpp
#include "kitty_fontfallback.h"

extern "C" {

void kff_init(HFONT primary_hfont, const LOGFONT *primary_lf, int quality)
{
    (void)primary_hfont; (void)primary_lf; (void)quality;
}

KffResult kff_lookup(unsigned int codepoint)
{
    (void)codepoint;
    KffResult r = {NULL, 0};
    return r;
}

int kff_char_width(unsigned int codepoint, int font_width)
{
    (void)codepoint; (void)font_width;
    return 0;
}

void kff_deinit(void) {}

} /* extern "C" */
```

- [ ] **Step 1.3 — Add CXX rule and object to MAKEFILE.MINGW**

In `0.76b_My_PuTTY/windows/MAKEFILE.MINGW`, find line 97 (`CC = $(TOOLPATH)gcc`) and add immediately after it:

```makefile
CXX      = $(TOOLPATH)g++
CXXFLAGS = $(CFLAGS) -std=c++11
```

Find the `putty.exe` dependency list (line ~413). After `kitty_store.o kitty_tools.o kitty_win.o \` add:
```makefile
		kitty_fontfallback.o \
```

Do the same in the linker object list (the line after `$(CC) -mwindows $(LDFLAGS) -o $@ ...`) — same insertion point after `kitty_win.o \`.

Find the linker flags line for putty.exe (~line 476):
```
		-ladvapi32 -lcomdlg32 -lgdi32 -limm32 -lpsapi -lole32 -lshell32 \
		-luser32 -lwsock32  -lwtsapi32 \
```
Change to:
```
		-ladvapi32 -lcomdlg32 -lgdi32 -limm32 -lpsapi -lole32 -lshell32 \
		-luser32 -lwsock32  -lwtsapi32 -ldwrite \
```

At the end of MAKEFILE.MINGW (before `.PHONY`), add the compile rule:
```makefile
kitty_fontfallback.o: ../../kitty_fontfallback.cpp ../../kitty_fontfallback.h
	$(CXX) $(COMPAT) $(XFLAGS) $(CXXFLAGS) -c ../../kitty_fontfallback.cpp
```

- [ ] **Step 1.4 — Build and verify it compiles**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -15
```

Expected: `kitty_fontfallback.o` compiled, `putty.exe` linked without errors. No DWrite symbols missing (stub doesn't call DWrite yet).

- [ ] **Step 1.5 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.h kitty_fontfallback.cpp 0.76b_My_PuTTY/windows/MAKEFILE.MINGW
git commit -m "feat(fallback): add kitty_fontfallback stub + Makefile wiring"
```

---

## Task 2: DWrite Initialization and Teardown

**Files:**
- Modify: `kitty_fontfallback.cpp` — add `KffState`, `kff_init()`, `kff_deinit()`

- [ ] **Step 2.1 — Replace stub .cpp with full init/deinit**

Replace the contents of `kitty_fontfallback.cpp` with:

```cpp
#define COBJMACROS
#include "kitty_fontfallback.h"
#include <dwrite.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                       */
/* ------------------------------------------------------------------ */

#define KFF_CACHE_BUCKETS 4096

typedef struct {
    unsigned int codepoint; /* 0 = empty slot */
    KffResult    result;
} KffCacheEntry;

typedef struct KffFontNode {
    LOGFONTW          lf;
    HFONT             hfont;
    struct KffFontNode *next;
} KffFontNode;

typedef struct {
    IDWriteFactory2     *dw_factory;
    IDWriteFontFallback *dw_fallback;
    IDWriteGdiInterop   *dw_gdi_interop;

    HFONT    primary_hfont;
    LOGFONTW primary_lfw;   /* wide version of the primary LOGFONT */
    int      quality;       /* lfQuality to apply to fallback HFONTs */

    KffCacheEntry cache[KFF_CACHE_BUCKETS];
    KffFontNode  *font_pool;

    bool initialized;
} KffState;

static KffState g_kff;

/* Built-in fallback priority list for PUA characters */
static const wchar_t *kff_builtin_fonts[] = {
    L"Symbols Nerd Font Mono",
    L"Symbols Nerd Font",
    L"Segoe UI Symbol",
    L"Segoe UI Emoji",
    L"Cascadia Mono",
    L"Consolas",
    L"Microsoft YaHei",
    L"SimSun",
    NULL
};

/* ------------------------------------------------------------------ */
/* IDWriteTextAnalysisSource — minimal impl for MapCharacters()         */
/* ------------------------------------------------------------------ */

class KffAnalysisSource : public IDWriteTextAnalysisSource {
    const wchar_t *m_text;
    UINT32         m_len;
public:
    KffAnalysisSource(const wchar_t *text, UINT32 len)
        : m_text(text), m_len(len) {}

    /* IUnknown — stack-allocated, no real ref counting */
    ULONG   STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override {
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* IDWriteTextAnalysisSource */
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(
        UINT32 pos, const wchar_t **text, UINT32 *len) override {
        if (pos >= m_len) { *text = nullptr; *len = 0; }
        else              { *text = m_text + pos; *len = m_len - pos; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(
        UINT32 pos, const wchar_t **text, UINT32 *len) override {
        if (pos == 0) { *text = nullptr; *len = 0; }
        else          { *text = m_text; *len = pos; }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(
        UINT32, UINT32 *len, const wchar_t **name) override {
        *name = L""; *len = m_len; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        UINT32, UINT32 *len, IDWriteNumberSubstitution **sub) override {
        *len = m_len; *sub = nullptr; return S_OK;
    }
};

/* ------------------------------------------------------------------ */
/* Cache helpers                                                        */
/* ------------------------------------------------------------------ */

static void cache_clear(void)
{
    memset(g_kff.cache, 0, sizeof(g_kff.cache));
}

static bool cache_get(unsigned int cp, KffResult *out)
{
    if (cp == 0) return false;
    unsigned int slot = cp % KFF_CACHE_BUCKETS;
    for (int i = 0; i < KFF_CACHE_BUCKETS; i++) {
        unsigned int s = (slot + i) % KFF_CACHE_BUCKETS;
        if (g_kff.cache[s].codepoint == 0)  return false; /* empty */
        if (g_kff.cache[s].codepoint == cp) { *out = g_kff.cache[s].result; return true; }
    }
    return false;
}

static void cache_set(unsigned int cp, KffResult r)
{
    if (cp == 0) return;
    unsigned int slot = cp % KFF_CACHE_BUCKETS;
    for (int i = 0; i < KFF_CACHE_BUCKETS; i++) {
        unsigned int s = (slot + i) % KFF_CACHE_BUCKETS;
        if (g_kff.cache[s].codepoint == 0 || g_kff.cache[s].codepoint == cp) {
            g_kff.cache[s].codepoint = cp;
            g_kff.cache[s].result    = r;
            return;
        }
    }
    /* Table full (shouldn't happen for typical terminal usage) — overwrite slot */
    g_kff.cache[slot].codepoint = cp;
    g_kff.cache[slot].result    = r;
}

/* ------------------------------------------------------------------ */
/* HFONT pool                                                           */
/* ------------------------------------------------------------------ */

static bool logfontw_eq(const LOGFONTW *a, const LOGFONTW *b)
{
    return a->lfHeight    == b->lfHeight    &&
           a->lfWeight    == b->lfWeight    &&
           a->lfItalic    == b->lfItalic    &&
           a->lfQuality   == b->lfQuality   &&
           a->lfCharSet   == b->lfCharSet   &&
           wcscmp(a->lfFaceName, b->lfFaceName) == 0;
}

static HFONT pool_get_or_create(const LOGFONTW *lf)
{
    for (KffFontNode *n = g_kff.font_pool; n; n = n->next)
        if (logfontw_eq(&n->lf, lf)) return n->hfont;

    HFONT hf = CreateFontIndirectW(lf);
    if (!hf) return NULL;

    KffFontNode *node = (KffFontNode *)malloc(sizeof(KffFontNode));
    if (!node) { DeleteObject(hf); return NULL; }
    node->lf    = *lf;
    node->hfont = hf;
    node->next  = g_kff.font_pool;
    g_kff.font_pool = node;
    return hf;
}

static void pool_free_all(void)
{
    KffFontNode *n = g_kff.font_pool;
    while (n) {
        DeleteObject(n->hfont);
        KffFontNode *next = n->next;
        free(n);
        n = next;
    }
    g_kff.font_pool = NULL;
}

/* ------------------------------------------------------------------ */
/* Glyph existence check (GDI)                                          */
/* ------------------------------------------------------------------ */

static bool font_has_glyph(HFONT hfont, unsigned int cp)
{
    wchar_t wch[2];
    int     wlen;
    WORD    idx[2] = {0, 0};

    if (cp < 0x10000) {
        wch[0] = (wchar_t)cp;
        wlen = 1;
    } else {
        /* Encode as surrogate pair */
        cp -= 0x10000;
        wch[0] = (wchar_t)(0xD800 + (cp >> 10));
        wch[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        wlen = 2;
    }

    HDC hdc = GetDC(NULL);
    if (!hdc) return false;
    SelectObject(hdc, hfont);
    GetGlyphIndicesW(hdc, wch, wlen, idx, GGI_MARK_NONEXISTING_GLYPHS);
    ReleaseDC(NULL, hdc);

    /* For surrogate pairs, the first idx holds the result */
    return idx[0] != 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* Glyph pixel-width measurement (GDI)                                  */
/* ------------------------------------------------------------------ */

static int measure_glyph_px(HFONT hfont, unsigned int cp)
{
    wchar_t wch[2];
    int     wlen;

    if (cp < 0x10000) {
        wch[0] = (wchar_t)cp;
        wlen = 1;
    } else {
        cp -= 0x10000;
        wch[0] = (wchar_t)(0xD800 + (cp >> 10));
        wch[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        wlen = 2;
    }

    HDC hdc = GetDC(NULL);
    if (!hdc) return 0;
    SelectObject(hdc, hfont);

    SIZE sz = {0, 0};
    GetTextExtentPoint32W(hdc, wch, wlen, &sz);
    ReleaseDC(NULL, hdc);
    return (int)sz.cx;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

extern "C" {

void kff_init(HFONT primary_hfont, const LOGFONT *primary_lf, int quality)
{
    /* Reset state */
    kff_deinit();

    g_kff.primary_hfont = primary_hfont;
    g_kff.quality       = quality;

    /* Convert LOGFONT (ANSI) to LOGFONTW (wide) for internal use */
    memset(&g_kff.primary_lfw, 0, sizeof(g_kff.primary_lfw));
    g_kff.primary_lfw.lfHeight    = primary_lf->lfHeight;
    g_kff.primary_lfw.lfWidth     = 0; /* let system choose */
    g_kff.primary_lfw.lfWeight    = primary_lf->lfWeight;
    g_kff.primary_lfw.lfItalic    = primary_lf->lfItalic;
    g_kff.primary_lfw.lfQuality   = (BYTE)quality;
    g_kff.primary_lfw.lfCharSet   = DEFAULT_CHARSET;
    g_kff.primary_lfw.lfPitchAndFamily = FF_DONTCARE;
    MultiByteToWideChar(CP_ACP, 0,
        primary_lf->lfFaceName, -1,
        g_kff.primary_lfw.lfFaceName, LF_FACESIZE);

    /* Initialise DirectWrite */
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory2),
        (IUnknown **)&g_kff.dw_factory);
    if (FAILED(hr) || !g_kff.dw_factory) return; /* silent degrade */

    hr = g_kff.dw_factory->GetSystemFontFallback(&g_kff.dw_fallback);
    if (FAILED(hr)) { g_kff.dw_factory->Release(); g_kff.dw_factory = NULL; return; }

    hr = g_kff.dw_factory->GetGdiInterop(&g_kff.dw_gdi_interop);
    if (FAILED(hr)) {
        g_kff.dw_fallback->Release(); g_kff.dw_fallback = NULL;
        g_kff.dw_factory->Release();  g_kff.dw_factory  = NULL;
        return;
    }

    g_kff.initialized = true;
}

void kff_deinit(void)
{
    pool_free_all();
    cache_clear();

    if (g_kff.dw_gdi_interop) { g_kff.dw_gdi_interop->Release(); g_kff.dw_gdi_interop = NULL; }
    if (g_kff.dw_fallback)    { g_kff.dw_fallback->Release();    g_kff.dw_fallback    = NULL; }
    if (g_kff.dw_factory)     { g_kff.dw_factory->Release();     g_kff.dw_factory     = NULL; }

    g_kff.initialized    = false;
    g_kff.primary_hfont  = NULL;
    g_kff.font_pool      = NULL;
}

/* Implemented in later tasks — stubs for now */
KffResult kff_lookup(unsigned int codepoint)
{
    (void)codepoint;
    KffResult r = {NULL, 0};
    return r;
}

int kff_char_width(unsigned int codepoint, int font_width)
{
    (void)codepoint; (void)font_width;
    return 0;
}

} /* extern "C" */
```

- [ ] **Step 2.2 — Build and verify no link errors**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -15
```

Expected: Compiles and links. `DWriteCreateFactory` resolves from `-ldwrite`.

- [ ] **Step 2.3 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.cpp
git commit -m "feat(fallback): DWrite init/deinit + HFONT pool + cache scaffolding"
```

---

## Task 3: Implement `kff_lookup()` — Primary Font Check + PUA List

**Files:**
- Modify: `kitty_fontfallback.cpp` — replace `kff_lookup()` stub

This task implements `kff_lookup()` for two cases:
1. Primary font already has the glyph → return `{NULL, 0}`
2. PUA character → try builtin Nerd Font list

- [ ] **Step 3.1 — Replace `kff_lookup()` stub with full implementation**

Find the `kff_lookup()` stub in `kitty_fontfallback.cpp` and replace it entirely:

```cpp
static bool is_pua(unsigned int cp)
{
    return (cp >= 0xE000  && cp <= 0xF8FF)   ||  /* BMP PUA */
           (cp >= 0xF0000 && cp <= 0xFFFFD)  ||  /* Supplementary PUA-A */
           (cp >= 0x100000&& cp <= 0x10FFFD);     /* Supplementary PUA-B */
}

static HFONT make_fallback_hfont(const wchar_t *face_name)
{
    LOGFONTW lf = g_kff.primary_lfw;
    wcsncpy(lf.lfFaceName, face_name, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = L'\0';
    lf.lfWidth = 0;
    return pool_get_or_create(&lf);
}

/* Try user-configured fonts (none in MVP), then builtin list.
   Returns HFONT if a matching font is found, NULL otherwise. */
static HFONT find_pua_font(unsigned int cp)
{
    for (int i = 0; kff_builtin_fonts[i]; i++) {
        HFONT hf = make_fallback_hfont(kff_builtin_fonts[i]);
        if (hf && font_has_glyph(hf, cp))
            return hf;
    }
    return NULL;
}

KffResult kff_lookup(unsigned int codepoint)
{
    KffResult miss = {NULL, 0};
    if (!g_kff.initialized) return miss;
    if (codepoint == 0)     return miss;

    /* 1. Cache hit */
    KffResult cached;
    if (cache_get(codepoint, &cached)) return cached;

    /* 2. Primary font already has this glyph → no fallback needed */
    if (g_kff.primary_hfont && font_has_glyph(g_kff.primary_hfont, codepoint)) {
        cache_set(codepoint, miss);
        return miss;
    }

    KffResult result = miss;

    if (is_pua(codepoint)) {
        /* 3a. PUA: try builtin Nerd Font list */
        HFONT hf = find_pua_font(codepoint);
        if (hf) {
            result.hfont    = hf;
            result.glyph_px = measure_glyph_px(hf, codepoint);
        }
    } else {
        /* 3b. Non-PUA (Box Drawing etc.): use DWrite system fallback.
               Implemented in Task 4. */
    }

    cache_set(codepoint, result);
    return result;
}
```

- [ ] **Step 3.2 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

Expected: Compiles and links cleanly.

- [ ] **Step 3.3 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.cpp
git commit -m "feat(fallback): kff_lookup PUA path via builtin font list"
```

---

## Task 4: Implement `kff_lookup()` — DWrite MapCharacters for Non-PUA

**Files:**
- Modify: `kitty_fontfallback.cpp` — fill in the `else` branch of `kff_lookup()`

- [ ] **Step 4.1 — Add DWrite fallback function**

Add the following function above `kff_lookup()` in `kitty_fontfallback.cpp`:

```cpp
/* Query DWrite system fallback for a single non-PUA codepoint.
   Returns HFONT if a fallback font is found, NULL if unavailable. */
static HFONT find_dwrite_fallback(unsigned int cp, int *out_glyph_px)
{
    if (!g_kff.dw_fallback || !g_kff.dw_gdi_interop) return NULL;

    /* Encode codepoint as UTF-16 */
    wchar_t wch[2];
    UINT32  wlen;
    if (cp < 0x10000) {
        wch[0] = (wchar_t)cp;
        wlen = 1;
    } else {
        UINT32 v = cp - 0x10000;
        wch[0] = (wchar_t)(0xD800 + (v >> 10));
        wch[1] = (wchar_t)(0xDC00 + (v & 0x3FF));
        wlen = 2;
    }

    KffAnalysisSource source(wch, wlen);

    UINT32        mapped_len  = 0;
    IDWriteFont  *mapped_font = NULL;
    FLOAT         scale       = 1.0f;

    HRESULT hr = g_kff.dw_fallback->MapCharacters(
        &source,
        0, wlen,
        NULL,                                /* use system font collection */
        g_kff.primary_lfw.lfFaceName,
        (DWRITE_FONT_WEIGHT)g_kff.primary_lfw.lfWeight,
        g_kff.primary_lfw.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        &mapped_len,
        &mapped_font,
        &scale);

    if (FAILED(hr) || !mapped_font) return NULL;

    /* Convert IDWriteFont → LOGFONTW → HFONT */
    LOGFONTW lfw;
    BOOL     is_sys;
    hr = g_kff.dw_gdi_interop->ConvertFontToLOGFONT(mapped_font, &lfw, &is_sys);
    mapped_font->Release();
    if (FAILED(hr)) return NULL;

    /* Override metrics to match primary font height */
    lfw.lfHeight  = g_kff.primary_lfw.lfHeight;
    lfw.lfWidth   = 0;
    lfw.lfQuality = g_kff.primary_lfw.lfQuality;

    HFONT hf = pool_get_or_create(&lfw);
    if (hf && out_glyph_px)
        *out_glyph_px = measure_glyph_px(hf, cp);
    return hf;
}
```

- [ ] **Step 4.2 — Fill in the `else` branch in `kff_lookup()`**

In `kff_lookup()`, replace the comment `/* 3b. Non-PUA ... Implemented in Task 4. */` with:

```cpp
    } else {
        /* 3b. Non-PUA (Box Drawing, Geometric Shapes, etc.): DWrite system fallback */
        int px = 0;
        HFONT hf = find_dwrite_fallback(codepoint, &px);
        if (hf) {
            result.hfont    = hf;
            result.glyph_px = px;
        }
    }
```

- [ ] **Step 4.3 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

Expected: Compiles cleanly. `IDWriteFontFallback::MapCharacters` and `IDWriteGdiInterop::ConvertFontToLOGFONT` resolve at link time.

- [ ] **Step 4.4 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.cpp
git commit -m "feat(fallback): DWrite MapCharacters for non-PUA characters"
```

---

## Task 5: Implement `kff_char_width()`

**Files:**
- Modify: `kitty_fontfallback.cpp` — replace `kff_char_width()` stub

- [ ] **Step 5.1 — Replace stub**

Find the `kff_char_width()` stub and replace it:

```cpp
int kff_char_width(unsigned int codepoint, int font_width)
{
    if (!g_kff.initialized || font_width <= 0) return 0;

    KffResult r = kff_lookup(codepoint);
    if (!r.hfont || r.glyph_px <= 0) return 0;

    /* Round glyph_px to nearest cell count (1 or 2) */
    int cells = (r.glyph_px + font_width / 2) / font_width;
    if (cells < 1) cells = 1;
    if (cells > 2) cells = 2;
    return cells;
}
```

- [ ] **Step 5.2 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

- [ ] **Step 5.3 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.cpp
git commit -m "feat(fallback): implement kff_char_width"
```

---

## Task 6: Hook `window.c` — `init_fonts()` and `deinit_fonts()`

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

- [ ] **Step 6.1 — Add include at top of window.c**

Near the other KiTTY includes at the top of `window.c` (search for `#include "../../kitty.h"`), add:

```c
#include "../../kitty_fontfallback.h"
```

- [ ] **Step 6.2 — Call `kff_init()` at the end of `init_fonts()`**

In `init_fonts()`, find the last two lines (around line 2811–2812):
```c
    fontflag[0] = true;
    fontflag[1] = true;
    fontflag[2] = true;

    init_ucs(conf, &ucsdata);
}
```

Add `kff_init()` call between `fontflag` assignments and `init_ucs`:

```c
    fontflag[0] = true;
    fontflag[1] = true;
    fontflag[2] = true;

    kff_init(fonts[FONT_NORMAL], &lfont, conf_get_int(conf, CONF_font_quality));

    init_ucs(conf, &ucsdata);
}
```

Note: `lfont` is the `static LOGFONT lfont` already filled by `GetObject(fonts[FONT_NORMAL], ...)` earlier in `init_fonts()`.

- [ ] **Step 6.3 — Call `kff_deinit()` at the end of `deinit_fonts()`**

In `deinit_fonts()` (around line 2885), find the closing brace:
```c
    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }
}
```

Add `kff_deinit()` before the closing brace:
```c
    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }

    kff_deinit();
}
```

- [ ] **Step 6.4 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

Expected: Compiles cleanly. `kff_init` and `kff_deinit` are now called at font lifecycle points.

- [ ] **Step 6.5 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add 0.76b_My_PuTTY/windows/window.c
git commit -m "feat(fallback): wire kff_init/kff_deinit into font lifecycle"
```

---

## Task 7: `do_text_internal()` — Fallback Segment Rendering

**Files:**
- Modify: `0.76b_My_PuTTY/windows/window.c`

This is the core rendering change. In `do_text_internal()`, the Unicode `else` branch (around line 6659) fills `wbuf[]` and calls `general_textout()`. We add a fallback detection pass and, if any character needs a different font, split the run into per-font segments.

- [ ] **Step 7.1 — Locate the target block**

In `window.c`, search for the comment `/* And 'normal' unicode characters */` (around line 6656). The block looks like:

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
		exact_textout(hdc, x + xoffset, ...
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

- [ ] **Step 7.2 — Insert fallback rendering after the `for (i = 0; i < len; i++) wbuf[i] = text[i];` line**

Replace from the `for (i = 0 ...)` line through the end of the `else` block with the following. Keep the variable declarations (`wbuf`, `wlen`, `i`) untouched above:

```c
            for (i = 0; i < len; i++)
                wbuf[i] = text[i];

            /* --- Font fallback: check if any character needs a different font --- */
            {
                int draw_y = y - font_height * (lattr == LATTR_BOT) + text_adjust;
                bool needs_fallback = false;
                int fi;
                for (fi = 0; fi < len && !needs_fallback; fi++) {
                    unsigned int cp = (unsigned int)wbuf[fi];
                    if (fi + 1 < len && IS_SURROGATE_PAIR(wbuf[fi], wbuf[fi+1]))
                        cp = 0x10000u + ((wbuf[fi]-0xD800u)<<10) + (wbuf[fi+1]-0xDC00u);
                    if (kff_lookup(cp).hfont)
                        needs_fallback = true;
                }

                if (!needs_fallback) {
                    /* Fast path: all chars use the primary font — existing behaviour */
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
                    if (GetBackgroundImageFlag() && (!PuttyFlag))
                        exact_textout(hdc, x + xoffset, draw_y,
                                      &line_box, wbuf, len, lpDx,
                                      !(attr & TATTR_COMBINING) && !transBg);
                    else
#endif
                    general_textout(wintw_hdc, x + xoffset, draw_y,
                                    &line_box, wbuf, len, lpDx,
                                    opaque && !(attr & TATTR_COMBINING));

                    if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                        SetBkMode(wintw_hdc, TRANSPARENT);
                        ExtTextOutW(wintw_hdc, x + xoffset - 1, draw_y,
                                    ETO_CLIPPED, &line_box, wbuf, len, lpDx_maybe);
                    }
                } else {
                    /* Fallback path: draw run as per-font segments */
                    int seg_x = x + xoffset;
                    int i2 = 0;
                    bool seg_opaque = opaque && !(attr & TATTR_COMBINING);

                    while (i2 < len) {
                        /* Determine codepoint and wchar count for current char */
                        unsigned int cp = (unsigned int)wbuf[i2];
                        int ch_wchars = 1;
                        if (i2 + 1 < len && IS_SURROGATE_PAIR(wbuf[i2], wbuf[i2+1])) {
                            cp = 0x10000u + ((wbuf[i2]-0xD800u)<<10) + (wbuf[i2+1]-0xDC00u);
                            ch_wchars = 2;
                        }
                        KffResult fb = kff_lookup(cp);
                        HFONT seg_font = fb.hfont ? fb.hfont : fonts[nfont];

                        /* Extend segment while consecutive chars map to the same font */
                        int j = i2 + ch_wchars;
                        while (j < len) {
                            unsigned int cp2 = (unsigned int)wbuf[j];
                            int ch2 = 1;
                            if (j + 1 < len && IS_SURROGATE_PAIR(wbuf[j], wbuf[j+1])) {
                                cp2 = 0x10000u + ((wbuf[j]-0xD800u)<<10) + (wbuf[j+1]-0xDC00u);
                                ch2 = 2;
                            }
                            KffResult fb2 = kff_lookup(cp2);
                            HFONT f2 = fb2.hfont ? fb2.hfont : fonts[nfont];
                            if (f2 != seg_font) break;
                            j += ch2;
                        }

                        /* Centering: if fallback glyph narrower than cell, offset inward */
                        int fb_xoff = 0;
                        if (fb.hfont && fb.glyph_px > 0 && lpDx[i2] > 0 &&
                            fb.glyph_px < lpDx[i2])
                            fb_xoff = (lpDx[i2] - fb.glyph_px) / 2;

                        /* Compute segment pixel width from lpDx */
                        int seg_w = 0;
                        int k;
                        for (k = i2; k < j; k++) seg_w += lpDx[k];

                        RECT seg_box = line_box;
                        seg_box.left  = seg_x;
                        seg_box.right = seg_x + seg_w;

                        SelectObject(wintw_hdc, seg_font);
                        ExtTextOutW(wintw_hdc, seg_x + fb_xoff, draw_y,
                                    ETO_CLIPPED | (seg_opaque ? ETO_OPAQUE : 0),
                                    &seg_box, wbuf + i2, j - i2, lpDx + i2);

                        if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                            SetBkMode(wintw_hdc, TRANSPARENT);
                            ExtTextOutW(wintw_hdc, seg_x + fb_xoff - 1, draw_y,
                                        ETO_CLIPPED, &seg_box,
                                        wbuf + i2, j - i2, lpDx + i2);
                        }

                        seg_x += seg_w;
                        seg_opaque = false;
                        SetBkMode(wintw_hdc, TRANSPARENT);
                        i2 = j;
                    }
                    /* Restore primary font for subsequent GDI calls */
                    SelectObject(wintw_hdc, fonts[nfont]);
                }
            }
        }
```

**Note:** The `#if MOD_BACKGROUNDIMAGE` block in the fast path re-uses the existing `exact_textout(hdc, ...)` call pattern from the original code. The fallback segment path only writes to `wintw_hdc` (the non-background path); the background-image path equivalence can be added in a follow-up if needed.

- [ ] **Step 7.3 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

Expected: Compiles cleanly. No new warnings.

- [ ] **Step 7.4 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add 0.76b_My_PuTTY/windows/window.c
git commit -m "feat(fallback): segment rendering in do_text_internal for fallback fonts"
```

---

## Task 8: `terminal.c` — PUA Width via Fallback Font

**Files:**
- Modify: `0.76b_My_PuTTY/terminal/terminal.c`

`term_char_width()` is called when a character arrives from the SSH stream (outside paint context). Currently, if the primary font lacks a PUA glyph, the width query fails and `mk_wcwidth()` returns 1 — causing the icon to be stored as single-width. This task extends PUA handling to query the fallback font.

- [ ] **Step 8.1 — Add include**

Near the top of `0.76b_My_PuTTY/terminal/terminal.c`, after existing includes, add:

```c
#include "../../kitty_fontfallback.h"
```

- [ ] **Step 8.2 — Extend `term_char_width()`**

Find `term_char_width()` (around line 4081). The current code is:

```c
int term_char_width(Terminal *term, unsigned int c)
{
    if (term && term->win &&
        ((c >= 0xE000 && c <= 0xF8FF)    ||
         (c >= 0xF0000 && c <= 0xFFFFD)  ||
         (c >= 0x100000 && c <= 0x10FFFD))) {
        int font_w = win_char_width(term->win, c);
        if (font_w > 0)
            return font_w;
    }
    if (term)
        return term->cjk_ambig_wide ? mk_wcwidth_cjk(c) : mk_wcwidth(c);
    return mk_wcwidth(c);
}
```

Replace with:

```c
int term_char_width(Terminal *term, unsigned int c)
{
    /* For Private Use Area characters, query font metrics directly. */
    if ((c >= 0xE000  && c <= 0xF8FF)   ||
        (c >= 0xF0000 && c <= 0xFFFFD)  ||
        (c >= 0x100000&& c <= 0x10FFFD)) {
        /* 1. Try primary font */
        if (term && term->win) {
            int font_w = win_char_width(term->win, c);
            if (font_w > 0) return font_w;
        }
        /* 2. Try fallback font — provides correct width even when
           the primary font lacks the glyph (e.g. Nerd Font icons). */
        {
            extern int font_width; /* declared in window.c */
            int fw = kff_char_width(c, font_width);
            if (fw > 0) return fw;
        }
    }
    if (term)
        return term->cjk_ambig_wide ? mk_wcwidth_cjk(c) : mk_wcwidth(c);
    return mk_wcwidth(c);
}
```

- [ ] **Step 8.3 — Build**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

Expected: Compiles cleanly.

- [ ] **Step 8.4 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add 0.76b_My_PuTTY/terminal/terminal.c
git commit -m "feat(fallback): extend term_char_width PUA branch to query fallback font"
```

---

## Task 9: Manual Verification

**Files:** None (runtime verification only)

- [ ] **Step 9.1 — Copy the binary to a Windows test machine**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
cp putty.exe /tmp/kitty-fallback-test.exe
```

- [ ] **Step 9.2 — Run the following test strings in KiTTY with Consolas as primary font**

Start KiTTY, set font to **Consolas**, connect to any shell, and run:

```bash
# Test 1: Box Drawing (U+2500–259F) — should show connected lines, not boxes
printf '┌─────┐\n│     │\n└─────┘\n'

# Test 2: Block Elements (U+2580–259F) — should show solid blocks
printf '▀▄█▌▐░▒▓\n'

# Test 3: Geometric shapes & symbols — U+23F5 should show ▶ not □
printf 'U+23F5: ⏵  U+25B6: ▶  U+2665: ♥  U+2660: ♠\n'

# Test 4: Nerd Font PUA — requires Symbols Nerd Font or similar installed
printf 'NF icons:    \n'

# Test 5: Surrogate pair (supplementary PUA)
printf 'Sup PUA: \U000F0001\n'

# Test 6: ASCII should be unchanged
printf 'ASCII: Hello World 1234567890 !@#$\n'

# Test 7: Width correctness — icons should not bleed into adjacent text
printf '||X||Y|\n'
```

- [ ] **Step 9.3 — Expected results**

| Test | Pass condition |
|---|---|
| Box Drawing | Lines connect at corners; no replacement □ glyphs |
| Block Elements | Solid fills; no gaps at cell edges |
| U+23F5 ⏵ | Visible play-button symbol (from Segoe UI Symbol) |
| Nerd Font PUA | Icons display whole; not half-glyph or □ |
| Surrogate pair | Glyph visible if any installed font covers it |
| ASCII | Identical to original Consolas rendering |
| Width test | `|` characters at expected column positions; no bleed |

- [ ] **Step 9.4 — If Box Drawing still shows □ (DWrite fallback not firing)**

Check if Segoe UI Symbol has the glyph:
```bash
# On Windows, in PowerShell:
[System.Drawing.Font]::new('Segoe UI Symbol', 12).GetType().Name
```

Then verify `kff_lookup(0x2500)` would reach the DWrite path. Add a temporary `OutputDebugStringA("kff: dwrite path\n")` in `find_dwrite_fallback()` and check with DebugView.

- [ ] **Step 9.5 — Commit test notes**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add docs/superpowers/specs/2026-05-26-font-fallback-design.md
git commit -m "docs: record verification test strings"
```

---

## Task 10 (Optional): User-Configurable Fallback Font List

**Files:**
- Modify: `kitty_fontfallback.h` — add `kff_set_user_fonts()`
- Modify: `kitty_fontfallback.cpp` — implement user font prepend
- Modify: `0.76b_My_PuTTY/windows/window.c` — read INI + pass to `kff_set_user_fonts()`

This task allows users to add custom fonts at the head of the PUA probe list via `FallbackFont0`…`FallbackFont7` in `kitty.ini`.

- [ ] **Step 10.1 — Add `kff_set_user_fonts()` to header**

In `kitty_fontfallback.h`, inside the `extern "C"` block, add:
```c
/* Call after kff_init() to prepend user-configured fonts (wide strings).
   Resets user list first; pass count=0 to clear. */
void kff_set_user_fonts(const wchar_t **names, int count);
```

- [ ] **Step 10.2 — Implement in `kitty_fontfallback.cpp`**

Add to `KffState`:
```cpp
    wchar_t user_fonts[8][LF_FACESIZE];
    int     user_font_count;
```

Add implementation:
```cpp
extern "C" void kff_set_user_fonts(const wchar_t **names, int count)
{
    g_kff.user_font_count = 0;
    if (count > 8) count = 8;
    for (int i = 0; i < count; i++) {
        wcsncpy(g_kff.user_fonts[i], names[i], LF_FACESIZE - 1);
        g_kff.user_fonts[i][LF_FACESIZE - 1] = L'\0';
        g_kff.user_font_count++;
    }
    cache_clear(); /* invalidate cache since font priority changed */
}
```

Update `find_pua_font()` to try user fonts first:
```cpp
static HFONT find_pua_font(unsigned int cp)
{
    /* User-configured fonts take priority */
    for (int i = 0; i < g_kff.user_font_count; i++) {
        HFONT hf = make_fallback_hfont(g_kff.user_fonts[i]);
        if (hf && font_has_glyph(hf, cp)) return hf;
    }
    /* Then builtin list */
    for (int i = 0; kff_builtin_fonts[i]; i++) {
        HFONT hf = make_fallback_hfont(kff_builtin_fonts[i]);
        if (hf && font_has_glyph(hf, cp)) return hf;
    }
    return NULL;
}
```

- [ ] **Step 10.3 — Read from KiTTY INI in `window.c`**

In `init_fonts()`, after the `kff_init(...)` call added in Task 6, add:

```c
    /* Read user fallback font list from INI */
    {
        static wchar_t user_fb[8][LF_FACESIZE];
        const wchar_t *ptrs[8];
        int n = 0;
        char key[32], val[LF_FACESIZE];
        int i;
        for (i = 0; i < 8; i++) {
            snprintf(key, sizeof(key), "FallbackFont%d", i);
            val[0] = '\0';
            /* conf_get_str returns empty string if key absent */
            strncpy(val,
                    conf_get_str_ambi(conf, key, ""),
                    LF_FACESIZE - 1);
            val[LF_FACESIZE - 1] = '\0';
            if (val[0] == '\0') break;
            MultiByteToWideChar(CP_ACP, 0, val, -1, user_fb[n], LF_FACESIZE);
            ptrs[n] = user_fb[n];
            n++;
        }
        if (n > 0)
            kff_set_user_fonts(ptrs, n);
    }
```

**Note:** `conf_get_str_ambi` is a placeholder — the real mechanism depends on how KiTTY reads INI keys. If using the `mini` INI parser directly (as other KiTTY settings do), replace with the appropriate `GetPrivateProfileString` or `mini`-based call matching the pattern used elsewhere in `kitty_settings.c`.

- [ ] **Step 10.4 — Build and verify**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree/0.76b_My_PuTTY/windows
make -e TOOLPATH=/usr/bin/x86_64-w64-mingw32- -f MAKEFILE.MINGW putty.exe 2>&1 | tail -10
```

- [ ] **Step 10.5 — Commit**

```bash
cd /home/admin/git/KiTTY.font.fallback.worktree
git add kitty_fontfallback.h kitty_fontfallback.cpp 0.76b_My_PuTTY/windows/window.c
git commit -m "feat(fallback): user-configurable FallbackFont0..7 INI keys"
```

---

## Self-Review Checklist

- [x] **Spec §1 (current state):** Documented in plan preamble and spec doc — no task needed.
- [x] **Spec §2.1 (DWrite+GDI):** Tasks 2–5 implement this fully.
- [x] **Spec §2.2 (MVP scope PUA + Box Drawing):** Task 3 covers PUA; Task 4 covers Box Drawing via DWrite.
- [x] **Spec §3.1 (module architecture):** Tasks 1–5 build `kitty_fontfallback.cpp/h`.
- [x] **Spec §3.2 (data structures):** `KffState`, `KffCacheEntry`, `KffFontNode`, `KffResult` all in Task 2.
- [x] **Spec §4 (kff_lookup chain):** Tasks 3 and 4.
- [x] **Spec §5 (cell width strategy):** Task 8 (`term_char_width`); Task 7 centering via `fb_xoff`.
- [x] **Spec §6 (config):** Task 10 (optional).
- [x] **Spec §7 (file list):** All files covered by tasks.
- [x] **Spec §8 (risks):** DWrite init failure: `kff_init()` sets `initialized=false` and all APIs degrade gracefully. HFONT lifetime: `kff_deinit()` called at end of `deinit_fonts()`. Bold-shadow: handled per-segment in Task 7.
- [x] **Spec §9 (acceptance criteria):** Verified in Task 9.
- [x] **Type consistency:** `KffResult` defined in Task 1, used identically in Tasks 3, 4, 7, 8. `kff_char_width` signature matches between header (Task 1) and impl (Task 5) and call site (Task 8).
- [x] **No placeholders:** All code blocks are complete. Task 10 Step 10.3 notes the INI mechanism ambiguity explicitly rather than hiding it.
