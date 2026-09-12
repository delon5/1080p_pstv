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
 *  1. Persistent state lives in ur0:data/pstv1080p/pstv1080p.cfg (pstv1080p_config_t,
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
 *     ur0:data/pstv1080p/pstv1080p.boot still exists, the previous boot in 1080p did not
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
 *  6. Five syscall exports (library "pstv1080p") used by the Settings-app
 *     plugin: Get/SetConfig, SetMode1080p, GetInfo and (1.6) Log, which
 *     appends the plugin's lines to the same log (no-op unless debug is on).
 *
 *  7. Logging (debug only) to ux0:data/pstv1080p/pstv1080p.log (never from the
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
 *      backoff, which covers both explanations.  The debug log shows which.
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
#include <psp2kern/kernel/proc_event.h>
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
#define SETRES_AUTO                   0x10000000u   /* Sony's "automatic" (the core also sends it for values it does not know) */
#define HOLD_REQUEST_US               (400u * 1000u) /* v1.4.7: how long a Sony request is held to merge it with ours */

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
#define NID_GETMAXFBRES               0x2EBFC7CBu   /* v1.4: _sceDisplayGetMaximumFrameBufResolution (spoof720) */
#define NID_GETRESINFOINTERNAL        0xFEFEB240u   /* v1.4: _sceDisplayGetResolutionInfoInternal (spoof720) */
#define NID_GETREFRESHRATE            0xA08CA60Du   /* v1.4.1: sceDisplayGetRefreshRate (spoof720 + logging) */
/* v1.4.3 "trace" diagnostics: SceSysmem user library exports (kernel module, hookable) */
#define SYSMEM_MODULE                 "SceSysmem"
#define SYSMEM_USER_LIB_NID           0x37FE725Au
#define NID_ALLOCMEMBLOCK             0xB9D5EBDEu
#define NID_GETFREEMEMORYSIZE         0x87CC580Bu

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
#define HOOK_BIT_GETMAXFBRES          (1u << 13)
#define HOOK_BIT_GETRESINFO           (1u << 14)
#define HOOK_BIT_GETREFRESHRATE       (1u << 15)
#define HOOK_BIT_ALLOCMEMBLOCK        (1u << 16)
#define HOOK_BIT_GETFREEMEM           (1u << 17)

#define SCE_ERRNO_EEXIST              ((int)0x80010011)

#define APPLY_MAX_ATTEMPTS_EPISODE    10             /* attempts per drift episode */
#define APPLY_MAX_TOTAL_SESSION       60             /* cap for AUTOMATIC attempts per boot; a user selection resets it */
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
/* Sony's Settings code calls sceAVConfigHdmiSetResolution with THREE
 * arguments: (mode, known, 1), known = 1 for a concrete mode and 0 for
 * "automatic" (SceSettings 0x81125102 on FW 3.60, RESEARCH_NOTES section 12).
 * They are passed through untouched and mimicked for our own calls. */
typedef int (*avconfig_setres_fn)(int mode, int known, int flag);
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
    PH_GETMAXFBRES,
    PH_GETRESINFO,
    PH_GETREFRESHRATE,
    PH_ALLOCMEMBLOCK,
    PH_GETFREEMEM,
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
static volatile SceInt64 g_last_sys_setres_us = 0; /* when Sony's code last called SetResolution (v1.4.4) */
static volatile uint32_t g_last_system_raw = 0;    /* last non-self request, unfiltered (may be SETRES_AUTO) */
static volatile SceInt64 g_last_native_us = 0;     /* v1.5: when Sony's patched Settings code last requested hd_mode_code itself */
#define NATIVE_RECENT_US              (3000u * 1000u) /* within this window SetMode1080p(1) does not re-send that request */
/* v1.4.7 held request: Sony's Settings code calls SetResolution BEFORE it
 * writes the registry, i.e. before the plugin learns what the user chose.
 * Holding that call for a moment lets the plugin merge it with its own
 * decision into ONE display transition (the boot path, which always worked,
 * is a single direct transition). */
static volatile int g_held = 0;
static volatile uint32_t g_held_mode = 0;
static volatile SceInt64 g_held_us = 0;
static volatile uint32_t g_mode_request = 0;       /* a mode to apply from the next user display syscall */
static volatile int g_mode_request_pending = 0;
#define SYS_SETRES_SETTLE_US          (500u * 1000u)
static volatile int      g_self_apply = 0;         /* set while we call the export ourselves */

/* Process filter */
static volatile SceUID g_shell_pid = 0;

/* Per-process table (v1.3): FORCE-filter verdict, per-title override,
 * frameskip accumulator and vsync-activity tracking, keyed by pid.  Filled on
 * the first display syscall of a process (one sysroot call + list lookups). */
#define PROC_ENTRIES                  8
#define VS_IDLE_PERIODS_X2            9              /* inject if no sync activity for > 4.5 refresh periods */
enum {
    /* 1.6.3: one PACING rule per title (this enum) plus any combination of
     * EXTRAS (OVRF_* flags below); a line reads "TITLEID rule extra extra". */
    OVR_NONE = 0,   /* no rule given: global rules apply */
    OVR_OFF,        /* "off":       no pacing change (inject only if asked for explicitly) */
    OVR_SCALE,      /* "scale":     SCALE rule + global inject setting, whatever the global mode */
    OVR_FRAMESKIP,  /* "frameskip": fractional vsync for frame-locked 60 fps games (2 logic frames per 30 Hz vblank) */
    OVR_NOWAIT,     /* "nowait":    every vblank wait returns at once */
    OVR_FORCE       /* "force":     FORCE rule for this title, whatever the global mode */
};
#define OVRF_INJECT    0x1u   /* "inject":   always wait one period after each flip (Framecapper Inject) */
#define OVRF_NOVSYNC   0x2u   /* "novsync":  every flip is made IMMEDIATE (what novsync.suprx does); with no
                               *             rule given it also implies "nowait" (novsync.suprx behaviour) */
#define OVRF_SPOOF720  0x4u   /* "spoof720": the display-info queries answer as a 720p60 head */
#define OVRF_TRACE     0x8u   /* "trace":    DIAGNOSTIC: lifecycle, allocations, display-call profile */
typedef struct {
    volatile SceUID pid;
    volatile uint32_t allowed;      /* FORCE title-filter verdict */
    volatile uint32_t override;     /* OVR_* pacing rule */
    volatile uint32_t flags;        /* OVRF_* extras (1.6.3) */
    volatile uint32_t acc;          /* frameskip credit, units of 1/60 vblank */
    volatile SceInt64 last_sync_us; /* last vsync-related syscall entry/exit */
    volatile uint32_t cb_synced;    /* registered a vblank callback: never inject */
    volatile SceInt64 created_us;   /* when the entry was resolved (eviction tiebreak) */
    volatile SceInt64 checked_us;   /* 1.6.6: last title re-check on the hit path */
    char title[TITLE_ID_LEN];
    volatile uint32_t cnt[10];      /* 1.6.2 display-call profile (PF_*), reported for "trace" titles */
} proc_entry_t;

/* Profile counters: how a game synchronises to the display.  Cheap increments
 * in the hooks; read and logged by the plugin thread (never from a hook). */
enum { PF_WAITVB = 0, PF_WAITVBCB, PF_WAITVBMULTI, PF_WAITSETFB, PF_WAITSETFBMULTI,
       PF_SETFB_IMM, PF_SETFB_NEXT, PF_GETVCOUNT, PF_MULTI_MAX, PF_COUNT_ };
#define PF_INC(e, i)        do { if (e) (e)->cnt[i]++; } while (0)
#define PF_MAX(e, i, v)     do { if ((e) && (uint32_t)(v) > (e)->cnt[i]) (e)->cnt[i] = (uint32_t)(v); } while (0)
static proc_entry_t g_procs[PROC_ENTRIES];
/* Serialises the table's miss path (resolve + publish), games_list_load() and
 * title_list_load().  Held for the few ms of a once-per-process resolve only;
 * never taken while g_mutex is needed from a hook path, so it cannot block a
 * frame behind the HDMI apply. */
static SceUID g_tbl_mutex = -1;

/* Per-title overrides (ur0:data/pstv1080p/pstv1080p_games.txt: "TITLEID mode" per line) */
#define GAMES_LIST_MAX                PSTV1080P_GAMES_MAX_ENTRIES   /* 1.6.1: shared with the configurator (was 32) */
typedef struct {
    char title[TITLE_ID_LEN];
    uint32_t mode;                  /* OVR_* pacing rule */
    uint32_t flags;                 /* OVRF_* extras */
} game_override_t;
static game_override_t g_games[GAMES_LIST_MAX];
static uint32_t g_games_count = 0;

/* Title list (ur0:data/pstv1080p/pstv1080p_titles.txt) */
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
static const uint32_t k_apply_backoff_s[APPLY_MAX_ATTEMPTS_EPISODE] = { 1, 2, 4, 8, 12, 20, 30, 45, 60, 60 };
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

/* SceShell's pid, learned on demand (the worker thread also caches it).
 * Costs one sysroot call per hook call only while it is still unknown,
 * i.e. during the first seconds of boot. */
static inline SceUID shell_pid_now(void)
{
    SceUID sp = g_shell_pid;
    if (sp <= 0) {
        sp = ksceKernelSysrootGetShellPid();
        if (sp > 0)
            g_shell_pid = sp;
    }
    return sp;
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

static void tbl_lock(void)
{
    if (g_tbl_mutex >= 0)
        ksceKernelLockMutex(g_tbl_mutex, 1, NULL);
}

static void tbl_unlock(void)
{
    if (g_tbl_mutex >= 0)
        ksceKernelUnlockMutex(g_tbl_mutex, 1);
}

/* 1.6: the plugin's directory on ur0 (config, overrides, marker, debug switch). */
static int g_dir_ok = 0;
static void ensure_dir(void)
{
    int r;
    if (g_dir_ok)
        return;
    ksceIoMkdir("ur0:data", 6);                /* may already exist: ignored */
    r = ksceIoMkdir(PSTV1080P_DIR, 6);
    if (r == 0 || r == SCE_ERRNO_EEXIST)
        g_dir_ok = 1;
}

/* The log directory on the memory card (only touched when logging is on). */
static void ensure_log_dir(void)
{
    int r;
    if (g_log_dir_ok)
        return;
    ksceIoMkdir("ux0:data", 6);
    r = ksceIoMkdir(PSTV1080P_LOG_DIR, 6);
    if (r == 0 || r == SCE_ERRNO_EEXIST)
        g_log_dir_ok = 1;
}

/* 1.6: NO log is written unless ur0:data/pstv1080p/pstv1080p_debug.txt exists
 * at boot.  With it, everything is logged (the 1.5 "verbose" set) to
 * ux0:data/pstv1080p/pstv1080p.log, including the Settings plugin's lines
 * (they arrive through the pstv1080pLog syscall). */
static int g_verbose = 0;
#define kvlog(...) do { if (g_verbose) klog(__VA_ARGS__); } while (0)

/* Append one line to the log.  Never called from the frame-pacing hooks. */
static void klog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n, m;

    if (!g_verbose)
        return;
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

    SceUID fd = ksceIoOpen(PSTV1080P_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 6);
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
    SceUID fd;
    ensure_dir();
    fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
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
        if (g_verbose) {
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
        } else {
            klog("display: mode=0x%04X refresh=%u Hz (get=0x%08X)", (unsigned)mode, (unsigned)g_refresh_hz, (unsigned)ret);
        }
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
    c->fps_target          = 60;    /* 1.6.6: Framecapper60 semantics by default (one vblank per wait at 30 Hz) */
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

/* v1.5.2: hd_mode_code is the code WE hand to the driver, so "plausible" is
 * not enough - it must be a mode this hardware can actually deliver.  1080p60
 * (0x8700) is not: the HDMI path has no 148.5 MHz clock and the driver answers
 * 0x803A0101 every single time, which produced an endless retry storm and no
 * picture on 1.5.1.  Only the 1080p variants that were verified on hardware
 * are accepted; anything else is repaired to 1080p30 instead of being used. */
static int hd_mode_usable(uint32_t m)
{
    return m == PSTV1080P_MODE_1080P30 || m == PSTV1080P_MODE_1080P24;
}

/* Repair an unusable hd_mode_code in place.  Returns 1 if it changed. */
static int hd_mode_repair(pstv1080p_config_t *c, const char *why)
{
    if (hd_mode_usable(c->hd_mode_code))
        return 0;
    klog("config(%s): hd_mode 0x%04X is not deliverable on this hardware, repaired to 0x%04X",
         why, (unsigned)c->hd_mode_code, (unsigned)PSTV1080P_MODE_1080P30);
    c->hd_mode_code = PSTV1080P_MODE_1080P30;
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
    if (!hd_mode_usable(c->hd_mode_code))
        return 0;                 /* v1.5.2: never accept a mode the driver always refuses */
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
    SceUID fd;
    int ret;
    ensure_dir();
    fd = ksceIoOpen(PSTV1080P_CFG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
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

static int g_target_fixed = 0;

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

    /* 1.6.6: the FORCE target default became 60 (Framecapper60 semantics: one
     * vblank per wait at 30 Hz).  A state file still carrying the old default
     * 30 is corrected in place and written back once; 20 is a deliberate
     * choice and is kept.  The file version is NOT bumped for this: every
     * other version is rejected by config_valid and would reset the console. */
    if (ret == (int)sizeof(tmp) && tmp.magic == PSTV1080P_CFG_MAGIC && tmp.fps_target == 30u) {
        tmp.fps_target = 60u;
        g_target_fixed = 1;
    }

    /* v1.3: a state file written by 1.0/1.1 (version 1) predates the adaptive
     * inject default; migrate it once to version 2 with fps_inject = AUTO. */
    if (ret == (int)sizeof(tmp) && tmp.magic == PSTV1080P_CFG_MAGIC && tmp.version == 1u) {
        tmp.version = 2u;
        tmp.fps_inject = 1u;
        klog("config: migrated state file v1 -> v%u (fps_inject=1 AUTO)", (unsigned)PSTV1080P_CFG_VERSION);
        if (config_valid(&tmp)) {
            config_clamp(&tmp);
            memcpy(&g_cfg, &tmp, sizeof(g_cfg));
            config_save();
        }
    }

    if (ret != (int)sizeof(tmp) || !config_valid(&tmp)) {
        klog("config: state file invalid (read=%d magic=0x%08X ver=%u), using defaults",
             ret, (unsigned)tmp.magic, (unsigned)tmp.version);
        return;
    }
    config_clamp(&tmp);
    memcpy(&g_cfg, &tmp, sizeof(g_cfg));
    /* v1.5.2: a state file carrying a mode the hardware cannot deliver (1.5.1
     * shipped one console a 0x8700 file) is repaired here and written back, so
     * the boot apply cannot spend its whole budget on a mode that always fails. */
    if (hd_mode_repair(&g_cfg, "state file") || g_target_fixed) {
        if (g_target_fixed)
            klog("config: FORCE target 30 -> 60 (1.6.6 default: one vblank per wait at 30 Hz)");
        config_save();
    }
    klog("config: loaded mode_1080p=%u hd_mode=0x%04X item=%u fps_mode=%u target=%u inject=%u safe=%us delay=%ums wd=%ums",
         (unsigned)g_cfg.mode_1080p, (unsigned)g_cfg.hd_mode_code, (unsigned)g_cfg.settings_item_value,
         (unsigned)g_cfg.fps_mode, (unsigned)g_cfg.fps_target, (unsigned)g_cfg.fps_inject,
         (unsigned)g_cfg.safe_boot_seconds, (unsigned)g_cfg.boot_apply_delay_ms,
         (unsigned)g_cfg.watchdog_period_ms);
}

/* ------------------------------------------------------------------------- */
/* Title list (FORCE mode filter)                                             */
/* ------------------------------------------------------------------------- */

static void proc_clear(void)
{
    int i;
    for (i = 0; i < PROC_ENTRIES; i++) {
        g_procs[i].pid = 0;
        g_procs[i].allowed = 0;
        g_procs[i].override = OVR_NONE;
        g_procs[i].flags = 0;
        g_procs[i].acc = 0;
        g_procs[i].last_sync_us = 0;
        g_procs[i].cb_synced = 0;
        g_procs[i].created_us = 0;
        g_procs[i].checked_us = 0;
        g_procs[i].title[0] = 0;
    }
}

/* Recompute the FORCE-filter verdict of every live entry from its stored
 * title.  The table itself is never cleared at runtime (only at module_start):
 * a game suspended behind the Settings app keeps its cb_synced / last_sync_us,
 * so a callback-synced title is not double-paced after a mode change.
 * Caller holds g_tbl_mutex. */
static void proc_refilter(void)
{
    int i;
    uint32_t j;
    for (i = 0; i < PROC_ENTRIES; i++) {
        proc_entry_t *e = &g_procs[i];
        uint32_t allowed = 0;
        if (e->pid == 0)
            continue;
        for (j = 0; j < g_title_count && j < TITLE_LIST_MAX; j++) {
            if (strncmp(e->title, g_titles[j], TITLE_ID_LEN) == 0) {
                allowed = 1;
                break;
            }
        }
        e->allowed = allowed;
    }
}

static void title_list_load(void)
{
    static char buf[4096];
    SceUID fd;
    int len, i, start;
    uint32_t count = 0;
    int all = 0;

    tbl_lock();
    g_title_filter_active = 0;   /* disable the filter while we rebuild it */
    g_title_count = 0;
    memset(g_titles, 0, sizeof(g_titles));

    fd = ksceIoOpen(PSTV1080P_TITLES_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        proc_refilter();
        klog("titles: no list file, FORCE mode applies to all titles");
        tbl_unlock();
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
    proc_refilter();
    if (all || count == 0) {
        g_title_filter_active = 0;
        klog("titles: list has *ALL or no entries (%u), FORCE mode applies to all titles", (unsigned)count);
    } else {
        g_title_filter_active = 1;
        klog("titles: filter active with %u title id(s), first='%s'", (unsigned)count, g_titles[0]);
    }
    tbl_unlock();
}

/* Slow path, once per new pid: resolve the title id and decide.  Runs in the
 * caller's context of a display syscall with g_tbl_mutex held: one sysroot
 * lookup, one small file read, one log line. */
static const char *ovr_name(uint32_t m)
{
    switch (m) {
    case OVR_OFF:       return "off";
    case OVR_SCALE:     return "scale";
    case OVR_FRAMESKIP: return "frameskip";
    case OVR_NOWAIT:    return "nowait";
    case OVR_FORCE:     return "force";
    default:            return "none";
    }
}

/* "rule+extra+extra" for the log ("none" when nothing is set). */
static const char *ovr_desc(uint32_t mode, uint32_t flags, char *buf, int len)
{
    int n = 0;
    buf[0] = 0;
    if (mode != OVR_NONE)
        n += snprintf(buf + n, (unsigned)(len - n), "%s", ovr_name(mode));
    if (flags & OVRF_NOVSYNC)  n += snprintf(buf + n, (unsigned)(len - n), "%snovsync",  n ? "+" : "");
    if (flags & OVRF_INJECT)   n += snprintf(buf + n, (unsigned)(len - n), "%sinject",   n ? "+" : "");
    if (flags & OVRF_SPOOF720) n += snprintf(buf + n, (unsigned)(len - n), "%sspoof720", n ? "+" : "");
    if (flags & OVRF_TRACE)    n += snprintf(buf + n, (unsigned)(len - n), "%strace",    n ? "+" : "");
    if (n == 0)
        snprintf(buf, (unsigned)len, "none");
    return buf;
}

static int str_ieq(const char *a, int alen, const char *b)
{
    int i;
    for (i = 0; i < alen; i++) {
        char c = a[i], d = b[i];
        if (d == 0) return 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != d) return 0;
    }
    return b[alen] == 0;
}

/* Load ur0:data/pstv1080p/pstv1080p_games.txt.  Lines: "TITLEID mode", '#' comments.
 * Cheap enough to be called on every new process (once per game start), so
 * edits take effect on the next launch without a reboot.  Caller holds
 * g_tbl_mutex (the parse buffer and g_games[] are shared). */
static void games_list_load(int do_log)
{
    static char buf[4096];
    SceUID fd;
    int len, i, start;
    uint32_t count = 0, bad = 0;

    fd = ksceIoOpen(PSTV1080P_GAMES_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        g_games_count = 0;
        if (do_log)
            klog("games: no override file (%s)", PSTV1080P_GAMES_PATH);
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
            int s0 = start, e = i, t_end, m_start, m_end;
            uint32_t mode = OVR_NONE, flags = 0;
            int unknown = 0, ntok = 0;
            start = i + 1;
            while (s0 < e && (buf[s0] == ' ' || buf[s0] == '\t'))
                s0++;
            while (e > s0 && (buf[e - 1] == ' ' || buf[e - 1] == '\t'))
                e--;
            if (s0 >= e || buf[s0] == '#')
                continue;
            t_end = s0;
            while (t_end < e && buf[t_end] != ' ' && buf[t_end] != '\t')
                t_end++;
            m_start = t_end;
            while (m_start < e && (buf[m_start] == ' ' || buf[m_start] == '\t'))
                m_start++;
            m_end = m_start;
            while (m_end < e && buf[m_end] != ' ' && buf[m_end] != '\t')
                m_end++;
            if ((t_end - s0) != 9 || m_start >= e) {
                /* Vita title ids are exactly 9 characters (e.g. PCSE00429). */
                bad++;
                if (do_log)
                    klog("games: ignored line '%.*s' (title id must be 9 characters, then a mode)",
                         (int)(e - s0), buf + s0);
                continue;
            }
            /* 1.6.3: every word after the id is a pacing rule (last one wins)
             * or an extra flag; any unknown word rejects the line. */
            while (m_start < e) {
                int tl = m_end - m_start;
                if      (str_ieq(buf + m_start, tl, "off"))       mode = OVR_OFF;
                else if (str_ieq(buf + m_start, tl, "scale"))     mode = OVR_SCALE;
                else if (str_ieq(buf + m_start, tl, "frameskip")) mode = OVR_FRAMESKIP;
                else if (str_ieq(buf + m_start, tl, "nowait"))    mode = OVR_NOWAIT;
                else if (str_ieq(buf + m_start, tl, "force"))     mode = OVR_FORCE;
                else if (str_ieq(buf + m_start, tl, "inject"))    flags |= OVRF_INJECT;
                else if (str_ieq(buf + m_start, tl, "novsync"))   flags |= OVRF_NOVSYNC;
                else if (str_ieq(buf + m_start, tl, "spoof720"))  flags |= OVRF_SPOOF720;
                else if (str_ieq(buf + m_start, tl, "trace"))     flags |= OVRF_TRACE;
                else { unknown = 1; break; }
                ntok++;
                m_start = m_end;
                while (m_start < e && (buf[m_start] == ' ' || buf[m_start] == '\t'))
                    m_start++;
                m_end = m_start;
                while (m_end < e && buf[m_end] != ' ' && buf[m_end] != '\t')
                    m_end++;
            }
            if (unknown || ntok == 0) {
                bad++;
                if (do_log)
                    klog("games: ignored line '%.*s' (unknown word)", (int)(e - s0), buf + s0);
                continue;
            }
            /* "novsync" with no rule = the novsync.suprx behaviour (waits too). */
            if (mode == OVR_NONE && (flags & OVRF_NOVSYNC))
                mode = OVR_NOWAIT;
            if (count < GAMES_LIST_MAX) {
                memset(g_games[count].title, 0, TITLE_ID_LEN);
                memcpy(g_games[count].title, buf + s0, (unsigned int)(t_end - s0));
                g_games[count].mode = mode;
                g_games[count].flags = flags;
                count++;
            } else {
                bad++;
                if (do_log)
                    klog("games: ignored line '%.*s' (more than %u entries)", (int)(e - s0), buf + s0,
                         (unsigned)GAMES_LIST_MAX);
            }
        }
    }
    g_games_count = count;
    if (do_log) {
        /* One line: "games: N override(s): PCSE00429=trace PCSE01262=frameskip ..." */
        char line[220];
        int pos = 0;
        uint32_t k;
        pos = snprintf(line, sizeof(line), "games: %u override(s), %u line(s) ignored:", (unsigned)count, (unsigned)bad);
        if (pos < 0 || pos >= (int)sizeof(line))
            pos = 0;
        for (k = 0; k < count; k++) {
            int n;
            if (pos > (int)sizeof(line) - 24) {          /* flush, continue on a new line */
                klog("%s", line);
                pos = snprintf(line, sizeof(line), "games:  ");
                if (pos < 0) pos = 0;
            }
            char d[48];
            n = snprintf(line + pos, sizeof(line) - (unsigned)pos, " %s=%s", g_games[k].title,
                         ovr_desc(g_games[k].mode, g_games[k].flags, d, sizeof(d)));
            if (n < 0 || pos + n >= (int)sizeof(line))
                break;
            pos += n;
        }
        klog("%s", line);
    }
}

/* 1 if any per-title override asks for "trace": then the lifecycle of every
 * process is logged so the traced title can be compared with a working one. */
static int trace_configured(void)
{
    uint32_t i;
    for (i = 0; i < g_games_count && i < GAMES_LIST_MAX; i++)
        if (g_games[i].flags & OVRF_TRACE)
            return 1;
    return 0;
}

/* Fill a fresh per-process entry: title id, FORCE-filter verdict, override. */
/* Fill a fresh per-process entry: title id, FORCE-filter verdict, override.
 * Runs inside the FIRST display syscall of a process, holding g_tbl_mutex.
 * Keep it to the sysroot title call, the override lists and one log line:
 * 1.6.6 briefly queried the display driver and taiHEN's module list here and
 * games stopped being resolved at all (no "process:" line, no pacing). */
static void proc_resolve(proc_entry_t *e, SceUID pid)
{
    uint32_t i;

    memset(e->title, 0, TITLE_ID_LEN);
    if (ksceKernelSysrootGetProcessTitleId(pid, e->title, TITLE_ID_LEN - 1) < 0)
        e->title[0] = 0;
    e->title[TITLE_ID_LEN - 1] = 0;

    e->allowed = 0;
    for (i = 0; i < g_title_count && i < TITLE_LIST_MAX; i++) {
        if (strncmp(e->title, g_titles[i], TITLE_ID_LEN) == 0) {
            e->allowed = 1;
            break;
        }
    }

    games_list_load(0);
    e->override = OVR_NONE;
    e->flags = 0;
    for (i = 0; i < g_games_count && i < GAMES_LIST_MAX; i++) {
        if (strncmp(e->title, g_games[i].title, TITLE_ID_LEN) == 0) {
            e->override = g_games[i].mode;
            e->flags = g_games[i].flags;
            break;
        }
    }
    /* Quiet mode: games and homebrew only (system apps NPXS* and the shell
     * are noise for the user; they still show up in verbose mode). */
    if (g_verbose || (pid != g_shell_pid && strncmp(e->title, "NPXS", 4) != 0)) {
        char d[48];
        klog("process: pid=0x%08X title=%s override=%s%s hz=%u", (unsigned)pid,
             e->title[0] ? e->title : "?", ovr_desc(e->override, e->flags, d, sizeof(d)),
             (pid == g_shell_pid) ? " (shell, never paced)" : "", (unsigned)g_refresh_hz);
    }
}

/* v1.4.1: process lifecycle events.  A finished process's entry is dropped
 * at once so a later process that receives the same pid never inherits its
 * title/override (hardware symptom: a game with no process: line at all).
 * Only the pid is unpublished, with a CAS and no lock: the callbacks run on
 * the creating/exiting/killing thread without g_tbl_mutex, and the miss path
 * of proc_lookup re-initialises every other field before it publishes a pid,
 * so plain stores here could clobber a slot that the miss path has just
 * evicted and re-assigned (a spoof720 title published with override NONE). */
static SceUID g_procevent_uid = -1;

static void proc_forget(SceUID pid)
{
    int i;
    if (pid <= 0)
        return;
    for (i = 0; i < PROC_ENTRIES; i++) {
        SceUID expect = pid;
        /* Unpublish the slot only if it still belongs to this pid.  If the
         * miss path has already reclaimed it (pid 0, or a new pid after the
         * resolve) the CAS fails and nothing is touched.  A pid never has
         * two entries, so stop at the first match. */
        if (__atomic_compare_exchange_n(&g_procs[i].pid, &expect, 0, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
}

/* "trace" diagnostics: lifecycle of the traced process, timed from create. */
static volatile SceUID g_trace_pid = 0;
static SceInt64 g_trace_t0 = 0;

static int pid_is_trace_title(SceUID pid, char *title_out)
{
    char tid[TITLE_ID_LEN];
    uint32_t i;
    memset(tid, 0, sizeof(tid));
    if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid) - 1) < 0)
        return 0;
    tid[sizeof(tid) - 1] = 0;
    for (i = 0; i < g_games_count && i < GAMES_LIST_MAX; i++) {
        if ((g_games[i].flags & OVRF_TRACE) && strncmp(tid, g_games[i].title, TITLE_ID_LEN) == 0) {
            if (title_out)
                memcpy(title_out, tid, TITLE_ID_LEN);
            return 1;
        }
    }
    return 0;
}

/* 1.6.2: every 5 s while a "trace" title runs, one line with how many
 * display calls of each kind it made in that interval.  This shows what the
 * game actually synchronises on (WaitVblank*, WaitSetFrameBuf*, the flip's
 * sync flag, GetVcount, a registered vblank callback), i.e. which pacing
 * override can work for it at all.  Runs on the plugin thread only. */
static SceUID   g_prof_pid = 0;
static SceInt64 g_prof_last_us = 0;
static uint32_t g_prof_snap[PF_COUNT_];
#define PROFILE_INTERVAL_US (5000u * 1000u)

static proc_entry_t *proc_find(SceUID pid)
{
    int i;
    for (i = 0; i < PROC_ENTRIES; i++)
        if (g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

static void trace_profile_tick(void)
{
    SceUID pid = g_trace_pid;
    proc_entry_t *e;
    uint32_t now[PF_COUNT_], d[PF_COUNT_];
    SceInt64 t = now_us();
    unsigned ms;
    int i;

    if (pid == 0) {
        g_prof_pid = 0;
        return;
    }
    e = proc_find(pid);
    if (g_prof_pid != pid) {
        g_prof_pid = pid;
        g_prof_last_us = t;
        for (i = 0; i < PF_COUNT_; i++)
            g_prof_snap[i] = e ? e->cnt[i] : 0;
        return;
    }
    if (t - g_prof_last_us < (SceInt64)PROFILE_INTERVAL_US)
        return;
    ms = (unsigned)((t - g_prof_last_us) / 1000);
    g_prof_last_us = t;
    if (!e) {
        klog("trace: display profile: no display call from the game yet (%u ms)", ms);
        return;
    }
    for (i = 0; i < PF_COUNT_; i++) {
        now[i] = e->cnt[i];
        d[i] = now[i] - g_prof_snap[i];
        g_prof_snap[i] = now[i];
    }
    {
        char od[48];
        klog("trace: display %u ms: flips next=%u imm=%u (%u/s) | WaitVblank=%u cb=%u multi=%u (max n=%u) | WaitSetFB=%u multi=%u | GetVcount=%u | vblank callback=%s | override=%s hz=%u",
             ms, d[PF_SETFB_NEXT], d[PF_SETFB_IMM],
             ms ? (unsigned)(((uint64_t)(d[PF_SETFB_NEXT] + d[PF_SETFB_IMM]) * 1000u) / ms) : 0u,
             d[PF_WAITVB], d[PF_WAITVBCB], d[PF_WAITVBMULTI], now[PF_MULTI_MAX],
             d[PF_WAITSETFB], d[PF_WAITSETFBMULTI], d[PF_GETVCOUNT],
             e->cb_synced ? "yes" : "no", ovr_desc(e->override, e->flags, od, sizeof(od)), (unsigned)g_refresh_hz);
    }
}

static void trace_event(SceUID pid, const char *what)
{
    char tid[TITLE_ID_LEN];
    if (g_trace_pid == pid && pid != 0) {
        klog("trace: pid=0x%08X %s (+%d ms)", (unsigned)pid, what, (int)((now_us() - g_trace_t0) / 1000));
        return;
    }
    if (g_trace_pid == 0 && pid_is_trace_title(pid, tid)) {
        g_trace_pid = pid;
        g_trace_t0 = now_us();
        klog("trace: %s pid=0x%08X %s (t0)", tid, (unsigned)pid, what);
    }
}

/* v1.4.6: one line per lifecycle event of EVERY process (a handful of lines
 * per app launch), so a working title's sequence can be compared with a
 * failing one; the raw event parameters are logged in case they carry a reason. */
static void lifecycle_log(const char *what, SceUID pid, const int *words, int nwords, int extra)
{
    char tid[TITLE_ID_LEN];
    if (!g_verbose && !trace_configured())
        return;                     /* v1.5: only while someone is tracing a title */
    memset(tid, 0, sizeof(tid));
    if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid) - 1) < 0)
        tid[0] = 0;
    tid[sizeof(tid) - 1] = 0;
    if (nwords >= 4)
        klog("proc: %s pid=0x%08X title=%s params=[%08X %08X %08X %08X] a=%d", what, (unsigned)pid,
             tid[0] ? tid : "?", (unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3], extra);
    else
        klog("proc: %s pid=0x%08X title=%s a=%d", what, (unsigned)pid, tid[0] ? tid : "?", extra);
}

static void param1_words(SceProcEventInvokeParam1 *p, int *w)
{
    w[0] = w[1] = w[2] = w[3] = 0;
    if (p) { w[0] = (int)p->size; w[1] = p->unk_0x04; w[2] = p->unk_0x08; w[3] = p->unk_0x0C; }
}

static int procevent_create(SceUID pid, SceProcEventInvokeParam2 *a2, int a3)
{
    int w[4] = { 0, 0, 0, 0 };
    proc_forget(pid);       /* a pid being (re)used: never start from a stale entry */
    if (a2) { w[0] = (int)a2->size; w[1] = (int)a2->pid; w[2] = a2->unk_0x08; w[3] = a2->unk_0x0C; }
    lifecycle_log("create", pid, w, 4, a3);
    trace_event(pid, "created");
    return 0;
}

static int procevent_start(SceUID pid, int event_type, SceProcEventInvokeParam1 *a3, int a4)
{
    int w[4];
    param1_words(a3, w);
    /* "start" fires once per event_type (0x10000.., dozens per app): only the
     * traced process or verbose mode gets these lines (1.5.1). */
    if (g_verbose || (g_trace_pid == pid && pid != 0))
        lifecycle_log("start", pid, w, 4, event_type);
    if (g_trace_pid == pid && pid != 0)
        klog("trace: pid=0x%08X started, event_type=%d (+%d ms)", (unsigned)pid, event_type,
             (int)((now_us() - g_trace_t0) / 1000));
    else
        trace_event(pid, "started");
    return 0;
}

static int procevent_exit(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{
    int w[4];
    param1_words(a2, w);
    lifecycle_log("exit", pid, w, 4, a3);
    proc_forget(pid);
    if (g_trace_pid == pid && pid != 0) {
        klog("trace: pid=0x%08X EXITED by itself (+%d ms)", (unsigned)pid, (int)((now_us() - g_trace_t0) / 1000));
        g_trace_pid = 0;
    }
    return 0;
}

static int procevent_kill(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{
    int w[4];
    param1_words(a2, w);
    lifecycle_log("kill", pid, w, 4, a3);
    proc_forget(pid);
    if (g_trace_pid == pid && pid != 0) {
        klog("trace: pid=0x%08X KILLED by the system (+%d ms)", (unsigned)pid, (int)((now_us() - g_trace_t0) / 1000));
        g_trace_pid = 0;
    }
    return 0;
}

static const SceProcEventHandler g_procevent_handler = {
    .size = sizeof(SceProcEventHandler),
    .create = procevent_create,
    .exit = procevent_exit,
    .kill = procevent_kill,
    .stop = NULL,
    .start = procevent_start,
    .switch_process = NULL,
};

/* pid -> entry.  Hits are a lock-free O(PROC_ENTRIES) scan.  A miss (first
 * display syscall of a process) takes g_tbl_mutex, re-checks the table (a
 * sibling thread of the same process may have just published this pid),
 * takes a free slot or evicts the entry idle for the longest, resolves it
 * once (one sysroot call, a small file read and one log line) and only then
 * publishes the pid.  A pid never has two entries, so cb_synced / acc /
 * override cannot be split across slots, and a live game is not pushed out
 * by newly started background processes. */
#define PROC_RECHECK_US               (3000000LL)   /* 1.6.6: title re-check period on the hit path */

static proc_entry_t *proc_lookup(SceUID pid, int create)
{
    uint32_t i, slot;
    proc_entry_t *e;
    SceInt64 oldest = 0;
    for (i = 0; i < PROC_ENTRIES; i++) {
        if (g_procs[i].pid == pid) {
            e = &g_procs[i];
            if (create) {
                /* 1.6.6: a pid can be reused by a new process before any
                 * lifecycle event dropped the old entry (or if that handler
                 * is not installed).  Every few seconds compare the title the
                 * kernel reports with the one stored; on a mismatch resolve
                 * the entry again.  One sysroot call per 3 s per process. */
                SceInt64 now = now_us();
                if (now - e->checked_us > PROC_RECHECK_US) {
                    char t[TITLE_ID_LEN];
                    e->checked_us = now;
                    memset(t, 0, sizeof(t));
                    if (ksceKernelSysrootGetProcessTitleId(pid, t, TITLE_ID_LEN - 1) >= 0
                        && strncmp(t, e->title, TITLE_ID_LEN) != 0) {
                        tbl_lock();
                        if (e->pid == pid) {
                            e->pid = 0;
                            e->acc = 0;
                            e->last_sync_us = 0;
                            e->cb_synced = 0;
                            e->created_us = now;
                            proc_resolve(e, pid);
                            e->pid = pid;
                        }
                        tbl_unlock();
                    }
                }
            }
            return e;
        }
    }
    if (!create)
        return NULL;

    tbl_lock();
    for (i = 0; i < PROC_ENTRIES; i++) {
        if (g_procs[i].pid == pid) {
            tbl_unlock();
            return &g_procs[i];
        }
    }
    slot = 0;
    for (i = 0; i < PROC_ENTRIES; i++) {
        SceInt64 t;
        if (g_procs[i].pid == 0) {
            slot = i;
            break;
        }
        t = g_procs[i].last_sync_us;
        if (g_procs[i].created_us > t)
            t = g_procs[i].created_us;
        if (i == 0 || t < oldest) {
            oldest = t;
            slot = i;
        }
    }
    e = &g_procs[slot];
    e->pid = 0;                 /* invalidate first: readers never pair a new pid with old data */
    e->acc = 0;
    e->last_sync_us = 0;
    e->cb_synced = 0;
    e->created_us = now_us();
    e->checked_us = e->created_us;
    proc_resolve(e, pid);
    e->pid = pid;
    tbl_unlock();
    return e;
}

static inline void proc_mark_sync(proc_entry_t *e)
{
    if (e)
        e->last_sync_us = now_us();
}

/* Effective pacing for one process. */
#define PACE_FRAMESKIP                3u
#define PACE_NOWAIT                   4u
typedef struct {
    uint32_t mode;      /* PSTV1080P_FPS_OFF / SCALE / FORCE, PACE_FRAMESKIP, PACE_NOWAIT */
    uint32_t inject;    /* 0 off, 1 auto, 2 always (explicit: honoured with every rule) */
    uint32_t novsync;   /* 1.6.3: make every flip IMMEDIATE (novsync.suprx) */
} pace_t;

static inline void pace_base(SceUID pid, proc_entry_t **pe, pace_t *out);

static inline void pace_for(SceUID pid, proc_entry_t **pe, pace_t *out)
{
    pace_base(pid, pe, out);
    if (*pe) {                              /* 1.6.3 extras on top of the rule */
        uint32_t f = (*pe)->flags;
        if (f & OVRF_INJECT)  out->inject = 2;
        if (f & OVRF_NOVSYNC) out->novsync = 1;
    }
}

static inline void pace_base(SceUID pid, proc_entry_t **pe, pace_t *out)
{
    proc_entry_t *e;

    out->mode = PSTV1080P_FPS_OFF;
    out->inject = 0;
    out->novsync = 0;
    *pe = NULL;
    if (pid <= 0 || pid == KERNEL_PID || pid == shell_pid_now())
        return;
    e = proc_lookup(pid, 1);
    *pe = e;

    switch (e->override) {
    case OVR_OFF:
        return;
    case OVR_SCALE:
        out->mode = PSTV1080P_FPS_SCALE;
        out->inject = g_cfg.fps_inject;
        return;
    case OVR_FRAMESKIP:
        out->mode = PACE_FRAMESKIP;     /* such games sync themselves: no automatic inject */
        return;
    case OVR_NOWAIT:
        out->mode = PACE_NOWAIT;
        return;
    case OVR_FORCE:
        out->mode = PSTV1080P_FPS_FORCE;
        out->inject = g_cfg.fps_inject;
        return;
    default:                            /* no rule: the global rules below */
        break;
    }

    if (g_cfg.fps_mode == PSTV1080P_FPS_OFF)
        return;
    if (g_cfg.fps_mode == PSTV1080P_FPS_FORCE && g_title_filter_active && !e->allowed)
        return;
    out->mode = g_cfg.fps_mode;
    out->inject = g_cfg.fps_inject;
}

/* Frameskip (fractional vsync): a request of n vblanks earns n*hz credits in
 * 1/60-vblank units; the wait really happens only when >= 60 credits are
 * banked.  At 30 Hz a 1-vblank request alternates skip/wait, so a game that
 * advances its logic once per wait runs 60 logic frames per second and shows
 * every second one.  At 60 Hz it is the identity.  Returns the real count,
 * 0 = return to the game immediately. */
static inline unsigned int frameskip_count(proc_entry_t *e, unsigned int n)
{
    uint32_t hz = g_refresh_hz ? g_refresh_hz : 60;
    uint32_t old, acc, w, nw;
    if (hz >= 60 || n > 0x00FFFFFFu || !e)
        return n;
    /* Two threads of one process may wait at the same instant: update the
     * accumulator with a CAS (ldrex/strex) so no credit is counted twice or
     * lost.  acc < 60 + n*hz < 2^31, no overflow. */
    old = e->acc;
    do {
        acc = old + n * hz;
        w = acc / 60;
        nw = acc - w * 60;
    } while (!__atomic_compare_exchange_n(&e->acc, &old, nw, 0,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return w;
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
/* v1.4.4: a pending apply runs from the display syscall of ANY user process
 * (the export only refuses kernel-thread callers).  Inside the Settings app
 * SceShell is not drawing, so restricting this to the shell made retries
 * time out there (1080i -> 1080p "does not change"). */
static void run_mode_request(void);

static inline void shell_apply_check_pid(SceUID pid)
{
    if (g_apply_pending && pid > 0 && pid != KERNEL_PID)
        run_pending_apply(pid == g_shell_pid ? "shell" : "user process");
    if (g_mode_request_pending && pid > 0 && pid != KERNEL_PID)
        run_mode_request();
}

/* Common prologue of the wait hooks. */
#define WAIT_PROLOGUE(pid, e, pc) \
    SceUID pid = ksceKernelGetProcessId(); \
    proc_entry_t *e; \
    pace_t pc; \
    shell_apply_check_pid(pid); \
    pace_for(pid, &e, &pc); \
    proc_mark_sync(e)

static int hook_WaitVblankStartMulti(unsigned int vcount)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITVBMULTI);
    PF_MAX(e, PF_MULTI_MAX, vcount);
    if (pc.mode == PACE_NOWAIT)
        return 0;
    if (pc.mode == PACE_FRAMESKIP) {
        vcount = frameskip_count(e, vcount);
        if (vcount == 0)
            return 0;
    } else if (pc.mode != PSTV1080P_FPS_OFF) {
        vcount = pace_vcount(pc.mode, vcount);
    }
    ret = HOOK_NEXT(hook_WaitVblankStartMulti, g_pacing_ref[PH_WAITVBLANKMULTI], vcount);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitVblankStartMultiCB(unsigned int vcount)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITVBMULTI);
    PF_MAX(e, PF_MULTI_MAX, vcount);
    if (pc.mode == PACE_NOWAIT) {
        ksceKernelCheckCallback();  /* the CB variants are the caller's callback-delivery point */
        return 0;
    }
    if (pc.mode == PACE_FRAMESKIP) {
        vcount = frameskip_count(e, vcount);
        if (vcount == 0)
            return 0;
    } else if (pc.mode != PSTV1080P_FPS_OFF) {
        vcount = pace_vcount(pc.mode, vcount);
    }
    ret = HOOK_NEXT(hook_WaitVblankStartMultiCB, g_pacing_ref[PH_WAITVBLANKMULTICB], vcount);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitVblankStart(void)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITVB);
    if (pc.mode == PACE_NOWAIT)
        return 0;
    if (pc.mode == PACE_FRAMESKIP) {
        unsigned int w = frameskip_count(e, 1);
        if (w == 0)
            return 0;
        if (w > 1) {
            ret = ksceDisplayWaitVblankStartMulti(w);
            proc_mark_sync(e);
            return ret;
        }
    } else if (pc.mode == PSTV1080P_FPS_FORCE) {
        unsigned int interval = force_interval();
        if (interval > 1) {
            ret = ksceDisplayWaitVblankStartMulti(interval);
            proc_mark_sync(e);
            return ret;
        }
    }
    /* SCALE: a 1-vblank request stays 1 -> pass through. */
    ret = HOOK_NEXT(hook_WaitVblankStart, g_pacing_ref[PH_WAITVBLANK]);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitVblankStartCB(void)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITVBCB);
    if (pc.mode == PACE_NOWAIT) {
        ksceKernelCheckCallback();  /* the CB variants are the caller's callback-delivery point */
        return 0;
    }
    if (pc.mode == PACE_FRAMESKIP) {
        unsigned int w = frameskip_count(e, 1);
        if (w == 0)
            return 0;
        if (w > 1) {
            ret = ksceDisplayWaitVblankStartMultiCB(w);
            proc_mark_sync(e);
            return ret;
        }
    } else if (pc.mode == PSTV1080P_FPS_FORCE) {
        unsigned int interval = force_interval();
        if (interval > 1) {
            ret = ksceDisplayWaitVblankStartMultiCB(interval);
            proc_mark_sync(e);
            return ret;
        }
    }
    ret = HOOK_NEXT(hook_WaitVblankStartCB, g_pacing_ref[PH_WAITVBLANKCB]);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitSetFrameBufMulti(unsigned int vcount)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITSETFBMULTI);
    PF_MAX(e, PF_MULTI_MAX, vcount);
    if (pc.mode == PACE_NOWAIT)
        return 0;
    if (pc.mode == PACE_FRAMESKIP) {
        vcount = frameskip_count(e, vcount);
        if (vcount == 0)
            return 0;
    } else if (pc.mode != PSTV1080P_FPS_OFF) {
        vcount = pace_vcount(pc.mode, vcount);
    }
    ret = HOOK_NEXT(hook_WaitSetFrameBufMulti, g_pacing_ref[PH_WAITSETFBMULTI], vcount);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitSetFrameBufMultiCB(unsigned int vcount)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITSETFBMULTI);
    PF_MAX(e, PF_MULTI_MAX, vcount);
    if (pc.mode == PACE_NOWAIT) {
        ksceKernelCheckCallback();  /* the CB variants are the caller's callback-delivery point */
        return 0;
    }
    if (pc.mode == PACE_FRAMESKIP) {
        vcount = frameskip_count(e, vcount);
        if (vcount == 0)
            return 0;
    } else if (pc.mode != PSTV1080P_FPS_OFF) {
        vcount = pace_vcount(pc.mode, vcount);
    }
    ret = HOOK_NEXT(hook_WaitSetFrameBufMultiCB, g_pacing_ref[PH_WAITSETFBMULTICB], vcount);
    proc_mark_sync(e);
    return ret;
}

