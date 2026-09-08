# Adrenaline+ RetroAchievements — Source Change Log

All edits made for the trophy-screen feature (Stage 0 + Stage 1), 2026-09-01.
Companion documents: `/kaniko-builds/vita/TROPHY-IMPLEMENTATION-PLAN.md` (plan),
`/kaniko-builds/vita/TROPHY-BUILD-REPORT.md` (build result).

## Stage 0 — Vita side

| File | Status | Change |
|---|---|---|
| `user/rcheevos/` | NEW (vendored, 56 files) | rcheevos 12.3.0 `include/` + `src/` (rapi, rcheevos, rhash, rc_client, rc_compat, rc_util, rc_version). Untouched upstream sources; compiled directly into `adrenaline_user`. |
| `user/ra/ra.h` | NEW | Public API of the RA subsystem (`ra_init`, `ra_shutdown`, `ra_is_logged_in`, `ra_get_client`, `ra_view_opened/closed`, `ra_draw_trophy_tab`, `ra_tick`, `ra_trophy_open_requested`). |
| `user/ra/ra_internal.h` | NEW | Internal (module-cross) declarations. |
| `user/ra/ra_client.c` | NEW | rc_client create/login/auto-login/game-load, memory+time+hash callbacks, credential/token persistence. |
| `user/ra/ra_net.c` | NEW | SceNet/SceNetCtl/SceHttp/SceSsl bootstrap, server_call worker thread, badge HTTP GET. |
| `user/ra/ra_badges.c` | NEW | Badge fetch → disk cache → lodepng decode → vita2d texture (LRU-capped). |
| `user/ra/ra_trophy_view.c` | NEW | Trophy tab view model + vita2d rendering + inline IME login. |
| `user/menu.c` | EDIT | 5th "Trophies" tab (`tab_entries[4]`, `TAB_TROPHIES=4`); draw + input branches; `EnterAdrenalineMenu()` jumps to the tab and calls `ra_view_opened()` when `ra_trophy_open_requested`; `ExitAdrenalineMenu()` calls `ra_view_closed()`; `ra_tick()` per frame. |
| `user/main.c` | EDIT | `ra_init()` at app start; Kermit dispatch branch for `ADRENALINE_VITA_CMD_OPEN_TROPHIES` (flag-only, responds immediately). |
| `user/CMakeLists.txt` | EDIT | rcheevos glob sources + `ra/` sources in `add_executable`; include dirs for rcheevos; `RC_CLIENT_SUPPORTS_HASH` + `RC_DISABLE_LUA` defines; `SceHttp_stub` + `SceNetCtl_stub` + `SceSsl_stub` link. |
| `adrenaline_vita.h` | EDIT | Appended `ADRENALINE_VITA_CMD_OPEN_TROPHIES` (value 16) as the last enum member — append-only. |

### Build-fix edits (documented per task instruction)

| Fix | Type | Why |
|---|---|---|
| `user/ra/ra_client.c`: added `#include <ctype.h>` | source edit | Needed for the `_U/_L/_N/_S/_P/_C/_X/_B` class-flag macros used by the `_ctype_` table initializer. |
| `user/ra/ra_client.c`: defined `const char _ctype_[257]` next to the existing `clock_gettime` shim | source edit | `adrenaline_user` links `-nostdlib`; the newlib ctype macros (`isdigit`, `isspace`, `tolower`, …) used by rcheevos index into `extern const char _ctype_[]`, which `SceLibc_stub` does not export. The table was extracted byte-for-byte from the toolchain's own newlib (`libc.a:lib_a-ctype_.o → .rodata._ctype_`, 257 bytes) via `arm-vita-eabi-objcopy` and machine-verified identical (python comparison, byte-exact) before the rebuild. Notable: newlib marks TAB and CR as `_C|_S` (blank-TAB is handled by the `isblank` macro's `== '\t'` fallback) and digits as `_N` only (`isxdigit` checks `_X|_N`). No `_tolower_tab_`/`_toupper_tab_` symbols exist in this newlib — `tolower`/`toupper` are macro forms over the same table, so one table suffices. |
| `-std=gnu17 -Wno-error=return-mismatch -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types` on the Vita configure line | build flags | Carried over verbatim from BUILD-REPORT Fixes A+C (GCC 15 C23 `TAI_CONTINUE` breakage and the `msfs.c` permerror). |
| `cmake --build build --target kernel_all -j6` before the parallel build | invocation ordering | Carried over from BUILD-REPORT Fix B (missing `adrenaline_vsh → kernel_all` dependency edge). |

