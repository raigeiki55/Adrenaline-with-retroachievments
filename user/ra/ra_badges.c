/*
 * Adrenaline+ RetroAchievements — badge texture cache
 *
 * Lookup order: memory (LRU textures) -> disk (raw PNG cache) -> network
 * download. While a download is pending the view draws a placeholder; the
 * texture appears once the worker delivers the PNG body.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <vita2d.h>

#include "ra_internal.h"
#include "../lodepng/lodepng.h"
#include "../utils.h"   /* adr_free: lodepng's deallocator (lodepng.c:101 -> utils.c:419) */

#define RA_BADGE_CACHE_MAX 64

/* v11: per-slot badge state machine (Fix B). */
#define RA_BADGE_EMPTY    0
#define RA_BADGE_FETCHING 1   /* download queued OR rgba awaiting GPU upload */
#define RA_BADGE_READY    2
#define RA_BADGE_FAILED   3

/* Retry constants, in frames (ra_badges_tick increments once per frame). */
#define RA_BADGE_FETCH_TIMEOUT 600  /* ~10 s in-flight watchdog */
#define RA_BADGE_FAIL_RETRY 1800    /* ~30 s after a failed download */
#define RA_BADGE_QUEUE_RETRY 60     /* ~1 s when the FIFO/pending queue was full */

typedef struct ra_badge_slot {
	vita2d_texture *tex;
	char name[16];
	int locked;
	int used;
	int state;          /* RA_BADGE_* */
	uint32_t retry_at;  /* tick after which FETCHING may be re-issued / FAILED retried */
	uint32_t last_use;
} ra_badge_slot;

static ra_badge_slot ra_badge_cache[RA_BADGE_CACHE_MAX];
static uint32_t ra_badge_tick_counter = 0;

/* ------------------------------------------------------------------------- */
/* v11 Fix A: pre-draw upload queue + deferred GPU free list                  */
/* ------------------------------------------------------------------------- */

#define RA_BADGE_PENDING_MAX 8
static struct {
	char name[16];
	int locked;
	unsigned char *rgba;   /* lodepng output, owned here until uploaded */
	unsigned w, h;
	int busy;
} ra_badge_pending[RA_BADGE_PENDING_MAX];

#define RA_BADGE_FREE_MAX 16
static vita2d_texture *ra_badge_free_list[RA_BADGE_FREE_MAX];
static int ra_badge_free_count = 0;
static unsigned ra_badge_req_counter = 0;  /* for the fetch log line */

/* per-frame aging, called from ra_tick() */
void ra_badges_tick(void)
{
	ra_badge_tick_counter++;
}

/* ------------------------------------------------------------------------- */
/* Disk                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * v21: 3-way suffix. `locked` is an RA_IMG_* variant code (ra.h), not a bool.
 *
 * The "_g" suffix for game icons is not cosmetic -- it prevents a real cache
 * collision: RA game image_names and achievement badge_names are both bare
 * decimal numbers drawn from two independent id spaces, and they share this one
 * RA_BADGES_DIR. Without the suffix, game 12345's box art and achievement
 * badge 12345 would fight over "12345.png".
 */
static void ra_badge_path(char *out, int out_size, const char *badge_name, int locked)
{
	const char *suffix;

	switch (locked) {
	case RA_IMG_BADGE_LOCKED: suffix = "_lock"; break;
	case RA_IMG_GAME_ICON:    suffix = "_g";    break;
	default:                  suffix = "";      break;   /* RA_IMG_BADGE_UNLOCKED */
	}

	snprintf(out, out_size, "%s/%s%s.png", RA_BADGES_DIR, badge_name, suffix);
}

static void ra_write_file(const char *path, const void *data, int size)
{
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, data, size);
	sceIoClose(fd);
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                     */
/* ------------------------------------------------------------------------- */

/* Decode a PNG buffer into RGBA bytes. Returns NULL on failure.
 * CPU only: lodepng decode. NO vita2d/GXM calls in this function. */