/* WaitSetFrameBuf(CB): "wait until my last flip was shown" = a 1-vblank class
 * wait for pacing purposes; otherwise pass-through + tracking. */
static int hook_WaitSetFrameBuf(void)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITSETFB);
    if (pc.mode == PACE_NOWAIT)
        return 0;
    if (pc.mode == PACE_FRAMESKIP && frameskip_count(e, 1) == 0)
        return 0;
    ret = HOOK_NEXT(hook_WaitSetFrameBuf, g_pacing_ref[PH_WAITSETFB]);
    proc_mark_sync(e);
    return ret;
}

static int hook_WaitSetFrameBufCB(void)
{
    int ret;
    WAIT_PROLOGUE(pid, e, pc);
    PF_INC(e, PF_WAITSETFB);
    if (pc.mode == PACE_NOWAIT) {
        ksceKernelCheckCallback();  /* the CB variants are the caller's callback-delivery point */
        return 0;
    }
    if (pc.mode == PACE_FRAMESKIP && frameskip_count(e, 1) == 0)
        return 0;
    ret = HOOK_NEXT(hook_WaitSetFrameBufCB, g_pacing_ref[PH_WAITSETFBCB]);
    proc_mark_sync(e);
    return ret;
}

/* Vcount hooks: tracking, plus for "frameskip" titles a count scaled to what
 * the game expects at 60 Hz (x2 at 30 Hz), so games that measure elapsed
 * frames by reading the counter keep their speed.  The native counter is a
 * plain "vblanks since boot" int, so the scaled value stays monotonic too
 * (no artificial 16-bit wrap: a game computing now - last must never see a
 * negative delta the stock driver would not produce). */
