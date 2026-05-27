#define COBJMACROS
#include "kitty_fontfallback.h"
#include <dwrite.h>
#include <dwrite_2.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

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
    wchar_t user_fonts[8][LF_FACESIZE];
    int     user_font_count;
    int      primary_cell_px; /* measured cell width of primary font, px */

    KffCacheEntry cache[KFF_CACHE_BUCKETS];
    KffFontNode  *font_pool;

    bool initialized;
} KffState;

static KffState g_kff;

/* ------------------------------------------------------------------ */
/* Debug logging                                                        */
/* ------------------------------------------------------------------ */
static FILE *g_kff_log      = NULL;
static int   g_kff_log_miss = 0;    /* count of lookup-level log lines */
#define KFF_LOG_MISS_MAX 300        /* stop detailed logging after this */

static void kff_log_open(void)
{
    g_kff_log_miss = 0;

    /* 1. Try EXE directory first (works even with non-ASCII usernames) */
    {
        wchar_t wpath[MAX_PATH];
        if (GetModuleFileNameW(NULL, wpath, MAX_PATH)) {
            wchar_t *slash = wcsrchr(wpath, L'\\');
            if (slash) {
                wcscpy(slash + 1, L"kitty_fontfallback.log");
                g_kff_log = _wfopen(wpath, L"w");
            }
        }
    }

    /* 2. Fall back to %TEMP% via wide API */
    if (!g_kff_log) {
        wchar_t wpath[MAX_PATH];
        if (GetTempPathW(MAX_PATH, wpath)) {
            wcsncat(wpath, L"kitty_fontfallback.log", MAX_PATH - wcslen(wpath) - 1);
            g_kff_log = _wfopen(wpath, L"w");
        }
    }
}

static void kff_log_close(void)
{
    if (g_kff_log) { fclose(g_kff_log); g_kff_log = NULL; }
}

static void kff_log(const char *fmt, ...)
{
    if (!g_kff_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = '\0';
    va_end(ap);
    fprintf(g_kff_log, "%s\n", buf);
    fflush(g_kff_log);
    OutputDebugStringA(buf);
    OutputDebugStringA("\r\n");
}

/* Log a wide string safely (converts to ACP narrow) */
static void kff_logw(const char *prefix, const wchar_t *wstr)
{
    if (!g_kff_log) return;
    char narrow[256];
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, narrow, sizeof(narrow) - 1, NULL, NULL);
    narrow[sizeof(narrow) - 1] = '\0';
    kff_log("%s%s", prefix, narrow);
}


