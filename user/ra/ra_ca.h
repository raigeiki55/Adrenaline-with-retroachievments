/*
 * ra_ca.h — embedded CA trust anchors for the RetroAchievements HTTPS client.
 *
 * v9: renamed from ra_smoke_ca.h (array ra_smoke_ca_pem -> ra_ca_pem). The PEM
 * payload is BYTE-IDENTICAL to the bundle the curl+OpenSSL smoke test shipped
 * and validated; only the include guard and the array name changed. It is now
 * consumed by ra_net.c (ra_curl_request -> CURLOPT_CAINFO_BLOB) on EVERY RA
 * request: the login POST, every rc_client server call, and every badge GET.
 *
 * GENERATED FILE. Do not hand-edit the base64 below; edit the recipe at the
 * bottom of this comment and regenerate in the build pod.
 *
 *
 * WHY THIS FILE EXISTS — two independent reasons, both load-bearing
 * ----------------------------------------------------------------
 * 1. CRASH AVOIDANCE (this is the one that unblocks phase3).
 *
 *    Pointing CURLOPT_CAINFO at a *file path* makes OpenSSL open that file
 *    with a stdio FILE BIO:
 *
 *        Curl_ssl_setup_x509_store -> X509_STORE_load_locations
 *          -> by_file_ctrl -> X509_load_cert_file -> BIO_s_file
 *          -> file_fopen (Sony fopen)  ... then PEM_read_bio -> BIO_gets
 *          -> file_gets (newlib fgets)
 *
 *    In this module fopen() binds to Sony's SceLibc stub while fgets() binds
 *    to newlib's libc.a. file_gets() therefore hands a *Sony* FILE* to
 *    *newlib's* fgets(), which reads AND WRITES through it using newlib's
 *    struct __sFILE layout. That is not a graceful failure: it is a wild
 *    dereference plus memory corruption, and it is exactly what killed smoke
 *    test #2 at the line "phase3 VERIFY begin".
 *
 *    Root-caused to instruction level in
 *    investigation-result/smoke2-success-analysis.md (file_gets @0x81128c2e
 *    -> bl 0x811fec5c <fgets>; BIO_s_file vtable @0x812b9634 word[5]).
 *
 *    CURLOPT_CAINFO_BLOB routes through BIO_new_mem_buf() instead. BIO_s_mem's
 *    mem_gets() calls only BIO_clear_flags() and mem_read() — ZERO stdio calls
 *    — so the fopen/fgets mismatch is structurally UNREACHABLE, not merely
 *    avoided. Verified in the shipped smoke2 binary: Curl_ssl_setup_x509_store
 *    contains both branches, 0x810b263e "bl BIO_new_mem_buf" (blob) and
 *    0x810b2806 "bl X509_STORE_load_locations" (file).
 *
 *    ==> Never repoint CURLOPT_CAINFO at "a different file". ANY path
 *        re-enters the broken file BIO. The fix is the blob, not the path.
 *
 * 2. RIGHT TRUST ANCHORS. Sony's stock store — vs0:data/external/cert/
 *    CA_LIST.cer, which is also the vitasdk curl package's compiled-in
 *    -DCURL_CA_BUNDLE default — is community-documented as no longer updated
 *    by SCE, and its contents are not ours to reason about. Embedding the
 *    roots makes verification deterministic and survives a user installing or
 *    removing iTLS-Enso.
 *
 *
 * WHICH ROOTS, AND WHY THESE — measured, not guessed
 * --------------------------------------------------
 * Probed from the build pod on 2026-09-02:
 *
 *     openssl s_client -connect retroachievements.org:443 \
 *                      -servername retroachievements.org -showcerts
 *       0 s:CN = retroachievements.org
 *       1 s:C = US, O = Google Trust Services, CN = WE1
 *       2 s:C = US, O = Google Trust Services LLC, CN = GTS Root R4
 *
 * So the anchor actually required TODAY is GTS Root R4. Two more are bundled
 * as rotation insurance, because a PS Vita hardware test cycle is expensive
 * and an anchor rotation would otherwise silently cost one:
 *
 *   - GTS Root R1  — Google Trust Services' RSA root. WE1/WR1 intermediates
 *                    move between the R1 and R4 hierarchies without notice.
 *   - ISRG Root X1 — Let's Encrypt. RA has used LE historically, and the
 *                    Cloudflare/GTS fronting can reissue against it.
 *
 * Verified in the pod against THIS BUNDLE ALONE, system store excluded:
 *
 *     openssl s_client ... -CAfile <bundle> -no-CApath
 *       -> Verify return code: 0 (ok)
 *
 * The bytes below were copied verbatim from the build pod's Mozilla NSS root
 * store (the Debian/Ubuntu ca-certificates package, /etc/ssl/certs/*.pem).
 * They are public root certificates: they carry public keys only. Nothing
 * here is a credential, so there is nothing to leak by committing it, and
 * nothing to rotate.
 *
 * Cost: ~4.6 KB of .rodata, against the ~2.2 MB that curl+OpenSSL already
 * add. Negligible.
 *
 *
 * REGENERATION (in the build pod, kubectl -n vita-builds exec deploy/vita-build-server)
 * ------------------------------------------------------------------------------------
 *   1. Re-probe the live chain and note the root CN:
 *        openssl s_client -connect retroachievements.org:443 \
 *          -servername retroachievements.org -showcerts </dev/null 2>/dev/null \
 *          | grep " s:"
 *   2. Concatenate the wanted /etc/ssl/certs/<Root>.pem files, then re-verify
 *      with -CAfile <bundle> -no-CApath and confirm "Verify return code: 0".
 *   3. Emit the literal:  awk '{ printf "\t\"%s\\n\"\n", $0 }' <bundle>
 *   4. Round-trip check: strip the C quoting back out and diff against the
 *      source PEMs. They must be byte-identical.
 */
