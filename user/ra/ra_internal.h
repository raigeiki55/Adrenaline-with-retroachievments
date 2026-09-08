/*
 * Adrenaline+ RetroAchievements subsystem — internal shared state
 */
#ifndef ADRENALINE_RA_INTERNAL_H
#define ADRENALINE_RA_INTERNAL_H

#include <psp2/types.h>
#include <psp2/kernel/clib.h>

#include "ra.h"
#include "ra_chime.h"   /* achievement-unlock chime (init/play/shutdown) */
#include "rc_api_request.h"

/* Logging ------------------------------------------------------------------ */

/*
 * Shared [RA] log macro (hoisted from ra_net.c in v6 so the login/game-load
 * chain in ra_client.c is equally observable).
 *
 * Dual sink by design: sceClibPrintf reaches a host debug console
 * (psp2shell / PSVita-DebugLog); debugPrintf appends to
 * ux0:data/adrenaline_user_log.txt, which is the only sink a user with just
 * a Vita can actually read back.
 *
 * v6 note: these lines are what pin the C2-12828-1 login-crash to a stage.
 * The v5 log ended at "stage8 worker thread started -- net layer UP" because
 * nothing past the network bootstrap was instrumented; see
 * investigation-result/v5-login-crash-debug.md WARNING #4.
 */
extern int debugPrintf(char *text, ...); /* utils.c */

#define RA_LOG(...)                       \
	do {                                  \
		sceClibPrintf(__VA_ARGS__);       \
		debugPrintf(__VA_ARGS__);         \
	} while (0)

/* FIFO --------------------------------------------------------------------- */

#define RA_FIFO_SIZE 16 /* must be a power of two */

enum ra_fifo_kind {
	RA_FIFO_NONE = 0,
	RA_FIFO_SERVER_CALL,   /* rc_client server_call completion */
	RA_FIFO_LOGIN_DONE,    /* IME login POST finished (token ready) */
	RA_FIFO_BADGE_DONE,    /* badge GET finished (body written to disk by worker) */
	RA_FIFO_HASH_JOB       /* work: hash the running game (worker); completion: hash result (render) */
};

/* Used on BOTH the render->worker work queue (kind: request to perform) and
 * the worker->render completion queue (kind: result to deliver). For work
 * entries the url/post_data/content_type pointers own heap copies; for
 * completion entries `body` owns a heap copy. */
typedef struct ra_fifo_entry {
	int kind;               /* ra_fifo_kind */
	int http_status;        /* HTTP status or RC_API_SERVER_RESPONSE_* */
	size_t body_length;     /* body bytes */
	char *body;             /* heap; owned by consumer */
	char *url;              /* heap; work entries only (render->worker) */
	char *post_data;        /* heap; work entries only (render->worker) */
	char *content_type;     /* heap; work entries only (render->worker) */
	rc_client_server_callback_t server_callback; /* SERVER_CALL */
	void *server_callback_data;                  /* SERVER_CALL */
	char badge_name[16];    /* badge file id (RA_FIFO_BADGE_DONE) */
	int locked;             /* badge locked variant (RA_FIFO_BADGE_DONE) */

	/* v6: RA_FIFO_HASH_JOB (game identification off the render thread).
	 * Work entries carry no payload (the worker reads everything itself);
	 * completion entries carry the result back to the render thread. */
	int hash_ok;            /* completion: non-zero when hash_result is valid */
	int hash_reason;        /* completion: RA_HASH_REASON_* on failure */
	char hash_psp_path[96]; /* PSP-side filename, for status messages */
	char hash_result[40];   /* completion: 32 hex chars + NUL */
} ra_fifo_entry;

/* Why a hash job failed (ra_fifo_entry.hash_reason). */
#define RA_HASH_REASON_NONE         0
#define RA_HASH_REASON_NO_GAME      1 /* SceAdrenaline.filename empty */
#define RA_HASH_REASON_BAD_PATH     2 /* no Vita-side mapping for the PSP path */
#define RA_HASH_REASON_HASH_FAILED  3 /* rc_hash_generate failed */
#define RA_HASH_REASON_POPS_GAME    4 /* SceAdrenaline.pops_mode set (PS1/POPS) */

/* Single-producer single-consumer lock-free ring. */
typedef struct ra_fifo {
	ra_fifo_entry entries[RA_FIFO_SIZE];
	volatile uint32_t head;  /* written by producer (worker thread) */
	volatile uint32_t tail;  /* written by consumer (render thread) */
} ra_fifo;

void ra_fifo_init(ra_fifo *fifo);
ra_fifo_entry *ra_fifo_produce_begin(ra_fifo *fifo);
void ra_fifo_produce_commit(ra_fifo *fifo);
ra_fifo_entry *ra_fifo_consume_begin(ra_fifo *fifo);
void ra_fifo_consume_commit(ra_fifo *fifo);

/* Net ---------------------------------------------------------------------- */

