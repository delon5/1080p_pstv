/*
 * pstv1080p_settings.suprx — Settings-app plugin for the pstv1080p kernel module.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Loaded by taiHEN under *NPXS10015 (the Settings app, main module "SceSettings"),
 * alongside henkaku.suprx. It adds a virtual "1080p (30 Hz)" entry to
 * Settings > Sound & Display > HDMI resolution without ever writing an unknown
 * value into Sony's registry key /CONFIG/DISPLAY/hdmi_resolution_mode.
 *
 * DESIGN (docs/DESIGN.md section B):
 *  1. module_start hooks the SceSettings imports sceKernelLoadStartModule and
 *     sceKernelStopUnloadModule. When system_settings_core.suprx is loaded the
 *     page hooks below are installed; they are released when it is unloaded
 *     (same technique as HENkaku's plugin/user.c; taiHEN chains both plugins).
 *  2. scePafMiscLoadXmlLayout (SceSettings <- ScePafMisc): if the XML page
 *     contains "hdmi_resolution_mode", the original is dumped once to
 *     ux0:data/pstv1080p/settings_page_orig.xml, the <list ...> element that
 *     binds that key is located, its <list_item value="N"> numbers are parsed
 *     and logged, a free value N is chosen (kernel cfg.settings_item_value if
 *     unused, else the smallest integer >= 3 not used, pushed to the kernel), and
 *       <list_item id="id_pstv1080p_1080p" title="msg_pstv1080p_1080p" value="N"/>
 *     is inserted before </list> into a static 64 KiB buffer that is handed to
 *     the real function. Any doubt -> the original buffer is passed through.
 *  3. sceRegMgrGetKeyInt (SceSystemSettingsCore <- SceRegMgr): for our key,
 *     when the kernel says mode_1080p is on, report N so the list highlights
 *     our entry; otherwise the real registry value is returned.
 *  4. sceRegMgrSetKeyInt: value == N -> pstv1080pSetMode1080p(1), registry is
 *     NOT written, return 0. Any other value -> if 1080p is on, turn it off via
 *     pstv1080pSetMode1080p(0) first, then let Sony's code write the registry
 *     and apply its own mode (the kernel hook passes it through once 1080p is off).
 *  5. NATIVE value->mode (1.5.0, docs/RESEARCH_NOTES.md section 12): the
 *     function in SceSettings (the main module, NOT the core) that turns the
 *     list value into a screen-mode code is a compare ladder
 *       1 -> 0x8500 (1080i), 2 -> 0x8600 (720p), 3 -> 0x8300 (480p),
 *       anything else -> 0x10000000 (automatic, "known" flag 0)
 *     followed by sceAVConfigHdmiSetResolution(mode, known, 1) and the
 *     registry write. At module_start the 58-byte signature of that function
 *     is searched in SceSettings' text segment; on exactly one match its
 *     52-byte dispatch is replaced IN MEMORY (taiInjectData, released on
 *     unload, nothing on disk) by an equivalent ladder with a fifth case
 *       N -> cfg.hd_mode_code (0x8710, "known" 1).
 *     From then on Sony's own code maps our entry itself; the kernel hook
 *     sees a plain request for 0x8710 and just lets it through. If the
 *     signature is missing or ambiguous (other firmware) nothing is patched
 *     and the kernel hook keeps doing the substitution as in 1.4.x.
 *  6. scePafToplevelGetText (SceSystemSettingsCore <- ScePafToplevel): returns
 *     the UTF-16 string "1080p (30 Hz)" for msg id "msg_pstv1080p_1080p".
 *  7. Kernel config is fetched with pstv1080pGetConfig at module_start, again
 *     when the page is loaded, on every access to our key and after every Set.
 *  8. Logged to ux0:data/pstv1080p/settings.log (append, best effort): quiet
 *     by default (user actions, state changes, failures); everything when the
 *     kernel reports verbose mode (ur0:tai/pstv1080p_verbose.txt exists).
 *  9. Creating ux0:data/pstv1080p/dump_request makes the plugin write the
 *     Settings modules (as mapped in memory) to dump_*.bin once, for analysis
 *     on another firmware; the request file is removed afterwards.
 *
 * Safety rules followed by every hook: NULL/length checks on every pointer,
 * all string compares/searches are bounded, no heap, no libc, no floats, and
 * on any unexpected shape of the data the original call is continued unchanged.
 *
 * UNTESTED ASSUMPTIONS (no PS TV available to the author):
 *  - The HDMI resolution page is a <list ... key="/CONFIG/DISPLAY/hdmi_resolution_mode">
 *    with self-closing <list_item .../> children (like HENkaku's button_behavior
 *    list). If the list is not found in that shape the page is passed through.
 *  - ScePaf parses the XML buffer synchronously inside scePafMiscLoadXmlLayout
 *    (or keeps using the pointer: our buffer is static, so that is fine too as
 *    long as the same page is patched identically every time, which it is).
 *  - The framework reads the selected value with sceRegMgrGetKeyInt and writes
 *    it with sceRegMgrSetKeyInt (category "/CONFIG/DISPLAY", possibly with a
 *    trailing '/'); HENkaku relies on the same imports for its own keys.
 *  - The framework tolerates a value (N) that is not one of Sony's list values
 *    coming back from GetKeyInt (it simply highlights the matching list_item).
 *  - scePafToplevelGetText returns a pointer the framework only reads.
 *  - The kernel module pstv1080p.skprx is loaded (the syscall stub library is a
 *    hard import; if it is missing the plugin will not load, which is harmless).
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <psp2/types.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/registrymgr.h>
#include <taihen.h>

#include "pstv1080p.h"

/*
 * taihen.h's TAI_CONTINUE casts the continuation to `type(*)()`, which under
 * GCC 15's default C23 mode means "no arguments" and fails to compile. This
 * macro does exactly the same chain walk but with an explicit prototype.
 */
#define PSTV_CONTINUE(fntype, ref, ...) ({ \
    struct _tai_hook_user *cur_ = (struct _tai_hook_user *)(ref); \
    struct _tai_hook_user *next_ = (struct _tai_hook_user *)cur_->next; \
    ((fntype)(next_ ? next_->func : cur_->old))(__VA_ARGS__); \
})

