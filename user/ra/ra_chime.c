/*
 * Adrenaline+ RetroAchievements — achievement-unlock chime
 *
 * Implements investigation-result/chime-sound-feasibility.md §1-§4:
 *
 *   - The PCM is SYNTHESIZED IN CODE (no .wav asset), int16 mono @ 48000 Hz,
 *     exactly the technique the official vitasdk `audio` sample uses
 *     (int16_t buffer filled from a waveform generator, pushed with
 *     sceAudioOutOutput). Report §2.
 *
 *   - v31: the port is opened MAIN first, VOICE as the fallback; never BGM,
 *     which is the type ScePspemu's own game audio contends for. MAIN's ONLY
 *     documented constraint is that freq must be 48000 Hz (psp2/audioout.h),
 *     and the chime was already synthesized at exactly 48000 Hz mono, so MAIN
 *     needs no format change. VOICE is tried second because pops.c:79-89's
 *     BGM-full -> VOICE fallback stores its port in the file-static
 *     pops_audio_port and NOTHING in this tree ever releases it, so VOICE is
 *     occupied for the whole process lifetime (web research Finding 2; matches
 *     10/10 hardware sessions of PORT_FULL on VOICE, including at boot with no
 *     game loaded). Report §1, as amended by the v31 plan.
 *
 *   - EVERY failure is silent and non-fatal. If the port cannot be opened, or
 *     output fails, the chime is disabled for the rest of the session with one
 *     logged reason and the toast simply shows without sound (= v16 behavior).
 *     Report §1/§4: "check the return code and treat any negative return as
 *     'no chime this run', never fatal" — the Saboteur note in the report's
 *     adversarial review says explicitly this must not be dropped.
 *
 *   - sceAudioOutOutput() is documented in psp2/audioout.h as a BLOCKING
 *     function, so it runs on a dedicated RA_Chime thread. ra_toast_push() (the
 *     render thread) only raises a flag. Report §3; thread precedent:
 *     ra_net.c:739 "RA_NetWorker", main.c:517/523/534.
 *
 *   - The port is opened EAGERLY, on the worker, at worker start (v30), and
 *     then kept for the process lifetime; per-chime open/close churn was
 *     explicitly advised against (report §3). The original lazy design's
 *     motivation — "leave the VOICE port free in case pops.c wants it as its
 *     BGM-full fallback" — is moot: pops.c's fallback port is already opened
 *     and held before ra_init() ever runs (the ScePspemu audio-init hook at
 *     main.c:874 fires before InitAdrenaline() at main.c:647).
 *
 * NO libm. adrenaline_user links -nostdlib with a deliberately load-bearing
 * library order (user/CMakeLists.txt "LINK ORDER IS LOAD-BEARING"), and libm is
 * NOT on that link line today. Rather than perturb it for two transcendentals,
 * the sine is a self-contained polynomial approximation and the exponential
 * decay is a per-sample multiply by a precomputed constant. Both are exact,
 * deterministic and add zero link dependencies.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#include "ra_internal.h"   /* RA_LOG */
#include "ra_chime.h"

/* ------------------------------------------------------------------------- */
/* Format                                                                     */
/* ------------------------------------------------------------------------- */

/* 48000 Hz mono, 16-bit signed — the official sample's default rate and the
 * only rate SCE_AUDIO_OUT_PORT_TYPE_MAIN would accept, so it is certain to be
 * a supported value (psp2/audioout.h sceAudioOutOpenPort freq list). */
#define RA_CHIME_FREQ    48000

/* Samples handed to sceAudioOutOutput() per call. MUST be a multiple of 64 and
 * within [SCE_AUDIO_MIN_LEN=64, SCE_AUDIO_MAX_LEN=65472] (header). 1024
 * samples = 21.3 ms per block, so a stop request is honoured promptly. */
#define RA_CHIME_BLOCK   1024

/* 14 tone blocks + 1 trailing silence block. 14 * 1024 = 14336 samples of
 * "ding" (298.7 ms, the ~300 ms the report asked for); the final silent block
 * flushes the hardware so the port does not sit on the last non-zero sample. */
