/*
 * pstv1080p Configurator — LiveArea app for the pstv1080p plugin.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Lists the installed games (ux0:app, ur0:app, gro0:app; names from each
 * title's param.sfo), shows the per-title override from
 * ur0:data/pstv1080p/pstv1080p_games.txt, and lets you change, remove or add
 * overrides and save the file.  A second screen edits the plugin's global
 * options (1080p on/off, frame pacing, target fps, inject, safe-boot window,
 * boot delay, watchdog).
 *
 * The file and the config are read and written THROUGH THE KERNEL MODULE
 * (pstv1080pReadGames / pstv1080pWriteGames / pstv1080pGetConfig /
 * pstv1080pSetConfig): the app itself never opens anything under ur0:, so it
 * needs no special permission.  Only the game list is read from the app
 * directories, which requires the app to be installed as unsafe homebrew.
 *
 * Controls (games screen):  Up/Down select, L/R page, Left/Right cycle the
 * override, Cross pick from the list with descriptions, Square remove the
 * override, Start save, Triangle global options, Select help, Circle exit.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vita2d.h>

#include "pstv1080p.h"

#define APP_VERSION      "1.6.8"
#define OWN_TITLE_ID     "PSTV10801"

#define SCREEN_W         960
#define SCREEN_H         544
#define ROW_H            28
#define LIST_TOP         74
#define LIST_ROWS        14
#define COL_NAME_X       28
#define COL_ID_X         610
#define COL_MODE_X       740
#define NAME_MAX_W       (COL_ID_X - COL_NAME_X - 16)

#define MAX_GAMES        512
#define NAME_LEN         96
#define TAIL_LEN         48                         /* text after the mode on a line, kept verbatim */
#define COMMENTS_LEN     PSTV1080P_GAMES_MAX_BYTES  /* '#' lines etc. can be as big as the file itself */

/* RGBA8() comes from vita2d.h (r | g<<8 | b<<16 | a<<24). */
#define C_BG        RGBA8(24, 26, 32, 255)
#define C_HEADER    RGBA8(38, 42, 52, 255)
#define C_FOOTER    RGBA8(38, 42, 52, 255)
#define C_ROW_ALT   RGBA8(30, 33, 40, 255)
#define C_SEL       RGBA8(58, 96, 150, 255)
#define C_TEXT      RGBA8(235, 235, 235, 255)
#define C_DIM       RGBA8(150, 150, 155, 255)
#define C_ACCENT    RGBA8(120, 190, 255, 255)
#define C_OK        RGBA8(120, 220, 140, 255)
#define C_WARN      RGBA8(255, 190, 90, 255)
#define C_ERR       RGBA8(255, 110, 110, 255)
#define C_POPUP     RGBA8(44, 48, 60, 255)
#define C_POPUP_BRD RGBA8(90, 130, 190, 255)
#define C_SHADE     RGBA8(0, 0, 0, 160)

/* ------------------------------------------------------------------------- */
/* Override modes (must match kernel/main.c games_list_load)                  */
/* ------------------------------------------------------------------------- */

enum { M_NONE = 0, M_FRAMESKIP, M_NOWAIT, M_OFF, M_INJECT, M_FORCE, M_SCALE, M_SPOOF720, M_TRACE, M_COUNT };

static const char *k_mode_name[M_COUNT] = {
    "none", "frameskip", "nowait", "off", "inject", "force", "scale", "spoof720", "trace"
};

static const char *k_mode_desc[M_COUNT] = {
    "No entry: the global frame-pacing rules apply.",
    "Skip every second vblank wait at 30 Hz. Fixes games that run at half speed.",
    "Never wait for vblank (like novsync) for this title. For games stuck at 15 fps.",
    "No pacing change and no inject for this title.",
    "Always wait one period after each frame flip (Framecapper \"Inject\").",
    "Framecapper-style fixed target (the global target fps) for this title.",
    "The default rule; use it to exempt a title from a global FORCE mode.",
    "Diagnostic: answer display queries as if the output were 720p60.",
    "Diagnostic: log this title's process lifecycle and allocations (slower start).",
};

static unsigned mode_color(int m)
{
    switch (m) {
    case M_NONE:      return C_DIM;
    case M_FRAMESKIP: return C_OK;
    case M_NOWAIT:    return C_WARN;
    case M_OFF:       return C_ERR;
    case M_INJECT:    return C_ACCENT;
    case M_FORCE:     return RGBA8(220, 140, 255, 255);
    case M_SCALE:     return C_OK;
    default:          return RGBA8(255, 230, 120, 255);
    }
}

/* File keywords only: "none" is not one (the kernel rejects such a line). */
static int mode_from_name(const char *s, int len)
{
    int i;
    for (i = 1; i < M_COUNT; i++) {
        const char *n = k_mode_name[i];
        int j;
        for (j = 0; j < len && n[j]; j++)
            if (tolower((unsigned char)s[j]) != n[j])
                break;
        if (j == len && n[j] == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Data                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    char id[10];
    char name[NAME_LEN];
    char tail[TAIL_LEN];   /* whatever followed the mode on its line (a note), written back */
    int  mode;
    int  orig_mode;
    int  novsync;          /* 1.6.3: the "novsync" switch attached to the option */
    int  orig_novsync;
    int  installed;
    int  from_file;        /* this title had a line in the file (first line wins, like the kernel) */
    int  file_order;       /* 1.6.6: 1-based position of its line in the file (0 = not from the file) */
    int  note_off;         /* 1.6.6: the comment lines that sat right above its line, kept in */
    int  note_len;         /*        g_comments[note_off .. note_off+note_len), written back above it */
} game_t;

static const char k_novsync_desc[] =
    "novsync: every flip is immediate and (with no option) all vblank waits return at once, like novsync.suprx.";

static game_t g_games[MAX_GAMES];
static int g_ngames;
static char g_comments[COMMENTS_LEN];   /* '#' and unparsable lines, kept verbatim, in file order */
static int g_comments_len;
static int g_comments_lost;             /* a kept line did not fit: saving would lose it */
static int g_trail_off, g_trail_len;    /* 1.6.6: comment lines after the last entry */
static int g_file_entries;              /* entries seen in the file (file_order counter) */
static int g_file_present;
static int g_load_failed;               /* file unreadable or larger than the kernel buffer: saving disabled */

static pstv1080p_config_t g_cfg, g_cfg_edit;
static pstv1080p_info_t g_info;
static int g_kernel_ok;
static int g_kernel_old;          /* module loaded but older than 1.6.1: it lacks ReadGames/WriteGames */
#define KERNEL_MIN_VERSION 0x0161u

static char g_status[160];
static unsigned g_status_color = C_DIM;
static int g_status_frames;
static int g_sel = 0, g_top = 0;         /* list selection (used by the post-save reload too) */
static void load_everything(void);
/* 1.6.6: an active Framecapper / novsync line in taiHEN's config.txt double-
 * caps every game together with pstv1080p; shown permanently until fixed. */
static char g_warn[200];

static void set_status(unsigned color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_color = color;
    g_status_frames = 60 * 5;
}

static int game_changed(const game_t *g)
{
    return g->mode != g->orig_mode || g->novsync != g->orig_novsync;
}

static int game_active(const game_t *g)
{
    return g->mode != M_NONE || g->novsync;
}

static int dirty(void)
{
    int i;
    for (i = 0; i < g_ngames; i++)
        if (game_changed(&g_games[i]))
            return 1;
    return 0;
}

static int active_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_ngames; i++)
        if (game_active(&g_games[i]))
            n++;
    return n;
}

