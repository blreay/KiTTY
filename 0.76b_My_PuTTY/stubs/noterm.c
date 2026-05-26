/*
 * Stubs of functions in terminal.c, for use in programs that don't
 * have a terminal.
 */

#include "putty.h"
#include "terminal.h"

void term_nopaste(Terminal *term)
{
}

int term_char_width(Terminal *term, unsigned int c)
{
    /* PUA width cannot be determined without a font; fall through */
    if (term)
        return term->cjk_ambig_wide ? mk_wcwidth_cjk(c) : mk_wcwidth(c);
    return mk_wcwidth(c);
}