## Stage 1 — PSP/XMB side + pinned SDK

| File | Change |
|---|---|
| `cef/core/xmbctrl/include/xmbctrl.h` | Added `sysconf_trophies_action_arg = 0x1024` (action-arg enum) and `CUSTOM_ID_RA_TROPHIES = 98` (CustomId enum). |
| `cef/core/xmbctrl/src/patch_vsh.c` | (1) New statics `g_trophy_item`, `ra_trophies_added`, `RA_TROPHY_NONDELEGATING` (default 0). (2) Re-patch of `g_trophy_item->action` when `g_sysconf_action` is learned. (3) Icon insertion block cloning the CFW-Settings block (trigger `msgtop_sysconf_theme`, respects `no_xmb_cfw_items`, uses `g_console_item` as icon source). (4) `ExecuteActionPatched` branch for `sysconf_trophies_action_arg`: fire `sctrlSendAdrenalineCmd(ADRENALINE_VITA_CMD_OPEN_TROPHIES, 0)`, then delegate to Sony's System Settings action with `g_is_cfw_config = 0` (variant a; variant b `return 0` behind `#if RA_TROPHY_NONDELEGATING`, off). (5) `scePafGetTextPatched` case returning `★ Trophies` for `msgtop_sysconf_ratrophies`. |
| `external/psp-cfw-sdk/` | NEW: `git clone pspdev/psp-cfw-sdk` checked out at `9a6a90b047156f353a6749ff967c2b7f207b063b` (the commit verified working in BUILD-REPORT); `include/systemctrl_epi.h` got the identical `ADRENALINE_VITA_CMD_OPEN_TROPHIES` append; cef configured with `-DPSPCFWSDK_PATH=/workspace/source/external/psp-cfw-sdk` (no edits to `_deps/`, no network fetch of `main`). |

Nothing in `SceAdrenaline` or any other struct was changed; the enum appends are the only bridge-level change.

## v10 — ISO hashing path (2026-09-02)

Supersedes the closing sentence of Stage 1: **`SceAdrenaline` HAS now changed** (append-only).
Implements Option B of `/kaniko-builds/workspaces/investigation-result/iso-hashing-path.md`.

| File | Change |
|---|---|
| `external/psp-cfw-sdk/include/systemctrl_epi.h` | Appended `char iso_path[256]` to `SceAdrenaline` (PSP-side definition). |
| `adrenaline_vita.h` | Appended the two ints the Vita copy was **missing** (`api_type`, `fake_api_type`) and then `char iso_path[256]`, so the Vita and PSP definitions are byte-identical again. Added `_Static_assert(sizeof(SceAdrenaline) <= ADRENALINE_SIZE)`. Struct is now 1728 B (limit 8192). |
| `cef/core/pentazemin/src/adrenaline.c` | `initAdrenalineInfo()` publishes `sctrlSEGetUmdFile()` into `g_adrenaline->iso_path` (bounded `strncpy` + explicit NUL). Also hardened the pre-existing unbounded `strcpy` into `filename` to a bounded copy. Added `#include <systemctrl_se.h>`. |
| `user/ra/ra_client.c` | `ra_hash_worker_job()` prefers `iso_path` over `filename` and logs both unconditionally (`[RA] hash job: iso_path='%s' filename='%s'`). `ra_translate_psp_path()` gained an `ms0:/__ef0__` branch (before the plain `ms0:/` branch), mirroring `user/msfs.c:52-55`. |

Why: for a UMD/ISO launch `sceKernelInitFileName()` returns `disc0:/PSP_GAME/SYSDIR/EBOOT.BIN`, a
virtual PSP device with no Vita-side file mapping, so RA hashing aborted with
`RA_HASH_REASON_BAD_PATH`. `sctrlSEGetUmdFile()` is the live `ms0:/ISO/<name>.iso` the ISO driver
reads sectors from, and `ms0:/` is a prefix the Vita side already translates.