/* Remove the first whole-word, case-insensitive occurrence of word from s
 * (collapsing the surrounding blank).  Returns 1 if it was there. */
static int strip_word(char *s, const char *word)
{
    int wl = (int)strlen(word), i, n = (int)strlen(s);
    for (i = 0; i + wl <= n; i++) {
        int j;
        if (i > 0 && s[i - 1] != ' ' && s[i - 1] != '\t')
            continue;
        for (j = 0; j < wl; j++)
            if (tolower((unsigned char)s[i + j]) != word[j])
                break;
        if (j != wl || (s[i + wl] != 0 && s[i + wl] != ' ' && s[i + wl] != '\t'))
            continue;
        {
            int end = i + wl;
            while (s[end] == ' ' || s[end] == '\t') end++;
            memmove(s + i, s + end, (size_t)(n - end + 1));
            n = (int)strlen(s);
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        }
        return 1;
    }
    return 0;
}

static game_t *find_game(const char *id)
{
    int i;
    for (i = 0; i < g_ngames; i++)
        if (strncmp(g_games[i].id, id, 9) == 0)
            return &g_games[i];
    return NULL;
}

static game_t *add_game(const char *id)
{
    game_t *g;
    if (g_ngames >= MAX_GAMES)
        return NULL;
    g = &g_games[g_ngames++];
    memset(g, 0, sizeof(*g));
    memcpy(g->id, id, 9);
    g->id[9] = 0;
    return g;
}

/* ------------------------------------------------------------------------- */
/* Games file (through the kernel)                                            */
/* ------------------------------------------------------------------------- */

/* Keep a line verbatim (optionally with a prefix) to write back ahead of the
 * entries.  Never overflows; a line that does not fit disables saving. */
static void comments_append2(const char *prefix, const char *s, int len)
{
    int cur = g_comments_len;
    int plen = prefix ? (int)strlen(prefix) : 0;
    if (cur + plen + len + 2 >= (int)COMMENTS_LEN) {
        g_comments_lost = 1;
        return;
    }
    if (plen)
        memcpy(g_comments + cur, prefix, (size_t)plen);
    memcpy(g_comments + cur + plen, s, (size_t)len);
    g_comments[cur + plen + len] = '\n';
    g_comments[cur + plen + len + 1] = 0;
    g_comments_len = cur + plen + len + 1;
}

/* Our own header lines are regenerated on every save: never keep them. */
static int is_own_header(const char *s, int len)
{
    return (len >= 31 && strncmp(s, "# pstv1080p per-title overrides", 31) == 0) ||
           (len >= 8  && strncmp(s, "# modes:", 8) == 0) ||
           (len >= 35 && strncmp(s, "# written by pstv1080p Configurator", 35) == 0);
}

static void comments_append(const char *s, int len)
{
    comments_append2(NULL, s, len);
}

