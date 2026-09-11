/*
 * pstv1080p — native 1080p (30 Hz) HDMI output option for PlayStation TV
 * Kernel module (pstv1080p.skprx), loaded under *KERNEL by taiHEN.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the pstv1080p contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  It is distributed WITHOUT ANY WARRANTY; see
 * <https://www.gnu.org/licenses/> for details.
 *
 * ---------------------------------------------------------------------------
 * DESIGN (see docs/DESIGN.md section A — this file implements it verbatim)
 * ---------------------------------------------------------------------------
 *
 *  1. Persistent state lives in ur0:tai/pstv1080p.cfg (pstv1080p_config_t,
 *     64 bytes, magic 'P18P').  Missing / invalid file -> defaults with the
 *     1080p mode OFF.  Every change is written back immediately.
 *
 *  2. One hook on the SceAVConfig user-library export
 *     sceAVConfigHdmiSetResolution(int mode) (lib 0x79E0F03F, func
 *     0x4D37F036).  Everything that changes the HDMI mode on the PS TV goes
 *     through it (Settings app, SceShell at boot, HDMI hot-plug).  The hook
 *     remembers what Sony's code asked for (g_last_system_mode - only codes
 *     that pass mode_code_plausible() and are not our own hd_mode_code, since
 *     that value is what the revert path hands back to SetResolution) and,
 *     while the virtual "1080p" option is selected, substitutes
 *     cfg.hd_mode_code (0x8710 = 1080p30 by default).  After every successful
 *     call it records the applied code and what ksceDisplayGetOutputMode
 *     reports for it (g_applied_mode / g_applied_readback).
 *
 *  3. Boot-time apply (v1.1): a low-priority kernel thread waits for SceShell
 *     + boot_apply_delay_ms and then SCHEDULES an attempt.  The attempt is
 *     executed from SceShell's next _sceDisplaySetFrameBuf syscall (a user
 *     process context, like the Settings-app path that is known to work on
 *     hardware); if no shell frame picks it up within 10 s the thread runs it
 *     itself.  An attempt counts as EFFECTIVE only if the driver readback
 *     actually changed (to hd_mode_code, or to a new value that becomes the
 *     "alias").  Ineffective attempts are retried with backoff (3,5,8,12,20,
 *     30,45,60 s...), at most 10 per drift episode and 30 per boot session.
 *     The same thread removes the safe-boot marker once the console survived
 *     safe_boot_seconds and acts as a watchdog that starts a new (bounded)
 *     episode if the driver later reports a non-HD mode.  Rate-limited to one
 *     attempt per 5 s, so it can never become a periodic HDMI renegotiation.
 *
 *  4. Safe boot: at module_start, if 1080p is enabled and the marker file
 *     ur0:tai/pstv1080p.boot still exists, the previous boot in 1080p did not
 *     survive safe_boot_seconds (black screen -> user pulled the plug), so
 *     the option is switched off and persisted before anything is applied.
 *
 *  5. Refresh-rate-aware frame pacing: hooks on the SceDisplay USER library
 *     (0x5ED8F994) syscall implementations for the WaitVblankStart(Multi)(CB),
 *     WaitSetFrameBufMulti(CB) family and _sceDisplaySetFrameBuf.  They run in the calling process'
 *     context.  The hot path only reads cached integers (refresh rate, config,
 *     shell pid, an 8-entry pid->allowed cache) — no I/O, no allocation, no
 *     floating point.
 *       SCALE (default): vcount' = max(1, vcount * refresh_hz / 60).  Identity
 *                        at 60 Hz, so the hooks are no-ops while 1080p is off.
 *       FORCE:           interval = max(1, refresh_hz / fps_target), applied
 *                        Framecapper-style (optionally also after SetFrameBuf).
 *
 *  6. Four syscall exports (library "pstv1080p") used by the Settings-app
 *     plugin: Get/SetConfig, SetMode1080p, GetInfo.
 *
 *  7. Best-effort logging to ux0:data/pstv1080p/kernel.log (never from the
 *     frame-pacing hot path; every failure is ignored).
 *
 * ---------------------------------------------------------------------------
 * HARDWARE RESULTS (PS TV, 2026-09-11) and remaining assumptions
 * ---------------------------------------------------------------------------
 *  Confirmed: A1 (0x8710 works), A3 (GetOutputMode reports 0x8710 after the
 *  apply, 0x8300/0x8600 for 480p/720p), A4 (all SceDisplay export hooks
 *  install), A7, A9.  sceAVConfigHdmiSetResolution returns 0x80010058
 *  (ENOSYS) when called from a kernel thread and works from any user-process
 *  syscall context; the thread fallback below is therefore expected to fail
 *  and only exists as a last resort.  Sony's Settings core calls
 *  SetResolution(<code>) BEFORE it writes hdmi_resolution_mode; for a value
 *  it does not know (ours) it sends 0x10000000, which the driver treats as
 *  "automatic" (720p on the test TV); our SetMode1080p(1) then switches to
 *  0x8710 right after, so selecting the item shows a brief 720p flash.
 *  Registry values on FW 3.60: 0 automatic, 1 1080i, 2 720p, 3 480p.
 *  v1.2 adds the adaptive inject (fps_inject = 1): games without any vsync
 *  wait were unpaced at 30 Hz (flicker / wrong rate reported by the user),
 *  which Framecapper's Inject build used to hide at the price of double-
 *  waiting games that do sync.
 *
 *  A1. sceAVConfigHdmiSetResolution(0x8710) really switches the HDMI path to
 *      1080p30 (gameblabla's plugin relies on exactly this; the user's own
 *      480p->1080p remap binary confirms the NID/library).
 *  A2. Screen-mode flag bits 0xF0 encode the refresh rate as documented on
 *      wiki.henkaku.xyz (0x10 = 30 Hz).  ksceDisplayGetRefreshRateInternal is
 *      used only as a logged cross-check, never as the source of truth.
 *  A3. ksceDisplayGetOutputMode(1, ...) reports the mode previously set via
 *      sceAVConfigHdmiSetResolution (used to decide whether an apply took
 *      effect and to detect drift).  If it uses a different encoding, the
 *      readback that appears when an apply visibly changes the driver state
 *      is learned as the alias of hd_mode_code (refresh rate then derived
 *      from the code we applied).  A readback that does not change at all
 *      means the apply did NOT work and is retried (v1.0 wrongly latched it).
 *  A9. Hardware test 1 (v1.0): Settings-path apply works; the boot-time apply
 *      from the kernel worker thread left the driver at 480p.  Whether that
 *      is the calling context (no user process) or timing is unknown; v1.1
 *      does the attempt from SceShell's SetFrameBuf syscall and retries with
 *      backoff, which covers both explanations.  kernel.log shows which.
 *  A8. _sceDisplaySetFrameBuf (0xF51523CB) is hooked unconditionally at
 *      module_start (DESIGN A.3 lists it as "FORCE+inject only"); the hook
 *      passes straight through unless fps_mode == FORCE && fps_inject.  This
 *      avoids installing/releasing a hook at runtime while games are calling
 *      it.  It shows up as a 7th "hook: ... ok" line and hooks_ok bit 7.
 *  A4. taiHookFunctionExportForKernel works on the SceDisplay user-library
 *      syscall exports (as it does for SceAVConfig).  Hooks that fail to
 *      install are logged, their hooks_ok bit stays clear, and everything
 *      else keeps working.
 *  A5. Calling ksceDisplayWaitVblankStartMulti(CB) from inside a hook that
 *      runs in the caller's syscall context is legal (it is what the original
 *      export does internally).
 *  A6. The SceAVConfig hook may be reached from a kernel worker thread on
 *      HDMI hot-plug; file logging from there is assumed to be allowed (it is
 *      a normal thread context, not an interrupt handler).
 *  A7. ur0: is mounted when *KERNEL plugins start (state file), ux0: may not
 *      be (log directory creation is retried on every log call until it
 *      succeeds).
 */

#include <stdint.h>
#include <stdarg.h>

#include <taihen.h>
#include <psp2kern/types.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/display.h>

#include "pstv1080p.h"

/* ------------------------------------------------------------------------- */
/* Prototypes the vitasdk headers do not provide (all verified in stub libs)  */
/* ------------------------------------------------------------------------- */

/* libSceDisplayForDriver_stub.a */
int ksceDisplayGetOutputMode(int head, unsigned int *pScreenMode, unsigned int *pPixelFormat);
int ksceDisplayGetRefreshRateInternal(int head, float *pFps, int *pScanMode);
/* libtaihenModuleUtils_stub.a */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