static const wchar_t *kff_builtin_fonts[] = {
    L"Sarasa Fixed SC Nerd Font",
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
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        /* IUnknown {00000000-0000-0000-C000-000000000046} */
        static const GUID iid_unk =
            {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
        /* IDWriteTextAnalysisSource {688E1A58-5094-47C8-ADC8-FBCEA60AE92B} */
        static const GUID iid_tas =
            {0x688e1a58,0x5094,0x47c8,{0xad,0xc8,0xfb,0xce,0xa6,0x0a,0xe9,0x2b}};
        if (IsEqualGUID(riid, iid_unk) || IsEqualGUID(riid, iid_tas)) {
            *ppv = static_cast<IDWriteTextAnalysisSource *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
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
    /* User-configured fonts take priority */
    for (int i = 0; i < g_kff.user_font_count; i++) {
        HFONT hf = make_fallback_hfont(g_kff.user_fonts[i]);
        bool has = hf && font_has_glyph(hf, cp);
        if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
            kff_logw("[find_pua_font]   user ", g_kff.user_fonts[i]);
        if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
            kff_log("                     hfont=%p has_glyph=%d", (void*)hf, (int)has);
        if (has) return hf;
    }
    /* Then builtin list */
    for (int i = 0; kff_builtin_fonts[i]; i++) {
        HFONT hf = make_fallback_hfont(kff_builtin_fonts[i]);
        bool has = hf && font_has_glyph(hf, cp);
        if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
            kff_logw("[find_pua_font]   builtin ", kff_builtin_fonts[i]);
        if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
            kff_log("                     hfont=%p has_glyph=%d", (void*)hf, (int)has);
        if (has) return hf;
    }
    return NULL;
}

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

    /* Clamp weight: GDI FW_DONTCARE=0 is invalid for DWrite (valid range 1-999) */
    LONG raw_weight = g_kff.primary_lfw.lfWeight;
    DWRITE_FONT_WEIGHT dw_weight = (raw_weight >= 100 && raw_weight <= 999)
        ? (DWRITE_FONT_WEIGHT)raw_weight
        : DWRITE_FONT_WEIGHT_NORMAL;

    kff_log("[find_dwrite_fallback] cp=0x%04X calling MapCharacters lfWeight=%ld dw_weight=%d",
            cp, raw_weight, (int)dw_weight);

    HRESULT hr = g_kff.dw_fallback->MapCharacters(
        &source,
        0, wlen,
        NULL,                                /* use system font collection */
        g_kff.primary_lfw.lfFaceName,
        dw_weight,
        g_kff.primary_lfw.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        &mapped_len,
        &mapped_font,
        &scale);

    if (FAILED(hr) || !mapped_font) {
        kff_log("[find_dwrite_fallback] cp=0x%04X MapCharacters FAILED hr=0x%08X", cp, (unsigned)hr);
        return NULL;
    }
    if (mapped_len < wlen) {
        kff_log("[find_dwrite_fallback] cp=0x%04X partial mapping mapped_len=%u wlen=%u", cp, mapped_len, wlen);
        mapped_font->Release();
        return NULL;
    }

    /* Convert IDWriteFont → LOGFONTW → HFONT */
    LOGFONTW lfw;
    BOOL     is_sys;
    hr = g_kff.dw_gdi_interop->ConvertFontToLOGFONT(mapped_font, &lfw, &is_sys);
    mapped_font->Release();
    if (FAILED(hr)) {
        kff_log("[find_dwrite_fallback] cp=0x%04X ConvertFontToLOGFONT FAILED hr=0x%08X", cp, (unsigned)hr);
        return NULL;
    }

    /* Override metrics to match primary font height */
    lfw.lfHeight  = g_kff.primary_lfw.lfHeight;
    lfw.lfWidth   = 0;
    lfw.lfQuality = g_kff.primary_lfw.lfQuality;

    HFONT hf = pool_get_or_create(&lfw);
    if (hf && out_glyph_px)
        *out_glyph_px = measure_glyph_px(hf, cp);
    if (g_kff_log_miss <= KFF_LOG_MISS_MAX) {
        char narrow[256];
        WideCharToMultiByte(CP_ACP, 0, lfw.lfFaceName, -1, narrow, sizeof(narrow)-1, NULL, NULL);
        narrow[sizeof(narrow)-1] = '\0';
        kff_log("[find_dwrite_fallback] cp=0x%04X -> font=\"%s\" glyph_px=%d",
                cp, narrow, out_glyph_px ? *out_glyph_px : 0);
    }
    return hf;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

extern "C" {

void kff_init(HFONT primary_hfont, const LOGFONT *primary_lf, int quality)
{
    kff_deinit();
    kff_log_open();
    kff_log("[kff_init] called: lfHeight=%d quality=%d", primary_lf->lfHeight, quality);
    kff_log("[kff_init] primary font: \"%s\"", primary_lf->lfFaceName);
    kff_log("[kff_init] primary_hfont=%p", (void*)primary_hfont);

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

    /* Measure primary font's cell width for later use by kff_char_width */
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            HGDIOBJ old = SelectObject(hdc, primary_hfont);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, old);
            ReleaseDC(NULL, hdc);
            g_kff.primary_cell_px = tm.tmAveCharWidth;
        }
    }
    kff_log("[kff_init] primary_cell_px=%d", g_kff.primary_cell_px);

    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory2),
        (IUnknown **)&g_kff.dw_factory);
    if (FAILED(hr) || !g_kff.dw_factory) {
        kff_log("[kff_init] DWriteCreateFactory: hr=0x%08X FAILED", (unsigned)hr);
        return;
    }
    kff_log("[kff_init] DWriteCreateFactory: hr=0x%08X OK", (unsigned)hr);

    hr = g_kff.dw_factory->GetSystemFontFallback(&g_kff.dw_fallback);
    kff_log("[kff_init] GetSystemFontFallback: hr=0x%08X %s",
            (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        g_kff.dw_factory->Release(); g_kff.dw_factory = NULL; return;
    }

    hr = g_kff.dw_factory->GetGdiInterop(&g_kff.dw_gdi_interop);
    kff_log("[kff_init] GetGdiInterop: hr=0x%08X %s",
            (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        g_kff.dw_fallback->Release(); g_kff.dw_fallback = NULL;
        g_kff.dw_factory->Release();  g_kff.dw_factory  = NULL;
        return;
    }

    g_kff.initialized = true;
    kff_log("[kff_init] initialized = true");
}

void kff_deinit(void)
{
    kff_log("[kff_deinit] called");
    kff_log_close();
    pool_free_all();
    cache_clear();

    if (g_kff.dw_gdi_interop) { g_kff.dw_gdi_interop->Release(); g_kff.dw_gdi_interop = NULL; }
    if (g_kff.dw_fallback)    { g_kff.dw_fallback->Release();    g_kff.dw_fallback    = NULL; }
    if (g_kff.dw_factory)     { g_kff.dw_factory->Release();     g_kff.dw_factory     = NULL; }

    g_kff.initialized   = false;
    g_kff.primary_hfont = NULL;
    g_kff.primary_cell_px = 0;
    g_kff.font_pool     = NULL;
    g_kff.user_font_count = 0;
}

void kff_set_user_fonts(const wchar_t **names, int count)
{
    kff_log("[kff_set_user_fonts] called with count=%d", count);
    g_kff.user_font_count = 0;
    if (count > 8) count = 8;
    for (int i = 0; i < count; i++) {
        wcsncpy(g_kff.user_fonts[i], names[i], LF_FACESIZE - 1);
        g_kff.user_fonts[i][LF_FACESIZE - 1] = L'\0';
        g_kff.user_font_count++;
        kff_logw("[kff_set_user_fonts]   added: ", g_kff.user_fonts[i]);
    }
    cache_clear();
}

KffResult kff_lookup(unsigned int codepoint)
{
    KffResult miss = {NULL, 0};
    if (!g_kff.initialized) return miss;
    if (codepoint == 0)     return miss;

    /* 1. Cache hit */
    KffResult cached;
    if (cache_get(codepoint, &cached)) return cached;

    /* Cache miss — count and log (gated) */
    if (g_kff_log_miss < KFF_LOG_MISS_MAX) {
        g_kff_log_miss++;
        kff_log("[kff_lookup] cp=0x%04X (cache miss #%d)", codepoint, g_kff_log_miss);
    }

    /* 2. Primary font already has this glyph → no fallback needed */
    if (g_kff.primary_hfont && font_has_glyph(g_kff.primary_hfont, codepoint)) {
        if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
            kff_log("[kff_lookup] cp=0x%04X primary font HAS glyph -> no fallback", codepoint);
        cache_set(codepoint, miss);
        return miss;
    }
    if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
        kff_log("[kff_lookup] cp=0x%04X primary font missing glyph -> %s",
                codepoint, is_pua(codepoint) ? "PUA path" : "DWrite path");

    KffResult result = miss;

    if (is_pua(codepoint)) {
        /* 3a. PUA: try builtin Nerd Font list */
        HFONT hf = find_pua_font(codepoint);
        if (hf) {
            result.hfont    = hf;
            result.glyph_px = measure_glyph_px(hf, codepoint);
        }
    } else {
        /* 3b. Non-PUA: DWrite system fallback first, then font list if DWrite fails */
        int px = 0;
        HFONT hf = find_dwrite_fallback(codepoint, &px);
        if (!hf) {
            /* DWrite unavailable or failed — try curated font list */
            hf = find_pua_font(codepoint);
            if (hf)
                px = measure_glyph_px(hf, codepoint);
        }
        if (hf) {
            result.hfont    = hf;
            result.glyph_px = px;
        }
    }

    if (g_kff_log_miss <= KFF_LOG_MISS_MAX)
        kff_log("[kff_lookup] cp=0x%04X result: hfont=%p glyph_px=%d",
                codepoint, (void*)result.hfont, result.glyph_px);

    cache_set(codepoint, result);
    return result;
}

int kff_char_width(unsigned int codepoint, int font_width)
{
    if (!g_kff.initialized) return 0;
    if (font_width <= 0)
        font_width = g_kff.primary_cell_px;
    if (font_width <= 0) return 0;

    KffResult r = kff_lookup(codepoint);
    if (!r.hfont || r.glyph_px <= 0) return 0;

    /* Round glyph_px to nearest cell count (1 or 2) */
    int cells = (r.glyph_px + font_width / 2) / font_width;
    if (cells < 1) cells = 1;
    if (cells > 2) cells = 2;
    return cells;
}

} /* extern "C" */
