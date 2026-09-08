/*
 * Adrenaline+ RetroAchievements — trophy screen (tab body in the Adrenaline
 * menu) with INLINE login per the UX directive:
 *
 *   1. The XMB "Trophies" icon (or the PS-button fallback tab) opens this
 *      screen.
 *   2. If not logged in when the screen opens, the login prompt appears
 *      inline on this same screen (Vita IME dialog) — no need to hunt for a
 *      separate login entry.
 *   3. After login: welcome (display name + score) and the trophy list for
 *      the current game.
 */
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/sysmodule.h>      /* v26: SCE_SYSMODULE_IME (v25-login-crash.md) */
#include <psp2/system_param.h>   /* v25: SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <vita2d.h>

#include "ra_internal.h"
#include "../menu.h"
#include "../utils.h"

extern int menu_open; /* menu.c */

/* ------------------------------------------------------------------------- */
/* View model                                                                 */
/* ------------------------------------------------------------------------- */

#define RA_MAX_ROWS    512
#define RA_ROWS_VISIBLE 6
#define RA_ROW_HEIGHT   54.0f

typedef struct ra_view_row {
	char badge_name[16];
	char title[96];
	char description[128];
	char right_top[32];   /* "5 pts" */
	char right_bottom[32]; /* unlock date / rarity */
	uint32_t id;          /* v21: RA achievement id -- the join key the remote
	                       * DETAIL screen needs to mark unlocks (the unlocks
	                       * endpoint returns bare ids). Populated by both
	                       * builders; unused and harmless on the other screens. */
	uint32_t points;
	int unlocked;
	int platinum;         /* synthesized mastery row */
} ra_view_row;

static ra_view_row ra_rows[RA_MAX_ROWS];
static int ra_n_rows = 0;
static int ra_row_sel = 0;
static int ra_row_base = 0;
static int ra_view_open = 0;
static int ra_offline_display = 0;

/* ------------------------------------------------------------------------- */
/* v21 — the three screens (GAME-COLLECTION-PLAN.md §1)                       */
/*                                                                            */
/* CURRENT is everything this file did before v21 and is the default on every  */
/* ra_view_opened(). The ONLY way out of it is ra_view_no_game_running(),      */
/* which the hash-failure funnel in ra_client_hash_done() calls when nothing   */
/* hashable is booted. A running, hashable PSP game therefore CANNOT reach the */
/* collection code at all -- that is the structural regression guard for the   */
/* booted-game path (plan Risk §9.2).                                          */
/* ------------------------------------------------------------------------- */

enum ra_screen {
	RA_SCREEN_CURRENT = 0,  /* trophy grid for the booted game (unchanged)     */
	RA_SCREEN_LIST,         /* Screen A: the user's PSP game collection        */
	RA_SCREEN_DETAIL,       /* Screen B-remote: one collection game, read-only */
};

static int ra_screen = RA_SCREEN_CURRENT;
static uint32_t ra_detail_game_id = 0;
static char ra_detail_title[96];
static char ra_detail_icon[16];

/* Screen A navigation, mirroring ra_row_sel / ra_row_base exactly. Kept
 * separate so returning from DETAIL restores the collection position (H4). */
static int ra_game_sel = 0;
static int ra_game_base = 0;

/* grade colors (ABGR) */
#define RA_GRADE_BRONZE   0xFF327FCD
#define RA_GRADE_SILVER   0xFFC0C0C0
#define RA_GRADE_GOLD     0xFF00D7FF
#define RA_GRADE_PLATINUM 0xFFE2E4E5

static uint32_t ra_grade_color(uint32_t points)
{
	if (points >= 25)
		return RA_GRADE_GOLD;
	if (points >= 10)
		return RA_GRADE_SILVER;
	return RA_GRADE_BRONZE;
}