static unsigned char *ra_decode_png_rgba(const void *data, size_t size, unsigned *w, unsigned *h)
{
	unsigned char *image = NULL;
	*w = *h = 0;
	if (lodepng_decode32(&image, w, h, (const unsigned char *)data, size) != 0)
		return NULL;
	RA_LOG("[RA] badge decode %ux%u rc=ok\n", *w, *h);
	/* WARNING-2 guard. v21: RA game icons (RA_IMG_GAME_ICON, /Images/) are 96x96
	 * today, so the existing 128 cap covers them and is intentionally left
	 * unchanged rather than widened for every badge. If RA ever ships larger
	 * game art, the "badge decode rejected WxH" line below is the diagnosis. */
	if (!image || *w == 0 || *h == 0 || *w > 128 || *h > 128) {
		RA_LOG("[RA] badge decode rejected %ux%u\n", *w, *h);
		adr_free(image);   /* lodepng allocated this on the AdrHeap mspace */
		return NULL;
	}
	return image;
}

/* ------------------------------------------------------------------------- */
/* Insert / lookup                                                            */
/* ------------------------------------------------------------------------- */

static ra_badge_slot *ra_badge_find(const char *badge_name, int locked)
{
	int i;
	for (i = 0; i < RA_BADGE_CACHE_MAX; i++) {
		if (ra_badge_cache[i].used && ra_badge_cache[i].locked == locked &&
			strcmp(ra_badge_cache[i].name, badge_name) == 0)
			return &ra_badge_cache[i];
	}
	return NULL;
}

static ra_badge_slot *ra_badge_evict_lru(void)
{
	int i;
	ra_badge_slot *victim = NULL;

	/* free slot first */
	for (i = 0; i < RA_BADGE_CACHE_MAX; i++) {
		if (!ra_badge_cache[i].used)
			return &ra_badge_cache[i];
	}

	/* v11: skip FETCHING slots (their delivery is in flight; evicting them
	 * re-triggers fetch churn). Fall back to any slot only if all are FETCHING. */
	for (i = 0; i < RA_BADGE_CACHE_MAX; i++) {
		if (ra_badge_cache[i].state == RA_BADGE_FETCHING)
			continue;
		if (!victim || ra_badge_cache[i].last_use < victim->last_use)
			victim = &ra_badge_cache[i];
	}
	if (!victim) {
		for (i = 0; i < RA_BADGE_CACHE_MAX; i++) {
			if (!victim || ra_badge_cache[i].last_use < victim->last_use)
				victim = &ra_badge_cache[i];
		}
	}

	if (victim->tex) {
		/* v11 WARNING-1 fix: do NOT free here (GPU may still reference the
		 * texture from the in-flight frame). Defer to the fenced window in
		 * ra_badges_upload_pending(). */
		if (ra_badge_free_count >= RA_BADGE_FREE_MAX)
			return NULL;               /* cannot safely free yet; caller retries next frame */
		ra_badge_free_list[ra_badge_free_count++] = victim->tex;
		victim->tex = NULL;
	}
	victim->used = 0;
	victim->state = RA_BADGE_EMPTY;
	return victim;
}

static void ra_badge_mark_failed(const char *badge_name, int locked, uint32_t retry_delay)
{
	ra_badge_slot *slot = ra_badge_find(badge_name, locked);
	if (slot) {
		slot->state = RA_BADGE_FAILED;
		slot->retry_at = ra_badge_tick_counter + retry_delay;
	}
}