/* taihen 0.11's TAI_CONTINUE casts the next function to `type(*)()`, which
 * under C23 (GCC 15 default) is a zero-argument prototype and fails to
 * compile with arguments.  HOOK_NEXT does the same chain walk but casts to
 * the prototype of the hook function itself, so the ABI is always right. */
#define HOOK_NEXT(this_fn, hook, ...) ({ \
    struct _tai_hook_user *_cur = (struct _tai_hook_user *)(hook); \
    struct _tai_hook_user *_next = (struct _tai_hook_user *)_cur->next; \
    (_next == NULL) \
        ? ((__typeof__(&this_fn))_cur->old)(__VA_ARGS__) \
        : ((__typeof__(&this_fn))_next->func)(__VA_ARGS__); \
})

/* ------------------------------------------------------------------------- */
/* Constants                                                                  */
/* ------------------------------------------------------------------------- */

#define HDMI_HEAD                     1

#define AVCONFIG_MODULE               "SceAVConfig"
#define AVCONFIG_LIB_NID              0x79E0F03Fu
#define AVCONFIG_SETRES_NID           0x4D37F036u

#define DISPLAY_MODULE                "SceDisplay"
#define DISPLAY_USER_LIB_NID          0x5ED8F994u
#define NID_WAITVBLANKSTARTMULTI      0xDD0A13B8u
#define NID_WAITVBLANKSTARTMULTICB    0x05F27764u
#define NID_WAITVBLANKSTART           0x5795E898u
#define NID_WAITVBLANKSTARTCB         0x78B41B92u
#define NID_WAITSETFRAMEBUFMULTI      0x7D9864A8u
#define NID_WAITSETFRAMEBUFMULTICB    0x3E796EF5u
#define NID_SETFRAMEBUF               0xF51523CBu
#define NID_WAITSETFRAMEBUF           0x9423560Cu   /* v1.2: sync-activity tracking only */
#define NID_WAITSETFRAMEBUFCB         0x814C90AFu
#define NID_GETVCOUNT                 0xB6FDE0BAu
#define NID_GETVCOUNTINTERNAL         0x9686859Eu
#define NID_REGISTERVBLANKCB          0x6BDF4C4Du

/* hooks_ok bitmask reported by pstv1080pGetInfo */
#define HOOK_BIT_AVCONFIG             (1u << 0)
#define HOOK_BIT_WAITVBLANKMULTI      (1u << 1)
#define HOOK_BIT_WAITVBLANKMULTICB    (1u << 2)
#define HOOK_BIT_WAITVBLANK           (1u << 3)
#define HOOK_BIT_WAITVBLANKCB         (1u << 4)
#define HOOK_BIT_WAITSETFBMULTI       (1u << 5)
#define HOOK_BIT_WAITSETFBMULTICB     (1u << 6)
#define HOOK_BIT_SETFRAMEBUF          (1u << 7)
#define HOOK_BIT_WAITSETFB            (1u << 8)
#define HOOK_BIT_WAITSETFBCB          (1u << 9)
#define HOOK_BIT_GETVCOUNT            (1u << 10)
#define HOOK_BIT_GETVCOUNTINT         (1u << 11)
#define HOOK_BIT_REGVBLANKCB          (1u << 12)

#define SCE_ERRNO_EEXIST              ((int)0x80010011)

#define APPLY_MAX_ATTEMPTS_EPISODE    10             /* attempts per drift episode */
#define APPLY_MAX_TOTAL_SESSION       30             /* hard cap per boot session */
#define APPLY_MIN_INTERVAL_US         (5u * 1000u * 1000u)
#define APPLY_SHELL_FALLBACK_US       (10u * 1000u * 1000u) /* no SceShell frame took the job -> thread does it */
#define SHELL_WAIT_POLL_US            (500u * 1000u)
#define SHELL_WAIT_MAX_POLLS          120            /* 60 s */
#define THREAD_TICK_US                (250u * 1000u)
#define WATCHDOG_MIN_PERIOD_MS        500u
#define BOOT_DELAY_MAX_MS             60000u
#define SAFE_BOOT_MAX_SECONDS         3600u

#define PID_CACHE_ENTRIES             8
#define TITLE_LIST_MAX                32
#define TITLE_ID_LEN                  16

/* ------------------------------------------------------------------------- */
/* Globals                                                                    */
/* ------------------------------------------------------------------------- */

static pstv1080p_config_t g_cfg;
static SceUID g_mutex = -1;

/* SceAVConfig hook */
static SceUID g_avconfig_uid = -1;
static tai_hook_ref_t g_avconfig_ref;
typedef int (*avconfig_setres_fn)(int mode);
static avconfig_setres_fn g_avconfig_fn = 0;

/* Frame-pacing hooks (index order == HOOK_BIT order minus one) */
enum {
    PH_WAITVBLANKMULTI = 0,
    PH_WAITVBLANKMULTICB,
    PH_WAITVBLANK,
    PH_WAITVBLANKCB,
    PH_WAITSETFBMULTI,
    PH_WAITSETFBMULTICB,
    PH_SETFRAMEBUF,
    PH_WAITSETFB,
    PH_WAITSETFBCB,
    PH_GETVCOUNT,
    PH_GETVCOUNTINT,
    PH_REGVBLANKCB,
    PH_COUNT
};
static SceUID g_pacing_uid[PH_COUNT];
static tai_hook_ref_t g_pacing_ref[PH_COUNT];

static volatile uint32_t g_hooks_ok = 0;

/* Cached display state (read from the hot path; written by thread/hook/syscalls) */
static volatile uint32_t g_refresh_hz = 60;
static volatile uint32_t g_current_output_mode = 0;
static volatile uint32_t g_last_system_mode = 0;   /* what Sony asked for (plausible, never our own code) */
static volatile uint32_t g_applied_mode = 0;       /* last mode a successful SetResolution applied (anyone's call) */
static volatile uint32_t g_applied_readback = 0;   /* what GetOutputMode reported right after that apply, 0 = unknown */
static volatile uint32_t g_hd_alias = 0;           /* driver's own readback for hd_mode_code, learned only when an apply
                                                    * visibly changed the readback to something other than our code */
static volatile int32_t  g_last_apply_result = 0;  /* result of the last call WE issued */
static volatile int32_t  g_last_setres_ret = 0;    /* result of the last call anyone issued */
static volatile int      g_self_apply = 0;         /* set while we call the export ourselves */

/* Process filter */
static volatile SceUID g_shell_pid = 0;

/* pid -> allowed cache for FORCE mode title filtering */
typedef struct {
    volatile SceUID pid;
    volatile uint32_t allowed;
} pid_cache_entry_t;
static pid_cache_entry_t g_pid_cache[PID_CACHE_ENTRIES];
static volatile uint32_t g_pid_cache_next = 0;

/* v1.2 adaptive inject: per-process record of the last vsync activity
 * (any SceDisplay wait entry/exit or vcount read) and whether the process
 * registered a vblank callback (then it syncs by itself and is never injected). */
#define VS_TRACK_ENTRIES              8
#define VS_IDLE_PERIODS_X2            9              /* inject if no sync activity for > 4.5 refresh periods */
typedef struct {
    volatile SceUID pid;
    volatile SceInt64 last_sync_us;
    volatile uint32_t cb_synced;
} vs_entry_t;
static vs_entry_t g_vs[VS_TRACK_ENTRIES];
static volatile uint32_t g_vs_next = 0;

/* Title list (ur0:tai/pstv1080p_titles.txt) */
static volatile uint32_t g_title_filter_active = 0;  /* 1 = only listed titles are paced in FORCE mode */
static uint32_t g_title_count = 0;
static char g_titles[TITLE_LIST_MAX][TITLE_ID_LEN];

/* Thread / watchdog */
static SceUID g_thread_uid = -1;
static volatile int g_thread_stop = 0;
static uint32_t g_apply_attempts = 0;              /* attempts in the current drift episode */
static uint32_t g_apply_total = 0;                 /* attempts this boot session */
static volatile SceInt64 g_apply_due_us = 0;       /* earliest time the scheduled attempt may run */
static volatile int g_apply_pending = 0;           /* an attempt is scheduled (run from SceShell's syscall context when possible) */
static volatile int g_in_apply = 0;                /* an apply is executing (re-entrancy guard for the SetFrameBuf hook) */
static SceInt64 g_last_apply_time = 0;
static const uint32_t k_apply_backoff_s[APPLY_MAX_ATTEMPTS_EPISODE] = { 3, 5, 8, 12, 20, 30, 45, 60, 60, 60 };
static volatile int g_marker_pending = 0;   /* marker file exists and must be removed after safe_boot_seconds */
static int g_boot_apply_done = 0;

/* Logging */
static int g_log_dir_ok = 0;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static SceInt64 now_us(void)
{
    return ksceKernelGetSystemTimeWide();
}

