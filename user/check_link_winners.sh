#!/usr/bin/env bash
#
# check_link_winners.sh — build-time guard for the four --allow-multiple-definition
# duplicates in adrenaline_user.
#
# WHY THIS EXISTS
# ---------------
# Linking libcurl/libssl/libcrypto forces newlib (libc.a) into a module that is
# otherwise built -nostdlib against Sony's SceLibc. Four symbols end up defined
# twice, because newlib packs each public function in the same object file as
# its reentrant twin and those _r variants are genuinely needed:
#
#     fclose   SceLibc_stub  vs  libc.a(lib_a-fclose.o)   [also defines _fclose_r]
#     strtol   SceLibc_stub  vs  libc.a(lib_a-strtol.o)   [also defines _strtol_r]
#     strtoul  SceLibc_stub  vs  libc.a(lib_a-strtoul.o)  [also defines _strtoul_r]
#     _ctype_  ra_client.c   vs  libc.a(lib_a-ctype_.o)   [also defines __ctype_ptr__]
#
# It ALSO guards a second, nastier failure mode that --allow-multiple-definition
# has nothing to do with: a direct .obj defining a symbol that an archive would
# otherwise provide, WITH THE WRONG TYPE.
#
#     __errno  main.c.obj `int __errno;`  vs  libc.a(lib_a-errno.o) `int *__errno(void)`
#
# ld never warns about this, because there is nothing to warn about at link
# level: main.c.obj is a direct object, so lib_a-errno.o is simply never
# extracted from the archive. There is exactly one definition. But every
# `errno` expression in newlib, curl and OpenSSL compiles to `bl __errno`, and
# those 198 branches then targeted a zeroed, non-executable .bss address --
# prefetch abort inside curl_global_init, thread killed silently (2026-09-02).
#
# The map check below asserts __errno's PROVIDER; the ELF check at the bottom
# asserts its TYPE is a text/function symbol, which is the invariant that
# actually matters.
#
# -Wl,--allow-multiple-definition makes ld take the FIRST definition and move
# on, SILENTLY. That is correct today only because of the library ordering in
# CMakeLists.txt. If someone later reorders that list, the build keeps
# succeeding but fclose() starts resolving to newlib while fopen() still
# resolves to Sony — a mismatched FILE* that corrupts memory at runtime, in
# lodepng and rcheevos/rhash, far from the edit that caused it.
#
# So: parse the linker map, assert who actually won, and fail the build if it
# is not who we intended.
set -u

MAPFILE="${MAPFILE:?MAPFILE must be set to the linker -Map output}"

if [ ! -r "$MAPFILE" ]; then
	echo "check_link_winners: FATAL: cannot read map file '$MAPFILE'" >&2
	exit 1
fi

# symbol -> substring that MUST appear in the winning provider's path
declare -A EXPECT=(
	[fclose]="libSceLibc_stub.a"
	[strtol]="libSceLibc_stub.a"
	[strtoul]="libSceLibc_stub.a"
	[_ctype_]="ra_client.c.obj"
	# __errno MUST come from newlib's archive member, never from a project
	# object. Matching the member name (not just "libc.a") keeps this
	# unambiguous regardless of how the map spells the archive path.
	[__errno]="lib_a-errno.o"
	# Sanity anchors: these are NOT duplicated, but if fopen/fread ever stop
	# coming from Sony's libc while fclose still does, the FILE* pairing is
	# broken just as badly. Assert the whole family together.
	[fopen]="libSceLibc_stub.a"
	[fread]="libSceLibc_stub.a"
)