static void load_games_file(void)
{
    static char buf[PSTV1080P_GAMES_MAX_BYTES];
    int n, i, start;

    g_comments[0] = 0;
    g_comments_len = 0;
    g_comments_lost = 0;
    g_trail_off = g_trail_len = 0;
    g_file_entries = 0;
    g_load_failed = 0;
    n = pstv1080pReadGames(buf, sizeof(buf));
    if (n < 0) {
        g_load_failed = 1;
        if (n == PSTV1080P_ERR_TOO_LARGE)
            set_status(C_ERR, "pstv1080p_games.txt is %u bytes or more: shorten it by hand (saving here is off)",
                       (unsigned)PSTV1080P_GAMES_MAX_BYTES);
        else
            set_status(C_ERR, "Could not read pstv1080p_games.txt (0x%08X): saving here is off", (unsigned)n);
        return;
    }
    g_file_present = n > 0;
    buf[n] = 0;

    /* 1.6.6: comment lines stay with the entry they sit above.  Lines are
     * kept in g_comments in file order; "pend" marks where the block that
     * belongs to the NEXT entry starts. */
    start = 0;
    for (i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0) {
            int s0 = start, e = i, t_end, m_start, m_end, mode;
            start = i + 1;
            while (s0 < e && (buf[s0] == ' ' || buf[s0] == '\t')) s0++;
            while (e > s0 && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;
            if (s0 >= e)
                continue;
            if (buf[s0] == '#') {
                if (!is_own_header(buf + s0, e - s0))
                    comments_append(buf + s0, e - s0);
                continue;
            }
            t_end = s0;
            while (t_end < e && buf[t_end] != ' ' && buf[t_end] != '\t') t_end++;
            m_start = t_end;
            while (m_start < e && (buf[m_start] == ' ' || buf[m_start] == '\t')) m_start++;
            m_end = m_start;
            while (m_end < e && buf[m_end] != ' ' && buf[m_end] != '\t') m_end++;
            mode = (m_start < e) ? mode_from_name(buf + m_start, m_end - m_start) : -1;
            /* "TITLEID novsync ..." (no option): the switch alone. */
            if (mode < 0 && m_start < e && (m_end - m_start) == 7 &&
                strncasecmp(buf + m_start, "novsync", 7) == 0)
                mode = M_NONE;
            if ((t_end - s0) != 9 || mode < 0) {
                /* The kernel ignores such a line too; keep it so nothing is lost. */
                comments_append(buf + s0, e - s0);
                continue;
            }
            {
                char id[10];
                game_t *g;
                int ts, tl;
                memcpy(id, buf + s0, 9);
                id[9] = 0;
                g = find_game(id);
                if (g && g->from_file) {
                    /* The kernel applies the FIRST line of a title.  A later
                     * duplicate never was in effect: keep it visible as a
                     * comment (written back before the entries, harmless). */
                    comments_append2("# duplicate ignored: ", buf + s0, e - s0);
                    continue;
                }
                if (!g)
                    g = add_game(id);
                if (!g)
                    continue;
                g->from_file = 1;
                g->file_order = ++g_file_entries;
                g->note_off = g_trail_off;              /* the block pending since the last entry */
                g->note_len = g_comments_len - g_trail_off;
                g_trail_off = g_comments_len;
                g->mode = mode;
                g->orig_mode = mode;
                /* Everything after the option is kept and written back on the
                 * same line; the "novsync" word in it is our switch. */
                ts = (mode == M_NONE) ? m_start : m_end;   /* line was "ID novsync ..." */
                while (ts < e && (buf[ts] == ' ' || buf[ts] == '\t'))
                    ts++;
                tl = e - ts;
                if (tl > TAIL_LEN - 1)
                    tl = TAIL_LEN - 1;
                memcpy(g->tail, buf + ts, (size_t)tl);
                g->tail[tl] = 0;
                if (strip_word(g->tail, "novsync"))
                    g->novsync = 1;
                g->orig_novsync = g->novsync;
            }
        }
    }
    g_trail_len = g_comments_len - g_trail_off;     /* comments after the last entry */
    if (g_comments_lost) {
        g_load_failed = 1;
        set_status(C_ERR, "pstv1080p_games.txt has more comment text than fits: edit it by hand (saving here is off)");
    }
}

/* Save order: the file's own entries in their original order, then the
 * ones added in the app (list order). */
static int cmp_save_order(const void *a, const void *b)
{
    const game_t *ga = *(const game_t *const *)a, *gb = *(const game_t *const *)b;
    int oa = ga->file_order ? ga->file_order : 0x7FFFFFFF;
    int ob = gb->file_order ? gb->file_order : 0x7FFFFFFF;
    if (oa != ob)
        return oa < ob ? -1 : 1;
    return (int)(ga - gb);
}

/* Append len bytes; 0 if they do not fit. */
static int out_put(char *buf, int cap, int *pos, const char *src, int len)
{
    if (*pos + len + 1 >= cap)
        return 0;
    memcpy(buf + *pos, src, (size_t)len);
    *pos += len;
    buf[*pos] = 0;
    return 1;
}

static int save_games_file(void)
{
    static char buf[PSTV1080P_GAMES_MAX_BYTES];
    int pos = 0, i, count = 0, r;

    if (g_load_failed) {
        set_status(C_ERR, "Not saved: the existing pstv1080p_games.txt could not be read completely");
        return -1;
    }
    count = active_count();
    if (count > (int)PSTV1080P_GAMES_MAX_ENTRIES) {
        set_status(C_ERR, "The plugin uses at most %u overrides (%d set): remove some first",
                   (unsigned)PSTV1080P_GAMES_MAX_ENTRIES, count);
        return -1;
    }
    count = 0;

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "# pstv1080p per-title overrides: \"TITLEID mode\" per line\n"
                    "# modes: frameskip nowait off inject force scale spoof720 trace\n"
                    "# written by pstv1080p Configurator " APP_VERSION "\n");
    {
        static const game_t *order[MAX_GAMES];
        int n_order = 0;
        for (i = 0; i < g_ngames; i++)
            if (game_active(&g_games[i]))
                order[n_order++] = &g_games[i];
        qsort(order, (size_t)n_order, sizeof(order[0]), cmp_save_order);

        for (i = 0; i < n_order; i++) {
            const game_t *g = order[i];
            char line[9 + 1 + 12 + 8 + TAIL_LEN + 4];
            int n = 0, ok;
            /* 1.6.6: the comment lines that sat above this entry go back above
             * it; an entry added here gets the game's name as its comment. */
            if (g->note_len > 0) {
                ok = out_put(buf, (int)sizeof(buf), &pos, g_comments + g->note_off, g->note_len);
            } else if (g->installed && g->name[0]) {
                char c[NAME_LEN + 4];
                int cl = snprintf(c, sizeof(c), "# %s\n", g->name);
                ok = out_put(buf, (int)sizeof(buf), &pos, c, cl);
            } else {
                ok = 1;
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s", g->id);
            if (g->mode != M_NONE)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", k_mode_name[g->mode]);
            if (g->novsync)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " novsync");
            if (g->tail[0])
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", g->tail);
            n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
            if (!ok || !out_put(buf, (int)sizeof(buf), &pos, line, n)) {
                set_status(C_ERR, "Comments and overrides do not fit in one file (limit reached after %d): nothing saved", count);
                return -1;
            }
            count++;
        }
        if (g_trail_len > 0 && !out_put(buf, (int)sizeof(buf), &pos, g_comments + g_trail_off, g_trail_len)) {
            set_status(C_ERR, "Comments and overrides do not fit in one file: nothing saved");
            return -1;
        }
    }
    buf[pos] = 0;

    r = pstv1080pWriteGames(buf, (uint32_t)pos);
    if (r < 0) {
        set_status(C_ERR, "Save failed: 0x%08X", (unsigned)r);
        return r;
    }
    for (i = 0; i < g_ngames; i++) {
        g_games[i].orig_mode = g_games[i].mode;
        g_games[i].orig_novsync = g_games[i].novsync;
    }
    g_file_present = 1;
    /* Re-read what is on disk so the comment blocks and the file order of
     * the next save match it (an entry added here now has a file position). */
    {
        int keep_sel = g_sel;
        load_everything();
        g_sel = keep_sel < g_ngames ? keep_sel : (g_ngames ? g_ngames - 1 : 0);
    }
    set_status(C_OK, "Saved %d override(s) to ur0:data/pstv1080p/pstv1080p_games.txt", count);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Installed games (param.sfo TITLE of every app directory)                   */