static void lock(void)
{
    if (g_mutex >= 0)
        ksceKernelLockMutex(g_mutex, 1, NULL);
}

static void unlock(void)
{
    if (g_mutex >= 0)
        ksceKernelUnlockMutex(g_mutex, 1);
}

static void ensure_log_dir(void)
{
    if (g_log_dir_ok)
        return;
    int r = ksceIoMkdir(PSTV1080P_LOG_DIR, 6);
    if (r == 0 || r == SCE_ERRNO_EEXIST)
        g_log_dir_ok = 1;
}

/* Append one line to the kernel log.  Never called from the frame-pacing hooks. */
static void klog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n, m;

    ensure_log_dir();
    if (!g_log_dir_ok)
        return;

    {
        SceInt64 t = now_us();
        uint32_t ms = (uint32_t)(t / 1000);
        n = snprintf(buf, sizeof(buf) - 2, "[%u.%03u] ", ms / 1000u, ms % 1000u);
        if (n < 0)
            n = 0;
        if (n > (int)sizeof(buf) - 2)
            n = (int)sizeof(buf) - 2;
    }

    va_start(ap, fmt);
    m = vsnprintf(buf + n, sizeof(buf) - 2 - n, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;

    SceUID fd = ksceIoOpen(PSTV1080P_KERNEL_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 6);
    if (fd < 0)
        return;
    ksceIoWrite(fd, buf, (SceSize)n);
    ksceIoClose(fd);
}

static int file_exists(const char *path)
{
    SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    ksceIoClose(fd);
    return 1;
}

static int file_touch(const char *path)
{
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
    if (fd < 0)
        return (int)fd;
    ksceIoClose(fd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Refresh-rate cache                                                         */
/* ------------------------------------------------------------------------- */

static uint32_t refresh_from_mode(uint32_t mode)
{
    switch (mode & PSTV1080P_SCREENMODE_HZ_MASK) {
    case PSTV1080P_SCREENMODE_60HZ: return 60;
    case PSTV1080P_SCREENMODE_30HZ: return 30;
    case PSTV1080P_SCREENMODE_24HZ: return 24;
    case PSTV1080P_SCREENMODE_25HZ: return 25;   /* assumed */
    case PSTV1080P_SCREENMODE_50HZ: return 50;
    default:                        return 60;
    }
}

/* Query the display driver, update g_current_output_mode / g_refresh_hz.
 * Returns the ksceDisplayGetOutputMode result.  Not for the hot path.
 *
 * If the driver reports exactly what it reported right after the last
 * successful SetResolution (g_applied_readback), the refresh rate is derived
 * from the code that was applied, not from the readback: assumption A3 says
 * the two may be encoded differently, and the applied code is the only thing
 * we know for sure.  On a driver error the previous values are kept. */
static int refresh_display_cache(int do_log)
{
    unsigned int mode = 0, pf = 0;
    int ret = ksceDisplayGetOutputMode(HDMI_HEAD, &mode, &pf);
    uint32_t old_hz = g_refresh_hz;
    uint32_t old_mode = g_current_output_mode;

    if (ret >= 0) {
        g_current_output_mode = mode;
        if (g_hd_alias != 0 && (uint32_t)mode == g_hd_alias)
            g_refresh_hz = refresh_from_mode(g_cfg.hd_mode_code);
        else
            g_refresh_hz = refresh_from_mode(mode);
    }

    if (do_log && (old_hz != g_refresh_hz || old_mode != g_current_output_mode || ret < 0)) {
        /* Cross-check with the driver's own idea of the refresh rate.  The
         * float is converted to an integer immediately (softfp, pointer only). */
        float fps = 0.0f;
        int scan = 0;
        int fps_x100 = -1;
        int rr = ksceDisplayGetRefreshRateInternal(HDMI_HEAD, &fps, &scan);
        if (rr >= 0)
            fps_x100 = (int)(fps * 100.0f);
        klog("display: GetOutputMode ret=0x%08X mode=0x%04X pf=0x%X -> refresh_hz=%u (driver fps*100=%d scan=%d rr=0x%08X)",
             (unsigned)ret, (unsigned)mode, (unsigned)pf, (unsigned)g_refresh_hz, fps_x100, scan, (unsigned)rr);
    }
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                     */
/* ------------------------------------------------------------------------- */

static void config_defaults(pstv1080p_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic               = PSTV1080P_CFG_MAGIC;
    c->version             = PSTV1080P_CFG_VERSION;
    c->mode_1080p          = 0;
    c->hd_mode_code        = PSTV1080P_MODE_1080P30;
    c->settings_item_value = 3;
    c->fps_mode            = PSTV1080P_FPS_SCALE;
    c->fps_target          = 30;
    c->fps_inject          = 1;     /* AUTO: inject only for games that do not vsync themselves */
    c->safe_boot_seconds   = 120;
    c->boot_apply_delay_ms = 3000;
    c->watchdog_period_ms  = 2000;
}

/* Shape test for a screen-mode code we are willing to hand to
 * sceAVConfigHdmiSetResolution: "standard mode" flag set, a resolution field
 * in 480P..1080P, nothing above 16 bits.  Used for cfg.hd_mode_code and for
 * the modes Sony's code requests (recorded for the revert path). */
static int mode_code_plausible(uint32_t m)
{
    uint32_t res;
    if (!(m & PSTV1080P_SCREENMODE_STD))
        return 0;
    if (m & ~0xFFFFu)
        return 0;
    res = m & PSTV1080P_SCREENMODE_RES_MASK;
    if (res < PSTV1080P_SCREENMODE_480P || res > PSTV1080P_SCREENMODE_1080P)
        return 0;
    return 1;
}

/* Strict validation of the user-controllable fields.  Returns 1 if valid. */
static int config_valid(const pstv1080p_config_t *c)
{
    if (c->magic != PSTV1080P_CFG_MAGIC || c->version != PSTV1080P_CFG_VERSION)
        return 0;
    if (c->mode_1080p > 1)
        return 0;
    if (!mode_code_plausible(c->hd_mode_code))
        return 0;
    if (c->settings_item_value < 1 || c->settings_item_value > 255)
        return 0;
    if (c->fps_mode > PSTV1080P_FPS_FORCE)
        return 0;
    if (c->fps_target != 20 && c->fps_target != 30 && c->fps_target != 60)
        return 0;
    if (c->fps_inject > 2)
        return 0;
    return 1;
}

/* Clamp the timing fields to sane ranges (never rejected, only limited). */
static void config_clamp(pstv1080p_config_t *c)
{
    if (c->safe_boot_seconds > SAFE_BOOT_MAX_SECONDS)
        c->safe_boot_seconds = SAFE_BOOT_MAX_SECONDS;
    if (c->boot_apply_delay_ms > BOOT_DELAY_MAX_MS)
        c->boot_apply_delay_ms = BOOT_DELAY_MAX_MS;
    if (c->watchdog_period_ms != 0 && c->watchdog_period_ms < WATCHDOG_MIN_PERIOD_MS)
        c->watchdog_period_ms = WATCHDOG_MIN_PERIOD_MS;
    memset(c->reserved, 0, sizeof(c->reserved));
}

static int config_save(void)
{
    SceUID fd = ksceIoOpen(PSTV1080P_CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
    int ret;
    if (fd < 0) {
        klog("config: open for write failed 0x%08X", (unsigned)fd);
        return (int)fd;
    }
    ret = ksceIoWrite(fd, &g_cfg, sizeof(g_cfg));
    ksceIoClose(fd);
    if (ret != (int)sizeof(g_cfg)) {
        klog("config: write failed 0x%08X", (unsigned)ret);
        return ret < 0 ? ret : PSTV1080P_ERR_APPLY_FAILED;
    }
    return 0;
}

static void config_load(void)
{
    pstv1080p_config_t tmp;
    SceUID fd;
    int ret;

    config_defaults(&g_cfg);

    fd = ksceIoOpen(PSTV1080P_CFG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        klog("config: no state file (0x%08X), using defaults", (unsigned)fd);
        return;
    }
    memset(&tmp, 0, sizeof(tmp));
    ret = ksceIoRead(fd, &tmp, sizeof(tmp));
    ksceIoClose(fd);

    if (ret != (int)sizeof(tmp) || !config_valid(&tmp)) {
        klog("config: state file invalid (read=%d magic=0x%08X ver=%u), using defaults",
             ret, (unsigned)tmp.magic, (unsigned)tmp.version);
        return;
    }
    config_clamp(&tmp);
    memcpy(&g_cfg, &tmp, sizeof(g_cfg));
    klog("config: loaded mode_1080p=%u hd_mode=0x%04X item=%u fps_mode=%u target=%u inject=%u safe=%us delay=%ums wd=%ums",
         (unsigned)g_cfg.mode_1080p, (unsigned)g_cfg.hd_mode_code, (unsigned)g_cfg.settings_item_value,
         (unsigned)g_cfg.fps_mode, (unsigned)g_cfg.fps_target, (unsigned)g_cfg.fps_inject,
         (unsigned)g_cfg.safe_boot_seconds, (unsigned)g_cfg.boot_apply_delay_ms,
         (unsigned)g_cfg.watchdog_period_ms);
}

/* ------------------------------------------------------------------------- */
/* Title list (FORCE mode filter)                                             */
/* ------------------------------------------------------------------------- */

static void pid_cache_clear(void)
{
    int i;
    for (i = 0; i < PID_CACHE_ENTRIES; i++) {
        g_pid_cache[i].pid = 0;
        g_pid_cache[i].allowed = 0;
    }
    g_pid_cache_next = 0;
}

/* Parse ur0:tai/pstv1080p_titles.txt: one title id per line, '#' comments,
 * "*ALL" means every process.  Missing file == "*ALL" (Framecapper-under-*ALL
 * behaviour).  Not for the hot path. */
static void title_list_load(void)
{
    static char buf[4096];
    SceUID fd;
    int len, i, start;
    uint32_t count = 0;
    int all = 0;

    g_title_filter_active = 0;   /* disable the filter while we rebuild it */
    g_title_count = 0;
    memset(g_titles, 0, sizeof(g_titles));

    fd = ksceIoOpen(PSTV1080P_TITLES_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        pid_cache_clear();
        klog("titles: no list file, FORCE mode applies to all titles");
        return;
    }
    len = ksceIoRead(fd, buf, sizeof(buf) - 1);
    ksceIoClose(fd);
    if (len < 0)
        len = 0;
    buf[len] = 0;

    start = 0;
    for (i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 0) {
            int s = start, e = i;
            start = i + 1;
            while (s < e && (buf[s] == ' ' || buf[s] == '\t'))
                s++;
            while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t'))
                e--;
            if (s >= e || buf[s] == '#')
                continue;
            if ((e - s) == 4 && strncmp(buf + s, "*ALL", 4) == 0) {
                all = 1;
                continue;
            }
            if ((e - s) >= TITLE_ID_LEN)
                continue;   /* not a title id */
            if (count < TITLE_LIST_MAX) {
                memcpy(g_titles[count], buf + s, (unsigned int)(e - s));
                g_titles[count][e - s] = 0;
                count++;
            }
        }
    }

    g_title_count = count;
    pid_cache_clear();
    if (all || count == 0) {
        g_title_filter_active = 0;
        klog("titles: list has *ALL or no entries (%u), FORCE mode applies to all titles", (unsigned)count);
    } else {
        g_title_filter_active = 1;
        klog("titles: filter active with %u title id(s), first='%s'", (unsigned)count, g_titles[0]);
    }
}

/* Slow path, once per new pid: resolve the title id and decide.  Runs in the
 * caller's context of a display syscall; sysroot lookup only, no file I/O. */
static uint32_t pid_resolve_allowed(SceUID pid)
{
    char tid[TITLE_ID_LEN];
    uint32_t i, allowed = 0;

    memset(tid, 0, sizeof(tid));
    if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid) - 1) < 0)
        return 0;
    tid[sizeof(tid) - 1] = 0;

    for (i = 0; i < g_title_count && i < TITLE_LIST_MAX; i++) {
        if (strncmp(tid, g_titles[i], TITLE_ID_LEN) == 0) {
            allowed = 1;
            break;
        }
    }
    return allowed;
}