/* Phase A of the two-phase bring-up: sysmodule load only. v9 loads
 * SCE_SYSMODULE_NET and NOTHING else -- ScePaf, SSL, HTTP and the HTTPS
 * umbrella are gone, because curl+OpenSSL are statically linked and need no
 * Sony TLS/HTTP provider. Also declared in ra.h.
 * DEFERRED PATH ONLY -- called from ra_net_ensure_started() and nowhere else.
 * module_start() (user/main.c) does NOT call it: v4 did, and black-screened. */
int ra_net_preload_modules(void);

int ra_net_ensure_started(void);
int ra_net_is_online(void);

/* v24: create the bootstrap mutex that serializes ra_net_ensure_started().
 * Called once from ra_init(), on the render thread, BEFORE the RA_BootNet
 * worker exists -- so the creation itself can never race. Idempotent. */
void ra_net_init_locks(void);

/* v24: non-zero once the network bootstrap has completed (sysmodule NET,
 * sceNetInit/sceNetCtlInit, OpenSSL locking, curl_global_init, RA_NetWorker).
 *
 * This is the gate ra_tick() uses to decide it may queue a hash job WITHOUT
 * risking a synchronous bootstrap on the render thread: ra_net_queue_hash_job()
 * falls back to calling ra_net_ensure_started() itself, which is exactly the
 * multi-hundred-millisecond stall the whole deferred design existed to avoid.
 * Checking this first keeps that stall off the draw thread while still letting
 * game identification start before login resolves. */
int ra_net_is_started(void);

/* Non-zero when the TLS layer is usable this session.
 *
 * v7 semantics: "SceSsl / the SceHttps umbrella is resident". v9 semantics:
 * "curl_global_init() succeeded" -- TLS now comes from statically linked
 * OpenSSL, so the answer no longer depends on Sony providing anything. The
 * meaning ra_client.c relies on ("can we do HTTPS at all right now?") is
 * unchanged, which is why the name and the call sites are unchanged.
 * Set on the render thread in ra_net_ensure_started(), read by the net worker. */
int ra_net_ssl_is_ready(void);

/* Last TLS-class failure, 0 = none.
 *
 * v7 returned an SCE_HTTPS_ERROR_SSL_* bitmask; v9 returns the CURLcode (35,
 * 58, 59, 60, 64, 77). Only TLS-class curl errors are latched, so a plain
 * connectivity failure is never reported to the user as a certificate problem.
 * TLS verification is deliberately ENABLED on every request in ra_curl_request,
 * so these exist to make a certificate problem diagnosable rather than silent. */
unsigned int ra_net_last_ssl_error(void);

/* Human-readable form of the above, or NULL when no TLS error has been seen. */
const char *ra_net_ssl_error_text(void);

/* Drains the completion FIFO on the render thread (invokes rc_client
 * server callbacks + delivers badge bodies). */
void ra_net_tick(void);

/* Called from rc_client (render thread): deep-copies the request and queues
 * it for the worker thread. NEVER touches rc_client after returning. */
void ra_server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data, rc_client_t *client);

/* Badge fetch: worker performs GET and writes the raw PNG to the badge cache
 * dir, then posts RA_FIFO_BADGE_DONE. Returns 1 when queued, 0 when it could
 * not be (net worker not up / offline / FIFO full). */
int ra_net_fetch_badge(const char *url, const char *badge_name, int locked);

/* v6 game-load worker plumbing (see ra_client.c "Game-load worker" header).
 *
 * The hash chain (SceAdrenaline filename read + PSP-path translation +
 * rc_hash file I/O) runs on the RA_NetWorker thread, NOT the render thread.
 * The render thread only queues the job and later consumes the result:
 *
 *   render: ra_net_queue_hash_job()  -- posts a RA_FIFO_HASH_JOB work entry
 *   worker:                          -- runs the chain, posts the completion
 *   render: ra_net_tick() drain      -- calls ra_client_hash_done(), which
 *                                       does the rc_client_begin_load_game
 *                                       (still on the render thread, so the
 *                                       rc_client single-writer rule holds).
 *
 * ra_net_queue_hash_job returns non-zero when the job was queued, 0 when it
 * could not be (net worker not up / FIFO full). */
int ra_net_queue_hash_job(void);

/* Render-thread consumer of a RA_FIFO_HASH_JOB completion. Called ONLY from
 * the ra_net_tick() drain. */
void ra_client_hash_done(const ra_fifo_entry *done);

/* Worker-side hash chain (ra_client.c): reads the Kermit block's boot
 * filename, translates the PSP path, runs rc_hash. Runs on RA_NetWorker;
 * must NEVER touch rc_client or render-thread state. */
void ra_hash_worker_job(char hash_out[40], int *ok_out, int *reason_out, char *psp_path_out, int psp_path_size);

/* Worker loop entry (SceKernelStartModule style). */
int ra_net_worker_main(unsigned int args, void *argp);