/* ------------------------------------------------------------------------- */

static int read_sfo_title(const char *path, char *out, int out_len)
{
    static unsigned char sfo[16384];
    SceUID fd;
    int n, i, entries;
    uint32_t key_off, data_off;

    out[0] = 0;
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, sfo, sizeof(sfo));
    sceIoClose(fd);
    if (n < 20 || memcmp(sfo, "\0PSF", 4) != 0)
        return -1;
    key_off  = sfo[8]  | (sfo[9] << 8)  | (sfo[10] << 16) | (sfo[11] << 24);
    data_off = sfo[12] | (sfo[13] << 8) | (sfo[14] << 16) | (sfo[15] << 24);
    entries  = (int)(sfo[16] | (sfo[17] << 8) | (sfo[18] << 16) | (sfo[19] << 24));
    if (entries <= 0)
        return -1;                    /* the loop below stops at the bytes actually read */
    for (i = 0; i < entries; i++) {
        const unsigned char *e = sfo + 20 + 16 * i;
        uint32_t ko, fmt, len, doff, kpos, dpos;
        if (20 + 16 * (i + 1) > n)
            break;
        ko   = e[0] | (e[1] << 8);
        fmt  = e[2] | (e[3] << 8);
        len  = e[4] | (e[5] << 8) | (e[6] << 16) | (e[7] << 24);
        doff = e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24);
        kpos = key_off + ko;
        dpos = data_off + doff;
        if (kpos + 6 > (uint32_t)n || dpos >= (uint32_t)n)
            continue;
        if (fmt == 0x0204 && strncmp((const char *)sfo + kpos, "TITLE", 6) == 0) {
            int copy = (int)len, j, k = 0;
            if (copy > n - (int)dpos) copy = n - (int)dpos;
            if (copy > out_len - 1) copy = out_len - 1;
            for (j = 0; j < copy && sfo[dpos + j]; j++) {
                unsigned char c = sfo[dpos + j];
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (c == ' ' && k > 0 && out[k - 1] == ' ')
                    continue;
                out[k++] = (char)c;
            }
            while (k > 0 && out[k - 1] == ' ') k--;
            out[k] = 0;
            return 0;
        }
    }
    return -1;
}

/* One app root.  ux0:app / ur0:app / gro0:app hold <id>/sce_sys/param.sfo;
 * ur0:appmeta (every installed bubble, cartridges included) holds
 * <id>/param.sfo.  A title seen without a readable name is still listed. */
static void scan_root(const char *root, int appmeta)
{
    SceUID d = sceIoDopen(root);
    SceIoDirent ent;
    if (d < 0)
        return;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0) {
        char path[320];
        char name[NAME_LEN];
        game_t *g;
        int have_name;
        if (!SCE_S_ISDIR(ent.d_stat.st_mode) || strlen(ent.d_name) != 9)
            goto next;
        if (strncmp(ent.d_name, "NPXS", 4) == 0 || strcmp(ent.d_name, OWN_TITLE_ID) == 0)
            goto next;
        snprintf(path, sizeof(path), appmeta ? "%s/%s/param.sfo" : "%s/%s/sce_sys/param.sfo", root, ent.d_name);
        have_name = read_sfo_title(path, name, sizeof(name)) == 0 && name[0];
        if (!have_name && !appmeta)
            goto next;                 /* not an app directory */
        g = find_game(ent.d_name);
        if (!g)
            g = add_game(ent.d_name);
        if (!g)
            break;
        if (!g->installed || (have_name && g->name[0] == '(')) {
            g->installed = 1;
            if (have_name) {
                strncpy(g->name, name, NAME_LEN - 1);
                g->name[NAME_LEN - 1] = 0;
            } else if (!g->name[0]) {
                snprintf(g->name, NAME_LEN, "(name unavailable)");
            }
        }
next:
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
}

static int cmp_games(const void *a, const void *b)
{
    const game_t *x = a, *y = b;
    if (x->installed != y->installed)
        return y->installed - x->installed;          /* installed first */
    if (x->installed) {
        int r = strcasecmp(x->name, y->name);
        if (r) return r;
    }
    return strcmp(x->id, y->id);
}

static void load_everything(void)
{
    g_ngames = 0;
    load_games_file();
    scan_root("ux0:app", 0);
    scan_root("ur0:app", 0);
    scan_root("gro0:app", 0);
    scan_root("ur0:appmeta", 1);
    {
        int i;
        for (i = 0; i < g_ngames; i++)
            if (!g_games[i].installed)
                snprintf(g_games[i].name, NAME_LEN, "(not installed)");
    }
    qsort(g_games, (size_t)g_ngames, sizeof(g_games[0]), cmp_games);
}

/* ------------------------------------------------------------------------- */
/* Kernel state                                                               */
/* ------------------------------------------------------------------------- */

static const char *mode_code_name(uint32_t m)
{
    switch (m) {
    case 0x8300: return "480p";
    case 0x8600: return "720p";
    case 0x8500: return "1080i";
    case 0x8710: return "1080p30";
    case 0x8720: return "1080p24";
    case 0x8700: return "1080p60";
    case 0x10000000: return "automatic";
    default: return "?";
    }
}

static void refresh_kernel(void)
{
    memset(&g_info, 0, sizeof(g_info));
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_kernel_ok = (pstv1080pGetInfo(&g_info) >= 0 && pstv1080pGetConfig(&g_cfg) >= 0 &&
                   g_cfg.magic == PSTV1080P_CFG_MAGIC);
    /* An older module resolves the newer syscalls to nothing: calling them
     * jumps to address 0 (that was the first crash report).  Never call
     * ReadGames/WriteGames unless the module says it has them. */
    g_kernel_old = g_kernel_ok && g_info.version < KERNEL_MIN_VERSION;
    if (g_kernel_old)
        g_kernel_ok = 0;
    g_cfg_edit = g_cfg;
}