static uint32_t pid_allowed_cached(SceUID pid)
{
    uint32_t i, slot, allowed;

    for (i = 0; i < PID_CACHE_ENTRIES; i++) {
        if (g_pid_cache[i].pid == pid)
            return g_pid_cache[i].allowed;
    }

    allowed = pid_resolve_allowed(pid);

    /* Insert round-robin.  Invalidate the slot before filling it so that a
     * concurrent reader never pairs a new pid with an old verdict. */
    slot = g_pid_cache_next % PID_CACHE_ENTRIES;
    g_pid_cache_next = slot + 1;
    g_pid_cache[slot].pid = 0;
    g_pid_cache[slot].allowed = allowed;
    g_pid_cache[slot].pid = pid;
    return allowed;
}

/* ------------------------------------------------------------------------- */
/* Frame pacing hot path                                                      */
/* ------------------------------------------------------------------------- */

/* Returns PSTV1080P_FPS_OFF (pass through), _SCALE or _FORCE for this caller. */
static vs_entry_t *vs_lookup(SceUID pid, int create)
{
    uint32_t i, slot;
    for (i = 0; i < VS_TRACK_ENTRIES; i++) {
        if (g_vs[i].pid == pid)
            return &g_vs[i];
    }
    if (!create)
        return NULL;
    slot = g_vs_next % VS_TRACK_ENTRIES;
    g_vs_next = slot + 1;
    g_vs[slot].pid = 0;
    g_vs[slot].last_sync_us = 0;
    g_vs[slot].cb_synced = 0;
    g_vs[slot].pid = pid;
    return &g_vs[slot];
}

static void vs_clear(void)
{
    uint32_t i;
    for (i = 0; i < VS_TRACK_ENTRIES; i++) {
        g_vs[i].pid = 0;
        g_vs[i].last_sync_us = 0;
        g_vs[i].cb_synced = 0;
    }
    g_vs_next = 0;
}

/* Called on entry and exit of every vsync-related syscall of a user process.
 * O(1), no I/O.  Only needed while fps_inject == AUTO. */
static inline void vs_mark_sync(void)
{
    SceUID pid;
    vs_entry_t *e;
    if (g_cfg.fps_inject != 1u)
        return;
    pid = ksceKernelGetProcessId();
    if (pid <= 0 || pid == KERNEL_PID)
        return;
    e = vs_lookup(pid, 1);
    e->last_sync_us = now_us();
}

static inline uint32_t pacing_mode_for_pid(SceUID pid)
{
    uint32_t mode = g_cfg.fps_mode;

    if (mode == PSTV1080P_FPS_OFF)
        return PSTV1080P_FPS_OFF;
    if (pid == KERNEL_PID || pid == g_shell_pid || pid <= 0)
        return PSTV1080P_FPS_OFF;
    if (mode == PSTV1080P_FPS_FORCE && g_title_filter_active) {
        if (!pid_allowed_cached(pid))
            return PSTV1080P_FPS_OFF;
    }
    return mode;
}

static inline uint32_t pacing_mode_for_caller(void)
{
    if (g_cfg.fps_mode == PSTV1080P_FPS_OFF)
        return PSTV1080P_FPS_OFF;
    return pacing_mode_for_pid(ksceKernelGetProcessId());
}

static inline unsigned int scale_vcount(unsigned int vcount)
{
    uint32_t hz = g_refresh_hz;
    uint32_t v;
    if (hz == 60 || hz == 0)
        return vcount;
    if (vcount > 0x00FFFFFFu)          /* avoid overflow on absurd inputs */
        return vcount;
    v = (vcount * hz) / 60u;
    return v < 1 ? 1 : v;
}

static inline unsigned int force_interval(void)
{
    uint32_t hz = g_refresh_hz;
    uint32_t target = g_cfg.fps_target;
    uint32_t v;
    if (target == 0)
        target = 30;
    v = hz / target;
    return v < 1 ? 1 : v;
}

static inline unsigned int pace_vcount(uint32_t mode, unsigned int vcount)
{
    if (mode == PSTV1080P_FPS_SCALE)
        return scale_vcount(vcount);
    if (mode == PSTV1080P_FPS_FORCE)
        return force_interval();
    return vcount;
}

static void run_pending_apply(const char *ctx);

/* Boot / re-apply trigger (v1.1): when an attempt is scheduled, execute it
 * from the first SceShell display syscall that comes along (frame flip or
 * vblank wait), i.e. from a user-process syscall context.  One volatile load
 * per call when nothing is pending. */
static inline void shell_apply_check(void)
{
    if (g_apply_pending && g_shell_pid > 0 && ksceKernelGetProcessId() == g_shell_pid)
        run_pending_apply("shell");
}

static int hook_WaitVblankStartMulti(unsigned int vcount)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode != PSTV1080P_FPS_OFF)
        vcount = pace_vcount(mode, vcount);
    ret = HOOK_NEXT(hook_WaitVblankStartMulti, g_pacing_ref[PH_WAITVBLANKMULTI], vcount);
    vs_mark_sync();
    return ret;
}