/* Reserve (or refresh) a slot BEFORE any fetch/upload is issued. Fix B core. */
static ra_badge_slot *ra_badge_reserve(const char *badge_name, int locked)
{
	ra_badge_slot *slot = ra_badge_find(badge_name, locked);
	if (!slot) {
		slot = ra_badge_evict_lru();
		if (!slot)
			return NULL;
		strncpy(slot->name, badge_name, sizeof(slot->name) - 1);
		slot->name[sizeof(slot->name) - 1] = 0;
		slot->locked = locked;
		slot->used = 1;
		slot->tex = NULL;
	}
	slot->state = RA_BADGE_FETCHING;
	slot->retry_at = ra_badge_tick_counter + RA_BADGE_FETCH_TIMEOUT;
	slot->last_use = ra_badge_tick_counter;
	return slot;
}

/* Hand RGBA bytes to the pre-draw upload window. Takes ownership of rgba. */
static void ra_badge_queue_upload(const char *badge_name, int locked,
	unsigned char *rgba, unsigned w, unsigned h)
{
	int i;
	for (i = 0; i < RA_BADGE_PENDING_MAX; i++) {
		if (ra_badge_pending[i].busy)
			continue;
		strncpy(ra_badge_pending[i].name, badge_name, sizeof(ra_badge_pending[i].name) - 1);
		ra_badge_pending[i].name[sizeof(ra_badge_pending[i].name) - 1] = 0;
		ra_badge_pending[i].locked = locked;
		ra_badge_pending[i].rgba = rgba;
		ra_badge_pending[i].w = w;
		ra_badge_pending[i].h = h;
		ra_badge_pending[i].busy = 1;
		return;
	}
	/* queue full: drop, short retry (disk hit next time, so cheap) */
	adr_free(rgba);   /* lodepng buffer -> AdrHeap mspace */
	ra_badge_mark_failed(badge_name, locked, RA_BADGE_QUEUE_RETRY);
}

