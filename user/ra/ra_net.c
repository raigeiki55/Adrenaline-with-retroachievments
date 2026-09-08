/*
 * Adrenaline+ RetroAchievements — networking layer (v9: curl + OpenSSL)
 * =====================================================================
 *
 * Lazy network bootstrap + single worker thread that performs all HTTP I/O.
 * rc_client never touches the network directly: ra_server_call deep-copies
 * the request (rc_client destroys the request immediately after the
 * server_call callback returns) and hands it to the worker; the worker posts
 * the completion to a lock-free FIFO drained by ra_tick() on the render
 * thread. That architecture is UNCHANGED in v9 and is load-bearing: every
 * rc_client_* call still happens on the render thread (single-writer rule).
 *
 *
 * WHAT CHANGED IN v9 — the transport, and only the transport
 * ----------------------------------------------------------
 * v3..v8 performed every request through Sony's SceHttp/SceSsl. That path was
 * never able to come up inside the ScePspemu process on this hardware:
 * sceSysmoduleLoadModule(SCE_SYSMODULE_SSL) and (SCE_SYSMODULE_HTTPS) both
 * returned 0x8002D0F3 — a SceKernelModulemgr PRELOAD-class error — on every
 * logged hardware run. Both leading hypotheses were tested on real hardware and
 * both were refuted: the ScePaf load-ordering theory (v8 restored the v4 PAF
 * load on the deferred path; SSL still failed) and the iTLS-Enso interception
 * theory (the user removed the in-app iTLS http/ssl modules; SSL still failed).
 * Trail: investigation-result/network-architecture-wayforward.md.
 *
 * So v9 stops asking Sony for TLS and ships its own: libcurl 8.17.0 over
 * OpenSSL 1.0.2i, both from the official vitasdk package repository, both
 * depending only on SCE_SYSMODULE_NET — the one sysmodule that HAS loaded
 * successfully inside this exact process on every hardware run this project
 * has logged.
 *
 * That decision was not taken on theory. ra_smoke.c (now retired, see below)
 * ran the exact same curl+OpenSSL stack from inside this same .suprx, in the
 * ScePspemu process, on the user's console, and climbed a three-rung ladder:
 *
 *     phase1 SOCKET  http://  no TLS               -> PASS (DNS + TCP work)
 *     phase2 TLS     https:// verifypeer=0         -> PASS (TLSv1.2 handshake)
 *     phase3 VERIFY  https:// verifypeer=1 + blob  -> PASS (chain validated)
 *
 * v9 ships phase3's exact shape: verification ON, trust anchors supplied as an
 * in-memory blob.
 *
 *
 * THE ONE RULE THAT MUST NOT BE BROKEN: CA MATERIAL COMES FROM MEMORY
 * ------------------------------------------------------------------
 * CURLOPT_CAINFO (a FILE PATH) is never set anywhere in this file, and must
 * never be. A path makes OpenSSL read the store through a stdio FILE BIO:
 *
 *     Curl_ssl_setup_x509_store -> X509_STORE_load_locations -> by_file_ctrl
 *       -> X509_load_cert_file -> BIO_s_file -> file_fopen  (Sony fopen)
 *       ... then PEM_read_bio -> BIO_gets -> file_gets      (newlib fgets)
 *
 * In this module fopen() binds to Sony's SceLibc stub while fgets() binds to
 * newlib's libc.a (see the LINK ORDER block in user/CMakeLists.txt for why both
 * providers are on the link line at once). file_gets() therefore hands a *Sony*
 * FILE* to *newlib's* fgets(), which reads AND WRITES through it using newlib's
 * struct __sFILE layout — a wild dereference plus memory corruption. That is
 * what killed smoke test #2 mid-transfer, root-caused to instruction level in
 * investigation-result/smoke2-success-analysis.md.
 *
 * CURLOPT_CAINFO_BLOB routes the same PEM through BIO_new_mem_buf() instead.
 * BIO_s_mem's mem_gets() makes ZERO stdio calls, so the mismatch is
 * structurally unreachable rather than merely dodged.
 *
 *   ==> If certificate verification ever fails, fix the BUNDLE (ra_ca.h).
 *       NEVER "fix" it by pointing CURLOPT_CAINFO at a file. Any path
 *       re-enters the broken file BIO. ra_curl_request() below therefore
 *       ABORTS the request outright if CURLOPT_CAINFO_BLOB is rejected,
 *       rather than letting curl silently fall back to its compiled-in
 *       -DCURL_CA_BUNDLE vs0: default.
 *
 *
 * WHAT WAS DELETED IN v9 (all of it dead once curl owns the transport)
 * -------------------------------------------------------------------
 *   - the whole SceHttp request path (sceHttpCreateTemplate /
 *     CreateConnectionWithURL / CreateRequestWithURL / SendRequest /
 *     ReadData / GetStatusCode / Delete*), in all three entry points
 *   - sceHttpInit / sceSslInit / sceHttpsEnableOption / sceHttpsGetSslError
 *   - the stage 0 ScePaf load (it existed ONLY to make SCE_SYSMODULE_SSL load)
 *   - the stage 2a/2b/2c SSL/HTTP/HTTPS module ladder and the stage 2d retry
 *   - the ra_net_ssl_ready gate and both of its https:// abort sites (curl does
 *     not need a Sony TLS provider, so there is nothing left to gate on)
 *   - ra_http_template
 *
 * SCE_SYSMODULE_NET stays: curl reaches the wire through newlib's socket shims
 * (socket/connect/send/recv/select/getaddrinfo), which are thin wrappers over
 * sceNetSocket/sceNetConnect/sceNetSend/sceNetRecv/sceNetResolver*.
 *
 *
 * BOOT SAFETY — unchanged, and deliberately so
 * --------------------------------------------
 * Nothing here runs at module_start(). ra_net_ensure_started() is reached only
 * from the deferred path (trophy menu open / login / auto-login in ra_tick).
 * v4 did network work at module_start and the console black-screened after the
 * Adrenaline logo (v4-black-screen-opus.md). That property is preserved.
 *
 * curl/OpenSSL/z/zstd are STATIC archives: they add no module import of their
 * own. The two module imports they pull in transitively via newlib (SceLibRng,
 * SceThreadmgrCoredumpTime) are forced WEAK in user/CMakeLists.txt for exactly
 * the same reason — a non-weak import on a library that is not resident inside
 * ScePspemu makes the loader refuse the module, which is a black screen.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <curl/curl.h>

/* v12 Fix 2: OpenSSL 1.0.2i threading callbacks. crypto.h is the header that
 * declares CRYPTO_num_locks / CRYPTO_set_locking_callback / CRYPTO_THREADID_*;
 * libcrypto.a is already on this target's link line (curl's TLS backend), so
 * this adds no new archive and no new module import. */
#include <openssl/crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_internal.h"
#include "ra_ca.h"
#include "../utils.h"
#include "../../adrenaline_version.h"   /* v32: ADRENALINE_VERSION_*_STR for the UA */

/*
 * Per-stage bootstrap failure codes.
 *
 * ra_client.c renders these as "Network init failed (stage %d)" using -nr, so
 * the number a user reports maps straight onto a [RA] stageN line in
 * ux0:data/adrenaline_user_log.txt.
 *
 * v9 note: the codes for the stages that no longer exist (HTTPS sysmodule,
 * sceSslInit, sceHttpCreateTemplate) are retired rather than renumbered, so an
 * old bug report quoting "stage 2" or "stage 7" still decodes correctly against
 * this table instead of silently meaning something new.
 */
