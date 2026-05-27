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
#include <stdbool.h>

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
 * Return the HFONT for a given fallback slot + attribute combination.
 * Returns NULL for slot < 0; in that case the caller should select
 * the primary font HFONT itself (winfb_draw_runs does this via the
 * nfont_primary parameter).
 * HFONTs are lazily created on first access.
 */
HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline);

/* ---- logging ---- */

void winfb_log_init(int level, const char *path);
void winfb_log_close(void);
void winfb_logf(int level, const char *fmt, ...);

#endif /* WINFONT_FALLBACK_H */