static inline int vcount_for(proc_entry_t *e, int v)
{
    uint32_t hz = g_refresh_hz ? g_refresh_hz : 60;
    if (!e || e->override != OVR_FRAMESKIP || hz >= 60 || v < 0)
        return v;
    if ((uint32_t)v > 0x03FFFFFFu)          /* keep v*60 inside 32 bits (years of uptime) */
        return v;
    return (int)(((uint32_t)v) * 60u / hz);
}

static int hook_GetVcount(void)
{
    SceUID pid = ksceKernelGetProcessId();
    proc_entry_t *e = NULL;
    int v;
    if (pid > 0 && pid != KERNEL_PID && pid != shell_pid_now()) {
        e = proc_lookup(pid, 1);
        proc_mark_sync(e);
        PF_INC(e, PF_GETVCOUNT);
    }
    v = HOOK_NEXT(hook_GetVcount, g_pacing_ref[PH_GETVCOUNT]);
    return vcount_for(e, v);
}

static int hook_GetVcountInternal(int head)
{
    SceUID pid = ksceKernelGetProcessId();
    proc_entry_t *e = NULL;
    int v;
    if (pid > 0 && pid != KERNEL_PID && pid != shell_pid_now()) {
        e = proc_lookup(pid, 1);
        proc_mark_sync(e);
        PF_INC(e, PF_GETVCOUNT);
    }
    v = HOOK_NEXT(hook_GetVcountInternal, g_pacing_ref[PH_GETVCOUNTINT], head);
    return vcount_for(e, v);
}