#define RA_NET_ERR_SYSMODULE_NET   (-1)
#define RA_NET_ERR_SYSMODULE_HTTPS (-2) /* RETIRED in v9 (no SceHttp/SceSsl) */
#define RA_NET_ERR_NETINIT         (-3) /* reserved: stage 3 is non-fatal */
#define RA_NET_ERR_NETCTLINIT      (-4) /* reserved: stage 4 is non-fatal */
#define RA_NET_ERR_HTTPINIT        (-5) /* RETIRED in v9 (no sceHttpInit) */
#define RA_NET_ERR_CURLINIT        (-6) /* curl_global_init failed */
#define RA_NET_ERR_TEMPLATE        (-7) /* RETIRED in v9 (no HTTP template) */
#define RA_NET_ERR_THREAD          (-8)

/* RA_LOG (dual sink: sceClibPrintf + debugPrintf -> ux0:data/adrenaline_user_log.txt)
 * was hoisted to ra_internal.h in v6 so the login/game-load chain in ra_client.c
 * is equally observable. See that header for the rationale. */

#define RA_NET_MEM_SIZE       (512 * 1024)
#define RA_HTTP_MAX_BODY      (512 * 1024)

/* curl timeouts are in SECONDS (the sceHttp ones were microseconds). Values
 * mirror the smoke test, which completed a full RA round trip inside them on
 * hardware (dns+tcp+tls well under 1 s). Bounded so a wedged handshake ends in
 * a logged curl=28 timeout, not a worker thread that never returns. */
#define RA_HTTP_CONNECT_TIMEOUT_SEC 15L
#define RA_HTTP_TOTAL_TIMEOUT_SEC   30L
/* RETIRED in v12 (Fix 1): no request path follows redirects any more, so
 * CURLOPT_MAXREDIRS is never set. Kept as documentation of the old value
 * rather than deleted, so a v11 log/bug report still decodes. */
#define RA_HTTP_MAX_REDIRECTS       5L

/* v32: the UA version now tracks the app version (RA compliance Section C:
 * "numeric and incrementing"). Resolves to AdrenalinePlus-RA/8.0.2 today and
 * follows every future bump of adrenaline_version.h automatically. The
 * product name is unchanged (still unique — G5/G6). adrenaline_version.h
 * defines function-like str()/xstr() macros; verified at v31 that this file
 * has zero str(/xstr( identifiers, so nothing collides. The single
 * CURLOPT_USERAGENT use site (ra_curl_request) is untouched. */
#define RA_HTTP_USER_AGENT "AdrenalinePlus-RA/" ADRENALINE_VERSION_MAJOR_STR "." ADRENALINE_VERSION_MINOR_STR "." ADRENALINE_VERSION_MICRO_STR

/*
 * Net worker stack: 512 KiB, up from the 128 KiB the sceHttp-era worker used.
 *
 * NOT arbitrary. OpenSSL's handshake path (X.509 chain parsing, BIGNUM
 * arithmetic) is far more stack-hungry than a sceHttp call, which did all its
 * work inside Sony's own module on Sony's own stacks. ra_smoke.c sized its
 * thread at 512 KiB for exactly this reason and completed a verified handshake
 * on hardware at that size; undersizing here would surface as an unexplained
 * worker crash mid-login and would be misread as "curl does not work in
 * ScePspemu". The same worker also runs the rc_hash job, which is only cheaper.
 */
#define RA_WORKER_STACK_SIZE (512 * 1024)

/*
 * v24: ra_net_started is now read from TWO threads.
 *
 * Before v24 the bootstrap had exactly one caller context (the render thread,
 * on first trophy-menu open), so a plain int was sufficient. v24 moves the
 * bootstrap onto a dedicated boot worker (RA_BootNet, ra_client.c) so that
 * login -- and therefore achievement tracking -- is live from game boot instead
 * of from the first menu-open. The render thread now polls this flag every
 * frame (ra_tick's game-load gate) while the boot worker may be setting it, so
 * it must be volatile: an aligned 32-bit access is atomic on Cortex-A9, but
 * without the qualifier the compiler is free to hoist the load out of the draw
 * loop and never observe the boot worker's store.
 */
static volatile int ra_net_started = 0;
static int ra_net_modules_loaded = 0;

/*
 * v24: bootstrap serialization.
 *
 * ra_net_ensure_started() used to be documented as single-threaded ("curl and
 * OpenSSL are brought up on the render thread, before the worker exists, so no
 * two threads can race into curl_global_init()"). That invariant is exactly
 * what the v24 boot worker would break: RA_BootNet calls the bootstrap while
 * the render thread can still reach it through ra_begin_login() (manual login),
 * ra_net_queue_hash_job() or ra_server_call().
 *
 * Rather than weaken the invariant, it is enforced with a lock: the whole
 * bootstrap body runs under this mutex, and the ra_net_started re-check inside
 * makes the second caller a no-op instead of a second curl_global_init().
 *
 * Created once from ra_net_init_locks(), called by ra_init() on the render
 * thread BEFORE the boot worker exists -- so the creation itself cannot race.
 * If creation fails the bootstrap still runs, just unserialized (v23
 * behaviour); that is strictly no worse than shipping without the lock.
 */
static SceUID ra_net_boot_mutex = -1;