static void ra_badge_insert(const char *badge_name, int locked, vita2d_texture *tex)
{
	ra_badge_slot *slot = ra_badge_find(badge_name, locked);
	if (slot) {
		/* duplicate delivery: replace in place; we are inside the fenced
		 * window (GPU idle), so an immediate free of the old texture is safe */
		if (slot->tex && slot->tex != tex)
			vita2d_free_texture(slot->tex);
	} else {
		slot = ra_badge_evict_lru();
		if (!slot) {
			vita2d_free_texture(tex);   /* fenced window: safe */
			return;
		}
		strncpy(slot->name, badge_name, sizeof(slot->name) - 1);
		slot->name[sizeof(slot->name) - 1] = 0;
		slot->locked = locked;
	}
	slot->tex = tex;
	slot->used = 1;
	slot->state = RA_BADGE_READY;
	slot->retry_at = 0;
	slot->last_use = ra_badge_tick_counter;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Returns a texture for the badge, or NULL while pending/failed (the view
 * draws a placeholder). Side effect: queues a download when neither memory
 * nor disk has the badge.
 */
void *ra_badge_get(const char *badge_name, int locked)
{
	ra_badge_slot *slot;
	char path[256];

	if (!badge_name || !badge_name[0])
		return NULL;

	/* memory */
	slot = ra_badge_find(badge_name, locked);
	if (slot) {
		slot->last_use = ra_badge_tick_counter;
		if (slot->state == RA_BADGE_READY && slot->tex)
			return slot->tex;
		if (slot->state == RA_BADGE_FETCHING &&
			ra_badge_tick_counter < slot->retry_at)
			return NULL;                 /* in flight: DO NOT re-request (Fix B) */
		if (slot->state == RA_BADGE_FAILED &&
			ra_badge_tick_counter < slot->retry_at)
			return NULL;                 /* negative cache */
		/* FETCHING timed out or FAILED expired: fall through and retry */
	}

	/* disk — CPU-only now; NO texture creation inside the scene (Fix A) */
	ra_badge_path(path, sizeof(path), badge_name, locked);
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
		sceIoLseek(fd, 0, SCE_SEEK_SET);
		if (size > 0 && size < 256 * 1024) {
			void *buf = malloc(size);
			if (buf) {
				int read = sceIoRead(fd, buf, size);
				sceIoClose(fd); fd = -1;
				if (read == size) {
					unsigned w = 0, h = 0;
					unsigned char *rgba = ra_decode_png_rgba(buf, size, &w, &h);
					free(buf); buf = NULL;
					if (rgba) {
						if (ra_badge_reserve(badge_name, locked))
							ra_badge_queue_upload(badge_name, locked, rgba, w, h);
						else
							adr_free(rgba);   /* lodepng buffer -> AdrHeap mspace */
						return NULL;   /* texture appears next frame via the fenced window */
					}
					/* corrupt disk file: fall through to network */
				}
				free(buf);
			} else {
				sceIoClose(fd); fd = -1;
			}
		}
		if (fd >= 0) sceIoClose(fd);
	}

	/* network — reserve FIRST, then fetch once (Fix B) */
	{
		char url[512];
		rc_client_t *client = ra_get_client();
		if (client) {
			if (!ra_badge_reserve(badge_name, locked))
				return NULL;
			/*
			 * v13 FIX 1 -- badge host swapped to the APEX host.
			 *
			 * This used to build
			 *     https://media.retroachievements.org/Badge/<id>[_lock].png
			 * which is the ONLY request in this module that talks to a second
			 * hostname. investigation-result/media-host-crash-research.md
			 * (2026-09-02) established that the two hosts are the same server,
			 * the same IPs and literally the same X.509 certificate; the only
			 * client-reachable difference is WHICH SubjectAltName matches:
			 *
			 *   retroachievements.org        -> SAN[0], exact match, short-circuits
			 *   media.retroachievements.org  -> SAN[1] "*.retroachievements.org",
			 *                                   i.e. curl's WILDCARD branch, which
			 *                                   calls Curl_host_is_ipnum() ->
			 *                                   newlib inet_pton() -> sceNetInetPton()
			 *                                   and Curl_memrchr(). That branch runs
			 *                                   for the media host and NEVER for the
			 *                                   API host, and it is the last
			 *                                   host-exclusive code path standing
			 *                                   after v10/v11/v12.
			 *
			 * The apex path was live-tested with this app's exact TLS profile
			 * (UA "AdrenalinePlus-RA/1.0", TLS 1.2 only, HTTP/1.1 only, GTS Root R4
			 * as the sole trust anchor):
			 *     https://retroachievements.org/Badge/109943.png
			 *     -> http=200 size=8642 verify=0, byte-identical to the media URL,
			 *        no redirect.
			 *
			 * So this is both the cheapest discriminator and a candidate fix:
			 * crash gone => host-B/wildcard-specific; crash persists => the host is
			 * not the variable.
			 *
			 * CAVEAT: the apex /Badge/ path is the legacy one and RA could
			 * deprecate it. If badges start 404ing, put the media host back here
			 * -- the rest of the URL is identical.
			 */
			/*
			 * v21: RA_IMG_GAME_ICON takes the /Images/ path (game box art is
			 * served from a different directory than achievement badges);
			 * variants 0/1 keep the exact v13 /Badge/ URL, byte for byte.
			 *
			 * Host: the SAME apex host for both, deliberately. See the v13
			 * analysis above -- the media host is the one that drags curl
			 * through the wildcard-SAN branch. Using apex for icons keeps
			 * every request in this module on the one host whose cert path was
			 * live-verified on this hardware.
			 *
			 * UNVERIFIED-ON-HARDWARE: apex /Badge/ was live-tested (v13);
			 * apex /Images/ has not been. If game icons 404 on the hardware
			 * run, the documented one-line fallback is
			 *     https://media.retroachievements.org/Images/%s.png
			 * (the canonical host), accepting re-exposure to the wildcard-SAN
			 * code path. Diagnose from the "[RA] badge GET <- ... status=404"
			 * line in ux0:data/adrenaline_user_log.txt.
			 */
			if (locked == RA_IMG_GAME_ICON)
				snprintf(url, sizeof(url), "https://retroachievements.org/Images/%s.png",
					badge_name);
			else
				snprintf(url, sizeof(url), "https://retroachievements.org/Badge/%s%s.png",
					badge_name, (locked == RA_IMG_BADGE_LOCKED) ? "_lock" : "");
			RA_LOG("[RA] badge fetch %s variant=%d (req#%u)\n",
				badge_name, locked, ++ra_badge_req_counter);
			if (!ra_net_fetch_badge(url, badge_name, locked))     /* now returns int */
				ra_badge_mark_failed(badge_name, locked, RA_BADGE_QUEUE_RETRY);
		}
	}

	return NULL;
}

