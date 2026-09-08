/*
 * Adrenaline+ RetroAchievements — achievement-unlock toast (public API)
 *
 * A PS-style toast slides in at the top of the game screen when an achievement
 * unlocks, holds ~3 s, then fades out (~4 s total). Rapid multi-unlocks are
 * queued and shown one after another.
 *
 * Threading: single-writer (ra.h "single writer" model). ra_toast_push() is
 * called from ra_event_handler() on the render thread; ra_toast_tick() from
 * ra_tick() on the render thread; ra_toast_draw() from inside the AdrenalineDraw
 * scene on the render thread. No locking is required.
 */
#ifndef ADRENALINE_RA_TOAST_H
#define ADRENALINE_RA_TOAST_H

#include <stdint.h>

/* Enqueue an unlock. Copies title/points/badge_name. Drops (and RA_LOGs) when
 * the queue is full (4 pending). Render-thread only. */
void ra_toast_push(const char *title, uint32_t points, const char *badge_name);

/* Per-frame upkeep: clears the active toast once its full lifetime has elapsed,
 * then promotes the next queued entry. Called from ra_tick(). */
void ra_toast_tick(void);

/* Draws the active toast (if any) at the top of the screen. Must be called from
 * inside the vita2d_start_drawing()/vita2d_end_drawing() scene, AFTER the game
 * frame composite and BEFORE the menu draw (menu.c:839-843). Draws only cached
 * badge textures; NEVER creates/destroys GXM objects. */
void ra_toast_draw(void);

/* Non-zero while a toast is showing or queued. Lets the draw loop keep the
 * composite path alive in Original-filter gameplay (menu.c:793). */
int ra_toast_is_active(void);

#endif /* ADRENALINE_RA_TOAST_H */