# ---------------------------------------------------------------------------
# The rest of the stdio family (added 2026-09-02, after smoke #2 crashed).
#
# The EXPECT map above covered seven symbols and printed "all libc symbol
# winners as expected" while the module shipped a live FILE* type confusion:
# fopen() bound to Sony, fgets() bound to newlib, and OpenSSL's file BIO used
# BOTH on the same handle. newlib's fgets() wrote into a Sony FILE* using
# newlib's struct __sFILE layout. The comment at the top of this file described
# that exact scenario as a FUTURE hazard -- it had already happened, for seven
# symbols, unguarded.
#
# Measured in build/user/adrenaline_user.map (smoke #2):
#     fgets fwrite fputs fflush setvbuf fileno fprintf  -> libc.a  (MISMATCHED)
#     fopen fread fclose fseek ftell                    -> SceLibc (correct)
#
# WHY THIS TIER IS ADVISORY BY DEFAULT, AND WHY THAT IS THE HONEST CHOICE
# ----------------------------------------------------------------------
# Making these bind to Sony is a LINK change (-Wl,--undefined=fgets,... ahead of
# the group, so the symbols are undefined when SceLibc_stub is first scanned).
# That change adds ~7 new non-weak function imports from SceLibc. SceLibc is
# already imported and is proven resident in ScePspemu -- but "the module gains
# non-weak imports" is the documented black-screen failure mode for this project
# (see the SceHttp/SceSsl weak-stub block in CMakeLists.txt), and it cannot be
# de-risked without a hardware cycle.
#
# Meanwhile the ONLY reachable consumer of the mismatch was OpenSSL's file BIO
# on the CAINFO path, and smoke #3 removed that path entirely by switching to
# CURLOPT_CAINFO_BLOB (ra/ra_smoke_ca.h). Verified in the pod: no project .obj
# references any of these symbols; only libcurl/libcrypto do.
#
# So the current state is: known-mismatched, but unreachable. Failing the build
# on it would block a fix that is already correct, and silently ignoring it is
# what got us here. This tier therefore does the third thing:
#
#   * a family symbol that is mismatched AND on the accepted list -> loud WARN
#   * a family symbol that is mismatched and NOT on the list      -> FAIL
#     (i.e. the defect class can grow no further without breaking the build)
#   * STRICT_STDIO=1 in the environment                           -> all FAIL
#     Set this once the link-order fix lands; it should then pass clean.
# ---------------------------------------------------------------------------
STDIO_FAMILY=(fgets fwrite fputs fflush setvbuf fileno fprintf feof ferror ungetc)

# Known-mismatched as of smoke #2/#3. Anything outside this set is a REGRESSION.
#
# ungetc was NOT in the investigator's table; this guard found it on its first
# run (2026-09-02) — which is the point of widening the list. It is a bystander,
# not a consumer: nothing in the link references ungetc. libc.a(lib_a-ungetc.o)
# was extracted for __submore, which lib_a-svfiscanf.o (newlib's sscanf engine)
# needs, and that object happens to export ungetc as well. Map evidence:
#     libc.a(lib_a-ungetc.o)
#       libc.a(lib_a-svfiscanf.o) (__submore)
# Verified: zero project .obj and zero curl/OpenSSL objects hold "U ungetc".
declare -A STDIO_ACCEPTED_NEWLIB=(
	[fgets]=1 [fwrite]=1 [fputs]=1 [fflush]=1 [setvbuf]=1 [fileno]=1 [fprintf]=1
	[ungetc]=1
)

STRICT_STDIO="${STRICT_STDIO:-0}"

# Extract "symbol <TAB> winning provider" from the map.
#
# GNU ld map layout for a resolved global symbol is two lines:
#     <addr> <size> /path/to/lib.a(member.o)      <- provider (or a .obj path)
#     <addr>        symbolname                    <- the symbol that bound here
# Section lines (".text.foo <addr> <size> /path") are also provider lines; we
# take the last path-looking field on any line as the current provider.
WINNERS="$(awk '
	NF >= 3 && $NF ~ /(\.a\([^)]*\)|\.obj|\.o)$/ { provider = $NF }
	NF == 2 && $1 ~ /^0x[0-9a-fA-F]+$/ && $2 ~ /^[A-Za-z_][A-Za-z_0-9]*$/ {
		if (!($2 in seen)) { seen[$2] = 1; print $2 "\t" provider }
	}