/* ------------------------------------------------------------------------- */
/* Drawing helpers                                                            */
/* ------------------------------------------------------------------------- */

static vita2d_pgf *g_font;

static void text(int x, int y, unsigned color, const char *s)
{
    vita2d_pgf_draw_text(g_font, x, y, color, 1.0f, s);
}

static void textf(int x, int y, unsigned color, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    text(x, y, color, b);
}

static int text_w(const char *s)
{
    return vita2d_pgf_text_width(g_font, 1.0f, s);
}

/* Copy s into out, cutting it (at a UTF-8 boundary) with "..." if wider than maxw. */
static void fit_text(char *out, int out_len, const char *s, int maxw)
{
    int len = (int)strlen(s);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, s, (size_t)len);
    out[len] = 0;
    if (text_w(out) <= maxw)
        return;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80)
            len--;
        out[len] = 0;
        strncat(out, "...", (size_t)(out_len - len - 1));
        if (text_w(out) <= maxw)
            return;
        out[len] = 0;
    }
}

static void text_center(int cx, int y, unsigned color, const char *s)
{
    text(cx - text_w(s) / 2, y, color, s);
}

static void draw_header(const char *title)
{
    char kern[128];
    vita2d_draw_rectangle(0, 0, SCREEN_W, 48, C_HEADER);
    text(COL_NAME_X, 32, C_TEXT, title);
    if (g_kernel_ok) {
        unsigned v = g_info.version;
        snprintf(kern, sizeof(kern), "plugin %u.%u.%u  |  1080p %s  |  output %s (%u Hz)",
                 (v >> 8) & 0xFF, (v >> 4) & 0xF, v & 0xF, g_cfg.mode_1080p ? "ON" : "off",
                 mode_code_name(g_info.current_output_mode), (unsigned)g_info.refresh_hz);
        text(SCREEN_W - 28 - text_w(kern), 32, C_DIM, kern);
    } else {
        snprintf(kern, sizeof(kern), "pstv1080p.skprx not loaded");
        text(SCREEN_W - 28 - text_w(kern), 32, C_ERR, kern);
    }
}

static void draw_footer(const char *hints)
{
    vita2d_draw_rectangle(0, SCREEN_H - 40, SCREEN_W, 40, C_FOOTER);
    text(COL_NAME_X, SCREEN_H - 13, C_DIM, hints);
}

/* 1.6.6: taiHEN uses ux0:tai/config.txt when it exists, else ur0:tai/config.txt.
 * An active (uncommented) Framecapper*.suprx or novsync.suprx line makes that
 * plugin pace the same games as pstv1080p: every wait doubles (30 fps games at
 * 15, 60 fps games at half speed).  The kernel module steps aside in such a
 * process, so the per-game rules set here do nothing there: say so on screen. */
static void check_taihen_config(void)
{
    static char buf[16384];
    static const char *paths[] = { "ux0:tai/config.txt", "ur0:tai/config.txt" };
    char section[48] = "(top)";
    int fd = -1, n, i, start;
    const char *used = NULL;

    g_warn[0] = 0;
    for (i = 0; i < 2 && fd < 0; i++) {
        fd = sceIoOpen(paths[i], SCE_O_RDONLY, 0);
        if (fd >= 0)
            used = paths[i];
    }
    if (fd < 0)
        return;
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    start = 0;
    for (i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0) {
            int s0 = start, e = i, k;
            start = i + 1;
            while (s0 < e && (buf[s0] == ' ' || buf[s0] == '\t')) s0++;
            while (e > s0 && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;
            if (s0 >= e || buf[s0] == '#')
                continue;
            if (buf[s0] == '*') {
                int l = e - s0 < (int)sizeof(section) - 1 ? e - s0 : (int)sizeof(section) - 1;
                memcpy(section, buf + s0, (size_t)l);
                section[l] = 0;
                continue;
            }
            for (k = s0; k + 11 <= e; k++) {
                if (strncasecmp(buf + k, "framecapper", 11) == 0 || strncasecmp(buf + k, "novsync.sup", 11) == 0) {
                    const char *slash = buf + e;
                    while (slash > buf + s0 && slash[-1] != '/' && slash[-1] != ':') slash--;
                    snprintf(g_warn, sizeof(g_warn), "%s: %.*s is active under %s - it double-caps; pstv1080p steps aside there. Comment it out.",
                             used, (int)(buf + e - slash), slash, section);
                    return;
                }
            }
        }
    }
}

static void draw_status(void)
{
    if (!(g_status_frames > 0 && g_status[0]) && g_warn[0])
        text(COL_NAME_X, SCREEN_H - 50, C_ERR, g_warn);
    if (g_status_frames > 0 && g_status[0]) {
        text(COL_NAME_X, SCREEN_H - 50, g_status_color, g_status);
        g_status_frames--;
    }
}

/* ------------------------------------------------------------------------- */
/* Screens                                                                    */
/* ------------------------------------------------------------------------- */

enum { SCR_GAMES = 0, SCR_SETTINGS, SCR_MODEPICK, SCR_CONFIRM_EXIT, SCR_HELP, SCR_NOKERNEL };

static int g_screen = SCR_GAMES;
static int g_pick_sel = 0;
static int g_set_sel = 0;

static void clamp_list(void)
{
    if (g_ngames == 0) { g_sel = 0; g_top = 0; return; }
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_ngames) g_sel = g_ngames - 1;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + LIST_ROWS) g_top = g_sel - LIST_ROWS + 1;
    if (g_top < 0) g_top = 0;
}

