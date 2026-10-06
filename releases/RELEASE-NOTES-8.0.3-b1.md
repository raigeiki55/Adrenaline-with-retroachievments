# Adrenaline+ 8.0.3-b1 — RetroAchievements for PSP on PS Vita

PSP games on the Vita, with real RetroAchievements: log in, earn achievements, get unlock
toasts and a chime, and browse a PS3-style trophy collection. Built on
[isage/Adrenaline](https://github.com/isage/Adrenaline) (a continuation of
[TheOfficialFloW/Adrenaline](https://github.com/TheOfficialFloW/Adrenaline)), running a full
PSP 6.61 custom firmware through the Vita's own PSP emulator.

## What's new since v33

### Smoother gameplay — the hitching is fixed

Badge loading used to stall the frame. That turned out to be **our own logging**: every log
line opened, wrote and closed a file on the memory card, on the render thread, at a measured
**29 ms per line** — and award moments produce a burst of them.

- The log file is now opened **once** instead of per line (**29.4 → 15.3 ms/line**).
- The hottest log points are **off by default**, removing the rest.

Measured on hardware: **badge frames now land inside one 60 Hz frame 83% of the time, versus
18% before.**

### Switching games no longer shows the wrong trophies

Opening the trophy list right after changing games used to paint the *previous* game's list
before updating. It now shows "Checking game…" and loads the right one.

### Plus

- Timestamps and far better diagnostics in the log
- Achievement data fetched and cached more efficiently
- Various stability fixes across networking and badge handling

## Installing

1. Install the `.vpk` over your existing Adrenaline bubble (or fresh).
2. On a fresh install, press **X** at the first boot prompt to auto-download the PSP 6.61
   firmware. Offline? Put it at `ux0:data/PSPEMUCFW/661.PBP`.
3. Log in from the **Trophies** tab (PS button → Trophies), or drop a `ra_login.txt` at
   `ux0:data/PSPEMUCFW/`.

## One thing that changed under your feet

**Detailed logging is now off by default** — that's the speed fix. If you're chasing a bug and
need the log back:

1. Create an **empty file** at `ux0:data/PSPEMUCFW/ra_verbose_log` (no extension).
2. **Fully restart** Adrenaline — it's read once at startup.
3. Delete the file and restart to go back to the fast default.

The log lives at `ux0:data/adrenaline_user_log.txt`.

## Notes

- **Hardcore mode** is opt-in and enforced: save-state loading is blocked, there's no
  mid-session switch, and cheat plugins are disabled while it's on.
- Achievements are for **PSP games**. PS1/POPS titles aren't covered.
- Built on **rcheevos 12.4.0**.

## Checksums

```
AdrenalinePlus-8.0.3-b1.vpk
sha256  20ac051372751f9d5644992b7932a96bd39dd4715b22af1cb68ad9afead1f1c6
```

## Credits

- [isage/Adrenaline](https://github.com/isage/Adrenaline) — the base fork this builds on
- [TheOfficialFloW/Adrenaline](https://github.com/TheOfficialFloW/Adrenaline) — original Adrenaline
- **[PPSSPP](https://github.com/hrydgard/ppsspp)** — its RetroAchievements client was a key
  reference for ours, alongside [RetroArch](https://github.com/libretro/RetroArch) and
  [DuckStation](https://github.com/stenzek/duckstation)
- [RetroAchievements](https://retroachievements.org) + [rcheevos](https://github.com/RetroAchievements/rcheevos)

**License:** GPL-3.0-or-later