static int hook_WaitVblankStartMultiCB(unsigned int vcount)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode != PSTV1080P_FPS_OFF)
        vcount = pace_vcount(mode, vcount);
    ret = HOOK_NEXT(hook_WaitVblankStartMultiCB, g_pacing_ref[PH_WAITVBLANKMULTICB], vcount);
    vs_mark_sync();
    return ret;
}

static int hook_WaitVblankStart(void)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode == PSTV1080P_FPS_FORCE) {
        unsigned int interval = force_interval();
        if (interval > 1) {
            ret = ksceDisplayWaitVblankStartMulti(interval);
            vs_mark_sync();
            return ret;
        }
    }
    /* SCALE: interval 1 scales to 1 -> always pass through. */
    ret = HOOK_NEXT(hook_WaitVblankStart, g_pacing_ref[PH_WAITVBLANK]);
    vs_mark_sync();
    return ret;
}

static int hook_WaitVblankStartCB(void)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode == PSTV1080P_FPS_FORCE) {
        unsigned int interval = force_interval();
        if (interval > 1) {
            ret = ksceDisplayWaitVblankStartMultiCB(interval);
            vs_mark_sync();
            return ret;
        }
    }
    ret = HOOK_NEXT(hook_WaitVblankStartCB, g_pacing_ref[PH_WAITVBLANKCB]);
    vs_mark_sync();
    return ret;
}

static int hook_WaitSetFrameBufMulti(unsigned int vcount)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode != PSTV1080P_FPS_OFF)
        vcount = pace_vcount(mode, vcount);
    ret = HOOK_NEXT(hook_WaitSetFrameBufMulti, g_pacing_ref[PH_WAITSETFBMULTI], vcount);
    vs_mark_sync();
    return ret;
}

static int hook_WaitSetFrameBufMultiCB(unsigned int vcount)
{
    uint32_t mode = pacing_mode_for_caller();
    int ret;
    shell_apply_check();
    vs_mark_sync();
    if (mode != PSTV1080P_FPS_OFF)
        vcount = pace_vcount(mode, vcount);
    ret = HOOK_NEXT(hook_WaitSetFrameBufMultiCB, g_pacing_ref[PH_WAITSETFBMULTICB], vcount);
    vs_mark_sync();
    return ret;
}

/* v1.2 pass-through hooks: they only feed the sync-activity tracker. */
static int hook_WaitSetFrameBuf(void)
{
    int ret;
    vs_mark_sync();
    ret = HOOK_NEXT(hook_WaitSetFrameBuf, g_pacing_ref[PH_WAITSETFB]);
    vs_mark_sync();
    return ret;
}

static int hook_WaitSetFrameBufCB(void)
{
    int ret;
    vs_mark_sync();
    ret = HOOK_NEXT(hook_WaitSetFrameBufCB, g_pacing_ref[PH_WAITSETFBCB]);
    vs_mark_sync();
    return ret;
}

static int hook_GetVcount(void)
{
    vs_mark_sync();
    return HOOK_NEXT(hook_GetVcount, g_pacing_ref[PH_GETVCOUNT]);
}

static int hook_GetVcountInternal(int head)
{
    vs_mark_sync();
    return HOOK_NEXT(hook_GetVcountInternal, g_pacing_ref[PH_GETVCOUNTINT], head);
}

static int hook_RegisterVblankStartCallback(SceUID uid)
{
    SceUID pid = ksceKernelGetProcessId();
    if (pid > 0 && pid != KERNEL_PID) {
        vs_entry_t *e = vs_lookup(pid, 1);
        e->cb_synced = 1;            /* this process syncs through a vblank callback: never inject */
    }
    return HOOK_NEXT(hook_RegisterVblankStartCallback, g_pacing_ref[PH_REGVBLANKCB], uid);
}

/* Adaptive inject (v1.2, fps_inject == 1): after a frame flip, wait one
 * refresh period (FORCE: the forced interval) ONLY if this process showed no
 * vsync activity for more than 4.5 refresh periods, i.e. it does not sync by
 * itself.  Games that already wait for vblank are never double-waited (the
 * defect of Framecapper's Inject build).  fps_inject == 2 injects always. */
static inline int inject_wanted(SceUID pid)
{
    vs_entry_t *e;
    SceInt64 idle, limit;
    uint32_t hz;

    if (g_cfg.fps_inject == 2u)
        return 1;
    if (g_cfg.fps_inject != 1u)
        return 0;
    e = vs_lookup(pid, 1);
    if (e->cb_synced)
        return 0;
    hz = g_refresh_hz;
    if (hz == 0)
        hz = 60;
    limit = (SceInt64)(1000000u / hz) * VS_IDLE_PERIODS_X2 / 2;
    idle = now_us() - e->last_sync_us;
    return idle > limit;
}

static int hook_SetFrameBuf(const void *pFrameBuf, int sync, void *pOpt)
{
    SceUID pid = ksceKernelGetProcessId();
    uint32_t mode = pacing_mode_for_pid(pid);
    int ret = HOOK_NEXT(hook_SetFrameBuf, g_pacing_ref[PH_SETFRAMEBUF], pFrameBuf, sync, pOpt);

    if (mode != PSTV1080P_FPS_OFF && g_cfg.fps_inject != 0u && inject_wanted(pid)) {
        unsigned int n = (mode == PSTV1080P_FPS_FORCE) ? force_interval() : 1u;
        ksceDisplayWaitVblankStartMulti(n);
    }
    /* After the flip: run a scheduled HD apply from SceShell's context (A9). */
    shell_apply_check();
    return ret;
}

/* ------------------------------------------------------------------------- */
/* SceAVConfig hook + apply / revert                                          */
/* ------------------------------------------------------------------------- */

/* Bookkeeping after a successful SetResolution(mode) by anyone: remember the
 * applied code and what the driver reports for it, then refresh the cache
 * (which derives the refresh rate from the applied code when the readback
 * is that known-different value, see refresh_display_cache).  Returns the
 * readback, 0 if the driver could not be queried.  Not for the hot path. */
static uint32_t record_applied(uint32_t mode, int do_log)
{
    unsigned int rb = 0, pf = 0;
    int gr = ksceDisplayGetOutputMode(HDMI_HEAD, &rb, &pf);

    g_applied_mode = mode;
    g_applied_readback = (gr >= 0) ? (uint32_t)rb : 0;

    if (gr >= 0) {
        refresh_display_cache(do_log);
        if (do_log && (uint32_t)rb != mode)
            klog("avconfig: driver reports 0x%04X after applying 0x%04X (assumption A3); pacing uses the applied code",
                 (unsigned)rb, (unsigned)mode);
    } else {
        /* No readback: trust the code we just applied. */
        g_current_output_mode = mode;
        g_refresh_hz = refresh_from_mode(mode);
        if (do_log)
            klog("avconfig: GetOutputMode failed 0x%08X after applying 0x%04X, assuming it",
                 (unsigned)gr, (unsigned)mode);
    }
    return g_applied_readback;
}

/* "1080p is in effect" = the driver reports our code, or the alias readback
 * we learned from an apply that visibly changed the driver state. */
static int hd_in_effect(uint32_t cur)
{
    if (cur == g_cfg.hd_mode_code)
        return 1;
    if (g_hd_alias != 0 && cur == g_hd_alias)
        return 1;
    return 0;
}

static void apply_clear_schedule(void)
{
    g_apply_pending = 0;
    g_apply_due_us = 0;
}

/* Schedule the next attempt of the current episode with backoff; give up
 * when the episode or session budget is exhausted.  Caller holds the lock. */
static void apply_schedule_retry(void)
{
    uint32_t n = g_apply_attempts;   /* attempts already made in this episode, >= 1 */
    uint32_t idx = n ? n - 1 : 0;

    if (n >= APPLY_MAX_ATTEMPTS_EPISODE || g_apply_total >= APPLY_MAX_TOTAL_SESSION) {
        apply_clear_schedule();
        klog("apply: giving up (episode %u/%u, session %u/%u); select the mode again in Settings to restart",
             (unsigned)n, (unsigned)APPLY_MAX_ATTEMPTS_EPISODE,
             (unsigned)g_apply_total, (unsigned)APPLY_MAX_TOTAL_SESSION);
        return;
    }
    if (idx >= APPLY_MAX_ATTEMPTS_EPISODE)
        idx = APPLY_MAX_ATTEMPTS_EPISODE - 1;
    g_apply_due_us = now_us() + (SceInt64)k_apply_backoff_s[idx] * 1000000LL;
    g_apply_pending = 1;
    klog("apply: retry %u scheduled in %u s", (unsigned)(n + 1), (unsigned)k_apply_backoff_s[idx]);
}

