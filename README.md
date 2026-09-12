# pstv1080p — a native "1080p (30 Hz)" option for the PlayStation TV

> **Status: tested on a PS TV (FW 3.60, 2026-09-11).** The Settings entry, the switch, the automatic apply at boot (1.1) and the safe-boot marker all behaved as designed. Frame pacing for games without their own vsync was fixed in 1.2; a wider game test is still open.
> The author does not currently own a PS TV. Everything below describes what the
> code is designed to do, verified only by compiling it and by reverse engineering
> the original plugin and Sony's modules. Please read
> [First test on hardware](#first-test-on-hardware) before installing, keep a way
> to reach `ur0:data/pstv1080p/` without the Settings app (FTP/VitaShell, safe
> mode) so you can delete `pstv1080p.cfg` blind (removing the `*KERNEL` line from
> `ur0:tai/config.txt` works too), and send back the log described there
> (`ux0:data/pstv1080p/pstv1080p.log`, written only while
> `ur0:data/pstv1080p/pstv1080p_debug.txt` exists).

`pstv1080p` is a pair of taiHEN plugins for the PlayStation TV (PS TV / Vita TV)
that add a real **"1080p (30 Hz)"** entry to
*Settings > Sound & Display > HDMI resolution*, remember your choice, apply it
automatically at every boot, and keep games' frame pacing correct while the
console is outputting 30 Hz.

It grew out of gameblabla's `1080p_pstv` (which silently turned every
resolution change into 1080p30) and the "480p → 1080p30" remap variant that
was built from it. This version is a rewrite; see the
[technical appendix](#appendix-how-the-original-plugin-worked-and-what-changed)
for how they differ.

## Contents

- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Install](#install)
- [Things to know before enabling 1080p](#things-to-know-before-enabling-1080p)
- [What this touches (and how to uninstall)](#what-this-touches-and-how-to-uninstall)
- [Configurator app](#configurator-app-161)
- [Frame pacing (SCALE / FORCE)](#frame-pacing)
- [Compatibility with Framecapper and Sharpscale](#compatibility-with-other-plugins)
- [Diagnostics and reporting issues](#diagnostics-and-reporting-issues)
- [First test on hardware](#first-test-on-hardware)
- [Developer section](#developer-section)
  - [Building](#building)
  - [Repository layout](#repository-layout)
  - [Config file `ur0:data/pstv1080p/pstv1080p.cfg`](#config-file-ur0datapstv1080ppstv1080pcfg)
  - [Frame-pacing model](#frame-pacing-model)
  - [Safe-boot revert rule](#safe-boot-revert-rule)
  - [Kernel syscall API](#kernel-syscall-api)
- [Appendix: how the original plugin worked and what changed](#appendix-how-the-original-plugin-worked-and-what-changed)
- [Credits](#credits)
- [License](#license)

## Changelog

- **1.6.10 (2026-09-12)** — **`novsync` now does what novsync.suprx does.**
  The reference plugin (junminlee2004/novsync, after Electry's VGi) hooks the
  eight vblank wait calls and returns from each immediately; it never touches
  the sync argument of `sceDisplaySetFrameBuf`. 1.6.3 to 1.6.9 instead forced
  every flip to IMMEDIATE, which on hardware left games running with a black
  screen or stuck on their splash. That is gone: the flip's sync argument is
  passed through untouched and `novsync` makes the waits return at once, the
  same thing the `nowait` rule does. Existing lines keep working; `novsync`
  next to another rule now means `nowait`.
- **1.6.9 (2026-09-12)** — **Per-game rules reach games again.** The override
  table was re-read from `ur0:data/pstv1080p/pstv1080p_games.txt` inside the
  first display call of every process, which runs on that process's own
  thread. A sandboxed retail game cannot open that file, and a failed open
  emptied the table, so the game was resolved with `override=none` while its
  line sat in the file. On hardware three of four titles lost their rule this
  way (`PCSE00120`, `PCSE01262`, `PCSE00015` all came up `none`, while
  `PCSG00009` happened to read the file successfully and got its `nowait`).
  The plugin thread now refreshes the table every two seconds, where the file
  is always readable; process resolution does no file I/O at all; and a failed
  read never empties an already loaded table. Edits still take effect a couple
  of seconds later without a reboot. The `process:` log line now ends with
  `rules=N`, so an empty table is visible at a glance.
- **1.6.8 (2026-09-12)** — **Reverts 1.6.7's frame-pacing change; keeps its
  logging and config fixes.** 1.6.7 assumed a flip set to "next frame" blocks
  the caller for a whole output period and shortened the following wait by
  that much. It does not block: it only says when the buffer becomes visible,
  and the vblank wait after it is the only throttle a normal frame loop has.
  Removing it let everything free-run (the Configurator at 400+ fps,
  Bloodstained: Curse of the Moon at 130). Every pacing function is now
  byte-identical to 1.6.6 again. What 1.6.7 got right and 1.6.8 keeps:
  (1) log lines produced on a game's own thread are queued and written by the
  plugin thread, because a sandboxed game cannot open a file on ux0 and every
  line it produced was silently dropped; (2) a state file is never rejected
  over its version, in either direction, so 1.6.6's brief version change
  cannot leave the console booting with 1080p off (i.e. in 1080i);
  (3) reverting 1080p after a short boot takes three such boots, not one.
- **1.6.7 (2026-09-12)** — **The log was lying, the flip was the missing frame
  time, and a rejected state file was booting the console into 1080i.**
  (1) A retail game is sandboxed: `ksceIoOpen("ux0:...")` on its own thread
  fails, and every display hook runs on that thread, so every log line a game
  produced was dropped without a trace. The log showed system apps and
  homebrew only, which made "no `process:` line for this game" look like "the
  plugin never saw this game". Lines from any non-kernel thread are now queued
  in a small ring and written by the plugin thread, which also takes the last
  file I/O out of the hook path. (2) A flip that waits for the next frame
  costs a whole output period: 33 ms at 30 Hz against the 16.7 ms the game was
  written for. Nothing accounted for it, so a 30 fps game that vsyncs its flip
  and then waits for its frame budget spent two periods per frame and ran at
  15 (Persona 4 Golden). Each vsynced flip is now counted and the wait that
  follows is shortened by what the flip already spent, and `frameskip` stops
  vsyncing flips below 60 Hz, since a blocking flip alone caps a game at 30
  logic frames (Curse of the Moon ran at 30 fps in slow motion). (3) A state
  file is no longer rejected because of its version. 1.6.6 briefly used
  version 3, and the build that followed used 2 again and threw those files
  away: the console came up with 1080p off, i.e. in 1080i, on every boot.
  Any version from 1 on is now accepted and stamped forward. (4) Reverting
  1080p after a short boot needs three such boots in a row, not one: rebooting
  the console within two minutes of a boot, which is what installing a plugin
  looks like, used to disable 1080p permanently.
- **1.6.6 (2026-09-12)** — **Per-game rules reach games again, `force` target
  is 60.** Two fixes and one regression of my own. (1) The per-process entry
  is resolved inside the *first display syscall* of a game, so that code must
  stay minimal: an interim 1.6.6 build queried the display driver and taiHEN's
  module list there and games were never resolved at all (hardware log:
  `proc: create` for the title, then no `process:` line and no pacing). Both
  calls are gone; the resolve is what it was in 1.6.3 plus the current refresh
  rate in its log line. (2) The cached refresh rate that every rule divides by
  was corrected only by the watchdog, and the watchdog returned immediately
  while the plugin's "1080p on" flag was 0 — which the 1.5.1 safe-boot revert
  had left it, with 1080p coming from the native Settings entry instead. The
  cache then stayed at 60 Hz for the whole session and `scale` and `frameskip`
  are identities at 60 Hz. Now the watchdog corrects the cache whatever that
  flag says, a native 1080p selection sets the flag, and a process entry
  re-checks its title every 3 s so a reused pid cannot keep another game's
  rules. (3) The FORCE target now defaults to 60 (Framecapper60 semantics: one
  vblank per wait at 30 Hz, no cap at 60 Hz), and a state file still at 30 is
  corrected in place on load — the file version is deliberately NOT bumped,
  since `config_valid` rejects every other version and that would reset a
  working console. `force inject` is now exactly Framecapper60Inject.
  (4) Configurator: comment lines stay above the entry they belonged to
  instead of being collected at the top, entries the app adds get the game's
  name above them, and an active Framecapper or novsync line in taiHEN's
  config.txt (`ux0:tai` first, then `ur0:tai`) is reported in red — those
  plugins pace the same waits and would double-cap.
- **1.6.5 (2026-09-12)** — Removes the 1.6.4 callback-doubler kernel thread
  (a plugin thread waiting on vblank has no business in the display's wait
  queue) and the Unregister hook; `frameskip` is the 1.6.3 rule again. Its
  release note blamed that thread for the half-rate reports; 1.6.6 found the
  real cause (Framecapper stacking). Only the kernel module changed.
- **1.6.4 (2026-09-12)** — **`frameskip` now also covers games that pace on
  the vblank callback.** Under a 30 Hz head the display fires a game's
  registered vblank callback 30 times a second; a game whose logic counts those
  runs at half speed and no wait hook could change it (Tales of Hearts R with
  `frameskip novsync` got in-game but stayed at half speed). For `frameskip`
  titles a small kernel thread now fires the game's own callback once more
  half a period after every real vblank, so the game counts 60 a second, the
  same thing the rule already did for the wait calls and the vblank counter.
  The callback is tracked through `sceDisplayRegister/UnregisterVblankStartCallback`;
  nothing happens for games that never register one or at 60 Hz.
- **1.6.3 (2026-09-12)** — **`novsync` switch.** A game can now carry any of
  the existing options *plus* `novsync` (`PCSE00080 nowait novsync inject`).
  `novsync` makes every `sceDisplaySetFrameBuf` flip immediate, exactly what
  `novsync.suprx` does; on its own it also drops all vblank waits (the full
  novsync.suprx behaviour). Games that throttle themselves through the flip
  were untouched by every earlier option; the old `novsync.suprx` +
  `Framecapper60Inject.suprx` pair is `nowait novsync inject`. `inject` is now
  honoured with any option when given explicitly. The Configurator's picker
  has the switch as a checkbox under the options. Single-word lines are
  unchanged.
- **1.6.2 (2026-09-12)** — `trace` override: while the traced game runs, the
  kernel logs a **display-call profile** every 5 s (flips with next-frame vs
  immediate sync, `WaitVblankStart*`, `WaitSetFrameBuf*`, `GetVcount`, whether
  a vblank callback is registered). It shows what a game actually
  synchronises on, i.e. which override can work for it at all; games that
  pace themselves through a vblank callback or through the flip's sync flag
  are not affected by any of the current overrides, and the profile is how
  that gets found out. Debug logging must be on. No behaviour change.
- **1.6.1 (2026-09-12)** — **Configurator app.** New `pstv1080p_configurator.vpk`
  (LiveArea, unsafe homebrew): lists installed games with their per-game
  override, change / pick / remove / save (`pstv1080p_games.txt`), plus a
  global options screen; see [Configurator app](#configurator-app-161). The
  kernel module gained two syscalls for it, `pstv1080pReadGames` and
  `pstv1080pWriteGames`, which read and replace the override file inside the
  plugin's own directory and reload the override table immediately. Settings
  plugin unchanged apart from the version.
- **1.6.0 (2026-09-11)** — **one directory, no logs unless asked.** Everything
  the plugin owns now lives in `ur0:data/pstv1080p/`: `pstv1080p.cfg`,
  `pstv1080p_games.txt`, `pstv1080p_titles.txt`, the safe-boot marker
  `pstv1080p.boot` and the debug switch `pstv1080p_debug.txt` (which replaces
  1.5's `ur0:tai/pstv1080p_verbose.txt`). The plugin creates that directory at
  its first boot and never reads or writes anything in `ur0:tai/` any more
  (only the two binaries stay there). **Coming from 1.x, move the files
  yourself**: `pstv1080p.cfg`, `pstv1080p_games.txt` and, if you have one,
  `pstv1080p_titles.txt`; delete any `ur0:tai/pstv1080p.boot` /
  `pstv1080p_verbose.txt` and the old `ux0:data/pstv1080p/kernel.log` /
  `settings.log`. With no config file it starts from defaults (1080p off until
  you select it once). Logging is **off** by default and no log file is
  created; create an empty `ur0:data/pstv1080p/pstv1080p_debug.txt` and reboot
  to get one log, `ux0:data/pstv1080p/pstv1080p.log`, with both the kernel and
  the Settings plugin in it (the Settings plugin now logs through a new
  `pstv1080pLog` syscall instead of writing its own file). The page-XML dump
  and the module dump are debug-only. No behaviour change otherwise.

- **1.5.2 (2026-09-11)** — **fixes the reboots introduced by 1.5.1.** 1.5.1's
  launch tracer hooked `ksceKernelCreateProcess`, `ksceKernelStartProcess(Ext)`
  and `ksceKernelKillProcess` and wrote log lines (file I/O on `ux0:`) from
  inside those kernel paths, with a 232-byte process-info block on the
  caller's kernel stack. That is what rebooted the console, and the same run
  left the state file with `hd_mode_code = 0x8700` (1080p60, which the PS TV
  cannot output; the driver answers `0x803A0101` every time), so 1080p never
  came back and the plugin retried the impossible mode for the whole session.
  1.5.2 removes the tracer entirely, accepts only hardware-deliverable HD
  modes (1080p30, 1080p24), repairs a bad state file at boot, and stops
  retrying when the driver refuses a mode outright. **Delete
  `ur0:tai/pstv1080p.cfg` after installing** if you want a guaranteed clean
  start; otherwise the plugin repairs it itself on first boot.
- **1.5.1 (2026-09-11)** — diagnostics only, no behaviour change. The
  `proc: start` lines (one per resume phase, dozens per app) now appear only
  for the traced title or in verbose mode. New **launch tracer**, installed
  only while a `trace` override exists: log-only hooks on the kernel's
  app-launch chain (`ksceKernelCreateProcess`, `ksceKernelLoadProcessImage`,
  `ksceKernelStartProcess[Ext]`, `ksceKernelKillProcess`,
  `ksceAppMgrKillProcess`, and `_sceErrorHistoryPostError`, the call SceShell
  makes to file an error dialog) with arguments, results, calling process,
  process status words and timing, so a launch the system aborts before any
  user code runs (Tales of Hearts R under 1080-line output) shows *which* step
  fails and with *which* SCE error. Background in `docs/RESEARCH_NOTES.md`
  section 13: the kill event's words are event type / an undocumented status
  word (0x10 only for the failing launch) / process type; C2-12828-1 is
  SceShell's generic "application terminated" code and encodes no reason;
  Hearts R's param.sfo is an ordinary retail profile (no memory expansion, no
  PS TV or resolution flags).
- **1.5.0 (2026-09-11)** — **the Settings entry is mapped natively.** Sony's
  value→mode code is not in the core module at all: it is a compare ladder in
  the main `SceSettings` module (1 → 1080i, 2 → 720p, 3 → 480p, anything else →
  "automatic"), followed by `sceAVConfigHdmiSetResolution(mode, known, 1)` and
  the registry write (`docs/reversing/settings_value_to_mode.txt`). The Settings
  plugin now finds that function by its byte signature and replaces its 52-byte
  dispatch **in memory** (`taiInjectData`, released on unload, nothing on disk)
  with an equivalent ladder that has a fifth case: our value → 0x8710. Sony's
  own code therefore asks for 1080p30 when you pick the entry; the kernel hook
  lets that request through untouched (and skips it when the head is already
  there) instead of holding an "automatic" request and merging it. The registry
  is still never written with our value. The kernel passes Sony's second and
  third SetResolution arguments through and uses the same ones for its own
  calls. Logging is quiet by default (state changes, user actions, failures);
  create `ur0:tai/pstv1080p_verbose.txt` for the full inventory (every hook,
  every process, lifecycle events). The 1.4.8 module dump now runs only when
  `ux0:data/pstv1080p/dump_request` exists (removed afterwards). On a firmware
  where the signature is not found the plugin logs that once and everything
  works as in 1.4.x.
- **1.4.8 (2026-09-11)** — Settings plugin: one-shot dump of the Settings
  app's two modules (`SceSettings`, `SceSystemSettingsCore`) as mapped in
  memory, to `ux0:data/pstv1080p/dump_*.bin` + `.txt`, the first time the
  resolution page is opened (marker `dump_done`). Purpose: locate Sony's
  value-to-mode table so the core can be patched in memory to recognise the
  plugin's value natively. Kernel module unchanged apart from the version.
- **1.4.7 (2026-09-11)** — mode switches from Settings are now a single
  transition. Sony's Settings code commands the driver before it writes the
  registry, so selecting "1080p (30 Hz)" used to produce 1080i -> 720p ->
  1080p30 within half a second, which some TVs do not follow (the driver
  reported 1080p30 while the picture stayed on 1080i). The plugin now holds
  Sony's request for 0.4 s: if your 1080p selection follows, it is cancelled
  and the display goes straight to 1080p30, exactly like the boot-time switch
  that has always worked; if you picked a Sony mode while in 1080p, that mode
  is applied directly; if nothing follows ("Automatic"), the held request is
  applied after the window. No more 720p flash when selecting 1080p.
- **1.4.6 (2026-09-11)** — the Hearts R trace showed the system killing the
  launch 11.5 s after creating the process, before the game's code ever ran
  (no start event, no allocation, no display call): a launcher-level refusal
  under a 1080-line head, not a game crash. This build logs one `proc:` line
  per lifecycle event of every process (create/start/exit/kill with the raw
  event parameters) so a working title's sequence can be compared. Also: when
  1080p is selected while at 1080i, the plugin now leaves the intermediate
  720p step in place for 1.5 s so TVs that miss the 1080i to 1080p30 change
  re-lock.
- **1.4.5 (2026-09-11)** — a deliberate selection in Settings now resets the
  per-boot apply budget (previously 30 attempts per boot for all paths; after
  enough failed switches in one session the plugin silently stopped trying
  until a reboot, which looked like "it won't let me back into 1080p"). The
  automatic budget is 60 per boot.
- **1.4.4 (2026-09-11)** — fixes "switching from 1080i to 1080p sometimes
  does not change the output". Sony's Settings code switches the head to
  "automatic" just before the plugin asks for 1080p30; if the driver is still
  in that transition the request is dropped, and the plugin's retries were
  only executed from SceShell's display calls, which do not happen while the
  Settings app is open, so they timed out through the kernel-thread fallback
  the driver refuses. Retries now run from any process's display call, the
  plugin waits up to 0.5 s after a system-initiated switch before issuing its
  own, and the first retries come after 1 s and 2 s instead of 3 s and 5 s.
- **1.4.3 (2026-09-11)** — diagnostic per-game mode `trace`: for a listed
  title, `kernel.log` records the process create/start/exit/kill events with
  timings, every `sceKernelAllocMemBlock` request (name, type, size, result)
  and every free-memory query. For finding out why a title dies at start-up
  without a crash dump. Pass-through for everything else.
- **1.4.2 (2026-09-11)** — the 720p per-game output switch that briefly
  existed as 1.5 is removed: this plugin exists to keep the output at 1080p,
  not to switch it away. Also carries the 1.4.1 review fix (a process-exit
  callback could race the table's miss path; it now only unpublishes the pid
  with a compare-and-swap). Otherwise identical to 1.4.1.
- **1.4.1 (2026-09-11)** — hardware log showed Tales of Hearts R never
  produced a `process:` line even with `spoof720`, i.e. it crashed before any
  hooked call. Three changes: (1) the per-process table now drops an entry
  the moment its process exits or is killed (process lifecycle callbacks), so
  a new game that receives a recycled process id can no longer inherit a
  stale title and override silently; (2) `sceDisplayGetRefreshRate` is hooked
  too, tracked for every game (it is often a game's very first display call)
  and forced to 59.94 Hz for `spoof720` titles; (3) the shell id is looked up
  on demand so SceShell is never tracked as "main" during boot. Fifteen
  SceDisplay hooks now (`hooks_ok=0xFFFF`). Version banner fixed (1.3.2 had
  printed itself as "v1.50").
- **1.4 (2026-09-11)** — new per-game mode `spoof720` for titles that crash
  at start-up under the 1080p30 head but run under 720p (Tales of Hearts R,
  C2-12828-1 before its first frame): for those titles the two display
  information queries answer as a 720p60 head. Two more pass-through hooks
  (`hooks_ok` bits 13 and 14). Default and other titles unchanged.
- **1.3.2 (2026-09-11)** — the override parser now rejects title ids that are
  not exactly 9 characters and logs the ignored line (a real config had
  `PCSG000009` instead of `PCSG00009`, which silently never matched); the
  loaded overrides are listed in `kernel.log` at boot. No behaviour change
  otherwise.
- **1.3.1 (2026-09-11)** — review fixes for 1.3 (confirmed by a 46-agent
  adversarial review, no behaviour change for titles without an override):
  changing the FORCE mode from Settings no longer wipes the per-game tracker
  (a callback-synced game could have been double-paced afterwards); the
  doubled vblank counter for `frameskip` titles is monotonic (1.3 wrapped it
  at 16 bits, which could stall frame timers); the override file reload and
  the per-process table's miss path are serialised by a mutex; the frameskip
  credit is updated atomically; `nowait` still delivers thread callbacks in
  the CB variants; the table evicts the longest-idle process instead of
  round-robin.
- **1.3 (2026-09-11)** — per-game overrides in `ur0:tai/pstv1080p_games.txt`
  for the few titles that misbehave at 30 Hz. Motivation: Bloodstained:
  Curse of the Moon runs at half speed with vsync and double speed without
  it, and Ys VIII runs at half speed; both advance their game logic per
  vblank (or per vblank count) instead of per elapsed time. New `frameskip`
  mode: at 30 Hz every second vblank wait returns immediately and the vblank
  counter is reported doubled, so such games run 60 logic frames per second
  and show every second one. Also `nowait`, `off`, `inject`, `force`,
  `scale` per title. Every process is logged once with its title id
  (`process: pid=... title=PCSxxxxxxx override=...`) so you can find the ids
  in `kernel.log`. The file is re-read when a game starts: no reboot needed.
  Also migrates a state file written by 1.0/1.1 (which kept `fps_inject = 0`,
  so the 1.2 adaptive inject never ran on upgraded consoles) to the AUTO default.
- **1.2 (2026-09-11)** — adaptive inject. The first game tests showed that
  titles which never wait for vblank themselves ran unpaced at 30 Hz
  (flicker, wrong frame rate); Framecapper's Inject build used to hide that
  at the price of double-waiting every game that *does* sync (30 fps at
  60 Hz, 15 fps at 30 Hz). `fps_inject` now has three values: 0 off, 1 AUTO
  (new default): after a frame flip the plugin waits one refresh period only
  if that process made no vsync call for more than 4.5 periods, 2 always
  (old Framecapper semantics). Five pass-through hooks feed the tracker
  (`WaitSetFrameBuf`, `WaitSetFrameBufCB`, `GetVcount`, `GetVcountInternal`,
  `RegisterVblankStartCallback`; a process that registers a vblank callback
  is never injected). Also documents the hardware findings: Sony's list is
  0 automatic / 1 1080i / 2 720p / 3 480p (our item takes 4), and the boot
  apply from a kernel thread fails with `0x80010058` (ENOSYS), which is why
  1.1 issues it from SceShell's context.
- **1.1 (2026-09-11)** — first hardware test found that the entry and the
  switch from Settings work, but after a reboot the console stayed at the
  Sony-selected mode (480p) while Settings still showed 1080p. Cause: the
  boot-time apply from the kernel worker thread did not take effect and v1.0
  then recorded the unchanged 480p readback as "expected", so the watchdog
  never retried. Fixed: an apply now only counts when the driver readback
  actually changes; the boot-time attempt is executed from SceShell's own
  display syscalls (a user-process context like the Settings path that works)
  with a thread fallback after 10 s; ineffective attempts are retried with
  backoff (3, 5, 8, 12, 20, 30, 45, 60 s), at most 10 per episode and 30 per
  boot. `kernel.log` lines `apply(shell|thread|watchdog): attempt N ...
  EFFECTIVE / not effective` show what happened.
- **1.0 (2026-09-10)** — initial release.

## What it does

The PS TV's HDMI encoder can output 1080p at 30 Hz, but Sony's Settings app
only offers 480p, 720p and 1080i. This project:

1. **Adds a native Settings entry.** A user-side plugin loaded into the Settings
   app (`pstv1080p_settings.suprx`) inserts an extra `list_item` into the HDMI
   resolution list and shows it as **"1080p (30 Hz)"**. Existing entries are
   left exactly as they are — this is not a remap of 480p or any other mode.
2. **Remembers the choice in its own state file**, never in Sony's registry.
   The value the Settings app sees for the "1080p" item is served by the plugin;
   Sony's `/CONFIG/DISPLAY/hdmi_resolution_mode` registry key is never written
   with a value Sony's firmware does not know about.
3. **Applies 1080p30 automatically at boot** and re-applies it if the output
   mode drifts (HDMI re-plug, a system component switching modes), from a
   kernel plugin (`pstv1080p.skprx`).
4. **Keeps frame pacing sane at 30 Hz.** At 30 Hz there are only 30 vblanks per
   second, so a game that "waits 2 vblanks" to run at 30 fps would drop to
   15 fps. The kernel plugin rescales those waits so a 30 fps title stays at
   30 fps (and can optionally act as a refresh-rate-aware Framecapper).
5. **Reverts itself if a 1080p boot goes wrong** (see
   [safe-boot revert](#things-to-know-before-enabling-1080p)).

## Requirements

> **Sharpscale is required.** On the tested console the 1080p30 head shows no
> picture at all unless `sharpscale.skprx` is loaded: the plugin switches the
> HDMI mode, Sharpscale scales the 960x544 framebuffer into it. Never disable
> Sharpscale while 1080p is selected. If you did and the screen is black:
> power off, boot, power off again within 2 minutes, and the safe-boot rule
> restores your Sony mode on the next start.



- A PlayStation TV (VTE-1000 series). This does nothing useful on a handheld Vita.
- HENkaku / taiHEN (Enso recommended) on firmware **3.60 – 3.74**.
- A display that accepts **1080p at 30 Hz** over HDMI (most TVs do; see below).
- A way to edit `ur0:tai/config.txt`, to create or delete files under
  `ur0:data/pstv1080p/` (force-disable = delete `pstv1080p.cfg`; logging =
  create `pstv1080p_debug.txt`) and to fetch `ux0:data/pstv1080p/pstv1080p.log`
  (VitaShell, FTP, etc.). Coming from 1.x you also need it to move your old
  `pstv1080p.cfg` / `pstv1080p_games.txt` / `pstv1080p_titles.txt` out of
  `ur0:tai/` yourself: the plugin does not migrate them.

## Install

1. Copy the two built modules to `ur0:tai/`:
   - `pstv1080p.skprx`
   - `pstv1080p_settings.suprx`
2. Edit `ur0:tai/config.txt` and add the lines shown below. `*KERNEL` and
   `*NPXS10015` sections usually already exist (HENkaku's own `henkaku.suprx` is
   listed under `*NPXS10015`); just add the new line inside each section.

   ```
   *KERNEL
   ur0:tai/pstv1080p.skprx

   *NPXS10015
   ur0:tai/pstv1080p_settings.suprx
   ```

3. **Remove** any other plugin that hooks the HDMI resolution or caps frame
   rate: the old `1080p.skprx` / `1080p_480phook.skprx`, and every
   `Framecapper*.suprx` line (see
   [compatibility](#compatibility-with-other-plugins)).
4. Reboot.
5. Open *Settings > Sound & Display > HDMI resolution*. The list should now
   contain **"1080p (30 Hz)"** after Sony's entries. Select it. The picture
   should switch to 1080p30 right away and stay 1080p30 across reboots.

To go back, simply select one of Sony's resolutions in the same list.

## Things to know before enabling 1080p

- **It is 30 Hz only.** The PS TV's HDMI output path cannot do 1080p at 60 Hz
  (1080p60 is expected to fail on this hardware; 1080p30 is the mode gameblabla
  verified). The system UI and all games therefore refresh at 30 Hz. Games
  designed for 60 fps will run at 30 fps; games designed for 30 fps should
  keep running at 30 fps thanks to the frame pacing below. Input latency is
  slightly higher than at 60 Hz.
- **Some displays will not sync 1080p30.** Reported problem cases are some PC
  monitors (notably FreeSync models) and some capture cards. If your screen
  goes black after selecting 1080p, wait for the safe-boot revert (next point)
  or force-disable the mode (point after).
- **Safe-boot revert.** When the console boots with 1080p enabled, the kernel
  plugin writes a marker file and deletes it after 120 seconds of uptime
  (configurable, `safe_boot_seconds`). If the console reboots or is powered
  off within that window, the next boot finds the marker and **turns the 1080p
  option off automatically** (falling back to whatever Sony's registry says,
  usually the mode you had before). So: if you get a black screen after a
  reboot, hold the power button, boot again, and you are back on a Sony mode.
  Then re-select "1080p (30 Hz)" in Settings if it was just an accident (for
  example a quick reboot after installing another plugin).
- **Force-disable.** Delete `ur0:data/pstv1080p/pstv1080p.cfg` (via FTP/VitaShell, or from
  safe mode) and reboot; the plugin starts with 1080p off. Removing the
  `*KERNEL` line from `config.txt` has the same effect. If you cannot see
  anything at all, connect the PS TV to a different TV first, or boot into
  safe mode (which does not load plugins).
- **HDMI-CEC / hot-plug.** The plugin re-checks the output mode every 2 seconds
  and re-applies 1080p30 if it changed (rate limited to once per 5 seconds,
  and it gives up after 5 failed or ineffective applies until you change the
  setting again, so it can never turn into a periodic HDMI renegotiation).
  This is meant to survive TV power cycles and cable re-plugs; it is one of
  the untested assumptions.

## What this touches (and how to uninstall)

Nothing is changed permanently; everything runs inside taiHEN and goes away
with the two `config.txt` lines.

- **No system file is ever modified.** Nothing is written to `vs0:`, `os0:`,
  `sa0:` or `pd0:`, and the Settings app's RCO/XML files on disk are never
  touched. The HDMI resolution page is patched **in RAM only**, at the moment
  the Settings app loads it, through a taiHEN import hook. Remove the plugin
  line and the stock page is back, with no residue.
- **Sony's registry is never written by this project.** The only registry
  writes that happen are the Settings app's own writes of a Sony-defined value
  (0/1/2) when you pick 480p/720p/1080i, which are passed through unchanged.
  Selecting "1080p (30 Hz)" stores nothing in the registry.
- **All hooks are taiHEN runtime hooks** (`taiHookFunctionExportForKernel` in
  the kernel module, `taiHookFunctionImport` in the Settings plugin), released
  in `module_stop`. There is no code patching of system modules on disk and no
  `taiInject` of persistent data.
- **Files this project creates** (all its own, all safe to delete):
  - `ur0:data/pstv1080p/` — the plugin's own directory (1.6), holding
    `pstv1080p.cfg` (the 64-byte state file), `pstv1080p.boot` (the safe-boot
    marker, normally deleted about two minutes after a successful boot),
    `pstv1080p_games.txt` and `pstv1080p_titles.txt` (optional, written by you,
    never by the plugin), `pstv1080p_debug.txt` (optional, written by you:
    turns logging on). The kernel also creates the parent `ur0:data/` if it
    does not exist. Coming from 1.x: move `pstv1080p.cfg` and
    `pstv1080p_games.txt` (and `pstv1080p_titles.txt` if you have one) from
    `ur0:tai/` into this folder yourself, and delete any `ur0:tai/pstv1080p.boot`
    and `ur0:tai/pstv1080p_verbose.txt` you find there (the 1.5 verbose switch
    is replaced by `pstv1080p_debug.txt` in this folder); the plugin never
    touches `ur0:tai/`.
  - `ux0:app/PSTV10801/` — the optional Configurator app, if you installed the VPK
    (remove it from the LiveArea like any app).
  - `ux0:data/pstv1080p/` — only while the debug file exists: the log
    `pstv1080p.log` and the developer dumps (`settings_page_orig.xml`, `dump_*`).

  To uninstall: remove the two lines from `ur0:tai/config.txt`
  (`ur0:tai/pstv1080p.skprx` under `*KERNEL` and
  `ur0:tai/pstv1080p_settings.suprx` under `*NPXS10015`), then delete
  `ur0:tai/pstv1080p.skprx`, `ur0:tai/pstv1080p_settings.suprx`, the
  `ur0:data/pstv1080p/` folder and, if present, the `ux0:data/pstv1080p/`
  folder (a 1.x install may also have left `pstv1080p.*` files in `ur0:tai/`
  and `kernel.log` / `settings.log` in `ux0:data/pstv1080p/`: delete those too).
  Reboot; the HDMI resolution is whatever Sony's registry says.

## Hardware findings (PS TV, FW 3.60)

- **Tales of Hearts R (PCSE00429) under a 1080-line head (1080i or 1080p30):**
  the process was created and started, then died before its first frame
  (kernel kill, error C2-12828-1, no core dump). Memory budget, the launcher
  and `param.sfo` were all ruled out. The cause is the game's own display
  flip: with the `novsync` switch (every `sceDisplaySetFrameBuf` made
  immediate instead of next-frame) it starts and runs. So a "next frame" flip
  fails for this title under a 1080-line output mode, and nothing in the wait
  calls was ever involved (confirmed on hardware 2026-09-12).

- The HDMI list in Settings > Sound & Display is defined in Sony's
  `sound_settings_plugin` page (dumped to `docs/reversing/sound_settings_page_fw360.xml`):
  `0` Automatic, `2` 720p, `1` 1080i, `3` 480p, bound to
  `/CONFIG/DISPLAY/hdmi_resolution_mode`. The plugin's item therefore uses
  value `4`; Sony's registry never sees it.
- When you pick an entry, Sony's Settings code calls
  `sceAVConfigHdmiSetResolution(<code>)` first and writes the registry key
  afterwards. For a value it does not know it sends `0x10000000`
  ("automatic", 720p on the test TV), and the plugin switches to 0x8710 right
  after, so selecting "1080p (30 Hz)" shows a short 720p flash. Cosmetic.
- `sceAVConfigHdmiSetResolution` returns `0x80010058` (ENOSYS) when called
  from a kernel thread; from any user-process system call it works. The boot
  apply is therefore issued from SceShell's first display call after the
  configured delay (about 9 s after power-on in the log).
- `ksceDisplayGetOutputMode(1)` reports the plain screen-mode codes
  (`0x8300`, `0x8600`, `0x8710`), so no alias handling was needed.

## Per-game overrides (1.3)

Most games are fine with the defaults. A few advance their game logic once per
vblank instead of per elapsed time; at 30 Hz they run at half speed, and with
vsync removed they run too fast. For those, create `ur0:data/pstv1080p/pstv1080p_games.txt`
with one line per title:

```
# title id   mode
PCSE01221    frameskip
PCSB01206    frameskip
```

| Mode | Effect for that title |
|---|---|
| `frameskip` | At 30 Hz, every second vblank wait returns immediately and the vblank counter is reported doubled: 60 logic frames per second, every second frame shown. Identity at 60 Hz. Use for games that run at half speed. Not injected unless `inject` is added. |
| `nowait` | Every vblank wait returns immediately, like `novsync.suprx`, for this title only. No inject. Use for games that sleep on a timer *and* wait for vblank (they land at 15 fps under 30 Hz and `frameskip` does not fully fix them). |
| `off` | No pacing changes and no inject for this title. |
| `inject` | Always wait one period after each frame flip (Framecapper "Inject" semantics) for this title. |
| `force` | Framecapper-style fixed target (`fps_target`, default 60 since 1.6.6) for this title regardless of the global mode. With `inject` added this is exactly Framecapper60Inject: one vblank per wait and one after every flip at 30 Hz. |
| `scale` | The default rule, useful to exempt a title from a global FORCE mode. |
| `trace` | Diagnostic only: logs the title's process lifecycle, every memory block allocation with its result, free-memory queries and (1.6.2) a display-call profile every 5 s to `pstv1080p.log` (debug logging must be on). Slows that game's start-up slightly. Remove the line when done. |
| `novsync` (= `nowait`) | Switch that attaches to any option (or stands alone): every flip is made immediate (`SCE_DISPLAY_SETBUF_IMMEDIATE`), which is what `novsync.suprx` does; alone it also returns every vblank wait at once. For games that throttle themselves through the flip rather than through a wait call, which no other option can reach. **Also the fix for Tales of Hearts R (PCSE00429)**, which crashed with C2-12828-1 before its first frame under any 1080-line head: with `novsync` it starts and runs. The old `novsync.suprx` + `Framecapper60Inject.suprx` pair = `nowait novsync inject`. |
| `spoof720` | Pacing as usual, but the display-information queries a game makes at start-up (`sceDisplayGetMaximumFrameBufResolution`, `sceDisplayGetResolutionInfoInternal`, `sceDisplayGetRefreshRate`) answer as if the output were 720p60. Was tried for Tales of Hearts R and did not help; kept as a diagnostic. |

How to find a title id: launch the game once and read `ux0:data/pstv1080p/pstv1080p.log`;
the plugin logs `process: pid=0x... title=PCSE01221 override=none` the first
time each process touches the display. The file is read when a game starts,
so edits take effect on the next launch. Bad lines are ignored and counted in
the `games:` log line at boot.

**Testing a `frameskip` entry:** start the game, confirm the `process:` log
line now says `override=frameskip`, then check that the game speed is normal
(a timer, a run cycle, music sync) and that motion looks like 30 fps. If the
picture shows tearing or stutter, try `nowait` instead; if the speed is still
wrong, report the title and the log.

## Configurator app (1.6.1)

`pstv1080p_configurator.vpk` is a LiveArea app that edits the per-game
overrides without FTP or a text editor. It lists every installed title
(`ux0:app`, `ur0:app`, `gro0:app` and `ur0:appmeta`, names from each game's
`param.sfo`), shows the override each one has in
`ur0:data/pstv1080p/pstv1080p_games.txt`, and lets you change, add or remove
overrides and save the file. A second screen edits the global options
(1080p on/off, frame pacing mode, FORCE target fps, inject, safe-boot window,
boot delay, watchdog). Everything is read and written **through the kernel
module** (`pstv1080pReadGames` / `pstv1080pWriteGames` / `pstv1080pGetConfig`
/ `pstv1080pSetConfig`), so the app never touches `ur0:` itself; only the game
list needs the app folders, which is why the VPK must be installed as
**unsafe homebrew** (HENkaku settings › Enable unsafe homebrew, as for
VitaShell).

| Button | Games screen |
|---|---|
| Up / Down, L / R | move the selection (L/R by a page) |
| Left / Right | cycle the override (`none` → `frameskip` → `nowait` → `off` → `inject` → `force` → `scale` → `spoof720` → `trace`) |
| Cross | pick the override from a list with a one-line description of each |
| Square | remove the override (back to `none`) |
| Start | **save** to `pstv1080p_games.txt` (the kernel reloads it at once; the change applies the next time that game starts) |
| Triangle | global options screen (Left/Right change, Start apply, Circle back) |
| Select | help |
| Circle | exit (asks what to do with unsaved changes) |

A `*` after an override marks an unsaved change. Titles that have an entry in
the file but are not installed are listed at the end as "(not installed)" so
their lines are preserved. Comment lines you wrote in the file by hand stay
above the entry they were above (1.6.6); an entry the app adds gets the game's
name written above it. If taiHEN's config.txt still loads a Framecapper or
novsync plugin, a red line at the bottom names it (1.6.6). The header shows the
plugin version, whether 1080p is on and the current output mode; if the
kernel module is not loaded the app says so and does nothing else.

## Frame pacing

In the default SCALE mode nothing changes at 60 Hz: the rescaling is
mathematically an identity there, so a PS TV that is not in 1080p behaves
exactly as before. (FORCE mode caps at 60 Hz too, by design.)

There are three modes, chosen by the `fps_mode` field of the config file (there
is no config app yet, see [Config file](#config-file-ur0datapstv1080ppstv1080pcfg)):

| `fps_mode` | Name  | Behaviour |
|-----------:|-------|-----------|
| 0 | OFF   | Pacing hooks pass everything through untouched. At 30 Hz, games that wait 2 vblanks run at 15 fps (the problem the original remap had). |
| 1 | SCALE (default) | Each game's own vblank-wait count is rescaled to the current refresh rate, so the game keeps its intended frame rate where the refresh rate allows it. A "wait 2 vblanks" (30 fps at 60 Hz) becomes "wait 1 vblank" at 30 Hz. 60 fps titles become 30 fps (unavoidable at 30 Hz). |
| 2 | FORCE | Framecapper-style fixed target: every wait becomes "one frame at `fps_target` fps", computed from the *current* refresh rate. With `fps_target = 30`: 2 vblanks at 60 Hz, 1 vblank at 30 Hz. `fps_inject = 2` additionally waits after every `SetFrameBuf`, like Framecapper's "Inject" builds (`fps_inject = 1`, the default, injects only for games that do not sync themselves, in both modes). An optional title list `ur0:data/pstv1080p/pstv1080p_titles.txt` (one title id per line, or a single line `*ALL`) limits FORCE mode to the listed games (see the config section for the no-file case). |

The pacing never touches the kernel itself or SceShell (the LiveArea / system
UI), only application processes.

## Compatibility with other plugins

- **Framecapper (Rinnegatamante) — do not run both.** Framecapper hooks the same
  `sceDisplayWaitVblankStart*` calls inside each game and forces a fixed vblank
  count that assumes 60 Hz. With 1080p30 that gives 15 fps, and combined with
  this plugin's hooks the two would cap on top of each other. Remove every
  `Framecapper*.suprx` line from `config.txt` and use `fps_mode = 2` (FORCE)
  with `fps_target` / `fps_inject` if you want Framecapper-like behaviour.
- **Sharpscale (cuevavirus / CBPS) — compatible.** Sharpscale changes how the
  framebuffer is *scaled* onto the output and hooks different functions.
  1080p30 gives Sharpscale a 1920x1080 target, so 960x544 content can be shown
  at an exact 2x integer scale. Sharpscale's "unlock framebuffer sizes" option is
  unrelated to this plugin and can stay whichever way you have it.
- **The old `1080p.skprx` / `1080p_480phook.skprx` — remove them.** They hook the
  same function and would fight with the watchdog.
- **HENkaku's `henkaku.suprx` under `*NPXS10015`** stays. Both plugins hook the
  Settings app's XML loader; taiHEN chains them and this plugin only edits the
  HDMI resolution list, never the pages HENkaku replaces.

## Diagnostics and reporting issues

**Logging (1.6): off unless you turn it on.** No log is written by default (the
kernel module only keeps its own config and safe-boot marker in
`ur0:data/pstv1080p/`, a folder it creates at its first boot). To get a log,
create an empty file `ur0:data/pstv1080p/pstv1080p_debug.txt` (FTP or VitaShell;
before the very first boot with the plugin installed create the folder yourself
too) and reboot: from then on everything, kernel module and
Settings plugin alike, goes into one file, `ux0:data/pstv1080p/pstv1080p.log`
(Settings-plugin lines are prefixed `settings:`; the file is on the memory card
so it is easy to fetch). Delete the debug file and reboot to stop. The log is append-only; delete it whenever you like. To dump
the Settings app's modules from memory (for porting the native patch to
another firmware) create an empty `ux0:data/pstv1080p/dump_request` while
debug logging is on and open *Sound & Display* once; the file is removed and
`dump_*.bin/.txt` appear in `ux0:data/pstv1080p/`.

The log is plain text, written by the kernel module (best effort; failures to write are ignored); the Settings plugin's lines reach it through the `pstv1080pLog` syscall:

| File | Written by | Contents |
|------|-----------|----------|
| `ux0:data/pstv1080p/pstv1080p.log` | `pstv1080p.skprx` (debug mode only; `settings:` lines arrive via the `pstv1080pLog` syscall) | kernel lines: config load, hook install results, **every** `sceAVConfigHdmiSetResolution` call (mode requested by the system / mode actually applied / return code), boot applies, watchdog re-applies, safe-boot reverts, per-game overrides. `settings:` lines: the HDMI resolution list Sony ships, the value chosen for the injected item, every registry get/set the Settings page makes for `hdmi_resolution_mode`, the result of the in-memory ladder patch. |
| `ux0:data/pstv1080p/settings_page_orig.xml` | `pstv1080p_settings.suprx` | a one-time dump of the unmodified Settings page XML that contains the HDMI resolution list. |

When reporting a problem, please attach both files (they exist only with the
debug switch) plus your firmware version, TV/monitor model, and
`ur0:tai/config.txt`. The XML dump and the value→mode evidence in the
`settings:` lines of `pstv1080p.log` are what is needed to make the next version
more robust (they reveal Sony's registry value → screen-mode mapping, which is
currently unknown, see the appendix).

## First test on hardware

Nobody has run this on a PS TV yet. If you are the first, please test in this
order and stop at the first step that fails. Steps 1–3 cannot change the video
mode; only step 4 does.

**Preparation**

- Have FTP (VitaShell) working *before* you start, so you can delete
  `ur0:data/pstv1080p/pstv1080p.cfg` blind if the screen goes dark.
- Use a TV known to accept 1080p30 (any recent TV; avoid PC monitors for the
  first test).
- Note which HDMI resolution is currently selected in Settings.
- Create the folder `ur0:data/pstv1080p/` and an empty file
  `pstv1080p_debug.txt` inside it (FTP/VitaShell). 1.6 writes no log and no XML
  dump at all without it; the switch is read once at boot, so it must exist
  before the reboot in Step 1.

**Step 1 — kernel module loads (no visible change expected).**
With `ur0:data/pstv1080p/pstv1080p_debug.txt` in place, add only the `*KERNEL`
line, reboot. Check that the console boots normally
and that `ux0:data/pstv1080p/pstv1080p.log` exists. Look for the config-load line
(should report defaults / `mode_1080p=0`) and the hook install results: the
`SceAVConfig` hook and the frame-pacing hooks should all report success
(`uid >= 0`). If any hook failed, its line says so; the module still runs.
Untested assumptions checked here: the `SceAVConfig` export hook installs
(same as gameblabla's plugin, so this one is low risk); hooking the
`SceDisplay` user-library exports (`0x5ED8F994`) with
`taiHookFunctionExportForKernel` succeeds for the fourteen `SceDisplay` hooks
(six vblank waits plus `_sceDisplaySetFrameBuf`, which is always installed
and passes straight through unless `fps_mode = 2` with `fps_inject = 1`, plus
the two v1.4 display-information queries used by `spoof720`).
Expect fifteen `hook: ... ok` lines after the `SceAVConfig` one
(`hooks_ok=0xFFFF`; 1.2–1.3.2 install twelve, confirmed on hardware as `hooks_ok=0x1FFF`).

**Step 2 — pacing at 60 Hz is a no-op.**
Still at your usual resolution, play a 60 fps and a 30 fps game for a minute.
Both must behave exactly as before (SCALE mode is an identity at 60 Hz). If
anything stutters here, set `fps_mode = 0` (see config section) and report it.

**Step 3 — Settings plugin (still no mode change).**
Add the `*NPXS10015` line, reboot, open *Settings > Sound & Display >
HDMI resolution* but do **not** select anything yet. Check:
- the list shows Sony's entries unchanged plus **"1080p (30 Hz)"** at the end;
- the currently selected entry is still the one you noted;
- `settings_page_orig.xml` exists and `pstv1080p.log` contains `settings:` lines; they list the
  original items with their `value=` numbers and the value chosen for the new
  item (default 3, or the next free number).
If the text shows as `msg_pstv1080p_1080p` the text hook failed (report). If the
entry is missing, the `settings:` lines of `pstv1080p.log` should say why (page not found, buffer too
small, etc.).
Untested assumptions: the HDMI list lives in a page loaded through
`scePafMiscLoadXmlLayout` in `SceSettings` and contains the literal string
`hdmi_resolution_mode`; Sony's list uses `<list_item ... value="N"/>` children;
the page framework tolerates a value that is not in the registry.

**Step 4 — select 1080p (the real test).**
Select "1080p (30 Hz)". Expected: the picture switches to 1920x1080 at 30 Hz
(your TV's info overlay should say 1080p/30 or 1080p/29.97). Now check
`pstv1080p.log`: there must be a line for the `SetMode1080p(1)` request and for a
`sceAVConfigHdmiSetResolution` call with applied mode `0x8710` and return `0`.
Also check the `settings:` lines of `pstv1080p.log`: the `sceRegMgrSetKeyInt` line for the new value
must say it was *not* forwarded to the registry.
Then go back to the list: the "1080p (30 Hz)" entry must show as selected.
Untested assumptions: `0x8710` is accepted by `sceAVConfigHdmiSetResolution`
when it arrives from the Settings app's context (gameblabla's plugin proved the
value works when substituted for 480p); the Settings page re-reads the value
through `sceRegMgrGetKeyInt` and therefore shows our entry as selected.

**Step 5 — survives a reboot, safe-boot works.**
Reboot and *wait at least 3 minutes* before touching anything. Expected: the
console comes up in 1080p30 within a few seconds after the LiveArea appears
(`boot_apply_delay_ms`, default 3 s, after SceShell starts). `pstv1080p.log`
should show the boot apply. Then reboot again *immediately* (within 2 minutes):
this must trigger the safe-boot revert — the console boots in your previous
Sony mode and `pstv1080p.log` says `reverted`. Re-select 1080p in Settings.
Untested assumptions: `ur0:` is writable when the kernel module starts (needed
for the marker and config); SceShell's own boot-time resolution apply either
goes through the hooked export (so it is substituted) or happens before our
delayed apply (so it is overridden), and it does not fight back afterwards.

**Step 6 — frame pacing at 30 Hz.**
In 1080p30, run a known 30 fps title (it must feel like 30 fps, not 15) and a
known 60 fps title (it will be 30 fps). Then try `fps_mode = 2`,
`fps_target = 30` to check FORCE mode. Report anything that runs at 15 fps
with the game's name.

**Step 7 — return to a Sony mode.**
Select 480p/720p/1080i again. Expected: the mode switches immediately,
`pstv1080p.log` shows `SetMode1080p(0)` followed by Sony's own
`sceAVConfigHdmiSetResolution` call passed through unchanged, and the choice
persists across a reboot.

What to send back: `settings_page_orig.xml`, `pstv1080p.log`, the
step you reached, firmware version, display model.

---

## Developer section

### Building

Requirements: [VitaSDK](https://vitasdk.org) with the taiHEN package, and GNU
make. No CMake is needed (the `CMakeLists.txt` in the root is gameblabla's
original and is not used by this branch).

```sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
vdpm taihen          # installs taihen.h, libtaihen_stub.a, libtaihenForKernel_stub.a, libtaihenModuleUtils_stub.a

git clone -b native-1080p https://github.com/delon5/1080p_pstv
cd 1080p_pstv
make                 # builds build/pstv1080p.skprx and build/pstv1080p_settings.suprx
```

Targets: `make kernel` (skprx), `make stubs` (generates `libpstv1080p_stub.a`
from the kernel ELF's exports so the user plugin can call the syscalls),
`make user` (suprx; depends on `stubs`), `make clean`.

Both modules are freestanding C: no libc, no heap, no floating point across
function boundaries (the toolchain is softfp). The kernel side uses
`psp2kern` headers and `SceSysclibForDriver` string helpers; the user side is
compiled with `-fshort-wchar` because the Settings app expects UTF-16 titles.

**Configurator VPK.** `make configurator` (part of `make all`) needs the
vendored `third_party/vita2d/libvita2d.a` (built from xerpi/libvita2d with the
same soft-float toolchain, core + PGF members only; see
`third_party/vita2d/SOURCE_COMMIT.txt`). Do not replace it with the `vdpm`
`libvita2d` package: that one is hard-float and cannot be linked against the
snapshot toolchain's soft-float C library. The Makefile also regenerates two
stale SDK stub archives from the NID database (`build/stubs_appmgr/`). The
LiveArea images were rendered once with `configurator/assets/make_assets.swift`
(macOS), converted to 8-bit palette PNGs (the promoter rejects 32-bit PNGs
with error 0x8010113D) and are committed.

### Repository layout

```
include/pstv1080p.h            shared contract: config/info structs, constants, syscall prototypes
kernel/main.c                  pstv1080p.skprx  (mode apply, watchdog, safe boot, frame pacing, syscalls, the log)
configurator/main.c            pstv1080p_configurator.vpk (LiveArea app: per-game overrides + global options, vita2d)
third_party/vita2d/            vendored soft-float libvita2d.a + vita2d.h (MIT, see SOURCE_COMMIT.txt)
kernel/pstv1080p.yml           module/export definition (library "pstv1080p", syscall: true)
user/main.c                    pstv1080p_settings.suprx (Settings-app XML/registry/text hooks, native ladder patch)
user/pstv1080p_settings.yml
Makefile                       all / kernel / stubs / user / clean
docs/DESIGN.md                 the specification this code implements
docs/RESEARCH_NOTES.md         verified facts: NIDs, screen-mode codes, registry keys, API availability
docs/reversing/                disassembly / decompilation of the original 1080p_480phook.skprx
main.c, 1080p.yml, 1080p.skprx, CMakeLists.txt, README.txt, taihen.json
                               gameblabla's original files (branch "simp"), unmodified, for reference
```

### Config file `ur0:data/pstv1080p/pstv1080p.cfg`

The kernel module keeps all state in a **binary, little-endian, 64-byte**
file. It is created/overwritten on every change (the first time you select
"1080p (30 Hz)" in Settings, or via the syscalls). If it is missing or fails
validation, defaults are used and `mode_1080p` is 0.

| Offset | Field | Type | Default | Meaning |
|-------:|-------|------|--------:|---------|
| 0x00 | `magic` | u32 | `0x50383150` (`"P18P"`) | file identification |
| 0x04 | `version` | u32 | 2 | layout version (2 since 1.3). A file carrying version 1 (written by 1.0/1.1) is accepted once at boot and rewritten as version 2 with `fps_inject` reset to 1 (AUTO); any other value makes the file invalid. |
| 0x08 | `mode_1080p` | u32 | 0 | 1 = the "1080p" entry is selected and gets applied at boot / by the watchdog; 0 = plugin is passive |
| 0x0C | `hd_mode_code` | u32 | `0x8710` | SceDisplay screen-mode code applied when `mode_1080p` = 1. `0x8710` = 1080p30 (the one that works). Advanced/experimental: `0x8720` = 1080p24 (untested), `0x8700` = 1080p60 (expected to fail). Must have bit 0x8000 set and a resolution field in 0x300..0x700. |
| 0x10 | `settings_item_value` | u32 | 3 | the `value="N"` number of the injected `list_item`. The Settings plugin bumps it to the smallest free number ≥ 3 if Sony's list already uses it (allowed range 1..255). |
| 0x14 | `fps_mode` | u32 | 1 | 0 = OFF, 1 = SCALE, 2 = FORCE (see [frame-pacing model](#frame-pacing-model)) |
| 0x18 | `fps_target` | u32 | 60 | FORCE mode target fps: 20, 30 or 60 (default 30 before 1.6.6; a state file still at 30 is corrected to 60 once on load, without a version change) |
| 0x1C | `fps_inject` | u32 | 1 | 0 = off. 1 = AUTO (default): after a frame flip, wait one refresh period (FORCE: the forced interval) only if the process made no vsync call of its own for more than 4.5 refresh periods, so games that already sync are never double-waited. 2 = always (Framecapper "Inject" semantics). |
| 0x20 | `safe_boot_seconds` | u32 | 120 | how long a 1080p boot must survive before it is considered good; 0 disables the safe-boot revert. Clamp: values above 3600 are lowered to 3600. |
| 0x24 | `boot_apply_delay_ms` | u32 | 3000 | delay after SceShell appears before the first apply at boot. Clamp: values above 60000 are lowered to 60000. |
| 0x28 | `watchdog_period_ms` | u32 | 2000 | how often the watchdog re-checks the output mode; 0 disables the watchdog (boot apply still happens). Clamp: a non-zero value below 500 is raised to 500. |
| 0x2C | `reserved[5]` | 5 × u32 | 0 | keep zero (zeroed by the module on load/set) |

**Editing it.** There is no config app yet. The Settings entry only toggles
`mode_1080p`; everything else has to be changed with a hex editor for now (or
by a future config app calling `pstv1080pSetConfig`). To switch to FORCE mode
at 30 fps, for example, set the u32 at offset `0x14` to `02 00 00 00` and keep
`0x18` at `1E 00 00 00` (30). Edit the file with the console off or reboot
afterwards: the module reads it once at start and rewrites it on every change,
so an edit made while a change is pending would be lost.

**Validation — be careful with the hex editor.** Every field is validated
both when the file is read at boot and when `pstv1080pSetConfig` is called.
If **any** field is out of range, the **whole file is ignored**, defaults are
used and **1080p is OFF** (the console boots in a Sony mode); the only hint is
a `config: state file invalid` line in `pstv1080p.log`, and the next successful
change made through Settings overwrites the file with defaults plus that
change. The checks are: `magic` = `0x50383150`, `version` = 2 (a version-1
file is the one exception: migrated as noted in the table, not ignored),
`mode_1080p` ≤ 1, `hd_mode_code` with bit `0x8000` set, a resolution field
(`& 0x0700`) between `0x0300` and `0x0700` and no bits above `0xFFFF`,
`settings_item_value` in 1..255, `fps_mode` ≤ 2, `fps_target` in
{20, 30, 60}, `fps_inject` ≤ 2. A typo such as `fps_target = 25` therefore
silently turns 1080p off. The three timing fields are never rejected, only
clamped as noted in the table, and `reserved[]` is zeroed.

`ur0:data/pstv1080p/pstv1080p_titles.txt` (optional, FORCE mode only): one title id per
line (`PCSE00001`); lines starting with `#` are ignored; a line `*ALL` means
every application. If the file is missing, or contains `*ALL`, or has no valid
entries, FORCE pacing applies to **every** application. Up to 32 title ids are
read (ids must be shorter than 16 characters). The file is read at boot (when
`fps_mode` is 2) and when `fps_mode` is switched to 2 through
`pstv1080pSetConfig`; creating or editing it while already in FORCE mode has
no effect until you reboot. `pstv1080p.log` reports the result on a line starting
with `titles:`. SCALE mode ignores the list.

### Frame-pacing model

Games regulate their frame rate with the SceDisplay syscalls
`sceDisplayWaitVblankStart[Multi][CB]` and `sceDisplayWaitSetFrameBufMulti[CB]`,
whose `vcount` argument means "wait this many vblanks". Game code assumes a
60 Hz vblank (Vita OLED/LCD, and the PS TV in every Sony mode). The kernel
module hooks the kernel-side implementations of those syscalls (library
`SceDisplay` `0x5ED8F994`) and keeps an integer `refresh_hz` derived from the
current HDMI output mode (screen-mode flag bits `0xF0`: `0x00` → 60,
`0x10` → 30, `0x20` → 24, `0x80` → 50, `0x40` → 25 assumed; fallback 60). The
cache is refreshed by the watchdog thread and after each successful
`sceAVConfigHdmiSetResolution`, and cross-checked once with
`ksceDisplayGetRefreshRateInternal`. The hot path uses only that cached
integer: no I/O, no allocation, no floats.

Process filter: calls from the kernel itself and from SceShell pass through
untouched, so the system UI is never paced. The pacing hooks run in the
calling process's context; per-process title ids (for the FORCE title list)
are cached in a small 8-entry table so the hot path never queries sysroot.

**SCALE (default):** `new_vcount = max(1, vcount * refresh_hz / 60)`, integer
floor.

| game asks for | intended fps | at 60 Hz | at 30 Hz | resulting fps at 30 Hz |
|--------------:|-------------:|---------:|---------:|-----------------------:|
| 1 vblank | 60 | 1 | max(1, 0) = 1 | 30 (best possible) |
| 2 vblanks | 30 | 2 | 1 | **30** |
| 3 vblanks | 20 | 3 | 1 | 30 (rounded up, 20 is not representable at 30 Hz) |
| 4 vblanks | 15 | 4 | 2 | 15 |

The no-argument `sceDisplayWaitVblankStart[CB]` variants imply `vcount = 1` and
always pass through. At 60 Hz the formula is the identity, so SCALE is a no-op
whenever 1080p is off.

**FORCE:** `interval = max(1, refresh_hz / fps_target)`; the `Multi` variants
get their `vcount` replaced by `interval`; the no-argument variants pass
through when `interval == 1` and otherwise wait `interval` vblanks through
the kernel `ksceDisplayWaitVblankStartMulti[CB]`. The `_sceDisplaySetFrameBuf`
syscall is always hooked (installed once at `module_start`, so no hook has to
be added or removed while games are running); with `fps_mode = 2` and
`fps_inject = 1` the hook additionally waits `interval` vblanks after each
buffer flip, otherwise it passes straight through.

| `fps_target` | at 60 Hz | at 30 Hz | at 24 Hz |
|-------------:|---------:|---------:|---------:|
| 60 | 1 | 1 (→ 30 fps) | 1 (→ 24 fps) |
| 30 | 2 | 1 | 1 (→ 24 fps) |
| 20 | 3 | 1 (→ 30 fps, 20 not representable) | 1 |

So "a 30 fps game waits 2 vblanks at 60 Hz and 1 vblank at 30 Hz" holds in
both modes; the difference is that SCALE respects each game's own choice
while FORCE imposes one target on every (listed) game.

**OFF:** every hook returns to the original immediately. Hooks that fail to
install are logged and skipped; the module still starts and reports which
hooks are active in `pstv1080p_info_t.hooks_ok`: bit 0 = the `SceAVConfig`
export hook, bits 1–6 = `WaitVblankStartMulti`, `WaitVblankStartMultiCB`,
`WaitVblankStart`, `WaitVblankStartCB`, `WaitSetFrameBufMulti`,
`WaitSetFrameBufMultiCB`, bit 7 = `_sceDisplaySetFrameBuf`, bits 8–12 =
`WaitSetFrameBuf`, `WaitSetFrameBufCB`, `GetVcount`, `GetVcountInternal`,
`RegisterVblankStartCallback`, bits 13–14 =
`_sceDisplayGetMaximumFrameBufResolution`,
`_sceDisplayGetResolutionInfoInternal` (all fifteen set = `0x7FFF`).

### Safe-boot revert rule

At `module_start`, *before* anything is applied:

1. If `mode_1080p` is 1 **and** `ur0:data/pstv1080p/pstv1080p.boot` exists, the previous
   boot with 1080p enabled did not reach `safe_boot_seconds` of uptime. The
   module sets `mode_1080p = 0`, persists the config, and logs `reverted`.
   Nothing is applied this boot; Sony's registry value wins.
2. Otherwise, if `mode_1080p` is 1, the marker file is created. The watchdog
   thread deletes it once uptime reaches `safe_boot_seconds` (immediately if
   that is 0, which effectively disables the rule).
3. If `mode_1080p` is 0, a leftover marker is removed.

A crash, a power cut, or a deliberate quick reboot inside the window therefore
costs you one re-selection in Settings, never a boot loop into a mode the TV
cannot display.

Turning 1080p off through Settings or the syscall does not use the marker: the
kernel re-applies the last mode Sony's code requested (`last_system_mode`) if
it has seen one, otherwise it does nothing and lets the Settings app apply the
mode it writes to the registry. Only plausible Sony codes are recorded there
(bit `0x8000` set, resolution field `0x0300`..`0x0700`, never our own
`hd_mode_code`), so the revert can never re-apply 1080p by mistake.

The watchdog (and the boot apply) compares the driver's reported output mode
with `hd_mode_code`, **or** with whatever the driver reported right after the
last successful apply of that code — the driver may encode the mode
differently (untested assumption A3 in `kernel/main.c`). An apply whose
readback still differs from `hd_mode_code` is logged and counted as a
failure, so at most 5 re-applies ever happen per state change; the plugin
never renegotiates HDMI periodically.

### Kernel syscall API

Library `pstv1080p` (syscall exports, see `include/pstv1080p.h`), usable from
any user-mode module linked against `libpstv1080p_stub.a` (`make stubs`):

```c
int pstv1080pGetConfig(pstv1080p_config_t *out);       // copy of the live config
int pstv1080pSetConfig(const pstv1080p_config_t *in);  // validate, persist; apply/revert if mode_1080p changed
int pstv1080pSetMode1080p(int enable);                 // persist + apply/revert now; returns the apply result
int pstv1080pGetInfo(pstv1080p_info_t *out);           // version, current output mode, refresh_hz, last system mode,
                                                        // last apply result, hooks_ok bitmask, settings item value,
                                                        // reserved[5] = debug logging on
int pstv1080pReadGames(char *buf, uint32_t size);      // (1.6.1) copy the override file (<= size-1 bytes, NUL-terminated); returns bytes read, 0 if none
int pstv1080pWriteGames(const char *buf, uint32_t size); // (1.6.1) replace the override file with buf[0..size) (size <= 4096) and reload it
int pstv1080pLog(const char *line);                    // (1.6) append "settings: <line>" to pstv1080p.log; no-op (returns 0)
                                                        // unless the debug switch existed at boot. The kernel copies a fixed
                                                        // 199 bytes from `line`, so pass a buffer readable for >= 199 bytes.
```

Errors: `PSTV1080P_ERR_INVALID_ARG` (`0x80F18001`), `PSTV1080P_ERR_NOT_READY`
(`0x80F18002`), `PSTV1080P_ERR_APPLY_FAILED` (`0x80F18003`), or a negative SCE
error passed through. A config app can be built on these seven calls (the bundled Configurator is one); the
struct layouts are fixed (64 bytes each, static-asserted in the header).

## Appendix: how the original plugin worked and what changed

### gameblabla's `1080p.skprx` and the `1080p_480phook.skprx` variant

Two binaries, one idea:

- **gameblabla's `1080p.skprx`** (root of this repo, module name `1080p`,
  4.5 KiB, built from the root `main.c` / `CMakeLists.txt` with the HENkaku
  CMake template) forces `0x8710` for **every** requested mode, unconditionally.
- **The user's `1080p_480phook.skprx`** (`docs/reversing/*.orig`, 2 KiB,
  module name literally `ds4vita` because it was built from xerpi's `ds4vita`
  template; fully reversed in `docs/reversing/`) is the same hook with the
  `mode == 0x8300` check added, i.e. only 480p is remapped.

The 480phook variant, reconstructed:

```c
uid = taiHookFunctionExportForKernel(KERNEL_PID, &ref, "SceAVConfig",
                                     0x79E0F03F /* SceAVConfig user lib */,
                                     0x4D37F036 /* sceAVConfigHdmiSetResolution */, hook);

static int hook(int mode, ...) {
    if (mode == 0x8300)   /* 480p60 requested ... */
        mode = 0x8710;    /* ... becomes 1080p30 */
    return TAI_CONTINUE(int, ref, mode, ...);
}
```

`sceAVConfigHdmiSetResolution(int screenMode)` is the kernel-side
implementation of the syscall Sony's Settings app and SceShell call to change
the HDMI mode; it reprograms both the display controller and the HDMI
transmitter. Every time anything asked for 480p (`0x8300`), the hook silently
substituted 1080p30 (`0x8710`). gameblabla's earlier `1080p.skprx` on the
`simp` branch did the same for *every* mode, unconditionally; the `480phook`
build is gameblabla's "TODO: add a check" done.

Consequences of that approach:

- No real "1080p" choice: you selected **480p** in Settings and got 1080p.
  The registry said 480p, the UI said 480p.
- If the TV refused the mode there was no way back except removing the plugin.
- Frame pacing was ignored: at 30 Hz, every 30 fps game (and every Framecapper
  cap) ran at 15 fps — the problem that motivated this rewrite.
- The template's logging code was dead; there was no way to see what happened.

### What this version does differently

| | original hook | pstv1080p |
|-|---------------|-----------|
| Settings UI | remaps the 480p entry | adds a native **"1080p (30 Hz)"** list item; 480p stays 480p |
| State | none (implicit: 480p selected) | own file `ur0:data/pstv1080p/pstv1080p.cfg`; Sony's `hdmi_resolution_mode` key is **never** written with an unknown value |
| Boot | relied on Sony re-applying 480p at boot and the hook catching it | explicit apply from a kernel thread after SceShell is up, plus a watchdog that re-applies if the mode drifts |
| Recovery | remove the plugin | safe-boot revert (marker file + 120 s window), force-disable by deleting the cfg |
| Frame pacing | none | refresh-rate-aware SCALE (default) or FORCE modes on the SceDisplay vblank syscalls |
| Other modes | only 480p affected | all of Sony's modes pass through unchanged when 1080p is off; the requested mode is logged as mapping evidence |
| Diagnostics | none | one log `ux0:data/pstv1080p/pstv1080p.log` (kernel + `settings:` lines, only while `ur0:data/pstv1080p/pstv1080p_debug.txt` exists), XML dump (debug only) |

The `sceAVConfigHdmiSetResolution` export hook is still the core mechanism —
it is the one thing the original plugin proved works on real hardware. The
difference is *when* the substitution happens (only while our own state says
1080p is selected, regardless of which Sony mode was requested) and that the
plugin remembers what the system wanted (`last_system_mode`) so it can restore
it when 1080p is switched off.

The Settings-app side uses the technique HENkaku's `henkaku.suprx` uses for
its own settings page: hook `scePafMiscLoadXmlLayout` to patch the page XML,
`sceRegMgrGetKeyInt` / `sceRegMgrSetKeyInt` to intercept the one key the list
is bound to, and `scePafToplevelGetText` to supply the entry's text. The
plugin only edits the list that contains `hdmi_resolution_mode` and passes
every other page through, so it coexists with HENkaku (and with plugins such
as SettingsPlus / ineedsettings as long as they do not also rewrite that list).

Sony's mapping from registry value (0/1/2) to screen-mode code is done in
Sony's code and is still unknown; the injected item uses a value Sony does not
use (3 by default) that never reaches the registry. The `settings:` lines of `pstv1080p.log` record the
original items and every code Sony passes to `sceAVConfigHdmiSetResolution`,
so the mapping can be documented from the first on-device reports.

## Credits

- **gameblabla** — [1080p_pstv](https://github.com/gameblabla/1080p_pstv): discovered that
  the PS TV HDMI encoder accepts 1080p30 and wrote the original hook this
  project descends from.
- **HENkaku / molecule** — the Settings-app XML / registry / text hooking
  technique (`plugin/user.c`).
- **SKGleba** — ineedsettings (Settings-app extension reference).
- **Rinnegatamante** — Framecapper (the frame-cap hook set the FORCE mode mirrors).
- **CBPS / cuevavirus** — Sharpscale and the SceDisplay reverse engineering it documents.
- **wiki.henkaku.xyz** — SceDisplay screen-mode flags, SceAVConfig, registry documentation.
- **xerpi** — the ds4vita template the original binary was built from.
- **Team Molecule / yifanlu** — taiHEN.

## License

New code on this branch (`include/`, `kernel/`, `user/`, `Makefile`, `docs/`)
is licensed under the **GNU General Public License v3.0 or later**
(`SPDX-License-Identifier: GPL-3.0-or-later`); see [LICENSE](LICENSE).

The files inherited from gameblabla's `simp` branch (`main.c`, `1080p.yml`,
`1080p.skprx`, `CMakeLists.txt`, `README.txt`, `taihen.json`) were published
without a license statement. They are kept unmodified for reference and
credited to gameblabla; the GPL notice does not cover them.