void ra_net_init_locks(void)
{
	if (ra_net_boot_mutex >= 0)
		return;

	/* RECURSIVE for the same reason RA_SSL_MUTEX_ATTR is (see its definition
	 * further down this file): a hypothetical same-thread re-entry becomes a
	 * counted re-lock instead of a hard deadlock inside the boot path, which
	 * would present as a hung console. The literal is spelled out here rather
	 * than using RA_SSL_MUTEX_ATTR because that macro is defined below, next
	 * to the OpenSSL callbacks it belongs to. */
	ra_net_boot_mutex = sceKernelCreateMutex("RA_NetBoot", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
	if (ra_net_boot_mutex < 0)
		RA_LOG("[RA] v24 boot mutex create FAILED: 0x%08X (bootstrap runs unserialized)\n",
			ra_net_boot_mutex);
}

int ra_net_is_started(void)
{
	return ra_net_started;
}

/*
 * curl_global_init() has completed successfully this session.
 *
 * Replaces v7's ra_net_ssl_ready. The semantics that ra_client.c depends on are
 * preserved -- "is TLS usable at all right now?" -- but the answer no longer
 * depends on Sony shipping us a TLS provider, only on our own statically linked
 * OpenSSL having initialised. Written by the render thread in
 * ra_net_ensure_started() BEFORE the worker is created, read by the worker;
 * volatile because the two threads share no lock and a single aligned int
 * load/store is atomic on Cortex-A9.
 */
static volatile int ra_curl_ready = 0;

int ra_net_ssl_is_ready(void)
{
	return ra_curl_ready;
}

static char ra_net_mem[RA_NET_MEM_SIZE] __attribute__((aligned(64)));

static ra_fifo ra_completion_fifo; /* worker -> render */
static ra_fifo ra_work_fifo;       /* render -> worker */

static SceUID ra_worker_thread = -1;
static volatile int ra_worker_stop = 0;

/*
 * adrenaline_user is linked -nostdlib -nostartfiles, so crt0.o is excluded.
 * newlib's lib_a-syscalls.o (pulled in transitively by curl/OpenSSL for
 * open/read/write/lseek/fstat) references _free_vita_newlib, which is defined
 * ONLY in crt0.o. The single reference is on newlib's _exit() teardown path,
 * which this module can never reach -- a .suprx injected into ScePspemu is
 * unloaded by the kernel, it does not call exit(). Providing a no-op here is
 * therefore the correct resolution, not a papering-over: the function is
 * unreachable, and without it the link fails with one undefined symbol.
 *
 * Same class of fix as the clock_gettime and _ctype_ shims in ra_client.c, and
 * for the same -nostdlib reason. Lived in ra_smoke.c until v9 retired that
 * file; moved here because ra_net.c is now the reason curl is on the link line.
 */
void _free_vita_newlib(void)
{
}

/* ------------------------------------------------------------------------- */
/* TLS failure reporting                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Last TLS-class CURLcode seen, 0 = none.
 *
 * Written by the worker thread, read by the render thread (ra_client.c's login
 * failure message); a torn read is harmless because it is diagnostic only.
 *
 * ONLY TLS-class codes are latched. A plain connectivity failure (CURLE_
 * COULDNT_RESOLVE_HOST, CURLE_OPERATION_TIMEDOUT, ...) must NOT end up
 * described as a certificate problem -- ra_client.c prints ra_net_ssl_error_text()
 * in preference to rc_client's own message, so latching a non-TLS error here
 * would actively mislead.
 */
static volatile int ra_last_curl_tls_error = 0;

static void ra_note_curl_error(CURLcode rc)
{
	switch (rc) {
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_PEER_FAILED_VERIFICATION: /* == CURLE_SSL_CACERT, 60 */
	case CURLE_USE_SSL_FAILED:
	case CURLE_SSL_CACERT_BADFILE:
		ra_last_curl_tls_error = (int)rc;
		break;
	default:
		break;
	}
}

unsigned int ra_net_last_ssl_error(void)
{
	return (unsigned int)ra_last_curl_tls_error;
}

const char *ra_net_ssl_error_text(void)
{
	switch (ra_last_curl_tls_error) {
	case 0:
		return NULL;
	case CURLE_SSL_CONNECT_ERROR:
		return "TLS: handshake failed (curl 35)";
	case CURLE_SSL_CERTPROBLEM:
		return "TLS: local certificate problem (curl 58)";
	case CURLE_SSL_CIPHER:
		return "TLS: no shared cipher (curl 59)";
	case CURLE_PEER_FAILED_VERIFICATION:
		/* The embedded bundle in ra_ca.h did not chain to the server's cert.
		 * The remedy is regenerating ra_ca.h with the current root -- NOT
		 * relaxing verification, and NOT pointing CURLOPT_CAINFO at a file. */
		return "TLS: server certificate not trusted by the built-in CA bundle (curl 60)";
	case CURLE_USE_SSL_FAILED:
		return "TLS: could not start SSL (curl 64)";
	case CURLE_SSL_CACERT_BADFILE:
		return "TLS: built-in CA bundle unparseable (curl 77) - ra_ca.h is corrupt";
	default:
		return "TLS: handshake failed";
	}
}

/* ------------------------------------------------------------------------- */
/* v12 FIX 2 -- OpenSSL 1.0.2i threading callbacks                            */
/* ------------------------------------------------------------------------- */

/*
 * WHY THIS EXISTS
 * ---------------
 * The TLS backend under curl on this target is OpenSSL 1.0.2i (verified:
 * $VITASDK/arm-vita-eabi/include/openssl/opensslv.h defines
 * OPENSSL_VERSION_NUMBER 0x10002090L, and the runtime logs
 * "libcurl/8.17.0 OpenSSL/1.0.2i ...").
 *
 * OpenSSL BEFORE 1.1.0 is not thread-safe on its own. curl's own thread-safety
 * page states it plainly: "OpenSSL 1.1.0+ can be safely used in multi-threaded
 * applications provided that support for the underlying OS threading API is
 * built-in. For older versions of OpenSSL, the user must set mutex callbacks."
 * (curl.se/libcurl/c/threadsafe.html; quoted in
 *  investigation-result/badge-download-crash-research.md §1.)
 *
 * With no callbacks installed, OpenSSL's CRYPTO_lock() is a NO-OP: every
 * CRYPTO_w_lock/CRYPTO_r_lock around the shared session cache, the error-state
 * table, the RAND state and the ex_data registry degrades to nothing. That was
 * the state of the v11 binary -- verified by disassembly, not assumed:
 *
 *     $ arm-vita-eabi-objdump -d build/user/adrenaline_user | \
 *           grep -cE 'bl 	[0-9a-f]+ <CRYPTO_set_locking_callback>'
 *     0
 *
 * Whether two threads in THIS process actually enter OpenSSL concurrently is
 * argued below and is NOT fully settled (see the honesty note). Installing the
 * callbacks costs 41 kernel mutexes and a handful of instructions per lock, and
 * removes the entire question. That trade is worth making unconditionally.
 *
 * API SHAPE -- taken verbatim from the headers in the build image, not from
 * memory ($VITASDK/arm-vita-eabi/include/openssl/crypto.h):
 *
 *     int  CRYPTO_num_locks(void);                          (:435)
 *     void CRYPTO_set_locking_callback(void (*)(int mode, int type,
 *                                      const char *file, int line));  (:437)
 *     void CRYPTO_THREADID_set_numeric(CRYPTO_THREADID *, unsigned long); (:453)
 *     int  CRYPTO_THREADID_set_callback(void (*)(CRYPTO_THREADID *));     (:455)
 *     void CRYPTO_set_id_callback(unsigned long (*)(void));               (:462)
 *     void CRYPTO_set_dynlock_{create,lock,destroy}_callback(...);    (:474-482)
 *     #define CRYPTO_LOCK 1 / CRYPTO_UNLOCK 2                        (:230-231)
 *     #define CRYPTO_NUM_LOCKS 41                                        (:228)
 *
 * BOTH threadid callbacks are installed on purpose. Disassembling
 * libcrypto.a(cryptlib.o) shows CRYPTO_THREADID_current() prefers
 * threadid_callback and falls back to the deprecated id_callback only when the
 * former is NULL, and CRYPTO_THREADID_set_callback() refuses (returns 0) if a
 * callback is already registered. Registering both is therefore harmless and
 * covers either dispatch path.
 *
 * The dynamic-lock (dynlock) trio is installed too: OpenSSL 1.0.2's
 * CRYPTO_get_new_dynlockid() path is used by the ex_data/engine code and, like
 * the static locks, silently does nothing without callbacks.
 *
 * Called exactly ONCE, from ra_net_ensure_started(), on the render thread,
 * BEFORE curl_global_init() and long before the net worker thread exists --
 * so the installation itself is never racing anything.
 */

/*
 * OpenSSL only ever handles a POINTER to this type; the definition belongs to
 * the application (that is the whole point of the opaque
 * `struct CRYPTO_dynlock_value` forward declaration in crypto.h).
 */
struct CRYPTO_dynlock_value {
	SceUID mutex;
};

static SceUID *ra_ssl_locks      = NULL;
static int     ra_ssl_lock_count = 0;
static int     ra_ssl_threads_ready = 0;

/*
 * RECURSIVE, not plain. OpenSSL's lock/unlock pairs are balanced, so a
 * recursive mutex is a strict superset of the canonical pthread example's
 * behaviour: identical for the cross-thread case, and it converts a
 * hypothetical same-thread re-lock (which would hard-deadlock the worker with
 * a plain mutex, presenting as a hung download rather than a crash) into a
 * counted re-entry. On a fix whose whole purpose is removing crash modes,
 * trading a possible deadlock for a counter is the right side to err on.
 */
#define RA_SSL_MUTEX_ATTR SCE_KERNEL_MUTEX_ATTR_RECURSIVE

static void ra_ssl_locking_cb(int mode, int type, const char *file, int line)
{
	(void)file;
	(void)line;

	/* Defensive: OpenSSL must never hand us a type outside [0, num_locks),
	 * but an out-of-range index here would be an OOB read on a function
	 * pointer path reached from inside the TLS stack -- exactly the class of
	 * fault this change exists to remove. Bounds-check anyway. */
	if (!ra_ssl_locks || type < 0 || type >= ra_ssl_lock_count)
		return;
	if (ra_ssl_locks[type] < 0)
		return;

	if (mode & CRYPTO_LOCK)
		sceKernelLockMutex(ra_ssl_locks[type], 1, NULL);
	else
		sceKernelUnlockMutex(ra_ssl_locks[type], 1);
}

static void ra_ssl_threadid_cb(CRYPTO_THREADID *id)
{
	CRYPTO_THREADID_set_numeric(id, (unsigned long)sceKernelGetThreadId());
}

/* Deprecated-API fallback path; see the dispatch note above. */
static unsigned long ra_ssl_id_cb(void)
{
	return (unsigned long)sceKernelGetThreadId();
}

static struct CRYPTO_dynlock_value *ra_ssl_dyn_create_cb(const char *file, int line)
{
	struct CRYPTO_dynlock_value *l;

	(void)file;
	(void)line;

	l = (struct CRYPTO_dynlock_value *)malloc(sizeof(*l));
	if (!l)
		return NULL;

	l->mutex = sceKernelCreateMutex("RA_SSLDynLock", RA_SSL_MUTEX_ATTR, 0, NULL);
	if (l->mutex < 0) {
		free(l);
		return NULL;
	}
	return l;
}

static void ra_ssl_dyn_lock_cb(int mode, struct CRYPTO_dynlock_value *l,
	const char *file, int line)
{
	(void)file;
	(void)line;

	if (!l || l->mutex < 0)
		return;

	if (mode & CRYPTO_LOCK)
		sceKernelLockMutex(l->mutex, 1, NULL);
	else
		sceKernelUnlockMutex(l->mutex, 1);
}

static void ra_ssl_dyn_destroy_cb(struct CRYPTO_dynlock_value *l,
	const char *file, int line)
{
	(void)file;
	(void)line;

	if (!l)
		return;
	if (l->mutex >= 0)
		sceKernelDeleteMutex(l->mutex);
	free(l);
}

/*
 * Install the callbacks. Returns 0 on success, negative on failure.
 *
 * Failure is NOT fatal to the caller: a network stack with un-locked OpenSSL is
 * exactly what v11 shipped, so falling back to that is a regression to the
 * previous behaviour rather than a new failure mode. It is logged loudly so the
 * condition is visible in ux0:data/adrenaline_user_log.txt instead of silent.
 *
 * Never torn down. Same reasoning as curl_global_cleanup() (see stage 6): this
 * .suprx dies by kernel module unload, and removing OpenSSL's locks while a
 * transfer is in flight would be strictly worse than leaking 41 mutex UIDs that
 * die with the process.
 */
static int ra_ssl_thread_setup(void)
{
	int n;
	int i;

	if (ra_ssl_threads_ready)
		return 0;

	n = CRYPTO_num_locks();
	if (n <= 0) {
		RA_LOG("[RA] stage5 CRYPTO_num_locks() = %d - no locks to install\n", n);
		return -1;
	}

	ra_ssl_locks = (SceUID *)malloc((size_t)n * sizeof(SceUID));
	if (!ra_ssl_locks) {
		RA_LOG("[RA] stage5 OpenSSL lock array alloc FAILED (%d locks)\n", n);
		return -2;
	}

	for (i = 0; i < n; i++)
		ra_ssl_locks[i] = -1;

	for (i = 0; i < n; i++) {
		ra_ssl_locks[i] = sceKernelCreateMutex("RA_SSLLock", RA_SSL_MUTEX_ATTR, 0, NULL);
		if (ra_ssl_locks[i] < 0) {
			int j;

			RA_LOG("[RA] stage5 sceKernelCreateMutex[%d] FAILED: 0x%08X - OpenSSL stays UNLOCKED\n",
				i, ra_ssl_locks[i]);
			/* Full unwind: a partially populated array must never be
			 * published, or the locking callback would silently skip
			 * the tail locks and give a false sense of safety. */
			for (j = 0; j < i; j++)
				sceKernelDeleteMutex(ra_ssl_locks[j]);
			free(ra_ssl_locks);
			ra_ssl_locks = NULL;
			return -3;
		}
	}

	/* Publish the array BEFORE the callback that reads it. */
	ra_ssl_lock_count = n;

	CRYPTO_set_locking_callback(ra_ssl_locking_cb);
	CRYPTO_set_id_callback(ra_ssl_id_cb);
	i = CRYPTO_THREADID_set_callback(ra_ssl_threadid_cb);

	CRYPTO_set_dynlock_create_callback(ra_ssl_dyn_create_cb);
	CRYPTO_set_dynlock_lock_callback(ra_ssl_dyn_lock_cb);
	CRYPTO_set_dynlock_destroy_callback(ra_ssl_dyn_destroy_cb);

	ra_ssl_threads_ready = 1;

	RA_LOG("[RA] stage5 OpenSSL locking callbacks installed (%d static locks, threadid=%d, dynlock on)\n",
		n, i);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Lock-free SPSC FIFO                                                        */
/* ------------------------------------------------------------------------- */

void ra_fifo_init(ra_fifo *fifo)
{
	memset(fifo, 0, sizeof(*fifo));
}

ra_fifo_entry *ra_fifo_produce_begin(ra_fifo *fifo)
{
	uint32_t head = fifo->head;
	uint32_t next = (head + 1) & (RA_FIFO_SIZE - 1);
	if (next == fifo->tail)
		return NULL; /* full */
	return &fifo->entries[head];
}

void ra_fifo_produce_commit(ra_fifo *fifo)
{
	/* Fix 3 (release): publish the payload BEFORE the index that exposes it.
	 * `head`/`tail` are volatile, which orders the compiler's accesses to
	 * them but says nothing about the non-volatile entry fields around them,
	 * and nothing at all about the Cortex-A9 store buffer. Without this the
	 * consumer can observe an incremented `head` while `entries[head]` is
	 * still half-written -- e.g. a `body` pointer that has not landed yet. */
	__sync_synchronize();
	fifo->head = (fifo->head + 1) & (RA_FIFO_SIZE - 1);
}

ra_fifo_entry *ra_fifo_consume_begin(ra_fifo *fifo)
{
	if (fifo->head == fifo->tail)
		return NULL; /* empty */
	/* Fix 3 (acquire): pairs with the release above -- the entry loads must
	 * not be hoisted above the `head` load that proved them valid. */
	__sync_synchronize();
	return &fifo->entries[fifo->tail];
}

void ra_fifo_consume_commit(ra_fifo *fifo)
{
	fifo->tail = (fifo->tail + 1) & (RA_FIFO_SIZE - 1);
}

/* ------------------------------------------------------------------------- */
/* Bootstrap                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Phase A of the two-phase bring-up: sysmodule LOAD ONLY, and in v9 that means
 * SCE_SYSMODULE_NET and nothing else.
 *
 * Called ONLY from the deferred path -- idempotently, from
 * ra_net_ensure_started() when the trophy menu first needs the network. It is
 * deliberately NOT called from module_start(): v4 did that and the console
 * black-screened after the Adrenaline logo (v4-black-screen-opus.md).
 *
 * v9 REMOVED, from what used to be a six-rung ladder here:
 *   - stage 0 ScePaf. It existed for one purpose: to make SCE_SYSMODULE_SSL
 *     load. v8 restored it on this deferred path as a controlled A/B test and
 *     SSL still failed with 0x8002D0F3, so the hypothesis is refuted AND the
 *     dependency is gone. Dropping it also removes a 64 MiB heap request from
 *     the trophy-menu path inside a process that is already carrying
 *     ScePspemu's 60 MiB PSP RAM + a 32 MiB savestate buffer + the 8 MiB
 *     AdrHeap.
 *   - stages 2a/2b/2c (SSL, HTTP, HTTPS) and the stage 2d retry pass. curl and
 *     OpenSSL are statically linked into this module; there is no Sony TLS or
 *     HTTP provider left to ask for.
 *
 * The NET load stays non-fatal and nothing is ever unloaded on failure: this
 * process may not own the module (ScePspemu brings the net stack up itself to
 * service PSP-game networking) and unloading somebody else's module leaves the
 * stack dead for every later attempt.
 *
 * Returns 0 once SceNet is resident, RA_NET_ERR_SYSMODULE_NET otherwise.
 * ra_net_modules_loaded is set ONLY on success so a later attempt re-tries.
 */
int ra_net_preload_modules(void)
{
	int r;

	if (ra_net_modules_loaded)
		return 0;

	r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (r < 0)
		RA_LOG("[RA] stage1 sysmodule NET: 0x%08X (non-fatal, continuing)\n", r);
	else
		RA_LOG("[RA] stage1 sysmodule NET ok\n");

	RA_LOG("[RA] stage1 resident: NET=0x%08X\n", sceSysmoduleIsLoaded(SCE_SYSMODULE_NET));

	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
		RA_LOG("[RA] stage1 SceNet not resident (will retry on next attempt)\n");
		return RA_NET_ERR_SYSMODULE_NET;
	}

	ra_net_modules_loaded = 1;
	RA_LOG("[RA] stage1 modules ready (NET only -- curl+OpenSSL supply TLS)\n");
	return 0;
}

/*
 * v24: the bootstrap body, now always entered holding ra_net_boot_mutex.
 * Everything below is unchanged from v23 except that the "deferred path is the
 * ONLY caller" comment is no longer true -- see ra_net_ensure_started() below
 * and the mutex's own comment near the top of this file.
 */
static int ra_net_ensure_started_locked(void)
{
	int r;

	/* Re-check under the lock: while this caller waited, another thread may
	 * have completed the whole bootstrap. Without this the second caller
	 * would run curl_global_init() a second time. */
	if (ra_net_started)
		return 0;

	/* Phase A, idempotent. */
	r = ra_net_preload_modules();
	if (r != 0) {
		/* Without SceNet there are no sockets, so curl has nothing to talk
		 * through. This is the one genuinely fatal bootstrap stage in v9. */
		RA_LOG("[RA] stage1 FATAL: no SceNet, cannot bring up curl\n");
		return RA_NET_ERR_SYSMODULE_NET;
	}

	SceNetInitParam param;
	param.memory = ra_net_mem;
	param.size = RA_NET_MEM_SIZE;
	param.flags = 0;

	/*
	 * Stages 3-4 are NON-FATAL by design (Fix 1d, retained from v6).
	 *
	 * Both can legitimately return a negative "already initialised" status when
	 * ScePspemu -- which shares this process -- brought the stack up first.
	 * Treating that as failure made login impossible on exactly the consoles
	 * where the network was already working. Nothing is torn down on failure
	 * either: sceNetTerm() here would destroy a stack this module does not own
	 * and turn one transient failure into a permanent one.
	 *
	 * This is not theory. In the SAME hardware session, from this same process
	 * (ux0:data/adrenaline_user_log.txt, 2026-09-02):
	 *
	 *     [RA]    stage3 sceNetInit:    0x80410110  (non-fatal, continuing)
	 *     [RA]    stage4 sceNetCtlInit: 0x80412102  (non-fatal, continuing)
	 *     [SMOKE] net2 sceNetInit  = 0x80410110
	 *     [SMOKE] net3 sceNetCtlInit = 0x80412102 (non-fatal)
	 *     [SMOKE] phase3 VERIFY PASS curl=0 http=422 tls=TLSv1.2
	 *
	 * BOTH calls failed and a fully certificate-verified TLS round trip
	 * completed anyway, because ScePspemu had already brought the stack up to
	 * service PSP-game networking. Treating either as fatal would break login on
	 * exactly the console where the network demonstrably works.
	 */
	r = sceNetInit(&param);
	if (r == (int)0x80410103 || r == (int)0x80410110)
		/* Already up, courtesy of ScePspemu's own KERMIT_MODE_WLAN handler.
		 * 0x80410110 is the value MEASURED on this console (EBUSY-class);
		 * 0x80410103 (psp2common/net.h:20, SCE_NET_ERROR_ESRCH) is the value
		 * seen on earlier runs. Both mean "someone got here first" and both are
		 * benign -- see the smoke-test evidence above. */
		RA_LOG("[RA] stage3 sceNetInit: 0x%08X already initialised by ScePspemu - ok\n", r);
	else if (r < 0)
		RA_LOG("[RA] stage3 sceNetInit: 0x%08X (non-fatal, continuing)\n", r);
	else
		RA_LOG("[RA] stage3 sceNetInit ok\n");

	r = sceNetCtlInit();
	if (r < 0)
		/* 0x80412102 measured on hardware, alongside a passing TLS transfer. */
		RA_LOG("[RA] stage4 sceNetCtlInit: 0x%08X (non-fatal, continuing)\n", r);
	else
		RA_LOG("[RA] stage4 sceNetCtlInit ok\n");

	/*
	 * Stage 5 (v12) -- OpenSSL 1.0.2i locking callbacks.
	 *
	 * Reuses the retired stage-5 slot (the old sceSslInit stage), so the
	 * stage numbering in bug reports stays monotonic with the log.
	 *
	 * Ordering is deliberate and load-bearing: BEFORE curl_global_init(), so
	 * that even OpenSSL's own initialisation runs with locks in place, and
	 * long before sceKernelCreateThread() below creates the only other thread
	 * that can reach the TLS stack. Non-fatal by design -- see the function.
	 */
	ra_ssl_thread_setup();

	/*
	 * Stage 6 -- curl global init. THE new gate, replacing sceHttpInit +
	 * sceSslInit + sceHttpCreateTemplate.
	 *
	 * Deliberately done HERE, on the render thread, before the worker exists:
	 * curl_global_init() is documented as not thread-safe, and this is the only
	 * call site in the module (ra_smoke.c, which had the only other one, is
	 * retired in v9 -- so there is no longer any window in which two threads
	 * could race into it).
	 *
	 * curl_global_cleanup() is never called, on purpose: this .suprx is torn
	 * down by the kernel unloading the module, not by an orderly shutdown, and
	 * tearing OpenSSL down while a worker request is in flight would be strictly
	 * worse than leaking a few KiB that die with the process anyway.
	 */
	r = (int)curl_global_init(CURL_GLOBAL_DEFAULT);
	if (r != 0) {
		RA_LOG("[RA] stage6 curl_global_init FAILED: %d\n", r);
		ra_curl_ready = 0;
		return RA_NET_ERR_CURLINIT;
	}
	ra_curl_ready = 1;
	RA_LOG("[RA] stage6 curl_global_init ok -- TLS ready (%s)\n", curl_version());

	ra_fifo_init(&ra_completion_fifo);
	ra_fifo_init(&ra_work_fifo);

	ra_net_started = 1;

	ra_worker_stop = 0;
	ra_worker_thread = sceKernelCreateThread("RA_NetWorker", ra_net_worker_main, 0x80,
		RA_WORKER_STACK_SIZE, 0, 0, NULL);
	if (ra_worker_thread < 0) {
		RA_LOG("[RA] stage8 sceKernelCreateThread FAILED: 0x%08X\n", ra_worker_thread);
		/* Non-sticky unwind (Fix 1d): drop only what this module owns, so the
		 * next attempt starts from a clean retryable state. The network stack
		 * and curl's global state stay up -- curl_global_init is refcounted and
		 * re-calling it on the retry is harmless. */
		ra_worker_thread = -1;
		ra_net_started = 0;
		return RA_NET_ERR_THREAD;
	}
	sceKernelStartThread(ra_worker_thread, 0, NULL);
	RA_LOG("[RA] stage8 worker thread started (%u KiB stack) -- net layer UP\n",
		(unsigned)(RA_WORKER_STACK_SIZE / 1024));

	return 0;
}

/*
 * v24 wrapper. Two things changed relative to v23:
 *
 *   1. It is now legal to call this from the RA_BootNet worker (ra_client.c)
 *      as well as from the render thread. The mutex below is what makes that
 *      safe -- specifically it protects curl_global_init(), which curl
 *      documents as not thread-safe and which stage 6 calls exactly once.
 *   2. The fast path (already started) still costs a single volatile load and
 *      takes no lock at all, because ra_tick() polls ra_net_is_started() every
 *      frame and must not serialize the draw loop against a worker.
 */
int ra_net_ensure_started(void)
{
	int r;

	if (ra_net_started)
		return 0;

	if (ra_net_boot_mutex < 0)
		return ra_net_ensure_started_locked(); /* unserialized fallback */

	sceKernelLockMutex(ra_net_boot_mutex, 1, NULL);
	r = ra_net_ensure_started_locked();
	sceKernelUnlockMutex(ra_net_boot_mutex, 1);

	return r;
}

/*
 * Connectivity probe.
 *
 * v9 CHANGE: a FAILED query is no longer read as "offline".
 *
 * This is the single highest-value line in the v9 diff that is NOT about curl,
 * and it is driven by a measurement, not a preference. On this console:
 *
 *     [RA] stage4 sceNetCtlInit: 0x80412102 (non-fatal, continuing)
 *
 * sceNetCtlInit() FAILED -- and in the same session, on the same boot, the
 * smoke test completed a certificate-verified TLSv1.2 round trip to
 * retroachievements.org. A NetCtl subsystem that would not initialise is in no
 * position to answer sceNetCtlInetGetState() authoritatively, yet v8 let that
 * unanswerable query veto every request BEFORE curl ever ran, silently, with no
 * log line at all (ra_server_call's `|| !ra_net_is_online()` guard).
 *
 * SceNetCtl is additionally a weak import (user/CMakeLists.txt), so with no
 * provider bound the call returns a bare -1 from an unbound stub -- another way
 * to get a negative status that says nothing about the link.
 *
 * So: an unanswerable query now means "assume reachable and let curl be the
 * authority". A genuinely dead link then costs one logged curl error
 * (6 = DNS, 7 = connect, 28 = timeout) and a retryable status rc_client already
 * knows how to schedule -- diagnosable, instead of a silent block.
 */
int ra_net_is_online(void)
{
	int state = 0;
	int r = sceNetCtlInetGetState(&state);

	if (r < 0) {
		RA_LOG("[RA] netctl state query failed: 0x%08X - assuming reachable, curl will decide\n", r);
		return 1;
	}
	return state == SCE_NETCTL_STATE_CONNECTED;
}

/* ------------------------------------------------------------------------- */
/* curl transport (worker thread context only)                                */
/* ------------------------------------------------------------------------- */

typedef struct ra_http_buf {
	char  *data;
	size_t len;
	size_t cap;
} ra_http_buf;

/*
 * Body sink.
 *
 * Returning fewer bytes than curl handed us aborts the transfer with
 * CURLE_WRITE_ERROR, which is exactly the behaviour we want for both the
 * RA_HTTP_MAX_BODY cap and an allocation failure: the request ends as a clean
 * curl error rather than as a truncated body that a caller might parse.
 *
 * One byte of slack is always allocated beyond `cap` and a NUL kept at
 * data[len]. rc_api_server_response_t carries an explicit body_length and
 * rcheevos honours it, but a NUL-terminated buffer costs one byte and removes
 * a whole class of "some helper called strlen on it" hazard.
 */
static size_t ra_curl_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	ra_http_buf *buf = (ra_http_buf *)userdata;
	size_t n = size * nmemb;

	if (!buf)
		return 0; /* abort: no sink */

	if (n == 0)
		return 0;

	if (buf->len + n > RA_HTTP_MAX_BODY) {
		RA_LOG("[RA] body exceeds %u bytes, aborting transfer\n", (unsigned)RA_HTTP_MAX_BODY);
		return 0; /* abort -> CURLE_WRITE_ERROR */
	}

	if (buf->len + n > buf->cap) {
		size_t new_cap = buf->cap ? buf->cap : 16384;
		char *new_data;

		while (new_cap < buf->len + n)
			new_cap *= 2;

		new_data = realloc(buf->data, new_cap + 1);
		if (!new_data) {
			RA_LOG("[RA] body realloc(%u) failed, aborting transfer\n", (unsigned)(new_cap + 1));
			return 0; /* abort -> CURLE_WRITE_ERROR */
		}
		buf->data = new_data;
		buf->cap = new_cap;
	}

	memcpy(buf->data + buf->len, ptr, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
	return n;
}

/*
 * ONE request, synchronously, on the worker thread. The single transport
 * primitive behind all three network entry points.
 *
 *   post_data == NULL  -> GET  (v12: redirects NOT followed either; see Fix 1)
 *   post_data != NULL  -> POST (redirects NOT followed; see below)
 *
 * Returns a malloc'd, NUL-terminated body on transport success (never NULL on
 * success -- a zero-length 200 yields a 1-byte "" so callers can distinguish
 * "empty body" from "request failed"), and NULL on transport failure.
 * *out_status carries the HTTP status on success, or
 * RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR on failure -- the same status
 * ra_server_call's not-started/offline paths already return, which rc_client
 * knows how to schedule a retry for.
 *
 * The HTTP status is reported but NOT used to decide success: a 4xx from
 * dorequest.php is a perfectly good response that rcheevos must be allowed to
 * parse, and the badge path does its own == 200 check.
 */
static char *ra_curl_request(const char *url, const char *post_data,
	const char *content_type, size_t *out_len, int *out_status)
{
	ra_http_buf buf;
	struct curl_blob ca;
	struct curl_slist *headers = NULL;
	CURL *curl;
	CURLcode rc;
	long status = 0;
	double appconnect = 0.0;

	if (out_len)
		*out_len = 0;
	if (out_status)
		*out_status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;

	if (!url)
		return NULL;

	/* Should be impossible -- the worker only exists after stage 6 set this --
	 * but an unchecked curl_easy_init() against an uninitialised OpenSSL is not
	 * a failure mode worth discovering on hardware. */
	if (!ra_curl_ready) {
		RA_LOG("[RA] curl not initialised, refusing request\n");
		return NULL;
	}

	memset(&buf, 0, sizeof(buf));

	curl = curl_easy_init();
	if (!curl) {
		RA_LOG("[RA] curl_easy_init returned NULL\n");
		return NULL;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ra_curl_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

	/* Bounded. A wedged handshake must end in a logged timeout, not a worker
	 * thread that never returns and a user who reports "nothing happened". */
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, RA_HTTP_CONNECT_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, RA_HTTP_TOTAL_TIMEOUT_SEC);

	/* The vitasdk curl package is built -DENABLE_THREADED_RESOLVER=OFF, so curl
	 * would otherwise use alarm()/SIGALRM for DNS timeouts. There is no signal
	 * handling on this platform; leaving NOSIGNAL unset in that configuration is
	 * the documented way to get undefined behaviour. */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	curl_easy_setopt(curl, CURLOPT_USERAGENT, RA_HTTP_USER_AGENT);

	/*
	 * TLS verification is ON, in full, and is not configurable.
	 *
	 * The login POST carries the user's RetroAchievements *password*
	 * (rc_client_begin_login_with_password in ra_client.c), so disabling peer
	 * or host verification here would hand that password to any MITM on the
	 * user's network -- CWE-295. VERIFYHOST=2 is the only meaningful value;
	 * curl treats 1 as 2 and warns.
	 */
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	/*
	 * Trust anchors from MEMORY. See the CA rule in this file's header comment:
	 * CURLOPT_CAINFO (a path) is never set, and a rejected blob must ABORT the
	 * request rather than let curl fall back to its compiled-in vs0: CA file --
	 * that fallback is the stdio-BIO crash the smoke test died on.
	 *
	 * CURL_BLOB_COPY: curl duplicates the bytes into the handle, so this stack
	 * frame does not have to outlive the call. Costs one ~4.6 KB malloc per
	 * request, which is noise next to a TLS handshake.
	 */
	ca.data  = (void *)ra_ca_pem;
	ca.len   = sizeof(ra_ca_pem) - 1; /* exclude the C terminator: OpenSSL's PEM
	                                   * scanner must not see the NUL as data */
	ca.flags = CURL_BLOB_COPY;

	rc = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
	if (rc != CURLE_OK) {
		RA_LOG("[RA] CURLOPT_CAINFO_BLOB rejected: curl=%d (%s) - refusing to fall back to a file CA path\n",
			(int)rc, curl_easy_strerror(rc));
		curl_easy_cleanup(curl);
		return NULL;
	}

	if (post_data) {
		/*
		 * POST. Redirects are NOT followed: RetroAchievements' dorequest.php
		 * does not redirect, and curl's default CURLOPT_POSTREDIR would convert
		 * a followed 301 into a GET, silently dropping the request body (and
		 * with it the credentials) instead of failing loudly.
		 *
		 * COPYPOSTFIELDS, not POSTFIELDS: POSTFIELDS keeps a borrowed pointer
		 * that must outlive the perform. The worker's heap copy does outlive it
		 * today, but that is a lifetime invariant spread across two functions,
		 * and one 300-byte copy is cheaper than the bug.
		 */
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_data));
		curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_data);

		if (content_type) {
			char hdr[160];

			snprintf(hdr, sizeof(hdr), "Content-Type: %s", content_type);
			headers = curl_slist_append(NULL, hdr);
			if (headers)
				curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		}
	} else {
		/*
		 * v12 FIX 1 -- GET no longer follows redirects.
		 *
		 * This branch used to read:
		 *     curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		 *     curl_easy_setopt(curl, CURLOPT_MAXREDIRS, RA_HTTP_MAX_REDIRECTS);
		 * under a comment asserting "Badge media URLs
		 * (media.retroachievements.org) DO redirect".
		 *
		 * That assertion was live-tested and is FALSE as of 2026-09-02
		 * (investigation-result/badge-download-crash-research.md §2):
		 *
		 *     $ curl -sI -L https://media.retroachievements.org/Badge/109943.png
		 *     HTTP/2 200
		 *     server: cloudflare
		 *     content-length: 8642
		 *     content-type: image/png
		 *
		 * No Location:, no 3xx -- with AND without -L, and the same for the
		 * legacy retroachievements.org/Badge/ path. So FOLLOWLOCATION=1 bought
		 * nothing, while making the badge GET the ONE request in this module
		 * that could re-enter curl's URL re-parse / connection-cache / new-host
		 * connect path from inside a single curl_easy_perform() on the worker
		 * thread. v11-persistent-crash-debug.md flagged that asymmetry as
		 * CRITICAL-1: it is the only FOLLOWLOCATION=1 path and the only path
		 * that talks to a second host.
		 *
		 * Setting it to 0 explicitly (rather than relying on the default) makes
		 * the GET and POST paths symmetric and greppable.
		 *
		 * If RA ever reintroduces a redirect, this does NOT silently break --
		 * the 3xx is logged below with its Location target, and the correct
		 * remedy is an explicit second request driven from this function, not
		 * re-enabling auto-follow inside the perform.
		 */
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	}

	rc = curl_easy_perform(curl);

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect);

	if (headers)
		curl_slist_free_all(headers);

	/*
	 * v12 FIX 1 companion: with auto-follow off, a 3xx now arrives here as a
	 * normal completion with a short/empty body. That must be VISIBLE, not
	 * silently mistaken for a corrupt PNG. If this line ever appears, the
	 * live-tested premise behind Fix 1 has changed and the remedy is an
	 * explicit second request from this function -- not FOLLOWLOCATION=1.
	 */
	if (rc == CURLE_OK && status >= 300 && status < 400) {
		char *redir = NULL;

		curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redir);
		RA_LOG("[RA] curl %ld REDIRECT not followed (v12 Fix 1) -> %s\n",
			status, redir ? redir : "(no Location)");
	}

	if (rc != CURLE_OK) {
		ra_note_curl_error(rc);
		RA_LOG("[RA] curl FAIL %d (%s) http=%ld bytes=%u\n",
			(int)rc, curl_easy_strerror(rc), status, (unsigned)buf.len);
		free(buf.data);
		curl_easy_cleanup(curl);
		if (out_status)
			*out_status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		return NULL;
	}

	/* Non-NULL on success even for a zero-length body, so callers never have to
	 * disambiguate "empty 200" from "transport failed" by inspecting status. */
	if (!buf.data) {
		buf.data = (char *)malloc(1);
		if (!buf.data) {
			curl_easy_cleanup(curl);
			return NULL;
		}
		buf.data[0] = '\0';
		buf.len = 0;
	}

	/* tls=NNNms is the TLS handshake completion time: non-zero is independent
	 * corroboration that the encrypted round trip really happened, and it is
	 * the field that tells a plaintext regression apart from a working POST. */
	RA_LOG("[RA] curl OK http=%ld bytes=%u tls=%dms\n",
		status, (unsigned)buf.len, (int)(appconnect * 1000.0));

	curl_easy_cleanup(curl);

	if (out_len)
		*out_len = buf.len;
	if (out_status)
		*out_status = (int)status;
	return buf.data;
}