#define RA_CHIME_TONE_BLOCKS  14
#define RA_CHIME_BLOCKS       15
#define RA_CHIME_TONE_SAMPLES (RA_CHIME_TONE_BLOCKS * RA_CHIME_BLOCK)  /* 14336 */
#define RA_CHIME_SAMPLES      (RA_CHIME_BLOCKS * RA_CHIME_BLOCK)       /* 15360 */

/* Two harmonically related partials — C6 + G6, the perfect fifth the report
 * proposed in §2. Stored as phase increments in TURNS per sample (f / Fs) so
 * the oscillator never needs a modulo of a large accumulated angle. */
#define RA_CHIME_F1_INC  0.02180208f   /* 1046.5 Hz / 48000 */
#define RA_CHIME_F2_INC  0.03266667f   /* 1568.0 Hz / 48000 */

/* Per-sample exponential-decay factors, precomputed here instead of calling
 * expf()/powf() at runtime (see the "NO libm" note in the file header):
 *   d1 = 0.001 ^ (1/14336)  = exp(-6.907755 / 14336) = 0.99951828
 *   d2 = exp(-0.00070024)                            = 0.99930000
 * d1 takes the fundamental to -60 dB exactly at the end of the tone region;
 * d2 makes the upper partial die sooner, which is what gives a struck-bell
 * character rather than a flat two-tone beep. */
#define RA_CHIME_DECAY1  0.99951828f
#define RA_CHIME_DECAY2  0.99930000f

/* Mix weights (sum = 1.0, so the pre-scale signal never exceeds +-1.0) and the
 * overall amplitude. 0.55 leaves ~5 dB of headroom under full scale: the chime
 * has to sit ON TOP of the game's own audio, not replace it. */
#define RA_CHIME_W1      0.62f
#define RA_CHIME_W2      0.38f
#define RA_CHIME_AMP     0.55f

/* 2 ms linear fade-in / 5 ms linear fade-out, in samples. Removes the click a
 * hard buffer edge would otherwise produce (report §2). */
#define RA_CHIME_ATTACK  96
#define RA_CHIME_RELEASE 240

/* Port volume, out of SCE_AUDIO_OUT_MAX_VOL (32768). ~70%: audible over the
 * game without dominating it. */
#define RA_CHIME_VOLUME  22937

/* Worker poll period. The chime is a human-scale notification, so 5 ms of
 * latency is inaudible; this mirrors ra_net.c:1221's polling worker rather
 * than adding a semaphore the rest of this subsystem does not use. */
#define RA_CHIME_POLL_US 5000

#define RA_CHIME_STACK   0x2000

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

/* 15360 samples * 2 bytes = 30720 bytes of .bss. Static, never freed, never
 * reallocated — this module performs zero heap allocation (and is therefore
 * structurally immune to the v14 cross-heap free class of bug). */
static int16_t ra_chime_pcm[RA_CHIME_SAMPLES];

static SceUID ra_chime_thread = -1;
static int    ra_chime_port   = -1;

/* v31: which port TYPE the held handle was opened on ("MAIN" or "VOICE"), for
 * the success log. Worker-thread only, written immediately after a successful
 * sceAudioOutOpenPort, points at a string literal in .rodata. */
static const char *ra_chime_port_type_name = "?";

/* Set once, on the worker, when the port could not be opened or output failed.
 * Sticky: the report warns against retry-spamming sceAudioOutOpenPort. */
static int ra_chime_disabled = 0;

/* Render thread WRITES, worker thread READS+CLEARS. A single-word flag with no
 * other state attached, mirroring ra_net.c's `ra_worker_stop` — the worst a
 * race can do is play one chime a few milliseconds early or late. */
static volatile int ra_chime_request = 0;
static volatile int ra_chime_stop    = 0;

/* ------------------------------------------------------------------------- */
/* Synthesis (worker thread, once)                                            */
/* ------------------------------------------------------------------------- */

static float ra_chime_absf(float x)
{
	return (x < 0.0f) ? -x : x;
}

/*
 * sin(2*pi*turns) for turns in [0,1), without libm.
 *
 * Folds the turn into x = angle in [-pi, pi], then applies the well-known
 * parabolic approximation with the one extra correction term:
 *     y = B*x + C*x*|x|            (B = 4/pi, C = -4/pi^2)
 *     y = P*(y*|y| - y) + y        (P = 0.225)
 * Peak absolute error ~0.001 of full scale — roughly 60 dB down, i.e. below
 * the 16-bit quantisation floor this buffer is written at, so the audible
 * result is a clean sine.
 */
