/*
 * pstv1080p — native 1080p (30 Hz) HDMI output option for PlayStation TV
 * Shared header: kernel module <-> Settings-app plugin contract.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSTV1080P_H
#define PSTV1080P_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSTV1080P_VERSION            0x0169u      /* 1.6.9 (0xMMmp: major, minor, patch) */
#define PSTV1080P_VERSION_STR        "1.6.9"

/* 1.6: everything the plugin owns lives in ONE directory on ur0 (always
 * mounted when kernel plugins start).  Users of 1.x move their files from
 * ur0:tai/ here themselves; the plugin never touches ur0:tai/. */
#define PSTV1080P_DIR                "ur0:data/pstv1080p"
#define PSTV1080P_CFG_PATH           "ur0:data/pstv1080p/pstv1080p.cfg"
#define PSTV1080P_BOOT_MARKER_PATH   "ur0:data/pstv1080p/pstv1080p.boot"
#define PSTV1080P_TITLES_PATH        "ur0:data/pstv1080p/pstv1080p_titles.txt"
#define PSTV1080P_GAMES_PATH         "ur0:data/pstv1080p/pstv1080p_games.txt"  /* per-title overrides: "TITLEID mode" */
#define PSTV1080P_GAMES_TMP_PATH     "ur0:data/pstv1080p/pstv1080p_games.tmp"  /* staging file of pstv1080pWriteGames */
#define PSTV1080P_DEBUG_PATH         "ur0:data/pstv1080p/pstv1080p_debug.txt"  /* exists at boot -> logging on; absent -> NO log is written */
/* The log itself goes to the memory card (easy to fetch, no wear on ur0). */
#define PSTV1080P_LOG_DIR            "ux0:data/pstv1080p"
#define PSTV1080P_LOG_PATH           "ux0:data/pstv1080p/pstv1080p.log"        /* the only log (kernel + Settings plugin), debug only */
/* Developer dumps written by the Settings plugin (a user process: ux0 only), debug mode only. */
#define PSTV1080P_DUMP_DIR           "ux0:data/pstv1080p"
#define PSTV1080P_SETTINGS_XML_DUMP  "ux0:data/pstv1080p/settings_page_orig.xml"
#define PSTV1080P_DUMP_REQUEST       "ux0:data/pstv1080p/dump_request"  /* create it -> Settings modules dumped once, file removed */

#define PSTV1080P_CFG_MAGIC          0x50383150u  /* "P18P" little endian */
#define PSTV1080P_CFG_VERSION        2u   /* 2 since 1.3: fps_inject default became AUTO; v1 files are migrated.
                                           * NEVER bump this to change a default: config_valid() rejects every
                                           * other version, which silently resets a working console to defaults.
                                           * Adjust the value in config_load() instead (see fps_target, 1.6.6). */

/* SceDisplay screen-mode codes (wiki.henkaku.xyz/vita/SceDisplay, SceDisplayScreenModeFlag). */
#define PSTV1080P_SCREENMODE_STD     0x8000u
#define PSTV1080P_SCREENMODE_RES_MASK 0x0700u
#define PSTV1080P_SCREENMODE_480P    0x0300u
#define PSTV1080P_SCREENMODE_576P    0x0400u
#define PSTV1080P_SCREENMODE_1080I   0x0500u
#define PSTV1080P_SCREENMODE_720P    0x0600u
#define PSTV1080P_SCREENMODE_1080P   0x0700u
#define PSTV1080P_SCREENMODE_HZ_MASK 0x00F0u
#define PSTV1080P_SCREENMODE_60HZ    0x0000u
#define PSTV1080P_SCREENMODE_30HZ    0x0010u
#define PSTV1080P_SCREENMODE_24HZ    0x0020u
#define PSTV1080P_SCREENMODE_25HZ    0x0040u   /* assumed */
#define PSTV1080P_SCREENMODE_50HZ    0x0080u

#define PSTV1080P_MODE_480P60        0x8300u
#define PSTV1080P_MODE_720P60        0x8600u
#define PSTV1080P_MODE_1080I60       0x8500u
#define PSTV1080P_MODE_1080P30       0x8710u   /* the one that works on PS TV (gameblabla) */
#define PSTV1080P_MODE_1080P24       0x8720u   /* untested */
#define PSTV1080P_MODE_1080P60       0x8700u   /* expected NOT to work (HDMI path limit) */

/* Frame pacing modes. */
#define PSTV1080P_FPS_OFF            0u
#define PSTV1080P_FPS_SCALE          1u   /* keep each game's intended fps under a non-60 Hz output (default) */
#define PSTV1080P_FPS_FORCE          2u   /* Framecapper-style fixed target, refresh-rate aware */

/* Registry key that Sony's Settings page binds the HDMI resolution list to. */
#define PSTV1080P_REG_CATEGORY       "/CONFIG/DISPLAY"
#define PSTV1080P_REG_KEY            "hdmi_resolution_mode"
/* XML ids used by the injected Settings entry. */
#define PSTV1080P_XML_ITEM_ID        "id_pstv1080p_1080p"
#define PSTV1080P_XML_ITEM_MSG       "msg_pstv1080p_1080p"