/* ------------------------------------------------------------------------- */
/* rc_client server call                                                      */
/* ------------------------------------------------------------------------- */

/*
 * ENTRY POINT 1 of 3. Render thread.
 *
 * Unchanged in v9 apart from the bootstrap it waits on: it still only deep-
 * copies the request and queues it. The actual transport (now curl) happens in
 * ra_net_worker_main below, and the callback is still marshalled back to the
 * render thread by ra_net_tick(). rc_client is never touched off the render
 * thread.
 *
 * v9 dropped the ra_net_ssl_ready pre-check that used to live in the worker:
 * with curl there is no state in which an https:// URL is unsafe to hand to the
 * transport.
 */
void ra_server_call(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callback_data, rc_client_t *client)
{
	(void)client;
	ra_fifo_entry *entry;

	if (!ra_net_started || ra_net_ensure_started() != 0 || !ra_net_is_online()) {
		/* Deliver a retryable client error so rc_client schedules a retry. */
		if (callback) {
			rc_api_server_response_t response;
			memset(&response, 0, sizeof(response));
			response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
			callback(&response, callback_data);
		}
		return;
	}

	entry = ra_fifo_produce_begin(&ra_work_fifo);
	if (!entry) {
		if (callback) {
			rc_api_server_response_t response;
			memset(&response, 0, sizeof(response));
			response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
			callback(&response, callback_data);
		}
		return;
	}

	memset(entry, 0, sizeof(*entry));
	entry->kind = RA_FIFO_SERVER_CALL;
	entry->server_callback = callback;
	entry->server_callback_data = callback_data;
	entry->url = request->url ? strdup(request->url) : NULL;
	entry->post_data = request->post_data ? strdup(request->post_data) : NULL;
	entry->content_type = request->content_type ? strdup(request->content_type) : NULL;

	/* NOTE: rc_client destroys `request` immediately after we return; the
	 * heap copies above are the only thing that may be touched later. */
	ra_fifo_produce_commit(&ra_work_fifo);
}

