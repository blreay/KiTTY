#ifndef KITTY_FONTFALLBACK_H
#define KITTY_FONTFALLBACK_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HFONT hfont;    /* NULL = use primary font */
    int   glyph_px; /* actual glyph pixel width; 0 = unknown */
    int   y_adjust; /* pixels to add to draw_y: primary_ascent - fallback_ascent */
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

/* Call after kff_init() to prepend user-configured fonts (wide strings).
   Resets user list first; pass count=0 to clear. */
void kff_set_user_fonts(const wchar_t **names, int count);

#ifdef __cplusplus
}
#endif

#endif /* KITTY_FONTFALLBACK_H */