typedef struct pstv1080p_config {
    uint32_t magic;               /* PSTV1080P_CFG_MAGIC */
    uint32_t version;             /* PSTV1080P_CFG_VERSION */
    uint32_t mode_1080p;          /* 0/1: the virtual "1080p" Settings item is selected */
    uint32_t hd_mode_code;        /* screen mode applied when mode_1080p, default PSTV1080P_MODE_1080P30 */
    uint32_t settings_item_value; /* registry-style value of the injected list_item, default 3 */
    uint32_t fps_mode;            /* PSTV1080P_FPS_* , default SCALE */
    uint32_t fps_target;          /* FORCE mode target fps: 20/30/60, default 60 since 1.6.6 (Framecapper60 semantics) */
    uint32_t fps_inject;          /* 0 off, 1 AUTO (default): after a flip wait one period only for processes that
                                   * made no vsync call of their own, 2 always (Framecapper "Inject" semantics) */
    uint32_t safe_boot_seconds;   /* revert-on-quick-reboot window, default 120, 0 disables */
    uint32_t boot_apply_delay_ms; /* delay after SceShell appears before first apply, default 3000 */
    uint32_t watchdog_period_ms;  /* 0 disables, default 2000 */
    uint32_t reserved[5];
} pstv1080p_config_t;             /* 64 bytes */

typedef struct pstv1080p_info {
    uint32_t size;                /* sizeof(pstv1080p_info_t), filled by the kernel */
    uint32_t version;             /* PSTV1080P_VERSION */
    uint32_t mode_1080p;          /* current state */
    uint32_t settings_item_value; /* value the Settings plugin must use for the injected item */
    uint32_t current_output_mode; /* ksceDisplayGetOutputMode(HDMI) or 0 if unknown */
    uint32_t refresh_hz;          /* cached integer refresh rate used for frame pacing */
    uint32_t last_system_mode;    /* last mode Sony's code asked for via sceAVConfigHdmiSetResolution (0 = never seen) */
    int32_t  last_apply_result;   /* return value of the last sceAVConfigHdmiSetResolution we issued */
    uint32_t hooks_ok;            /* bitmask: bit0 AVConfig hook, bit1.. frame pacing hooks (see kernel) */
    uint32_t reserved[7];         /* [0] apply attempts this episode, [1] this session, [2] learned alias readback,
                                   * [3] attempt pending (v1.1), [4] per-game overrides loaded, [5] debug logging on (1.5/1.6) */
} pstv1080p_info_t;               /* 64 bytes */

#define PSTV1080P_STATIC_ASSERT(cond, name) typedef char pstv1080p_assert_##name[(cond) ? 1 : -1]
PSTV1080P_STATIC_ASSERT(sizeof(pstv1080p_config_t) == 64, config_size);
PSTV1080P_STATIC_ASSERT(sizeof(pstv1080p_info_t) == 64, info_size);

/* Kernel syscall exports (library "pstv1080p"). All pointers are user-space pointers. */
int pstv1080pGetConfig(pstv1080p_config_t *out);
int pstv1080pSetConfig(const pstv1080p_config_t *in);
int pstv1080pSetMode1080p(int enable);
int pstv1080pGetInfo(pstv1080p_info_t *out);
/* 1.6: append one line (NUL-terminated, <= 199 bytes used) to the plugin log.
 * A no-op unless debug logging is on, so callers may call it freely. */
int pstv1080pLog(const char *line);
/* 1.6.1 (configurator app): read / replace the per-title override file
 * ur0:data/pstv1080p/pstv1080p_games.txt through the kernel, so a plain
 * user app needs no ur0 permission.
 * Read: copies at most size-1 bytes into buf and NUL-terminates; returns the
 * byte count (0 if the file does not exist), a negative SCE error if the read
 * fails, or PSTV1080P_ERR_TOO_LARGE if the file does not fit in size-1 bytes
 * (nothing is copied in either error case).
 * Write: stages buf[0..size) (size <= PSTV1080P_GAMES_MAX_BYTES) in a
 * temporary file, renames it over the real one only after a complete write,
 * and reloads the override table; returns 0 or a negative error (the previous
 * file is untouched on failure).
 * The kernel applies at most PSTV1080P_GAMES_MAX_ENTRIES lines, in file order. */
#define PSTV1080P_GAMES_MAX_BYTES    4096u
#define PSTV1080P_GAMES_MAX_ENTRIES  128u
int pstv1080pReadGames(char *buf, uint32_t size);
int pstv1080pWriteGames(const char *buf, uint32_t size);

/* Error codes returned by the kernel exports (besides negative SCE errors passed through). */
#define PSTV1080P_ERR_INVALID_ARG    ((int)0x80F18001)
#define PSTV1080P_ERR_NOT_READY      ((int)0x80F18002)
#define PSTV1080P_ERR_APPLY_FAILED   ((int)0x80F18003)
#define PSTV1080P_ERR_TOO_LARGE      ((int)0x80F18004)   /* pstv1080pReadGames: file does not fit the buffer */

#ifdef __cplusplus
}
#endif
#endif /* PSTV1080P_H */
