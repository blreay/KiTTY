/*
 * fontfallback.h - Font fallback mechanism for KiTTY
 *
 * When the primary terminal font lacks a glyph for a character,
 * this module selects an alternative font from a fallback list
 * and provides the appropriate HFONT for rendering.
 *
 * Design goals:
 *   - Minimal invasion into existing rendering pipeline
 *   - Cache codepoint->font mappings for performance
 *   - Respect KiTTY's cell-based grid model
 *   - Support Box Drawing, Block Elements, CJK, PUA/Nerd Fonts, etc.
 */

#ifndef FONTFALLBACK_H
#define FONTFALLBACK_H

#include <windows.h>

/*
 * Fallback font entry. Forms a singly-linked list ordered by priority.
 * The primary font is NOT in this list; it is always tried first.
 */
typedef struct FFB_FontEntry {
    wchar_t *name;              /* Font face name (e.g. L"Segoe UI Symbol") */
    HFONT hfont;                /* Created font handle, or NULL if not yet created */
    int ascent;                 /* Font ascent (pixels), for baseline alignment */
    int height;                 /* Font height (pixels) */
    int avg_width;              /* Average character width (pixels) */
    struct FFB_FontEntry *next; /* Next fallback font in priority order */
} FFB_FontEntry;

/*
 * Cache entry: maps a Unicode codepoint to the font index that
 * contains a glyph for it. -1 means "no font has this glyph"
 * (i.e., draw a replacement character box).
 */
typedef struct FFB_CacheEntry {
    unsigned int codepoint;     /* Unicode codepoint */
    int font_index;             /* Index into fallback list, -1 = not found, 0 = primary */
    struct FFB_CacheEntry *next; /* Hash chain */
} FFB_CacheEntry;

#define FFB_CACHE_SIZE 1024

/*
 * Global fallback state.
 */
typedef struct FFB_State {
    FFB_FontEntry *fallbacks;           /* Head of fallback font list */
    FFB_CacheEntry *cache[FFB_CACHE_SIZE]; /* Hash table */
    int fallback_count;                 /* Number of fallback fonts */
    HFONT primary_font;                 /* Cached primary font handle */
    TEXTMETRIC primary_tm;              /* Primary font metrics (for baseline alignment) */
    int cell_width;                     /* Primary font cell width (pixels) */
    bool initialized;                   /* Whether ffb_init has been called */
} FFB_State;

/*
 * Initialize the font fallback system. Must be called after the
 * primary font has been created (after init_fonts).
 *   hdc  - device context with primary font selected
 *   primary_hfont - the primary terminal font (FONT_NORMAL)
 */
void ffb_init(HDC hdc, HFONT primary_hfont, int cell_width);

/*
 * Clean up all fallback resources. Call before program exit
 * or when the primary font changes.
 */
void ffb_cleanup(void);

/*
 * Invalidate the cache. Call when the primary font changes
 * (e.g., user changes font in configuration).
 */
void ffb_invalidate_cache(void);

/*
 * Check whether a glyph exists in a given font.
 *   hdc  - device context (any font can be selected)
 *   hfont - font to check
 *   wc   - character to check
 * Returns: true if glyph exists, false otherwise.
 */
bool ffb_glyph_exists(HDC hdc, HFONT hfont, wchar_t wc);

/*
 * Find the best font for a given character.
 *   hdc - device context (primary font should be selected)
 *   wc  - character to find a font for
 *   out_hfont  - receives the HFONT for the character
 *   out_ascent - receives the ascent of the found font (for baseline alignment)
 *   out_width_cells - receives the width in cells (1 or 2)
 * Returns: true if a font was found, false if no font has this glyph.
 *
 * The function first checks the primary font, then walks the
 * fallback list. Results are cached for subsequent lookups.
 */
bool ffb_find_font(HDC hdc, wchar_t wc, HFONT *out_hfont,
                   int *out_ascent, int *out_width_cells);

/*
 * Get the width of a character in cells using the fallback system.
 * This extends wintw_char_width to also check fallback fonts
 * when the primary font lacks the glyph.
 *   hdc - device context
 *   wc  - character to measure
 * Returns: width in cells (1 or 2), or 0 if no font has the glyph.
 */
int ffb_char_width(HDC hdc, wchar_t wc);

/*
 * Add a fallback font to the list.
 *   name - font face name (e.g., L"Segoe UI Symbol")
 * Priority is determined by insertion order: first added = highest priority.
 */
void ffb_add_fallback(const wchar_t *name);

/*
 * Set up the default fallback font list.
 * Called once during initialization.
 */
void ffb_setup_defaults(void);

/*
 * Check if the font fallback system is initialized.
 */
bool ffb_is_initialized(void);

/*
 * Get the primary font handle (for comparison in rendering code).
 */
HFONT ffb_get_primary_font(void);

#endif /* FONTFALLBACK_H */