static int hook_RegisterVblankStartCallback(SceUID uid)
{
    SceUID pid = ksceKernelGetProcessId();
    proc_entry_t *e = NULL;
    int ret;
    if (pid > 0 && pid != KERNEL_PID && pid != shell_pid_now()) {
        e = proc_lookup(pid, 1);
        e->cb_synced = 1;            /* this process syncs through a vblank callback: never inject */
    }
    ret = HOOK_NEXT(hook_RegisterVblankStartCallback, g_pacing_ref[PH_REGVBLANKCB], uid);
    return ret;
}

/* v1.4 "spoof720": for the listed titles, the two display-information
 * queries a game typically makes at start-up answer as if the HDMI head were
 * 720p60 (screen mode 0x8600, 1280x720, progressive, 59.94 Hz, maximum
 * framebuffer 960x544).  Hardware finding: Tales of Hearts R (PCSE00429)
 * crashes (C2-12828-1) before its first frame under a 1080p30 head and runs
 * under 720p, so it acts on what these queries return.  Pass-through for
 * every other title.  User pointers are only touched through
 * ksceKernelCopyFromUser/CopyToUser after the original call succeeded. */
typedef struct {
    uint32_t size;
    uint32_t screenMode;
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t scanMode;
    uint32_t fps_bits;          /* float 59.94f as bits: 0x426FC28F */
} spoof_resinfo_t;              /* == SceDisplayResolutionInfo, 0x1C bytes on FW 3.60 */

