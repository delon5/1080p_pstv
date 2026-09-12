# pstv1080p — native 1080p (30 Hz) for PlayStation TV

A taiHEN plugin that gives the PS TV a real **1080p (30 Hz)** entry in
Settings → Sound & Display → HDMI Resolution, applies it at boot, and adapts
game frame pacing to the lower refresh rate.

Nothing on the console is modified permanently. The Settings entry is created
by patching Sony's own value-to-mode table in memory at runtime, so the entry
behaves like a stock option while the plugin is loaded and disappears with it.
The registry is never written with a value the system does not already know.

## What is in it

- **1080p30 output**, applied at boot and re-applied if the system drifts off
  it. A safe-boot guard reverts to the system mode if the console fails to
  stay up across several short boots.
- **Per-game frame pacing.** A 30 Hz output halves the rate of any game that
  paces itself on vblank. Each title can be given one rule, in
  `ur0:data/pstv1080p/pstv1080p_games.txt`:

  | Rule | Effect |
  |---|---|
  | `scale` | Rescale multi-vblank waits, the default |
  | `frameskip` | Every second wait returns at once, so a 60 fps game keeps its speed |
  | `nowait` | Every vblank wait returns at once |
  | `force` | Fixed target, Framecapper style |
  | `off` | No pacing for this title |

  Extras stack on a rule: `inject`, `novsync`, `syncflip`, `smooth`,
  `spoof720`, `trace`.
- **A Configurator app.** Lists every installed game by its real name from its
  `param.sfo`, sets the rule and extras on a row, and writes the file through
  the kernel module. No typing title IDs.
- **Quiet by default.** No log is written unless
  `ur0:data/pstv1080p/pstv1080p_debug.txt` exists.

## Install

1. Copy `pstv1080p.skprx` to `ur0:tai/` and add it under `*KERNEL` in
   `config.txt`, and `pstv1080p_settings.suprx` under `*NPXS10015`.
2. Install `pstv1080p_configurator.vpk` if you want the GUI.
3. Reboot, then pick **1080p (30 Hz)** in Settings.

Sharpscale must be installed for 1080p output to work on PS TV.

## Credits

This plugin stands on other people's work, and several of its behaviours were
derived by reading theirs.

- **[1080p_pstv](https://github.com/gameblabla/1080p_pstv)** by **gameblabla** —
  the original plugin this is a fork of, and the source of the working
  1080p30 mode code for PS TV.
- **[taiHEN](https://github.com/yifanlu/taiHEN)** by **Yifan Lu** — the plugin
  framework everything here runs on.
- **[Sharpscale](https://github.com/psv-plugins-archive/sharpscale)** by
  **浅倉麗子 (Asakura Reiko)**, published as **cuevavirus** — required for 1080p
  output on PS TV.
- **[novsync](https://github.com/junminlee2004/novsync)** by **junminlee2004** —
  its source defined what `novsync` means here: the eight vblank wait calls
  return at once and the flip is left alone. It credits
  **[VGi](https://github.com/Electry/VGi)** by **Electry** for the original
  vsync-off approach.
- **[Framecapper](https://github.com/Rinnegatamante/Framecapper)** by
  **Rinnegatamante** — the fixed-target capper whose semantics `force` and
  `inject` reproduce. The `Framecapper60` builds that circulate are community
  modifications of it, and I could not identify their author.
- **[VitaGrafixConfigurator](https://github.com/Kirezar/VitaGrafixConfigurator)**
  by **Kirezar** — the model for the Configurator app.
- **[libvita2d](https://github.com/xerpi/libvita2d)** by **xerpi** (Sergi
  Granell) — the Configurator's rendering.
- **[vitasdk](https://github.com/vitasdk)** and its
  **[vita-headers](https://github.com/vitasdk/vita-headers)** NID database, by
  the Vita SDK team — toolchain and every NID used here.
- **[vita-parse-core](https://github.com/xyzz/vita-parse-core)** by **xyzz** —
  used to read crash dumps while debugging.

GPL-3.0, same as the plugin it forks.