/*
 * v18 badge PREFETCH (investigation-result/toast-badge-delay.md, "Primary
 * recommendation: prefetch BOTH badge variants at achievement-list load time").
 *
 * Warm-up entry point for a badge variant that nothing is drawing yet, so the
 * unlock toast finds it already cached instead of paying a full uncached TLS
 * round trip on its critical path (ra_net.c:905/1079 -- a fresh curl handle,
 * hence a fresh DNS+TCP+TLS handshake, per request; serialized behind the
 * award-submission POST in the single work FIFO, ra_net.c:1121/1156).
 *
 * This is deliberately NOT a second cache. The fetch itself is ra_badge_get()
 * below -- the exact same reserve/fetch/deliver/disk-persist state machine the
 * draw path uses. The only thing added here is a cheap "do we already have the
 * bytes?" test, because a warm-up walk over ~184 variants must NOT:
 *   - synchronously lodepng-decode every already-cached badge on the render
 *     thread (ra_badge_get's disk tier, ra_badges.c:274-304, decodes on hit)
 *     and overflow the 8-slot RA_BADGE_PENDING_MAX upload queue, and
 *   - reserve/evict its way through all 64 RA_BADGE_CACHE_MAX LRU slots for
 *     textures nothing is about to draw.
 * Skipping the decode keeps this a pure network warm-up: the bytes land on
 * disk, and the eventual real draw (toast or list) does the ~1-frame decode.
 *
 * Returns 1 when a cold variant was handed to the fetch machinery this call,
 * 0 when it was already cached / in flight / negative-cached. The caller uses
 * that to pace itself (ra_trophy_view.c: one issue per RA_PREFETCH_ISSUE_GAP
 * frames) so the 16-deep work FIFO is never flooded.
 */
int ra_badge_prefetch(const char *badge_name, int locked)
{
	ra_badge_slot *slot;
	char path[256];
	SceUID fd;

	if (!badge_name || !badge_name[0])
		return 0;

	/* memory: READY, in flight (FETCHING before its watchdog) or negative-cached
	 * (FAILED before its retry) all mean "leave it alone". Note we deliberately
	 * do NOT touch slot->last_use: a prefetch must never reorder the LRU against
	 * the badges the draw path is actually showing. */
	slot = ra_badge_find(badge_name, locked);
	if (slot) {
		if (slot->state == RA_BADGE_READY && slot->tex)
			return 0;
		if ((slot->state == RA_BADGE_FETCHING || slot->state == RA_BADGE_FAILED) &&
			ra_badge_tick_counter < slot->retry_at)
			return 0;
	}

	/* disk: the bytes are already local, so the real draw is a decode away
	 * (~1 frame) -- which is the "instant badge" target. Nothing to fetch. */
	ra_badge_path(path, sizeof(path), badge_name, locked);
	fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
		sceIoClose(fd);
		if (size > 0)
			return 0;
	}

	/* cold in both tiers: hand it to the normal state machine. It re-checks
	 * memory and disk (both just missed), reserves the slot and queues the
	 * download. Returns NULL by construction -- we want the side effect. */
	ra_badge_get(badge_name, locked);
	return 1;
}