typedef int (*fn_loadxml_t)(int, void *, int, int);
typedef int (*fn_reggetint_t)(const char *, const char *, int *);
typedef int (*fn_regsetint_t)(const char *, const char *, int);
typedef wchar_t *(*fn_gettext_t)(void *, char **);
typedef SceUID (*fn_loadstart_t)(const char *, SceSize, void *, int, SceKernelLMOption *, int *);
typedef int (*fn_stopunload_t)(SceUID, SceSize, void *, int, SceKernelULMOption *, int *);

#define SETTINGS_CORE_PATH   "vs0:app/NPXS10015/system_settings_core.suprx"
#define XML_OUT_SIZE         (64 * 1024)
#define XML_MAX_IN_SIZE      (16 * 1024 * 1024)
#define MAX_LIST_VALUES      32
#define MAX_INDENT           64
#define LOG_LINE_MAX         512

/* NIDs (docs/RESEARCH_NOTES.md sections 2 and 4). */
#define NID_LIB_SCELIBKERNEL     0xCAE9ACE6u
#define NID_SCEKERNELLOADSTARTMODULE 0x2DCC4AFAu
#define NID_SCEKERNELSTOPUNLOADMODULE 0x2415F8A4u
#define NID_LIB_SCEPAFMISC       0x3D643CE8u
#define NID_SCEPAFMISCLOADXMLLAYOUT 0x19FE55A8u
#define NID_LIB_SCEREGMGR        0xC436F916u
#define NID_SCEREGMGRGETKEYINT   0x16DDF3DCu
#define NID_SCEREGMGRSETKEYINT   0xD72EA399u
#define NID_LIB_SCEPAFTOPLEVEL   0x4D9A9DD0u
#define NID_SCEPAFTOPLEVELGETTEXT 0x19CEFDA7u

/* Hook slots. */
enum {
    HOOK_LOADSTART = 0,
    HOOK_STOPUNLOAD,
    HOOK_LOADXML,
    HOOK_REG_GETINT,
    HOOK_REG_SETINT,
    HOOK_GETTEXT,
    HOOK_COUNT
};

static SceUID g_hooks[HOOK_COUNT];
static tai_hook_ref_t g_refs[HOOK_COUNT];
static SceUID g_settings_core_modid = -1;

/* Cached kernel state. */
static pstv1080p_config_t g_cfg;
static int g_cfg_valid;          /* last pstv1080pGetConfig succeeded */
static uint32_t g_item_value = 3; /* N: registry-style value of our list_item */
static uint32_t g_mode_1080p;     /* kernel says the virtual item is selected */

/* Diagnostics. */
static int g_log_dir_done;
static int g_xml_dumped;
static int g_verbose;             /* mirrors the kernel's verbose flag (GetInfo reserved[5]) */
static char g_xml_out[XML_OUT_SIZE];

/* v1.5.0 native value->mode patch state. */
static SceUID g_native_inject = -1;
static uint32_t g_native_value;   /* N the patched ladder maps */
static uint32_t g_native_mode;    /* to this screen-mode code */
static int g_native_failed;       /* signature not found/ambiguous: logged once, not retried */
static void native_patch_apply(const char *why);
static void native_patch_release(void);

/* UTF-16 (-fshort-wchar) title for the injected list item. */
static wchar_t g_text_1080p[] = L"1080p (30 Hz)";

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void ensure_log_dir(void)
{
    if (!g_log_dir_done) {
        g_log_dir_done = 1;
        sceIoMkdir("ux0:data", 0777);
        sceIoMkdir(PSTV1080P_LOG_DIR, 0777);
    }
}

static void pstv_log(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list ap;
    int len;
    SceUID fd;

    if (!fmt)
        return;
    va_start(ap, fmt);
    len = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    if (len > (int)sizeof(line) - 2)
        len = (int)sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = '\0';

    ensure_log_dir();
    fd = sceIoOpen(PSTV1080P_SETTINGS_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, line, (SceSize)len);
    sceIoClose(fd);
}

#define LOG(fmt, ...) pstv_log("[pstv1080p] " fmt, ##__VA_ARGS__)
#define VLOG(fmt, ...) do { if (g_verbose) pstv_log("[pstv1080p] " fmt, ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------- */
/* Kernel config cache                                                       */
/* ------------------------------------------------------------------------- */

static void refresh_config(const char *why)
{
    static int logged;
    static uint32_t last_mode, last_item, last_hd;
    pstv1080p_config_t tmp;
    int ret;

    sceClibMemset(&tmp, 0, sizeof(tmp));
    ret = pstv1080pGetConfig(&tmp);
    if (ret < 0 || tmp.magic != PSTV1080P_CFG_MAGIC) {
        g_cfg_valid = 0;
        g_mode_1080p = 0;
        LOG("GetConfig(%s) failed: 0x%08X magic=0x%08X -> assuming 1080p off, N=%u",
            why ? why : "?", (unsigned)ret, (unsigned)tmp.magic, (unsigned)g_item_value);
        return;
    }
    sceClibMemcpy(&g_cfg, &tmp, sizeof(g_cfg));
    g_cfg_valid = 1;
    g_mode_1080p = g_cfg.mode_1080p ? 1u : 0u;
    if (g_cfg.settings_item_value >= 1 && g_cfg.settings_item_value <= 255)
        g_item_value = g_cfg.settings_item_value;
    /* Quiet: one line per actual change of state (verbose: every fetch). */
    if (g_verbose || !logged || last_mode != g_mode_1080p || last_item != g_item_value || last_hd != g_cfg.hd_mode_code) {
        LOG("config(%s): mode_1080p=%u hd_mode=0x%04X N=%u fps_mode=%u",
            why ? why : "?", (unsigned)g_cfg.mode_1080p, (unsigned)g_cfg.hd_mode_code,
            (unsigned)g_item_value, (unsigned)g_cfg.fps_mode);
        logged = 1;
        last_mode = g_mode_1080p;
        last_item = g_item_value;
        last_hd = g_cfg.hd_mode_code;
    }
}

