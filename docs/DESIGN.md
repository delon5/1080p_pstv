# DESIGN: "pstv1080p" — native 1080p (30 Hz) output option for PlayStation TV

Goal (user's words): a native "1080p" option in the PS TV video output settings (Settings > Sound & Display > HDMI resolution), NOT a remap of an existing mode; applied automatically at boot; with Framecapper-style frame pacing that stays correct at 30 Hz (a 30 fps title must run at 30 fps, not 15).

Two modules, one repo, one Makefile:

## A. Kernel plugin `pstv1080p.skprx` (loaded under *KERNEL)
State file `ur0:data/pstv1080p/pstv1080p.cfg` (1.0-1.5: `ur0:tai/pstv1080p.cfg`; binary struct, magic 'P18P', version 2 since 1.3, a version-1 file is migrated once; little endian, padded to 64 bytes):
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
   - Safe-boot check happens BEFORE any apply, at module_start: if `cfg.mode_1080p` and marker file `ur0:data/pstv1080p/pstv1080p.boot` exists -> previous boot with 1080p did not survive `safe_boot_seconds` -> set mode_1080p = 0, persist, log "reverted". Then (if still enabled) create the marker; the thread deletes the marker after `safe_boot_seconds` of uptime (or immediately if 0). If mode is disabled, remove marker if present.
   - Apply: `apply_hd_mode()` = if `cfg.mode_1080p`: get current via `ksceDisplayGetOutputMode(1 /*HDMI*/, &cur, &pf)`; if cur != hd_mode_code (or the call failed) -> call the SceAVConfig export function pointer obtained once via `module_get_export_func(KERNEL_PID, "SceAVConfig", 0x79E0F03F, 0x4D37F036, &fn)` as `fn(hd_mode_code)` (it goes through our hook, harmless). Log result. Count consecutive failures; after 5 consecutive failures stop retrying until the state changes (avoid fighting the system or hammering a TV that refuses the mode).
   - Watchdog: every `watchdog_period_ms`, if enabled, re-check output mode and re-apply if it drifted (rate-limited: max 1 apply per 5 s, max 5 consecutive failures). Also re-measure the refresh rate when the mode changes.
   - Never call apply when mode_1080p == 0. When the user turns 1080p off (syscall), if `g_last_system_mode` is known re-apply it once via fn(g_last_system_mode) so the picture returns to the Sony-selected mode; otherwise do nothing (the Settings app will apply the mode it writes).
3. Frame pacing hooks on the SceDisplay USER library (0x5ED8F994) exports (these are syscall implementations; the hook runs in the caller's process context):
   - 0xDD0A13B8 sceDisplayWaitVblankStartMulti(unsigned vcount), 0x05F27764 ...MultiCB, 0x5795E898 sceDisplayWaitVblankStart(void), 0x78B41B92 ...CB, 0x7D9864A8 sceDisplayWaitSetFrameBufMulti(unsigned vcount), 0x3E796EF5 ...MultiCB, and (FORCE+inject only) 0xF51523CB _sceDisplaySetFrameBuf(const SceDisplayFrameBuf*, int sync, void *opt).
   - Refresh rate: `g_refresh_hz` derived from the current HDMI output mode (flag bits 0xF0 of the screen mode: 0x00->60, 0x10->30, 0x20->24, 0x80->50, 0x40->25(assumed)); fall back to 60. Cross-check/log once with `ksceDisplayGetRefreshRateInternal(head,&fps,&scan)` converted to int immediately (pointer-based, allowed) — but the hot path must only use the cached integer. Cache is refreshed by the watchdog thread and after each successful SetResolution. Hot path: no I/O, no allocation, no float.
   - Process filter: `pid = ksceKernelGetProcessId()`; skip (pass through unchanged) if pid == KERNEL_PID or pid == ksceKernelSysrootGetShellPid() (cache the shell pid). In FORCE mode additionally honour an optional title list file `ur0:data/pstv1080p/pstv1080p_titles.txt` (one title id per line, or `*ALL`); resolve title id per pid with ksceKernelSysrootGetProcessTitleId and cache it in a small 8-entry pid->titleid table (hot path must not call sysroot every frame).
   - SCALE mode (default): `new = max(1, (vcount * g_refresh_hz) / 60)` (integer floor). At 60 Hz this is the identity, so the hooks are effectively no-ops when 1080p is off. For the no-arg WaitVblankStart/CB, interval is 1 -> always pass through.
   - FORCE mode: `interval = max(1, g_refresh_hz / fps_target)`; Multi variants: replace vcount; non-Multi variants: if interval == 1 pass through else return `ksceDisplayWaitVblankStartMulti(interval)` (or the CB variant). SetFrameBuf hook (inject only): `ret = TAI_CONTINUE(...); ksceDisplayWaitVblankStartMulti(interval); return ret;`
   - When fps_mode == 0, all pacing hooks pass through immediately. Hooks that fail to install (uid < 0) are logged and skipped; the module still starts.
4. Syscall exports (library `pstv1080p`, `syscall: true`) for the Settings-app plugin and any config app:
   - `int pstv1080pGetConfig(pstv1080p_config_t *user_out)` — ksceKernelCopyToUser.
   - `int pstv1080pSetConfig(const pstv1080p_config_t *user_in)` — ksceKernelCopyFromUser, validate (hd_mode_code must have 0x8000 set and a resolution field 0x300..0x700; fps_target in {20,30,60}; item value 1..255), persist; if mode_1080p changed -> apply/revert now.
   - `int pstv1080pSetMode1080p(int enable)` — convenience wrapper (persist + apply/revert now). Returns the apply result (0 on success or when nothing to apply, <0 on error).
   - `int pstv1080pGetInfo(pstv1080p_info_t *user_out)` — {version, current_output_mode, refresh_hz, last_system_mode, last_apply_result, hooks_ok_bitmask, settings_item_value}.
   Header `include/pstv1080p.h` shared by both modules (structs + prototypes + PSTV1080P_* constants).
5. Logging (1.6): only when `ur0:data/pstv1080p/pstv1080p_debug.txt` existed at module_start; then append lines to `ux0:data/pstv1080p/pstv1080p.log` via a small helper (open/append/close; mkdir on first use; silently ignore failures; never log from the frame-pacing hot path). Without the switch `klog()` is a no-op. Log: config load, hook install results, every SetResolution call (requested/applied/ret), apply attempts, safe-boot revert.
6. module_stop releases all hooks, stops the thread (flag + wait), and returns SCE_KERNEL_STOP_SUCCESS.

## B. Settings-app plugin `pstv1080p_settings.suprx` (loaded under *NPXS10015, alongside henkaku.suprx)
Pattern: same as HENkaku user.c (which is the reference implementation of this technique); coexist with it (taiHEN chains hooks; we never touch the pages HENkaku replaces).
1. module_start: hook `sceKernelLoadStartModule` import in "SceSettings" (SceLibKernel 0xCAE9ACE6, func 0x2DCC4AFA) and `sceKernelStopUnloadModule` (0xCAE9ACE6, 0x2415F8A4). When `vs0:app/NPXS10015/system_settings_core.suprx` is loaded, install the hooks below; release them on unload (like HENkaku).
2. `scePafMiscLoadXmlLayout` import in "SceSettings" (ScePafMisc 0x3D643CE8, 0x19FE55A8) `(int a1, void *xml_buf, int xml_size, int a4)`:
   - If the buffer (bounded by xml_size) contains `hdmi_resolution_mode`: dump the original once to `ux0:data/pstv1080p/settings_page_orig.xml` (debug mode only: skipped unless the kernel reports debug logging on), then build a patched copy in a static 64 KiB buffer: locate the `<list` element whose attribute list contains `hdmi_resolution_mode` (search backwards from the key for the nearest `<list`), find its matching `</list>`; collect existing `value="..."` numbers of its `<list_item` children; choose our value = cfg.settings_item_value if unused else the smallest integer >= 3 not used (and push it to the kernel with pstv1080pSetConfig); insert before `</list>`:
     `<list_item id="id_pstv1080p_1080p" title="msg_pstv1080p_1080p" value="N"/>` (copy the indentation style of the previous item). If anything is not found or the result would exceed the buffer: pass the original through unchanged and log why. Pass the patched buffer + new size to TAI_CONTINUE. Log the list's original items (id/title/value) through `pstv1080pLog` (they appear as `settings:` lines in the kernel's `ux0:data/pstv1080p/pstv1080p.log`, debug mode only) — this tells us Sony's value->mode mapping for future versions.
3. `sceRegMgrGetKeyInt` import in "SceSystemSettingsCore" (SceRegMgr 0xC436F916, 0x16DDF3DC) `(const char *category, const char *name, int *value)`: if category matches "/CONFIG/DISPLAY" (tolerate trailing '/') and name == "hdmi_resolution_mode" and kernel config says mode_1080p: `*value = N; return 0;` else TAI_CONTINUE.
4. `sceRegMgrSetKeyInt` import in "SceSystemSettingsCore" (0xC436F916, 0xD72EA399) `(category, name, int value)`: same key match: if value == N -> `pstv1080pSetMode1080p(1)`; DO NOT write the registry; return 0. Else -> if 1080p currently enabled call `pstv1080pSetMode1080p(0)` first, then TAI_CONTINUE (Sony writes the registry and applies its mode; the kernel hook passes it through because 1080p is now off). Log every call.
5. `sceAVConfigHdmiSetResolution` import in "SceSystemSettingsCore" (SceAVConfig 0x79E0F03F, 0x4D37F036): log the code Sony's code passes (mapping evidence); if kernel mode_1080p is enabled, substitute cfg.hd_mode_code (belt and braces with the kernel hook). Install with taiHookFunctionImport; if it fails (module may not import it directly), just log.
6. `scePafToplevelGetText` import in "SceSystemSettingsCore" (ScePafToplevel 0x4D9A9DD0, 0x19CEFDA7) `wchar_t *(void *arg, char **msg)`: if `*msg` == "msg_pstv1080p_1080p" return a static UTF-16 string L"1080p (30 Hz)" (compile with -fshort-wchar); else TAI_CONTINUE.
7. Config cache: call pstv1080pGetConfig once at module_start and again after each Set; keep N and mode_1080p in globals.
8. Logging helper (1.6): `pstv_log()` formats into a static 512-byte buffer and calls the `pstv1080pLog` syscall; no file of its own (the kernel drops the line unless debug logging is on). Must never crash the Settings app: every hook validates pointers/lengths, and all string compares are bounded (sceClibStrncmp / a local bounded memmem).

## C. Build (plain Makefile, no cmake) and repo layout (branch `native-1080p` of delon5/1080p_pstv)
```
include/pstv1080p.h        shared header
kernel/main.c, kernel/pstv1080p.yml   (module name pstv1080p; exports library pstv1080p syscall functions above)
user/main.c, user/pstv1080p_settings.yml (module name pstv1080p_settings)
Makefile                   targets: all, kernel, stubs, user, clean; VITASDK env var required
README.md                  (see D) ; docs/RESEARCH_NOTES.md, docs/DESIGN.md, docs/reversing/*
```
Kernel: `arm-vita-eabi-gcc -Wl,-q -Wall -O2 -nostdlib -fno-builtin -D__VITA_KERNEL__ -mfloat-abi=softfp` linking `-ltaihenForKernel_stub -ltaihenModuleUtils_stub -lSceSysclibForDriver_stub -lSceSysmemForDriver_stub -lSceIofilemgrForDriver_stub -lSceThreadmgrForDriver_stub -lSceDisplayForDriver_stub -lSceRegMgrForDriver_stub -lSceSysrootForDriver_stub -lSceSysrootForKernel_stub -lgcc`; then `vita-elf-create -e kernel/pstv1080p.yml kernel.elf kernel.velf` and `vita-make-fself -c kernel.velf pstv1080p.skprx`. Stubs for the user side: `vita-elf-export k kernel.elf kernel/pstv1080p.yml build/pstv1080p_imports.yml` then `vita-libs-gen-2 -yml=build/pstv1080p_imports.yml -output=build/stubs && make -C build/stubs` -> libpstv1080p_stub.a (v2 takes `-yml=`/`-output=` options, not positional arguments).
User: `-Wl,-q -Wall -O2 -nostdlib -fshort-wchar` linking `-ltaihen_stub -lpstv1080p_stub -lSceLibKernel_stub -lSceIofilemgr_stub -lSceRegistryMgr_stub -lSceAVConfig_stub_weak -lgcc`; vita-elf-create -e user/pstv1080p_settings.yml; vita-make-fself -c.
No libc: use sceClib* (user) / SceSysclibForDriver (kernel: memcpy/memset/strncmp/strlen/snprintf are exported by SceSysclibForDriver — check names in psp2kern/kernel/sysclib.h) and never call malloc.

## D. README must state
- What it does; requirements (PS TV, taiHEN/HENkaku, FW 3.60-3.74); install lines (`*KERNEL ur0:tai/pstv1080p.skprx`, `*NPXS10015 ur0:tai/pstv1080p_settings.suprx`), reboot, then Settings > Sound & Display > HDMI resolution shows "1080p (30 Hz)".
- 1080p is 30 Hz only (hardware limit; 1080p60 is not possible on the PS TV HDMI path), some displays (FreeSync/some monitors) won't sync it; safe-boot revert behaviour (if the console reboots/powers off within 2 minutes after a boot in 1080p, the option is turned off automatically — re-select it in Settings); how to force-disable (delete `ur0:data/pstv1080p/pstv1080p.cfg` via FTP/VitaShell or safe mode; 1.6 does not read `ur0:tai/pstv1080p.cfg`).
- Frame pacing section (SCALE default vs FORCE with fps target/inject, title list), interaction with Framecapper (remove Framecapper*.suprx lines to avoid double-capping) and with Sharpscale (compatible; Sharpscale's "unlock framebuffer sizes" is unrelated).
- Diagnostics: off by default (no file is written). With `ur0:data/pstv1080p/pstv1080p_debug.txt` present at boot, kernel and Settings-plugin lines go to the single log `ux0:data/pstv1080p/pstv1080p.log`; please share it and `ux0:data/pstv1080p/settings_page_orig.xml` (written only in debug mode) when reporting issues.
- Credits: gameblabla (1080p_pstv), HENkaku/molecule (settings XML technique), SKGleba (ineedsettings), Rinnegatamante (Framecapper), CBPS/cuevavirus (Sharpscale, SceDisplay RE), wiki.henkaku.xyz.
- Status: built but NOT yet tested on hardware (no PS TV available to the author); list exactly which assumptions need a first on-device test.

## E. Footprint and safety constraints (user requirement: nothing permanently changed; runs only inside taiHEN)
- NO system file is ever modified: nothing is written to vs0:, os0:, sa0:, pd0:, or the Settings app's RCO/XML files. The Settings XML is patched in RAM only, at the moment the page is loaded, by the taiHEN import hook; removing the plugin line restores stock behaviour with no residue.
- Sony's registry is never written by this project. The only registry writes that happen are the Settings app's own writes of a Sony-defined value (0/1/2) when the user picks 480p/720p/1080i, which we pass through unchanged. Selecting our "1080p" item does NOT store anything in the registry.
- All hooks are taiHEN runtime hooks (taiHookFunctionExportForKernel / taiHookFunctionImport) released in module_stop; no code patching of system modules on disk, no taiInject of persistent data.
- Files this project creates (all our own, all safe to delete): the directory `ur0:data/pstv1080p/` (created at boot, parent `ur0:data/` too if missing) with `pstv1080p.cfg` (64-byte state) and `pstv1080p.boot` (boot marker, normally deleted ~2 min after boot); user-authored, never written by the plugin: `pstv1080p_games.txt`, `pstv1080p_titles.txt`, `pstv1080p_debug.txt` in the same directory; and, only in debug mode, `ux0:data/pstv1080p/` with `pstv1080p.log` and the dumps. Nothing in `ur0:tai/` but the two binaries. Uninstall = remove the two config.txt lines and delete those two directories (plus any 1.x leftovers in `ur0:tai/`).
- The README must contain a "What this touches" section stating exactly the above.

## F. v1.1 change (after hardware test 1)
- Settings-path apply works on hardware; the kernel-thread boot apply did not take effect (driver kept reporting 0x8300) and the v1.0 "expected readback" latch then suppressed retries.
- New rule: an apply is EFFECTIVE only if ksceDisplayGetOutputMode changes (to hd_mode_code, or to a new plausible value which becomes the alias). Unchanged readback = not applied.
- Boot-time attempt is scheduled by the thread and executed from SceShell's display syscalls (_sceDisplaySetFrameBuf, sceDisplayWaitVblankStart*, sceDisplayWaitSetFrameBufMulti*) via the existing kernel export hooks, i.e. in a user-process syscall context; thread fallback after 10 s. Retries with backoff, 10/episode, 30/session. Re-entrancy guard + try-lock so a frame flip never blocks behind the watchdog.

## G. v1.2 change (adaptive inject)
- Per-process tracker: every SceDisplay vsync-related syscall (WaitVblankStart*, WaitSetFrameBuf*, GetVcount*) stamps last_sync_us for the calling pid on entry and exit; RegisterVblankStartCallback marks the pid as callback-synced.
- _sceDisplaySetFrameBuf hook: if fps_mode != OFF and fps_inject == 1 and the pid had no sync activity for > 4.5 refresh periods (and is not callback-synced) -> ksceDisplayWaitVblankStartMulti(1) (FORCE: force_interval()). fps_inject == 2 = always (old Framecapper Inject). Default fps_inject = 1.
- Rationale: games that never vsync were unpaced at 30 Hz (user-reported flicker / wrong rate); Framecapper60Inject paced them but double-waited every syncing game.

## H. v1.3 change (per-title overrides)
- ur0:data/pstv1080p/pstv1080p_games.txt (1.3-1.5: ur0:tai/pstv1080p_games.txt): "TITLEID mode" lines, modes off/scale/frameskip/nowait/inject/force. Loaded at module_start and re-read on every new process (proc_resolve), so no reboot is needed.
- Unified per-process table g_procs[8] {pid, allowed, override, acc, last_sync_us, cb_synced, title}; resolved once per process (sysroot title id + list lookups + one log line).
- frameskip: credit accumulator acc += n*hz; wait floor(acc/60) vblanks when acc >= 60 else return 0 (n=1 @30 Hz alternates; n=2 @30 Hz waits 1 each; identity @60 Hz). GetVcount/GetVcountInternal return v*60/hz (monotonic, no artificial wrap) for frameskip titles. Never injected.
- nowait: all wait hooks return 0; no inject. off: nothing. inject: inject=2. force: FORCE rule. scale: SCALE rule.

## I. v1.4 change (spoof720)
- Hardware: Tales of Hearts R (PCSE00429) crashes (C2-12828-1) before any hooked display call under 1080p30, runs under 720p -> it acts on start-up display queries.
- Override "spoof720": hooks _sceDisplayGetMaximumFrameBufResolution (0x2EBFC7CB) and _sceDisplayGetResolutionInfoInternal (0xFEFEB240) on the SceDisplay user library; after the original succeeds, for the listed pid the user results are rewritten to 960x544 max framebuffer and {0x8600, 1280x720, progressive, 59.94} via ksceKernelCopyFromUser/CopyToUser. Logged once per process (flag bits in e->acc, unused by spoof720). Pacing follows the global rules.

## K. v1.4.7 held requests (single-transition switching)
- hook_HdmiSetResolution: non-self request while mode_1080p==1, or an "automatic" (0x10000000) request while mode_1080p==0, is HELD (return 0, display untouched) for HOLD_REQUEST_US=400 ms. Explicit Sony modes while 1080p is off pass through unchanged.
- SetMode1080p(1) within the window: hold cancelled, direct SetResolution(0x8710) (same path as boot). SetMode1080p(0) within the window: the held mode is applied directly (revert). Window expiry (thread tick): if 1080p off -> the held mode is applied from the next user display syscall (run_mode_request); if on -> dropped.
- Revert without a hold: SETRES_AUTO if Sony's last raw request was automatic, else last plausible Sony mode.


## L. Native value→mode mapping (1.5.0)

**Where the mapping lives.** Not in `SceSystemSettingsCore` (no screen-mode
constant exists in that module) but in the main `SceSettings` module: one
function (FW 3.60: 0x81125102, `docs/reversing/settings_value_to_mode.txt`)
reads the list value, runs a compare ladder (1 → 0x8500, 2 → 0x8600,
3 → 0x8300, else → 0x10000000 with the "known" flag cleared), calls
`sceAVConfigHdmiSetResolution(mode, known, 1)` through a syscall stub and, on
success, the core's registry object → `SetKeyInt("/CONFIG/DISPLAY/hdmi_resolution_mode", value)`.

**The patch.** The Settings plugin searches SceSettings' text segment for the
58-byte signature of that function (push … through the third case). On exactly
one match it replaces the 52 bytes from `movs r1,#0` to the `blx` with a
same-size ladder that keeps every register/flag convention (r0 = mode,
r1 = known, r2 = 1, r3 untouched) and adds `cmp r3,#N; beq ours` →
`movw r0,#cfg.hd_mode_code`. N and the mode are patched into the template at
run time (N = the injected list item's value, normally 4). `taiInjectData`
does the write (taiHEN handles the cache maintenance); `taiInjectRelease` in
module_stop restores the original bytes. The eboot on disk is never touched.

**Interaction with the kernel hook.** A request equal to `hd_mode_code` that
is not one of our own is now "Sony's code asking for our mode": the hook lets
it through unchanged (no hold, no substitution, never recorded as the
system mode), or returns 0 without calling the driver if the head already
shows that mode. `SetMode1080p(1)` that follows (from the SetKeyInt hook)
finds the head already there, or sees that a native request was sent within
the last 3 s and does not stack a second one; the watchdog re-checks the
readback as usual.

**Fallback.** Signature not found or ambiguous (another firmware): logged
once, no patch, and the 1.4.x behaviour (kernel-side substitution of the
"automatic" request) remains in force.


## M. Launch tracer (1.5.1) — REMOVED in 1.5.2

> **Removed.** The tracer hooked `ksceKernelCreateProcess`, `ksceKernelStartProcess(Ext)`
> and `ksceKernelKillProcess` and called `klog()` (ux0: file open/write/close) from
> inside them, with a 232-byte `SceKernelProcessInfo` on the caller's kernel stack.
> On hardware this rebooted the console and corrupted the state file
> (`hd_mode_code` became 0x8700). Rule for this project from now on: never do
> file I/O or large stack allocations inside hooks on process/module-manager
> internals; buffer to a static ring and flush from the plugin's own thread, or
> trace from user-library export hooks (syscall context) only. The section below
> is kept as a record of what was tried.

Opt-in, log-only. When a `trace` override exists (or verbose logging is on)
the kernel module hooks the seven launch-chain exports listed in
RESEARCH_NOTES section 13 and logs, per call: arguments (bounded strings,
raw integers), the calling process (SceShell / a title / kernel), the result,
milliseconds since the last CreateProcess, and the process status words
(`ksceKernelGetProcessStatus`, `SceKernelProcessInfo` status / modid /
entrypoint / type / budget) before StartProcess and before/at kill. Every hook
continues the original call unchanged. The error-history hook copies 0x140
bytes of the posted record from the caller and dumps the words after the
0x100-byte message (layout unverified, hence raw). Without a trace override
none of these hooks is installed.

## N. File layout and logging (1.6.0)

**One directory.** `ur0:data/pstv1080p/` holds everything the plugin owns:
`pstv1080p.cfg`, `pstv1080p_games.txt`, `pstv1080p_titles.txt`, the safe-boot
marker `pstv1080p.boot` and the debug switch `pstv1080p_debug.txt`. The log
itself, `pstv1080p.log`, goes to `ux0:data/pstv1080p/` (memory card: easy to
fetch; log traffic never touches `ur0`, whose only writes by the plugin are the
directory itself, the 64-byte config and the safe-boot marker). `ur0` is chosen (not `ux0`) because it is mounted before
kernel plugins start, so the config and the marker are readable at boot. The
two plugin binaries stay in `ur0:tai/` (taiHEN loads them from there).

**No migration.** The plugin reads and writes only inside its own directory
(`ensure_dir()` creates it at `module_start`). Files a 1.x user has in
`ur0:tai/` are moved by the user; the plugin never touches `ur0:tai/`.

**No log by default.** `klog()` returns immediately unless
`pstv1080p_debug.txt` existed when the module started; `g_verbose` is set
right after `ensure_dir()` and before the first `klog()`. With it on, every line
(the former "verbose" set) goes to `pstv1080p.log`.

**One log file.** So that kernel and Settings-plugin lines land in one file in
order, the Settings plugin no longer writes a file of its own: `pstv_log()` formats into a static buffer and calls
the new syscall `pstv1080pLog(const char *)`. The kernel side does a checked
`ksceKernelCopyFromUser` of a fixed 199 bytes into a stack buffer (no strlen
over user memory), NUL-terminates, strips trailing newlines and logs
`settings: <line>`. It is a no-op when logging is off, so the plugin may call
it before it has learned the debug state from `pstv1080pGetInfo`
(`reserved[5]`). Developer dumps (page XML, module dumps) are written by the
plugin itself to `ux0:data/pstv1080p/`, only in debug mode.


## O. Configurator app (1.6.1)

`configurator/main.c`, a vita2d LiveArea app (title id `PSTV10801`, packed by
the Makefile target `configurator` with vita-mksfoex / vita-pack-vpk; assets
rendered once by `configurator/assets/make_assets.swift` and committed).

**No file access of its own to the plugin's data.** The app calls
`pstv1080pReadGames` / `pstv1080pWriteGames` (new 1.6.1 syscalls, 4 KiB
bound, kernel-side `tbl_lock`, checked user copies) for the override file and
`pstv1080pGetConfig` / `pstv1080pSetConfig` for the global options. The
kernel reloads the override table right after a write, so a change applies to
the next launch of that game. The app's only direct I/O is reading
`<root>/<id>/sce_sys/param.sfo` (ux0:app, ur0:app, gro0:app) and
`ur0:appmeta/<id>/param.sfo` for game names, which needs the unsafe-homebrew
permission.

**Round trip.** `#` lines and lines the kernel would ignore are kept verbatim
and written back first; then one `TITLEID mode` line per game whose override is
not `none`. Titles present in the file but not installed are kept as
"(not installed)" rows. Mode names and their meaning mirror
`games_list_load` in kernel/main.c exactly (`k_mode_name`).

**Build notes.** The vdpm `libvita2d` package is hard-float (`-mfloat-abi=hard
-mfpu=neon`) and needs `SceSharedFb` (a library of the SceAppMgr module whose
stub the SDK snapshot lacks: the Makefile generates it from the NID database
into build/stubs_appmgr as a regenerated libSceAppMgr_stub.a) and `sceAppMgrGetBudgetInfo` (SceDriverUser).

## P. 1.6.4 callback doubler — REMOVED in 1.6.5

1.6.4 added a kernel thread that looped on `ksceDisplayWaitVblankStart()` and,
half a period after each vblank, fired the vblank callback of `frameskip`
titles a second time. On hardware every title then ran at half rate
(30 fps games at 15, `frameskip` titles at 15). The display driver wakes ONE
vblank waiter per vblank in queue order: the plugin thread and the game
thread alternated, so each game wait took two vblanks. Rule, alongside the
1.5.1 lesson: no plugin thread may ever wait on the display's vblank; every
vblank wait the plugin issues happens on the game's own thread, inside a
hook, on that game's behalf.

## Q. Framecapper (1.6.6)

Framecapper60 / Framecapper60Inject (disassembly in RESEARCH_NOTES §16) hook,
per game process, the imports of sceDisplayWaitVblankStart/CB/Multi/MultiCB and
force the count to 1; the Inject build also hooks sceDisplaySetFrameBuf and
calls WaitVblankStartMulti(1) after the original. Loaded next to pstv1080p every
wait would double. The kernel module does NOT try to detect this: an interim
1.6.6 build called taiGetModuleInfoForKernel from proc_resolve, which runs
inside a game's first display syscall under g_tbl_mutex, and games stopped being
resolved entirely. The Configurator reports the config.txt line instead, where
the work is free. The FORCE target default is 60 (Framecapper60 semantics), set
in config_defaults and corrected in place in config_load; PSTV1080P_CFG_VERSION
stays 2, because config_valid rejects every other version and a bump silently
resets a working console to defaults.

## R. The stale refresh-rate cache (1.6.6)


g_refresh_hz is what every pacing hook divides by; it was written only by
record_applied (after a SetResolution seen or issued by the plugin) and by the
watchdog, and the watchdog returned at once while cfg.mode_1080p == 0. The
1.5.1 reboot tripped the safe-boot revert, which stores mode_1080p = 0. From
1.5.0 on the user selects 1080p through the native Settings entry: Sony's code
sends 0x8710, the hook records it, but if the driver's readback lags the link
renegotiation the cache keeps 60, nothing corrects it, and every rule is an
identity at 60 Hz. Symptoms on hardware: 30 fps games at 15, frameskip titles
at half speed, novsync (a flag, not a rate) still working, no crash, no log
unless debug mode. Fix: the watchdog refreshes the cache unconditionally (only
the re-apply logic stays gated on mode_1080p), proc_resolve refreshes it once
per process start, the native request path sets mode_1080p = 1, and
proc_lookup re-validates an entry's title every 3 s (a reused pid must never
inherit an older process's rules even without the lifecycle handler).

Rule added with this fix: proc_resolve and everything it calls run in the hot
path of a game's first display syscall. Sysroot title lookup, the override
lists and one log line only — no display-driver query, no module-manager or
taiHEN query, no large stack objects.

## S. Logging from a game's thread (1.6.7)

Every display hook runs on the calling process's thread. A retail game is
sandboxed and kernel file I/O on its thread inherits that: ksceIoOpen on
ux0 returns an error, so klog wrote nothing. Only system apps, homebrew and
the plugin's own thread ever reached the file, and a game's entire life
appeared in the log as two lines written by the shell's thread (proc: create
and proc: kill). Since 1.6.7 klog compares ksceKernelGetProcessId() with
KERNEL_PID: kernel context writes directly, everything else is queued in a
48-slot ring that the plugin thread drains every 250 ms, counting drops.
This is also the last file I/O removed from the hook path (section on the
1.5.1 lesson).

## T. The flip is a frame time too (1.6.7)

sceDisplaySetFrameBuf with SETBUF_NEXTFRAME blocks until the next output
period: 16.7 ms at 60 Hz, 33 ms at 30 Hz. Every rule here rescaled the wait
calls and ignored the flip, so on a 30 Hz head a 30 fps game paid one period
in the flip plus a full period in its wait (15 fps), and a 60 fps game under
frameskip could not exceed 30 logic frames no matter how many waits were
skipped. hook_SetFrameBuf now records one unit of "flip debt" per vsynced
flip below 60 Hz and flip_debt_take() subtracts it from the next computed
real wait (bounded at two, so a mistake cannot free-run a game); frameskip
additionally forces flips to IMMEDIATE below 60 Hz, which is what novsync did
by hand.