/*
 * ENTRY POINT 2 of 3. Render thread producer; the GET itself runs on the worker
 * via ra_http_get -> ra_curl_request.
 */
int ra_net_fetch_badge(const char *url, const char *badge_name, int locked)
{
	ra_fifo_entry *entry;

	if (!ra_net_started || ra_net_ensure_started() != 0 || !ra_net_is_online())
		return 0;

	entry = ra_fifo_produce_begin(&ra_work_fifo);
	if (!entry)
		return 0;

	memset(entry, 0, sizeof(*entry));
	entry->kind = RA_FIFO_BADGE_DONE;
	entry->url = strdup(url);
	strncpy(entry->badge_name, badge_name, sizeof(entry->badge_name) - 1);
	entry->locked = locked;

	ra_fifo_produce_commit(&ra_work_fifo);
	return 1;
}

/* v6: queue the game-identification job for the net worker. Returns non-zero
 * on success. Render-side producer of RA_FIFO_HASH_JOB; the worker runs the
 * heavy chain (ra_hash_worker_job in ra_client.c) and the completion is
 * consumed by ra_client_hash_done() in the ra_net_tick() drain below. */
int ra_net_queue_hash_job(void)
{
	ra_fifo_entry *entry;

	if (!ra_net_started || ra_net_ensure_started() != 0)
		return 0;

	entry = ra_fifo_produce_begin(&ra_work_fifo);
	if (!entry)
		return 0;

	memset(entry, 0, sizeof(*entry));
	entry->kind = RA_FIFO_HASH_JOB;
	/* no payload: the worker reads the Kermit block + game file itself */

	ra_fifo_produce_commit(&ra_work_fifo);
	return 1;
}

