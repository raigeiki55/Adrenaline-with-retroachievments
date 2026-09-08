/*
 * Adrenaline+ RetroAchievements — rc_client integration
 *
 * Single-writer rule: every rc_client_* call happens on the AdrenalineDraw
 * (render) thread, driven by ra_tick(). The net worker thread only performs
 * HTTP and file hashing; it never calls rc_client directly.
 *
 * v6 — game-load worker: the post-login game-identification chain
 * (SceAdrenaline filename read + PSP-path translation + rc_hash file I/O)
 * no longer runs synchronously inside AdrenalineDraw. v5 crashed with
 * C2-12828-1 during login and the last log line was "stage8 ... net layer
 * UP"; the leading hypothesis (investigation-result/v5-login-crash-debug.md)
 * is that first-ever synchronous chain
 *     ra_login_callback -> ra_pending_game_load
 *         -> ra_start_game_load(): ScePspemuConvertAddress + rc_hash
 *            + rc_client_begin_load_game
 * faulted on the 64 KB render stack. Adrenaline's own proven pattern for
 * exactly this class of work (ScePspemuConvertAddress + heavy synchronous
 * processing of PSP memory/files) is the dedicated AdrenalineCompat thread
 * (user/main.c SaveState/LoadState), NOT the draw thread. v6 mirrors that:
 *
 *   render thread            RA_NetWorker (128 KB stack)
 *   -------------            ----------------------------
 *   ra_start_game_load()  -> ra_net_queue_hash_job()
 *                            -> read SceAdrenaline->filename
 *                            -> ra_translate_psp_path()
 *                            -> ra_local_hash() (rc_hash file I/O)
 *                            <- ra_client_hash_done() (via completion FIFO)
 *   rc_client_begin_load_game stays HERE, on the render thread
 *
 * The [RA] log lines added in v6 (see v5-login-crash-debug.md "Recommended
 * minimal fix") make the next hardware run self-diagnosing: the last line
 * written before a crash names the stage that faulted.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h> /* _U/_L/_N/_S/_P/_C/_X/_B class flags for the _ctype_ table below */

#include "ra_internal.h"
#include "rc_compat.h"
#include "../main.h"
#include "../utils.h"

extern int menu_open; /* menu.c */

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

static rc_client_t *ra_client = NULL;
static int ra_initialized = 0;
static int ra_logged_in = 0;
static int ra_login_in_progress = 0;
static int ra_game_loaded = 0;
static int ra_game_loading = 0;
/* v32: cached copy of the hardcore opt-in, written once in ra_init() right
 * after rc_client_create(). Logging/UI convenience only — ra_is_hardcore()
 * deliberately reads the live client instead, so it can never disagree with
 * rcheevos. */
static int ra_hardcore_active = 0;

static char ra_game_hash[64];
static char ra_status_message[160];
static char ra_welcome[160];

static uint8_t *ra_psp_ram = NULL; /* cached PSP RAM mapping (0x88000000 base) */

/* pending requests consumed by ra_tick() on the render thread */
static int ra_pending_auto_login = 0;
static int ra_pending_game_load = 0;

/*
 * v24 — boot-time network bootstrap (autologin-tracking-fix-scope.md, Option A).
 *
 * v23 and earlier deferred the ENTIRE login chain to the first trophy-menu open
 * of the session, because ra_net_ensure_started() is synchronous and slow and
 * running it on the render thread's first frames stalls the boot. The cost of
 * that deferral is that rcheevos does not arm a single achievement trigger
 * until the player opens the menu -- and rcheevos' anti-save-scum guard refuses
 * to fire a trigger that is ALREADY TRUE when tracking first arms. A one-time
 * story flag (the "Poliasha class") reached before that first menu-open is
 * therefore swallowed permanently, with no error anywhere.
 *
 * v24 keeps the bootstrap off the render thread but moves it to BOOT, on a
 * dedicated RA_BootNet worker, mirroring the AdrenalineCompat/RA_NetWorker
 * pattern this codebase already relies on for heavy synchronous work.
 *
 * States:
 *    0  bootstrap not finished (thread still running) -- a login IS coming
 *    1  bootstrap succeeded    -- ra_tick may auto-login immediately
 *   -1  bootstrap failed, or was never attempted (no saved credentials, or the
 *       thread could not be created) -- fall back to the v23 menu-open path,
 *       where paying the synchronous retry cost is acceptable because the PSP
 *       world is already paused.
 *
 * volatile: written by RA_BootNet, polled every frame by the render thread in
 * ra_tick(). An aligned 32-bit access is atomic on Cortex-A9; the qualifier is
 * what stops the compiler caching the load across draw-loop iterations.
 */
static volatile int ra_boot_net_state = 0;
static SceUID ra_boot_thread = -1;

/*
 * v24 — the saved token was specifically REJECTED by the server (as opposed to
 * "the network is down"), which is what a password change produces on the next
 * boot. Set in ra_login_callback()'s failure branch, cleared by a successful
 * login or by the user starting a manual one. Read by the trophy view so the
 * UI can say "sign in again" instead of a generic transport error.
 */
static int ra_stale_token = 0;

/*
 * v24 — Option B decoupling latch (defense-in-depth).
 *
 * Set when a hash job has IDENTIFIED the running game but the boot-time login
 * has not resolved yet. rcheevos itself has no login-state dependency in
 * rc_client_begin_load_game()/rc_client_activate_achievements(), so the slow
 * half of the chain (reading the PSP-side boot filename, translating the path,
 * and rc_hash's file I/O over the ISO) now runs in PARALLEL with the login
 * round trip instead of strictly after it. When login lands, ra_tick() attaches
 * the rc_client session immediately from the hash already in ra_game_hash --
 * no second hash pass, and the tracking window opens as early as it can.
 *
 * Render-thread only (written in ra_client_hash_done()/ra_tick(), both render
 * context); the net worker never touches it.
 */
static int ra_load_deferred_pending = 0;

/* v6: a hash job is currently on the worker. Set when the render thread
 * queues RA_FIFO_HASH_JOB, cleared when ra_client_hash_done() consumes the
 * completion. Render-owned (written/read only in ra_tick()/ra_client_hash_done,
 * both render-thread context); the worker never touches it. */
static int ra_hash_job_in_flight = 0;

/*
 * v15 Bug 2 (v14-scroll-stale-debug.md): "the loaded game may no longer be the
 * running game."
 *
 * Set by ra_note_menu_closed() (ExitAdrenalineMenu), cleared once a hash job
 * has confirmed what is running now. While set, the ra_game_loaded latch stops
 * short-circuiting ra_start_game_load()/ra_client_hash_done(), so the next
 * trophy-screen open re-hashes and, if the hash changed (or the game is gone),
 * unloads the old rc_client game and loads the new one.
 *
 * NOTE (report's "New Hire" warning): ra_game_loaded and ra_game_loading are
 * NOT interchangeable. ra_game_loading means "an rc_client load is in flight";
 * clearing it out of band would let a second load race the first callback, so
 * nothing below ever writes it except the load path itself.
 */
static int ra_revalidate_game = 0;

/*
 * v26 — set when a game-identification hash job returns RA_HASH_REASON_NO_GAME
 * while we are logged in with no game loaded. Drives the low-frequency
 * background re-identification poll in ra_tick() (trophy-menu-delay.md Fix 2).
 * Render-thread only. NOT set for POPS_GAME / BAD_PATH / HASH_FAILED -- those
 * are not "the PSP game has not finished booting yet".
 */
static int ra_identify_retry = 0;

volatile int ra_trophy_open_requested = 0;

/* ------------------------------------------------------------------------- */
/* Memory read                                                                */
/* ------------------------------------------------------------------------- */

/*
 * RetroAchievements addresses for the PSP cover 0x00000000-0x03FFFFFF:
 * 0x00000000-0x01FFFFFF is the base 32 MiB (Kernel + System RAM) and
 * 0x02000000-0x03FFFFFF is the PSP-2000+ Extended RAM (rcheevos 12.4.0,
 * upstream PR #514), enabled per-game via the MEMSIZE flag in PARAM.SFO.
 * The PSP physical address is ra_addr + 0x08000000 for the ENTIRE span.
 * Adrenaline maps the PSP main memory with ScePspemuConvertAddress(
 * 0x88000000, KERMIT_INPUT_MODE, PSP_RAM_SIZE) (same pattern SaveState
 * uses): the returned pointer is the base of one contiguous 64 MiB PSP RAM
 * block at PSP virtual 0x88000000, i.e. physical 0x08000000. Extended RAM
 * is not a separate island -- ApplyMemory() (cef systemctrl) grows the user
 * partition in place inside this window. Therefore the offset into the
 * block equals the RA address itself across the whole span:
 * off = (ra_addr + 0x08000000) - 0x08000000 = ra_addr.
 */