static inline proc_entry_t *spoof_entry_for(SceUID pid)
{
    proc_entry_t *e;
    if (pid <= 0 || pid == KERNEL_PID || pid == shell_pid_now())
        return NULL;
    e = proc_lookup(pid, 1);
    return (e && (e->flags & OVRF_SPOOF720)) ? e : NULL;
}

static int hook_GetMaximumFrameBufResolution(uint32_t *pWidth, uint32_t *pHeight)
{
    SceUID pid = ksceKernelGetProcessId();
    int ret = HOOK_NEXT(hook_GetMaximumFrameBufResolution, g_pacing_ref[PH_GETMAXFBRES], pWidth, pHeight);
    proc_entry_t *e = spoof_entry_for(pid);
    if (ret >= 0 && e) {
        uint32_t ow = 0, oh = 0, sw = 960u, sh = 544u;
        if (pWidth)  ksceKernelCopyFromUser(&ow, pWidth, sizeof(ow));
        if (pHeight) ksceKernelCopyFromUser(&oh, pHeight, sizeof(oh));
        if (pWidth)  ksceKernelCopyToUser(pWidth, &sw, sizeof(sw));
        if (pHeight) ksceKernelCopyToUser(pHeight, &sh, sizeof(sh));
        if (!(e->acc & 0x80000000u)) {      /* log once per process (acc is unused by spoof720) */
            e->acc |= 0x80000000u;
            klog("spoof720: %s GetMaximumFrameBufResolution %ux%u -> %ux%u", e->title, ow, oh, sw, sh);
        }
    }
    return ret;
}

