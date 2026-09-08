# Adrenaline+ — RetroAchievements for PSP on PS Vita

**Adrenaline+** is a fork of the [isage/Adrenaline](https://github.com/isage/Adrenaline) PSP
emulator for the **PlayStation Vita**, extended with a full
[RetroAchievements](https://retroachievements.org) integration. It turns the Vita's built-in
PSP emulator into a retroachievement-capable PSP console: log in, earn PSP achievements,
see unlock toasts + chime, and a PS3-style trophy collection.

Built on **isage/Adrenaline** (itself a continuation of
[TheOfficialFloW/Adrenaline](https://github.com/TheOfficialFloW/Adrenaline)); the base runs a
full PSP 6.61 custom firmware via the taiHEN framework / ePSP.

## Features

- **RetroAchievements login** — token-based, persists across boots; custom TLS (curl + OpenSSL)
  works inside the PSP-emulator process where the Vita's own HTTPS stack cannot load.
- **PSP game detection + hashing** — auto-identifies the running PSP ISO/CD and hashes it
  (MD5 of PARAM.SFO + EBOOT.BIN) against the RetroAchievements set.
- **Trophy collection** — a PS3-style game list with per-game achievement grids, badge icons,
  and unlock status, opened from the PS button → Trophies tab.
- **Unlock notifications** — toast + synthesized chime on achievement earned, during gameplay.
- **Offline queueing** — unlocks are cached and synced when connectivity returns.
- **Hardcore mode** (see below) with enforcement + a cheat-plugin gate.

## Hardcore mode

Hardcore mode is opt-in (default off). When on, it enforces the RetroAchievements hardcore
rules:

- **Save-state loading is blocked** — both at the UI (the load option reads
  "Load State (blocked: Hardcore)") and at the wire layer (the RAM write is refused while the
  response still completes so the session does not hang).
- **No mid-session mode switch** — hardcore is applied once at app init when no game is loaded;
  toggling it only takes effect at the next launch. There is no path to switch casual→hardcore
  mid-game.
- **Cheat-plugin gate** — a shared `SceAdrenaline` struct field flags hardcore to the PSP-CFW
  layer, which forces `g_disable_plugins`, so CWCheat / cheat devices and all CFW plugins are
  disabled during a hardcore session.

The integration uses the official **[rcheevos](https://github.com/RetroAchievements/rcheevos)
12.4.0** client library, vendored under [`user/rcheevos/`](user/rcheevos/).

## Building

The build is done in the VitaSDK container (see the `Dockerfile` / `vita-build-server`
deployment). It requires the **pspsdk** (for the `cef/` PSP-side modules) + **VitaSDK**, then a
full three-stage build (PSP/MIPS cef, kernel, user). The output is `Adrenaline.vpk`.

## Usage

Install the `.vpk` over your existing Adrenaline bubble (or fresh). Login via the Trophy tab,
or place a `ra_login.txt` credentials file at `ux0:data/PSPEMUCFW/`.

## License

This fork inherits **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)). The vendored RetroAchievements
runtime (rcheevos, MIT) and other components are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## Credits

- [isage/Adrenaline](https://github.com/isage/Adrenaline) — the base fork this builds on
- [TheOfficialFloW/Adrenaline](https://github.com/TheOfficialFloW/Adrenaline) — original Adrenaline
- [RetroAchievements](https://retroachievements.org) + [rcheevos](https://github.com/RetroAchievements/rcheevos)