uint32_t ra_read_psp_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
	(void)client;

	if (!ra_psp_ram)
		return 0;

	/* ra address range check (0x00000000-0x03FFFFFF: base + extended RAM) */
	if (address >= 0x04000000)
		return 0;

	if ((uint64_t)address + num_bytes > (uint64_t)PSP_RAM_SIZE) {
		if ((uint64_t)num_bytes <= 0 || address >= PSP_RAM_SIZE)
			return 0;
		num_bytes = PSP_RAM_SIZE - address;
	}

	memcpy(buffer, ra_psp_ram + address, num_bytes);
	return num_bytes;
}

static void ra_cache_psp_ram(void)
{
	if (!ra_psp_ram)
		ra_psp_ram = (uint8_t *)ScePspemuConvertAddress(0x88000000, KERMIT_INPUT_MODE, PSP_RAM_SIZE);
}

/* ------------------------------------------------------------------------- */
/* Time                                                                       */
/* ------------------------------------------------------------------------- */

static rc_clock_t ra_get_time_millisecs(const rc_client_t *client)
{
	(void)client;
	/* microseconds -> milliseconds; monotonic process time */
	return (rc_clock_t)(sceKernelGetProcessTimeWide() / 1000);
}

/*
 * newlib's time.h declares clock_gettime, but SceLibc_stub does not export
 * it. rc_client.c's default time source (compiled because CLOCK_MONOTONIC is
 * defined) references it, so the symbol must resolve at link time even though
 * we install ra_get_time_millisecs at runtime. Provide a monotonic shim.
 */
int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	(void)clk_id;
	uint64_t usec = (uint64_t)sceKernelGetProcessTimeWide();
	tp->tv_sec = (time_t)(usec / 1000000ULL);
	tp->tv_nsec = (long)((usec % 1000000ULL) * 1000ULL);
	return 0;
}

/*
 * newlib's ctype.h implements isdigit()/isspace()/tolower()/toupper() etc. as
 * macros that index into the global class table `extern const char _ctype_[]`
 * (macro form: ((__CTYPE_PTR + sizeof(""[c]))[(int)(c)]) — the table is
 * indexed at offset +1 so that element 0 maps EOF/-1 to 0). The class flags
 * come from the same header: _U 0x01, _L 0x02, _N 0x04, _S 0x08, _P 0x10,
 * _C 0x20, _X 0x40, _B 0x80.
 *
 * adrenaline_user is linked -nostdlib (SceLibc_stub does NOT export _ctype_),
 * so rcheevos sources that use these macros fail at link time with
 * "undefined reference to `_ctype_'". Provide the standard C-locale table
 * here. The bytes are copied verbatim from newlib's own object
 * (libc.a:lib_a-ctype_.o -> .rodata._ctype_, 257 bytes, verified in the pod
 * via arm-vita-eabi-objcopy). The 0x20 (space) row uses the _S|_B form
 * (== 0x88) and the TAB..CR rows use _C|_S (== 0x28), matching newlib byte for
 * byte across all 257 entries.
 *
 * No _tolower_tab_/_toupper_tab_ exist in this newlib — tolower()/toupper()
 * are macros built from this same table (the _MB_EXTENDED_CHARSETS function
 * form is not compiled for this toolchain).
 */
const char _ctype_[257] = {
	0,                                    /* EOF slot (index 0) */
	_C, _C, _C, _C, _C, _C, _C, _C,       /* 0x00-0x07: NUL..BEL */
	_C, _C | _S, _C | _S, _C | _S, _C | _S, _C | _S, _C, _C, /* BS..SI (TAB/LF/VT/FF/CR: _C|_S; newlib's isblank() special-cases TAB) */
	_C, _C, _C, _C, _C, _C, _C, _C,       /* 0x10-0x17 */
	_C, _C, _C, _C, _C, _C, _C, _C,       /* 0x18-0x1F */
	_S | _B, _P, _P, _P, _P, _P, _P, _P,  /* ' ' ! " # $ % & ' */
	_P, _P, _P, _P, _P, _P, _P, _P,       /* ( ) * + , - . / */
	_N, _N, _N, _N, _N, _N, _N, _N,       /* 0-7 (_N only; isxdigit checks _X|_N) */
	_N, _N, _P, _P, _P, _P, _P, _P,       /* 8 9 : ; < = > ? */
	_P, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U, /* @ A-G */
	_U, _U, _U, _U, _U, _U, _U, _U,       /* H-O */
	_U, _U, _U, _U, _U, _U, _U, _U,       /* P-W */
	_U, _U, _U, _P, _P, _P, _P, _P,       /* X-Z [ \ ] ^ _ */
	_P, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L, /* ` a-g */
	_L, _L, _L, _L, _L, _L, _L, _L,       /* h-o */
	_L, _L, _L, _L, _L, _L, _L, _L,       /* p-w */
	_L, _L, _L, _P, _P, _P, _P, _C,       /* x-y z { | } ~ DEL */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x80-0x8F */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x90-0x9F */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xA0-0xAF */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xB0-0xBF */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xC0-0xCF */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xD0-0xDF */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xE0-0xEF */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xF0-0xFF */
};

/* ------------------------------------------------------------------------- */
/* Hash callbacks (custom sceIo filereader; default cdreader rides on top)    */
/* ------------------------------------------------------------------------- */

static void *ra_filereader_open(const char *path)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	/* store fd+1 so handle 0 is never ambiguous */
	return (void *)(uintptr_t)(fd + 1);
}

static void ra_filereader_seek(void *handle, int64_t offset, int origin)
{
	SceUID fd = (SceUID)(uintptr_t)handle - 1;
	int whence = SCE_SEEK_SET;
	if (origin == SEEK_CUR)
		whence = SCE_SEEK_CUR;
	else if (origin == SEEK_END)
		whence = SCE_SEEK_END;
	sceIoLseek(fd, offset, whence);
}

static int64_t ra_filereader_tell(void *handle)
{
	SceUID fd = (SceUID)(uintptr_t)handle - 1;
	return sceIoLseek(fd, 0, SCE_SEEK_CUR);
}

static size_t ra_filereader_read(void *handle, void *buffer, size_t requested)
{
	SceUID fd = (SceUID)(uintptr_t)handle - 1;
	int read = sceIoRead(fd, buffer, requested);
	return (read > 0) ? (size_t)read : 0;
}

static void ra_filereader_close(void *handle)
{
	SceUID fd = (SceUID)(uintptr_t)handle - 1;
	sceIoClose(fd);
}

/* Client-level hash callbacks (ra_install_hash_callbacks). rc_client only
 * invokes these from the thread that calls rc_client_begin_load_game, which
 * here is always the render thread, so touching ra_status_message is safe. */
static void ra_hash_verbose(const char *message, const rc_hash_iterator_t *iterator)
{
	(void)iterator;
	snprintf(ra_status_message, sizeof(ra_status_message), "Hashing: %s", message);
}

static void ra_hash_error(const char *message, const rc_hash_iterator_t *iterator)
{
	(void)iterator;
	snprintf(ra_status_message, sizeof(ra_status_message), "Hash error: %s", message);
}

/* v6 worker-safe variants: ra_local_hash now runs on the RA_NetWorker
 * thread, where writing the render thread's status line would be a data
 * race. Log-only; the render thread sets the user-visible message when the
 * completion arrives (ra_client_hash_done). */
static void ra_hash_worker_verbose(const char *message, const rc_hash_iterator_t *iterator)
{
	(void)iterator;
	RA_LOG("[RA] hash: %s\n", message);
}

static void ra_hash_worker_error(const char *message, const rc_hash_iterator_t *iterator)
{
	(void)iterator;
	RA_LOG("[RA] hash error: %s\n", message);
}

void ra_install_hash_callbacks(rc_client_t *client)
{
	rc_hash_callbacks_t callbacks;
	memset(&callbacks, 0, sizeof(callbacks));

	callbacks.verbose_message = ra_hash_verbose;
	callbacks.error_message = ra_hash_error;

	callbacks.filereader.open = ra_filereader_open;
	callbacks.filereader.seek = ra_filereader_seek;
	callbacks.filereader.tell = ra_filereader_tell;
	callbacks.filereader.read = ra_filereader_read;
	callbacks.filereader.close = ra_filereader_close;

	/* IMPORTANT: rc_client_set_hash_callbacks memcpys the whole struct, so
	 * the default cdreader must be filled in alongside the filereader or the
	 * disc code path loses its read_sector/close_track handlers. The default
	 * cdreader opens tracks through iterator->callbacks.filereader, so our
	 * sceIo reader is used for ISO hashing as well. */
	rc_hash_get_default_cdreader(&callbacks.cdreader);

	rc_client_set_hash_callbacks(client, &callbacks);
}

/* ------------------------------------------------------------------------- */
/* Credentials                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Format: one line "username=...\n" + one line "token=...\n".
 * The TOKEN is the sole persisted credential; the password never touches disk.
 */