/* HTTP GET into a malloc'd buffer (worker thread context).
 * Returns malloc'd body and sets *out_len; NULL on failure. */
char *ra_http_get(const char *url, size_t *out_len, int *out_status);

/* Client ------------------------------------------------------------------- */

/* Login callback marshaling (render thread). */
void ra_client_login_succeeded(const char *username, const char *display_name, uint32_t score, const char *token);
void ra_client_login_failed(const char *error_message);

/* Hash callback setup (custom sceIo filereader + default cdreader). */
void ra_install_hash_callbacks(rc_client_t *client);

/* View --------------------------------------------------------------------- */

/* Refresh the achievement list from the client (buckets flattened). */
void ra_view_rebuild_list(void);

/* Per-frame upkeep of view + badge cache. */
void ra_view_tick(void);
void ra_badges_tick(void);

/* Welcome line ("Welcome, X! (N pts)") once logged in, else NULL. */
const char *ra_get_welcome_message(void);

/* Game collection (v21, ra_gamelist.c) ------------------------------------- */
/*
 * PS3-style trophy-collection browser. Everything below runs on the RENDER
 * thread only (the two rc_client begin_* calls, the two raw rc_api calls, and
 * every completion callback -- the latter are invoked from the ra_net_tick()
 * SERVER_CALL drain, which is render-thread by construction, ra_net.c:1354).
 * So the view and the gamelist module share plain statics with no locking, the
 * same single-writer discipline the rest of the subsystem uses (ra.h:8-13).
 */

#define RA_MAX_GAMES 256

typedef struct ra_game_row {
	uint32_t game_id;
	char     title[96];
	char     icon_name[16];   /* RA image_name, e.g. "085573" */
	uint32_t num_achievements;
	uint32_t num_unlocked;    /* softcore */
	uint32_t num_unlocked_hc;
} ra_game_row;

/* ra_gamelist_state() -- Screen A (the collection) */
#define RA_GL_IDLE      0
#define RA_GL_PROGRESS  1   /* rc_client_begin_fetch_all_user_progress in flight */
#define RA_GL_TITLES    2   /* rc_client_begin_fetch_game_titles in flight       */
#define RA_GL_READY     3
#define RA_GL_ERROR     4

/* ra_detail_state() -- Screen B-remote (one game's read-only grid) */
#define RA_DT_IDLE      0
#define RA_DT_GAMEDATA  1   /* rc_api fetch_game_data in flight     */
#define RA_DT_UNLOCKS   2   /* rc_api fetch_user_unlocks in flight  */
#define RA_DT_READY     3
#define RA_DT_ERROR     4

/* Kick the two-call collection fetch. Shows the disk cache instantly when one
 * exists, and latches after the first SUCCESSFUL live fetch of the session, so
 * reopening the trophy tab does not refetch. */
void ra_gamelist_request(void);

/* Load ra_cache/games_c41.bin into the row array. Returns the row count, or
 * <= 0 when there is no usable cache. */
int ra_gamelist_load_cache(void);

const ra_game_row *ra_gamelist_rows(int *count);
int ra_gamelist_state(void);
const char *ra_gamelist_error(void);

/* Non-zero once a live fetch has succeeded this session (drives the offline
 * banner: rows that came only from disk are stale). */
int ra_gamelist_is_cached_only(void);

/* Non-zero while a fetch pair is out. Distinct from the state: with a disk
 * cache on screen the state is READY throughout the background refresh. */
int ra_gamelist_is_fetching(void);

/* Screen B-remote: fetch definitions + this user's unlocks for a game that is
 * NOT booted, via raw rc_api over ra_server_call. Read-only by API shape. */
void ra_detail_request(uint32_t game_id);
int ra_detail_state(void);
const char *ra_detail_error(void);

/* Funnel from ra_client_hash_done()'s "nothing hashable is running" branch. */
void ra_view_no_game_running(void);

/*
 * Row-array population accessors, implemented in ra_trophy_view.c and called
 * from ra_gamelist.c's detail callbacks.
 *
 * DEVIATION FROM PLAN §5.2 (documented, deliberate): the plan had ra_gamelist.c
 * write ra_rows[] "directly". That array is static to ra_trophy_view.c and the
 * plan also requires ra_gamelist.c to contain no drawing/view logic. Rather
 * than un-static the row array (which would expose the grid's backing store to
 * every TU), the detail callbacks push rows through these four calls. Same
 * array, same fields, same single thread -- just not a shared global.
 */
void ra_view_detail_begin(void);
int  ra_view_detail_add_row(uint32_t id, const char *badge_name, const char *title,
	const char *description, uint32_t points, float rarity);
void ra_view_detail_mark_unlocked(uint32_t id);
void ra_view_detail_finish(const char *game_icon_name);

#endif /* ADRENALINE_RA_INTERNAL_H */
