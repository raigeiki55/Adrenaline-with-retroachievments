/*
 * Adrenaline+ RetroAchievements — achievement-unlock toast overlay
 *
 * Wall-clock timing (sceKernelGetProcessTimeWide, microseconds) — NOT frame
 * counted — so the ~4 s slide/hold/fade is frame-rate independent (the render
 * loop's frame time varies across PSP/native/POPS paths; precedent for the
 * timer at ra_client.c:148,160).
 *
 * GXM safety: this module NEVER creates or destroys textures. It only draws
 * textures returned by ra_badge_get() (memory-hit fast path, ra_badges.c:260-264)
 * and never touches lodepng buffers. All badge texture creation stays in the
 * fenced pre-draw window ra_badges_upload_pending() (menu.c:772), which runs
 * before vita2d_start_drawing() every frame (v11 Fix A).
 */
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide */

#include <string.h>
#include <stdio.h>

#include <vita2d.h>

#include "ra_internal.h"   /* RA_LOG; ra.h -> ra_badge_get prototype */
#include "ra_toast.h"
#include "../menu.h"       /* WHITE/GREEN/GRAY/DARKGRAY/YELLOW/BLACK, COLOR_ALPHA,
                              ALIGN_CENTER, SCREEN_WIDTH, pgf_draw_text, font */
#include "../main.h"       /* v30 instrumentation: AdrenalineConfig + `config`.
                              menu.h includes only <vita2d.h>, so the type is
                              NOT otherwise visible in this translation unit. */

/* v30 instrumentation: menu_open is defined at menu.c:164 and declared in no
 * header at all (main.c externs it locally the same way), so it is extern'd
 * here to record the mode-0 skip's decision inputs alongside the toast state. */
extern int menu_open;

/* ------------------------------------------------------------------------- */
/* Timing + geometry                                                          */
/* ------------------------------------------------------------------------- */

/* Timing windows (microseconds). Sum = 4.0 s, matching the approved design. */
#define RA_TOAST_SLIDE_US 300000u   /* slide in from the top edge  */
#define RA_TOAST_HOLD_US  3000000u  /* hold on screen              */
#define RA_TOAST_FADE_US  700000u   /* fade out                    */
#define RA_TOAST_TOTAL_US (RA_TOAST_SLIDE_US + RA_TOAST_HOLD_US + RA_TOAST_FADE_US)

#define RA_TOAST_QUEUE_MAX 4        /* pending (not-yet-shown) toasts */

#define RA_TOAST_TITLE_MAX 96       /* matches ra_snapshot_entry.title[96] */
#define RA_TOAST_BADGE_MAX 16       /* matches ra_badge_slot.name / ra_fifo_entry.badge_name */

/* Card geometry on the 960x544 Vita screen. */
#define RA_TOAST_W      620.0f
#define RA_TOAST_H      76.0f
#define RA_TOAST_Y_REST 20.0f       /* resting top edge (top of screen) */
#define RA_TOAST_BADGE  48.0f       /* badge draw size, same as trophy rows */

/* ------------------------------------------------------------------------- */
/* State (all static, render-thread only — no locking)                        */
/* ------------------------------------------------------------------------- */

typedef struct ra_toast_entry {
	char title[RA_TOAST_TITLE_MAX];
	char badge_name[RA_TOAST_BADGE_MAX];
	uint32_t points;
} ra_toast_entry;

/* Pending FIFO ring (single-writer, render thread only — no locking, mirroring
 * the badge-pending array style in ra_badges.c rather than the cross-thread
 * lock-free SPSC ring in ra_internal.h). */
static ra_toast_entry ra_toast_queue[RA_TOAST_QUEUE_MAX];
static int ra_toast_head = 0;    /* next to promote */
static int ra_toast_tail = 0;    /* next free slot  */
static int ra_toast_count = 0;   /* pending entries */

/* The single currently-showing toast. */
static ra_toast_entry ra_toast_current;
static int ra_toast_active = 0;
static uint64_t ra_toast_shown_at_us = 0;

/* ------------------------------------------------------------------------- */
/* Push                                                                       */
/* ------------------------------------------------------------------------- */

