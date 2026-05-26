#define COBJMACROS
#include "kitty_fontfallback.h"
#include <dwrite.h>
#include <dwrite_2.h>
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
    LOGFONTW primary_lfw;
    int      quality;

    KffCacheEntry cache[KFF_CACHE_BUCKETS];
    KffFontNode  *font_pool;

    bool initialized;
} KffState;

static KffState g_kff;

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
/* IDWriteTextAnalysisSource                                            */
/* ------------------------------------------------------------------ */

class KffAnalysisSource : public IDWriteTextAnalysisSource {
    const wchar_t *m_text;
    UINT32         m_len;
public:
    KffAnalysisSource(const wchar_t *text, UINT32 len)
        : m_text(text), m_len(len) {}

    ULONG   STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override {
        *ppv = nullptr; return E_NOINTERFACE;
    }
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
/* Cache                                                                */
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
        if (g_kff.cache[s].codepoint == 0)  return false;
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
    g_kff.cache[slot].codepoint = cp;
    g_kff.cache[slot].result    = r;
}

/* ------------------------------------------------------------------ */
/* HFONT pool                                                           */
/* ------------------------------------------------------------------ */

static bool logfontw_eq(const LOGFONTW *a, const LOGFONTW *b)
{
    return a->lfHeight  == b->lfHeight  &&
           a->lfWeight  == b->lfWeight  &&
           a->lfItalic  == b->lfItalic  &&
           a->lfQuality == b->lfQuality &&
           a->lfCharSet == b->lfCharSet &&
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
/* GDI glyph helpers (used by kff_lookup in later tasks)               */
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
    return idx[0] != 0xFFFF;
}

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
/* Lookup helpers                                                        */
/* ------------------------------------------------------------------ */

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

static HFONT find_pua_font(unsigned int cp)
{
    for (int i = 0; kff_builtin_fonts[i]; i++) {
        HFONT hf = make_fallback_hfont(kff_builtin_fonts[i]);
        if (hf && font_has_glyph(hf, cp))
            return hf;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

extern "C" {

void kff_init(HFONT primary_hfont, const LOGFONT *primary_lf, int quality)
{
    kff_deinit();

    g_kff.primary_hfont = primary_hfont;
    g_kff.quality       = quality;

    memset(&g_kff.primary_lfw, 0, sizeof(g_kff.primary_lfw));
    g_kff.primary_lfw.lfHeight  = primary_lf->lfHeight;
    g_kff.primary_lfw.lfWidth   = 0;
    g_kff.primary_lfw.lfWeight  = primary_lf->lfWeight;
    g_kff.primary_lfw.lfItalic  = primary_lf->lfItalic;
    g_kff.primary_lfw.lfQuality = (BYTE)quality;
    g_kff.primary_lfw.lfCharSet = DEFAULT_CHARSET;
    g_kff.primary_lfw.lfPitchAndFamily = FF_DONTCARE;
    MultiByteToWideChar(CP_ACP, 0,
        primary_lf->lfFaceName, -1,
        g_kff.primary_lfw.lfFaceName, LF_FACESIZE);

    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory2),
        (IUnknown **)&g_kff.dw_factory);
    if (FAILED(hr) || !g_kff.dw_factory) return;

    hr = g_kff.dw_factory->GetSystemFontFallback(&g_kff.dw_fallback);
    if (FAILED(hr)) {
        g_kff.dw_factory->Release(); g_kff.dw_factory = NULL; return;
    }

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

    g_kff.initialized   = false;
    g_kff.primary_hfont = NULL;
    g_kff.font_pool     = NULL;
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
        /* 3b. Non-PUA (Box Drawing etc.): DWrite fallback — implemented in Task 4 */
        (void)0;
    }

    cache_set(codepoint, result);
    return result;
}

int kff_char_width(unsigned int codepoint, int font_width)
{
    (void)codepoint; (void)font_width;
    return 0;
}

} /* extern "C" */