static float ra_chime_sin_turns(float turns)
{
	const float B = 1.2732395447f;   /*  4 / pi   */
	const float C = -0.4052847346f;  /* -4 / pi^2 */
	const float P = 0.225f;
	float x, y;

	if (turns >= 0.5f)
		turns -= 1.0f;               /* fold to [-0.5, 0.5) turns */
	x = turns * 6.2831853072f;       /* -> [-pi, pi) radians */

	y = B * x + C * x * ra_chime_absf(x);
	y = P * (y * ra_chime_absf(y) - y) + y;
	return y;
}

static void ra_chime_synth(void)
{
	float p1 = 0.0f, p2 = 0.0f;   /* oscillator phases, in turns */
	float e1 = 1.0f, e2 = 1.0f;   /* decay envelopes */
	int i;

	memset(ra_chime_pcm, 0, sizeof(ra_chime_pcm));

	for (i = 0; i < RA_CHIME_TONE_SAMPLES; i++) {
		float s = RA_CHIME_W1 * e1 * ra_chime_sin_turns(p1)
		        + RA_CHIME_W2 * e2 * ra_chime_sin_turns(p2);
		float g = RA_CHIME_AMP;
		int v;

		/* click-free edges */
		if (i < RA_CHIME_ATTACK)
			g *= (float)i / (float)RA_CHIME_ATTACK;
		else if (i >= RA_CHIME_TONE_SAMPLES - RA_CHIME_RELEASE)
			g *= (float)(RA_CHIME_TONE_SAMPLES - i) / (float)RA_CHIME_RELEASE;

		v = (int)(s * g * 32767.0f);
		if (v > 32767) v = 32767;
		else if (v < -32768) v = -32768;
		ra_chime_pcm[i] = (int16_t)v;

		p1 += RA_CHIME_F1_INC; if (p1 >= 1.0f) p1 -= 1.0f;
		p2 += RA_CHIME_F2_INC; if (p2 >= 1.0f) p2 -= 1.0f;
		e1 *= RA_CHIME_DECAY1;
		e2 *= RA_CHIME_DECAY2;
	}
	/* samples [RA_CHIME_TONE_SAMPLES, RA_CHIME_SAMPLES) stay zero: the
	 * trailing silence block memset above already left them at 0. */
}

/* ------------------------------------------------------------------------- */
/* Port (worker thread only)                                                  */
/* ------------------------------------------------------------------------- */

/* Returns 1 when ra_chime_port is usable, 0 when the chime must be skipped.
 * NEVER fatal — every negative return from the SDK ends up here as a logged
 * skip reason plus a sticky disable. */