/* One-shot diagnostics line from the kernel (output mode, refresh rate, hooks). */
static void log_kernel_info(void)
{
    pstv1080p_info_t info;
    int ret;

    sceClibMemset(&info, 0, sizeof(info));
    ret = pstv1080pGetInfo(&info);
    if (ret < 0) {
        LOG("GetInfo failed: 0x%08X", (unsigned)ret);
        return;
    }
    g_verbose = info.reserved[5] ? 1 : 0;
    LOG("GetInfo: version=0x%04X mode_1080p=%u N=%u output_mode=0x%04X refresh=%u Hz "
        "last_system_mode=0x%04X last_apply=0x%08X hooks_ok=0x%08X",
        (unsigned)info.version, (unsigned)info.mode_1080p, (unsigned)info.settings_item_value,
        (unsigned)info.current_output_mode, (unsigned)info.refresh_hz,
        (unsigned)info.last_system_mode, (unsigned)info.last_apply_result, (unsigned)info.hooks_ok);
}

/* Push a new list_item value to the kernel so GetInfo/config apps agree with us. */
static void push_item_value(uint32_t value)
{
    pstv1080p_config_t tmp;
    int ret;

    if (!g_cfg_valid) {
        LOG("push_item_value(%u): no valid kernel config, keeping it local", (unsigned)value);
        g_item_value = value;
        return;
    }
    sceClibMemcpy(&tmp, &g_cfg, sizeof(tmp));
    tmp.settings_item_value = value;
    ret = pstv1080pSetConfig(&tmp);
    LOG("SetConfig(settings_item_value=%u) -> 0x%08X", (unsigned)value, (unsigned)ret);
    g_item_value = value;
    refresh_config("after SetConfig");
    /* The kernel may have rejected it; keep the value we are going to inject. */
    g_item_value = value;
}

/* ------------------------------------------------------------------------- */
/* Tiny bounded string helpers (no libc)                                     */
/* ------------------------------------------------------------------------- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Length of a NUL-terminated string, bounded. */
static int bstrlen(const char *s, int max)
{
    int i = 0;
    if (!s || max <= 0)
        return 0;
    while (i < max && s[i] != '\0')
        i++;
    return i;
}

/* Find needle (nlen bytes) in hay[from, hay_len). Returns offset or -1. */
static int find_bytes(const char *hay, int hay_len, int from, const char *needle, int nlen)
{
    int i, j;
    if (!hay || !needle || nlen <= 0 || from < 0 || hay_len - from < nlen)
        return -1;
    for (i = from; i <= hay_len - nlen; i++) {
        if (hay[i] != needle[0])
            continue;
        for (j = 1; j < nlen; j++) {
            if (hay[i + j] != needle[j])
                break;
        }
        if (j == nlen)
            return i;
    }
    return -1;
}

/* Search backwards from 'from' (inclusive) for "<list" followed by whitespace
 * (i.e. not "<list_item"). Returns offset or -1. */
static int find_list_open_backwards(const char *buf, int len, int from)
{
    int i;
    if (!buf || len < 6 || from < 0)
        return -1;
    if (from > len - 6)
        from = len - 6;
    for (i = from; i >= 0; i--) {
        if (buf[i] == '<' && buf[i + 1] == 'l' && buf[i + 2] == 'i' &&
            buf[i + 3] == 's' && buf[i + 4] == 't' && is_space(buf[i + 5]))
            return i;
    }
    return -1;
}

/* Search forward in buf[from, end) for a nested <list opener: "<list"
 * followed by whitespace, '>' or '/' (anything but the '_' of <list_item).
 * Same predicate family as find_list_open_backwards, so a "<list\n\tkey=..."
 * opener is caught too. Returns offset or -1. */
static int find_list_open_forward(const char *buf, int end, int from)
{
    int i;
    if (!buf || from < 0 || end - from < 6)
        return -1;
    for (i = from; i <= end - 6; i++) {
        if (buf[i] == '<' && buf[i + 1] == 'l' && buf[i + 2] == 'i' &&
            buf[i + 3] == 's' && buf[i + 4] == 't' &&
            (is_space(buf[i + 5]) || buf[i + 5] == '>' || buf[i + 5] == '/'))
            return i;
    }
    return -1;
}