void ra_toast_push(const char *title, uint32_t points, const char *badge_name)
{
	ra_toast_entry *e;

	if (!title || !title[0])
		return;

	if (ra_toast_count >= RA_TOAST_QUEUE_MAX) {
		RA_LOG("[RA] toast queue full (%d); dropping \"%s\"\n", RA_TOAST_QUEUE_MAX, title);
		return;   /* bounded queue: drop the NEWEST on overflow */
	}

	e = &ra_toast_queue[ra_toast_tail];
	strncpy(e->title, title, sizeof(e->title) - 1);
	e->title[sizeof(e->title) - 1] = '\0';
	strncpy(e->badge_name, badge_name ? badge_name : "", sizeof(e->badge_name) - 1);
	e->badge_name[sizeof(e->badge_name) - 1] = '\0';
	e->points = points;

	ra_toast_tail = (ra_toast_tail + 1) % RA_TOAST_QUEUE_MAX;
	ra_toast_count++;

	/* v30: record the mode-0 skip's decision inputs at push time. filter==0 with
	 * menu==0 is exactly the state the Original-mode toast question turns on. */
	RA_LOG("[RA] toast queued: \"%s\" (%u pts, badge %s) filter=%d menu=%d\n", e->title,
		(unsigned int)points, e->badge_name, config.graphics_filtering, menu_open);

	/* SOUND HOOK (v17): the chime is played HERE, right after enqueueing, via
	 * its OWN sceAudioOutOpenPort VOICE stream — never the emulator's
	 * main.c/pops.c audio path. This call is non-blocking: it only raises a
	 * flag for the RA_Chime worker, because sceAudioOutOutput() is a documented
	 * blocking call and this is the render thread. If the audio port could not
	 * be opened (port pool full, etc.) the call is a silent no-op and the toast
	 * shows without sound. See chime-sound-feasibility.md §1-§4. */
	ra_chime_play();
}

/* ------------------------------------------------------------------------- */
/* Tick                                                                       */
/* ------------------------------------------------------------------------- */

/* v30 instrumentation: cleared when a card is promoted below, set on that
 * card's first actual draw, so the "toast drawn" line fires ONCE per card
 * rather than on all ~240 frames of its 4 s life. Render-thread only, same as
 * every other symbol in this file. */
static int ra_toast_draw_logged = 0;

void ra_toast_tick(void)
{
	if (ra_toast_active) {
		uint64_t now = (uint64_t)sceKernelGetProcessTimeWide();
		if (now - ra_toast_shown_at_us >= RA_TOAST_TOTAL_US)
			ra_toast_active = 0;   /* finished; promote the next on this call */
	}

	if (!ra_toast_active && ra_toast_count > 0) {
		ra_toast_current = ra_toast_queue[ra_toast_head];
		ra_toast_head = (ra_toast_head + 1) % RA_TOAST_QUEUE_MAX;
		ra_toast_count--;
		ra_toast_active = 1;
		ra_toast_shown_at_us = (uint64_t)sceKernelGetProcessTimeWide();
		ra_toast_draw_logged = 0;   /* v30: re-arm the one-shot draw line */
	}
}

/* True while a toast is showing OR one is queued and about to be promoted.
 * Read by the AdrenalineDraw loop's mode-0 skip condition (menu.c:793) so the
 * Original-filter path temporarily enters the normal composite while a toast
 * needs to be drawn. Render-thread only, same as every other symbol here. */
int ra_toast_is_active(void)
{
	return ra_toast_active || ra_toast_count > 0;
}

/* ------------------------------------------------------------------------- */
/* Animation (pure function of elapsed time)                                  */
/* ------------------------------------------------------------------------- */

/* Map elapsed microseconds -> card top-left y and 8-bit alpha.
 * slide: y from fully above the screen down to Y_REST (ease-out), alpha ramps in.
 * hold:  fixed y, fixed alpha.
 * fade:  fixed y, alpha ramps to 0. */
