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