/* ------------------------------------------------------------------------- */
/* HTTP GET                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * ENTRY POINT 3 of 3. Worker thread context.
 *
 * v9: a thin wrapper over ra_curl_request. The 90 lines of sceHttp connection /
 * request / chunked-read / teardown bookkeeping it used to contain now live
 * once, in ra_curl_request's write callback, shared with the POST path.
 */
char *ra_http_get(const char *url, size_t *out_len, int *out_status)
{
	return ra_curl_request(url, NULL, NULL, out_len, out_status);
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

int ra_net_worker_main(unsigned int args, void *argp)
{
	(void)args;
	(void)argp;

	while (!ra_worker_stop) {
		ra_fifo_entry *work = ra_fifo_consume_begin(&ra_work_fifo);
		if (!work) {
			sceKernelDelayThread(10 * 1000);
			continue;
		}

		if (work->kind == RA_FIFO_SERVER_CALL) {
			char *body = NULL;
			size_t body_len = 0;
			int status = 0;

			/* v6 instrumentation, kept: bracket the worker-side HTTP execution.
			 * A log that ends with "req ->" and never reaches "req <-" pins a
			 * crash inside the worker's transport rather than on the render
			 * thread. In v9 that transport is curl+OpenSSL, and ra_curl_request
			 * logs its own "curl OK"/"curl FAIL" line in between. */
			RA_LOG("[RA] req -> %s (post=%u)\n", work->url ? work->url : "(null)",
				work->post_data ? (unsigned)strlen(work->post_data) : 0u);

			body = ra_curl_request(work->url, work->post_data, work->content_type,
				&body_len, &status);

			RA_LOG("[RA] req <- status=%d body=%u\n", status, (unsigned)body_len);

			/* marshal back to the render thread; spin if the FIFO is full.
			 * NEVER call the rc_client callback from the worker thread: rc_client
			 * is only touched by the render thread. ra_tick() drains every frame. */
			for (;;) {
				ra_fifo_entry *done = ra_fifo_produce_begin(&ra_completion_fifo);
				if (done) {
					memset(done, 0, sizeof(*done));
					done->kind = RA_FIFO_SERVER_CALL;
					done->http_status = status;
					done->body = body; /* ownership transfers to consumer */
					done->body_length = body_len;
					done->server_callback = work->server_callback;
					done->server_callback_data = work->server_callback_data;
					ra_fifo_produce_commit(&ra_completion_fifo);
					body = NULL; /* consumed */
					break;
				}
				sceKernelDelayThread(1000);
			}

			free(body);

		} else if (work->kind == RA_FIFO_BADGE_DONE) {
			size_t body_len = 0;
			int status = 0;
			/*
			 * v13 FIX 2 -- the PRE-transfer log, asked for by v11 and v12 and
			 * missing for three releases (v12-draw-crash-debug.md CRITICAL-1).
			 *
			 * Without it, the badge GET logs only AFTER ra_http_get() returns,
			 * so a fault anywhere inside curl_easy_perform() on this worker
			 * thread produces a log that ends at the render thread's last line
			 * ("badge tex=...") -- indistinguishable from a render-thread crash.
			 * With this line, a log ending in "badge GET ->" and never reaching
			 * "badge GET <-" pins the fault inside the worker's transport.
			 *
			 * The URL is logged in full so the host actually used (apex vs media,
			 * see ra_badges.c v13 FIX 1) is visible on the hardware run.
			 */
			RA_LOG("[RA] badge GET -> %s\n", work->url ? work->url : "(null)");

			char *body = ra_http_get(work->url, &body_len, &status);

			RA_LOG("[RA] badge GET <- %s locked=%d status=%d len=%u\n",
				work->badge_name, work->locked, status, (unsigned)body_len);

			for (;;) {
				ra_fifo_entry *done = ra_fifo_produce_begin(&ra_completion_fifo);
				if (done) {
					memset(done, 0, sizeof(*done));
					done->kind = RA_FIFO_BADGE_DONE;
					done->http_status = status;
					done->body = body; /* ownership transfers to consumer */
					done->body_length = body_len;
					strncpy(done->badge_name, work->badge_name, sizeof(done->badge_name) - 1);
					done->locked = work->locked;
					ra_fifo_produce_commit(&ra_completion_fifo);
					body = NULL;
					break;
				}
				sceKernelDelayThread(1000);
			}

			free(body);

		} else if (work->kind == RA_FIFO_HASH_JOB) {
			/* v6: game identification OFF the render thread. The heavy chain
			 * (ScePspemuConvertAddress + path translation + rc_hash file I/O)
			 * runs here, mirroring the way AdrenalineCompat runs SaveState/
			 * LoadState. The worker NEVER touches rc_client or the render
			 * thread's status line: it only computes the hash and posts the
			 * completion; the render thread spends it in ra_client_hash_done()
			 * (ra_net_tick drain). */
			char hash_result[40];
			char psp_path[96];
			int hash_ok = 0;
			int hash_reason = RA_HASH_REASON_NONE;

			ra_hash_worker_job(hash_result, &hash_ok, &hash_reason,
				psp_path, (int)sizeof(psp_path));

			for (;;) {
				ra_fifo_entry *done = ra_fifo_produce_begin(&ra_completion_fifo);
				if (done) {
					memset(done, 0, sizeof(*done));
					done->kind = RA_FIFO_HASH_JOB;
					done->hash_ok = hash_ok;
					done->hash_reason = hash_reason;
					strncpy(done->hash_result, hash_result, sizeof(done->hash_result) - 1);
					strncpy(done->hash_psp_path, psp_path, sizeof(done->hash_psp_path) - 1);
					ra_fifo_produce_commit(&ra_completion_fifo);
					break;
				}
				sceKernelDelayThread(1000);
			}
		}

		free(work->url);
		free(work->post_data);
		free(work->content_type);
		work->url = work->post_data = work->content_type = NULL;
		ra_fifo_consume_commit(&ra_work_fifo);
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* Completion drain (render thread)                                           */
/* ------------------------------------------------------------------------- */

void ra_net_tick(void)
{
	for (;;) {
		ra_fifo_entry *done = ra_fifo_consume_begin(&ra_completion_fifo);
		if (!done)
			break;

		if (done->kind == RA_FIFO_SERVER_CALL && done->server_callback) {
			rc_api_server_response_t response;
			memset(&response, 0, sizeof(response));
			response.body = done->body;
			response.body_length = done->body_length;
			response.http_status_code = done->http_status;
			/* v6 instrumentation: last render-thread line before rcheevos parses
			 * the response and fires the rc_client callback (login result). */
			RA_LOG("[RA] cb dispatch status=%d len=%u\n", done->http_status,
				(unsigned)done->body_length);
			done->server_callback(&response, done->server_callback_data);
		} else if (done->kind == RA_FIFO_BADGE_DONE) {
			/* badge body delivered: decode + cache is handled by ra_badges.c */
			extern void ra_badge_deliver(const char *badge_name, int locked, const char *data, size_t len, int ok);
			ra_badge_deliver(done->badge_name, done->locked, done->body, done->body_length,
				done->http_status == 200 ? 1 : 0);
		} else if (done->kind == RA_FIFO_HASH_JOB) {
			/* v6: hash result back on the render thread; ra_client_hash_done
			 * spends it (and issues rc_client_begin_load_game, keeping the
			 * rc_client single-writer rule). */
			ra_client_hash_done(done);
		}

		free(done->body);
		done->body = NULL;
		done->body_length = 0;
		ra_fifo_consume_commit(&ra_completion_fifo);
	}
}
