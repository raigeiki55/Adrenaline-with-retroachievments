/*
 * Adrenaline+ RetroAchievements — v21 game collection (PS3-style browser).
 *
 * GAME-COLLECTION-PLAN.md §2 and §5.2. Two independent jobs live here:
 *
 *   Screen A (the collection list)
 *     rc_client_begin_fetch_all_user_progress(console 41 = PSP)  -> counts
 *     rc_client_begin_fetch_game_titles(the ids from call 1)     -> titles+art
 *     joined by game_id into ra_games[], cached to
 *     ux0:data/PSPEMUCFW/ra_cache/games_c41.bin.
 *
 *   Screen B-remote (one non-booted game's read-only grid)
 *     raw rc_api_init_fetch_game_data_request   -> achievement definitions
 *     raw rc_api_init_fetch_user_unlocks_request -> which ids this user earned
 *     both issued through ra_server_call() directly, with no rc_client session:
 *     rc_client_begin_load_game keys on a FILE HASH and there is no file to
 *     hash for a game that is not running (ra_client.c:854), and starting a
 *     session would fight the real one (ra_game_loaded / do_frame / snapshots).
 *
 * THREADING. Everything in this file runs on the render thread:
 *   - ra_gamelist_request / ra_detail_request are called from the view, which
 *     is driven by AdrenalineDraw;
 *   - every callback below is invoked from the ra_net_tick() completion drain
 *     (ra_net.c:1354-1372), which is also the render thread.
 * So plain statics, no locking -- the subsystem's single-writer rule (ra.h:8-13)
 * holds unchanged. There is NO new thread and NO new transport: both rc_client
 * calls and both raw rc_api calls ride the existing ra_server_call FIFO.
 *
 * NO vita2d / GXM / drawing code belongs in this file.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "ra_internal.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"

/* ------------------------------------------------------------------------- */
/* Screen A state                                                             */
/* ------------------------------------------------------------------------- */

static ra_game_row ra_games[RA_MAX_GAMES];
static int ra_n_games = 0;
static int ra_gl_state = RA_GL_IDLE;
static char ra_gl_error[96];

/*
 * Session latch (plan §2.4). RA's own integration guidance is "cache
 * aggressively, do not poll"; the collection changes at most once per play
 * session and the list is not the mechanism by which a just-earned trophy
 * appears (that is the live current-game path). So the first SUCCESSFUL live
 * fetch of an Adrenaline session is the last one: reopening the trophy tab
 * replays from memory. Same idiom as the v18 prefetch arm latch.
 */
static int ra_gl_fetched_this_session = 0;

/* Non-zero while the rows on screen came only from disk (no live fetch has
 * completed yet this session) -- drives the orange offline banner. */
static int ra_gl_cached_only = 0;

/*
 * Explicit in-flight flag, NOT derived from ra_gl_state.
 *
 * When a disk cache exists the state is deliberately READY while the network
 * pair runs (that is the whole point: show cached rows instantly, refresh over
 * the top), so state alone cannot answer "is a fetch already out?". Without
 * this, the two repeat callers -- the hash-failure funnel and the 5s reconnect
 * poll in ra_view_tick -- could each stack a second request pair on top of a
 * live one.
 */
static int ra_gl_inflight = 0;

/* Non-zero while the collection is being (re)fetched; the view draws a
 * "Refreshing..." line when rows are already on screen. */
int ra_gamelist_is_fetching(void)
{
	return ra_gl_inflight;
}

/* ------------------------------------------------------------------------- */
/* Disk cache — format mirrors the proven snapshot pattern (ra_badges.c:527+)  */
/* ------------------------------------------------------------------------- */

#define RA_GAMES_MAGIC "RAGAMES1"

static void ra_gamelist_cache_path(char *out, int out_size)
{
	/* console id is in the filename so a future non-PSP console cannot alias */
	snprintf(out, out_size, "%s/games_c%d.bin", RA_CACHE_DIR, RA_CONSOLE_PSP);
}

static void ra_gamelist_save_cache(void)
{
	char path[256];
	SceUID fd;

	if (ra_n_games <= 0)
		return;

	sceIoMkdir(RA_CACHE_DIR, 0777);   /* harmless when it already exists */

	ra_gamelist_cache_path(path, sizeof(path));
	fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		RA_LOG("[RA] gamelist cache save failed (open %s)\n", path);
		return;
	}
	sceIoWrite(fd, RA_GAMES_MAGIC, 8);
	sceIoWrite(fd, &ra_n_games, sizeof(ra_n_games));
	sceIoWrite(fd, ra_games, ra_n_games * (int)sizeof(ra_game_row));
	sceIoClose(fd);

	RA_LOG("[RA] gamelist cache saved: %d games\n", ra_n_games);
}