' "$MAPFILE")"

rc=0
for sym in "${!EXPECT[@]}"; do
	want="${EXPECT[$sym]}"
	got="$(printf '%s\n' "$WINNERS" | awk -F'\t' -v s="$sym" '$1 == s { print $2; exit }')"

	if [ -z "$got" ]; then
		# Not linked in at all. Not an error: if nothing references fread, it is
		# simply absent, and absent cannot be mismatched.
		echo "check_link_winners: skip  $sym (not present in link)"
		continue
	fi

	case "$got" in
		*"$want"*)
			echo "check_link_winners: ok    $sym -> ${got##*/}"
			;;
		*)
			echo "check_link_winners: FAIL  $sym -> ${got##*/} (expected a provider matching '$want')" >&2
			rc=1
			;;
	esac
done

if [ "$rc" -ne 0 ]; then
	cat >&2 <<'MSG'

check_link_winners: LINK ORDER REGRESSION.

One or more libc symbols bound to the wrong provider. This almost always means
target_link_libraries() in user/CMakeLists.txt was reordered so that newlib
(-lc) is scanned before -lSceLibc_stub inside the --start-group.

Fix the ORDER; do not silence this check. Mixing Sony's fopen() with newlib's
fclose() passes a Sony FILE* to a newlib implementation that will dereference
it as a newlib struct _reent FILE — memory corruption at runtime, nowhere near
the edit that caused it.

See the "LINK ORDER IS LOAD-BEARING" comment block in user/CMakeLists.txt.
MSG
	exit 1
fi

# ---------------------------------------------------------------------------
# Tier 2: the rest of the stdio family. See the rationale block above.
# ---------------------------------------------------------------------------
srcv=0   # a NEW mismatch appeared -> always fatal
swarn=0  # a known-accepted mismatch is still present -> warn (fatal if STRICT)

for sym in "${STDIO_FAMILY[@]}"; do
	got="$(printf '%s\n' "$WINNERS" | awk -F'\t' -v s="$sym" '$1 == s { print $2; exit }')"

	if [ -z "$got" ]; then
		# Absent cannot be mismatched. feof/ferror/ungetc are normally absent.
		echo "check_link_winners: skip  $sym (not present in link)"
		continue
	fi

	case "$got" in
		*libSceLibc_stub.a*)
			echo "check_link_winners: ok    $sym -> ${got##*/} (Sony, pairs with fopen)"
			;;
		*)
			if [ "${STDIO_ACCEPTED_NEWLIB[$sym]:-0}" = "1" ] && [ "$STRICT_STDIO" != "1" ]; then
				echo "check_link_winners: WARN  $sym -> ${got##*/} (newlib; known + accepted," \
					"unreachable since phase3 uses CURLOPT_CAINFO_BLOB)" >&2
				swarn=1
			else
				echo "check_link_winners: FAIL  $sym -> ${got##*/} (newlib; expected libSceLibc_stub.a)" >&2
				srcv=1
			fi
			;;
	esac
done

if [ "$srcv" -ne 0 ]; then
	cat >&2 <<'MSG'

check_link_winners: STDIO FAMILY REGRESSION.

A stdio function bound to newlib that was NOT on the known-accepted list.

This is the fopen/fgets bug class. Sony's fopen() returns a Sony FILE*; newlib's
implementations dereference it as a newlib `struct __sFILE` and WRITE through
it. The result is memory corruption at runtime, far from the edit that caused
it -- it killed smoke test #2 inside curl_easy_perform() with no log line at
all (investigation-result/smoke2-success-analysis.md).

Either:
  a) remove whatever new code pulled this symbol in, or
  b) force it to Sony by adding it to the -Wl,--undefined=... list ahead of the
     --start-group in user/CMakeLists.txt, so SceLibc_stub defines it on the
     first scan -- and then re-verify the module's import list did not gain a
     new LIBRARY (new libraries are the black-screen failure mode; new functions
     from the already-imported SceLibc are not).

