/*
 * Adrenaline+ RetroAchievements subsystem — public API
 *
 * Architecture (Option C): all RA data, badges and UI are rendered Vita-side
 * by the AdrenalineDraw thread; only a scalar (ADRENALINE_VITA_CMD_OPEN_TROPHIES)
 * crosses the Kermit bridge from the PSP/XMB side.
 *
 * Threading model (single writer):
 *  - every rc_client_* call happens on the render thread (AdrenalineDraw,
 *    low priority) inside ra_tick();
 *  - the network worker thread only performs HTTP and posts completion
 *    records to a lock-free SPSC FIFO drained by ra_tick();
 *  - rc_client itself spawns no threads.
 */
#ifndef ADRENALINE_RA_H
#define ADRENALINE_RA_H

#include <stdint.h>

#include "rc_client.h"
#include "rc_hash.h"

#include "ra_toast.h"   /* achievement-unlock toast overlay (push/tick/draw) */

/* Paths (prefix only; the RA data dir is created under ux0:data/PSPEMUCFW) */
#define RA_DATA_DIR        "ux0:data/PSPEMUCFW"
#define RA_LOGIN_CFG       "ux0:data/PSPEMUCFW/ra_login.cfg"
/* v32 — RetroAchievements hardcore opt-in marker. Existence of this file IS
 * the setting: present => hardcore at the NEXT Adrenaline launch. Read once
 * in ra_init() (before any game exists) and never re-read mid-session, so a
 * casual->hardcore flip with a game loaded — the rcheevos waiting_for_reset
 * trap — is structurally unreachable. Written/deleted by the Settings-tab
 * toggle only; deliberately NOT an AdrenalineConfig field (no struct growth,
 * no 7.17 migration touch). Delete the file to opt out. */
#define RA_HARDCORE_CFG    "ux0:data/PSPEMUCFW/ra_hardcore"
/* v26 — IME-free file-login fallback. Same data dir as ra_login.cfg. Format:
 *   username=...
 *   password=...
 * Consumed at boot (ra_start_boot_net) or on a Triangle login press; deleted
 * after the first successful login (the token then lives in ra_login.cfg). */
#define RA_LOGIN_TXT       "ux0:data/PSPEMUCFW/ra_login.txt"
#define RA_BADGES_DIR      "ux0:data/PSPEMUCFW/ra_badges"
#define RA_CACHE_DIR       "ux0:data/PSPEMUCFW/ra_cache"

/* Console id for the PSP (rcheevos rc_consoles) */
#define RA_CONSOLE_PSP     41

typedef struct ra_login_info {
	char username[64];
	char password[64]; /* kept only while the IME buffer is alive; never persisted */
	int valid;         /* username && password non-empty */
} ra_login_info;

/* Subsystem lifecycle ------------------------------------------------------ */

/* Phase A of the two-phase network bring-up: the SCE_SYSMODULE_NET load and
 * nothing else.
 *
 * v9 removed the stage 0 ScePaf load and the SSL/HTTP/HTTPS ladder. They
 * existed solely to bring up Sony's TLS stack, which never once loaded inside
 * ScePspemu on this hardware (0x8002D0F3 on every logged run). TLS now comes
 * from statically linked curl + OpenSSL, whose only sysmodule dependency is
 * NET -- the one that has always loaded here.
 *
 * DO NOT CALL FROM module_start(). v4 did exactly that and the console
 * black-screened after the Adrenaline logo (v4-black-screen-opus.md). The one
 * and only caller is ra_net_ensure_started(), on the deferred trophy-menu-open
 * path, on the render thread, once the compat/draw threads exist. Declared here
 * only because ra.h is the subsystem's public header; there is no boot-time
 * caller and there must never be one.
 *
 * Deliberately does NOT call sceNetInit/sceNetCtlInit/curl_global_init --
 * those stay in Phase B (ra_net_ensure_started), after the load. Idempotent and
 * safe to call again.
 * Returns 0 once SceNet is resident, negative otherwise. */