int ra_gamelist_load_cache(void)
{
	char path[256];
	char magic[8];
	int count = 0;
	SceUID fd;

	ra_gamelist_cache_path(path, sizeof(path));

	fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;

	if (sceIoRead(fd, magic, 8) != 8 || memcmp(magic, RA_GAMES_MAGIC, 8) != 0) {
		sceIoClose(fd);
		return -1;
	}
	if (sceIoRead(fd, &count, sizeof(count)) != sizeof(count) ||
		count < 0 || count > RA_MAX_GAMES) {
		sceIoClose(fd);
		return -1;
	}
	if (count > 0 &&
		sceIoRead(fd, ra_games, count * (int)sizeof(ra_game_row)) !=
			count * (int)sizeof(ra_game_row)) {
		sceIoClose(fd);
		return -1;
	}
	sceIoClose(fd);

	ra_n_games = count;
	RA_LOG("[RA] gamelist cache loaded: %d games\n", count);
	return count;
}

/* ------------------------------------------------------------------------- */
/* Accessors                                                                  */
/* ------------------------------------------------------------------------- */

const ra_game_row *ra_gamelist_rows(int *count)
{
	if (count)
		*count = ra_n_games;
	return ra_games;
}

int ra_gamelist_state(void)
{
	return ra_gl_state;
}

const char *ra_gamelist_error(void)
{
	return ra_gl_error[0] ? ra_gl_error : NULL;
}

int ra_gamelist_is_cached_only(void)
{
	return ra_gl_cached_only;
}

static void ra_gamelist_set_error(const char *msg)
{
	ra_gl_inflight = 0;                  /* every error path is terminal */

	snprintf(ra_gl_error, sizeof(ra_gl_error), "%s",
		(msg && msg[0]) ? msg : "Could not load your game list.");

	/* a cache (loaded now or already on screen) beats an error page */
	if (ra_n_games > 0 || ra_gamelist_load_cache() > 0) {
		ra_gl_cached_only = 1;
		ra_gl_state = RA_GL_READY;
		RA_LOG("[RA] gamelist error (%s) -> falling back to %d cached games\n",
			ra_gl_error, ra_n_games);
	} else {
		ra_gl_state = RA_GL_ERROR;
		RA_LOG("[RA] gamelist error: %s\n", ra_gl_error);
	}
}

/* ------------------------------------------------------------------------- */
/* Call 2 — titles + icon names                                               */
/* ------------------------------------------------------------------------- */

/*
 * OWNERSHIP NOTE (deviation from plan §2.2's parenthetical, verified in
 * source): rc_client mallocs `list`, hands it to this callback and then does
 * NOT free it -- see rc_client.c:1094-1099 and :3818-3823, where the callback
 * invocation is the last use of the pointer. The CALLBACK owns it. Both
 * callbacks below therefore call the matching rc_client_destroy_* on every
 * path where list is non-NULL. Getting this wrong is a per-fetch leak.
 */
static void ra_gamelist_titles_cb(int result, const char *error_message,
	rc_client_game_title_list_t *list, rc_client_t *client, void *userdata)
{
	uint32_t i;
	int j;
	int matched = 0;

	(void)client;
	(void)userdata;

	/* a completion can outlive ra_shutdown(); drop it then (ra_client.c:770) */
	if (!ra_get_client()) {
		if (list)
			rc_client_destroy_game_title_list(list);
		ra_gl_inflight = 0;
		return;
	}

	if (result != RC_OK || !list) {
		/*
		 * Titles failed but the counts did not: keep the rows. Every one of
		 * them already carries a "Game #<id>" placeholder title from call 1,
		 * so the collection is still usable (and correct) without art.
		 */
		RA_LOG("[RA] gamelist titles failed: result=%d %s\n", result,
			error_message ? error_message : "(no message)");
		if (list)
			rc_client_destroy_game_title_list(list);

		ra_gl_inflight = 0;
		ra_gl_state = RA_GL_READY;
		ra_gl_cached_only = 0;
		ra_gl_fetched_this_session = 1;   /* counts ARE live */
		ra_gamelist_save_cache();
		return;
	}

	for (i = 0; i < list->num_entries; i++) {
		const rc_client_game_title_entry_t *e = &list->entries[i];
		for (j = 0; j < ra_n_games; j++) {
			if (ra_games[j].game_id != e->game_id)
				continue;
			if (e->title && e->title[0]) {
				strncpy(ra_games[j].title, e->title, sizeof(ra_games[j].title) - 1);
				ra_games[j].title[sizeof(ra_games[j].title) - 1] = 0;
			}
			/* badge_name is the RA image_name; badge_url is deliberately
			 * IGNORED -- it is built against the media host, and v13 moved
			 * this app's image traffic to the apex host (ra_badges.c:313-349). */
			strncpy(ra_games[j].icon_name, e->badge_name, sizeof(ra_games[j].icon_name) - 1);
			ra_games[j].icon_name[sizeof(ra_games[j].icon_name) - 1] = 0;
			matched++;
			break;
		}
	}

	rc_client_destroy_game_title_list(list);

	ra_gl_inflight = 0;
	ra_gl_error[0] = 0;
	ra_gl_state = RA_GL_READY;
	ra_gl_cached_only = 0;
	ra_gl_fetched_this_session = 1;
	RA_LOG("[RA] gamelist ready: %d games (%d titled)\n", ra_n_games, matched);

	ra_gamelist_save_cache();
}