/*
 * Delivery path (render thread): the worker downloaded the PNG body; persist
 * it to the disk cache and decode into a texture.
 */
void ra_badge_deliver(const char *badge_name, int locked, const char *data, size_t len, int ok)
{
	char path[256];
	unsigned w = 0, h = 0;
	unsigned char *rgba;

	RA_LOG("[RA] badge deliver %s locked=%d len=%u ok=%d\n",
		badge_name ? badge_name : "(null)", locked, (unsigned)len, ok);

	if (!badge_name || !badge_name[0])
		return;
	if (!ok || !data || len == 0) {
		ra_badge_mark_failed(badge_name, locked, RA_BADGE_FAIL_RETRY);
		return;
	}

	/* persist for offline use */
	ra_badge_path(path, sizeof(path), badge_name, locked);
	ra_write_file(path, data, (int)len);

	/* decode ONLY — no vita2d call here (Fix A) */
	rgba = ra_decode_png_rgba(data, len, &w, &h);
	if (!rgba) {
		ra_badge_mark_failed(badge_name, locked, RA_BADGE_FAIL_RETRY);
		return;
	}
	if (!ra_badge_find(badge_name, locked)) {
		/* slot was evicted while in flight; re-reserve so insert has a home */
		if (!ra_badge_reserve(badge_name, locked)) {
			adr_free(rgba);   /* lodepng buffer -> AdrHeap mspace */
			return;
		}
	}
	ra_badge_queue_upload(badge_name, locked, rgba, w, h);
}

/*
 * v11 Fix A — the fenced GPU window.
 * Called from user/menu.c immediately BEFORE vita2d_start_drawing(), i.e.
 * outside any GXM scene. Fences ONCE (sceGxmFinish via
 * vita2d_wait_rendering_done) only when there is GPU work to do, then:
 *   1. destroys deferred textures (safe: GPU idle),
 *   2. creates + fills textures for delivered/disk-loaded badges.
 * This is the ONLY place in the badge subsystem that creates or destroys
 * GXM objects.
 */
