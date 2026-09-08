/*
 * Adrenaline+ RetroAchievements — achievement-unlock chime (public API)
 *
 * A short synthesized "ding" (~320 ms, two decaying sine partials) played when
 * an achievement unlocks, through its OWN SceAudio port — never the emulator's
 * main.c/pops.c audio path (those taiHEN import hooks only intercept ScePspemu's
 * own sceAudioOut* calls; ours bypass them entirely).
 *
 * Design source: investigation-result/chime-sound-feasibility.md
 *   §1  never BGM — that is what ScePspemu contends for (see pops.c:83).
 *       v31: try SCE_AUDIO_OUT_PORT_TYPE_MAIN first (its only documented
 *       constraint is freq == 48000 Hz, which this chime already uses), then
 *       SCE_AUDIO_OUT_PORT_TYPE_VOICE as the fallback — VOICE is held for the
 *       process lifetime by pops.c:84's never-released BGM-full fallback port.
 *       Check the return code and SKIP silently on any failure — never fatal.
 *   §2  synthesize the PCM in code (no WAV asset), int16 mono @ 48000 Hz,
 *       precedent: the official vitasdk `audio` sample.
 *   §3  sceAudioOutOutput() is a documented BLOCKING call, so it runs on a
 *       dedicated RA_Chime thread; the render thread only sets a flag.
 *
 * Threading:
 *   ra_chime_init()     — render thread, once, from ra_init().
 *   ra_chime_play()     — render thread (ra_toast_push), non-blocking, just
 *                         raises a flag. Safe to call when audio is unavailable.
 *   ra_chime_shutdown() — render thread, from ra_shutdown().
 * All blocking audio work happens on the RA_Chime worker thread.
 */
#ifndef ADRENALINE_RA_CHIME_H
#define ADRENALINE_RA_CHIME_H

/* Synthesize the chime PCM and start the RA_Chime worker thread. The audio
 * port is opened EAGERLY by the worker at worker start (v30) and kept for the
 * process lifetime; a PORT_FULL at that point does not latch, so the next
 * unlock retries. The old "stay off VOICE so pops.c can have it" rationale is
 * moot — pops.c's fallback port is already held before ra_init() runs. Never
 * fatal: on a non-transient failure the chime is disabled for the session and
 * a reason is logged. */
void ra_chime_init(void);

/* Request one chime. Render-thread only, non-blocking, never fails loudly.
 * Repeated calls inside one playback collapse into a single chime. */
void ra_chime_play(void);

/* Stop the worker and release the port. Safe to call when never initialized. */
void ra_chime_shutdown(void);

#endif /* ADRENALINE_RA_CHIME_H */
