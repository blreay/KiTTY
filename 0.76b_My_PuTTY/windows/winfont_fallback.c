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
#include <stdint.h>

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
    if (g_log_fp) {
        setvbuf(g_log_fp, NULL, _IOFBF, 8192);
    }
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
 *  Module state (used by Task 6+ implementations)
 * =================================================================== */

#define WINFB_MAX_SLOTS   16
#define WINFB_ATTR_DIM     8   /* bold(2) x italic(2) x underline(2) */
#define WINFB_SMP_CAP   1024
#define WINFB_OVR_CAP     64

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

typedef struct { uint32_t cp; int8_t slot; } winfb_smp_ent;
static winfb_smp_ent g_smp[WINFB_SMP_CAP];
static int g_n_smp;

/* Override ranges */
typedef struct { uint32_t lo, hi; int slot; } winfb_override;
static winfb_override g_ovr[WINFB_OVR_CAP];
static int g_n_ovr;

/* Suppress unused-static warnings for state that Tasks 6-10 will use. */
static void winfb_state_touch_unused(void)
{
    (void)g_slots; (void)g_n_slots;
    (void)g_base_lf; (void)g_cell_w; (void)g_cell_h;
    (void)g_probe_dc; (void)g_initialised;
    (void)g_bmp_map;
    (void)g_smp; (void)g_n_smp;
    (void)g_ovr; (void)g_n_ovr;
    (void)winfb_state_touch_unused;
}

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
