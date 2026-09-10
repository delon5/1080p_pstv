# DESIGN: "pstv1080p" — native 1080p (30 Hz) output option for PlayStation TV

Goal (user's words): a native "1080p" option in the PS TV video output settings (Settings > Sound & Display > HDMI resolution), NOT a remap of an existing mode; applied automatically at boot; with Framecapper-style frame pacing that stays correct at 30 Hz (a 30 fps title must run at 30 fps, not 15).

Two modules, one repo, one Makefile:

## A. Kernel plugin `pstv1080p.skprx` (loaded under *KERNEL)
State file `ur0:tai/pstv1080p.cfg` (binary struct, magic 'P18P', version 1, little endian, padded to 64 bytes):
```
uint32 magic, version;
uint32 mode_1080p;        // 0/1: the virtual "1080p" item is selected
uint32 hd_mode_code;      // screen mode to apply, default 0x8710 (1080p30). Advanced users may try 0x8720 (1080p24) / 0x8700 (1080p60, expected to fail).
uint32 settings_item_value; // registry-style value used for the injected list_item (default 3)
uint32 fps_mode;          // 0 = off, 1 = SCALE (default): keep the game's intended fps under non-60Hz output, 2 = FORCE: Framecapper-style fixed target
uint32 fps_target;        // FORCE mode target fps (20/30/60), default 30
uint32 fps_inject;        // FORCE mode: also wait after every SetFrameBuf (Framecapper "Inject"), default 0
uint32 safe_boot_seconds; // revert-on-quick-reboot window, default 120, 0 disables
uint32 boot_apply_delay_ms; // delay after SceShell appears before first apply, default 3000
uint32 watchdog_period_ms;  // 0 disables; default 2000
uint32 reserved[5];
```
Load at module_start; missing/invalid -> defaults (mode_1080p = 0). Persist with ksceIoOpen(O_WRONLY|O_CREAT|O_TRUNC, 6) on every change.

Hooks (all `taiHookFunctionExportForKernel(KERNEL_PID, ...)`):
1. `SceAVConfig` lib 0x79E0F03F func 0x4D37F036 `sceAVConfigHdmiSetResolution(int mode)`:
   - record `g_last_system_mode = mode` (what Sony's code wanted; e.g. 0x8300/0x8600/0x8500), log it;
   - if `cfg.mode_1080p` -> mode = cfg.hd_mode_code;
   - `ret = TAI_CONTINUE(int, ref, mode)`; record ret and the mode actually applied; on success refresh the cached refresh-rate info (see 4). Return ret.
   This covers: Settings app changes, SceShell's boot-time apply (if it goes through this export), HDMI re-plug paths that use it.
2. Boot/watchdog thread (priority 0x10000100, stack 0x2000-0x4000):
   - Wait until `ksceKernelSysrootGetShellPid() > 0` (poll every 500 ms, give up waiting after 60 s but continue), then sleep `boot_apply_delay_ms`.
   - Safe-boot check happens BEFORE any apply, at module_start: if `cfg.mode_1080p` and marker file `ur0:tai/pstv1080p.boot` exists -> previous boot with 1080p did not survive `safe_boot_seconds` -> set mode_1080p = 0, persist, log "reverted". Then (if still enabled) create the marker; the thread deletes the marker after `safe_boot_seconds` of uptime (or immediately if 0). If mode is disabled, remove marker if present.
   - Apply: `apply_hd_mode()` = if `cfg.mode_1080p`: get current via `ksceDisplayGetOutputMode(1 /*HDMI*/, &cur, &pf)`; if cur != hd_mode_code (or the call failed) -> call the SceAVConfig export function pointer obtained once via `module_get_export_func(KERNEL_PID, "SceAVConfig", 0x79E0F03F, 0x4D37F036, &fn)` as `fn(hd_mode_code)` (it goes through our hook, harmless). Log result. Count consecutive failures; after 5 consecutive failures stop retrying until the state changes (avoid fighting the system or hammering a TV that refuses the mode).
   - Watchdog: every `watchdog_period_ms`, if enabled, re-check output mode and re-apply if it drifted (rate-limited: max 1 apply per 5 s, max 5 consecutive failures). Also re-measure the refresh rate when the mode changes.
   - Never call apply when mode_1080p == 0. When the user turns 1080p off (syscall), if `g_last_system_mode` is known re-apply it once via fn(g_last_system_mode) so the picture returns to the Sony-selected mode; otherwise do nothing (the Settings app will apply the mode it writes).
3. Frame pacing hooks on the SceDisplay USER library (0x5ED8F994) exports (these are syscall implementations; the hook runs in the caller's process context):
   - 0xDD0A13B8 sceDisplayWaitVblankStartMulti(unsigned vcount), 0x05F27764 ...MultiCB, 0x5795E898 sceDisplayWaitVblankStart(void), 0x78B41B92 ...CB, 0x7D9864A8 sceDisplayWaitSetFrameBufMulti(unsigned vcount), 0x3E796EF5 ...MultiCB, and (FORCE+inject only) 0xF51523CB _sceDisplaySetFrameBuf(const SceDisplayFrameBuf*, int sync, void *opt).
   - Refresh rate: `g_refresh_hz` derived from the current HDMI output mode (flag bits 0xF0 of the screen mode: 0x00->60, 0x10->30, 0x20->24, 0x80->50, 0x40->25(assumed)); fall back to 60. Cross-check/log once with `ksceDisplayGetRefreshRateInternal(head,&fps,&scan)` converted to int immediately (pointer-based, allowed) — but the hot path must only use the cached integer. Cache is refreshed by the watchdog thread and after each successful SetResolution. Hot path: no I/O, no allocation, no float.
   - Process filter: `pid = ksceKernelGetProcessId()`; skip (pass through unchanged) if pid == KERNEL_PID or pid == ksceKernelSysrootGetShellPid() (cache the shell pid). In FORCE mode additionally honour an optional title list file `ur0:tai/pstv1080p_titles.txt` (one title id per line, or `*ALL`); resolve title id per pid with ksceKernelSysrootGetProcessTitleId and cache it in a small 8-entry pid->titleid table (hot path must not call sysroot every frame).
   - SCALE mode (default): `new = max(1, (vcount * g_refresh_hz) / 60)` (integer floor). At 60 Hz this is the identity, so the hooks are effectively no-ops when 1080p is off. For the no-arg WaitVblankStart/CB, interval is 1 -> always pass through.
   - FORCE mode: `interval = max(1, g_refresh_hz / fps_target)`; Multi variants: replace vcount; non-Multi variants: if interval == 1 pass through else return `ksceDisplayWaitVblankStartMulti(interval)` (or the CB variant). SetFrameBuf hook (inject only): `ret = TAI_CONTINUE(...); ksceDisplayWaitVblankStartMulti(interval); return ret;`
   - When fps_mode == 0, all pacing hooks pass through immediately. Hooks that fail to install (uid < 0) are logged and skipped; the module still starts.
4. Syscall exports (library `pstv1080p`, `syscall: true`) for the Settings-app plugin and any config app:
   - `int pstv1080pGetConfig(pstv1080p_config_t *user_out)` — ksceKernelCopyToUser.
   - `int pstv1080pSetConfig(const pstv1080p_config_t *user_in)` — ksceKernelCopyFromUser, validate (hd_mode_code must have 0x8000 set and a resolution field 0x300..0x700; fps_target in {20,30,60}; item value 1..255), persist; if mode_1080p changed -> apply/revert now.
   - `int pstv1080pSetMode1080p(int enable)` — convenience wrapper (persist + apply/revert now). Returns the apply result (0 on success or when nothing to apply, <0 on error).
   - `int pstv1080pGetInfo(pstv1080p_info_t *user_out)` — {version, current_output_mode, refresh_hz, last_system_mode, last_apply_result, hooks_ok_bitmask, settings_item_value}.
   Header `include/pstv1080p.h` shared by both modules (structs + prototypes + PSTV1080P_* constants).
5. Logging: append lines to `ux0:data/pstv1080p/kernel.log` via a small helper (open/append/close; mkdir once; silently ignore failures; never log from the frame-pacing hot path). Log: config load, hook install results, every SetResolution call (requested/applied/ret), apply attempts, safe-boot revert.
6. module_stop releases all hooks, stops the thread (flag + wait), and returns SCE_KERNEL_STOP_SUCCESS.

## B. Settings-app plugin `pstv1080p_settings.suprx` (loaded under *NPXS10015, alongside henkaku.suprx)
Pattern: same as HENkaku user.c (which is the reference implementation of this technique); coexist with it (taiHEN chains hooks; we never touch the pages HENkaku replaces).
1. module_start: hook `sceKernelLoadStartModule` import in "SceSettings" (SceLibKernel 0xCAE9ACE6, func 0x2DCC4AFA) and `sceKernelStopUnloadModule` (0xCAE9ACE6, 0x2415F8A4). When `vs0:app/NPXS10015/system_settings_core.suprx` is loaded, install the hooks below; release them on unload (like HENkaku).
2. `scePafMiscLoadXmlLayout` import in "SceSettings" (ScePafMisc 0x3D643CE8, 0x19FE55A8) `(int a1, void *xml_buf, int xml_size, int a4)`:
   - If the buffer (bounded by xml_size) contains `hdmi_resolution_mode`: dump the original once to `ux0:data/pstv1080p/settings_page_orig.xml` (diagnostics), then build a patched copy in a static 64 KiB buffer: locate the `<list` element whose attribute list contains `hdmi_resolution_mode` (search backwards from the key for the nearest `<list`), find its matching `</list>`; collect existing `value="..."` numbers of its `<list_item` children; choose our value = cfg.settings_item_value if unused else the smallest integer >= 3 not used (and push it to the kernel with pstv1080pSetConfig); insert before `</list>`:
     `<list_item id="id_pstv1080p_1080p" title="msg_pstv1080p_1080p" value="N"/>` (copy the indentation style of the previous item). If anything is not found or the result would exceed the buffer: pass the original through unchanged and log why. Pass the patched buffer + new size to TAI_CONTINUE. Log the list's original items (id/title/value) to `ux0:data/pstv1080p/settings.log` — this tells us Sony's value->mode mapping for future versions.
3. `sceRegMgrGetKeyInt` import in "SceSystemSettingsCore" (SceRegMgr 0xC436F916, 0x16DDF3DC) `(const char *category, const char *name, int *value)`: if category matches "/CONFIG/DISPLAY" (tolerate trailing '/') and name == "hdmi_resolution_mode" and kernel config says mode_1080p: `*value = N; return 0;` else TAI_CONTINUE.
4. `sceRegMgrSetKeyInt` import in "SceSystemSettingsCore" (0xC436F916, 0xD72EA399) `(category, name, int value)`: same key match: if value == N -> `pstv1080pSetMode1080p(1)`; DO NOT write the registry; return 0. Else -> if 1080p currently enabled call `pstv1080pSetMode1080p(0)` first, then TAI_CONTINUE (Sony writes the registry and applies its mode; the kernel hook passes it through because 1080p is now off). Log every call.
5. `sceAVConfigHdmiSetResolution` import in "SceSystemSettingsCore" (SceAVConfig 0x79E0F03F, 0x4D37F036): log the code Sony's code passes (mapping evidence); if kernel mode_1080p is enabled, substitute cfg.hd_mode_code (belt and braces with the kernel hook). Install with taiHookFunctionImport; if it fails (module may not import it directly), just log.
6. `scePafToplevelGetText` import in "SceSystemSettingsCore" (ScePafToplevel 0x4D9A9DD0, 0x19CEFDA7) `wchar_t *(void *arg, char **msg)`: if `*msg` == "msg_pstv1080p_1080p" return a static UTF-16 string L"1080p (30 Hz)" (compile with -fshort-wchar); else TAI_CONTINUE.
7. Config cache: call pstv1080pGetConfig once at module_start and again after each Set; keep N and mode_1080p in globals.
8. Logging helper writing to `ux0:data/pstv1080p/settings.log` (sceIoMkdir + append). Must never crash the Settings app: every hook validates pointers/lengths, and all string compares are bounded (sceClibStrncmp / a local bounded memmem).

## C. Build (plain Makefile, no cmake) and repo layout (branch `native-1080p` of delon5/1080p_pstv)
```
include/pstv1080p.h        shared header
kernel/main.c, kernel/pstv1080p.yml   (module name pstv1080p; exports library pstv1080p syscall functions above)
user/main.c, user/pstv1080p_settings.yml (module name pstv1080p_settings)
Makefile                   targets: all, kernel, stubs, user, clean; VITASDK env var required
README.md                  (see D) ; docs/RESEARCH_NOTES.md, docs/DESIGN.md, docs/reversing/*
```
Kernel: `arm-vita-eabi-gcc -Wl,-q -Wall -O2 -nostdlib -fno-builtin -D__VITA_KERNEL__ -mfloat-abi=softfp` linking `-ltaihenForKernel_stub -ltaihenModuleUtils_stub -lSceSysclibForDriver_stub -lSceSysmemForDriver_stub -lSceIofilemgrForDriver_stub -lSceThreadmgrForDriver_stub -lSceDisplayForDriver_stub -lSceRegMgrForDriver_stub -lSceSysrootForDriver_stub -lSceSysrootForKernel_stub -lgcc`; then `vita-elf-create -e kernel/pstv1080p.yml kernel.elf kernel.velf` and `vita-make-fself -c kernel.velf pstv1080p.skprx`. Stubs for the user side: `vita-elf-export k kernel.elf kernel/pstv1080p.yml build/pstv1080p_imports.yml` then `vita-libs-gen-2 build/pstv1080p_imports.yml build/stubs && make -C build/stubs` -> libpstv1080p_stub.a.
User: `-Wl,-q -Wall -O2 -nostdlib -fshort-wchar` linking `-ltaihen_stub -lpstv1080p_stub -lSceLibKernel_stub -lSceIofilemgr_stub -lSceRegistryMgr_stub -lSceAVConfig_stub_weak -lgcc`; vita-elf-create -e user/pstv1080p_settings.yml; vita-make-fself -c.
No libc: use sceClib* (user) / SceSysclibForDriver (kernel: memcpy/memset/strncmp/strlen/snprintf are exported by SceSysclibForDriver — check names in psp2kern/kernel/sysclib.h) and never call malloc.

## D. README must state
- What it does; requirements (PS TV, taiHEN/HENkaku, FW 3.60-3.74); install lines (`*KERNEL ur0:tai/pstv1080p.skprx`, `*NPXS10015 ur0:tai/pstv1080p_settings.suprx`), reboot, then Settings > Sound & Display > HDMI resolution shows "1080p (30 Hz)".
- 1080p is 30 Hz only (hardware limit; 1080p60 is not possible on the PS TV HDMI path), some displays (FreeSync/some monitors) won't sync it; safe-boot revert behaviour (if the console reboots/powers off within 2 minutes after a boot in 1080p, the option is turned off automatically — re-select it in Settings); how to force-disable (delete ur0:tai/pstv1080p.cfg via FTP/VitaShell or safe mode).
- Frame pacing section (SCALE default vs FORCE with fps target/inject, title list), interaction with Framecapper (remove Framecapper*.suprx lines to avoid double-capping) and with Sharpscale (compatible; Sharpscale's "unlock framebuffer sizes" is unrelated).
- Diagnostics: log files in ux0:data/pstv1080p/, please share `settings_page_orig.xml` + logs when reporting issues.
- Credits: gameblabla (1080p_pstv), HENkaku/molecule (settings XML technique), SKGleba (ineedsettings), Rinnegatamante (Framecapper), CBPS/cuevavirus (Sharpscale, SceDisplay RE), wiki.henkaku.xyz.
- Status: built but NOT yet tested on hardware (no PS TV available to the author); list exactly which assumptions need a first on-device test.