/* ------------------------------------------------------------------------- */
/* Call 1 — per-console progress counts                                       */
/* ------------------------------------------------------------------------- */

static void ra_gamelist_progress_cb(int result, const char *error_message,
	rc_client_all_user_progress_t *list, rc_client_t *client, void *userdata)
{
	uint32_t i;
	uint32_t ids[RA_MAX_GAMES];
	int n_ids = 0;

	(void)userdata;

	if (!ra_get_client()) {
		if (list)
			rc_client_destroy_all_user_progress(list);
		ra_gl_inflight = 0;
		return;
	}

	if (result != RC_OK || !list) {
		if (list)
			rc_client_destroy_all_user_progress(list);
		ra_gamelist_set_error(error_message);
		return;
	}

	RA_LOG("[RA] gamelist allprogress: %u entries for console %d\n",
		list->num_entries, RA_CONSOLE_PSP);

	/*
	 * FILTER (plan §2.2): keep only games this user has actually unlocked
	 * something in. Two reasons, both load-bearing:
	 *   - product: the PS3 model is "YOUR collection", not RA's PSP catalog;
	 *   - safety: the rc_client doc comment says the query covers games
	 *     "tracked by this console", and whether the server scopes it to the
	 *     played set is not verifiable offline. PSP has 1000+ sets, so without
	 *     this the list could be the whole catalog.
	 * A zero-row result is an empty state, not an error.
	 */
	ra_n_games = 0;
	for (i = 0; i < list->num_entries && ra_n_games < RA_MAX_GAMES; i++) {
		const rc_client_all_user_progress_entry_t *e = &list->entries[i];
		ra_game_row *row;

		if (e->num_unlocked_achievements == 0 &&
			e->num_unlocked_achievements_hardcore == 0)
			continue;

		row = &ra_games[ra_n_games++];
		memset(row, 0, sizeof(*row));
		row->game_id = e->game_id;
		row->num_achievements = e->num_achievements;
		row->num_unlocked = e->num_unlocked_achievements;
		row->num_unlocked_hc = e->num_unlocked_achievements_hardcore;
		/* placeholder until call 2 lands; a row is NEVER dropped for want of
		 * a title */
		snprintf(row->title, sizeof(row->title), "Game #%u", e->game_id);

		if (n_ids < RA_MAX_GAMES)
			ids[n_ids++] = e->game_id;
	}

	/* `list` is ours; nothing below retains a pointer into it */
	rc_client_destroy_all_user_progress(list);

	RA_LOG("[RA] gamelist filtered to %d played games\n", ra_n_games);

	if (n_ids == 0) {
		ra_gl_inflight = 0;
		ra_gl_error[0] = 0;
		ra_gl_state = RA_GL_READY;      /* empty state, drawn by the view */
		ra_gl_cached_only = 0;
		ra_gl_fetched_this_session = 1;
		return;
	}

	/* chain call 2 -- still on the render thread, so the single-writer rule
	 * holds for this second rc_client entry point too */
	ra_gl_state = RA_GL_TITLES;
	rc_client_begin_fetch_game_titles(ra_get_client(), ids, (uint32_t)n_ids,
		ra_gamelist_titles_cb, NULL);
}

/* ------------------------------------------------------------------------- */
/* Screen A entry point                                                       */
/* ------------------------------------------------------------------------- */