static int ra_chime_port_ready(void)
{
	int vol[2];
	int r;

	if (ra_chime_port >= 0)
		return 1;
	if (ra_chime_disabled)
		return 0;

	/* v31: MAIN first, VOICE second. Both are tried with the chime's existing
	 * format (48000 Hz mono, 1024-sample blocks): 48000 Hz is the ONLY rate
	 * SCE_AUDIO_OUT_PORT_TYPE_MAIN accepts (psp2/audioout.h), and the chime was
	 * already designed at exactly that rate, so MAIN needs no format change.
	 *
	 * Why VOICE is no longer tried first: on hardware VOICE returned PORT_FULL
	 * at EVERY attempt in 10/10 sessions, including at boot with no game loaded.
	 * pops.c:79-89 (unmodified upstream TheFloW code, byte-identical in the
	 * isage fork) opens a VOICE port as its BGM-full fallback and stores it in
	 * the file-static pops_audio_port, and NOTHING in this tree ever releases it
	 * — so within this process VOICE is occupied for the process lifetime. That
	 * also corrects the v30 comment here, which claimed "PORT_FULL is TRANSIENT
	 * — a port frees the moment the game's audio engine releases a channel":
	 * true only if the occupant is the game's engine, and the observed occupant
	 * is Adrenaline's own never-released fallback port. Non-latching PORT_FULL
	 * is KEPT anyway — it costs one syscall pair per unlock and a different
	 * occupant might genuinely release.
	 *
	 * Nothing in this tree, in upstream, or in the isage fork ever opens MAIN. */
	{
		static const int ra_chime_types[2] = {
			SCE_AUDIO_OUT_PORT_TYPE_MAIN, SCE_AUDIO_OUT_PORT_TYPE_VOICE
		};
		static const char *const ra_chime_type_names[2] = { "MAIN", "VOICE" };
		int saw_port_full = 0;
		int i;

		/* v31 probe: which port TYPES are in use right now? sceAudioOutGetAdopt
		 * takes a TYPE, not a handle (psp2/audioout.h: "(1) if port is in use,
		 * (0) otherwise, < 0 on error"). Logged on EVERY attempt so the boot
		 * reading (no game loaded) and the unlock-time reading (in-game) can be
		 * compared. %d, not %u: a negative return must stay visible. This is
		 * CONTEXT, not ground truth — the open result on the next lines is what
		 * actually decides, and the header does not say whether "in use" is
		 * scoped to this process or system-wide. */
		RA_LOG("[RA] chime adopt: main=%d bgm=%d voice=%d\n",
			sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_MAIN),
			sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM),
			sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_VOICE));

		r = SCE_AUDIO_OUT_ERROR_PORT_FULL;
		for (i = 0; i < 2; i++) {
			r = sceAudioOutOpenPort((SceAudioOutPortType)ra_chime_types[i],
				RA_CHIME_BLOCK, RA_CHIME_FREQ, SCE_AUDIO_OUT_MODE_MONO);
			if (r >= 0)
				break;
			if ((unsigned int)r == SCE_AUDIO_OUT_ERROR_PORT_FULL)
				saw_port_full = 1;
			/* NOT one-shot gated: attempts happen once at boot plus once per
			 * unlock, which is human-scale, and seeing the per-type result at
			 * unlock time as well as at boot is exactly what v31 is for. */
			RA_LOG("[RA] chime: %s port open -> 0x%08X\n",
				ra_chime_type_names[i], (unsigned int)r);
		}
		if (r < 0) {
			if (saw_port_full) {
				/* v30 semantics preserved: PORT_FULL NEVER latches. A retry on
				 * the next unlock can still succeed. One summary line per
				 * process, so this cannot become a retry storm. */
				static int ra_chime_port_full_logged = 0;
				if (!ra_chime_port_full_logged) {
					ra_chime_port_full_logged = 1;
					RA_LOG("[RA] chime: MAIN and VOICE both unavailable (port full) - will retry on the next unlock\n");
				}
				return 0;
			}
			/* Neither type was transiently busy: the SDK rejected us outright.
			 * This is the only latch site in this function. */
			ra_chime_disabled = 1;
			RA_LOG("[RA] chime skipped: sceAudioOutOpenPort failed on both types (last 0x%08X) - toast will be silent this session\n",
				(unsigned int)r);
			return 0;
		}
		ra_chime_port = r;
		ra_chime_port_type_name = ra_chime_type_names[i];
	}

	vol[0] = RA_CHIME_VOLUME;
	vol[1] = RA_CHIME_VOLUME;
	r = sceAudioOutSetVolume(ra_chime_port,
		(SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
	if (r < 0)   /* non-fatal: the port defaults to max volume anyway */
		RA_LOG("[RA] chime volume set failed 0x%08X (continuing at default)\n", (unsigned int)r);

	RA_LOG("[RA] chime port open: %s port=%d %d Hz mono, %d-sample blocks\n",
		ra_chime_port_type_name, ra_chime_port, RA_CHIME_FREQ, RA_CHIME_BLOCK);
	return 1;
}

/* ------------------------------------------------------------------------- */
/* Worker                                                                     */
/* ------------------------------------------------------------------------- */

static int ra_chime_worker_main(unsigned int args, void *argp)
{
	(void)args;
	(void)argp;

	ra_chime_synth();

	/* v30: open the port EAGERLY (v31: MAIN first, VOICE second), here at
	 * process start, instead of
	 * lazily at the first unlock. The lazy design (file header, :28-33)
	 * deferred the request past the one window when the pool is provably
	 * quiet — boot, before the PSP title has booted its audio — and landed it
	 * at the moment of MAXIMUM contention inside a process that shares
	 * ScePspemu's SceAudio port budget. Result on hardware: the chime has
	 * NEVER once opened a port across 8 sessions.
	 *
	 * A failure HERE must never be sticky: at boot we may simply be too early,
	 * and latching would be strictly worse than v29 (which at least tried at
	 * unlock time). Clear the flag and let the first unlock retry. */
	if (!ra_chime_port_ready())
		ra_chime_disabled = 0;

	while (!ra_chime_stop) {
		int b;

		if (!ra_chime_request) {
			sceKernelDelayThread(RA_CHIME_POLL_US);
			continue;
		}
		ra_chime_request = 0;

		if (!ra_chime_port_ready())
			continue;   /* reason already logged, once */

		RA_LOG("[RA] chime\n");

		for (b = 0; b < RA_CHIME_BLOCKS && !ra_chime_stop; b++) {
			/* BLOCKING (psp2/audioout.h): this is exactly why the call
			 * lives on this thread and not on the render thread. */
			int r = sceAudioOutOutput(ra_chime_port,
				&ra_chime_pcm[b * RA_CHIME_BLOCK]);
			if (r < 0) {
				RA_LOG("[RA] chime output failed 0x%08X at block %d - chime disabled\n",
					(unsigned int)r, b);
				ra_chime_disabled = 1;
				break;
			}
		}
	}

	if (ra_chime_port >= 0) {
		sceAudioOutReleasePort(ra_chime_port);
		ra_chime_port = -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API (render thread)                                                 */
/* ------------------------------------------------------------------------- */

void ra_chime_init(void)
{
	if (ra_chime_thread >= 0)
		return;

	ra_chime_stop = 0;
	ra_chime_request = 0;
	/* v30: must be reset BEFORE the create below, so the create-failure latch
	 * at the end of this function still takes effect. Latent today (nothing
	 * calls ra_shutdown()), but a future re-init would otherwise come back
	 * permanently silent with no logged reason. */
	ra_chime_disabled = 0;

	/* Priority 0x80 and the "create + start, log on failure, carry on"
	 * shape both mirror ra_net.c:739-753's RA_NetWorker. */
	ra_chime_thread = sceKernelCreateThread("RA_Chime", ra_chime_worker_main,
		0x80, RA_CHIME_STACK, 0, 0, NULL);
	if (ra_chime_thread < 0) {
		RA_LOG("[RA] chime skipped: sceKernelCreateThread failed 0x%08X\n",
			(unsigned int)ra_chime_thread);
		ra_chime_thread = -1;
		ra_chime_disabled = 1;
		return;
	}
	sceKernelStartThread(ra_chime_thread, 0, NULL);
	RA_LOG("[RA] chime thread started (%d samples synthesized, opening MAIN/VOICE port now)\n",
		RA_CHIME_SAMPLES);
}

void ra_chime_play(void)
{
	if (ra_chime_thread < 0 || ra_chime_disabled)
		return;   /* audio unavailable: toast shows silently */

	ra_chime_request = 1;   /* worker picks this up within RA_CHIME_POLL_US */
}

void ra_chime_shutdown(void)
{
	if (ra_chime_thread < 0)
		return;

	ra_chime_stop = 1;
	{
		SceUInt timeout = 1000 * 1000;   /* 1 s; a block is only ~21 ms */
		sceKernelWaitThreadEnd(ra_chime_thread, NULL, &timeout);
	}
	sceKernelDeleteThread(ra_chime_thread);
	ra_chime_thread = -1;

	/* v30: the worker releases the port on its own clean exit (:287-290), but
	 * if the wait above timed out we have just deleted it while it still held
	 * the port. Release here so ownership never depends on the worker winning
	 * that race. This must come AFTER the delete: on a timeout the worker may
	 * be blocked inside sceAudioOutOutput() on this very port, and releasing
	 * it out from under a live thread is a worse race than the leak. No-op
	 * after a clean exit (the worker already set the port to -1). */
	if (ra_chime_port >= 0) {
		sceAudioOutReleasePort(ra_chime_port);
		ra_chime_port = -1;
	}
}