int ra_net_preload_modules(void);

/* Must be called once from InitAdrenaline(). Safe to call again (no-op). */
void ra_init(void);

/*
 * v24: start the boot-time network bootstrap on the dedicated RA_BootNet
 * worker. Call once from InitAdrenaline(), immediately after ra_init().
 *
 * This is the change that makes achievement tracking live from GAME BOOT
 * rather than from the player's first Adrenaline-menu-open. It is deliberately
 * asynchronous: the render thread is never blocked on it, and the actual
 * rc_client login still happens on the render thread from ra_tick(), so the
 * subsystem's single-writer rule is untouched.
 *
 * No-op (and no thread) when no token is saved -- those users keep the v23
 * lazy path. Still NOT safe to call from module_start(); this belongs in
 * InitAdrenaline(), after the compat/draw threads exist. */
void ra_start_boot_net(void);

/* Tear everything down (net, client, textures). */
void ra_shutdown(void);

/* Returns non-zero once initialized. */
int ra_is_initialized(void);

/* Called every frame from AdrenalineDraw. Drains the net-completion FIFO,
 * polls the IME login dialog, drives rc_client idle processing. */
void ra_tick(void);

/* Login state -------------------------------------------------------------- */

int ra_is_logged_in(void);
int ra_is_login_in_progress(void);

/* v24: non-zero while an automatic login is still queued and expected to run.
 *
 * The three login states the UI must distinguish are: logged in, a login is
 * happening/about to happen, and genuinely signed out. v23 only exposed the
 * first two, so the trophy view rendered "about to happen" as "signed out" and
 * showed an offline banner over a perfectly healthy session
 * (investigation-result/autologin-tracking-fix-scope.md, CRITICAL-2/E8). */
int ra_is_login_pending(void);

/* v24: non-zero once the server has specifically REJECTED the saved token
 * (RC_INVALID_CREDENTIALS / RC_ACCESS_DENIED / RC_EXPIRED_TOKEN) -- the
 * password-change case. The token has been erased from disk by then; the UI
 * should ask the user to sign in again rather than report a transport error. */
int ra_is_login_stale(void);

/* Credentials persistence: stores username + token only, never the password. */
int ra_save_credentials(const char *username, const char *token);
int ra_load_credentials(char *username, int username_size, char *token, int token_size);
int ra_has_saved_credentials(void);

/* v24: erase the persisted token. Called only when the server has rejected it
 * (see ra_is_login_stale); never on a transport/TLS failure, where the token is
 * still perfectly good and only the network is missing. */
int ra_clear_credentials(void);

/* v25: explicit sign-out (the trophy tab's Triangle hotkey).
 *
 * Tears down BOTH halves of the session: rc_client_logout() for the library
 * side (which also unloads the current game) and this module's mirrored state
 * (ra_is_logged_in(), the welcome line, every queued login/game-load), then
 * calls ra_clear_credentials() so the next boot does not silently sign the user
 * straight back in. Synchronous -- ra_is_logged_in() reads 0 on return. */
void ra_logout(void);

/* Attempts login with the saved token (token is the sole persisted credential).
 * No-op if already logged in or a login is in progress. */
void ra_try_auto_login(void);

/* Login initiated by the inline trophy-screen IME dialog. password only lives
 * in memory until the login callback; only the token is persisted. */
void ra_begin_login(const char *username, const char *password);

/* v26: login from ux0:data/PSPEMUCFW/ra_login.txt, no IME. Returns 1 when a
 * credentials file was found and a login was started, 0 when there is no
 * usable file. No-op if already logged in or a login is in progress. */
int ra_file_login_attempt(void);

/* Game load ---------------------------------------------------------------- */

/* Translates a PSP path (ms0:/ef0:/disc0:/cache0:) to a Vita path.
 * Returns non-zero on success and writes into out (out_size bytes).
 * For cache0:/disc0: entries there is no known Vita mapping; returns 0. */
