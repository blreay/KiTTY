/*
 * fontfallback.c - Font fallback mechanism for KiTTY
 *
 * When the primary terminal font lacks a glyph for a character,
 * this module selects an alternative font from a fallback list
 * and provides the appropriate HFONT for rendering.
 *
 * Uses GDI GetGlyphIndicesW to check glyph existence and caches
 * codepoint->font mappings for performance.
 */

#include <windows.h>
#include <string.h>

#define PUTTY_DO_GLOBALS
#include "putty.h"
#include "terminal.h"

#include "fontfallback.h"

static FFB_State ffb_state;

/* Simple hash for codepoints */
static unsigned int ffb_hash(unsigned int cp)
{
    return cp % FFB_CACHE_SIZE;
}

/*
 * Check whether a glyph exists in a given font using GetGlyphIndicesW.
 */
bool ffb_glyph_exists(HDC hdc, HFONT hfont, wchar_t wc)
{
    HFONT old_font;
    WORD glyph_index;
    DWORD ret;

    if (!hfont || !hdc)
        return false;

    old_font = SelectObject(hdc, hfont);
    ret = GetGlyphIndicesW(hdc, &wc, 1, &glyph_index, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(hdc, old_font);

    if (ret == GDI_ERROR)
        return false;

    /* 0xFFFF is the default glyph index for missing glyphs */
    return glyph_index != 0xFFFF;
}

/*
 * Create a fallback font HFONT matching the primary font's size/weight.
 */
static HFONT ffb_create_font(const wchar_t *name, int height, int avg_width,
                              int weight, int charset, int quality)
{
    return CreateFontW(height, avg_width, 0, 0, weight, FALSE, FALSE, FALSE,
                       charset, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality),
                       DEFAULT_PITCH | FF_DONTCARE, name);
}

/*
 * Initialize the font fallback system. Must be called after the
 * primary font has been created (after init_fonts).
 */
void ffb_init(HDC hdc, HFONT primary_hfont, int cell_width)
{
    TEXTMETRIC tm;
    HFONT old_font;

    ffb_cleanup();

    memset(&ffb_state, 0, sizeof(ffb_state));

    old_font = SelectObject(hdc, primary_hfont);
    GetTextMetrics(hdc, &tm);
    SelectObject(hdc, old_font);

    ffb_state.primary_font = primary_hfont;
    ffb_state.primary_tm = tm;
    ffb_state.cell_width = cell_width;
    ffb_state.initialized = true;

    ffb_setup_defaults();
}

/*
 * Clean up all fallback resources.
 */
void ffb_cleanup(void)
{
    FFB_FontEntry *entry, *next;
    int i;

    for (entry = ffb_state.fallbacks; entry; entry = next) {
        next = entry->next;
        if (entry->hfont)
            DeleteObject(entry->hfont);
        sfree(entry->name);
        sfree(entry);
    }
    ffb_state.fallbacks = NULL;
    ffb_state.fallback_count = 0;

    for (i = 0; i < FFB_CACHE_SIZE; i++) {
        FFB_CacheEntry *ce, *cenext;
        for (ce = ffb_state.cache[i]; ce; ce = cenext) {
            cenext = ce->next;
            sfree(ce);
        }
        ffb_state.cache[i] = NULL;
    }

    ffb_state.primary_font = NULL;
    ffb_state.initialized = false;
}

/*
 * Invalidate the cache. Call when the primary font changes.
 */
void ffb_invalidate_cache(void)
{
    int i;
    for (i = 0; i < FFB_CACHE_SIZE; i++) {
        FFB_CacheEntry *ce, *cenext;
        for (ce = ffb_state.cache[i]; ce; ce = cenext) {
            cenext = ce->next;
            sfree(ce);
        }
        ffb_state.cache[i] = NULL;
    }
}

/*
 * Add a fallback font to the list.
 */