static int hook_HdmiSetResolution(int mode)
{
    int requested = mode;
    int ret;

    /* Remember what Sony's code wanted, but never our own code (the Settings
     * plugin or an HDMI re-plug may re-send it) and never garbage: this value
     * is handed back to SetResolution by the revert path. */
    if (!g_self_apply && (uint32_t)mode != g_cfg.hd_mode_code && mode_code_plausible((uint32_t)mode))
        g_last_system_mode = (uint32_t)mode;

    if (g_cfg.mode_1080p)
        mode = (int)g_cfg.hd_mode_code;

    ret = HOOK_NEXT(hook_HdmiSetResolution, g_avconfig_ref, mode);
    g_last_setres_ret = ret;

    klog("avconfig: SetResolution requested=0x%04X applied=0x%04X ret=0x%08X self=%d",
         (unsigned)requested, (unsigned)mode, (unsigned)ret, g_self_apply);

    if (ret >= 0)
        record_applied((uint32_t)mode, 1);
    return ret;
}

/* Issue sceAVConfigHdmiSetResolution(mode) ourselves. */
static int call_set_resolution(uint32_t mode)
{
    int ret;
    if (!g_avconfig_fn)
        return PSTV1080P_ERR_NOT_READY;
    g_self_apply = 1;
    ret = g_avconfig_fn((int)mode);
    g_self_apply = 0;
    g_last_apply_result = ret;
    g_last_apply_time = now_us();
    return ret;
}

/* Bring the HDMI output to cfg.hd_mode_code if it is not there already.
 * Returns 0 on success or nothing to do, <0 on error.  Caller holds the lock. */
static int apply_hd_mode(const char *why)
{
    unsigned int before = 0, after = 0, pf = 0;
    int gb, ga, ret, effective;

    if (!g_cfg.mode_1080p)
        return 0;

    gb = ksceDisplayGetOutputMode(HDMI_HEAD, &before, &pf);
    if (gb >= 0 && hd_in_effect(before)) {
        /* Already there: nothing to do, no extra HDMI renegotiation. */
        refresh_display_cache(0);
        g_apply_attempts = 0;
        apply_clear_schedule();
        return 0;
    }

    if (g_apply_total >= APPLY_MAX_TOTAL_SESSION) {
        apply_clear_schedule();
        return PSTV1080P_ERR_APPLY_FAILED;
    }
    g_apply_attempts++;
    g_apply_total++;

    g_in_apply = 1;
    ret = call_set_resolution(g_cfg.hd_mode_code);
    g_in_apply = 0;

    ga = ksceDisplayGetOutputMode(HDMI_HEAD, &after, &pf);

    /* An apply counts only if the driver state actually changed: either it
     * now reports our code, or it reports something new that is neither the
     * mode it had before nor the mode Sony last asked for (then that value is
     * the driver's own encoding of our mode and becomes the alias).  A call
     * that "succeeds" but leaves the readback untouched is NOT a success:
     * this is exactly what happened on the first hardware test (boot apply
     * from the kernel thread, readback stayed 0x8300, v1.0 latched it). */
    effective = 0;
    if (ret >= 0 && ga >= 0) {
        if ((uint32_t)after == g_cfg.hd_mode_code) {
            effective = 1;
        } else if (gb >= 0 && after != before && (uint32_t)after != g_last_system_mode
                   && mode_code_plausible((uint32_t)after)) {
            effective = 1;
            g_hd_alias = (uint32_t)after;
        }
    }

    klog("apply(%s): attempt %u (session %u): before=0x%04X -> SetResolution(0x%04X) ret=0x%08X, after=0x%04X (get=0x%08X/0x%08X) %s%s",
         why, (unsigned)g_apply_attempts, (unsigned)g_apply_total, before,
         (unsigned)g_cfg.hd_mode_code, (unsigned)ret, after, (unsigned)gb, (unsigned)ga,
         effective ? "EFFECTIVE" : "not effective",
         (effective && (uint32_t)after != g_cfg.hd_mode_code) ? " (alias learned)" : "");

    if (effective) {
        g_apply_attempts = 0;
        apply_clear_schedule();
        refresh_display_cache(1);
        return 0;
    }

    apply_schedule_retry();
    if (ret < 0)
        return ret;
    return 0;   /* the syscall itself succeeded; the retry schedule covers the rest */
}

/* Execute a scheduled attempt if it is due.  Safe to call from the SetFrameBuf
 * hook (SceShell's syscall context) and from the worker thread.  Uses a
 * try-lock so a frame flip never blocks behind the watchdog, and skips while
 * another apply is in progress (re-entrancy guard). */
static void run_pending_apply(const char *ctx)
{
    int locked;

    if (!g_apply_pending || g_in_apply)
        return;
    if (now_us() < g_apply_due_us)
        return;

    if (g_mutex >= 0) {
        if (ksceKernelTryLockMutex(g_mutex, 1) < 0)
            return;
        locked = 1;
    } else {
        locked = 0;
    }

    if (g_apply_pending && !g_in_apply && now_us() >= g_apply_due_us) {
        g_apply_pending = 0;
        if (g_cfg.mode_1080p)
            apply_hd_mode(ctx);      /* reschedules itself if not effective */
        else
            apply_clear_schedule();
    }

    if (locked)
        ksceKernelUnlockMutex(g_mutex, 1);
}

/* Return to the Sony-selected mode after the user turned 1080p off.
 * Caller holds the lock and has already cleared cfg.mode_1080p. */
static int revert_hd_mode(void)
{
    uint32_t sys = g_last_system_mode;
    int ret;

    if (sys == 0) {
        klog("revert: system mode unknown, leaving the display alone");
        refresh_display_cache(1);
        return 0;
    }
    if (sys == g_cfg.hd_mode_code || !mode_code_plausible(sys)) {
        /* Should not happen (the hook filters what it records) but never
         * re-apply our own code or garbage in the name of "reverting". */
        klog("revert: recorded system mode 0x%04X is our own code or implausible, leaving the display alone",
             (unsigned)sys);
        refresh_display_cache(1);
        return 0;
    }
    ret = call_set_resolution(sys);
    klog("revert: SetResolution(0x%04X) ret=0x%08X", (unsigned)sys, (unsigned)ret);
    if (ret >= 0)
        record_applied(sys, !(g_hooks_ok & HOOK_BIT_AVCONFIG));
    else
        refresh_display_cache(1);
    return ret < 0 ? ret : 0;
}

/* Common tail for every state change: persist, reset the failure counter,
 * handle the boot marker, then apply or revert.  Caller holds the lock. */