int ra_translate_psp_path(const char *psp_path, char *out, int out_size);

/* Requests identification + loading of the currently running game from the
 * PSP-side filename stored in the Kermit shared block. Safe to call multiple
 * times; the request is issued lazily by ra_tick() once logged in. */
void ra_request_game_load(void);

int ra_is_game_loaded(void);

/* v32 — hardcore mode accessors.
 *
 * ra_is_hardcore() reads the LIVE rc_client state (not a cached flag), so it
 * can never disagree with rcheevos. It is the gate for the save-state load
 * block (user/main.c, user/states.c), the unlock-fetch mode (ra_gamelist.c)
 * and the mode indicator/labels (menu.c About tab). Returns 0 when the RA
 * client does not exist (no network bring-up yet).
 *
 * The opt-in accessors ONLY touch the RA_HARDCORE_CFG marker file; they never
 * call rc_client_set_hardcore_enabled. The single enable point is ra_init(),
 * which reads the marker once at launch. The Settings-tab toggle (menu.c)
 * calls ra_hardcore_opt_in_set() and tells the user it applies at next
 * launch — no mid-session flip exists, so none needs guarding. */
int ra_is_hardcore(void);
int ra_hardcore_opt_in_get(void);
int ra_hardcore_opt_in_set(int enabled);

/* v15: called from ExitAdrenalineMenu(). The menu is closing, so the game that
 * was identified may exit, be swapped, or keep running -- flag the session for
 * re-identification on the next trophy-screen open instead of trusting the
 * cached identity forever (v14-scroll-stale-debug.md, Bug 2). Login state,
 * badges and the offline snapshot are untouched, and the live rc_client
 * session (hence rc_client_do_frame unlock detection) is kept until a hash job
 * actually proves the running game changed. */
void ra_note_menu_closed(void);

/* Non-zero while a re-identification requested by ra_note_menu_closed() is
 * still outstanding. */
int ra_needs_game_revalidate(void);

/* UI ----------------------------------------------------------------------- */

/* Set when the PSP side fires ADRENALINE_VITA_CMD_OPEN_TROPHIES (written on
 * the Kermit request thread, user/main.c:453). Consumed by menu.c on the
 * RENDER thread: the AdrenalineDraw loop polls it and opens the menu on the
 * Trophies tab, and EnterAdrenalineMenu() clears it.
 *
 * volatile: an aligned 32-bit store is atomic on Cortex-A9 so no tearing is
 * possible, but the qualifier is what stops the compiler from caching the
 * load across iterations of the draw loop. */
extern volatile int ra_trophy_open_requested;

/* Called by menu.c when the trophy tab becomes visible. */
void ra_view_opened(void);

/* Called by menu.c when leaving the trophy tab. */
void ra_view_closed(void);

int ra_view_is_open(void);

/* Drawing/input of the trophy tab content (called from drawMenu/ctrlMenu). */
void ra_draw_trophy_tab(void);
void ra_ctrl_trophy_tab(void);

/* v21: O/back arbitration for menu.c's PAD_CANCEL handler. Returns non-zero
 * when the trophy view CONSUMED the press (only case: the remote game-detail
 * screen pops back to the collection). Returns 0 everywhere else, so O keeps
 * its existing meaning of "close the Adrenaline menu". */
int ra_view_handle_cancel(void);

/* Per-frame view upkeep (polls the login IME dialog, refreshes rows). */
void ra_view_tick(void);

/* Non-zero while the inline login IME dialog is active; the draw loop then
 * composites the dialog instead of skipping the frame. */
int ra_is_ime_active(void);

/* Badge cache -------------------------------------------------------------- */