void ra_gamelist_request(void)
{
	rc_client_t *client = ra_get_client();

	if (ra_gl_inflight)
		return;                          /* a pair is already out */

	if (ra_gl_fetched_this_session) {
		/*
		 * Session latch (plan §2.4). A live fetch already succeeded this boot,
		 * so the rows in memory ARE the live answer -- reopening the trophy
		 * tab must not refetch. A game unlocked during this session surfaces
		 * through the normal current-game path, not through this list.
		 */
		ra_gl_state = RA_GL_READY;
		return;
	}

	/* show whatever is on disk INSTANTLY, then refresh over the top */
	if (ra_n_games == 0 && ra_gamelist_load_cache() > 0) {
		ra_gl_cached_only = 1;
		ra_gl_state = RA_GL_READY;
	}

	if (!client || !ra_is_logged_in()) {
		if (ra_n_games == 0)
			ra_gamelist_set_error("Log in to RetroAchievements to see your games.");
		return;
	}
	if (!ra_net_is_online()) {
		if (ra_n_games == 0)
			ra_gamelist_set_error("Offline - no cached game list.");
		return;
	}

	ra_gl_error[0] = 0;
	if (ra_n_games == 0)
		ra_gl_state = RA_GL_PROGRESS;    /* nothing to show yet: say "fetching" */

	RA_LOG("[RA] gamelist request: allprogress console %d\n", RA_CONSOLE_PSP);
	ra_gl_inflight = 1;
	rc_client_begin_fetch_all_user_progress(client, RA_CONSOLE_PSP,
		ra_gamelist_progress_cb, NULL);
	/*
	 * NOTE: that call can complete SYNCHRONOUSLY on its validation paths
	 * (rc_client.c:1115-1140 invokes the callback before returning), and
	 * ra_server_call does the same when offline or the FIFO is full
	 * (ra_net.c:1110-1130). Both cases have already cleared ra_gl_inflight and
	 * set the final state by the time we get here, so nothing is assigned
	 * after the call.
	 */
}

/* ------------------------------------------------------------------------- */
/* Screen B-remote — one non-booted game's read-only grid                     */
/* ------------------------------------------------------------------------- */

static int ra_dt_state = RA_DT_IDLE;
static char ra_dt_error[96];
static uint32_t ra_dt_game_id = 0;      /* the game the in-flight pair is for */
static char ra_dt_icon_name[16];

int ra_detail_state(void)
{
	return ra_dt_state;
}

const char *ra_detail_error(void)
{
	return ra_dt_error[0] ? ra_dt_error : NULL;
}

static void ra_detail_fail(const char *msg)
{
	snprintf(ra_dt_error, sizeof(ra_dt_error), "%s",
		(msg && msg[0]) ? msg : "Could not load this game's achievements.");
	ra_dt_state = RA_DT_ERROR;
	RA_LOG("[RA] detail error: %s\n", ra_dt_error);
}

/* --- step 2 of 2: which achievements this user has earned ----------------- */

static void ra_detail_unlocks_cb(const rc_api_server_response_t *resp, void *ud)
{
	rc_api_fetch_user_unlocks_response_t response;
	uint32_t game_id = (uint32_t)(uintptr_t)ud;
	uint32_t i;
	int rc;

	if (!ra_get_client())
		return;

	/* stale completion: the user backed out or picked another game */
	if (game_id != ra_dt_game_id)
		return;

	memset(&response, 0, sizeof(response));
	rc = rc_api_process_fetch_user_unlocks_server_response(&response, resp);
	if (rc != RC_OK) {
		rc_api_destroy_fetch_user_unlocks_response(&response);
		/*
		 * Definitions already landed and are on screen. Unlock state is the
		 * only casualty, so finish READY with everything shown locked rather
		 * than throwing the grid away.
		 */
		RA_LOG("[RA] detail unlocks failed rc=%d http=%d\n", rc, resp ? resp->http_status_code : -1);
		ra_view_detail_finish(ra_dt_icon_name);
		ra_dt_state = RA_DT_READY;
		return;
	}

	for (i = 0; i < response.num_achievement_ids; i++)
		ra_view_detail_mark_unlocked(response.achievement_ids[i]);

	RA_LOG("[RA] detail unlocks: %u earned\n", response.num_achievement_ids);
	rc_api_destroy_fetch_user_unlocks_response(&response);

	ra_view_detail_finish(ra_dt_icon_name);
	ra_dt_error[0] = 0;
	ra_dt_state = RA_DT_READY;
}