static void ra_format_unlock_time(uint64_t t, char *out, int out_size)
{
	time_t tt = (time_t)t;
	struct tm *tm = gmtime(&tt);
	if (!tm) {
		out[0] = 0;
		return;
	}
	snprintf(out, out_size, "Unlocked %04d/%02d/%02d",
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

/* ------------------------------------------------------------------------- */
/* v18 badge prefetch (investigation-result/toast-badge-delay.md)             */
/*                                                                            */
/* The badge cache was pull-based only: ra_badge_get() had exactly two call    */
/* sites, both at DRAW time (ra_draw_badge below, and ra_toast.c:197). So the  */
/* first unlock of an achievement was the first time its UNLOCKED badge        */
/* variant was ever requested -- the trophy list only ever asks for the LOCKED */
/* variant while an achievement is locked (ra_draw_badge call at the row loop  */
/* passes !row->unlocked). That put a cold, full-TLS download on the toast's   */
/* critical path, behind the award-submission POST in the single work FIFO:    */
/* the reported 2-3 s of grey placeholder.                                     */
/*                                                                            */
/* Fix: when the row list is (re)built, walk it once and warm BOTH variants of */
/* every badge, so the toast hits memory/disk instead of the network. Same     */
/* state machine, just triggered earlier.                                      */
/*                                                                            */
/* Pacing matters (report, "Risk Assessment"): 92 achievements x 2 variants is */
/* up to 184 downloads, against a 16-deep work FIFO shared with the API/award  */
/* traffic. So the walk issues at most ONE download per RA_PREFETCH_ISSUE_GAP  */
/* frames -- roughly the rate the single net worker drains them -- and budgets */
/* its cheap already-cached probes per frame. It is a background trickle, not  */
/* a burst, and it never blocks a draw.                                        */
/* ------------------------------------------------------------------------- */

#define RA_PREFETCH_IDLE      (-1)
#define RA_PREFETCH_ISSUE_GAP  30   /* frames to wait after issuing a download */
#define RA_PREFETCH_SCAN_MAX   16   /* already-cached probes allowed per frame */

static int ra_prefetch_cursor = RA_PREFETCH_IDLE;  /* 0..2*rows-1, pass-major */
static int ra_prefetch_rows = 0;                   /* row count the walk was armed for */
static uint32_t ra_prefetch_wait = 0;
static unsigned ra_prefetch_issued = 0;
static char ra_prefetch_gen_hash[48];              /* game the walk was armed for */
static int ra_prefetch_gen_rows = -1;

/*
 * One-shot latch (report's adversarial self-review, point 2): ra_view_rebuild_list
 * also runs on the 60-tick periodic refresh and on every unlock event, so arming
 * unconditionally would restart the walk forever. Re-arm only when the list
 * actually changed identity (different game hash or different row count), or
 * when 'force' says this rebuild is the offline->online transition, which is the
 * reconnect backfill: badges the offline session could not fetch get picked up.
 */
static void ra_view_prefetch_arm(int force)
{
	const char *hash = ra_get_game_hash();
	char h[48];

	h[0] = 0;
	if (hash)
		strncpy(h, hash, sizeof(h) - 1);
	h[sizeof(h) - 1] = 0;

	if (!force && ra_prefetch_gen_rows == ra_n_rows &&
		strcmp(h, ra_prefetch_gen_hash) == 0)
		return;   /* same list: a walk already ran (or is still running) */

	strncpy(ra_prefetch_gen_hash, h, sizeof(ra_prefetch_gen_hash) - 1);
	ra_prefetch_gen_hash[sizeof(ra_prefetch_gen_hash) - 1] = 0;
	ra_prefetch_gen_rows = ra_n_rows;

	ra_prefetch_rows = ra_n_rows;
	ra_prefetch_cursor = (ra_n_rows > 0) ? 0 : RA_PREFETCH_IDLE;
	ra_prefetch_wait = 0;
	ra_prefetch_issued = 0;

	RA_LOG("[RA] badge prefetch armed: %d rows x2 variants%s\n",
		ra_n_rows, force ? " (reconnect backfill)" : "");
}

/*
 * Called every frame from ra_view_tick(). Advances the warm-up walk by at most
 * one issued download, then sleeps RA_PREFETCH_ISSUE_GAP frames.
 *
 * Cursor layout is pass-major so the TOAST-critical variant goes first: pass 0
 * warms locked=0 (the unlocked artwork the toast draws) for every row, and only
 * then pass 1 warms locked=1 (the grey artwork the list draws for still-locked
 * rows, which the list already fetches lazily anyway).
 */
static void ra_view_prefetch_pump(void)
{
	int scanned = 0;

	if (ra_prefetch_cursor < 0 || ra_prefetch_rows <= 0)
		return;

	if (ra_prefetch_wait > 0) {
		ra_prefetch_wait--;
		return;
	}

	/* nothing to warm without a usable transport; the walk resumes when the
	 * link is back (and a reconnect rebuild re-arms it from the start anyway) */
	if (!ra_is_logged_in() || !ra_net_is_online())
		return;

	while (ra_prefetch_cursor < ra_prefetch_rows * 2) {
		int idx = ra_prefetch_cursor % ra_prefetch_rows;
		int pass = ra_prefetch_cursor / ra_prefetch_rows;
		const char *name;
		int issued;

		ra_prefetch_cursor++;

		/* the row list can shrink under us between rebuilds */
		if (idx >= ra_n_rows)
			continue;
		name = ra_rows[idx].badge_name;
		if (!name[0])
			continue;

		/* Both variants, per the report: locked=0 (what the toast draws) and
		 * locked=1 (what the list draws while locked). Written as two explicit
		 * literal calls rather than a computed argument so the intent -- and
		 * the constants -- stay visible in the source and the disassembly. */
		if (pass == 0)
			issued = ra_badge_prefetch(name, 0);
		else
			issued = ra_badge_prefetch(name, 1);

		if (issued) {
			ra_prefetch_issued++;
			ra_prefetch_wait = RA_PREFETCH_ISSUE_GAP;
			return;   /* one download in flight per gap: never floods the FIFO */
		}

		/* already cached: cheap, but still an sceIoOpen -- budget it */
		if (++scanned >= RA_PREFETCH_SCAN_MAX)
			return;
	}

	RA_LOG("[RA] badge prefetch done: %d rows, %u downloads issued\n",
		ra_prefetch_rows, ra_prefetch_issued);
	ra_prefetch_cursor = RA_PREFETCH_IDLE;
}

/* Rebuild the flattened row list from the live rc_client state. */
void ra_view_rebuild_list(void)
{
	rc_client_t *client = ra_get_client();
	uint32_t total_points = 0;
	uint32_t num_unlocked = 0;
	uint32_t num_total = 0;
	/* v18: sampled BEFORE the stand-down below, so the prefetch walk can tell
	 * an ordinary rebuild from the offline->online transition (reconnect). */
	int was_offline = ra_offline_display;

	ra_n_rows = 0;
	ra_row_sel = 0;
	ra_row_base = 0;

	if (!client || !rc_client_is_game_loaded(client))
		return;

	/* v15: past this point the rows come from a LIVE rc_client session, so the
	 * "Offline - showing cached data" banner (and the offline poll in
	 * ra_view_tick) must stand down. This is the reconnect completion of the
	 * Bug 3 fix: snapshot rows get replaced by real ones. */
	ra_offline_display = 0;

	rc_client_achievement_list_t *list = rc_client_create_achievement_list(client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list)
		return;

	for (uint32_t b = 0; b < list->num_buckets && ra_n_rows < RA_MAX_ROWS; b++) {
		const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
		for (uint32_t i = 0; i < bucket->num_achievements && ra_n_rows < RA_MAX_ROWS; i++) {
			const rc_client_achievement_t *ach = bucket->achievements[i];
			ra_view_row *row = &ra_rows[ra_n_rows++];
			memset(row, 0, sizeof(*row));

			if (ach->badge_name)
				strncpy(row->badge_name, ach->badge_name, sizeof(row->badge_name) - 1);
			else
				row->badge_name[0] = 0;
			if (ach->title)
				strncpy(row->title, ach->title, sizeof(row->title) - 1);
			if (ach->description)
				strncpy(row->description, ach->description, sizeof(row->description) - 1);
			row->id = ach->id;   /* v21 */
			row->points = ach->points;
			row->unlocked = ach->unlocked;

			snprintf(row->right_top, sizeof(row->right_top), "%u pts", ach->points);
			if (ach->unlocked && ach->unlock_time)
				ra_format_unlock_time((uint64_t)ach->unlock_time, row->right_bottom, sizeof(row->right_bottom));
			else if (ach->rarity > 0.0f)
				snprintf(row->right_bottom, sizeof(row->right_bottom), "Rarity %.1f%%", ach->rarity);

			total_points += ach->points;
			num_total++;
			if (ach->unlocked)
				num_unlocked++;
		}
	}

	rc_client_destroy_achievement_list(list);

	/* synthesized platinum row (mastery) when everything is earned */
	if (num_total > 0 && num_unlocked == num_total && ra_n_rows < RA_MAX_ROWS) {
		const rc_client_game_t *game = rc_client_get_game_info(client);

		memmove(&ra_rows[1], &ra_rows[0], ra_n_rows * sizeof(ra_view_row));
		ra_n_rows++;

		ra_view_row *row = &ra_rows[0];
		memset(row, 0, sizeof(*row));
		if (game && game->badge_name)
			strncpy(row->badge_name, game->badge_name, sizeof(row->badge_name) - 1);
		snprintf(row->title, sizeof(row->title), "Platinum");
		snprintf(row->description, sizeof(row->description), "All achievements earned!");
		row->points = total_points;
		row->unlocked = 1;
		row->platinum = 1;
		snprintf(row->right_top, sizeof(row->right_top), "%u pts", total_points);
		snprintf(row->right_bottom, sizeof(row->right_bottom), "%u/%u", num_unlocked, num_total);
	}

	/*
	 * v18: the rows are live and final for this rebuild -- warm both badge
	 * variants for all of them in the background (ra_view_prefetch_pump, driven
	 * from ra_view_tick). Latched, so the periodic refresh and the per-unlock
	 * rebuild do not restart the walk; forced when this rebuild IS the
	 * offline->online transition, which is the reconnect backfill.
	 */
	ra_view_prefetch_arm(was_offline);
}

/* Offline rows from the snapshot. */
static void ra_view_build_from_snapshot(void)
{
	int count = 0;
	const ra_snapshot_entry *entries = ra_snapshot_get_entries(&count);

	ra_n_rows = 0;
	ra_row_sel = 0;
	ra_row_base = 0;

	for (int i = 0; i < count && ra_n_rows < RA_MAX_ROWS; i++) {
		const ra_snapshot_entry *e = &entries[i];
		ra_view_row *row = &ra_rows[ra_n_rows++];
		memset(row, 0, sizeof(*row));

		strncpy(row->badge_name, e->badge_name, sizeof(row->badge_name) - 1);
		strncpy(row->title, e->title, sizeof(row->title) - 1);
		strncpy(row->description, e->description, sizeof(row->description) - 1);
		row->points = e->points;
		row->unlocked = e->unlocked;

		snprintf(row->right_top, sizeof(row->right_top), "%u pts", e->points);
		if (e->unlocked && e->unlock_time)
			ra_format_unlock_time(e->unlock_time, row->right_bottom, sizeof(row->right_bottom));
	}
}

/* ------------------------------------------------------------------------- */
/* v21 — row population for Screen B-remote (called from ra_gamelist.c)       */
/*                                                                            */
/* These four are the ONLY way ra_gamelist.c touches the grid's backing store, */
/* which stays static to this file. Same array the CURRENT screen draws, and   */
/* that is safe here because every other writer is unreachable while DETAIL is */
/* on screen: ra_view_rebuild_list() returns early without a loaded rc_client  */
/* game, the 60-tick refresh is gated on ra_is_game_loaded(), and the unlock   */
/* event handler needs a live session. Returning to LIST or CURRENT rebuilds   */
/* the rows from their own source anyway.                                     */
/* ------------------------------------------------------------------------- */

void ra_view_detail_begin(void)
{
	ra_n_rows = 0;
	ra_row_sel = 0;
	ra_row_base = 0;
}

/* Returns 1 when the row was stored, 0 when the array is full. */
int ra_view_detail_add_row(uint32_t id, const char *badge_name, const char *title,
	const char *description, uint32_t points, float rarity)
{
	ra_view_row *row;

	if (ra_n_rows >= RA_MAX_ROWS)
		return 0;

	row = &ra_rows[ra_n_rows++];
	memset(row, 0, sizeof(*row));

	row->id = id;
	if (badge_name)
		strncpy(row->badge_name, badge_name, sizeof(row->badge_name) - 1);
	if (title)
		strncpy(row->title, title, sizeof(row->title) - 1);
	if (description)
		strncpy(row->description, description, sizeof(row->description) - 1);
	row->points = points;
	row->unlocked = 0;   /* ra_view_detail_mark_unlocked() flips these */

	snprintf(row->right_top, sizeof(row->right_top), "%u pts", points);
	/*
	 * Rarity, not an unlock date. The token-only fetch_user_unlocks endpoint
	 * returns achievement IDs and nothing else -- no timestamps exist to show
	 * for a game that is not running. The CURRENT screen already falls back to
	 * exactly this string for its locked rows, so the grammar is consistent.
	 */
	if (rarity > 0.0f)
		snprintf(row->right_bottom, sizeof(row->right_bottom), "Rarity %.1f%%", (double)rarity);

	return 1;
}

void ra_view_detail_mark_unlocked(uint32_t id)
{
	int i;

	if (id == 0)
		return;
	for (i = 0; i < ra_n_rows; i++) {
		if (ra_rows[i].id == id) {
			ra_rows[i].unlocked = 1;
			return;
		}
	}
}

/*
 * Called once the unlock pass is done: synthesize the mastery row when the set
 * is complete (same block as ra_view_rebuild_list) and warm this game's badges
 * through the v18 prefetch walk.
 */
void ra_view_detail_finish(const char *game_icon_name)
{
	uint32_t total_points = 0;
	int num_unlocked = 0;
	int i;

	for (i = 0; i < ra_n_rows; i++) {
		total_points += ra_rows[i].points;
		if (ra_rows[i].unlocked)
			num_unlocked++;
	}

	if (ra_n_rows > 0 && num_unlocked == ra_n_rows && ra_n_rows < RA_MAX_ROWS) {
		ra_view_row *row;

		memmove(&ra_rows[1], &ra_rows[0], ra_n_rows * sizeof(ra_view_row));
		ra_n_rows++;

		row = &ra_rows[0];
		memset(row, 0, sizeof(*row));
		if (game_icon_name)
			strncpy(row->badge_name, game_icon_name, sizeof(row->badge_name) - 1);
		snprintf(row->title, sizeof(row->title), "Platinum");
		snprintf(row->description, sizeof(row->description), "All achievements earned!");
		row->points = total_points;
		row->unlocked = 1;
		row->platinum = 1;
		snprintf(row->right_top, sizeof(row->right_top), "%u pts", total_points);
		snprintf(row->right_bottom, sizeof(row->right_bottom), "%d/%d", num_unlocked, ra_n_rows - 1);
	}

	/*
	 * FORCED arm. ra_view_prefetch_arm's no-force key is (game hash, row
	 * count) -- and on this screen ra_get_game_hash() is the booted game's
	 * hash (usually empty here) which does NOT change when the user picks a
	 * different collection game. Forcing is the plan's own stated fallback
	 * (§5.2) and is correct: each DETAIL open is a genuinely new badge set.
	 */
	ra_view_prefetch_arm(1);
}

/* ------------------------------------------------------------------------- */
/* Inline login (IME)                                                         */
/* ------------------------------------------------------------------------- */

enum ra_login_step {
	RA_LOGIN_IDLE = 0,
	RA_LOGIN_USERNAME,
	RA_LOGIN_PASSWORD,
};

/* v6 password-buffer fix (v5-login-crash-debug.md WARNING #3): the password
 * IME used to allow 255 chars into char[64] buffers -- anything past 63
 * chars was silently truncated into a WRONG password by ra_utf16_to_utf8.
 * Chosen fix: cap the IME at 63 characters (63 + NUL fits both 64-byte
 * buffers exactly) rather than growing the buffers, because RA passwords are
 * short, the buffers feed fixed-size rc_client login calls, and a capped
 * input is simpler to audit than a bigger one. */
#define RA_LOGIN_IME_MAX_LEN 63

static int ra_login_step = RA_LOGIN_IDLE;
static int ra_ime_active = 0;
static SceWChar16 ra_ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static SceWChar16 ra_ime_initial[256];
static SceWChar16 ra_ime_buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static char ra_login_username[64];
static char ra_login_password[64];
static uint32_t ra_login_max_len = RA_LOGIN_IME_MAX_LEN;

static void ra_utf8_to_utf16(const char *src, SceWChar16 *dst, int max)
{
	int i = 0;
	while (*src && i < max - 1) {
		unsigned char c = (unsigned char)*src++;
		if (c < 0x80) {
			dst[i++] = c;
		} else if ((c & 0xE0) == 0xC0 && (*src & 0xC0) == 0x80) {
			dst[i++] = ((c & 0x1F) << 6) | (*src++ & 0x3F);
		} else if ((c & 0xF0) == 0xE0 && (*src & 0xC0) == 0x80 && (src[1] & 0xC0) == 0x80) {
			dst[i++] = ((c & 0x0F) << 12) | ((*src & 0x3F) << 6) | (src[1] & 0x3F);
			src += 2;
		} else {
			src++; /* skip malformed sequence */
		}
	}
	dst[i] = 0;
}

static void ra_utf16_to_utf8(const SceWChar16 *src, char *dst, int max)
{
	int i = 0;
	while (*src && i < max - 1) {
		uint32_t c = *src++;
		if (c < 0x80) {
			dst[i++] = c;
		} else if (c < 0x800) {
			if (i + 2 > max - 1)
				break;
			dst[i++] = 0xC0 | (c >> 6);
			dst[i++] = 0x80 | (c & 0x3F);
		} else {
			if (i + 3 > max - 1)
				break;
			dst[i++] = 0xE0 | (c >> 12);
			dst[i++] = 0x80 | ((c >> 6) & 0x3F);
			dst[i++] = 0x80 | (c & 0x3F);
		}
	}
	dst[i] = 0;
}

static int ra_start_ime(const char *title, const char *initial, int password_mode, uint32_t max_len)
{
	SceImeDialogParam param;

	/*
	 * v26 — v25-login-crash.md hardening. sceImeDialogInit was first reached in
	 * v25 and crashed (C2-12828-1). The one identifiable setup gap vs the SDK's
	 * own usage contract (ime_dialog.h usage tag: "SceIme_stub,
	 * SCE_SYSMODULE_IME") is that SCE_SYSMODULE_IME is never loaded -- the app
	 * imports SceIme weak because it may not be resident in ScePspemu. Load it
	 * here (idempotent, best-effort, NON-fatal on failure so firmwares where
	 * init works without it are not regressed), then log around the init so a
	 * hardware run pins any crash to init vs the composite/poll path below.
	 * Only ASCII literals are logged here -- never buffer or credential bytes.
	 */
	int ime_mod = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
	RA_LOG("[RA] v26 ime: SCE_SYSMODULE_IME load = 0x%08X\n", ime_mod);

	sceImeDialogParamInit(&param);

	ra_utf8_to_utf16(title, ra_ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
	if (initial && initial[0])
		ra_utf8_to_utf16(initial, ra_ime_initial, sizeof(ra_ime_initial) / sizeof(SceWChar16));

	memset(ra_ime_buffer, 0, sizeof(ra_ime_buffer));

	param.inputMethod = 0;
	param.supportedLanguages = 0;
	param.languagesForced = 0;
	param.type = SCE_IME_TYPE_BASIC_LATIN;
	param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
	param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	param.textBoxMode = password_mode ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	param.title = ra_ime_title;
	param.maxTextLength = max_len;
	param.initialText = (initial && initial[0]) ? ra_ime_initial : NULL;
	param.inputTextBuffer = ra_ime_buffer;
	param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;

	RA_LOG("[RA] v26 ime: calling sceImeDialogInit (title='%s')\n", title);
	int ime_ret = sceImeDialogInit(&param);
	RA_LOG("[RA] v26 ime: sceImeDialogInit returned 0x%08X\n", ime_ret);
	if (ime_ret >= 0) {
		ra_ime_active = 1;
		return 0;
	}

	RA_LOG("[RA] v26 ime: sceImeDialogInit failed 0x%08X\n", ime_ret);
	return -1;
}

static void ra_login_poll_ime(void)
{
	SceCommonDialogStatus status;
	SceImeDialogResult result;

	if (!ra_ime_active)
		return;

	status = sceImeDialogGetStatus();
	if (status == SCE_COMMON_DIALOG_STATUS_NONE) {
		/* v26: init "succeeded" but the dialog never actually came up -- do not
		 * wedge ra_ime_active, or every later login attempt is a silent no-op
		 * (v25-login-crash.md). */
		RA_LOG("[RA] v26 ime: status NONE, cleaning up\n");
		sceImeDialogTerm();
		ra_ime_active = 0;
		ra_login_step = RA_LOGIN_IDLE;
		return;
	}
	if (status != SCE_COMMON_DIALOG_STATUS_FINISHED)
		return;

	memset(&result, 0, sizeof(result));
	sceImeDialogGetResult(&result);

	if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
		if (ra_login_step == RA_LOGIN_USERNAME) {
			ra_utf16_to_utf8(ra_ime_buffer, ra_login_username, sizeof(ra_login_username));
			ra_login_step = RA_LOGIN_PASSWORD;
			sceImeDialogTerm();
			ra_ime_active = 0;
			/* v6: cap at RA_LOGIN_IME_MAX_LEN (63) to match the 64-byte
			 * ra_login_password buffer; was 255 -> silent truncation */
			ra_start_ime("RetroAchievements Password", "", 1, RA_LOGIN_IME_MAX_LEN);
			return;
		} else if (ra_login_step == RA_LOGIN_PASSWORD) {
			ra_utf16_to_utf8(ra_ime_buffer, ra_login_password, sizeof(ra_login_password));
			sceImeDialogTerm();
			ra_ime_active = 0;

			if (ra_login_username[0] && ra_login_password[0]) {
				ra_begin_login(ra_login_username, ra_login_password);
			}

			/* never keep the password around after handing it off */
			memset(ra_login_password, 0, sizeof(ra_login_password));
			ra_login_step = RA_LOGIN_IDLE;
			return;
		}
	}

	/* closed/cancelled */
	sceImeDialogTerm();
	ra_ime_active = 0;
	ra_login_step = RA_LOGIN_IDLE;
}

/* non-zero while the inline login IME dialog is up; the draw loop then
 * composites the dialog (vita2d_common_dialog_update) instead of skipping
 * the frame like it does for other common dialogs */
int ra_is_ime_active(void)
{
	return ra_ime_active;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle hooks                                                            */
/* ------------------------------------------------------------------------- */

void ra_view_opened(void)
{
	ra_view_open = 1;

	/*
	 * v21: ALWAYS start on the booted-game screen. The collection is entered
	 * only through ra_view_no_game_running(), i.e. only after the hash job has
	 * reported that nothing hashable is running. This single line is what
	 * guarantees a booted PSP game never sees the new code paths.
	 */
	ra_screen = RA_SCREEN_CURRENT;

	/*
	 * v9: the curl+OpenSSL TLS smoke test that used to fire here has been
	 * RETIRED, together with ra/ra_smoke.[ch] and ra/ra_smoke_ca.h.
	 *
	 * It had answered its question on hardware (socket / TLS / certificate
	 * verification against the embedded PEM), and that answer now ships as the
	 * production transport: ra_net.c performs every request through curl +
	 * OpenSSL with CURLOPT_CAINFO_BLOB over the same bundle, which moved to
	 * ra/ra_ca.h with a byte-identical PEM payload.
	 *
	 * Two concrete reasons it had to go rather than merely be left in place:
	 *   - curl_global_init() is not thread-safe. The smoke thread and the RA
	 *     net worker would both have raced into it on menu-open, since both
	 *     are reached from this same deferred path. ra_net.c now owns the one
	 *     and only call, on the render thread, before the worker exists.
	 *   - the real login path is the better diagnostic anyway: its [RA] curl
	 *     OK/FAIL lines report the same curl code, HTTP status and TLS
	 *     handshake time, for the request that actually matters.
	 */

	if (ra_is_logged_in()) {
		ra_offline_display = 0;
		if (ra_is_game_loaded()) {
			/* show what we already have immediately... */
			ra_view_rebuild_list();
			/* ...and, if the menu has been closed since that game was
			 * identified, re-identify what is running NOW in the background
			 * (v15 Bug 2: the old game's list used to be shown forever). The
			 * rows are replaced by ra_load_game_callback if the hash changed. */
			if (ra_needs_game_revalidate())
				ra_request_game_load();
		} else {
			/*
			 * v26 — cached-rows-first while the live identify+load chain runs
			 * (trophy-menu-delay.md Fix 1). Mirror the sibling branches below
			 * (login-in-progress, signed-out): if a previous identification
			 * left a hash for the running game and a snapshot exists on disk,
			 * show it NOW instead of a blank panel while the live chain
			 * resolves. ra_load_game_callback() replaces these rows when the
			 * load lands. Deliberately does NOT set ra_offline_display: these
			 * are a warm-up, not the "offline" state, so no orange banner.
			 */
			const char *hash = ra_get_game_hash();
			if (hash && ra_snapshot_load(hash) > 0)
				ra_view_build_from_snapshot();
			ra_request_game_load();
		}
		return;
	}

	/*
	 * v24 — THE OFFLINE-DISPLAY RACE (autologin-tracking-fix-scope.md,
	 * CRITICAL-2 / E8).
	 *
	 * ra_is_logged_in() above is a synchronous read of a flag that an
	 * ASYNCHRONOUS login sets. On the first frame the trophy menu opens, a
	 * login that was kicked off at boot with a perfectly valid token may still
	 * be in flight -- so the read returns 0 and everything below paints the
	 * signed-out/offline presentation over a session that is healthy and about
	 * to succeed. The on-device log for exactly such a session shows
	 * "login result=0", trophies loading, and http=200 throughout, with no
	 * "offline" line anywhere: the banner was generated purely here, by the UI
	 * reading the answer too early.
	 *
	 * "A login is in flight" is not "offline", so do not say offline. Cached
	 * rows are still shown if a snapshot exists -- something on screen beats an
	 * empty panel while the handshake finishes -- but WITHOUT setting
	 * ra_offline_display, so the orange "Offline - showing cached data" banner
	 * stays off and the login row renders as "Signing in..." (see
	 * ra_draw_current_game). ra_view_tick() promotes this to the real thing the
	 * moment login lands.
	 */
	if (ra_is_login_in_progress() || ra_is_login_pending()) {
		ra_offline_display = 0;
		ra_n_rows = 0;

		const char *pending_hash = ra_get_game_hash();
		if (pending_hash && ra_snapshot_load(pending_hash) > 0)
			ra_view_build_from_snapshot();

		return;
	}

	/* not logged in: the draw path shows the inline login prompt; also try an
	 * offline snapshot display if a previous hash is known */
	ra_offline_display = 0;
	ra_n_rows = 0;

	const char *hash = ra_get_game_hash();
	if (hash && ra_snapshot_load(hash) > 0) {
		ra_view_build_from_snapshot();
		ra_offline_display = 1;
		return;
	}

	/*
	 * v21 (plan §1.2, offline tail): signed out means the hash-failure funnel
	 * can never fire (login needs the network), so the LIST screen would be
	 * unreachable offline. If a previous online session left a collection
	 * cache on disk, show THAT instead of an empty login screen -- with the
	 * existing orange "Offline - showing cached data" banner, since these rows
	 * are by definition stale.
	 */
	if (ra_gamelist_load_cache() > 0) {
		ra_screen = RA_SCREEN_LIST;
		ra_game_sel = 0;
		ra_game_base = 0;
		ra_offline_display = 1;
	}
}

/*
 * v21 — the single funnel into the collection (plan §1.2).
 *
 * Called from ra_client_hash_done()'s !hash_ok branch, on the render thread,
 * for the NO_GAME and POPS_GAME reasons. That branch is the one place in the
 * system that learns there is nothing (PSP-)bootable running, and it is reached
 * identically by every path into the empty state: XMB-open while logged in,
 * XMB-open then inline login, auto-login with a saved token, and POPS.
 */
void ra_view_no_game_running(void)
{
	if (!ra_view_open || !ra_is_logged_in())
		return;                       /* closed, or the login prompt owns the screen */
	if (ra_screen == RA_SCREEN_DETAIL)
		return;                       /* user is browsing: do not yank the screen */

	if (ra_screen != RA_SCREEN_LIST) {
		ra_screen = RA_SCREEN_LIST;
		ra_game_sel = 0;
		ra_game_base = 0;
		RA_LOG("[RA] no game running -> game collection\n");
	}
	ra_gamelist_request();
}

/*
 * v21 — O/back gate for menu.c (plan §1.3).
 *
 * Returns 1 only when it consumed the press: in DETAIL, O pops back to the
 * collection with the previous selection intact. Everywhere else it returns 0
 * and O keeps today's meaning (close the menu), including from LIST, which is
 * the PS3 behaviour: back out of the collection to the XMB.
 *
 * Gating on ra_view_is_open() is sound: menu.c's tab tracker calls
 * ra_view_opened()/ra_view_closed() on EVERY tab enter/leave, including the
 * L/R switches (menu.c:472-486, inside the same !open_options block), so
 * ra_view_open is false whenever another tab owns input. Plan Risk §9.8
 * checked and cleared.
 */
int ra_view_handle_cancel(void)
{
	if (!ra_view_open || ra_screen != RA_SCREEN_DETAIL)
		return 0;

	ra_screen = RA_SCREEN_LIST;
	ra_detail_game_id = 0;
	ra_n_rows = 0;          /* the detail rows belong to the game we just left */
	ra_row_sel = 0;
	ra_row_base = 0;
	return 1;
}

void ra_view_closed(void)
{
	ra_view_open = 0;

	/* abort any in-flight login dialog when leaving the tab */
	if (ra_ime_active) {
		sceImeDialogAbort();
		sceImeDialogTerm();
		ra_ime_active = 0;
		ra_login_step = RA_LOGIN_IDLE;
	}
}

int ra_view_is_open(void)
{
	return ra_view_open;
}

void ra_request_open_trophies(void)
{
	ra_trophy_open_requested = 1;
}

/* ------------------------------------------------------------------------- */
/* Draw                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * v25 — button glyphs that match the console, not the developer's console.
 *
 * Every prompt in this file used to hardcode "X" for confirm and "O" for
 * cancel. That is only correct on a Cross-confirm system. utils.c:175-203
 * remaps PAD_ENTER/PAD_CANCEL onto PAD_CIRCLE/PAD_CROSS (or the reverse)
 * according to the console's SCE_SYSTEM_PARAM_ID_ENTER_BUTTON setting, which
 * menu.c:585 reads into `enter_button` (declared menu.h:102) -- so on a
 * Circle-confirm Vita the input layer does the right thing while the on-screen
 * text told the user to press the button that actually cancels. That mismatch
 * is what produced the "the prompt says press X and X does nothing" report.
 *
 * These two are the single source of truth for printed button names; they read
 * the same `enter_button` the remap reads, so the label can never disagree with
 * the binding again.
 */
static const char *ra_confirm_glyph(void)
{
	return (enter_button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "O" : "X";
}

static const char *ra_cancel_glyph(void)
{
	return (enter_button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "X" : "O";
}

/*
 * v25 — the always-available Login/Logout hint (see ra_ctrl_trophy_tab()).
 * Triangle is a fixed physical button and is never remapped by enter_button,
 * so its own name needs no substitution; only the confirm/cancel glyphs printed
 * next to it do.
 */
static const char *ra_login_hint(void)
{
	return ra_is_logged_in() ? "/\\: Logout" : "/\\: Login";
}

static void ra_draw_header(float y)
{
	if (ra_is_logged_in()) {
		const char *welcome = ra_get_welcome_message();
		if (welcome)
			pgf_draw_text(WINDOW_X + 10.0f, y, GREEN, FONT_SIZE, welcome);
	} else {
		char prompt[96];

		pgf_draw_text(WINDOW_X + 10.0f, y, WHITE, FONT_SIZE, "Not logged in to RetroAchievements.");
		/* v25: the glyph now follows the console's enter-button setting, and
		 * the Triangle hotkey is named here because it is the one route to
		 * login that works from EVERY screen -- including a zero-row DETAIL,
		 * where there is no "Login" row "below" to press anything on. */
		snprintf(prompt, sizeof(prompt),
			"Press %s on \"Login\" below, or /\\ (Triangle) from anywhere.",
			ra_confirm_glyph());
		pgf_draw_text(WINDOW_X + 10.0f, y + FONT_Y_SPACE, ORANGE, FONT_SIZE, prompt);
	}
}

static void ra_draw_badge(const char *badge_name, int locked, float x, float y)
{
	void *tex = ra_badge_get(badge_name, locked);
	if (tex) {
		float w = (float)vita2d_texture_get_width((vita2d_texture *)tex);
		float h = (float)vita2d_texture_get_height((vita2d_texture *)tex);
		float scale = (w > 0.0f && h > 0.0f) ? (48.0f / ((w > h) ? w : h)) : 1.0f;
		vita2d_draw_texture_scale((vita2d_texture *)tex, x, y, scale, scale);
	} else {
		/* placeholder while pending */
		vita2d_draw_rectangle(x, y, 48.0f, 48.0f, COLOR_ALPHA(GRAY, 0x5F));
		vita2d_draw_fill_circle(x + 24.0f, y + 24.0f, 10.0f, COLOR_ALPHA(DARKGRAY, 0x9F));
	}
}

/*
 * v21: game box art. Sibling of ra_draw_badge above, differing only in the
 * cache variant it asks for -- same 48px fit-scale, same grey placeholder while
 * the fetch is in flight, same GXM-fenced upload window. Lazy and draw-driven:
 * with RA_ROWS_VISIBLE (6) rows on screen at most 6 icons are ever requested,
 * and ra_badge_get's FETCHING negative-cache stops a miss from re-enqueuing, so
 * this cannot flood the 16-deep work FIFO.
 */
static void ra_draw_game_icon(const char *icon_name, float x, float y)
{
	void *tex = ra_badge_get(icon_name, RA_IMG_GAME_ICON);
	if (tex) {
		float w = (float)vita2d_texture_get_width((vita2d_texture *)tex);
		float h = (float)vita2d_texture_get_height((vita2d_texture *)tex);
		float scale = (w > 0.0f && h > 0.0f) ? (48.0f / ((w > h) ? w : h)) : 1.0f;
		vita2d_draw_texture_scale((vita2d_texture *)tex, x, y, scale, scale);
	} else {
		vita2d_draw_rectangle(x, y, 48.0f, 48.0f, COLOR_ALPHA(GRAY, 0x5F));
		vita2d_draw_fill_circle(x + 24.0f, y + 24.0f, 10.0f, COLOR_ALPHA(DARKGRAY, 0x9F));
	}
}

/*
 * v21: the achievement row loop, factored verbatim out of ra_draw_trophy_tab so
 * the CURRENT screen and the remote DETAIL screen render identically. The only
 * parameter DETAIL changes is login_offset, which is always 0 there (that
 * screen is unreachable signed out).
 */
static void ra_draw_rows(float rows_top, int login_offset)
{
	int first_visible = ra_row_base;

	for (int i = 0; i < RA_ROWS_VISIBLE && first_visible + i < ra_n_rows; i++) {
		int idx = first_visible + i;
		ra_view_row *row = &ra_rows[idx];
		float ry = rows_top + i * RA_ROW_HEIGHT;
		int selected = (idx + login_offset) == ra_row_sel;

		if (selected)
			vita2d_draw_rectangle(WINDOW_X, ry, WINDOW_WIDTH, RA_ROW_HEIGHT - 4.0f, COLOR_ALPHA(0xFFFF1F7F, 0x5F));

		/* badge */
		ra_draw_badge(row->badge_name, !row->unlocked, WINDOW_X + 10.0f, ry + 2.0f);

		/* grade glyph */
		uint32_t grade_color = row->platinum ? RA_GRADE_PLATINUM : ra_grade_color(row->points);
		vita2d_draw_fill_circle(WINDOW_X + 72.0f, ry + RA_ROW_HEIGHT / 2.0f, 7.0f, grade_color);

		/* title + description */
		uint32_t title_color = row->unlocked ? WHITE : GRAY;
		pgf_draw_text(WINDOW_X + 90.0f, ry + 6.0f, title_color, FONT_SIZE, row->title);
		pgf_draw_text(WINDOW_X + 90.0f, ry + 6.0f + FONT_Y_SPACE * 0.9f, COLOR_ALPHA(GRAY, 0xCF), FONT_SIZE * 0.8f, row->description);

		/* right-aligned points + unlock date/rarity */
		float right_x = WINDOW_X + WINDOW_WIDTH - 12.0f;
		pgf_draw_text(ALIGN_RIGHT(right_x, vita2d_pgf_text_width(font, FONT_SIZE, row->right_top)),
			ry + 6.0f, row->unlocked ? GREEN : WHITE, FONT_SIZE, row->right_top);
		if (row->right_bottom[0])
			pgf_draw_text(ALIGN_RIGHT(right_x, vita2d_pgf_text_width(font, FONT_SIZE * 0.8f, row->right_bottom)),
				ry + 6.0f + FONT_Y_SPACE * 0.9f, GRAY, FONT_SIZE * 0.8f, row->right_bottom);
	}

	/* scroll indicators */
	if (ra_row_base > 0)
		pgf_draw_text(WINDOW_X + WINDOW_WIDTH / 2.0f - 4.0f, rows_top - FONT_Y_SPACE, WHITE, FONT_SIZE, "^");
	if (ra_row_base + RA_ROWS_VISIBLE < ra_n_rows)
		pgf_draw_text(WINDOW_X + WINDOW_WIDTH / 2.0f - 4.0f, rows_top + RA_ROWS_VISIBLE * RA_ROW_HEIGHT - 6.0f, WHITE, FONT_SIZE, "v");
}

/* --- Screen A: the collection (plan §4) ---------------------------------- */

static void ra_draw_game_list(void)
{
	float y = FONT_Y_LINE(2);
	int n_games = 0;
	const ra_game_row *games = ra_gamelist_rows(&n_games);
	int state = ra_gamelist_state();
	float rows_top;

	ra_draw_header(y);
	y += FONT_Y_SPACE + 6.0f;

	pgf_draw_text(WINDOW_X + 10.0f, y, WHITE, FONT_SIZE, "Trophy Collection - PSP");
	y += FONT_Y_SPACE + 4.0f;

	if (ra_offline_display || ra_gamelist_is_cached_only()) {
		pgf_draw_text(WINDOW_X + 10.0f, y, ORANGE, FONT_SIZE, "Offline - showing cached data");
		y += FONT_Y_SPACE;
	}

	rows_top = y;

	for (int i = 0; i < RA_ROWS_VISIBLE && ra_game_base + i < n_games; i++) {
		int idx = ra_game_base + i;
		const ra_game_row *g = &games[idx];
		float ry = rows_top + i * RA_ROW_HEIGHT;
		float right_x = WINDOW_X + WINDOW_WIDTH - 12.0f;
		char counts[32];
		char pct[16];

		if (idx == ra_game_sel)
			vita2d_draw_rectangle(WINDOW_X, ry, WINDOW_WIDTH, RA_ROW_HEIGHT - 4.0f, COLOR_ALPHA(0xFFFF1F7F, 0x5F));

		ra_draw_game_icon(g->icon_name, WINDOW_X + 10.0f, ry + 2.0f);

		/* WHITE when the user has earned something here, GRAY otherwise --
		 * the same colour language the achievement rows use */
		pgf_draw_text(WINDOW_X + 90.0f, ry + 6.0f,
			(g->num_unlocked > 0) ? WHITE : GRAY, FONT_SIZE, g->title);

		snprintf(counts, sizeof(counts), "%u / %u", g->num_unlocked, g->num_achievements);
		pgf_draw_text(ALIGN_RIGHT(right_x, vita2d_pgf_text_width(font, FONT_SIZE, counts)),
			ry + 6.0f, WHITE, FONT_SIZE, counts);

		if (g->num_achievements > 0) {
			snprintf(pct, sizeof(pct), "%u%%",
				(unsigned)((g->num_unlocked * 100u) / g->num_achievements));
			pgf_draw_text(ALIGN_RIGHT(right_x, vita2d_pgf_text_width(font, FONT_SIZE * 0.8f, pct)),
				ry + 6.0f + FONT_Y_SPACE * 0.9f, GRAY, FONT_SIZE * 0.8f, pct);
		}
	}

	/* scroll indicators (same glyph grammar as the achievement grid) */
	if (ra_game_base > 0)
		pgf_draw_text(WINDOW_X + WINDOW_WIDTH / 2.0f - 4.0f, rows_top - FONT_Y_SPACE, WHITE, FONT_SIZE, "^");
	if (ra_game_base + RA_ROWS_VISIBLE < n_games)
		pgf_draw_text(WINDOW_X + WINDOW_WIDTH / 2.0f - 4.0f, rows_top + RA_ROWS_VISIBLE * RA_ROW_HEIGHT - 6.0f, WHITE, FONT_SIZE, "v");

	/* state lines, where the empty-grid message goes on the CURRENT screen */
	if (n_games == 0) {
		const char *msg;
		uint32_t col = GRAY;

		if (ra_gamelist_is_fetching() || state == RA_GL_PROGRESS || state == RA_GL_TITLES) {
			msg = "Fetching your game list...";
		} else if (state == RA_GL_ERROR) {
			msg = ra_gamelist_error();
			if (!msg)
				msg = "Could not load your game list.";
			col = ORANGE;
		} else {
			msg = "No PSP games with achievements yet - play one!";
		}
		pgf_draw_text(WINDOW_X + 10.0f, rows_top, col, FONT_SIZE, msg);
	} else if (ra_gamelist_is_fetching()) {
		/* refreshing over a cached list: say so without hiding the rows */
		pgf_draw_text(WINDOW_X + 10.0f, FONT_Y_LINE(16), GRAY, FONT_SIZE, "Refreshing...");
	}

	/* v25: glyphs follow the console's enter-button setting, and the Triangle
	 * Login/Logout hotkey is advertised on every screen. */
	char footer[96];
	snprintf(footer, sizeof(footer), "%s: view trophies    %s: close    %s",
		ra_confirm_glyph(), ra_cancel_glyph(), ra_login_hint());
	pgf_draw_text(WINDOW_X + 10.0f, FONT_Y_LINE(17), GRAY, FONT_SIZE, footer);
}

/* --- Screen B-remote: one collection game, read-only (plan §5.3) ---------- */

static void ra_draw_game_detail(void)
{
	float y = FONT_Y_LINE(2);
	int state = ra_detail_state();
	int unlocked = 0;
	float rows_top;
	char counts[32];

	/* header: the game's own art + title + progress */
	ra_draw_game_icon(ra_detail_icon, WINDOW_X + 10.0f, y - 12.0f);
	pgf_draw_text(WINDOW_X + 70.0f, y, WHITE, FONT_SIZE, ra_detail_title);

	for (int i = 0; i < ra_n_rows; i++)
		if (ra_rows[i].unlocked && !ra_rows[i].platinum)
			unlocked++;
	snprintf(counts, sizeof(counts), "%d / %d", unlocked,
		ra_n_rows - (ra_n_rows > 0 && ra_rows[0].platinum ? 1 : 0));
	pgf_draw_text(ALIGN_RIGHT(WINDOW_X + WINDOW_WIDTH - 12.0f,
			vita2d_pgf_text_width(font, FONT_SIZE, counts)),
		y, WHITE, FONT_SIZE, counts);

	y += FONT_Y_SPACE + 24.0f;
	rows_top = y;

	ra_draw_rows(rows_top, 0);   /* login_offset is always 0 here */

	if (ra_n_rows == 0) {
		const char *msg;
		uint32_t col = GRAY;

		if (state == RA_DT_GAMEDATA || state == RA_DT_UNLOCKS) {
			msg = "Loading achievements...";
		} else if (state == RA_DT_ERROR) {
			msg = ra_detail_error();
			if (!msg)
				msg = "Could not load this game's achievements.";
			col = ORANGE;
		} else {
			msg = "No achievements found for this game.";
		}
		pgf_draw_text(WINDOW_X + 10.0f, rows_top, col, FONT_SIZE, msg);
	}

	/*
	 * The honest limitation, stated in-product: this grid is a VIEW. It is
	 * built from the definitions + the user's unlock IDs, which is all the
	 * token-only endpoints expose for a game that is not running -- no unlock
	 * timestamps exist to show, and nothing can be earned from here.
	 */
	pgf_draw_text(WINDOW_X + 10.0f, FONT_Y_LINE(16), COLOR_ALPHA(GRAY, 0xCF), FONT_SIZE * 0.85f,
		"Viewing only - play the game to earn trophies");
	/* v25: the cancel glyph is now correct for the console, and the Triangle
	 * hotkey is named HERE above all -- this is the screen a signed-out user
	 * gets stuck on, with zero rows and nothing else to press. */
	char footer[96];
	snprintf(footer, sizeof(footer), "%s: back to collection    %s",
		ra_cancel_glyph(), ra_login_hint());
	pgf_draw_text(WINDOW_X + 10.0f, FONT_Y_LINE(17), GRAY, FONT_SIZE, footer);
}

/* --- the CURRENT screen: unchanged behaviour ------------------------------ */

static void ra_draw_current_game(void)
{
	float y = FONT_Y_LINE(2);

	ra_draw_header(y);
	y += FONT_Y_SPACE * (ra_is_logged_in() ? 1 : 2) + 6.0f;

	/* login row when signed out */
	if (!ra_is_logged_in()) {
		if (ra_row_sel == 0)
			vita2d_draw_rectangle(WINDOW_X, y - 2.0f, WINDOW_WIDTH, FONT_Y_SPACE + 6.0f, COLOR_ALPHA(0xFFFF1F7F, 0x8F));

		/*
		 * v24: three states, not two. "Signing in..." now also covers the
		 * window where a boot-time auto-login is queued but has not been
		 * issued yet -- v23 rendered that as a green, actionable
		 * "Login to RetroAchievements", i.e. it told the user they were
		 * signed out while the client was in the middle of signing them in.
		 *
		 * A stale/rejected token is the one signed-out state that is NOT
		 * self-correcting, so it gets its own call to action.
		 */
		const int login_busy = ra_is_login_in_progress() || ra_is_login_pending();
		const char *label;
		char stale_label[80];

		if (login_busy) {
			label = "Signing in...";
		} else if (ra_is_login_stale()) {
			/* v25: was hardcoded "press X"; on a Circle-confirm console X is
			 * the CANCEL button, so this instruction was actively wrong. */
			snprintf(stale_label, sizeof(stale_label),
				"Saved login expired - press %s to sign in again", ra_confirm_glyph());
			label = stale_label;
		} else {
			label = "Login to RetroAchievements";
		}

		pgf_draw_text(WINDOW_X + 10.0f, y, login_busy ? GRAY : GREEN, FONT_SIZE, label);
		y += FONT_Y_SPACE + 10.0f;
	}

	if (ra_offline_display) {
		pgf_draw_text(WINDOW_X + 10.0f, y, ORANGE, FONT_SIZE, "Offline - showing cached data");
		y += FONT_Y_SPACE;
	}

	/* trophy rows */
	float rows_top = y;
	int login_offset = ra_is_logged_in() ? 0 : 1; /* row 0 = login when signed out */

	ra_draw_rows(rows_top, login_offset);

	if (ra_n_rows == 0 && ra_is_logged_in()) {
		pgf_draw_text(WINDOW_X + 10.0f, rows_top, GRAY, FONT_SIZE, ra_get_status_message());
	}

	/* status line */
	const char *status = ra_get_status_message();
	if (status)
		pgf_draw_text(WINDOW_X + 10.0f, FONT_Y_LINE(17), GRAY, FONT_SIZE, status);

	/* v25: the Triangle Login/Logout hint, on its own line above the status so
	 * it does not fight the (variable-length) status text for room. Drawn on
	 * all three screens; this is the CURRENT screen's copy. */
	pgf_draw_text(ALIGN_RIGHT(WINDOW_X + WINDOW_WIDTH - 12.0f,
			vita2d_pgf_text_width(font, FONT_SIZE, ra_login_hint())),
		FONT_Y_LINE(16), GRAY, FONT_SIZE, ra_login_hint());
}

/* v21: 3-way dispatcher. menu.c is unchanged -- it still just calls this. */
void ra_draw_trophy_tab(void)
{
	switch (ra_screen) {
	case RA_SCREEN_LIST:
		ra_draw_game_list();
		break;
	case RA_SCREEN_DETAIL:
		ra_draw_game_detail();
		break;
	default:
		ra_draw_current_game();
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * v21 Screen A input. Same hold_pad + analog pattern and the same visible-window
 * clamp arithmetic as the achievement grid below, minus the login-offset
 * complication: the collection is only reachable logged in (funnel) or from a
 * disk cache, and the signed-out login row lives on the CURRENT screen.
 */
static void ra_ctrl_game_list(void)
{
	int n_games = 0;

	(void)ra_gamelist_rows(&n_games);

	if (hold_pad[PAD_UP] || hold_pad[PAD_LEFT_ANALOG_UP]) {
		if (ra_game_sel > 0)
			ra_game_sel--;
	}
	if (hold_pad[PAD_DOWN] || hold_pad[PAD_LEFT_ANALOG_DOWN]) {
		if (ra_game_sel < n_games - 1)
			ra_game_sel++;
	}

	/* keep selection visible */
	if (ra_game_sel < ra_game_base)
		ra_game_base = ra_game_sel;
	if (ra_game_sel >= ra_game_base + RA_ROWS_VISIBLE)
		ra_game_base = ra_game_sel - RA_ROWS_VISIBLE + 1;
	if (ra_game_base < 0)
		ra_game_base = 0;

	if (released_pad[PAD_ENTER] && n_games > 0 && ra_game_sel < n_games) {
		const ra_game_row *g = &ra_gamelist_rows(NULL)[ra_game_sel];

		ra_detail_game_id = g->game_id;
		strncpy(ra_detail_title, g->title, sizeof(ra_detail_title) - 1);
		ra_detail_title[sizeof(ra_detail_title) - 1] = 0;
		strncpy(ra_detail_icon, g->icon_name, sizeof(ra_detail_icon) - 1);
		ra_detail_icon[sizeof(ra_detail_icon) - 1] = 0;

		/* ra_game_sel / ra_game_base are NOT touched: O restores this exact
		 * position when the user comes back (H4). */
		ra_screen = RA_SCREEN_DETAIL;
		RA_LOG("[RA] collection -> detail game %u (%s)\n", g->game_id, ra_detail_title);
		ra_detail_request(g->game_id);
	}
}

/*
 * v25 — start the inline IME login flow (username, then password).
 *
 * Extracted from the CURRENT screen's row-0 confirm handler below so the new
 * Triangle hotkey and the existing login row cannot drift apart. The
 * ra_start_ime() return value is checked here, unlike at the original call
 * site: a failed sceImeDialogInit() used to leave ra_login_step pinned at
 * RA_LOGIN_USERNAME with ra_ime_active still 0, which permanently wedges the
 * "ra_login_step == RA_LOGIN_IDLE" guard and makes every later login attempt a
 * silent no-op (login-x-press-debug.md, Addendum 1 H2).
 */
static void ra_begin_inline_login(void)
{
	if (ra_is_login_in_progress() || ra_login_step != RA_LOGIN_IDLE)
		return;

	/* v26: prefer the IME-free credentials file when one exists; only fall
	 * through to the Vita IME dialog when there is no file (the IME path is
	 * what crashes on this build -- v25-login-crash.md). */
	if (ra_file_login_attempt())
		return;

	ra_login_step = RA_LOGIN_USERNAME;
	ra_login_max_len = 63;
	if (ra_start_ime("RetroAchievements Username", ra_login_username, 0, ra_login_max_len) < 0) {
		RA_LOG("[RA] v25 login IME failed to open - resetting step\n");
		ra_login_step = RA_LOGIN_IDLE;
	}
}

/*
 * v25 — put the view back on a screen that makes sense after signing out.
 *
 * LIST and DETAIL are both built from session data that ra_logout() has just
 * invalidated, and DETAIL in particular becomes a permanently empty grid for a
 * signed-out user. CURRENT is the screen that owns the login row, so land
 * there. The collection cache on disk is NOT touched, so a later Triangle
 * login (or the offline path in ra_view_opened()) can still surface it.
 */
static void ra_view_after_logout(void)
{
	ra_screen = RA_SCREEN_CURRENT;
	ra_detail_game_id = 0;
	ra_n_rows = 0;
	ra_row_sel = 0;
	ra_row_base = 0;
	ra_offline_display = 0;
}

void ra_ctrl_trophy_tab(void)
{
	int login_offset;
	int n_items;

	/* do not navigate while the IME dialog is up (checked before the screen
	 * dispatch so it covers all three) */
	if (ra_ime_active)
		return;

	/*
	 * v25 — the always-reachable Login/Logout hotkey.
	 *
	 * Deliberately the FIRST thing checked after the IME guard: before the
	 * RA_SCREEN_LIST dispatch below, and before the n_items early return, so
	 * it fires identically on CURRENT, LIST and DETAIL and depends on nothing
	 * -- not ra_screen, not ra_row_sel, not ra_n_rows. That matters because a
	 * signed-out user on DETAIL has zero rows and, before this build, every
	 * other control in this function was unreachable (login-x-press-debug.md,
	 * Addendum 2): the login affordance had to live somewhere the freeze could
	 * not reach.
	 *
	 * Triangle is safe to claim: it is written by utils.c:110-111 but read by
	 * nothing else in menu.c or this file, and this handler only runs while the
	 * trophy tab owns input.
	 */
	if (released_pad[PAD_TRIANGLE]) {
		if (ra_is_logged_in()) {
			RA_LOG("[RA] v25 Triangle -> logout (screen %d)\n", ra_screen);
			ra_logout();
			ra_view_after_logout();
		} else {
			RA_LOG("[RA] v25 Triangle -> login (screen %d)\n", ra_screen);
			ra_begin_inline_login();
		}
		return;   /* consume the press; skip this frame's screen dispatch */
	}

	if (ra_screen == RA_SCREEN_LIST) {
		ra_ctrl_game_list();
		return;
	}
	/* RA_SCREEN_DETAIL falls through to the shared grid navigation below; its
	 * O/back is handled by ra_view_handle_cancel() from menu.c. */

	/* DETAIL has no login row of its own -- the Triangle hotkey above covers
	 * login from there -- so its offset stays 0. */
	login_offset = (ra_screen == RA_SCREEN_DETAIL || ra_is_logged_in()) ? 0 : 1;
	n_items = ra_n_rows + login_offset;

	/*
	 * v25 — THE DETAIL INPUT FREEZE (login-x-press-debug.md, Addendum 2).
	 *
	 * This guard used to be a bare "if (n_items == 0) return;". The comment it
	 * replaced claimed DETAIL was "unreachable signed out", but
	 * ra_ctrl_game_list() enters DETAIL from a cached collection row with no
	 * login check at all -- and once there, ra_detail_request() bails on
	 * !ra_is_logged_in() AFTER ra_view_detail_begin() has zeroed ra_n_rows, so
	 * n_items is pinned at 0 forever. The early return then fired every frame,
	 * ahead of the dpad block, the confirm block, and (had it been placed
	 * below) the new hotkey: total input death on that screen.
	 *
	 * Narrowed so DETAIL always falls through. The navigation below is already
	 * safe with n_items == 0 -- "ra_row_sel > 0" and "ra_row_sel < n_items - 1"
	 * are both false, so up/down become no-ops rather than running off the
	 * array -- and PAD_ENTER's only branch requires ra_row_sel == 0 while
	 * signed out, which on DETAIL is the (absent) login row, so it does
	 * nothing there either. DETAIL stays legitimately empty while signed out;
	 * it just no longer swallows the frame.
	 */
	if (n_items == 0 && ra_screen != RA_SCREEN_DETAIL)
		return;

	if (hold_pad[PAD_UP] || hold_pad[PAD_LEFT_ANALOG_UP]) {
		if (ra_row_sel > 0)
			ra_row_sel--;
	}
	if (hold_pad[PAD_DOWN] || hold_pad[PAD_LEFT_ANALOG_DOWN]) {
		if (ra_row_sel < n_items - 1)
			ra_row_sel++;
	}

	/* keep selection visible */
	if (ra_row_sel < ra_row_base + login_offset)
		ra_row_base = (ra_row_sel >= login_offset) ? ra_row_sel - login_offset : 0;
	if (ra_row_sel - login_offset >= ra_row_base + RA_ROWS_VISIBLE)
		ra_row_base = ra_row_sel - login_offset - RA_ROWS_VISIBLE + 1;

	if (released_pad[PAD_ENTER]) {
		if (!ra_is_logged_in() && ra_row_sel == 0) {
			/*
			 * The CURRENT screen's login row. v25 routes it through the shared
			 * helper so the row and the Triangle hotkey run identical code
			 * (including the ra_start_ime() failure reset the old inline
			 * version lacked).
			 *
			 * On a signed-out DETAIL this same branch is now reachable too --
			 * n_items is 0 there, so ra_row_sel is 0 and no real row exists to
			 * confirm. Triggering login is the only sensible meaning confirm
			 * can have on an empty auth-gated screen, and it matches what the
			 * footer already offers via Triangle.
			 */
			ra_begin_inline_login();
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Tick integration                                                           */
/* ------------------------------------------------------------------------- */

void ra_view_tick(void)
{
	/* poll the login IME dialog */
	ra_login_poll_ime();

	/*
	 * v18 badge prefetch, deliberately NOT gated on ra_view_open: the whole
	 * point is to have the badge cached before an unlock toast appears during
	 * gameplay, with the trophy tab closed. Self-paced and self-latching; a
	 * no-op once the walk is done.
	 */
	ra_view_prefetch_pump();

	/* live list refresh while displayed (unlocks, progress) */
	if (ra_view_open && ra_is_logged_in() && ra_is_game_loaded() && !ra_offline_display) {
		static uint32_t refresh_counter = 0;
		if ((refresh_counter++ % 60) == 0) {
			/*
			 * v15 Bug 1 fix (v14-scroll-stale-debug.md, "Minimal fix Option B").
			 *
			 * ra_view_rebuild_list() unconditionally zeroes ra_row_sel /
			 * ra_row_base because it also serves the cold-open path
			 * (ra_view_opened), where resetting the scroll IS correct. This
			 * periodic refresh only wants fresh row CONTENT, not a scroll
			 * reset -- without the save/restore below the user's position
			 * snapped back to row 0 once per second while idle.
			 */
			int saved_sel = ra_row_sel;
			int saved_base = ra_row_base;

			ra_view_rebuild_list();

			ra_row_sel = saved_sel;
			ra_row_base = saved_base;

			/* re-clamp: the list may have grown or shrunk since the last
			 * rebuild (e.g. the synthesized platinum/mastery row appearing) */
			int login_offset = ra_is_logged_in() ? 0 : 1;
			int n_items = ra_n_rows + login_offset;
			if (ra_row_sel >= n_items)
				ra_row_sel = (n_items > 0) ? n_items - 1 : 0;
			if (ra_row_base > ra_row_sel)
				ra_row_base = ra_row_sel;
		}
	}

	/*
	 * v15 Bug 3 fix -- offline sync-on-reconnect (v14-scroll-stale-debug.md,
	 * "Gap B: periodic re-check").
	 *
	 * ra_net_is_online() is a live sceNetCtlInetGetState() poll, but after the
	 * cold-start offline decision in ra_client_hash_done() nothing ever calls
	 * it again, so a Wi-Fi reconnect while the trophy screen is open went
	 * unnoticed until the tab was closed and reopened. Poll it at ~5s while a
	 * cached (offline) list is on screen and, once the link is back, kick the
	 * normal online path: an auto-login with the stored token (the cold-start
	 * offline case is always signed out, since login needs the network) whose
	 * success callback sets ra_pending_game_load, plus an explicit game-load
	 * request for the already-signed-in case.
	 *
	 * Deliberately NOT the bigger "detect unlocks that happened while offline"
	 * architecture -- that needs an rc_client session running offline and is
	 * out of scope here (report, "Important scoping note").
	 */
	/*
	 * v24 — immediate promotion when an in-flight login lands.
	 *
	 * The ~5 s poll below is the wrong instrument for the login race: it only
	 * runs while the menu stays open, and a user who glances at the overlay,
	 * sees stale/offline content and backs out within those 5 seconds never
	 * sees it correct itself at all. Since ra_offline_display is only ever set
	 * from ra_view_opened()'s signed-out branch, observing it set while now
	 * logged in and online means exactly one thing: the login we were racing
	 * has completed. Drop the offline presentation on that very frame and
	 * re-request the live data.
	 *
	 * Self-limiting, not a request loop: clearing the flag falsifies this
	 * condition, so the block runs once per offline->online transition.
	 */
	if (ra_view_open && ra_offline_display && ra_is_logged_in() && ra_net_is_online()) {
		ra_offline_display = 0;
		if (ra_screen == RA_SCREEN_LIST)
			ra_gamelist_request();
		else
			ra_request_game_load();
	}

	if (ra_view_open && (ra_offline_display || ra_gamelist_is_cached_only())) {
		static uint32_t offline_counter = 0;
		if ((offline_counter++ % 300) == 0 && ra_net_is_online()) {
			if (!ra_is_logged_in() && !ra_is_login_in_progress() && ra_has_saved_credentials())
				ra_try_auto_login();

			/*
			 * v21: on the collection screen the thing to refresh is the
			 * collection, not the (non-existent) booted game. The session
			 * latch inside ra_gamelist_request() stops this 5s poll from
			 * turning into a request loop once a live fetch succeeds.
			 */
			if (ra_screen == RA_SCREEN_LIST)
				ra_gamelist_request();
			else
				ra_request_game_load();
		}
	}
}