static int state_changed(int old_enabled, const char *why)
{
    int save_ret = config_save();
    int ret = 0;

    g_apply_attempts = 0;
    apply_clear_schedule();

    if (!g_cfg.mode_1080p) {
        /* Disabled at runtime: the marker must not survive into the next boot. */
        if (g_marker_pending || file_exists(PSTV1080P_BOOT_MARKER_PATH)) {
            ksceIoRemove(PSTV1080P_BOOT_MARKER_PATH);
            g_marker_pending = 0;
        }
        if (old_enabled)
            ret = revert_hd_mode();
    } else {
        /* Enabled at runtime: the user is interacting with a working display,
         * so this session needs no safe-boot marker. */
        g_marker_pending = 0;
        ret = apply_hd_mode(why);
    }

    if (save_ret < 0 && ret == 0)
        ret = save_ret;
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Boot / watchdog thread                                                     */
/* ------------------------------------------------------------------------- */

static int sleep_checking_stop(uint32_t total_us)
{
    while (total_us > 0 && !g_thread_stop) {
        uint32_t step = total_us > THREAD_TICK_US ? THREAD_TICK_US : total_us;
        ksceKernelDelayThread(step);
        total_us -= step;
    }
    return g_thread_stop;
}

static void marker_tick(void)
{
    if (!g_marker_pending)
        return;
    {
        SceInt64 limit = (SceInt64)g_cfg.safe_boot_seconds * 1000000LL;
        if (now_us() >= limit) {
            int r = ksceIoRemove(PSTV1080P_BOOT_MARKER_PATH);
            g_marker_pending = 0;
            klog("safeboot: survived %us, marker removed (0x%08X)",
                 (unsigned)g_cfg.safe_boot_seconds, (unsigned)r);
        }
    }
}

static void watchdog_tick(void)
{
    unsigned int cur = 0, pf = 0;
    int gret;

    lock();
    if (!g_cfg.mode_1080p || g_cfg.watchdog_period_ms == 0) {
        unlock();
        return;
    }

    gret = ksceDisplayGetOutputMode(HDMI_HEAD, &cur, &pf);
    if (gret >= 0) {
        if (cur != g_current_output_mode) {
            klog("watchdog: output mode changed 0x%04X -> 0x%04X", (unsigned)g_current_output_mode, cur);
            refresh_display_cache(1);
        }
        if (hd_in_effect(cur)) {
            if (g_apply_attempts != 0 || g_apply_pending) {
                klog("watchdog: HD mode in effect (0x%04X)", cur);
                g_apply_attempts = 0;
                apply_clear_schedule();
            }
        } else if (!g_apply_pending && g_apply_attempts == 0
                   && g_apply_total < APPLY_MAX_TOTAL_SESSION
                   && (now_us() - g_last_apply_time) >= (SceInt64)APPLY_MIN_INTERVAL_US) {
            /* New drift episode (e.g. the system re-applied its own mode after
             * an HDMI re-plug).  Bounded by the episode and session budgets. */
            klog("watchdog: drift, driver reports 0x%04X; scheduling re-apply", cur);
            g_apply_due_us = now_us();
            g_apply_pending = 1;
        }
    }
    unlock();
}

static int pstv1080p_thread(SceSize args, void *argp)
{
    int polls = 0;
    SceInt64 last_wd = 0;

    /* 1. Wait for SceShell (max 60 s), caching its pid for the process filter. */
    while (!g_thread_stop) {
        SceUID sp = ksceKernelSysrootGetShellPid();
        if (sp > 0) {
            g_shell_pid = sp;
            break;
        }
        if (++polls >= SHELL_WAIT_MAX_POLLS) {
            klog("thread: SceShell did not appear within 60 s, continuing anyway");
            break;
        }
        ksceKernelDelayThread(SHELL_WAIT_POLL_US);
    }
    if (g_thread_stop)
        return 0;
    klog("thread: shell pid=0x%08X, waiting %u ms before first apply",
         (unsigned)g_shell_pid, (unsigned)g_cfg.boot_apply_delay_ms);

    /* 2. Boot apply. */
    if (sleep_checking_stop(g_cfg.boot_apply_delay_ms * 1000u))
        return 0;

    lock();
    refresh_display_cache(1);
    if (g_cfg.mode_1080p) {
        unsigned int cur = 0, pf = 0;
        if (ksceDisplayGetOutputMode(HDMI_HEAD, &cur, &pf) >= 0 && hd_in_effect(cur)) {
            klog("boot: HD mode already in effect (0x%04X)", cur);
        } else {
            g_apply_attempts = 0;
            g_apply_due_us = now_us();
            g_apply_pending = 1;
            klog("boot: apply scheduled, waiting for a SceShell frame (thread fallback after %u s)",
                 (unsigned)(APPLY_SHELL_FALLBACK_US / 1000000u));
        }
    }
    g_boot_apply_done = 1;
    unlock();

    /* 3. Periodic work: marker removal, shell pid refresh, watchdog. */
    while (!g_thread_stop) {
        ksceKernelDelayThread(THREAD_TICK_US);
        if (g_thread_stop)
            break;

        marker_tick();

        /* Fallback: if no SceShell frame flip picked up the scheduled attempt
         * within APPLY_SHELL_FALLBACK_US of it becoming due, do it from here. */
        if (g_apply_pending && (now_us() - g_apply_due_us) >= (SceInt64)APPLY_SHELL_FALLBACK_US)
            run_pending_apply("thread");

        if (g_shell_pid <= 0) {
            SceUID sp = ksceKernelSysrootGetShellPid();
            if (sp > 0)
                g_shell_pid = sp;
        }

        if (g_cfg.watchdog_period_ms != 0) {
            SceInt64 t = now_us();
            if (t - last_wd >= (SceInt64)g_cfg.watchdog_period_ms * 1000LL) {
                last_wd = t;
                watchdog_tick();
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Syscall exports                                                            */
/* ------------------------------------------------------------------------- */

int pstv1080pGetConfig(pstv1080p_config_t *out)
{
    pstv1080p_config_t tmp;
    int ret;

    if (!out)
        return PSTV1080P_ERR_INVALID_ARG;
    lock();
    memcpy(&tmp, &g_cfg, sizeof(tmp));
    unlock();
    ret = ksceKernelCopyToUser(out, &tmp, sizeof(tmp));
    return ret < 0 ? ret : 0;
}

int pstv1080pSetConfig(const pstv1080p_config_t *in)
{
    pstv1080p_config_t tmp;
    int ret, old_enabled;

    if (!in)
        return PSTV1080P_ERR_INVALID_ARG;
    memset(&tmp, 0, sizeof(tmp));
    ret = ksceKernelCopyFromUser(&tmp, in, sizeof(tmp));
    if (ret < 0)
        return ret;
    if (!config_valid(&tmp)) {
        klog("SetConfig: rejected (magic=0x%08X ver=%u hd=0x%04X item=%u fps_mode=%u target=%u inject=%u)",
             (unsigned)tmp.magic, (unsigned)tmp.version, (unsigned)tmp.hd_mode_code,
             (unsigned)tmp.settings_item_value, (unsigned)tmp.fps_mode,
             (unsigned)tmp.fps_target, (unsigned)tmp.fps_inject);
        return PSTV1080P_ERR_INVALID_ARG;
    }
    config_clamp(&tmp);

    lock();
    old_enabled = (int)g_cfg.mode_1080p;
    {
        int fps_changed = (tmp.fps_mode != g_cfg.fps_mode);
        memcpy(&g_cfg, &tmp, sizeof(g_cfg));
        klog("SetConfig: mode_1080p=%u hd_mode=0x%04X item=%u fps_mode=%u target=%u inject=%u safe=%us delay=%ums wd=%ums",
             (unsigned)g_cfg.mode_1080p, (unsigned)g_cfg.hd_mode_code, (unsigned)g_cfg.settings_item_value,
             (unsigned)g_cfg.fps_mode, (unsigned)g_cfg.fps_target, (unsigned)g_cfg.fps_inject,
             (unsigned)g_cfg.safe_boot_seconds, (unsigned)g_cfg.boot_apply_delay_ms,
             (unsigned)g_cfg.watchdog_period_ms);
        if (fps_changed && g_cfg.fps_mode == PSTV1080P_FPS_FORCE)
            title_list_load();
    }
    if ((int)g_cfg.mode_1080p != old_enabled) {
        ret = state_changed(old_enabled, "SetConfig");
    } else {
        ret = config_save();
        g_apply_attempts = 0;         /* hd_mode_code may have changed: allow a fresh episode */
        apply_clear_schedule();
    }
    unlock();
    return ret;
}

int pstv1080pSetMode1080p(int enable)
{
    int ret, old_enabled;
    uint32_t want = enable ? 1u : 0u;

    lock();
    old_enabled = (int)g_cfg.mode_1080p;
    g_cfg.mode_1080p = want;
    klog("SetMode1080p(%d): was %d", enable, old_enabled);
    if (want != (uint32_t)old_enabled) {
        ret = state_changed(old_enabled, "SetMode1080p");
    } else if (want) {
        /* Already on: persist and make sure the output really is in HD mode. */
        ret = config_save();
        g_apply_attempts = 0;
        apply_clear_schedule();
        {
            int a = apply_hd_mode("SetMode1080p(re-apply)");
            if (ret == 0)
                ret = a;
        }
    } else {
        ret = config_save();
    }
    unlock();
    return ret;
}

int pstv1080pGetInfo(pstv1080p_info_t *out)
{
    pstv1080p_info_t info;
    unsigned int cur = 0, pf = 0;
    int ret;

    if (!out)
        return PSTV1080P_ERR_INVALID_ARG;

    memset(&info, 0, sizeof(info));
    info.size    = sizeof(info);
    info.version = PSTV1080P_VERSION;

    lock();
    info.mode_1080p          = g_cfg.mode_1080p;
    info.settings_item_value = g_cfg.settings_item_value;
    if (ksceDisplayGetOutputMode(HDMI_HEAD, &cur, &pf) >= 0) {
        info.current_output_mode = cur;
    } else {
        info.current_output_mode = g_current_output_mode;
    }
    info.refresh_hz          = g_refresh_hz;
    info.last_system_mode    = g_last_system_mode;
    info.last_apply_result   = g_last_apply_result;
    info.hooks_ok            = g_hooks_ok;
    info.reserved[0]         = g_apply_attempts;
    info.reserved[1]         = g_apply_total;
    info.reserved[2]         = g_hd_alias;
    info.reserved[3]         = (uint32_t)g_apply_pending;
    unlock();

    ret = ksceKernelCopyToUser(out, &info, sizeof(info));
    return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------------- */
/* Hook installation                                                          */
/* ------------------------------------------------------------------------- */

static void install_pacing_hook(int idx, uint32_t nid, const void *fn, uint32_t bit, const char *name)
{
    SceUID uid = taiHookFunctionExportForKernel(KERNEL_PID, &g_pacing_ref[idx], DISPLAY_MODULE,
                                                DISPLAY_USER_LIB_NID, nid, fn);
    g_pacing_uid[idx] = uid;
    if (uid >= 0) {
        g_hooks_ok |= bit;
        klog("hook: %s ok (uid=0x%08X)", name, (unsigned)uid);
    } else {
        klog("hook: %s FAILED 0x%08X (skipped)", name, (unsigned)uid);
    }
}

static void install_hooks(void)
{
    uintptr_t fn = 0;

    g_avconfig_uid = taiHookFunctionExportForKernel(KERNEL_PID, &g_avconfig_ref, AVCONFIG_MODULE,
                                                    AVCONFIG_LIB_NID, AVCONFIG_SETRES_NID,
                                                    hook_HdmiSetResolution);
    if (g_avconfig_uid >= 0) {
        g_hooks_ok |= HOOK_BIT_AVCONFIG;
        klog("hook: sceAVConfigHdmiSetResolution ok (uid=0x%08X)", (unsigned)g_avconfig_uid);
    } else {
        klog("hook: sceAVConfigHdmiSetResolution FAILED 0x%08X", (unsigned)g_avconfig_uid);
    }

    if (module_get_export_func(KERNEL_PID, AVCONFIG_MODULE, AVCONFIG_LIB_NID, AVCONFIG_SETRES_NID, &fn) >= 0 && fn) {
        g_avconfig_fn = (avconfig_setres_fn)fn;
        klog("export: sceAVConfigHdmiSetResolution resolved");
    } else {
        g_avconfig_fn = 0;
        klog("export: sceAVConfigHdmiSetResolution NOT found, apply/revert unavailable");
    }

    install_pacing_hook(PH_WAITVBLANKMULTI,   NID_WAITVBLANKSTARTMULTI,   hook_WaitVblankStartMulti,   HOOK_BIT_WAITVBLANKMULTI,   "WaitVblankStartMulti");
    install_pacing_hook(PH_WAITVBLANKMULTICB, NID_WAITVBLANKSTARTMULTICB, hook_WaitVblankStartMultiCB, HOOK_BIT_WAITVBLANKMULTICB, "WaitVblankStartMultiCB");
    install_pacing_hook(PH_WAITVBLANK,        NID_WAITVBLANKSTART,        hook_WaitVblankStart,        HOOK_BIT_WAITVBLANK,        "WaitVblankStart");
    install_pacing_hook(PH_WAITVBLANKCB,      NID_WAITVBLANKSTARTCB,      hook_WaitVblankStartCB,      HOOK_BIT_WAITVBLANKCB,      "WaitVblankStartCB");
    install_pacing_hook(PH_WAITSETFBMULTI,    NID_WAITSETFRAMEBUFMULTI,   hook_WaitSetFrameBufMulti,   HOOK_BIT_WAITSETFBMULTI,    "WaitSetFrameBufMulti");
    install_pacing_hook(PH_WAITSETFBMULTICB,  NID_WAITSETFRAMEBUFMULTICB, hook_WaitSetFrameBufMultiCB, HOOK_BIT_WAITSETFBMULTICB,  "WaitSetFrameBufMultiCB");
    install_pacing_hook(PH_SETFRAMEBUF,       NID_SETFRAMEBUF,            hook_SetFrameBuf,            HOOK_BIT_SETFRAMEBUF,       "_sceDisplaySetFrameBuf");
    install_pacing_hook(PH_WAITSETFB,         NID_WAITSETFRAMEBUF,        hook_WaitSetFrameBuf,        HOOK_BIT_WAITSETFB,         "WaitSetFrameBuf");
    install_pacing_hook(PH_WAITSETFBCB,       NID_WAITSETFRAMEBUFCB,      hook_WaitSetFrameBufCB,      HOOK_BIT_WAITSETFBCB,       "WaitSetFrameBufCB");
    install_pacing_hook(PH_GETVCOUNT,         NID_GETVCOUNT,              hook_GetVcount,              HOOK_BIT_GETVCOUNT,         "GetVcount");
    install_pacing_hook(PH_GETVCOUNTINT,      NID_GETVCOUNTINTERNAL,      hook_GetVcountInternal,      HOOK_BIT_GETVCOUNTINT,      "GetVcountInternal");
    install_pacing_hook(PH_REGVBLANKCB,       NID_REGISTERVBLANKCB,       hook_RegisterVblankStartCallback, HOOK_BIT_REGVBLANKCB,  "RegisterVblankStartCallback");
}

static void release_hooks(void)
{
    int i;
    for (i = PH_COUNT - 1; i >= 0; i--) {
        if (g_pacing_uid[i] >= 0) {
            taiHookReleaseForKernel(g_pacing_uid[i], g_pacing_ref[i]);
            g_pacing_uid[i] = -1;
        }
    }
    if (g_avconfig_uid >= 0) {
        taiHookReleaseForKernel(g_avconfig_uid, g_avconfig_ref);
        g_avconfig_uid = -1;
    }
    g_hooks_ok = 0;
}

/* ------------------------------------------------------------------------- */
/* Safe boot                                                                  */
/* ------------------------------------------------------------------------- */

static void safe_boot_check(void)
{
    int marker = file_exists(PSTV1080P_BOOT_MARKER_PATH);

    if (g_cfg.mode_1080p && marker) {
        g_cfg.mode_1080p = 0;
        config_save();
        ksceIoRemove(PSTV1080P_BOOT_MARKER_PATH);
        klog("safeboot: marker found from a previous boot -> 1080p reverted (disabled)");
        marker = 0;
    }

    if (g_cfg.mode_1080p) {
        if (g_cfg.safe_boot_seconds == 0) {
            if (marker)
                ksceIoRemove(PSTV1080P_BOOT_MARKER_PATH);
            g_marker_pending = 0;
            klog("safeboot: disabled by config");
        } else {
            int r = file_touch(PSTV1080P_BOOT_MARKER_PATH);
            g_marker_pending = (r == 0);
            klog("safeboot: marker created (0x%08X), window %us", (unsigned)r, (unsigned)g_cfg.safe_boot_seconds);
        }
    } else {
        if (marker)
            ksceIoRemove(PSTV1080P_BOOT_MARKER_PATH);
        g_marker_pending = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Module entry points                                                        */
/* ------------------------------------------------------------------------- */

void _start(void) __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    int i;

    for (i = 0; i < PH_COUNT; i++) {
        g_pacing_uid[i] = -1;
        g_pacing_ref[i] = 0;
    }
    g_avconfig_ref = 0;
    pid_cache_clear();
    vs_clear();

    g_mutex = ksceKernelCreateMutex("pstv1080p_mtx", 0, 0, NULL);

    klog("pstv1080p kernel module v%u.%u starting",
         (unsigned)(PSTV1080P_VERSION >> 8), (unsigned)(PSTV1080P_VERSION & 0xFF));

    config_load();
    safe_boot_check();
    if (g_cfg.fps_mode == PSTV1080P_FPS_FORCE)
        title_list_load();

    /* Refresh cache before hooks go live so the hot path never sees stale 60. */
    refresh_display_cache(1);

    install_hooks();

    g_thread_stop = 0;
    g_thread_uid = ksceKernelCreateThread("pstv1080p", pstv1080p_thread, 0x10000100, 0x2000, 0, 0, NULL);
    if (g_thread_uid >= 0) {
        int r = ksceKernelStartThread(g_thread_uid, 0, NULL);
        if (r < 0) {
            klog("thread: start failed 0x%08X", (unsigned)r);
            ksceKernelDeleteThread(g_thread_uid);
            g_thread_uid = -1;
        }
    } else {
        klog("thread: create failed 0x%08X", (unsigned)g_thread_uid);
    }

    klog("started: hooks_ok=0x%04X mode_1080p=%u refresh_hz=%u fps_mode=%u inject=%u",
         (unsigned)g_hooks_ok, (unsigned)g_cfg.mode_1080p, (unsigned)g_refresh_hz,
         (unsigned)g_cfg.fps_mode, (unsigned)g_cfg.fps_inject);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    if (g_thread_uid >= 0) {
        SceUInt timeout = 5u * 1000u * 1000u;
        g_thread_stop = 1;
        ksceKernelWaitThreadEnd(g_thread_uid, NULL, &timeout);
        ksceKernelDeleteThread(g_thread_uid);
        g_thread_uid = -1;
    }

    release_hooks();

    if (g_mutex >= 0) {
        ksceKernelDeleteMutex(g_mutex);
        g_mutex = -1;
    }

    klog("stopped");
    return SCE_KERNEL_STOP_SUCCESS;
}