/*
 * v21: the badge cache's `locked` parameter is now a three-valued IMAGE VARIANT
 * code, not a boolean. It was always an opaque discriminator as far as the
 * fetch/FIFO/LRU machinery is concerned -- every use of it in ra_badges.c and
 * ra_net.c is an equality compare or a pass-through (audited: ra_badges.c lines
 * 115-240, 389-427, 433-467, 498-520; ra_net.c 1149-1164 / 1298 / 1372-1375).
 * Exactly two places ever decoded it, and both are now 3-way:
 *   - ra_badge_path()  : disk-cache filename suffix "" / "_lock" / "_g"
 *   - ra_badge_get()   : the URL it builds (/Badge/ vs /Images/)
 * So game box art rides the identical fetch -> disk -> lodepng -> GXM-fenced
 * upload pipeline, with zero new state and no FIFO change.
 *
 * The values of 0 and 1 are deliberately unchanged so every existing call site
 * (ra_draw_badge's `!row->unlocked`, ra_view_prefetch_pump's literal 0/1,
 * ra_toast.c) keeps its exact meaning.
 */
#define RA_IMG_BADGE_UNLOCKED 0   /* achievement badge, earned artwork  */
#define RA_IMG_BADGE_LOCKED   1   /* achievement badge, grey artwork    */
#define RA_IMG_GAME_ICON      2   /* game box art (RA image_name)       */

/* Returns an opaque texture (vita2d_texture*) for the badge/icon, or NULL while
 * pending/failed. `variant` is one of RA_IMG_* above (the parameter is still
 * named `locked` in the implementation for diff continuity).
 * Cast to vita2d_texture* on the caller side. */
void *ra_badge_get(const char *badge_name, int locked);

/* v18: warm the cache for a badge variant nothing is drawing yet, so an unlock
 * toast finds it resident instead of waiting on the network. Same fetch state
 * machine as ra_badge_get(); skips the decode when the bytes are already in
 * memory or on disk. Returns 1 when a download was issued, 0 when cached. */
int ra_badge_prefetch(const char *badge_name, int locked);

/* Pre-draw GPU window: uploads decoded badges / destroys evicted textures.
 * MUST be called from the render loop BEFORE vita2d_start_drawing() and
 * never between start_drawing/end_drawing. Fences (sceGxmFinish) only when
 * it has work. */
void ra_badges_upload_pending(void);

/* Offline snapshot --------------------------------------------------------- */

/* Minimal achievement record persisted for offline display. */
typedef struct ra_snapshot_entry {
	char badge_name[16];
	char title[96];
	char description[128];
	uint32_t points;
	int unlocked;
	uint64_t unlock_time; /* unix time, 0 if locked */
} ra_snapshot_entry;

/* Save/load the snapshot for the current game hash. */
void ra_snapshot_save(const char *game_hash);
int ra_snapshot_load(const char *game_hash);
const ra_snapshot_entry *ra_snapshot_get_entries(int *count);
void ra_snapshot_clear(void);

/* Misc --------------------------------------------------------------------- */

/* rc_client instance (for tests/debug; may be NULL before init). */
rc_client_t *ra_get_client(void);

/* Read a block of PSP RAM; used both by rc_client and by the snapshot.
 * Signature matches rc_client_read_memory_func_t (trailing client is unused). */
uint32_t ra_read_psp_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client);

/* Current game hash once identification finished (32 hex chars + NUL). */
const char *ra_get_game_hash(void);

/* Status line shown at the bottom of the trophy tab. */
const char *ra_get_status_message(void);

/* v32: set the Trophies-tab status line from outside ra_client.c. Needed by
 * the enforcing save-state gate in user/main.c, which runs on the
 * AdrenalineCompat thread and must tell the user (on the on-screen status
 * surface drawn from ra_get_status_message) why a refused hardcore load did
 * nothing — matching RetroArch's explicit-warning strength. The buffer is a
 * file-static in ra_client.c, so callers go through this setter rather than
 * touching the symbol directly. Cross-thread write: a 160-byte snprintf; on
 * Cortex-A9 aligned word stores do not tear, and the LOADSTATE-refused path
 * is rare (only when a load is blocked in hardcore). NULL is ignored. */
void ra_set_status_message(const char *message);

#endif /* ADRENALINE_RA_H */