int ra_save_credentials(const char *username, const char *token)
{
	char buf[512];
	int len = snprintf(buf, sizeof(buf), "username=%s\ntoken=%s\n", username, token);
	if (len <= 0 || len >= (int)sizeof(buf))
		return -1;
	return (WriteFile((char *)RA_LOGIN_CFG, buf, len) == len) ? 0 : -1;
}

static int ra_cfg_get(const char *buf, int len, const char *key, char *out, int out_size)
{
	const char *p = buf;
	int key_len = strlen(key);

	while (p < buf + len) {
		const char *eol = memchr(p, '\n', (buf + len) - p);
		int line_len = eol ? (int)(eol - p) : (int)((buf + len) - p);

		if (line_len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			int val_len = line_len - key_len - 1;
			if (val_len >= out_size)
				val_len = out_size - 1;
			memcpy(out, p + key_len + 1, val_len);
			out[val_len] = 0;
			/* strip trailing CR */
			if (val_len > 0 && out[val_len - 1] == '\r')
				out[val_len - 1] = 0;
			return 0;
		}

		p = eol ? eol + 1 : buf + len;
	}

	return -1;
}

int ra_load_credentials(char *username, int username_size, char *token, int token_size)
{
	SceUID fd = sceIoOpen(RA_LOGIN_CFG, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;

	char buf[512];
	int len = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (len <= 0)
		return -1;
	buf[len] = 0;

	if (ra_cfg_get(buf, len, "username", username, username_size) < 0)
		return -1;
	if (ra_cfg_get(buf, len, "token", token, token_size) < 0)
		return -1;

	return (username[0] && token[0]) ? 0 : -1;
}

int ra_has_saved_credentials(void)
{
	char username[64], token[256];
	return ra_load_credentials(username, sizeof(username), token, sizeof(token)) == 0;
}

/*
 * v24 — discard the persisted token.
 *
 * Called from exactly one place: ra_login_callback()'s rejected-token branch.
 * Deliberately NOT called for transport/TLS failures -- a token that is merely
 * unreachable is still good, and deleting it there would sign the user out
 * every time the Wi-Fi drops.
 *
 * The file holds a username and an API token, never the password (see
 * ra_save_credentials above), so removal loses nothing the user cannot restore
 * by signing in again -- which is precisely what the caller then prompts for.
 */
int ra_clear_credentials(void)
{
	int r = sceIoRemove(RA_LOGIN_CFG);
	/* No credential material is logged here, only the syscall status: the
	 * log is a plain file on ux0 that the user is routinely asked to share
	 * in bug reports. */
	RA_LOG("[RA] v24 cleared saved credentials (sceIoRemove=0x%08X)\n", r);
	return (r < 0) ? -1 : 0;
}

/*
 * v25 — sign out on demand (the Triangle hotkey in ra_ctrl_trophy_tab()).
 *
 * Before v25 there was no way back out of a session: ra_clear_credentials()
 * above deletes the token FILE but leaves ra_logged_in, the rc_client user
 * struct and the loaded game entirely untouched, so calling it alone would
 * produce a still-"logged in" UI backed by a credential that no longer exists
 * on disk -- the two halves of the state would disagree until the next reboot.
 *
 * rc_client_logout() is the library half (rc_client.h:182, implemented at
 * rcheevos/src/rc_client.c:844). It is SYNCHRONOUS -- void return, no callback
 * parameter -- and does three things: sets state.user back to
 * RC_CLIENT_USER_STATE_NONE, memsets client->user, and calls
 * rc_client_unload_game() (rc_client.c:870-877). Because it unloads the game,
 * ra_game_loaded MUST be cleared here too, or ra_tick()'s
 * "else if (ra_game_loaded) rc_client_do_frame(ra_client)" would keep driving
 * frames against a game the library no longer has.
 *
 * Everything else zeroed below is this module's own mirror of the session:
 *   - ra_login_in_progress / ra_pending_auto_login: a logout is an explicit
 *     "stop", so cancel anything queued rather than letting a boot auto-login
 *     silently sign the user straight back in on the next tick (ra_tick():1286).
 *   - ra_stale_token: the "your saved token was rejected" call to action is
 *     meaningless once the user has deliberately signed out.
 *   - ra_pending_game_load / ra_revalidate_game: both describe work for a
 *     session that no longer exists.
 *   - ra_welcome: ra_get_welcome_message() returns NULL once empty, which is
 *     what ra_draw_header() keys off to switch back to the signed-out prompt.
 *
 * Unlike the rejected-token path in ra_login_callback(), this one deletes the
 * saved credentials by intent: the user asked to be signed out, and leaving the
 * token on disk would sign them back in at the next boot.
 */
void ra_logout(void)
{
	if (ra_client)
		rc_client_logout(ra_client);

	ra_logged_in = 0;
	ra_login_in_progress = 0;
	ra_stale_token = 0;
	ra_pending_auto_login = 0;
	ra_pending_game_load = 0;
	ra_load_deferred_pending = 0;
	ra_game_loaded = 0;
	ra_revalidate_game = 0;
	ra_identify_retry = 0;   /* v26: no session left to re-identify for */
	ra_welcome[0] = 0;

	ra_clear_credentials();

	snprintf(ra_status_message, sizeof(ra_status_message),
		"Signed out of RetroAchievements.");
	RA_LOG("[RA] v25 logout: session cleared, saved credentials removed\n");
}

/* ------------------------------------------------------------------------- */
/* Login                                                                      */
/* ------------------------------------------------------------------------- */

/* v23: (re)build the welcome line from the current user info.
 *
 * The points shown are user->score_softcore, NOT user->score: rc_client keeps
 * two separate cumulative totals and credits an unlock to score only while
 * hardcore is on (rcheevos rc_client.c, rc_client_award_achievement). This
 * client disables hardcore permanently in ra_init() below, so user->score can
 * never move here and reads as the account's unrelated hardcore lifetime total
 * (0 for most players) -- every point this app earns lands in score_softcore.
 *
 * Called at login and again on every unlock, so the total grows live instead
 * of freezing at its login-time value. Render-thread only: ra_login_callback
 * and ra_event_handler are both driven from ra_tick(), the same thread that
 * reads ra_welcome via ra_get_welcome_message(). */
static void ra_update_welcome(rc_client_t *client)
{
	const rc_client_user_t *user = client ? rc_client_get_user_info(client) : NULL;
	const char *name;

	if (!user)
		return;

	name = user->display_name ? user->display_name : user->username;
	if (!name)
		return;

	snprintf(ra_welcome, sizeof(ra_welcome), "Welcome, %s! (%u pts)",
		name, user->score_softcore);
}

static void ra_login_callback(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	int from_file = (userdata != NULL);   /* v26: non-NULL == ra_login.txt */

	/* v6 instrumentation: the login outcome. If the log ends at
	 * "login result=0" the crash is in the post-login chain that this
	 * callback triggers (game load), not in the login POST itself. */
	RA_LOG("[RA] login result=%d\n", result);

	ra_login_in_progress = 0;

	if (result == RC_OK) {
		const rc_client_user_t *user = rc_client_get_user_info(client);
		if (!user) {
			snprintf(ra_status_message, sizeof(ra_status_message), "Login failed: no user info");
			return;
		}

		ra_logged_in = 1;
		/* v24: whatever was wrong with the old token, this one works. */
		ra_stale_token = 0;

		/* persist username + token (token is the sole credential) */
		if (user->token && user->username)
			ra_save_credentials(user->username, user->token);

		/* v26: file login succeeded -- drop the password file; the token is
		 * already saved above to ra_login.cfg. */
		if (from_file) {
			sceIoRemove(RA_LOGIN_TXT);
			RA_LOG("[RA] v26 file login ok - removed %s\n", RA_LOGIN_TXT);
		}

		/* v23: softcore score, not user->score (see ra_update_welcome) */
		ra_update_welcome(client);
		snprintf(ra_status_message, sizeof(ra_status_message), "%s", ra_welcome);

		/* after login, (re)load the current game */
		ra_pending_game_load = 1;
	} else if (result == RC_INVALID_CREDENTIALS || result == RC_ACCESS_DENIED ||
			result == RC_EXPIRED_TOKEN) {
		/*
		 * v24 (autologin-tracking-fix-scope.md WARNING-3) — the server did not
		 * fail to answer, it answered "no".
		 *
		 * rcheevos already computes this distinction: rc_api_common.c's
		 * rc_json_convert_error_code() maps the server's "invalid_credentials"
		 * / "access_denied" / "expired_token" strings onto these three codes,
		 * and hands them to this callback as `result`. v23 flattened all of
		 * them into the same generic transport-error text below and left the
		 * dead token on disk, so a password change produced an unexplained
		 * "Login failed: ..." on every boot, forever, with no route to fixing
		 * it.
		 *
		 * Clearing the token here is what makes the next boot behave sensibly:
		 * ra_has_saved_credentials() then returns 0, the boot worker stops
		 * retrying a credential the server has already rejected, and the
		 * trophy screen falls back to its normal signed-out login row.
		 */
		RA_LOG("[RA] v24 saved token REJECTED (result=%d) - clearing and prompting re-login\n",
			result);

		ra_logged_in = 0;
		ra_pending_auto_login = 0; /* do not retry a credential known to be dead */
		ra_clear_credentials();

		if (from_file) {
			/* v26: keep ra_login.txt so the user can fix it, and do NOT set
			 * ra_stale_token -- the stale-token UI routes to the Vita IME,
			 * which is exactly the path this file exists to avoid. */
			ra_stale_token = 0;
			RA_LOG("[RA] v26 file login REJECTED (result=%d) - fix " RA_LOGIN_TXT " and retry\n",
				result);
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Login rejected - check username/password in " RA_LOGIN_TXT);
		} else {
			ra_stale_token = 1;
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Saved login no longer valid (password changed?) - select Login to sign in again.");
		}
	} else {
		ra_logged_in = 0;

		/* A TLS failure surfaces through rc_client as a generic transport
		 * error. Verification is intentionally left enabled (ra_net.c), so
		 * report the certificate reason when there is one -- otherwise the
		 * only visible symptom is an unexplained login failure. */
		const char *ssl = ra_net_ssl_error_text();
		if (!ra_net_ssl_is_ready()) {
			/* v7: the request never left the console -- the worker refused to
			 * hand an https:// URL to a TLS stack that was not brought up
			 * (ra_net.c "SSL not ready, aborting HTTPS"). Say so, and name the
			 * action that actually retries it: closing and reopening the menu
			 * re-runs the whole sysmodule ladder including the stage2d retry. */
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Login failed: TLS unavailable this session. Close and reopen the trophy menu to retry.");
		} else if (ssl) {
			snprintf(ra_status_message, sizeof(ra_status_message), "Login failed: %s", ssl);
		} else {
			snprintf(ra_status_message, sizeof(ra_status_message), "Login failed: %s",
				error_message ? error_message : "unknown error");
		}
	}
}

/*
 * v26 — the real login entry point. `userdata` is the source sentinel handed
 * straight to rc_client and read back by ra_login_callback(): NULL for an
 * IME/manual login, (void *)1 for a login driven by ra_login.txt. No other
 * rc_client_begin_login_with_password() caller exists; a new one must keep
 * using NULL.
 */
static void ra_begin_login_ex(const char *username, const char *password, void *userdata)
{
	if (!ra_initialized || !ra_client || ra_logged_in || ra_login_in_progress)
		return;

	/* v24: the user is supplying fresh credentials, so the "your saved login
	 * is dead" state is over regardless of how this attempt turns out. */
	ra_stale_token = 0;

	/* Fix 1a: these are two different failures and must not share a message.
	 * A bootstrap failure names the stage that broke (1..8, see ra_net.c and
	 * the [RA] lines in ux0:data/adrenaline_user_log.txt); only the
	 * connectivity check may report "no network", which is what the plan
	 * (TROPHY-IMPLEMENTATION-PLAN.md:112) actually specified. */
	int nr = ra_net_ensure_started();
	if (nr != 0) {
		snprintf(ra_status_message, sizeof(ra_status_message),
			"Network init failed (stage %d) - see ux0:data/adrenaline_user_log.txt", -nr);
		return;
	}

	if (!ra_net_is_online()) {
		snprintf(ra_status_message, sizeof(ra_status_message), "No network connection. Check Wi-Fi.");
		return;
	}

	ra_login_in_progress = 1;
	snprintf(ra_status_message, sizeof(ra_status_message), "Logging in as %s...", username);
	rc_client_begin_login_with_password(ra_client, username, password, ra_login_callback, userdata);
}

void ra_begin_login(const char *username, const char *password)
{
	ra_begin_login_ex(username, password, NULL);   /* IME / manual: userdata NULL */
}

void ra_try_auto_login(void)
{
	if (!ra_initialized || !ra_client || ra_logged_in || ra_login_in_progress)
		return;
	ra_pending_auto_login = 1;
}

static void ra_do_auto_login(void)
{
	char username[64], token[256];

	if (ra_load_credentials(username, sizeof(username), token, sizeof(token)) < 0)
		return;

	if (ra_net_ensure_started() != 0 || !ra_net_is_online()) {
		snprintf(ra_status_message, sizeof(ra_status_message), "Offline. Trophies shown from cache if available.");
		return;
	}

	ra_login_in_progress = 1;
	snprintf(ra_status_message, sizeof(ra_status_message), "Logging in as %s...", username);
	rc_client_begin_login_with_token(ra_client, username, token, ra_login_callback, NULL);
}

/* v26 — IME-free file-based login (bakkerrrs-ra-analysis.md §3.1 item 1).
 * A user who cannot use the Vita IME (v25-login-crash.md) drops a text file
 * on the card before launching Adrenaline:
 *   ux0:data/PSPEMUCFW/ra_login.txt:
 *       username=YOUR_RA_USERNAME
 *       password=YOUR_RA_PASSWORD
 * The password travels only over the TLS login POST and the file is deleted on
 * success; only the API token persists (ra_login.cfg). Nothing here logs
 * credential material. Render-thread only, like every other login entry point.
 */
static int ra_file_login_present(void)
{
	SceIoStat stat;
	return sceIoGetstat(RA_LOGIN_TXT, &stat) >= 0;
}

int ra_file_login_attempt(void)
{
	char buf[512];
	char username[64], password[64];
	int len;
	SceUID fd;

	if (ra_logged_in || ra_login_in_progress)
		return 0;

	fd = sceIoOpen(RA_LOGIN_TXT, SCE_O_RDONLY, 0);
	if (fd < 0)
		return 0;
	len = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (len <= 0)
		return 0;
	buf[len] = 0;

	if (ra_cfg_get(buf, len, "username", username, sizeof(username)) < 0 ||
	    ra_cfg_get(buf, len, "password", password, sizeof(password)) < 0 ||
	    !username[0] || !password[0]) {
		/* v26 hardening: buf held the plaintext file, scrub before returning */
		memset(buf, 0, sizeof(buf));
		memset(password, 0, sizeof(password));
		return 0;
	}

	RA_LOG("[RA] v26 file login: credentials found in %s\n", RA_LOGIN_TXT);
	ra_begin_login_ex(username, password, (void *)1);  /* userdata = "from file" */
	memset(password, 0, sizeof(password));
	memset(buf, 0, sizeof(buf));
	return 1;
}

/* ------------------------------------------------------------------------- */
/* v24 — boot-time network bootstrap worker                                   */
/* ------------------------------------------------------------------------- */

/*
 * RA_BootNet. Runs ONCE, at boot, and then exits.
 *
 * Its entire job is the one-time bootstrap that v23 paid for synchronously on
 * the render thread at first menu-open: sysmodule NET, sceNetInit,
 * sceNetCtlInit, the OpenSSL locking callbacks, curl_global_init, and the
 * RA_NetWorker thread that carries every later request.
 *
 * SINGLE-WRITER RULE IS PRESERVED. This thread never calls rc_client. It brings
 * the transport up and publishes ra_boot_net_state; the actual
 * rc_client_begin_login_with_token() still happens on the render thread, from
 * ra_tick(), exactly as before. All this thread changes is WHEN the render
 * thread is allowed to make that call -- from boot instead of from the first
 * menu-open -- and it does so without the render thread ever blocking on the
 * bootstrap.
 *
 * Boot safety (v4-black-screen-opus.md): the v4 black screen was root-caused to
 * a 64 MiB ScePaf heap reservation issued as the first instruction of
 * module_start(), NOT to the network module loads themselves -- AdrenalineRA
 * loads NET/HTTPS/SSL at module_start on this same hardware and boots. ScePaf
 * was removed from this path entirely in v9 and nothing here reintroduces it or
 * any comparable early heap request. This also still runs from
 * InitAdrenaline(), never from module_start().
 */
static int ra_boot_net_main(SceSize args, void *argp)
{
	int r;

	(void)args;
	(void)argp;

	RA_LOG("[RA] v24 boot-net: bootstrap starting off the render thread\n");

	r = ra_net_ensure_started();

	/* Published LAST, after every side effect the render thread will act on
	 * (ra_net_started, the worker thread, curl's globals) has landed. */
	ra_boot_net_state = (r == 0) ? 1 : -1;

	RA_LOG("[RA] v24 boot-net: ensure_started=%d -> state=%d\n", r, ra_boot_net_state);

	return sceKernelExitDeleteThread(0);
}

void ra_start_boot_net(void)
{
	if (!ra_initialized || ra_boot_thread >= 0)
		return;

	/*
	 * No saved token means there is nothing to auto-log-in WITH, so bringing
	 * the network up at boot would buy nothing and would make every user who
	 * has never signed in pay a sysmodule load + curl init they do not use.
	 * Those users keep exactly the v23 behaviour: the bootstrap happens
	 * lazily, on the first trophy-menu open, where the PSP world is paused.
	 *
	 * -1 (rather than 0) is the correct state to publish here: it tells
	 * ra_tick() "no boot login is coming, use the menu-open path", instead of
	 * leaving it waiting forever for a thread that will never report.
	 */
	/*
	 * v26: also boot the network when an IME-free ra_login.txt exists -- a user
	 * who cannot use the Vita IME still gets boot-time login + tracking from the
	 * file. The actual password POST still happens on the render thread from
	 * ra_tick(), exactly like the token auto-login path.
	 */
	if (!ra_has_saved_credentials() && !ra_file_login_present()) {
		ra_boot_net_state = -1;
		RA_LOG("[RA] v24 boot-net: no saved credentials, staying lazy (v23 path)\n");
		return;
	}

	/*
	 * Priority 0xB0 is numerically LOWER priority than AdrenalineDraw (0xA0)
	 * and RA_NetWorker (0x80) -- deliberately. This work is not on any
	 * critical path; if the draw thread wants the CPU during boot it gets it,
	 * which is what keeps "async" from quietly becoming "stalls the boot".
	 *
	 * 128 KiB of stack for the same reason AdrenalineDraw was widened in v6:
	 * OpenSSL and curl initialisation is the deepest call chain this thread
	 * will ever run, and a stack overflow here would present as a boot-time
	 * crash -- the exact regression class this change must not introduce.
	 */
	ra_boot_thread = sceKernelCreateThread("RA_BootNet", ra_boot_net_main, 0xB0, 0x20000, 0, 0, NULL);
	if (ra_boot_thread < 0) {
		RA_LOG("[RA] v24 boot-net: sceKernelCreateThread FAILED: 0x%08X (falling back to menu-open path)\n",
			ra_boot_thread);
		ra_boot_thread = -1;
		ra_boot_net_state = -1;
		return;
	}

	sceKernelStartThread(ra_boot_thread, 0, NULL);
	RA_LOG("[RA] v24 boot-net: RA_BootNet started (128 KiB stack, prio 0xB0)\n");
}

/* ------------------------------------------------------------------------- */
/* Game identification + load                                                 */
/* ------------------------------------------------------------------------- */

int ra_translate_psp_path(const char *psp_path, char *out, int out_size)
{
	if (!psp_path || !psp_path[0])
		return 0;

	/* Must precede the plain "ms0:/" branch: vshctrl rewrites an ef0: ISO path to
	 * the magic "ms0:/__ef0__/..." form because ef0: is not mounted yet when the
	 * PSP reboots to launch the game (cef/core/vshctrl/src/patch_vsh.c:82-93).
	 * Mirrors buildPspemuMsfsPath() in user/msfs.c:52-55. */
	if (strncmp(psp_path, "ms0:/__ef0__", 12) == 0) {
		char *ef0 = getPspemuEfLocation();
		const char *rest = psp_path + 12;
		if (!ef0)
			return 0;
		if (*rest == '/')	/* keep the single separator supplied below */
			rest++;
		snprintf(out, out_size, "%s/%s", ef0, rest);
		return 1;
	}

	if (strncmp(psp_path, "ms0:/", 5) == 0) {
		snprintf(out, out_size, "%s/%s", getPspemuMemoryStickLocation(), psp_path + 5);
		return 1;
	}

	if (strncmp(psp_path, "ef0:/", 5) == 0) {
		snprintf(out, out_size, "%s/%s", getPspemuEfLocation(), psp_path + 5);
		return 1;
	}

	/* disc0:/cache0:/host0: have no Vita-side file mapping in this emulator */
	return 0;
}

/* Local (offline) hash using the iterator directly, with our filereader.
 *
 * v6: runs on the RA_NetWorker thread (called from ra_hash_worker_job), so
 * the iterator callbacks must be the worker-safe, log-only variants — the
 * render thread's ra_status_message must not be touched from the worker.
 *
 * hash needs 33 bytes (rc_hash_generate writes 32 hex chars + NUL); the
 * worker passes its 40-byte completion buffer. */
static int ra_local_hash(const char *vita_path, char hash[40])
{
	rc_hash_iterator_t iterator;
	memset(&iterator, 0, sizeof(iterator));

	rc_hash_initialize_iterator(&iterator, vita_path, NULL, 0);

	iterator.callbacks.verbose_message = ra_hash_worker_verbose;
	iterator.callbacks.error_message = ra_hash_worker_error;
	iterator.callbacks.filereader.open = ra_filereader_open;
	iterator.callbacks.filereader.seek = ra_filereader_seek;
	iterator.callbacks.filereader.tell = ra_filereader_tell;
	iterator.callbacks.filereader.read = ra_filereader_read;
	iterator.callbacks.filereader.close = ra_filereader_close;
	rc_hash_get_default_cdreader(&iterator.callbacks.cdreader);

	int ok = rc_hash_generate(hash, RA_CONSOLE_PSP, &iterator);
	rc_hash_destroy_iterator(&iterator);
	return ok;
}

static void ra_load_game_callback(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	(void)userdata;

	ra_game_loading = 0;

	if (result == RC_OK) {
		ra_game_loaded = 1;
		/* v26: a ra_request_game_load() that raced an already-queued background
		 * hash must not leave a stale pending flag behind. */
		ra_pending_game_load = 0;
		const rc_client_game_t *game = rc_client_get_game_info(client);
		const char *title = (game && game->title) ? game->title : ra_game_hash;
		/* v32: mode indicator at game start (E2 of the RA compliance spec).
		 * The toast is the start-of-game placard (ra_toast_draw runs over the
		 * gameplay frame, menu.c); the status suffix shows in the Trophies-tab
		 * header for the rest of the session. The label is honest: our UA is
		 * unique but not yet on RA's server-side allowlist, so hardcore
		 * unlocks are expected to be demoted to softcore until RAdmin
		 * validates it. Drop "(pending RA validation)" only after that. */
		if (ra_is_hardcore()) {
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Loaded: %s  [Hardcore (pending RA validation)]", title);
			ra_toast_push("Hardcore (pending RA validation)", 0, NULL);
		} else {
			snprintf(ra_status_message, sizeof(ra_status_message), "Loaded: %s", title);
			ra_toast_push("Casual mode", 0, NULL);
		}

		/* build the UI row list + persist an offline snapshot */
		ra_view_rebuild_list();
		ra_snapshot_save(ra_game_hash);
	} else {
		ra_game_loaded = 0;
		snprintf(ra_status_message, sizeof(ra_status_message), "Game load failed: %s",
			error_message ? error_message : "unknown error");
	}
}

void ra_request_game_load(void)
{
	ra_pending_game_load = 1;
}

/*
 * v15 Bug 2 fix — called from ExitAdrenalineMenu() (user/menu.c).
 *
 * The menu is closing; from here we cannot know whether the PSP-side game
 * keeps running, exits to the XMB, or is swapped for another one. Before v15
 * ra_game_loaded was a one-way latch (set on load success, cleared only by
 * ra_shutdown()), so the next trophy-screen open redisplayed the OLD game's
 * achievements forever and ra_start_game_load() refused to queue a new
 * identification.
 *
 * We deliberately do NOT clear ra_game_loaded here, which is where this
 * departs from the report's sketch: ra_tick() gates rc_client_do_frame() on
 * ra_game_loaded (see below in this file), and that call is what detects
 * unlocks while the PSP world runs with the menu closed. Zeroing the flag on
 * every menu close would silently kill in-game unlock detection. Instead we
 * mark the session for re-identification; the hash job on the next open
 * decides whether to keep, swap, or drop the loaded game.
 *
 * Login state, credentials, the badge cache and the offline snapshot are all
 * untouched.
 */
void ra_note_menu_closed(void)
{
	if (!ra_initialized)
		return;

	ra_revalidate_game = 1;
}

int ra_needs_game_revalidate(void)
{
	return ra_revalidate_game;
}

/*
 * v6 — WORKER side of the game-identification chain.
 *
 * Runs on the RA_NetWorker thread (invoked from ra_net_worker_main for a
 * RA_FIFO_HASH_JOB work entry). It performs exactly the heavy synchronous
 * work that v5 did inside AdrenalineDraw's 64 KB stack:
 *
 *   ScePspemuConvertAddress(ADRENALINE_ADDRESS)  -> PSP-side boot filename
 *   ra_translate_psp_path()                      -> Vita-side file path
 *   ra_local_hash()                              -> rc_hash file I/O
 *
 * This mirrors Adrenaline's own proven pattern for this class of work:
 * SaveState/LoadState run ScePspemuConvertAddress + heavy synchronous
 * processing on the dedicated AdrenalineCompat thread, never on the draw
 * thread (user/main.c). See investigation-result/login-crash-web-research.md
 * section 5.
 *
 * Thread rules honored here:
 *   - NEVER touches rc_client (single-writer rule: rc_client stays on the
 *     render thread; the completion is consumed by ra_client_hash_done()).
 *   - NEVER writes ra_status_message (render-owned); the render thread sets
 *     the user-visible status when the completion arrives. The worker-safe
 *     iterator callbacks above log instead.
 */
void ra_hash_worker_job(char hash_out[40], int *ok_out, int *reason_out, char *psp_path_out, int psp_path_size)
{
	char vita_path[256];
	SceAdrenaline *adrenaline;
	const char *psp_path;
	int ok;

	*ok_out = 0;
	*reason_out = RA_HASH_REASON_NO_GAME;
	hash_out[0] = 0;
	if (psp_path_out && psp_path_size > 0)
		psp_path_out[0] = 0;

	adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
	if (!adrenaline) {
		RA_LOG("[RA] hash job: no game running\n");
		return;
	}

	/* v19 - PSP-only display. A POPS/PS1 title is not a PSP game. Hashing it
	 * would run rc_hash's PSP parser (ra_local_hash hardcodes RA_CONSOLE_PSP)
	 * over a PSISOIMG/PSAR-wrapped PBP that rcheevos has no cdreader decoder
	 * for - a guaranteed failure. Bail before any path translation or file I/O
	 * so the render side can show a purpose-built message instead of a generic
	 * "Could not hash ...". */
	if (adrenaline->pops_mode) {
		RA_LOG("[RA] hash job: POPS game running - PSP trophies unavailable\n");
		*reason_out = RA_HASH_REASON_POPS_GAME;
		return;
	}

	/* v10: for a UMD/ISO launch the PSP kernel reports filename as
	 * "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN" - a virtual device with no Vita-side file
	 * mapping. pentazemin now publishes the real backing image path in iso_path
	 * ("ms0:/ISO/<name>.iso"), which ra_translate_psp_path() can resolve. Empty for
	 * PBP/homebrew, where filename is already an ms0:/ path - so prefer iso_path
	 * when present and fall back to filename otherwise.
	 *
	 * Logged unconditionally: if the PSP side ever fails to publish iso_path, the
	 * log line below is the one-line diagnosis instead of a silent regression. */
	RA_LOG("[RA] hash job: iso_path='%s' filename='%s'\n",
		adrenaline->iso_path, adrenaline->filename);

	psp_path = adrenaline->iso_path[0] ? adrenaline->iso_path : adrenaline->filename;

	if (!psp_path[0]) {
		RA_LOG("[RA] hash job: no game running\n");
		return;
	}

	if (psp_path_out && psp_path_size > 1) {
		strncpy(psp_path_out, psp_path, psp_path_size - 1);
		psp_path_out[psp_path_size - 1] = 0;
	}

	if (!ra_translate_psp_path(psp_path, vita_path, sizeof(vita_path))) {
		RA_LOG("[RA] hash job: path %s not accessible\n", psp_path);
		*reason_out = RA_HASH_REASON_BAD_PATH;
		return;
	}

	/* v6 instrumentation: hash begin/end (the sketch in
	 * v5-login-crash-debug.md). A log that ends at "hashing" pins the crash
	 * inside rc_hash/file I/O on the worker thread. */
	RA_LOG("[RA] hashing %s\n", vita_path);

	ok = ra_local_hash(vita_path, hash_out);

	RA_LOG("[RA] hash done ok=%d\n", ok);

	if (!ok) {
		*reason_out = RA_HASH_REASON_HASH_FAILED;
		return;
	}

	*ok_out = 1;
	*reason_out = RA_HASH_REASON_NONE;
}

/*
 * v6 — RENDER side (producer). Queues the identification job for the net
 * worker; the heavy chain itself is ra_hash_worker_job() above. If the job
 * cannot be queued (worker not up yet, or the work FIFO is momentarily full)
 * the request stays pending and ra_tick retries next frame — no synchronous
 * fallback on the render stack, that is precisely what v6 removes.
 */
static void ra_start_game_load(void)
{
	/* busy states leave ra_pending_game_load set so ra_tick retries once the
	 * condition clears; only a successfully queued job consumes the flag */
	if (ra_game_loading || ra_hash_job_in_flight)
		return;

	/* v15 Bug 2: a loaded game no longer blocks identification forever -- it
	 * only blocks it while we still trust that this IS the running game. */
	if (ra_game_loaded && !ra_revalidate_game)
		return;

	ra_pending_game_load = 0;

	if (ra_net_queue_hash_job()) {
		ra_hash_job_in_flight = 1;
		/* v6 instrumentation (the 8th [RA] line): the render->worker
		 * handoff. With this, every transition in the login/game-load
		 * pipeline is logged, so the last line before a crash names the
		 * stage that faulted. */
		RA_LOG("[RA] game load: hash job queued to worker\n");
		snprintf(ra_status_message, sizeof(ra_status_message), "Identifying game...");
	} else {
		/* retry on the next tick */
		ra_pending_game_load = 1;
	}
}

/*
 * v6 — RENDER side (consumer). Called from the ra_net_tick() completion
 * drain, i.e. on the render thread, so the rc_client call below keeps the
 * single-writer rule intact: the worker computed the hash, the render thread
 * spends it.
 */
void ra_client_hash_done(const ra_fifo_entry *done)
{
	ra_hash_job_in_flight = 0;

	/* a hash job may outlive a logout/shutdown; drop it then */
	if (!ra_initialized || !ra_client)
		return;

	/* an rc_client load already in flight owns the session; drop this result */
	if (ra_game_loading)
		return;

	/* v15 Bug 2: a stray completion for a game we already trust is still a
	 * no-op, but a revalidation pass (menu was closed since) is not. */
	if (ra_game_loaded && !ra_revalidate_game)
		return;

	/* this completion answers the revalidation request, whatever it says */
	ra_revalidate_game = 0;

	if (!done->hash_ok) {
		/* v15 Bug 2, the "exit to XMB" half: we were revalidating a loaded
		 * game and nothing hashable is running any more -- drop the old
		 * session instead of keeping its trophies on screen. */
		if (ra_game_loaded) {
			RA_LOG("[RA] game gone (reason %d), unloading %s\n", done->hash_reason, ra_game_hash);
			rc_client_unload_game(ra_client);
			ra_game_loaded = 0;
			ra_game_hash[0] = 0;
			ra_view_rebuild_list();
		}

		switch (done->hash_reason) {
		case RA_HASH_REASON_NO_GAME:
			snprintf(ra_status_message, sizeof(ra_status_message), "No game running.");
			/*
			 * v21 funnel (GAME-COLLECTION-PLAN.md §1.2). This branch is the ONE
			 * place the system learns there is nothing bootable to show
			 * trophies for, and it runs on the render thread with the view
			 * possibly open. Instead of dead-ending on the status line, hand
			 * the screen the user's PSP game collection. The status message
			 * above stays and becomes accurate context under a populated list.
			 * No-op when the view is closed or the user is signed out.
			 */
			ra_view_no_game_running();
			/*
			 * v26 — a NO_GAME result means the identification attempt ran before
			 * the PSP game published its boot path: the v24 boot-time race
			 * (trophy-menu-delay.md Why 3-5). Re-arm the ~2s background poll so
			 * identification completes during gameplay instead of being deferred
			 * to the next menu-open (where the live 3-request chain is the
			 * user-visible ~5s blank). ra_view_no_game_running() already no-ops
			 * while the menu is closed, so nothing on screen changes here. The
			 * re-arm block in ra_tick() also requires !menu_open, so this never
			 * yanks the collection view out from under a watching user.
			 */
			if (ra_logged_in && !ra_game_loaded)
				ra_identify_retry = 1;
			break;
		case RA_HASH_REASON_POPS_GAME:
			/* v19: PSP-only. ASCII only - this string goes through the plain
			 * status-line font, not the XMB label path. */
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Not a PSP game - trophies unavailable for PS1/POPS titles.");
			/* v21: a PS1 title is not a PSP game, so showing the PSP
			 * collection is strictly better than the old dead end. */
			ra_view_no_game_running();
			break;
		case RA_HASH_REASON_BAD_PATH:
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Game path %s not accessible for hashing.",
				done->hash_psp_path[0] ? done->hash_psp_path : "(unknown)");
			break;
		default:
			snprintf(ra_status_message, sizeof(ra_status_message),
				"Could not hash %s",
				done->hash_psp_path[0] ? done->hash_psp_path : "the running game");
			break;
		}
		return;
	}

	/* v15 Bug 2, the "game switch" half: compare what is running NOW against
	 * the hash the currently loaded session was built from. */
	char prev_hash[sizeof(ra_game_hash)];
	strncpy(prev_hash, ra_game_hash, sizeof(prev_hash) - 1);
	prev_hash[sizeof(prev_hash) - 1] = 0;

	strncpy(ra_game_hash, done->hash_result, sizeof(ra_game_hash) - 1);
	ra_game_hash[sizeof(ra_game_hash) - 1] = 0;

	if (ra_game_loaded) {
		if (strcmp(prev_hash, ra_game_hash) == 0) {
			/* same game still running: keep the live session (and with it
			 * rc_client_do_frame's unlock detection), just refresh the rows */
			ra_view_rebuild_list();
			return;
		}

		/* genuine in-place switch (e.g. Wild Arms XF -> Valkyria Chronicles 2):
		 * drop the old game before loading the new one. rc_client would also
		 * unload internally on the next attach, but doing it here keeps
		 * ra_game_loaded and rc_client's own state in step. */
		RA_LOG("[RA] game switch %s -> %s, unloading\n", prev_hash, ra_game_hash);
		rc_client_unload_game(ra_client);
		ra_game_loaded = 0;
		ra_view_rebuild_list();
	}

	if (ra_logged_in && ra_net_is_online()) {
		ra_game_loading = 1;
		/* v6 instrumentation: the last line before rc_client fires the
		 * load-game request. */
		RA_LOG("[RA] begin_load_game %s\n", ra_game_hash);
		rc_client_begin_load_game(ra_client, ra_game_hash, ra_load_game_callback, NULL);
	} else if (ra_net_is_online() && (ra_login_in_progress || ra_pending_auto_login) &&
			ra_boot_net_state != -1) {
		/*
		 * v24 Option B — the game is identified but the boot-time login has
		 * not landed yet.
		 *
		 * The load request itself is NOT issued here: rc_client's game-data
		 * fetch is an authenticated call, so firing it now would just produce
		 * a "Game load failed" the user would see and then have to watch
		 * self-correct. What matters is that the EXPENSIVE half is already
		 * done -- the ISO has been hashed, ra_game_hash is populated -- so the
		 * instant ra_login_callback reports success, ra_tick() attaches the
		 * session with no second hash pass. That is what shrinks the window in
		 * which a one-time story flag can resolve before tracking arms.
		 */
		ra_load_deferred_pending = 1;
		RA_LOG("[RA] v24 hash %s ready, holding for login to resolve\n", ra_game_hash);
		snprintf(ra_status_message, sizeof(ra_status_message), "Signing in...");
	} else {
		/* offline: render from the snapshot if one exists */
		if (ra_snapshot_load(ra_game_hash) > 0)
			snprintf(ra_status_message, sizeof(ra_status_message), "Offline - showing cached data");
		else
			snprintf(ra_status_message, sizeof(ra_status_message), "Offline and no cached data for this game.");
	}
}