static void ra_detail_begin_unlocks(uint32_t game_id)
{
	const rc_client_user_t *user = rc_client_get_user_info(ra_get_client());
	rc_api_fetch_user_unlocks_request_t params;
	rc_api_request_t request;

	if (!user) {
		ra_detail_fail("Log in to view this game.");
		return;
	}

	memset(&params, 0, sizeof(params));
	params.username = user->username;
	params.api_token = user->token;
	params.game_id = game_id;
	/* v32: fetch unlocks for the LIVE mode. Querying softcore while the
	 * client enforces hardcore would paint casual unlock counts over the
	 * hardcore grid (V32 gap-map a2). */
	params.hardcore = ra_is_hardcore();

	memset(&request, 0, sizeof(request));
	if (rc_api_init_fetch_user_unlocks_request(&request, &params) != RC_OK) {
		rc_api_destroy_request(&request);
		ra_detail_fail("Could not build the unlocks request.");
		return;
	}

	ra_dt_state = RA_DT_UNLOCKS;
	/* ra_server_call strdups url/post_data/content_type before returning, so
	 * destroying the request immediately after is correct (ra_net.c:1131-1142) */
	ra_server_call(&request, ra_detail_unlocks_cb, (void *)(uintptr_t)game_id, NULL);
	rc_api_destroy_request(&request);
}

/* --- step 1 of 2: the achievement definitions ----------------------------- */

static void ra_detail_gamedata_cb(const rc_api_server_response_t *resp, void *ud)
{
	rc_api_fetch_game_data_response_t response;
	uint32_t game_id = (uint32_t)(uintptr_t)ud;
	uint32_t i;
	int rc;
	int added = 0;

	if (!ra_get_client())
		return;

	if (game_id != ra_dt_game_id)
		return;                          /* stale: screen moved on */

	memset(&response, 0, sizeof(response));
	rc = rc_api_process_fetch_game_data_server_response(&response, resp);
	if (rc != RC_OK) {
		RA_LOG("[RA] detail gamedata failed rc=%d http=%d\n", rc,
			resp ? resp->http_status_code : -1);
		rc_api_destroy_fetch_game_data_response(&response);
		ra_detail_fail("Could not load this game's achievements.");
		return;
	}

	/* the game's own art, for the detail header (and it is normally already
	 * warm: the list drew the same icon) */
	if (response.image_name && response.image_name[0]) {
		strncpy(ra_dt_icon_name, response.image_name, sizeof(ra_dt_icon_name) - 1);
		ra_dt_icon_name[sizeof(ra_dt_icon_name) - 1] = 0;
	}

	ra_view_detail_begin();
	for (i = 0; i < response.num_achievements; i++) {
		const rc_api_achievement_definition_t *a = &response.achievements[i];

		/* CORE only -- same filter the live grid uses
		 * (RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE in ra_view_rebuild_list) */
		if (a->category != RC_ACHIEVEMENT_CATEGORY_CORE)
			continue;

		if (ra_view_detail_add_row(a->id, a->badge_name, a->title,
				a->description, a->points, a->rarity))
			added++;
	}

	RA_LOG("[RA] detail gamedata: %u defs, %d core rows\n",
		response.num_achievements, added);

	rc_api_destroy_fetch_game_data_response(&response);

	/* chain the unlocks query; the rows are drawn (all locked) meanwhile */
	ra_detail_begin_unlocks(game_id);
}

void ra_detail_request(uint32_t game_id)
{
	rc_client_t *client = ra_get_client();
	const rc_client_user_t *user;
	rc_api_fetch_game_data_request_t params;
	rc_api_request_t request;

	ra_dt_error[0] = 0;
	ra_dt_icon_name[0] = 0;
	ra_dt_game_id = game_id;             /* also invalidates any in-flight pair */
	ra_dt_state = RA_DT_IDLE;

	/* start from an empty grid so a failure cannot show the PREVIOUS game's
	 * achievements under this game's header */
	ra_view_detail_begin();

	if (!client || !ra_is_logged_in()) {
		ra_detail_fail("Log in to RetroAchievements to view this game.");
		return;
	}
	if (!ra_net_is_online()) {
		ra_detail_fail("Offline - this game's achievements are not cached.");
		return;
	}

	user = rc_client_get_user_info(client);
	if (!user) {
		ra_detail_fail("Log in to RetroAchievements to view this game.");
		return;
	}

	memset(&params, 0, sizeof(params));
	params.username = user->username;
	params.api_token = user->token;
	params.game_id = game_id;
	params.game_hash = NULL;             /* ignored when game_id != 0 */

	memset(&request, 0, sizeof(request));
	if (rc_api_init_fetch_game_data_request(&request, &params) != RC_OK) {
		rc_api_destroy_request(&request);
		ra_detail_fail("Could not build the game-data request.");
		return;
	}

	ra_dt_state = RA_DT_GAMEDATA;
	RA_LOG("[RA] detail request game %u\n", game_id);
	ra_server_call(&request, ra_detail_gamedata_cb, (void *)(uintptr_t)game_id, NULL);
	rc_api_destroy_request(&request);
}