static void ra_toast_anim(uint64_t elapsed, float *y_out, uint8_t *alpha_out)
{
	const float ALPHA_HOLD = 240.0f;   /* 0xF0 — ~94% opaque card at rest */
	float k, ease;

	if (elapsed < RA_TOAST_SLIDE_US) {
		k = (float)elapsed / (float)RA_TOAST_SLIDE_US;   /* 0..1 */
		if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
		ease = 1.0f - (1.0f - k) * (1.0f - k);           /* ease-out quad */
		*y_out = RA_TOAST_Y_REST - (1.0f - ease) * (RA_TOAST_H + 24.0f);
		*alpha_out = (uint8_t)(ALPHA_HOLD * k);
	} else if (elapsed < (uint64_t)RA_TOAST_SLIDE_US + RA_TOAST_HOLD_US) {
		*y_out = RA_TOAST_Y_REST;
		*alpha_out = (uint8_t)ALPHA_HOLD;
	} else if (elapsed < RA_TOAST_TOTAL_US) {
		uint64_t f = elapsed - RA_TOAST_SLIDE_US - RA_TOAST_HOLD_US;
		k = (float)f / (float)RA_TOAST_FADE_US;          /* 0..1 */
		if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
		*y_out = RA_TOAST_Y_REST;
		*alpha_out = (uint8_t)(ALPHA_HOLD * (1.0f - k));
	} else {
		*y_out = RA_TOAST_Y_REST;
		*alpha_out = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* Draw                                                                       */
/* ------------------------------------------------------------------------- */

void ra_toast_draw(void)
{
	float x, y, tx;
	uint8_t alpha;
	uint64_t elapsed;
	char pts[32];
	vita2d_texture *tex;

	if (!ra_toast_active)
		return;

	elapsed = (uint64_t)sceKernelGetProcessTimeWide() - ra_toast_shown_at_us;
	if (elapsed >= RA_TOAST_TOTAL_US)
		return;   /* tick will clear/promote this frame */

	ra_toast_anim(elapsed, &y, &alpha);
	if (alpha == 0)
		return;

	/* v30: past every early-out, so reaching here means this card really is
	 * being drawn this frame. One line per card (re-armed at promotion in
	 * ra_toast_tick). This is what discriminates a drawn toast from an
	 * undrawn one in a hardware log — ra_toast_draw() previously contained no
	 * RA_LOG at all, so the two states were logged identically. */
	if (!ra_toast_draw_logged) {
		ra_toast_draw_logged = 1;
		RA_LOG("[RA] toast drawn: \"%s\" filter=%d menu=%d\n",
			ra_toast_current.title, config.graphics_filtering, menu_open);
	}

	x = (float)ALIGN_CENTER(SCREEN_WIDTH, RA_TOAST_W);   /* 170.0f for W=620 */

	/* --- card background (fades with alpha) --- */
	vita2d_draw_rectangle(x, y, RA_TOAST_W, RA_TOAST_H, COLOR_ALPHA(BLACK, alpha));

	/* thin gold accent along the top edge (PS-style) */
	vita2d_draw_rectangle(x, y, RA_TOAST_W, 3.0f, COLOR_ALPHA(YELLOW, alpha));

	/* --- badge icon (48 px, vertically centered in the card) --- */
	tex = (vita2d_texture *)ra_badge_get(ra_toast_current.badge_name, 0);
	{
		float by = y + (RA_TOAST_H - RA_TOAST_BADGE) / 2.0f;

		if (tex) {
			float bw = (float)vita2d_texture_get_width(tex);
			float bh = (float)vita2d_texture_get_height(tex);
			float scale = (bw > 0.0f && bh > 0.0f)
				? (RA_TOAST_BADGE / ((bw > bh) ? bw : bh)) : 1.0f;
			/* Fade the badge too — the same tint mechanism pgf text glyphs use
			 * every frame, so it is safe inside this scene. Fallback if hardware
			 * shows a glitch: plain vita2d_draw_texture_scale. */
			vita2d_draw_texture_tint_scale(tex, x + 14.0f, by, scale, scale,
				COLOR_ALPHA(WHITE, alpha));
		} else {
			/* placeholder while the badge is pending (mirrors ra_draw_badge in
			 * ra_trophy_view.c). ra_badges_upload_pending() at menu.c:772 runs
			 * every drawn frame, so a real texture replaces this within a few
			 * frames. */
			vita2d_draw_rectangle(x + 14.0f, by, RA_TOAST_BADGE, RA_TOAST_BADGE,
				COLOR_ALPHA(GRAY, alpha));
			vita2d_draw_fill_circle(x + 14.0f + RA_TOAST_BADGE / 2.0f,
				by + RA_TOAST_BADGE / 2.0f, 10.0f, COLOR_ALPHA(DARKGRAY, alpha));
		}
	}

	/* --- text column right of the badge --- */
	tx = x + 14.0f + RA_TOAST_BADGE + 14.0f;

	/* title: white, single line, shrunk to fit the card so it never bleeds */
	{
		float max_text_w = (x + RA_TOAST_W - 14.0f) - tx;   /* ~530 px */
		float title_scale = FONT_SIZE;
		float w = (float)vita2d_pgf_text_width(font, FONT_SIZE, ra_toast_current.title);
		if (w > max_text_w && w > 0.0f)
			title_scale = FONT_SIZE * (max_text_w / w);
		pgf_draw_text(tx, y + 12.0f, COLOR_ALPHA(WHITE, alpha), title_scale,
			ra_toast_current.title);
	}

	/* points: green, smaller, second line beneath the title */
	snprintf(pts, sizeof(pts), "%u pts", (unsigned int)ra_toast_current.points);
	pgf_draw_text(tx, y + 12.0f + FONT_Y_SPACE * 0.85f, COLOR_ALPHA(GREEN, alpha),
		FONT_SIZE * 0.8f, pts);
}