void ra_badges_upload_pending(void)
{
	int i, have_uploads = 0;

	for (i = 0; i < RA_BADGE_PENDING_MAX; i++)
		if (ra_badge_pending[i].busy) { have_uploads = 1; break; }
	if (!have_uploads && ra_badge_free_count == 0)
		return;                            /* fast path: nothing to do, no fence */

	vita2d_wait_rendering_done();          /* sceGxmFinish — the barrier menu.c already uses */

	/* 1. deferred frees (eviction victims) */
	for (i = 0; i < ra_badge_free_count; i++) {
		RA_LOG("[RA] badge free tex=%p\n", (void *)ra_badge_free_list[i]);
		vita2d_free_texture(ra_badge_free_list[i]);
	}
	ra_badge_free_count = 0;

	/* 2. uploads */
	for (i = 0; i < RA_BADGE_PENDING_MAX; i++) {
		if (!ra_badge_pending[i].busy) continue;

		SceKernelMemBlockType prev = vita2d_texture_get_alloc_memblock_type();   /* WARNING-3 */
		vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
		vita2d_texture *tex = vita2d_create_empty_texture(ra_badge_pending[i].w, ra_badge_pending[i].h);
		vita2d_texture_set_alloc_memblock_type(prev);                            /* restore! */

		RA_LOG("[RA] badge tex=%p %s locked=%d\n",
			(void *)tex, ra_badge_pending[i].name, ra_badge_pending[i].locked);

		if (tex) {
			sceClibMemcpy(vita2d_texture_get_datap(tex), ra_badge_pending[i].rgba,
				ra_badge_pending[i].w * ra_badge_pending[i].h * 4);
			ra_badge_insert(ra_badge_pending[i].name, ra_badge_pending[i].locked, tex);
		} else {
			ra_badge_mark_failed(ra_badge_pending[i].name, ra_badge_pending[i].locked,
				RA_BADGE_FAIL_RETRY);
		}
		adr_free(ra_badge_pending[i].rgba);   /* lodepng buffer -> AdrHeap mspace */
		ra_badge_pending[i].rgba = NULL;
		ra_badge_pending[i].busy = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* Offline snapshot                                                           */
/* ------------------------------------------------------------------------- */

#define RA_SNAPSHOT_MAGIC "RACACHE1"
#define RA_SNAPSHOT_MAX_ENTRIES 256

static ra_snapshot_entry ra_snapshot_entries[RA_SNAPSHOT_MAX_ENTRIES];
static int ra_snapshot_count = 0;

static void ra_snapshot_path(char *out, int out_size, const char *game_hash)
{
	snprintf(out, out_size, "%s/%s.bin", RA_CACHE_DIR, game_hash);
}

void ra_snapshot_save(const char *game_hash)
{
	rc_client_t *client = ra_get_client();
	if (!client || !game_hash || !game_hash[0])
		return;

	rc_client_achievement_list_t *list = rc_client_create_achievement_list(client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list)
		return;

	ra_snapshot_count = 0;

	for (uint32_t b = 0; b < list->num_buckets && ra_snapshot_count < RA_SNAPSHOT_MAX_ENTRIES; b++) {
		const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
		for (uint32_t i = 0; i < bucket->num_achievements && ra_snapshot_count < RA_SNAPSHOT_MAX_ENTRIES; i++) {
			const rc_client_achievement_t *ach = bucket->achievements[i];
			ra_snapshot_entry *e = &ra_snapshot_entries[ra_snapshot_count];
			memset(e, 0, sizeof(*e));
			if (ach->badge_name)
				strncpy(e->badge_name, ach->badge_name, sizeof(e->badge_name) - 1);
			if (ach->title)
				strncpy(e->title, ach->title, sizeof(e->title) - 1);
			if (ach->description)
				strncpy(e->description, ach->description, sizeof(e->description) - 1);
			e->points = ach->points;
			e->unlocked = ach->unlocked;
			e->unlock_time = (uint64_t)ach->unlock_time;
			ra_snapshot_count++;
		}
	}

	rc_client_destroy_achievement_list(list);

	char path[256];
	ra_snapshot_path(path, sizeof(path), game_hash);

	/* write magic + count + entries (same arch read/write; binary format) */
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, RA_SNAPSHOT_MAGIC, 8);
	sceIoWrite(fd, &ra_snapshot_count, sizeof(ra_snapshot_count));
	sceIoWrite(fd, ra_snapshot_entries, ra_snapshot_count * sizeof(ra_snapshot_entry));
	sceIoClose(fd);
}

int ra_snapshot_load(const char *game_hash)
{
	char path[256];
	char magic[8];
	int count = 0;

	if (!game_hash || !game_hash[0])
		return -1;

	ra_snapshot_path(path, sizeof(path), game_hash);

	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;

	if (sceIoRead(fd, magic, 8) != 8 || memcmp(magic, RA_SNAPSHOT_MAGIC, 8) != 0) {
		sceIoClose(fd);
		return -1;
	}

	if (sceIoRead(fd, &count, sizeof(count)) != sizeof(count) || count < 0 || count > RA_SNAPSHOT_MAX_ENTRIES) {
		sceIoClose(fd);
		return -1;
	}

	if (sceIoRead(fd, ra_snapshot_entries, count * sizeof(ra_snapshot_entry)) !=
			count * (int)sizeof(ra_snapshot_entry)) {
		sceIoClose(fd);
		return -1;
	}

	sceIoClose(fd);
	ra_snapshot_count = count;
	return count;
}

const ra_snapshot_entry *ra_snapshot_get_entries(int *count)
{
	if (count)
		*count = ra_snapshot_count;
	return ra_snapshot_entries;
}

void ra_snapshot_clear(void)
{
	ra_snapshot_count = 0;
}