/* ------------------------------------------------------------------------- */
/* Events                                                                     */
/* ------------------------------------------------------------------------- */

static void ra_event_handler(const rc_client_event_t *event, rc_client_t *client)
{
	(void)client;

	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		if (event->achievement) {
			snprintf(ra_status_message, sizeof(ra_status_message), "Unlocked: %s", event->achievement->title);
			/* surface the unlock as a toast over the game (queue handles bursts) */
			ra_toast_push(event->achievement->title, event->achievement->points,
				event->achievement->badge_name);
		}
		/* v23: the account total just grew -- rebuild the welcome line so the
		 * header points count follows the unlock instead of staying frozen at
		 * its login-time value. rc_client has already credited the points to
		 * user->score_softcore before firing this event. */
		ra_update_welcome(client);
		/* refresh the rows so the new unlock is reflected */
		ra_view_rebuild_list();
		break;

	case RC_CLIENT_EVENT_GAME_COMPLETED:
		snprintf(ra_status_message, sizeof(ra_status_message), "Congratulations! All achievements earned!");
		break;

	case RC_CLIENT_EVENT_DISCONNECTED:
		snprintf(ra_status_message, sizeof(ra_status_message), "Disconnected from RetroAchievements. Unlocks pending.");
		break;

	case RC_CLIENT_EVENT_RECONNECTED:
		snprintf(ra_status_message, sizeof(ra_status_message), "Reconnected. Pending unlocks submitted.");
		break;

	case RC_CLIENT_EVENT_RESET:
		/* v32 breach detector — LOG ONLY, deliberately.
		 *
		 * rcheevos raises this when hardcore is enabled with a game loaded
		 * (waiting_for_reset set; rc_client_do_frame then processes NOTHING
		 * until rc_client_reset()). By v32 design that is unreachable: the
		 * one and only rc_client_set_hardcore_enabled call site is in
		 * ra_init() where client->game is still NULL. If this line ever
		 * appears in a hardware log, the structural guard has been broken —
		 * that is a bug to FIX, not a state to recover from. We do NOT call
		 * rc_client_reset() here: there is no emulator-reset primitive to
		 * pair it with, and a silent partial "reset" (achievements reset,
		 * RAM not rewound) would be worse than a logged stall. */
		RA_LOG("[RA] v32 UNEXPECTED EVENT_RESET (hardcore flip with game loaded?) — tracking will stall until relaunch\n");
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Lifecycle / tick                                                           */
/* ------------------------------------------------------------------------- */

void ra_init(void)
{
	if (ra_initialized)
		return;

	ra_client = rc_client_create(ra_read_psp_memory, ra_server_call);
	if (!ra_client)
		return;

	rc_client_set_event_handler(ra_client, ra_event_handler);
	rc_client_set_get_time_millisecs_function(ra_client, ra_get_time_millisecs);
	/* v32: Hardcore is applied ONCE, here, right after rc_client_create() and
	 * before any game can exist (client->game is NULL), so
	 * rc_client_enable_hardcore() takes its no-game branch
	 * (rcheevos/src/rc_client.c:6922-6924): no waiting_for_reset, no
	 * RC_CLIENT_EVENT_RESET, do_frame never stalls. This is the ONLY call site
	 * of rc_client_set_hardcore_enabled in the tree (V32 gate V1/V2). The
	 * opt-in marker file is the only input; the menu toggle changes the file,
	 * never the client, so no mid-session flip exists to guard. Default OFF. */
	ra_hardcore_active = ra_hardcore_opt_in_get();
	rc_client_set_hardcore_enabled(ra_client, ra_hardcore_active);
	RA_LOG("[RA] v32 hardcore=%d (opt-in file %s)\n", ra_hardcore_active,
		ra_hardcore_active ? "present" : "absent");

	ra_install_hash_callbacks(ra_client);

	/* v24: create the bootstrap mutex HERE, on the render thread, before
	 * ra_start_boot_net() can create the worker that will contend for it --
	 * so the creation itself is never racy. */
	ra_net_init_locks();

	/* data directories */
	sceIoMkdir(RA_BADGES_DIR, 0777);
	sceIoMkdir(RA_CACHE_DIR, 0777);

	ra_cache_psp_ram();

	/* achievement-unlock chime worker (synthesizes its PCM on its own thread;
	 * the audio port is opened lazily at the first unlock). Never fatal. */
	ra_chime_init();

	ra_initialized = 1;

	snprintf(ra_status_message, sizeof(ra_status_message), "RetroAchievements ready.");

	/* auto-login with saved token happens on the render thread */
	ra_pending_auto_login = 1;
}

void ra_shutdown(void)
{
	if (!ra_initialized)
		return;

	if (ra_client) {
		rc_client_destroy(ra_client);
		ra_client = NULL;
	}

	/* stop the chime worker and release its audio port */
	ra_chime_shutdown();

	ra_initialized = 0;
	ra_logged_in = 0;
	ra_game_loaded = 0;
	ra_revalidate_game = 0;
	ra_identify_retry = 0;   /* v26 */
}

int ra_is_initialized(void)
{
	return ra_initialized;
}

void ra_tick(void)
{
	if (!ra_initialized)
		return;

	/* completions from the net worker (invokes rc_client server callbacks) */
	ra_net_tick();

	/* badge cache maintenance (LRU aging) */
	ra_badges_tick();

	/* trophy view upkeep (login IME polling, row refresh) */
	ra_view_tick();

	/* achievement-toast queue upkeep (timeout active / promote next) */
	ra_toast_tick();

	/*
	 * v24 — auto-login as soon as the BOOT-time bootstrap has finished, not
	 * on the first menu-open.
	 *
	 * v23's comment here (kept below for the fallback it still describes)
	 * argued that ra_do_auto_login() -> ra_net_ensure_started() is synchronous
	 * and slow -- sysmodule loads, sceNetInit, curl/OpenSSL init, hundreds of
	 * milliseconds at best -- so it must not run on frame 1 of the boot. That
	 * reasoning is still correct, and this change does not violate it: by the
	 * time ra_boot_net_state == 1, RA_BootNet has ALREADY paid that cost off
	 * the render thread, so the ra_net_ensure_started() call inside
	 * ra_do_auto_login() hits its "already started" fast path and returns
	 * immediately. What is left on the render thread is
	 * rc_client_begin_login_with_token(), which is asynchronous -- it queues an
	 * HTTP request to RA_NetWorker and returns.
	 *
	 * The cost of the old behaviour was not a stutter, it was correctness:
	 * nothing was tracked until the player opened the menu, so any one-time
	 * story flag reached before that was permanently swallowed by rcheevos'
	 * already-true guard (autologin-tracking-fix-scope.md, CRITICAL-2).
	 *
	 * state == -1 keeps the exact v23 path: no boot login was attempted (no
	 * saved token, or the bootstrap/thread failed), so wait for menu_open,
	 * where paying the synchronous retry is acceptable because the PSP world
	 * is already paused.
	 * state == 0 means RA_BootNet is still working -- wait, do NOT stall.
	 */
	if (ra_pending_auto_login && !ra_logged_in && !ra_login_in_progress &&
			(ra_boot_net_state == 1 || (ra_boot_net_state == -1 && menu_open))) {
		ra_pending_auto_login = 0;
		if (!ra_has_saved_credentials() && ra_file_login_present())
			ra_file_login_attempt();   /* v26: IME-free credentials file */
		else
			ra_do_auto_login();        /* token path (unchanged) */
	}

	/*
	 * v24 Option B, part 2 — login has landed and a hash is already in hand.
	 *
	 * Attach the rc_client session straight from ra_game_hash. Ordered BEFORE
	 * the game-load gate below so that ra_game_loading is set first and the
	 * gate cannot queue a redundant second hash job for the same game.
	 */
	if (ra_load_deferred_pending && ra_logged_in && ra_client &&
			!ra_game_loading && !ra_game_loaded && ra_game_hash[0]) {
		ra_load_deferred_pending = 0;
		ra_pending_game_load = 0;
		ra_game_loading = 1;
		RA_LOG("[RA] v24 deferred begin_load_game %s (hash was ready before login)\n", ra_game_hash);
		rc_client_begin_load_game(ra_client, ra_game_hash, ra_load_game_callback, NULL);
	}

	/*
	 * v24 Option B, part 1 — game identification no longer waits for login.
	 *
	 * v23 gated this on ra_logged_in, which serialized the whole chain:
	 * login round trip, THEN read the PSP boot filename, THEN hash the ISO,
	 * THEN arm the triggers. rcheevos imposes no such ordering --
	 * rc_client_begin_load_game() and rc_client_activate_achievements()
	 * contain zero references to the client's user state
	 * (autologin-tracking-fix-scope.md E4) -- the ordering was purely this
	 * integration's own. Dropping it lets the expensive identification run
	 * concurrently with the login round trip.
	 *
	 * ra_net_is_started() replaces ra_logged_in as the gate rather than simply
	 * being removed: ra_net_queue_hash_job() calls ra_net_ensure_started()
	 * itself if the transport is down, and that call on the render thread is
	 * precisely the multi-hundred-millisecond boot stall this design exists to
	 * avoid. Checking first keeps the stall off the draw thread.
	 */
	if (ra_pending_game_load && ra_net_is_started() && !ra_game_loading &&
			(!ra_game_loaded || ra_revalidate_game)) {
		/* v15 Bug 2: also drain the request when a loaded game is pending
		 * re-identification (menu closed since it was loaded) */
		ra_start_game_load();
	} else if (ra_pending_game_load && menu_open && !ra_logged_in && !ra_login_in_progress &&
			!ra_has_saved_credentials()) {
		/* No credentials at all: offline snapshot display only.
		 *
		 * v24 added the menu_open term. ra_has_saved_credentials() opens and
		 * reads a file, and with the transport-gate above this branch is now
		 * reachable on every frame instead of being dead code -- doing that
		 * file I/O 60x a second during gameplay is not acceptable. Gating on
		 * menu_open costs nothing (the message is only ever visible there,
		 * and the PSP world is paused) and the branch clears the pending flag
		 * on its first hit anyway. */
		ra_pending_game_load = 0;
		snprintf(ra_status_message, sizeof(ra_status_message), "Not logged in. Select Trophies > Login.");
	}

	/*
	 * v26 — background re-identification retry (trophy-menu-delay.md Fix 2).
	 *
	 * When the boot-time hash lost its race against the PSP game's boot
	 * (RA_HASH_REASON_NO_GAME, ra_client_hash_done above), no game is loaded and
	 * nothing else retries. While the menu is CLOSED (i.e. during gameplay / at
	 * the PSP XMB), re-arm ra_pending_game_load on a ~2s cadence (120 ticks at
	 * 60 fps). The existing Option-B gate above then queues the hash job; once
	 * the PSP game is actually running the hash succeeds and rc_client attaches
	 * the session in the background -- so by first menu-open ra_game_loaded is
	 * already true and ra_view_opened() renders synchronously.
	 *
	 * Self-terminating and non-fighting:
	 *   - a successful load sets ra_game_loaded, which falsifies the guard;
	 *   - ra_game_loading / ra_hash_job_in_flight (set by the very paths this
	 *     feeds) suppress it while a hash or an rc_client load is out, so it can
	 *     never stack a second hash on a live menu-open load.
	 * The cost when no game ever launches is one trivial no-op hash job per ~2s
	 * (ScePspemuConvertAddress + two string reads, no file I/O, no network).
	 */
	if (ra_identify_retry && !menu_open && ra_logged_in && ra_net_is_started() &&
			!ra_game_loaded && !ra_game_loading && !ra_hash_job_in_flight) {
		static uint32_t ra_identify_retry_ticks = 0;
		if (++ra_identify_retry_ticks >= 120) {
			ra_identify_retry_ticks = 0;
			ra_identify_retry = 0;
			ra_pending_game_load = 1;   /* the Option-B gate above queues it */
		}
	}

	/* drive rc_client: the PSP world is paused while the menu is open */
	if (ra_client) {
		if (menu_open)
			rc_client_idle(ra_client);
		else if (ra_game_loaded)
			rc_client_do_frame(ra_client);
	}
}

/* ------------------------------------------------------------------------- */
/* Accessors                                                                  */
/* ------------------------------------------------------------------------- */

int ra_is_logged_in(void)
{
	return ra_logged_in;
}

int ra_is_login_in_progress(void)
{
	return ra_login_in_progress;
}

/*
 * v24 — "a login is COMING, we just have not asked yet."
 *
 * This is the missing third state that produced the "thinks we're logged out"
 * bug. The trophy view only ever asked ra_is_logged_in(), synchronously, on the
 * first frame the menu opened -- and on a healthy session with a valid token
 * that read is simply too early, because the login it is racing has not been
 * issued or has not come back yet. Answering "no" there is what painted the
 * orange offline banner and the signed-out login row over a session that was
 * about to succeed.
 *
 * Returns non-zero only while an auto-login is genuinely still queued AND the
 * boot bootstrap has not reported failure. state == -1 means no boot login is
 * coming, so a signed-out presentation is then the honest one.
 */
int ra_is_login_pending(void)
{
	if (ra_logged_in || ra_login_in_progress)
		return 0;

	return ra_pending_auto_login && ra_boot_net_state != -1;
}

/*
 * v24 — the saved token was rejected by the server (password change, revoked
 * token, banned account). Distinct from "login failed", which is usually the
 * network. The view uses this to tell the user to sign in again.
 */
int ra_is_login_stale(void)
{
	return ra_stale_token;
}

int ra_is_game_loaded(void)
{
	return ra_game_loaded;
}

/* v32 — hardcore accessors (declared in ra.h next to ra_is_game_loaded).
 *
 * ra_is_hardcore() reads the live client, never ra_hardcore_active, so the
 * save-state gates, the unlock-fetch mode and the labels cannot disagree with
 * what rcheevos is actually enforcing. */
int ra_is_hardcore(void)
{
	return ra_client ? rc_client_get_hardcore_enabled(ra_client) : 0;
}

/* Presence of the marker file IS the opt-in. Read once per launch (ra_init). */
int ra_hardcore_opt_in_get(void)
{
	SceIoStat stat;
	return sceIoGetstat(RA_HARDCORE_CFG, &stat) >= 0;
}

/* File operations ONLY — these never call rc_client_set_hardcore_enabled.
 * The Settings-tab toggle (menu.c) is the only caller; the value takes effect
 * at the next Adrenaline launch, when ra_init() reads it back. */
int ra_hardcore_opt_in_set(int enabled)
{
	if (enabled) {
		char marker = '1';
		return (WriteFile((char *)RA_HARDCORE_CFG, &marker, 1) == 1) ? 0 : -1;
	}

	return sceIoRemove(RA_HARDCORE_CFG);
}

rc_client_t *ra_get_client(void)
{
	return ra_client;
}

const char *ra_get_game_hash(void)
{
	return ra_game_hash[0] ? ra_game_hash : NULL;
}

const char *ra_get_status_message(void)
{
	return ra_status_message;
}

/* v32: setter for the enforcing save-state gate in user/main.c — see the
 * declaration in ra.h for the threading note. */
void ra_set_status_message(const char *message)
{
	if (!message)
		return;
	snprintf(ra_status_message, sizeof(ra_status_message), "%s", message);
}

const char *ra_get_welcome_message(void)
{
	return ra_welcome[0] ? ra_welcome : NULL;
}