static int hook_GetResolutionInfoInternal(int head, void *pInfo, SceSize infoSize)
{
    SceUID pid = ksceKernelGetProcessId();
    int ret = HOOK_NEXT(hook_GetResolutionInfoInternal, g_pacing_ref[PH_GETRESINFO], head, pInfo, infoSize);
    proc_entry_t *e = spoof_entry_for(pid);
    if (ret >= 0 && e && pInfo && infoSize >= sizeof(spoof_resinfo_t)) {
        spoof_resinfo_t t;
        if (ksceKernelCopyFromUser(&t, pInfo, sizeof(t)) >= 0) {
            uint32_t om = t.screenMode, ow = t.width, oh = t.height;
            t.screenMode = PSTV1080P_MODE_720P60;
            t.width      = 1280u;
            t.height     = 720u;
            t.scanMode   = 0u;
            t.fps_bits   = 0x426FC28Fu;
            ksceKernelCopyToUser(pInfo, &t, sizeof(t));
            if (!(e->acc & 0x40000000u)) {
                e->acc |= 0x40000000u;
                klog("spoof720: %s GetResolutionInfoInternal(head %d) mode=0x%04X %ux%u -> 0x8600 1280x720 p59.94",
                     e->title, head, om, ow, oh);
            }
        }
    }
    return ret;
}

/* v1.4.1: sceDisplayGetRefreshRate.  Tracked for every process (it is often
 * the very first display call a game makes, so the process: line appears as
 * early as possible); for spoof720 titles the answer is forced to 59.94 Hz. */
static int hook_GetRefreshRate(float *pFps)
{
    SceUID pid = ksceKernelGetProcessId();
    int ret = HOOK_NEXT(hook_GetRefreshRate, g_pacing_ref[PH_GETREFRESHRATE], pFps);
    if (pid > 0 && pid != KERNEL_PID && pid != shell_pid_now()) {
        proc_entry_t *e = proc_lookup(pid, 1);
        if (e && ret >= 0 && pFps) {
            uint32_t ob = 0;
            ksceKernelCopyFromUser(&ob, pFps, sizeof(ob));
            if (e->flags & OVRF_SPOOF720) {
                uint32_t sb = 0x426FC28Fu;           /* 59.94f */
                ksceKernelCopyToUser(pFps, &sb, sizeof(sb));
            }
            if (!(e->acc & 0x20000000u) && (e->flags & OVRF_SPOOF720)) {
                e->acc |= 0x20000000u;
                klog("spoof720: %s GetRefreshRate bits=0x%08X -> 0x426FC28F (59.94)", e->title, ob);
            }
        }
    }
    return ret;
}

/* v1.4.3 "trace" diagnostics: memory block allocations and free-memory
 * queries of the traced title.  Pass-through (one pid compare) for every
 * other process; the traced pid is known from the process-create event so no
 * table lookup is needed here. */
static int hook_AllocMemBlock(const char *name, int type, SceSize size, void *opt)
{
    SceUID pid = ksceKernelGetProcessId();
    int ret = HOOK_NEXT(hook_AllocMemBlock, g_pacing_ref[PH_ALLOCMEMBLOCK], name, type, size, opt);
    if (pid != 0 && pid == g_trace_pid) {
        char nm[32];
        nm[0] = 0;
        if (name) {
            if (ksceKernelStrncpyFromUser(nm, name, sizeof(nm) - 1) < 0)
                nm[0] = 0;
            nm[sizeof(nm) - 1] = 0;
        }
        klog("trace: alloc '%s' type=0x%08X size=%u (%u KB) -> 0x%08X%s (+%d ms)",
             nm, (unsigned)type, (unsigned)size, (unsigned)(size >> 10), (unsigned)ret,
             ret < 0 ? " FAILED" : "", (int)((now_us() - g_trace_t0) / 1000));
    }
    return ret;
}

static int hook_GetFreeMemorySize(void *info)
{
    SceUID pid = ksceKernelGetProcessId();
    int ret = HOOK_NEXT(hook_GetFreeMemorySize, g_pacing_ref[PH_GETFREEMEM], info);
    if (pid != 0 && pid == g_trace_pid && ret >= 0 && info) {
        int32_t v[4] = { 0, 0, 0, 0 };   /* size, user, cdram, phycont */
        if (ksceKernelCopyFromUser(v, info, sizeof(v)) >= 0)
            klog("trace: free memory user=%d KB cdram=%d KB phycont=%d KB (+%d ms)",
                 v[1] >> 10, v[2] >> 10, v[3] >> 10, (int)((now_us() - g_trace_t0) / 1000));
    }
    return ret;
}

/* Adaptive inject (fps_inject == 1): after a frame flip, wait one refresh
 * period (FORCE: the forced interval) ONLY if this process showed no vsync
 * activity for more than 4.5 refresh periods, i.e. it does not sync by
 * itself.  Games that already wait for vblank are never double-waited (the
 * defect of Framecapper's Inject build).  inject == 2 injects always. */
