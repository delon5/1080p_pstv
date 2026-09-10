/* Reconstructed source of the user's 1080p_480phook.skprx (module name "ds4vita", 2164 bytes).
 * Derived from disassembly; behaviourally equivalent. Original build used xerpi's ds4vita template
 * (the logging helpers below are present in the binary but never called). */
#include <taihen.h>
#include <psp2kern/kernel/modulemgr.h>

static tai_hook_ref_t g_ref;   /* .bss 0x20000 */
static SceUID         g_uid;   /* .bss 0x20004 */

static int hook_sceAVConfigHdmiSetResolution(int mode, int a1, int a2, int a3) {
    if (mode == 0x8300)            /* 480p60 selected in Settings ... */
        mode = 0x8710;             /* ... becomes 1080p30 */
    return TAI_CONTINUE(int, g_ref, mode, a1, a2, a3);
}

int module_start(SceSize argc, const void *args) {
    g_ref = -1; g_uid = -1;
    g_uid = taiHookFunctionExportForKernel(KERNEL_PID, &g_ref, "SceAVConfig",
                                           0x79E0F03F /* SceAVConfig (user lib) */,
                                           0x4D37F036 /* sceAVConfigHdmiSetResolution */,
                                           hook_sceAVConfigHdmiSetResolution);
    return g_uid < 0 ? SCE_KERNEL_START_FAILED : SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    if (g_uid >= 0) taiHookReleaseForKernel(g_uid, g_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}

/* ---- dead code carried over from the ds4vita template (log.c) ---- */
/* static char  log_buf[0x4000]; static int log_len;
 * log_reset(): ksceIoOpen("ux0:dump/ds4vita_log.txt", O_WRONLY|O_CREAT|O_TRUNC, 6); close; memset(log_buf,0,0x4000)
 * log_write(s,len): bounded memcpy into log_buf
 * log_flush(): ksceIoMkdir("ux0:dump/",6); open(...O_APPEND); ksceIoWrite(fd, log_buf, strlen(log_buf)); close */