static void draw_games(void)
{
    int i;
    char nm[NAME_LEN + 4];

    draw_header("pstv1080p Configurator " APP_VERSION " - per-game overrides");
    text(COL_NAME_X, LIST_TOP - 8, C_DIM, "Game");
    text(COL_ID_X, LIST_TOP - 8, C_DIM, "Title ID");
    text(COL_MODE_X, LIST_TOP - 8, C_DIM, "Override");

    if (g_ngames == 0) {
        text_center(SCREEN_W / 2, 240, C_DIM, "No games found in ux0:app, ur0:app or gro0:app.");
        text_center(SCREEN_W / 2, 270, C_DIM, "(The app must be installed as unsafe homebrew to read the app folders.)");
    }
    for (i = 0; i < LIST_ROWS; i++) {
        int idx = g_top + i;
        int y = LIST_TOP + i * ROW_H;
        const game_t *g;
        unsigned name_col;
        if (idx >= g_ngames)
            break;
        g = &g_games[idx];
        if (idx == g_sel)
            vita2d_draw_rectangle(16, y, SCREEN_W - 32, ROW_H, C_SEL);
        else if (i & 1)
            vita2d_draw_rectangle(16, y, SCREEN_W - 32, ROW_H, C_ROW_ALT);
        name_col = g->installed ? C_TEXT : C_DIM;
        fit_text(nm, sizeof(nm), g->name, NAME_MAX_W);
        text(COL_NAME_X, y + 20, name_col, nm);
        text(COL_ID_X, y + 20, C_DIM, g->id);
        textf(COL_MODE_X, y + 20, game_active(g) ? mode_color(g->mode == M_NONE ? M_NOWAIT : g->mode) : (unsigned)C_DIM,
              "%s%s%s", (g->mode == M_NONE && g->novsync) ? "" : k_mode_name[g->mode],
              g->novsync ? ((g->mode == M_NONE) ? "novsync" : "+novsync") : "",
              game_changed(g) ? " *" : "");
    }
    if (g_ngames > LIST_ROWS) {
        /* scrollbar */
        int track_h = LIST_ROWS * ROW_H;
        int th = track_h * LIST_ROWS / g_ngames;
        int ty = LIST_TOP + (track_h - th) * g_top / (g_ngames - LIST_ROWS);
        if (th < 12) th = 12;
        vita2d_draw_rectangle(SCREEN_W - 14, LIST_TOP, 6, track_h, C_ROW_ALT);
        vita2d_draw_rectangle(SCREEN_W - 14, ty, 6, th, C_DIM);
    }
    draw_status();
    {
        char cnt[40];
        snprintf(cnt, sizeof(cnt), "%d/%u overrides", active_count(), (unsigned)PSTV1080P_GAMES_MAX_ENTRIES);
        text(SCREEN_W - 28 - text_w(cnt), LIST_TOP - 8, C_DIM, cnt);
    }
    draw_footer(dirty()
        ? "Left/Right change   X pick   [] remove   START save*   /\\ options   SELECT help   O exit"
        : "Left/Right change   X pick   [] remove   START save    /\\ options   SELECT help   O exit");
}

static void draw_popup_box(int w, int h, const char *title)
{
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_SHADE);
    vita2d_draw_rectangle(x - 2, y - 2, w + 4, h + 4, C_POPUP_BRD);
    vita2d_draw_rectangle(x, y, w, h, C_POPUP);
    text(x + 20, y + 32, C_TEXT, title);
}

#define PICK_ROWS (M_COUNT + 1)          /* the options + the novsync switch */

static void draw_modepick(void)
{
    int w = 760, h = 24 + 40 + PICK_ROWS * 30 + 80, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2, i;
    char title[160];
    const game_t *g = &g_games[g_sel];
    draw_games();
    snprintf(title, sizeof(title), "Override for %s  [%s]", g->name, g->id);
    draw_popup_box(w, h, title);
    for (i = 0; i < M_COUNT; i++) {
        int ry = y + 50 + i * 30;
        if (i == g_pick_sel)
            vita2d_draw_rectangle(x + 12, ry, w - 24, 28, C_SEL);
        text(x + 24, ry + 20, mode_color(i), k_mode_name[i]);
        if (i == g->mode)
            text(x + 160, ry + 20, C_DIM, "(current)");
    }
    {
        int ry = y + 50 + M_COUNT * 30 + 6;
        vita2d_draw_rectangle(x + 12, ry - 4, w - 24, 1, C_POPUP_BRD);
        if (g_pick_sel == M_COUNT)
            vita2d_draw_rectangle(x + 12, ry, w - 24, 28, C_SEL);
        textf(x + 24, ry + 20, g->novsync ? C_WARN : C_DIM, "[%s] novsync", g->novsync ? "X" : " ");
        text(x + 160, ry + 20, C_DIM, "attach to any option (X toggles)");
    }
    text(x + 20, y + h - 40, C_DIM, g_pick_sel == M_COUNT ? k_novsync_desc : k_mode_desc[g_pick_sel]);
    text(x + 20, y + h - 14, C_DIM, "X choose / toggle   O close");
}

static void draw_confirm_exit(void)
{
    draw_games();
    draw_popup_box(560, 150, "You have unsaved changes.");
    text_center(SCREEN_W / 2, SCREEN_H / 2 + 10, C_TEXT, "X  save and exit      []  exit without saving      O  back");
}

static void draw_help(void)
{
    int y = 120, i;
    draw_games();
    draw_popup_box(860, 372, "Per-game overrides (saved to ur0:data/pstv1080p/pstv1080p_games.txt)");
    for (i = 1; i < M_COUNT; i++) {
        textf(70, y, mode_color(i), "%-10s", k_mode_name[i]);
        text(190, y, C_TEXT, k_mode_desc[i]);
        y += 28;
    }
    textf(70, y, C_WARN, "%-10s", "novsync");
    text(190, y, C_TEXT, "Switch on top of any option: immediate flips (+ no waits when alone). Old novsync+Framecapper60Inject = nowait+novsync+inject.");
    y += 28;
    text(70, y + 12, C_DIM, "Changes apply the next time the game is started; no reboot needed.");
    text(70, y + 40, C_DIM, "O close");
}

/* Settings screen -------------------------------------------------------- */

