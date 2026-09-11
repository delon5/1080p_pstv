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

#define PSTV1080P_VERSION            0x0131u      /* 1.3.1 */

/* Persistent kernel state (ur0 is always mounted when kernel plugins start). */
#define PSTV1080P_CFG_PATH           "ur0:tai/pstv1080p.cfg"
#define PSTV1080P_BOOT_MARKER_PATH   "ur0:tai/pstv1080p.boot"
#define PSTV1080P_TITLES_PATH        "ur0:tai/pstv1080p_titles.txt"
#define PSTV1080P_GAMES_PATH         "ur0:tai/pstv1080p_games.txt"   /* v1.3 per-title overrides: "TITLEID mode" */
/* Diagnostics (best effort, ux0 may not be mounted yet early at boot). */
#define PSTV1080P_LOG_DIR            "ux0:data/pstv1080p"
#define PSTV1080P_KERNEL_LOG         "ux0:data/pstv1080p/kernel.log"
#define PSTV1080P_SETTINGS_LOG       "ux0:data/pstv1080p/settings.log"
#define PSTV1080P_SETTINGS_XML_DUMP  "ux0:data/pstv1080p/settings_page_orig.xml"

#define PSTV1080P_CFG_MAGIC          0x50383150u  /* "P18P" little endian */
#define PSTV1080P_CFG_VERSION        2u   /* 2 since 1.3: fps_inject default became AUTO; v1 files are migrated */

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
    uint32_t fps_target;          /* FORCE mode target fps: 20/30/60, default 30 */
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
    uint32_t reserved[7];         /* [0] apply attempts this episode, [1] this session, [2] learned alias readback, [3] attempt pending (v1.1) */
} pstv1080p_info_t;               /* 64 bytes */

#define PSTV1080P_STATIC_ASSERT(cond, name) typedef char pstv1080p_assert_##name[(cond) ? 1 : -1]
PSTV1080P_STATIC_ASSERT(sizeof(pstv1080p_config_t) == 64, config_size);
PSTV1080P_STATIC_ASSERT(sizeof(pstv1080p_info_t) == 64, info_size);

/* Kernel syscall exports (library "pstv1080p"). All pointers are user-space pointers. */
int pstv1080pGetConfig(pstv1080p_config_t *out);
int pstv1080pSetConfig(const pstv1080p_config_t *in);
int pstv1080pSetMode1080p(int enable);
int pstv1080pGetInfo(pstv1080p_info_t *out);

/* Error codes returned by the kernel exports (besides negative SCE errors passed through). */
#define PSTV1080P_ERR_INVALID_ARG    ((int)0x80F18001)
#define PSTV1080P_ERR_NOT_READY      ((int)0x80F18002)
#define PSTV1080P_ERR_APPLY_FAILED   ((int)0x80F18003)

#ifdef __cplusplus
}
#endif
#endif /* PSTV1080P_H */