void ffb_add_fallback(const wchar_t *name)
{
    FFB_FontEntry *entry, **tail;
    int len;

    if (!name || !name[0])
        return;

    len = wcslen(name) + 1;
    entry = snew(FFB_FontEntry);
    memset(entry, 0, sizeof(*entry));
    entry->name = snewn(len, wchar_t);
    memcpy(entry->name, name, len * sizeof(wchar_t));
    entry->hfont = NULL;
    entry->next = NULL;

    /* Append to end of list */
    tail = &ffb_state.fallbacks;
    while (*tail)
        tail = &(*tail)->next;
    *tail = entry;

    ffb_state.fallback_count++;
}

/*
 * Set up the default fallback font list.
 * Ordered by priority: first added = highest priority.
 */
void ffb_setup_defaults(void)
{
    /*
     * Box Drawing & Block Elements: Segoe UI Symbol covers U+2500-U+259F
     * and many other symbols.
     */
    ffb_add_fallback(L"Segoe UI Symbol");

    /*
     * CJK fallback: covers Chinese, Japanese, Korean characters.
     * Microsoft YaHei is widely available on Windows 10+.
     */
    ffb_add_fallback(L"Microsoft YaHei");

    /*
     * Japanese fallback.
     */
    ffb_add_fallback(L"MS Gothic");

    /*
     * Korean fallback.
     */
    ffb_add_fallback(L"Malgun Gothic");

    /*
     * Emoji and symbols: covers emoji, PUA characters used by
     * symbol fonts, and various other symbol ranges.
     */
    ffb_add_fallback(L"Segoe UI Emoji");
}

/*
 * Lazy-create a fallback font's HFONT, matching the primary font metrics.
 */
static FFB_FontEntry *ffb_ensure_hfont(HDC hdc, FFB_FontEntry *entry)
{
    if (entry->hfont)
        return entry;

    entry->hfont = ffb_create_font(
        entry->name,
        ffb_state.primary_tm.tmHeight,
        ffb_state.primary_tm.tmAveCharWidth,
        ffb_state.primary_tm.tmWeight,
        ffb_state.primary_tm.tmCharSet,
        ffb_state.primary_tm.tmQuality
    );

    if (entry->hfont) {
        TEXTMETRIC tm;
        HFONT old_font = SelectObject(hdc, entry->hfont);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, old_font);
        entry->ascent = tm.tmAscent;
        entry->height = tm.tmHeight;
        entry->avg_width = tm.tmAveCharWidth;
    }

    return entry;
}

/*
 * Look up the cache for a codepoint.
 * Returns: cached font_index, or a sentinel value.
 *   0  = primary font has the glyph
 *  -1  = no font has the glyph
 *  -2  = not cached
 */
static int ffb_cache_lookup(unsigned int cp)
{
    unsigned int h = ffb_hash(cp);
    FFB_CacheEntry *ce;

    for (ce = ffb_state.cache[h]; ce; ce = ce->next) {
        if (ce->codepoint == cp)
            return ce->font_index;
    }
    return -2; /* not cached */
}

/*
 * Store a codepoint->font_index mapping in the cache.
 */
static void ffb_cache_store(unsigned int cp, int font_index)
{
    unsigned int h = ffb_hash(cp);
    FFB_CacheEntry *ce;

    /* Check if already cached (shouldn't happen, but be safe) */
    for (ce = ffb_state.cache[h]; ce; ce = ce->next) {
        if (ce->codepoint == cp) {
            ce->font_index = font_index;
            return;
        }
    }

    ce = snew(FFB_CacheEntry);
    ce->codepoint = cp;
    ce->font_index = font_index;
    ce->next = ffb_state.cache[h];
    ffb_state.cache[h] = ce;
}

/*
 * Find the best font for a given character.
 * Returns: true if a font was found, false if no font has this glyph.
 */