#define SET_ROWS 7
static const char *k_set_label[SET_ROWS] = {
    "1080p (30 Hz) output",
    "Frame pacing",
    "FORCE target fps",
    "Inject after frame flip",
    "Safe-boot revert window",
    "Boot apply delay",
    "Watchdog period",
};
static const char *k_set_help[SET_ROWS] = {
    "Same as picking the entry in Settings > Sound & Display. Applied when you press START.",
    "SCALE keeps each game's intended speed under 30 Hz (default). FORCE = Framecapper-style fixed target. OFF = no pacing.",
    "Target frame rate used by FORCE mode (and by per-game 'force' overrides).",
    "AUTO injects a one-period wait only for games that never sync themselves. ALWAYS = Framecapper Inject builds.",
    "If the console reboots within this many seconds after booting in 1080p, 1080p is switched off. 0 disables.",
    "Wait after SceShell starts before applying 1080p at boot.",
    "How often the plugin re-checks that the output mode is still 1080p. 0 disables.",
};

static void set_value_str(int row, const pstv1080p_config_t *c, char *out, int len)
{
    switch (row) {
    case 0: snprintf(out, len, "%s", c->mode_1080p ? "ON" : "off"); break;
    case 1: snprintf(out, len, "%s", c->fps_mode == 0 ? "OFF" : c->fps_mode == 1 ? "SCALE (default)" : "FORCE"); break;
    case 2: snprintf(out, len, "%u", (unsigned)c->fps_target); break;
    case 3: snprintf(out, len, "%s", c->fps_inject == 0 ? "off" : c->fps_inject == 1 ? "AUTO (default)" : "ALWAYS"); break;
    case 4: if (c->safe_boot_seconds) snprintf(out, len, "%u s", (unsigned)c->safe_boot_seconds); else snprintf(out, len, "disabled"); break;
    case 5: snprintf(out, len, "%u ms", (unsigned)c->boot_apply_delay_ms); break;
    case 6: if (c->watchdog_period_ms) snprintf(out, len, "%u ms", (unsigned)c->watchdog_period_ms); else snprintf(out, len, "disabled"); break;
    }
}

static void set_adjust(int row, int dir)
{
    pstv1080p_config_t *c = &g_cfg_edit;
    switch (row) {
    case 0: c->mode_1080p = !c->mode_1080p; break;
    case 1: c->fps_mode = (uint32_t)(((int)c->fps_mode + dir + 3) % 3); break;
    case 2: c->fps_target = (c->fps_target == 20) ? (dir > 0 ? 30 : 60) : (c->fps_target == 30) ? (dir > 0 ? 60 : 20) : (dir > 0 ? 20 : 30); break;
    case 3: c->fps_inject = (uint32_t)(((int)c->fps_inject + dir + 3) % 3); break;
    case 4: { int v = (int)c->safe_boot_seconds + dir * 30; if (v < 0) v = 0; if (v > 600) v = 600; c->safe_boot_seconds = (uint32_t)v; } break;
    case 5: { int v = (int)c->boot_apply_delay_ms + dir * 500; if (v < 500) v = 500; if (v > 15000) v = 15000; c->boot_apply_delay_ms = (uint32_t)v; } break;
    case 6: { int v = (int)c->watchdog_period_ms + dir * 500; if (v < 0) v = 0; if (v > 0 && v < 1000) v = dir > 0 ? 1000 : 0; if (v > 10000) v = 10000; c->watchdog_period_ms = (uint32_t)v; } break;
    }
}

static int settings_dirty(void)
{
    return memcmp(&g_cfg, &g_cfg_edit, sizeof(g_cfg)) != 0;
}

static void draw_settings(void)
{
    int i;
    draw_header("pstv1080p Configurator " APP_VERSION " - global options");
    for (i = 0; i < SET_ROWS; i++) {
        int y = 90 + i * 44;
        char v[48];
        int changed;
        set_value_str(i, &g_cfg_edit, v, sizeof(v));
        {
            char cur[48];
            set_value_str(i, &g_cfg, cur, sizeof(cur));
            changed = strcmp(cur, v) != 0;
        }
        if (i == g_set_sel)
            vita2d_draw_rectangle(16, y - 4, SCREEN_W - 32, 36, C_SEL);
        text(COL_NAME_X, y + 20, C_TEXT, k_set_label[i]);
        textf(520, y + 20, changed ? C_WARN : C_ACCENT, "<  %s  >%s", v, changed ? "  *" : "");
    }
    text(COL_NAME_X, 90 + SET_ROWS * 44 + 20, C_DIM, k_set_help[g_set_sel]);
    draw_status();
    draw_footer(settings_dirty()
        ? "Left/Right change   START apply*   O back (discards unapplied changes)"
        : "Left/Right change   START apply    O back");
}

static void apply_settings(void)
{
    int r = pstv1080pSetConfig(&g_cfg_edit);
    refresh_kernel();                 /* the kernel has taken the values even when applying them failed */
    if (r < 0) {
        set_status(C_ERR, "Options stored, but saving or applying them failed (0x%08X)", (unsigned)r);
        return;
    }
    set_status(C_OK, "Options applied.");
}