static inline int inject_wanted(proc_entry_t *e, uint32_t inject)
{
    SceInt64 idle, limit;
    uint32_t hz;

    if (inject == 2u)
        return 1;
    if (inject != 1u || !e)
        return 0;
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
    proc_entry_t *e;
    pace_t pc;
    int ret;

    pace_for(pid, &e, &pc);
    PF_INC(e, sync ? PF_SETFB_NEXT : PF_SETFB_IMM);
    /* 1.6.3 "novsync": the flip never waits for the next frame, exactly what
     * novsync.suprx does; a game that throttles itself through the flip is
     * otherwise capped at the output rate no matter what the wait hooks do. */
    if (pc.novsync && sync != 0)
        sync = 0;                                   /* SCE_DISPLAY_SETBUF_IMMEDIATE */
    ret = HOOK_NEXT(hook_SetFrameBuf, g_pacing_ref[PH_SETFRAMEBUF], pFrameBuf, sync, pOpt);

    /* Inject: an explicit "inject" extra applies with any rule (Framecapper
     * Inject); the automatic kind only where the rule allows it. */
    if (pc.inject == 2u ||
        (pc.mode != PSTV1080P_FPS_OFF && pc.mode != PACE_NOWAIT && pc.mode != PACE_FRAMESKIP
         && pc.inject != 0u && inject_wanted(e, pc.inject))) {
        unsigned int n = (pc.mode == PSTV1080P_FPS_FORCE) ? force_interval() : 1u;
        ksceDisplayWaitVblankStartMulti(n);
        proc_mark_sync(e);
    }
    /* After the flip: run a scheduled HD apply from SceShell's context (A9). */
    shell_apply_check_pid(pid);
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
        /* Expected for "automatic" (the driver reports the concrete mode it
         * picked); worth a line for anything else. */
        if (do_log && (uint32_t)rb != mode && (mode != SETRES_AUTO || g_verbose))
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

static int hook_HdmiSetResolution(int mode, int known, int flag)
{
    int requested = mode;
    int ret;

    /* v1.5.0: the Settings plugin patches Sony's value->mode ladder in memory,
     * so when the user picks the 1080p entry Sony's OWN code asks for our
     * mode.  That request is the user's choice: apply it as-is (no hold, no
     * substitution).  If the head already shows it (the same entry selected
     * again) nothing is sent: re-selecting must not renegotiate the link. */
    if (!g_self_apply && (uint32_t)mode == g_cfg.hd_mode_code) {
        unsigned int cur = 0, pf = 0;
        g_last_system_raw = (uint32_t)mode;
        if (g_held) {
            g_held = 0;
            klog("avconfig: held request 0x%08X cancelled by a native 0x%04X request", (unsigned)g_held_mode, (unsigned)mode);
        }
        if (ksceDisplayGetOutputMode(HDMI_HEAD, &cur, &pf) >= 0 && hd_in_effect(cur)) {
            g_last_native_us = now_us();
            g_last_setres_ret = 0;
            klog("avconfig: native request 0x%04X already in effect (driver 0x%04X), not re-sent", (unsigned)mode, cur);
            return 0;
        }
        ret = HOOK_NEXT(hook_HdmiSetResolution, g_avconfig_ref, mode, known, flag);
        g_last_setres_ret = ret;
        g_last_sys_setres_us = now_us();
        g_last_native_us = g_last_sys_setres_us;
        klog("avconfig: native request 0x%04X from Settings (known=%d) ret=0x%08X", (unsigned)mode, known, (unsigned)ret);
        if (ret >= 0) {
            record_applied((uint32_t)mode, 1);
            /* 1.6.6: the user chose our entry through Sony's own code: that is
             * "1080p on" for the boot apply and the watchdog, whatever the
             * Settings plugin's registry hook managed to record. */
            if (!g_cfg.mode_1080p) {
                g_cfg.mode_1080p = 1;
                config_save();
                klog("avconfig: native 1080p selection recorded (mode_1080p=1)");
            }
        }
        return ret;
    }

    /* Remember what Sony's code wanted, but never our own code (the Settings
     * plugin or an HDMI re-plug may re-send it) and never garbage: this value
     * is handed back to SetResolution by the revert path. */
    if (!g_self_apply) {
        g_last_system_raw = (uint32_t)mode;
        if ((uint32_t)mode != g_cfg.hd_mode_code && mode_code_plausible((uint32_t)mode))
            g_last_system_mode = (uint32_t)mode;

        /* Hold the request (do not touch the display now) when the plugin's
         * decision may follow within a moment:
         *  - 1080p is on: any Sony request precedes either a SetMode1080p(0)
         *    (user picked a Sony mode -> that mode becomes the single
         *    transition) or nothing (we keep 1080p, request dropped);
         *  - 1080p is off and the request is "automatic": either the user
         *    picked our item (-> one direct transition to 1080p30) or
         *    "Automatic" (-> the held request is applied after the window). */
        if (g_avconfig_fn && (g_cfg.mode_1080p || (uint32_t)mode == SETRES_AUTO)) {
            g_held_mode = (uint32_t)mode;
            g_held_us = now_us();
            g_held = 1;
            klog("avconfig: request 0x%08X held (%u ms) to merge it with the user's choice",
                 (unsigned)mode, (unsigned)(HOLD_REQUEST_US / 1000u));
            return 0;
        }
    }

    if (g_cfg.mode_1080p && !g_self_apply) {
        mode = (int)g_cfg.hd_mode_code;
        known = 1;
    }

    ret = HOOK_NEXT(hook_HdmiSetResolution, g_avconfig_ref, mode, known, flag);
    g_last_setres_ret = ret;
    if (!g_self_apply)
        g_last_sys_setres_us = now_us();

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
    ret = g_avconfig_fn((int)mode, (mode == SETRES_AUTO) ? 0 : 1, 1);   /* same arguments as Sony's code */
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

    if (g_held) {
        g_held = 0;
        klog("apply: held request 0x%08X cancelled, switching directly to 0x%04X", (unsigned)g_held_mode, (unsigned)g_cfg.hd_mode_code);
    }

    gb = ksceDisplayGetOutputMode(HDMI_HEAD, &before, &pf);
    if (gb >= 0 && hd_in_effect(before)) {
        /* Already there: nothing to do, no extra HDMI renegotiation. */
        refresh_display_cache(0);
        g_apply_attempts = 0;
        apply_clear_schedule();
        return 0;
    }

    /* v1.5.0: Sony's own (patched) Settings code has just requested our mode
     * and the driver may simply not report it yet.  Never stack a second
     * request on it; the watchdog re-checks the readback in a moment. */
    if (g_last_native_us != 0 && g_last_setres_ret >= 0
        && (now_us() - g_last_native_us) < (SceInt64)NATIVE_RECENT_US) {
        klog("apply(%s): native request 0x%04X sent %d ms ago, not re-sent",
             why, (unsigned)g_cfg.hd_mode_code, (int)((now_us() - g_last_native_us) / 1000));
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

    /* Sony's Settings code switches the head to "automatic" right before the
     * plugin gets its turn; a request issued while the driver is still in
     * that transition is silently dropped.  Let it settle first (we are on a
     * user thread inside a syscall: sleeping here is fine). */
    {
        SceInt64 since = now_us() - g_last_sys_setres_us;
        /* Coming from 1080i the TV sees two changes on the same pixel clock
         * (1080i60 -> 720p -> 1080p30); give it 1.5 s on the intermediate mode
         * so it re-locks, otherwise 0.5 s (v1.4.6). */
        uint32_t settle = (g_last_system_mode == PSTV1080P_MODE_1080I60) ? (1500u * 1000u) : SYS_SETRES_SETTLE_US;
        if (g_last_sys_setres_us != 0 && since >= 0 && since < (SceInt64)settle)
            ksceKernelDelayThread((SceUInt)(settle - (uint32_t)since));
    }

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

    /* v1.5.2: the driver rejected the mode outright (0x803A0101 on 1.5.1's
     * corrupted 0x8700 config).  Retrying cannot help: repair the mode if it is
     * one we should never have held, otherwise stop instead of hammering the
     * HDMI link once per backoff step for the rest of the session. */
    if (ret < 0) {
        if (hd_mode_repair(&g_cfg, "driver refused it")) {
            config_save();
            g_apply_attempts = 0;
            g_apply_due_us = now_us();
            g_apply_pending = 1;      /* one clean attempt with the repaired mode */
            return 0;
        }
        klog("apply(%s): driver refused 0x%04X (0x%08X); not retrying this session",
             why, (unsigned)g_cfg.hd_mode_code, (unsigned)ret);
        apply_clear_schedule();
        return ret;
    }

    apply_schedule_retry();
    if (ret < 0)
        return ret;
    return 0;   /* the syscall itself succeeded; the retry schedule covers the rest */
}

/* Apply a released held request from a user display syscall (the export
 * refuses kernel-thread callers). */
static void run_mode_request(void)
{
    int ret;
    uint32_t mode;

    if (!g_mode_request_pending || g_in_apply)
        return;
    if (g_mutex >= 0 && ksceKernelTryLockMutex(g_mutex, 1) < 0)
        return;
    if (g_mode_request_pending && !g_in_apply) {
        mode = g_mode_request;
        g_mode_request_pending = 0;
        g_in_apply = 1;
        ret = call_set_resolution(mode);
        g_in_apply = 0;
        klog("avconfig: released request 0x%08X applied ret=0x%08X", (unsigned)mode, (unsigned)ret);
        refresh_display_cache(1);
    }
    if (g_mutex >= 0)
        ksceKernelUnlockMutex(g_mutex, 1);
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

    if (g_held) {
        /* The user just picked a Sony mode: Sony's request for it is on hold,
         * apply exactly that as the single transition. */
        sys = g_held_mode;
        g_held = 0;
        ret = call_set_resolution(sys);
        klog("revert: held request 0x%08X applied directly ret=0x%08X", (unsigned)sys, (unsigned)ret);
        if (ret >= 0 && mode_code_plausible(sys))
            record_applied(sys, !(g_hooks_ok & HOOK_BIT_AVCONFIG));
        else
            refresh_display_cache(1);
        return ret < 0 ? ret : 0;
    }
    if (g_last_system_raw == SETRES_AUTO) {
        ret = call_set_resolution(SETRES_AUTO);
        klog("revert: SetResolution(automatic) ret=0x%08X", (unsigned)ret);
        refresh_display_cache(1);
        return ret < 0 ? ret : 0;
    }

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
    g_apply_total = 0;          /* a deliberate selection always gets a fresh budget (v1.4.5) */
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
    if (g_cfg.watchdog_period_ms == 0) {
        unlock();
        return;
    }

    /* 1.6.6: the refresh-rate cache is corrected here no matter who changed
     * the output (Sony's Settings code through the native entry, an HDMI
     * re-plug, a late readback after our own apply).  Hardware finding: with
     * mode_1080p == 0 (the 1.5.1 safe-boot revert left it so) and 1080p30
     * selected through the native entry, the cache stayed at 60 Hz for the
     * whole session and every rule was an identity: 30 fps games at 15,
     * frameskip titles at half speed. */
    gret = ksceDisplayGetOutputMode(HDMI_HEAD, &cur, &pf);
    if (gret >= 0) {
        uint32_t want = (g_hd_alias != 0 && cur == g_hd_alias) ? refresh_from_mode(g_cfg.hd_mode_code)
                                                               : refresh_from_mode(cur);
        if (cur != g_current_output_mode || g_refresh_hz != want) {
            klog("watchdog: driver reports 0x%04X (cache 0x%04X, %u Hz): refresh cache updated",
                 cur, (unsigned)g_current_output_mode, (unsigned)g_refresh_hz);
            refresh_display_cache(1);
        }
    }
    if (!g_cfg.mode_1080p) {
        unlock();
        return;
    }
    if (gret >= 0) {
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
    kvlog("thread: shell pid=0x%08X, waiting %u ms before first apply",
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
        trace_profile_tick();

        /* v1.4.7: release a held Sony request nobody merged with. */
        if (g_held && (now_us() - g_held_us) >= (SceInt64)HOLD_REQUEST_US) {
            uint32_t m = g_held_mode;
            g_held = 0;
            if (!g_cfg.mode_1080p) {
                g_mode_request = m;
                g_mode_request_pending = 1;
                klog("avconfig: held request 0x%08X released (no 1080p selection followed)", (unsigned)m);
            } else {
                klog("avconfig: held request 0x%08X dropped, 1080p stays on", (unsigned)m);
            }
        }

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
        g_apply_total = 0;
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

/* 1.6: the Settings plugin logs through the kernel so there is one log file
 * in one place (a user process cannot write to ur0).  Nothing happens unless
 * debug logging is on. */
int pstv1080pLog(const char *line)
{
    char buf[200];
    int n;

    if (!line)
        return PSTV1080P_ERR_INVALID_ARG;
    if (!g_verbose)
        return 0;
    /* The plugin passes a static 512-byte buffer; a checked copy of a fixed
     * size is the safest way to read it (no strlen over user memory). */
    if (ksceKernelCopyFromUser(buf, line, sizeof(buf) - 1) < 0)
        return PSTV1080P_ERR_INVALID_ARG;
    buf[sizeof(buf) - 1] = 0;
    n = (int)strnlen(buf, sizeof(buf) - 1);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    klog("settings: %s", buf);
    return 0;
}

/* 1.6.1: the configurator app edits the per-title override file through the
 * kernel (a user app cannot touch ur0).  Both calls run in the caller's
 * syscall context and share the parse buffer's lock with the loader. */
static char g_games_io[PSTV1080P_GAMES_MAX_BYTES];

int pstv1080pReadGames(char *buf, uint32_t size)
{
    SceUID fd;
    int n = 0, ret;

    if (!buf || size < 2 || size > PSTV1080P_GAMES_MAX_BYTES)
        return PSTV1080P_ERR_INVALID_ARG;
    tbl_lock();
    fd = ksceIoOpen(PSTV1080P_GAMES_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = ksceIoRead(fd, g_games_io, size - 1);
        if (n < 0) {                          /* open ok but read failed: not "no file" */
            ksceIoClose(fd);
            tbl_unlock();
            return n;
        }
        if (n == (int)size - 1) {             /* buffer full: is there more? */
            char c;
            if (ksceIoRead(fd, &c, 1) > 0) {
                ksceIoClose(fd);
                tbl_unlock();
                return PSTV1080P_ERR_TOO_LARGE;
            }
        }
        ksceIoClose(fd);
    }
    g_games_io[n] = 0;
    ret = ksceKernelCopyToUser(buf, g_games_io, (SceSize)n + 1);
    tbl_unlock();
    return ret < 0 ? ret : n;
}

int pstv1080pWriteGames(const char *buf, uint32_t size)
{
    SceUID fd;
    int ret, w;

    if (!buf || size > PSTV1080P_GAMES_MAX_BYTES)
        return PSTV1080P_ERR_INVALID_ARG;
    tbl_lock();
    ret = ksceKernelCopyFromUser(g_games_io, buf, size);
    if (ret < 0) {
        tbl_unlock();
        return ret;
    }
    /* Stage in a temporary file so a short or failed write can never destroy
     * the existing overrides; the real file is replaced only afterwards. */
    ensure_dir();
    fd = ksceIoOpen(PSTV1080P_GAMES_TMP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
    if (fd < 0) {
        tbl_unlock();
        return (int)fd;
    }
    w = size ? ksceIoWrite(fd, g_games_io, size) : 0;
    ksceIoClose(fd);
    if (w != (int)size) {
        ksceIoRemove(PSTV1080P_GAMES_TMP_PATH);      /* real file untouched */
        tbl_unlock();
        return w < 0 ? w : PSTV1080P_ERR_APPLY_FAILED;
    }
    ksceIoRemove(PSTV1080P_GAMES_PATH);              /* rename refuses an existing destination */
    ret = ksceIoRename(PSTV1080P_GAMES_TMP_PATH, PSTV1080P_GAMES_PATH);
    games_list_load(1);                 /* the table always mirrors what is on disk */
    tbl_unlock();
    if (ret < 0) {
        klog("games: rename of the staging file failed (0x%08X); new content is in " PSTV1080P_GAMES_TMP_PATH, (unsigned)ret);
        return ret;
    }
    klog("games: file replaced by the configurator (%u bytes)", (unsigned)size);
    return 0;
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
    info.reserved[4]         = g_games_count;
    info.reserved[5]         = (uint32_t)g_verbose;
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
        kvlog("hook: %s ok (uid=0x%08X)", name, (unsigned)uid);
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
        kvlog("hook: sceAVConfigHdmiSetResolution ok (uid=0x%08X)", (unsigned)g_avconfig_uid);
    } else {
        klog("hook: sceAVConfigHdmiSetResolution FAILED 0x%08X", (unsigned)g_avconfig_uid);
    }

    if (module_get_export_func(KERNEL_PID, AVCONFIG_MODULE, AVCONFIG_LIB_NID, AVCONFIG_SETRES_NID, &fn) >= 0 && fn) {
        g_avconfig_fn = (avconfig_setres_fn)fn;
        kvlog("export: sceAVConfigHdmiSetResolution resolved");
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
    install_pacing_hook(PH_GETMAXFBRES,       NID_GETMAXFBRES,            hook_GetMaximumFrameBufResolution, HOOK_BIT_GETMAXFBRES, "_sceDisplayGetMaximumFrameBufResolution");
    install_pacing_hook(PH_GETRESINFO,        NID_GETRESINFOINTERNAL,     hook_GetResolutionInfoInternal,    HOOK_BIT_GETRESINFO,  "_sceDisplayGetResolutionInfoInternal");
    install_pacing_hook(PH_GETREFRESHRATE,    NID_GETREFRESHRATE,         hook_GetRefreshRate,               HOOK_BIT_GETREFRESHRATE, "sceDisplayGetRefreshRate");
    {
        SceUID u;
        u = taiHookFunctionExportForKernel(KERNEL_PID, &g_pacing_ref[PH_ALLOCMEMBLOCK], SYSMEM_MODULE,
                                           SYSMEM_USER_LIB_NID, NID_ALLOCMEMBLOCK, hook_AllocMemBlock);
        g_pacing_uid[PH_ALLOCMEMBLOCK] = u;
        if (u >= 0) g_hooks_ok |= HOOK_BIT_ALLOCMEMBLOCK;
        if (u < 0 || g_verbose)
            klog("hook: sceKernelAllocMemBlock %s (0x%08X)", u >= 0 ? "ok" : "FAILED", (unsigned)u);
        u = taiHookFunctionExportForKernel(KERNEL_PID, &g_pacing_ref[PH_GETFREEMEM], SYSMEM_MODULE,
                                           SYSMEM_USER_LIB_NID, NID_GETFREEMEMORYSIZE, hook_GetFreeMemorySize);
        g_pacing_uid[PH_GETFREEMEM] = u;
        if (u >= 0) g_hooks_ok |= HOOK_BIT_GETFREEMEM;
        if (u < 0 || g_verbose)
            klog("hook: sceKernelGetFreeMemorySize %s (0x%08X)", u >= 0 ? "ok" : "FAILED", (unsigned)u);
    }
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
            if (r != 0 || g_verbose)
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
    proc_clear();

    g_mutex = ksceKernelCreateMutex("pstv1080p_mtx", 0, 0, NULL);
    g_tbl_mutex = ksceKernelCreateMutex("pstv1080p_tbl", 0, 0, NULL);

    /* 1.6: one directory for everything the plugin owns; decide first whether
     * anything at all gets logged this boot. */
    ensure_dir();
    g_verbose = file_exists(PSTV1080P_DEBUG_PATH);
    klog("pstv1080p kernel module v%s starting (debug logging on)", PSTV1080P_VERSION_STR);
    if (g_tbl_mutex < 0)
        klog("table mutex create failed 0x%08X (miss path unserialised)", (unsigned)g_tbl_mutex);

    config_load();
    safe_boot_check();
    if (g_cfg.fps_mode == PSTV1080P_FPS_FORCE)
        title_list_load();
    tbl_lock();
    games_list_load(1);
    tbl_unlock();

    /* Refresh cache before hooks go live so the hot path never sees stale 60. */
    refresh_display_cache(1);

    install_hooks();

    g_procevent_uid = ksceKernelRegisterProcEventHandler("pstv1080p", &g_procevent_handler, 0);
    if (g_procevent_uid < 0 || g_verbose)
        klog("procevent: register -> 0x%08X", (unsigned)g_procevent_uid);

    g_thread_stop = 0;
    /* 1.6.5: NO plugin thread may ever wait on the display's vblank.  The
     * driver wakes one waiter per vblank in queue order, so a kernel thread
     * looping on ksceDisplayWaitVblankStart (the 1.6.4 callback doubler)
     * alternated with the game: every game wait took two vblanks and every
     * title ran at half rate (30 fps games at 15, frameskip titles too). */
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
    g_thread_stop = 1;
    if (g_thread_uid >= 0) {
        SceUInt timeout = 5u * 1000u * 1000u;
        ksceKernelWaitThreadEnd(g_thread_uid, NULL, &timeout);
        ksceKernelDeleteThread(g_thread_uid);
        g_thread_uid = -1;
    }

    release_hooks();

    if (g_procevent_uid >= 0) {
        ksceKernelUnregisterProcEventHandler(g_procevent_uid);
        g_procevent_uid = -1;
    }

    if (g_mutex >= 0) {
        ksceKernelDeleteMutex(g_mutex);
        g_mutex = -1;
    }
    if (g_tbl_mutex >= 0) {
        ksceKernelDeleteMutex(g_tbl_mutex);
        g_tbl_mutex = -1;
    }

    klog("stopped");
    return SCE_KERNEL_STOP_SUCCESS;
}