bool ffb_find_font(HDC hdc, wchar_t wc, HFONT *out_hfont,
                   int *out_ascent, int *out_width_cells)
{
    int cached;
    FFB_FontEntry *entry;
    int idx;

    if (!ffb_state.initialized)
        return false;

    /* Check cache first */
    cached = ffb_cache_lookup((unsigned int)wc);
    if (cached == 0) {
        /* Primary font has it */
        if (out_hfont) *out_hfont = ffb_state.primary_font;
        if (out_ascent) *out_ascent = ffb_state.primary_tm.tmAscent;
        if (out_width_cells) {
            int ibuf = 0;
            int pw = ffb_state.cell_width > 0 ? ffb_state.cell_width : ffb_state.primary_tm.tmAveCharWidth;
            HFONT old_font = SelectObject(hdc, ffb_state.primary_font);
            if (GetCharWidth32W(hdc, wc, wc, &ibuf)) {
                ibuf += pw / 2 - 1;
                ibuf /= pw;
                if (ibuf < 1) ibuf = 1;
                *out_width_cells = ibuf;
            } else {
                *out_width_cells = 1;
            }
            SelectObject(hdc, old_font);
        }
        return true;
    }
    if (cached == -1) {
        /* No font has it */
        return false;
    }

    /* Check primary font */
    if (ffb_glyph_exists(hdc, ffb_state.primary_font, wc)) {
        ffb_cache_store((unsigned int)wc, 0);
        if (out_hfont) *out_hfont = ffb_state.primary_font;
        if (out_ascent) *out_ascent = ffb_state.primary_tm.tmAscent;
        if (out_width_cells) {
            int ibuf = 0;
            int pw = ffb_state.cell_width > 0 ? ffb_state.cell_width : ffb_state.primary_tm.tmAveCharWidth;
            HFONT old_font = SelectObject(hdc, ffb_state.primary_font);
            if (GetCharWidth32W(hdc, wc, wc, &ibuf)) {
                ibuf += pw / 2 - 1;
                ibuf /= pw;
                if (ibuf < 1) ibuf = 1;
                *out_width_cells = ibuf;
            } else {
                *out_width_cells = 1;
            }
            SelectObject(hdc, old_font);
        }
        return true;
    }

    /* Walk fallback list */
    for (entry = ffb_state.fallbacks, idx = 1; entry;
         entry = entry->next, idx++) {
        ffb_ensure_hfont(hdc, entry);
        if (!entry->hfont)
            continue;

        if (ffb_glyph_exists(hdc, entry->hfont, wc)) {
            ffb_cache_store((unsigned int)wc, idx);
            if (out_hfont) *out_hfont = entry->hfont;
            if (out_ascent) *out_ascent = entry->ascent;
            if (out_width_cells) {
                int ibuf = 0;
                int pw = ffb_state.cell_width > 0 ? ffb_state.cell_width : ffb_state.primary_tm.tmAveCharWidth;
                HFONT old_font = SelectObject(hdc, entry->hfont);
                if (GetCharWidth32W(hdc, wc, wc, &ibuf)) {
                    ibuf += pw / 2 - 1;
                    ibuf /= pw;
                    if (ibuf < 1) ibuf = 1;
                    *out_width_cells = ibuf;
                } else {
                    *out_width_cells = 1;
                }
                SelectObject(hdc, old_font);
            }
            return true;
        }
    }

    /* No font has this glyph */
    ffb_cache_store((unsigned int)wc, -1);
    return false;
}

/*
 * Get the width of a character in cells using the fallback system.
 */
int ffb_char_width(HDC hdc, wchar_t wc)
{
    HFONT hfont;
    int width_cells;

    if (!ffb_state.initialized)
        return 0;

    if (ffb_find_font(hdc, wc, &hfont, NULL, &width_cells))
        return width_cells;

    return 0;
}

/*
 * Check if the font fallback system is initialized.
 */
bool ffb_is_initialized(void)
{
    return ffb_state.initialized;
}

/*
 * Get the primary font handle (for comparison in rendering code).
 */
HFONT ffb_get_primary_font(void)
{
    return ffb_state.primary_font;
}