#ifndef RA_CA_H
#define RA_CA_H

/*
 * One NUL-terminated PEM blob, concatenated certificates. OpenSSL's
 * PEM_read_bio_X509_AUX loop over a memory BIO reads them all; sizeof()-1 is
 * passed as curl_blob.len so the terminator is excluded.
 */
static const char ra_ca_pem[] =
	/* GTS_Root_R4  --  REQUIRED TODAY - issuer chain of retroachievements.org (leaf <- WE1 <- R4)
	 * subject : C = US, O = Google Trust Services LLC, CN = GTS Root R4
	 * sha256  : 34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D
	 * expires : Jun 22 00:00:00 2036 GMT
	 * source  : pod /etc/ssl/certs/GTS_Root_R4.pem (Mozilla NSS / ca-certificates)
	 */
	"-----BEGIN CERTIFICATE-----\n"
	"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
	"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
	"A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
	"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
	"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
	"AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
	"QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
	"HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
	"BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
	"9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
	"p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
	"-----END CERTIFICATE-----\n"

	/* GTS_Root_R1  --  rotation insurance - GTS RSA hierarchy
	 * subject : C = US, O = Google Trust Services LLC, CN = GTS Root R1
	 * sha256  : D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
	 * expires : Jun 22 00:00:00 2036 GMT
	 * source  : pod /etc/ssl/certs/GTS_Root_R1.pem (Mozilla NSS / ca-certificates)
	 */
	"-----BEGIN CERTIFICATE-----\n"
	"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
	"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
	"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
	"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
	"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
	"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
	"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
	"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
	"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
	"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
	"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
	"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
	"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
	"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
	"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
	"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
	"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
	"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
	"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
	"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
	"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
	"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
	"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
	"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
	"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
	"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
	"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
	"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
	"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
	"-----END CERTIFICATE-----\n"

	/* ISRG_Root_X1  --  rotation insurance - Let's Encrypt
	 * subject : C = US, O = Internet Security Research Group, CN = ISRG Root X1
	 * sha256  : 96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6
	 * expires : Jun  4 11:04:38 2035 GMT
	 * source  : pod /etc/ssl/certs/ISRG_Root_X1.pem (Mozilla NSS / ca-certificates)
	 */
	"-----BEGIN CERTIFICATE-----\n"
	"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\n"
	"-----END CERTIFICATE-----\n"

	;

#endif /* RA_CA_H */