Build order matters: `cef` (PSP) first — its PRXs are linked into `adrenaline_user.suprx` as binary
resources (`user/CMakeLists.txt:82-88`) — then the Vita side.

Known limitation (unchanged, pre-existing): `.cso`/`.zso` images still cannot be hashed; rcheevos'
cdreader has no CISO layer. Those now fail with "Could not hash" instead of "not accessible".

---

## v19 Part A — PSP-only gate in the RA hash worker (2026-09-03)

Plan: `/kaniko-builds/vita/V19-XMB-EXTRAS-PLAN.md` Part A. Vita-side only; cef NOT rebuilt for
this artifact (the embedded `xmbctrl.prx` is bit-identical to the one shipped in v18).

| File | Change |
|---|---|
| `user/ra/ra_internal.h` | Added `RA_HASH_REASON_POPS_GAME 4` after `RA_HASH_REASON_HASH_FAILED 3`. Append-only — existing values are persisted in `ra_fifo_entry.hash_reason` completions and must not be renumbered. |
| `user/ra/ra_client.c` | `ra_hash_worker_job()`: early return on `adrenaline->pops_mode`, placed immediately after the `ScePspemuConvertAddress` null check and **before** the `iso_path`/`filename` log, path translation and `ra_local_hash()`. Sets `*reason_out = RA_HASH_REASON_POPS_GAME`; `*ok_out` stays 0 from the prologue. |
| `user/ra/ra_client.c` | `ra_client_hash_done()`: new `case RA_HASH_REASON_POPS_GAME` setting `"Not a PSP game - trophies unavailable for PS1/POPS titles."` (ASCII only — plain status-line font, not the XMB label path). |

Why: `ra_local_hash()` hardcodes `RA_CONSOLE_PSP`, so a POPS/PS1 title was being run through
rcheevos' PSP parser over a PSISOIMG/PSAR-wrapped PBP that has no cdreader decoder — a guaranteed
failure preceded by a pointless file-I/O round trip, surfacing as a generic "Could not hash ...".
Before v19 the RA subsystem read `pops_mode` in exactly zero places.

The v15 revalidation path (`ra_client_hash_done`, the `!done->hash_ok` + `ra_game_loaded` branch)
already unloads a stale rc_client session before the reason switch runs, so the "was PSP, now POPS"
transition is handled with no extra logic: the gate produces `hash_ok == 0`, which is exactly the
condition that branch keys on.

Threading unchanged: the worker only writes `*reason_out`; `ra_status_message` stays render-owned.

## v19 Part B — DEBUG=4 discovery build (2026-09-03)

No source change. `cef` reconfigured into a **separate** `cef/build-debug` tree with `-DDEBUG=4` so
the pre-existing `logmsg4` trace in `AddVshItemPatched()` (`cef/core/xmbctrl/src/patch_vsh.c:210-212`)
compiles to a real `_logmsg` call and `logInit("ms0:/log_xmbctrl.txt")` becomes live. Artifact:
`v19-extras-discover.vpk` — a throwaway diagnostic build, NOT a release.

**Provenance hazard, worse than the plan's E2 assumed.** `copy_prx_flash0()` is a POST_BUILD step
writing into the shared `user/flash0/` tree, so the DEBUG build overwrote **13** packed PRX modules,
not just `xmbctrl.prx`. Two further findings:

1. A plain `cmake --build cef/build` does **not** undo it — CMake considers the release targets
   up to date, so the POST_BUILD copy never re-runs and the debug PRXs stay in `user/flash0/`.
   Restoring release provenance required wiping and reconfiguring `cef/build`.
2. The plan's proposed provenance check ("compare sha256 against the pre-B value") does **not**
   work in this tree. A clean release rebuild yields `xmbctrl.prx` = `33fd7349...`, not the
   v18-shipped `ac9c7946...`, despite identical packed size (19,249 B), identical `~PSP` header
   bytes and identical embedded size fields; the streams diverge at offset 321, inside the
   compressed payload. The pack is reproducible run-to-run from identical sources (verified by
   touch+rebuild), so the v18 prx came from different inputs. **Use size (19,249 B) plus
   `psp-objdump -r ... patch_vsh.c.obj | grep -c _logmsg` == 0 as the release check instead.**