Do not add it to STDIO_ACCEPTED_NEWLIB without doing (b) or proving the symbol
is unreachable, and write down which, here.
MSG
	exit 1
fi

if [ "$swarn" -ne 0 ]; then
	cat >&2 <<'MSG'

check_link_winners: NOTE -- the stdio family above is still split between Sony
and newlib. It is accepted for now ONLY because the single reachable consumer
(OpenSSL's file BIO on the CURLOPT_CAINFO path) was removed in smoke #3 in
favour of CURLOPT_CAINFO_BLOB. If any code ever calls fopen()/fgets() on the
same FILE*, or sets CURLOPT_CAINFO / CURLOPT_CAPATH / a cookie jar / a netrc
file / a file:// URL, this becomes live again.

To fix properly and turn this into a hard check: add the --undefined= list to
user/CMakeLists.txt and build with STRICT_STDIO=1.
MSG
fi

# ---------------------------------------------------------------------------
# ELF symbol-TYPE assertions.
#
# The map tells us WHO defined a symbol. It does not tell us WHAT KIND of
# symbol it is. For __errno that distinction is the whole bug: `int __errno;`
# and `int *__errno(void)` are the same name to the linker, and the data one
# silently satisfies every `bl __errno`.
#
# nm type letters: T/t = text (function), W/w = weak function,
#                  B/b = bss, D/d = data, R/r = rodata, A = absolute.
# For __errno we require a text/function type. Anything in B/D/R means a
# project object re-introduced it as a variable.
# ---------------------------------------------------------------------------
ELFFILE="${ELFFILE:-}"
NM="${NM:-}"
[ -n "$NM" ] && [ -x "$(command -v "$NM" 2>/dev/null)" ] || NM="$(command -v arm-vita-eabi-nm || true)"

# symbol -> regex of ACCEPTABLE nm type letters
declare -A EXPECT_TYPE=(
	[__errno]="[TtWw]"
)

if [ -z "$ELFFILE" ] || [ ! -r "$ELFFILE" ]; then
	echo "check_link_winners: note  ELFFILE unset/unreadable — skipping symbol-type checks"
elif [ -z "$NM" ]; then
	echo "check_link_winners: note  no nm found — skipping symbol-type checks"
else
	trc=0
	for sym in "${!EXPECT_TYPE[@]}"; do
		want_t="${EXPECT_TYPE[$sym]}"
		got_t="$("$NM" "$ELFFILE" 2>/dev/null | awk -v s="$sym" '$3 == s { print $2; exit }')"

		if [ -z "$got_t" ]; then
			echo "check_link_winners: skip  $sym (not defined in $(basename "$ELFFILE"))"
			continue
		fi

		if printf '%s' "$got_t" | grep -Eq "^$want_t$"; then
			echo "check_link_winners: ok    $sym type '$got_t' (function)"
		else
			echo "check_link_winners: FAIL  $sym type '$got_t' (expected a function type matching '$want_t')" >&2
			trc=1
		fi
	done

	if [ "$trc" -ne 0 ]; then
		cat >&2 <<'MSG'

check_link_winners: SYMBOL TYPE REGRESSION.

__errno resolved to a DATA object instead of newlib's function.

This is almost certainly because a project source file re-declared it, e.g.

    int __errno;            /* <-- never do this */

That definition lives in .bss, which is zeroed and NOT executable. Every
`errno` expression in newlib, curl and OpenSSL compiles to `bl __errno`, so
the first errno evaluation at runtime branches into non-executable data and
the thread dies with a prefetch abort — silently, with no log line, far from
the edit that caused it.

Delete the variable. `__errno` must bind to libc.a(lib_a-errno.o).
See the comment block where it used to live in user/main.c.
MSG
		exit 1
	fi
fi

echo "check_link_winners: all libc symbol winners as expected"
exit 0