static void draw_nokernel(void)
{
    draw_header("pstv1080p Configurator " APP_VERSION);
    if (g_kernel_old) {
        unsigned v = g_info.version;
        textf(SCREEN_W / 2 - 300, 230, C_ERR, "The pstv1080p kernel module is version %u.%u.%u; this app needs 1.6.1 or newer.",
              (v >> 8) & 0xFF, (v >> 4) & 0xF, v & 0xF);
        text_center(SCREEN_W / 2, 262, C_DIM, "Replace ur0:tai/pstv1080p.skprx with the one from the same release as this app and reboot.");
    } else {
        text_center(SCREEN_W / 2, 230, C_ERR, "The pstv1080p kernel module is not loaded.");
        text_center(SCREEN_W / 2, 262, C_DIM, "Add  ur0:tai/pstv1080p.skprx  under *KERNEL in ur0:tai/config.txt and reboot.");
    }
    text_center(SCREEN_W / 2, 294, C_DIM, "This app reads and writes the plugin's files through that module.");
    draw_footer("O exit");
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

static unsigned g_prev_buttons;
static unsigned g_held_buttons;
static int g_held_frames;

/* Returns the buttons to act on this frame: newly pressed ones, plus the
 * d-pad with auto-repeat while held. */
static unsigned read_input(void)
{
    SceCtrlData pad;
    unsigned now, pressed, repeat = 0;
    const unsigned dpad = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    now = pad.buttons;
    pressed = now & ~g_prev_buttons;
    g_prev_buttons = now;

    if ((now & dpad) && (now & dpad) == (g_held_buttons & dpad)) {
        g_held_frames++;
        if (g_held_frames > 18 && (g_held_frames % 4) == 0)
            repeat = now & dpad;
    } else {
        g_held_buttons = now;
        g_held_frames = 0;
    }
    return pressed | repeat;
}

static void input_games(unsigned b)
{
    game_t *g = g_ngames ? &g_games[g_sel] : NULL;
    if (b & SCE_CTRL_UP)    { g_sel--; }
    if (b & SCE_CTRL_DOWN)  { g_sel++; }
    if (b & SCE_CTRL_LTRIGGER) { g_sel -= LIST_ROWS; }
    if (b & SCE_CTRL_RTRIGGER) { g_sel += LIST_ROWS; }
    clamp_list();
    g = g_ngames ? &g_games[g_sel] : NULL;
    if (g && (b & SCE_CTRL_RIGHT)) g->mode = (g->mode + 1) % M_COUNT;
    if (g && (b & SCE_CTRL_LEFT))  g->mode = (g->mode + M_COUNT - 1) % M_COUNT;
    if (g && (b & SCE_CTRL_SQUARE)) {
        g->mode = M_NONE;
        g->novsync = 0;
        set_status(C_DIM, "Override removed for %s (press START to save)", g->id);
    }
    if (g && (b & SCE_CTRL_CROSS)) { g_pick_sel = g->mode; g_screen = SCR_MODEPICK; }
    if (b & SCE_CTRL_START)    save_games_file();
    if (b & SCE_CTRL_TRIANGLE) { refresh_kernel(); g_screen = SCR_SETTINGS; }
    if (b & SCE_CTRL_SELECT)   g_screen = SCR_HELP;
    if (b & SCE_CTRL_CIRCLE) {
        if (dirty()) g_screen = SCR_CONFIRM_EXIT;
        else sceKernelExitProcess(0);
    }
}

static void input_modepick(unsigned b)
{
    if (b & SCE_CTRL_UP)   g_pick_sel = (g_pick_sel + PICK_ROWS - 1) % PICK_ROWS;
    if (b & SCE_CTRL_DOWN) g_pick_sel = (g_pick_sel + 1) % PICK_ROWS;
    if (b & SCE_CTRL_CROSS) {
        if (g_pick_sel == M_COUNT) {
            g_games[g_sel].novsync = !g_games[g_sel].novsync;   /* toggle, stay open */
        } else {
            g_games[g_sel].mode = g_pick_sel;
            g_screen = SCR_GAMES;
        }
    }
    if (b & SCE_CTRL_CIRCLE) g_screen = SCR_GAMES;
}

static void input_confirm_exit(unsigned b)
{
    if (b & SCE_CTRL_CROSS) {
        if (save_games_file() == 0)
            sceKernelExitProcess(0);
        g_screen = SCR_GAMES;
    }
    if (b & SCE_CTRL_SQUARE) sceKernelExitProcess(0);
    if (b & SCE_CTRL_CIRCLE) g_screen = SCR_GAMES;
}

static void input_settings(unsigned b)
{
    if (b & SCE_CTRL_UP)    g_set_sel = (g_set_sel + SET_ROWS - 1) % SET_ROWS;
    if (b & SCE_CTRL_DOWN)  g_set_sel = (g_set_sel + 1) % SET_ROWS;
    if (b & SCE_CTRL_LEFT)  set_adjust(g_set_sel, -1);
    if (b & SCE_CTRL_RIGHT) set_adjust(g_set_sel, +1);
    if (b & SCE_CTRL_START) apply_settings();
    if (b & SCE_CTRL_CIRCLE) { g_cfg_edit = g_cfg; g_screen = SCR_GAMES; }
}

/* ------------------------------------------------------------------------- */

/* System font groups: Latin (and Cyrillic) from the Latin font, Hangul from
 * the Korean font, everything else (kana, kanji, symbols) from the default
 * font.  The catch-all entry must come last. */
static int in_latin(unsigned int c)  { return c <= 0x00FF || (c >= 0x0400 && c <= 0x04FF); }
static int in_hangul(unsigned int c) { return (c >= 0x3130 && c <= 0x318F) || (c >= 0xAC00 && c <= 0xD7AF) || c == 0xFFE6; }

int main(int argc, char *argv[])
{
    static const vita2d_system_pgf_config font_cfg[] = {
        { SCE_FONT_LANGUAGE_LATIN,   in_latin  },
        { SCE_FONT_LANGUAGE_KOREAN,  in_hangul },
        { SCE_FONT_LANGUAGE_DEFAULT, NULL      },
    };
    (void)argc; (void)argv;

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    g_font = vita2d_load_system_pgf(3, font_cfg);
    if (!g_font)
        g_font = vita2d_load_default_pgf();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    refresh_kernel();
    check_taihen_config();
    if (g_kernel_ok) {
        load_everything();
        if (!g_file_present && !g_load_failed)
            set_status(C_DIM, "No pstv1080p_games.txt yet: pick overrides and press START to create it.");
    } else {
        g_screen = SCR_NOKERNEL;
    }

    for (;;) {
        unsigned b = read_input();

        switch (g_screen) {
        case SCR_GAMES:        input_games(b); break;
        case SCR_MODEPICK:     input_modepick(b); break;
        case SCR_CONFIRM_EXIT: input_confirm_exit(b); break;
        case SCR_HELP:         if (b & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT | SCE_CTRL_CROSS)) g_screen = SCR_GAMES; break;
        case SCR_SETTINGS:     input_settings(b); break;
        case SCR_NOKERNEL:     if (b & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) sceKernelExitProcess(0); break;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        switch (g_screen) {
        case SCR_GAMES:        draw_games(); break;
        case SCR_MODEPICK:     draw_modepick(); break;
        case SCR_CONFIRM_EXIT: draw_confirm_exit(); break;
        case SCR_HELP:         draw_help(); break;
        case SCR_SETTINGS:     draw_settings(); break;
        case SCR_NOKERNEL:     draw_nokernel(); break;
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    /* not reached */
    vita2d_fini();
    return 0;
}