/* Parse an unsigned decimal number from s[0, len). Returns -1 on failure. */
static int parse_uint(const char *s, int len)
{
    int i, v = 0;
    if (!s || len <= 0 || len > 9)
        return -1;
    for (i = 0; i < len; i++) {
        if (!is_digit(s[i]))
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Extract attribute name="..." from tag[0, tag_len) into out (NUL terminated).
 * Returns the attribute length, or -1 if not found. The attribute name must be
 * preceded by whitespace so "value" does not match "xvalue". */
static int get_attr(const char *tag, int tag_len, const char *name, char *out, int out_size)
{
    int nlen = bstrlen(name, 64);
    int pos = 0, p, q, vlen;

    if (!tag || !name || !out || out_size < 2 || nlen <= 0)
        return -1;
    out[0] = '\0';
    for (;;) {
        pos = find_bytes(tag, tag_len, pos, name, nlen);
        if (pos < 0)
            return -1;
        if (pos > 0 && is_space(tag[pos - 1])) {
            p = pos + nlen;
            while (p < tag_len && is_space(tag[p]))
                p++;
            if (p < tag_len && tag[p] == '=') {
                p++;
                while (p < tag_len && is_space(tag[p]))
                    p++;
                if (p < tag_len && (tag[p] == '"' || tag[p] == '\'')) {
                    char quote = tag[p];
                    q = p + 1;
                    while (q < tag_len && tag[q] != quote)
                        q++;
                    if (q >= tag_len)
                        return -1;
                    vlen = q - (p + 1);
                    if (vlen > out_size - 1)
                        vlen = out_size - 1;
                    sceClibMemcpy(out, tag + p + 1, (SceSize)vlen);
                    out[vlen] = '\0';
                    return vlen;
                }
            }
        }
        pos += 1;
    }
}

/* ------------------------------------------------------------------------- */
/* XML page patching                                                         */
/* ------------------------------------------------------------------------- */

static void dump_original_xml(const char *buf, int len)
{
    SceUID fd;
    if (g_xml_dumped)
        return;
    g_xml_dumped = 1;
    ensure_log_dir();
    fd = sceIoOpen(PSTV1080P_SETTINGS_XML_DUMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        LOG("xml dump: open failed 0x%08X", (unsigned)fd);
        return;
    }
    sceIoWrite(fd, buf, (SceSize)len);
    sceIoClose(fd);
    LOG("xml dump: wrote %d bytes to " PSTV1080P_SETTINGS_XML_DUMP, len);
}

static int value_used(const int *values, int count, int v)
{
    int i;
    for (i = 0; i < count; i++)
        if (values[i] == v)
            return 1;
    return 0;
}

/*
 * Try to build a patched copy of the page in g_xml_out.
 * Returns the new size (> 0) on success, or 0 if the original must be used.
 */
static int patch_hdmi_page(const char *buf, int len)
{
    static const char key[] = PSTV1080P_REG_KEY;
    static const char item_open[] = "<list_item";
    static const char list_close[] = "</list>";
    static const char our_id[] = PSTV1080P_XML_ITEM_ID;
    int key_pos, list_pos, tag_end, close_pos, p, q;
    int values[MAX_LIST_VALUES];
    int nvalues = 0, nitems = 0;
    int last_item = -1;
    char indent[MAX_INDENT + 1];
    int indent_len = 0;
    const char *eol = "";
    int ins_pos;
    char item[192];
    int item_len, total;
    uint32_t chosen;
    char id[64], title[64], val[16];
    char seen[160];
    int seen_len = 0;

    key_pos = find_bytes(buf, len, 0, key, (int)sizeof(key) - 1);
    if (key_pos < 0)
        return 0;

    if (find_bytes(buf, len, 0, our_id, (int)sizeof(our_id) - 1) >= 0) {
        LOG("xml: page already contains %s, passing through", our_id);
        return 0;
    }

    list_pos = find_list_open_backwards(buf, len, key_pos);
    if (list_pos < 0) {
        LOG("xml: no <list before key at %d, passing through", key_pos);
        return 0;
    }
    /* The key must be inside the <list ...> start tag: first '>' after it must be past the key. */
    for (tag_end = list_pos; tag_end < len && buf[tag_end] != '>'; tag_end++)
        ;
    if (tag_end >= len || tag_end < key_pos) {
        LOG("xml: key at %d not inside <list at %d (tag ends %d), passing through",
            key_pos, list_pos, tag_end);
        return 0;
    }
    if (tag_end > 0 && buf[tag_end - 1] == '/') {
        LOG("xml: <list at %d is self-closing, passing through", list_pos);
        return 0;
    }
    tag_end++;

    close_pos = find_bytes(buf, len, tag_end, list_close, (int)sizeof(list_close) - 1);
    if (close_pos < 0) {
        LOG("xml: no </list> after %d, passing through", tag_end);
        return 0;
    }
    /* Nested <list ...> inside would confuse us (close_pos would be the inner
     * </list>): bail out. Same opener predicate as find_list_open_backwards. */
    p = find_list_open_forward(buf, close_pos, tag_end);
    if (p >= 0) {
        LOG("xml: nested <list at %d, passing through", p);
        return 0;
    }

    /* Walk the children. */
    p = tag_end;
    for (;;) {
        int ipos = find_bytes(buf, close_pos, p, item_open, (int)sizeof(item_open) - 1);
        int iend, v;
        if (ipos < 0)
            break;
        for (iend = ipos; iend < close_pos && buf[iend] != '>'; iend++)
            ;
        if (iend >= close_pos) {
            LOG("xml: unterminated <list_item at %d, passing through", ipos);
            return 0;
        }
        nitems++;
        last_item = ipos;
        get_attr(buf + ipos, iend - ipos, "id", id, sizeof(id));
        get_attr(buf + ipos, iend - ipos, "title", title, sizeof(title));
        if (get_attr(buf + ipos, iend - ipos, "value", val, sizeof(val)) >= 0)
            v = parse_uint(val, bstrlen(val, sizeof(val)));
        else
            v = -1;
        VLOG("xml: existing list_item id=\"%s\" title=\"%s\" value=%d", id, title, v);
        if (seen_len < (int)sizeof(seen) - 24) {
            int n = sceClibSnprintf(seen + seen_len, sizeof(seen) - (unsigned)seen_len, " %s=%d", title, v);
            if (n > 0)
                seen_len += n;
        }
        if (v >= 0 && nvalues < MAX_LIST_VALUES)
            values[nvalues++] = v;
        p = iend + 1;
    }
    if (nitems == 0) {
        LOG("xml: list has no <list_item children, passing through");
        return 0;
    }
    seen[seen_len] = '\0';
    LOG("xml: Sony's items:%s", seen);

    /* Choose our value. */
    chosen = g_item_value;
    if (chosen < 1 || chosen > 255 || value_used(values, nvalues, (int)chosen)) {
        uint32_t c = 3;
        while (c < 255 && value_used(values, nvalues, (int)c))
            c++;
        LOG("xml: configured value %u unusable, choosing %u", (unsigned)chosen, (unsigned)c);
        chosen = c;
    }
    if (chosen != g_item_value || (g_cfg_valid && g_cfg.settings_item_value != chosen))
        push_item_value(chosen);
    native_patch_apply("page load");   /* (re)patch Sony's ladder for the final N */

    /* Indentation of the last existing item (only if it starts its own line). */
    q = last_item - 1;
    while (q >= tag_end && (buf[q] == ' ' || buf[q] == '\t'))
        q--;
    if (q >= tag_end - 1 && q >= 0 && buf[q] == '\n') {
        indent_len = last_item - (q + 1);
        if (indent_len > MAX_INDENT)
            indent_len = MAX_INDENT;
        sceClibMemcpy(indent, buf + q + 1, (SceSize)indent_len);
        eol = (q > 0 && buf[q - 1] == '\r') ? "\r\n" : "\n";
    }
    indent[indent_len] = '\0';

    /* Insertion point: start of the line holding </list> if it is alone on it. */
    q = close_pos - 1;
    while (q >= tag_end && (buf[q] == ' ' || buf[q] == '\t'))
        q--;
    if (q >= 0 && buf[q] == '\n') {
        ins_pos = q + 1;
    } else {
        ins_pos = close_pos;
        indent[0] = '\0';
        indent_len = 0;
        eol = "";
    }

    item_len = sceClibSnprintf(item, sizeof(item),
                               "%s<list_item id=\"%s\" title=\"%s\" value=\"%u\"/>%s",
                               indent, PSTV1080P_XML_ITEM_ID, PSTV1080P_XML_ITEM_MSG,
                               (unsigned)chosen, eol);
    if (item_len <= 0 || item_len >= (int)sizeof(item)) {
        LOG("xml: item string too long, passing through");
        return 0;
    }

    total = len + item_len;
    if (total > XML_OUT_SIZE) {
        LOG("xml: patched page would be %d bytes > %d, passing through", total, XML_OUT_SIZE);
        return 0;
    }
    sceClibMemcpy(g_xml_out, buf, (SceSize)ins_pos);
    sceClibMemcpy(g_xml_out + ins_pos, item, (SceSize)item_len);
    sceClibMemcpy(g_xml_out + ins_pos + item_len, buf + ins_pos, (SceSize)(len - ins_pos));

    LOG("xml: patched: <list at %d, </list> at %d, %d items, inserted value=%u at %d, size %d -> %d",
        list_pos, close_pos, nitems, (unsigned)chosen, ins_pos, len, total);
    return total;
}

static int scePafMiscLoadXmlLayout_patched(int a1, void *xml_buf, int xml_size, int a4)
{
    const char *buf = (const char *)xml_buf;
    static const char key[] = PSTV1080P_REG_KEY;

    if (buf && xml_size > 0 && xml_size < XML_MAX_IN_SIZE &&
        find_bytes(buf, xml_size, 0, key, (int)sizeof(key) - 1) >= 0) {
        int new_size;
        LOG("xml: HDMI resolution page detected (%d bytes)", xml_size);
        dump_original_xml(buf, xml_size);
        refresh_config("page load");
        new_size = patch_hdmi_page(buf, xml_size);
        if (new_size > 0) {
            xml_buf = g_xml_out;
            xml_size = new_size;
        }
    }
    return PSTV_CONTINUE(fn_loadxml_t, g_refs[HOOK_LOADXML], a1, xml_buf, xml_size, a4);
}

/* ------------------------------------------------------------------------- */
/* Registry hooks                                                            */
/* ------------------------------------------------------------------------- */

static int is_our_key(const char *category, const char *name)
{
    static const char cat[] = PSTV1080P_REG_CATEGORY;
    static const char key[] = PSTV1080P_REG_KEY;
    const int clen = (int)sizeof(cat) - 1;

    if (!category || !name)
        return 0;
    if (sceClibStrncmp(category, cat, (SceSize)clen) != 0)
        return 0;
    if (!(category[clen] == '\0' || (category[clen] == '/' && category[clen + 1] == '\0')))
        return 0;
    return sceClibStrncmp(name, key, sizeof(key)) == 0;
}

static int sceRegMgrGetKeyInt_patched(const char *category, const char *name, int *value)
{
    int ret;

    if (!is_our_key(category, name))
        return PSTV_CONTINUE(fn_reggetint_t, g_refs[HOOK_REG_GETINT], category, name, value);

    refresh_config("GetKeyInt");
    if (g_mode_1080p) {
        if (value)
            *value = (int)g_item_value;
        VLOG("GetKeyInt(%s, %s): 1080p on -> %u", category, name, (unsigned)g_item_value);
        return 0;
    }
    ret = PSTV_CONTINUE(fn_reggetint_t, g_refs[HOOK_REG_GETINT], category, name, value);
    VLOG("GetKeyInt(%s, %s): passthrough ret=0x%08X value=%d",
         category, name, (unsigned)ret, value ? *value : -1);
    return ret;
}

static int sceRegMgrSetKeyInt_patched(const char *category, const char *name, int value)
{
    int ret;

    if (!is_our_key(category, name))
        return PSTV_CONTINUE(fn_regsetint_t, g_refs[HOOK_REG_SETINT], category, name, value);

    refresh_config("SetKeyInt");
    if (value == (int)g_item_value) {
        ret = pstv1080pSetMode1080p(1);
        LOG("SetKeyInt(%s, %s, %d): our item -> SetMode1080p(1) = 0x%08X (registry untouched)",
            category, name, value, (unsigned)ret);
        refresh_config("after SetMode1080p(1)");
        return 0;
    }

    if (g_mode_1080p) {
        ret = pstv1080pSetMode1080p(0);
        LOG("SetKeyInt(%s, %s, %d): leaving 1080p -> SetMode1080p(0) = 0x%08X",
            category, name, value, (unsigned)ret);
        refresh_config("after SetMode1080p(0)");
    }
    ret = PSTV_CONTINUE(fn_regsetint_t, g_refs[HOOK_REG_SETINT], category, name, value);
    LOG("SetKeyInt(%s, %s, %d): passthrough ret=0x%08X", category, name, value, (unsigned)ret);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Native value->mode: patch Sony's ladder in SceSettings, in memory (1.5.0)   */
/* ------------------------------------------------------------------------- */

/*
 * SceSettings (vs0:app/NPXS10015/eboot.bin, FW 3.60, module NID 0xC2A86F54)
 * at 0x81125102 in the dump of 2026-09-11 (docs/reversing/settings_value_to_mode.txt):
 *
 *   push {r4,r5,r6,lr}; mov r4,r0; ldr r3,[r4,#8]   ; r3 = list value
 *   movs r1,#0                                       ; "known" flag
 *   cmp r3,#1; mov.w r0,#0x10000000; mov.w r2,#1
 *   bgt .Lhi; cmp r3,#0; ble .Lcall; b .L1080i
 *   .Lhi: cmp r3,#2; ble .L720p; cmp r3,#3; ble .L480p; b .Lcall
 *   .L1080i: movs.w r0,#0x8500; mov r1,r2; b .Lcall
 *   .L720p:  movs.w r0,#0x8600; mov r1,r2; b .Lcall
 *   .L480p:  movs.w r0,#0x8300; mov r1,r2
 *   .Lcall:  blx sceAVConfigHdmiSetResolution(r0=mode, r1=known, r2=1)
 *            ... then core registry object ->SetKeyInt(key, value)
 *
 * The 52 bytes from "movs r1,#0" to ".Lcall" are replaced by (same size,
 * same registers, same flag semantics, one more case):
 *
 *   movs r1,#1; movs r2,#1; mov.w r0,#0x10000000
 *   cmp r3,#1; beq .L1080i; cmp r3,#2; beq .L720p
 *   cmp r3,#3; beq .L480p;  cmp r3,#N; beq .Lours
 *   movs r1,#0; b .Lcall                         ; anything else: automatic
 *   .L1080i: movs.w r0,#0x8500; b .Lcall
 *   .L720p:  movs.w r0,#0x8600; b .Lcall
 *   .L480p:  movs.w r0,#0x8300; b .Lcall
 *   .Lours:  movw r0,#hd_mode_code; nop          ; falls into .Lcall
 *
 * Both listings were assembled with the vitasdk toolchain and diffed against
 * the dump before release (scratch test in the repo history).
 */
#define LADDER_SIG_LEN    58
#define LADDER_PATCH_OFF  6       /* patch starts after push/mov/ldr */
#define LADDER_PATCH_LEN  52
#define LADDER_VALUE_OFF  20      /* imm8 of "cmp r3,#N" inside the patch */
#define LADDER_MOVW_OFF   46      /* "movw r0,#mode" inside the patch */

static const unsigned char k_ladder_sig[LADDER_SIG_LEN] = {
    0x70,0xb5, 0x04,0x1c, 0xa3,0x68, 0x00,0x21, 0x01,0x2b, 0x4f,0xf0,0x80,0x50,
    0x4f,0xf0,0x01,0x02, 0x02,0xdc, 0x00,0x2b, 0x10,0xdd, 0x04,0xe0, 0x02,0x2b,
    0x06,0xdd, 0x03,0x2b, 0x08,0xdd, 0x0a,0xe0, 0x5f,0xf4,0x05,0x40, 0x11,0x1c,
    0x06,0xe0, 0x5f,0xf4,0x06,0x40, 0x11,0x1c, 0x02,0xe0, 0x5f,0xf4,0x03,0x40,
    0x11,0x1c
};

static const unsigned char k_ladder_patch[LADDER_PATCH_LEN] = {
    0x01,0x21,              /* movs r1,#1            */
    0x01,0x22,              /* movs r2,#1            */
    0x4f,0xf0,0x80,0x50,    /* mov.w r0,#0x10000000  */
    0x01,0x2b, 0x07,0xd0,   /* cmp r3,#1; beq +0x1c  */
    0x02,0x2b, 0x08,0xd0,   /* cmp r3,#2; beq +0x22  */
    0x03,0x2b, 0x09,0xd0,   /* cmp r3,#3; beq +0x28  */
    0x04,0x2b, 0x0a,0xd0,   /* cmp r3,#N; beq +0x2e  (N patched in)  */
    0x00,0x21,              /* movs r1,#0            */
    0x0b,0xe0,              /* b .Lcall              */
    0x5f,0xf4,0x05,0x40,    /* movs.w r0,#0x8500     */
    0x08,0xe0,              /* b .Lcall              */
    0x5f,0xf4,0x06,0x40,    /* movs.w r0,#0x8600     */
    0x05,0xe0,              /* b .Lcall              */
    0x5f,0xf4,0x03,0x40,    /* movs.w r0,#0x8300     */
    0x02,0xe0,              /* b .Lcall              */
    0x48,0xf2,0x10,0x70,    /* movw r0,#0x8710       (mode patched in) */
    0x00,0xbf               /* nop                   */
};

/* Thumb-2 MOVW r0,#imm16 (T3 encoding), little-endian halfwords. */
static void enc_movw_r0(unsigned char *p, uint32_t imm16)
{
    uint32_t imm4 = (imm16 >> 12) & 0xFu, i = (imm16 >> 11) & 1u;
    uint32_t imm3 = (imm16 >> 8) & 7u, imm8 = imm16 & 0xFFu;
    uint32_t hw1 = 0xF240u | (i << 10) | imm4;
    uint32_t hw2 = (imm3 << 12) | imm8;          /* Rd = r0 */
    p[0] = (unsigned char)(hw1 & 0xFF); p[1] = (unsigned char)(hw1 >> 8);
    p[2] = (unsigned char)(hw2 & 0xFF); p[3] = (unsigned char)(hw2 >> 8);
}

/* Count occurrences of sig in [base, base+size); stop counting at 2. */
static int count_sig(const unsigned char *base, SceSize size, const unsigned char *sig, int siglen, SceSize *first)
{
    SceSize i, j;
    int n = 0;
    if (!base || size < (SceSize)siglen)
        return 0;
    for (i = 0; i + (SceSize)siglen <= size; i += 2) {   /* Thumb code: halfword aligned */
        if (base[i] != sig[0])
            continue;
        for (j = 1; j < (SceSize)siglen; j++)
            if (base[i + j] != sig[j])
                break;
        if (j == (SceSize)siglen) {
            if (n == 0)
                *first = i;
            if (++n >= 2)
                break;
        }
    }
    return n;
}

static void native_patch_release(void)
{
    if (g_native_inject >= 0) {
        int r = taiInjectRelease(g_native_inject);
        VLOG("native: inject 0x%08X released (0x%08X)", (unsigned)g_native_inject, (unsigned)r);
        g_native_inject = -1;
    }
}

static void native_patch_apply(const char *why)
{
    tai_module_info_t tinfo;
    SceKernelModuleInfo minfo;
    unsigned char patch[LADDER_PATCH_LEN];
    const unsigned char *text;
    SceSize text_size, off = 0;
    uint32_t value = g_item_value;
    uint32_t mode = (g_cfg_valid && g_cfg.hd_mode_code != 0) ? g_cfg.hd_mode_code : PSTV1080P_MODE_1080P30;
    int n, r;

    if (g_native_failed)
        return;
    if (value < 1 || value > 255 || mode > 0xFFFFu) {
        LOG("native: value %u / mode 0x%X not encodable, ladder left alone", (unsigned)value, (unsigned)mode);
        return;
    }
    if (g_native_inject >= 0 && g_native_value == value && g_native_mode == mode)
        return;                                   /* already patched for these */

    sceClibMemset(&tinfo, 0, sizeof(tinfo));
    tinfo.size = sizeof(tinfo);
    r = taiGetModuleInfo("SceSettings", &tinfo);
    if (r < 0) {
        LOG("native: taiGetModuleInfo(SceSettings) failed 0x%08X", (unsigned)r);
        return;
    }
    sceClibMemset(&minfo, 0, sizeof(minfo));
    minfo.size = sizeof(minfo);
    r = sceKernelGetModuleInfo(tinfo.modid, &minfo);
    if (r < 0) {
        LOG("native: sceKernelGetModuleInfo(0x%08X) failed 0x%08X", (unsigned)tinfo.modid, (unsigned)r);
        return;
    }
    text = (const unsigned char *)minfo.segments[0].vaddr;
    text_size = minfo.segments[0].memsz;

    n = count_sig(text, text_size, k_ladder_sig, LADDER_SIG_LEN, &off);
    if (n != 1) {
        g_native_failed = 1;
        LOG("native: value->mode ladder %s in SceSettings (module nid 0x%08X, text 0x%08X+0x%X): "
            "Sony's code keeps mapping value %u to automatic, the kernel hook substitutes as before",
            n == 0 ? "NOT FOUND" : "AMBIGUOUS", (unsigned)tinfo.module_nid,
            (unsigned)(uintptr_t)text, (unsigned)text_size, (unsigned)value);
        return;
    }

    native_patch_release();

    sceClibMemcpy(patch, k_ladder_patch, sizeof(patch));
    patch[LADDER_VALUE_OFF] = (unsigned char)value;
    enc_movw_r0(patch + LADDER_MOVW_OFF, mode);

    g_native_inject = taiInjectData(tinfo.modid, 0, (uint32_t)(off + LADDER_PATCH_OFF), patch, sizeof(patch));
    if (g_native_inject < 0) {
        LOG("native(%s): taiInjectData at SceSettings+0x%X failed 0x%08X; kernel hook substitutes as before",
            why, (unsigned)(off + LADDER_PATCH_OFF), (unsigned)g_native_inject);
        return;
    }
    g_native_value = value;
    g_native_mode = mode;
    LOG("native(%s): Sony's ladder at 0x%08X patched in memory: value %u -> 0x%04X (inject 0x%08X)",
        why, (unsigned)(uintptr_t)(text + off), (unsigned)value, (unsigned)mode, (unsigned)g_native_inject);
}

/* ------------------------------------------------------------------------- */
/* scePafToplevelGetText                                                     */
/* ------------------------------------------------------------------------- */

static wchar_t *scePafToplevelGetText_patched(void *arg, char **msg)
{
    static const char our_msg[] = PSTV1080P_XML_ITEM_MSG;

    if (msg && *msg && sceClibStrncmp(*msg, our_msg, sizeof(our_msg)) == 0)
        return g_text_1080p;
    return PSTV_CONTINUE(fn_gettext_t, g_refs[HOOK_GETTEXT], arg, msg);
}

/* ------------------------------------------------------------------------- */
/* Module load/unload tracking                                               */
/* ------------------------------------------------------------------------- */

static void install_hook(int slot, const char *module, uint32_t lib, uint32_t func,
                         const void *hook, const char *what)
{
    if (g_hooks[slot] >= 0) {
        LOG("hook %s already installed (0x%08X)", what, (unsigned)g_hooks[slot]);
        return;
    }
    g_hooks[slot] = taiHookFunctionImport(&g_refs[slot], module, lib, func, hook);
    if (g_hooks[slot] < 0 || g_verbose)
        LOG("hook %s in %s: 0x%08X", what, module, (unsigned)g_hooks[slot]);
}

static void release_hook(int slot, const char *what)
{
    if (g_hooks[slot] >= 0) {
        int ret = taiHookRelease(g_hooks[slot], g_refs[slot]);
        if (ret < 0 || g_verbose)
            LOG("release %s: 0x%08X", what, (unsigned)ret);
        g_hooks[slot] = -1;
        g_refs[slot] = 0;
    }
}

static void install_page_hooks(void)
{
    install_hook(HOOK_LOADXML, "SceSettings", NID_LIB_SCEPAFMISC, NID_SCEPAFMISCLOADXMLLAYOUT,
                 scePafMiscLoadXmlLayout_patched, "scePafMiscLoadXmlLayout");
    install_hook(HOOK_REG_GETINT, "SceSystemSettingsCore", NID_LIB_SCEREGMGR, NID_SCEREGMGRGETKEYINT,
                 sceRegMgrGetKeyInt_patched, "sceRegMgrGetKeyInt");
    install_hook(HOOK_REG_SETINT, "SceSystemSettingsCore", NID_LIB_SCEREGMGR, NID_SCEREGMGRSETKEYINT,
                 sceRegMgrSetKeyInt_patched, "sceRegMgrSetKeyInt");
    install_hook(HOOK_GETTEXT, "SceSystemSettingsCore", NID_LIB_SCEPAFTOPLEVEL, NID_SCEPAFTOPLEVELGETTEXT,
                 scePafToplevelGetText_patched, "scePafToplevelGetText");
    {
        int ok = 0, i;
        for (i = HOOK_LOADXML; i <= HOOK_GETTEXT; i++)
            if (g_hooks[i] >= 0)
                ok++;
        LOG("page hooks: %d/4 installed (core modid 0x%08X)", ok, (unsigned)g_settings_core_modid);
    }
}

static void release_page_hooks(void)
{
    release_hook(HOOK_LOADXML, "scePafMiscLoadXmlLayout");
    release_hook(HOOK_REG_GETINT, "sceRegMgrGetKeyInt");
    release_hook(HOOK_REG_SETINT, "sceRegMgrSetKeyInt");
    release_hook(HOOK_GETTEXT, "scePafToplevelGetText");
}

/* Module dump (1.4.8, on request since 1.5.0): writes the segments of the
 * given module, as mapped in this process, to
 * ux0:data/pstv1080p/dump_<tag>_seg<i>.bin plus a .txt with the layout, so
 * the value->mode ladder can be located on another firmware. */
static void dump_module(SceUID modid, const char *tag)
{
    SceKernelModuleInfo info;
    tai_module_info_t tinfo;
    char path[128];
    int i, r;
    SceUID fd;

    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    r = sceKernelGetModuleInfo(modid, &info);
    if (r < 0) {
        LOG("dump: sceKernelGetModuleInfo(0x%08X) failed 0x%08X", (unsigned)modid, (unsigned)r);
        return;
    }
    sceClibMemset(&tinfo, 0, sizeof(tinfo));
    tinfo.size = sizeof(tinfo);
    taiGetModuleInfo(info.module_name, &tinfo);

    sceClibSnprintf(path, sizeof(path), PSTV1080P_LOG_DIR "/dump_%s.txt", tag);
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
    if (fd >= 0) {
        char line[256];
        int n = sceClibSnprintf(line, sizeof(line), "module=%s path=%s modid=0x%08X nid=0x%08X ver=%u.%u\n",
                                info.module_name, info.path, (unsigned)modid, (unsigned)tinfo.module_nid,
                                info.modver[0], info.modver[1]);
        sceIoWrite(fd, line, (SceSize)n);
        for (i = 0; i < 4; i++) {
            n = sceClibSnprintf(line, sizeof(line), "seg%d vaddr=0x%08X memsz=0x%08X filesz=0x%08X perms=0x%X\n",
                                i, (unsigned)(uintptr_t)info.segments[i].vaddr, (unsigned)info.segments[i].memsz,
                                (unsigned)info.segments[i].filesz, (unsigned)info.segments[i].perms);
            sceIoWrite(fd, line, (SceSize)n);
        }
        sceIoClose(fd);
    }

    for (i = 0; i < 4; i++) {
        const char *base = (const char *)info.segments[i].vaddr;
        SceSize left = info.segments[i].memsz, off = 0;
        if (!base || left == 0)
            continue;
        sceClibSnprintf(path, sizeof(path), PSTV1080P_LOG_DIR "/dump_%s_seg%d.bin", tag, i);
        fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
        if (fd < 0) {
            LOG("dump: cannot create %s (0x%08X)", path, (unsigned)fd);
            continue;
        }
        while (left > 0) {
            SceSize chunk = left > 0x10000 ? 0x10000 : left;
            int w = sceIoWrite(fd, base + off, chunk);
            if (w <= 0)
                break;
            off += (SceSize)w;
            left -= (SceSize)w;
        }
        sceIoClose(fd);
        LOG("dump: %s seg%d vaddr=0x%08X size=0x%08X -> %s (%u bytes written)", info.module_name, i,
            (unsigned)(uintptr_t)base, (unsigned)info.segments[i].memsz, path, (unsigned)off);
    }
}

static void dump_settings_modules_on_request(void)
{
    SceIoStat st;
    tai_module_info_t tinfo;

    if (sceIoGetstat(PSTV1080P_DUMP_REQUEST, &st) < 0)
        return;                                   /* nobody asked */
    sceIoRemove(PSTV1080P_DUMP_REQUEST);
    ensure_log_dir();
    LOG("dump: " PSTV1080P_DUMP_REQUEST " found, writing the Settings modules (request file removed)");
    if (g_settings_core_modid >= 0)
        dump_module(g_settings_core_modid, "core");
    sceClibMemset(&tinfo, 0, sizeof(tinfo));
    tinfo.size = sizeof(tinfo);
    if (taiGetModuleInfo("SceSettings", &tinfo) >= 0)
        dump_module(tinfo.modid, "settings");
}

static SceUID sceKernelLoadStartModule_patched(const char *path, SceSize args, void *argp,
                                               int flags, SceKernelLMOption *option, int *status)
{
    static const char core[] = SETTINGS_CORE_PATH;
    SceUID ret = PSTV_CONTINUE(fn_loadstart_t, g_refs[HOOK_LOADSTART], path, args, argp, flags, option, status);

    if (ret >= 0 && path && sceClibStrncmp(path, core, sizeof(core)) == 0) {
        VLOG("system_settings_core.suprx loaded (modid 0x%08X), installing page hooks", (unsigned)ret);
        g_settings_core_modid = ret;
        install_page_hooks();
        native_patch_apply("core load");          /* no-op if module_start already did it */
        dump_settings_modules_on_request();
    }
    return ret;
}

static int sceKernelStopUnloadModule_patched(SceUID modid, SceSize args, void *argp, int flags,
                                             SceKernelULMOption *option, int *status)
{
    if (modid >= 0 && modid == g_settings_core_modid) {
        VLOG("system_settings_core.suprx unloading, releasing page hooks");
        g_settings_core_modid = -1;
        release_page_hooks();
    }
    return PSTV_CONTINUE(fn_stopunload_t, g_refs[HOOK_STOPUNLOAD], modid, args, argp, flags, option, status);
}

/* ------------------------------------------------------------------------- */
/* Module entry points                                                       */
/* ------------------------------------------------------------------------- */

#ifdef __vita__
/* Silences the linker's "cannot find entry symbol _start" (module entry points come from the .yml). */
int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
#endif

int module_start(SceSize argc, const void *args)
{
    int i;
    (void)argc;
    (void)args;

    for (i = 0; i < HOOK_COUNT; i++) {
        g_hooks[i] = -1;
        g_refs[i] = 0;
    }
    LOG("module_start (version " PSTV1080P_VERSION_STR ")");
    log_kernel_info();                 /* also learns the verbose flag */
    refresh_config("module_start");

    install_hook(HOOK_LOADSTART, "SceSettings", NID_LIB_SCELIBKERNEL, NID_SCEKERNELLOADSTARTMODULE,
                 sceKernelLoadStartModule_patched, "sceKernelLoadStartModule");
    install_hook(HOOK_STOPUNLOAD, "SceSettings", NID_LIB_SCELIBKERNEL, NID_SCEKERNELSTOPUNLOADMODULE,
                 sceKernelStopUnloadModule_patched, "sceKernelStopUnloadModule");

    /* SceSettings is already mapped when taiHEN starts us: patch its ladder now. */
    native_patch_apply("module_start");
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    LOG("module_stop");
    native_patch_release();
    release_page_hooks();
    release_hook(HOOK_STOPUNLOAD, "sceKernelStopUnloadModule");
    release_hook(HOOK_LOADSTART, "sceKernelLoadStartModule");
    return SCE_KERNEL_STOP_SUCCESS;
}
