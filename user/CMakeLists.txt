cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_user)
include("${VITASDK}/share/vita.cmake" REQUIRED)

include(ExternalProject)
find_package(Git REQUIRED)

if(DEFINED NIGHTLY AND NIGHTLY EQUAL 1)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin -DNIGHTLY=1")
else()
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

ExternalProject_Add(libvita2dfbo
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    GIT_REPOSITORY https://github.com/frangarcj/vita2dlib.git
    GIT_TAG        fbo
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
      ${GIT_EXECUTABLE} apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/vita2d-fbo-utf8.patch
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C libvita2d
    INSTALL_COMMAND ""
)

add_library(libvita2dfbo_libA STATIC IMPORTED GLOBAL)
set_property(TARGET libvita2dfbo_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/libvita2d.a
)

ExternalProject_Add(vitashadercollection
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    URL https://github.com/VitaArchive/vita-shader-collection/releases/download/master-0.2-v1767160600/vita-shader-collection.tar.gz
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
)

add_library(vitashadercollection_libA STATIC IMPORTED GLOBAL)
set_property(TARGET vitashadercollection_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/lib/libvitashaders.a
)


# Builds
FUNCTION(ADD_RESOURCES out_var)
  SET(result)
  FOREACH(in_f ${ARGN})
  SET(out_f "${CMAKE_CURRENT_BINARY_DIR}/${in_f}.o")
  GET_FILENAME_COMPONENT(out_dir ${out_f} DIRECTORY)
  ADD_CUSTOM_COMMAND(OUTPUT ${out_f}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${CMAKE_LINKER} -r -b binary -o ${out_f} ${in_f}
    DEPENDS ${in_f}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building resource ${out_f}"
    VERBATIM
    )
  LIST(APPEND result ${out_f})
  ENDFOREACH()
  SET(${out_var} "${result}" PARENT_SCOPE)
ENDFUNCTION()

file(GLOB res_files RELATIVE
  ${CMAKE_CURRENT_SOURCE_DIR}
  flash0/kd/*.prx
  flash0/kd/*.bin
  flash0/vsh/module/*.prx
  flash0/*.bin
)
add_resources(cef_res ${res_files})

# rcheevos 12.4.0 (vendored): core runtime + API + hash, no lua
file(GLOB RCHEEVOS_CORE_SOURCES  rcheevos/src/*.c)
file(GLOB RCHEEVOS_RAPI_SOURCES  rcheevos/src/rapi/*.c)
file(GLOB RCHEEVOS_RCORE_SOURCES rcheevos/src/rcheevos/*.c)
file(GLOB RCHEEVOS_HASH_SOURCES  rcheevos/src/rhash/*.c)

# ---------------------------------------------------------------------------
# BOOT-SAFETY: force the two NEW module imports introduced by curl/OpenSSL to
# be WEAK.
#
# Linking libcurl/libssl/libcrypto (via newlib) added exactly two imported
# libraries that the hardware-proven v8 module did not have — measured by
# diffing `vita-elf-create -v` output against the v8 baseline, not guessed:
#
#     SceLibRng                 <- sceKernelGetRandomNumber  (OpenSSL RAND seed)
#     SceThreadmgrCoredumpTime  <- sceKernelExitThread       (newlib lib_a-threading.o)
#
# Both arrive NON-WEAK, from libSceLibKernel_stub.a / libSceKernelThreadMgr_stub.a.
# A non-weak import on a library that is not resident makes the loader REFUSE
# the module, so ksceKernelLoadStartModuleForPid() (kernel/main.c) fails,
# adrenaline_user never starts, InitAdrenaline() never runs, and the console
# shows a permanent BLACK SCREEN — the exact failure mode already documented on
# SceHttp/SceSsl/SceNetCtl in the target_link_libraries comment below.
#
# Whether ScePspemu exports these two particular library NIDs is UNVERIFIED. We
# cannot test it without shipping to hardware, and shipping a black screen to
# find out is the most expensive possible way to learn it.
#
# Weak imports remove the question. If the library IS present, a weak import
# binds exactly like a non-weak one — zero behavioural difference. If it is
# NOT, the module still loads and the call returns -1 from an unbound stub:
#   - sceKernelExitThread  is only reachable from newlib's thread-exit helper,
#     which nothing in this module calls (ra_net.c uses
#     sceKernelExitDeleteThread directly). Dead either way.
#   - sceKernelGetRandomNumber failing makes OpenSSL's seeding fail, which
#     surfaces as a logged [RA] curl FAIL 35/60 TLS error — a diagnosable
#     result, not a silent hang.
#
# Surgical, not blanket: only these two stub objects are taken from the _weak
# archives. Every other SceLibKernel / SceKernelThreadMgr import stays non-weak
# so a genuinely missing core library still fails loudly.
#
# These are added as EXTERNAL_OBJECTs (not libraries) because object files are
# emitted before all archives on the link line, so they win resolution and the
# non-weak members are never pulled.
# ---------------------------------------------------------------------------
set(WEAK_STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/weak_stubs)
set(WEAK_RNG_OBJ  ${WEAK_STUB_DIR}/SceLibKernel_SceLibRng_sceKernelGetRandomNumber.wo)
set(WEAK_EXIT_OBJ ${WEAK_STUB_DIR}/SceKernelThreadMgr_SceThreadmgrCoredumpTime_sceKernelExitThread.wo)

# Created at CONFIGURE time, not as the custom command's first COMMAND:
# WORKING_DIRECTORY is applied to every command in the rule including the
# first, so `cmake -E make_directory` there fails with "cd: can't cd to ...".
file(MAKE_DIRECTORY ${WEAK_STUB_DIR})

add_custom_command(
  OUTPUT ${WEAK_RNG_OBJ} ${WEAK_EXIT_OBJ}
  COMMAND ${CMAKE_AR} x $ENV{VITASDK}/arm-vita-eabi/lib/libSceLibKernel_stub_weak.a
          SceLibKernel_SceLibRng_sceKernelGetRandomNumber.wo
  COMMAND ${CMAKE_AR} x $ENV{VITASDK}/arm-vita-eabi/lib/libSceKernelThreadMgr_stub_weak.a
          SceKernelThreadMgr_SceThreadmgrCoredumpTime_sceKernelExitThread.wo
  WORKING_DIRECTORY ${WEAK_STUB_DIR}
  COMMENT "Extracting weak stubs for SceLibRng / SceThreadmgrCoredumpTime (boot safety)"
  VERBATIM
)
set_source_files_properties(${WEAK_RNG_OBJ} ${WEAK_EXIT_OBJ}
  PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(adrenaline_user
  ${cef_res}
  ${WEAK_RNG_OBJ}
  ${WEAK_EXIT_OBJ}
  main.c
  pops.c
  titleinfo.c
  flashfs.c
  msfs.c
  menu.c
  states.c
  usb.c
  utils.c
  math_utils.c
  lz4/lz4.c
  lodepng/lodepng.c
  ra/ra_client.c
  ra/ra_net.c
  ra/ra_badges.c
  ra/ra_trophy_view.c
  ra/ra_gamelist.c   # v21: PS3-style trophy game-collection (list + remote detail)
  ra/ra_toast.c
  ra/ra_chime.c
  # v9: ra/ra_smoke.c is GONE. The curl+OpenSSL smoke test answered its question
  # on hardware and its transport is now the production one in ra/ra_net.c. Its
  # embedded CA bundle moved to ra/ra_ca.h (byte-identical PEM) and its
  # _free_vita_newlib() -nostdlib shim moved into ra/ra_net.c, which is now the
  # reason curl is on the link line at all.
  ${RCHEEVOS_CORE_SOURCES}
  ${RCHEEVOS_RAPI_SOURCES}
  ${RCHEEVOS_RCORE_SOURCES}
  ${RCHEEVOS_HASH_SOURCES}
)

add_dependencies(adrenaline_user libvita2dfbo)
add_dependencies(adrenaline_user vitashadercollection)

target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/include)
target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/includes)
target_include_directories(adrenaline_user PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/include
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src
)

# rcheevos feature switches:
# - RC_CLIENT_SUPPORTS_HASH: game identification via rc_hash (pbp/iso)
# - RC_DISABLE_LUA: lua is not present in the 12.4.0 tree (vestigial, harmless)
# - RC_NO_THREADS: rc_client spawns no threads of its own; all rc_client_*
#   calls happen on the AdrenalineDraw thread (single writer), so the internal
#   mutexes never contend. This also avoids a pthread dependency in the
#   -nostdlib build.
target_compile_definitions(adrenaline_user PRIVATE
  RC_CLIENT_SUPPORTS_HASH
  RC_DISABLE_LUA
  RC_NO_THREADS
)
target_link_directories(adrenaline_user PRIVATE ${CMAKE_BINARY_DIR}/kernel)
# ---------------------------------------------------------------------------
# LINK ORDER IS LOad-BEARING — read before editing target_link_libraries below.
#
# adrenaline_user is built -nostdlib -nostartfiles -fno-builtin, so neither
# newlib (libc.a) nor libgcc is linked by default; C library calls resolve to
# Sony's SceLibc_stub. That worked while the only network code was sceHttp*.
#
# The curl+OpenSSL transport in ra/ra_net.c (v9) changes that. libcurl.a and
# libcrypto.a need ~50 POSIX symbols SceLibc_stub does not export — crucially
# socket/connect/send/recv/select/getaddrinfo/poll. Those DO exist, in newlib's
# libc.a, as thin shims over sceNetSocket/sceNetConnect/sceNetSend/sceNetRecv/
# sceNetResolver* (verified in the build pod by disassembling socket.o et al).
# So libc.a must be linked — and that creates a circular dependency:
#
#     libcurl.a  ->  libc.a  ->  libSceNet_stub_weak.a / libSceIofilemgr_stub.a
#
# GNU ld makes a single left-to-right pass over archives, so without a rescan
# group the sce* symbols newly required by libc.a cannot be satisfied by stubs
# that were already passed. Measured in the pod: 79 undefined sce* symbols
# without the group, 0 with it.
#
# --start-group ... --end-group makes ld rescan until closure, fixing that.
#
# Ordering inside the group also matters and is NOT arbitrary: the Sce stubs
# come FIRST so that symbols defined by both SceLibc_stub and newlib bind to
# Sony's implementation — the one adrenaline's own lodepng.c and
# rcheevos/hash.c already use via fopen/fread. Letting newlib win would mix a
# Sony FILE* into a newlib fclose() and crash.
#
# --allow-multiple-definition: exactly FOUR symbols are defined twice, measured
# in the pod (not guessed) — fclose, strtol, strtoul, _ctype_. They collide
# because newlib packs each public function in the same object as its
# reentrant twin (lib_a-fclose.o defines both fclose and _fclose_r), and the
# _r variants ARE genuinely needed by newlib's internals, so the object cannot
# be kept out. GNU ld resolves a duplicate to the FIRST definition seen, and
# with the ordering above the first definition is the correct one in all four
# cases:
#     fclose  -> libSceLibc_stub.a   (pairs with Sony's fopen/fread — correct)
#     strtol  -> libSceLibc_stub.a
#     strtoul -> libSceLibc_stub.a
#     _ctype_ -> ra/ra_client.c      (object files always precede archives;
#                                     this is the existing -nostdlib shim, and
#                                     its 257 bytes were copied verbatim from
#                                     newlib's own lib_a-ctype_.o, so the two
#                                     definitions are byte-identical anyway)
# The winners are asserted at build time by the POST_BUILD check below — if a
# future edit reorders this list and newlib starts winning fclose, the build
# FAILS rather than silently shipping the Sony-FILE*-into-newlib-fclose crash.
#
# Cost: ~2.2 MB of static text. Verified acceptable; the Vita has 512 MB.
# ---------------------------------------------------------------------------
target_link_libraries(adrenaline_user
  AdrenalineKernel_stub
  taihen_stub
  libvita2dfbo_libA
  vitashadercollection_libA
  -Wl,--start-group
  SceRtabi_stub
  SceLibc_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceCompat_stub
  SceCtrl_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceMtpIfDriver_stub
  SceNet_stub_weak
  # RetroAchievements networking + login IME.
  #
  # These MUST be the _weak variants. SceNetCtl is NOT resident in the
  # ScePspemu process at module-load time: ra_net.c brings SceNet's provider up
  # on demand via sceSysmoduleLoadModule(SCE_SYSMODULE_NET). A NON-weak import
  # on an absent library makes the loader refuse the module, so
  # ksceKernelLoadStartModuleForPid() (kernel/main.c:145) fails, adrenaline_user
  # never starts, ScePspemuBuildFlash0()/InitAdrenaline() never run and the
  # console shows a permanent BLACK SCREEN.
  #
  # Same convention as SceNet_stub_weak above and ScePgf/SceScreenShot/
  # SceLiveArea below; SceLiveAreaUtil is the in-tree precedent that is proven
  # on hardware (weak import, provider loaded at runtime by main.c:690, called
  # at main.c:710).
  #
  # v9 REMOVED SceHttp_stub_weak and SceSsl_stub_weak. The rewrite of ra_net.c
  # deleted the last call to sceHttp*/sceSsl*/sceHttps*, so those two imports
  # now have zero referencing symbols and would only be dead weight in the
  # module's import table. curl + OpenSSL (linked statically below) supply both
  # HTTP and TLS. Removing them is also the binary-level assertion that the
  # rewrite is complete: if any sceHttp*/sceSsl* call survived anywhere in the
  # tree, this link would now FAIL with an undefined reference instead of
  # silently binding to a weak stub that returns -1 at runtime.
  SceNetCtl_stub_weak
  SceIme_stub_weak
  ScePgf_stub_weak
  ScePower_stub
  SceProcessmgr_stub
  SceRtc_stub
  SceScreenShot_stub_weak
  SceShellSvc_stub
  SceSysmem_stub
  SceSysmodule_stub
  SceUdcd_stub
  SceUsbstorVStorDriver_stub
  SceVshBridge_stub
  SceLiveArea_stub_weak
  SceAVConfig_stub

  # --- curl + OpenSSL TLS stack (vitasdk packages) -------------------------
  #
  # curl 8.17.0 over OpenSSL 1.0.2i, both already present in the build pod's
  # $VITASDK/arm-vita-eabi/lib (libcurl.a / libssl.a / libcrypto.a), installed
  # from the official vitasdk/packages VITABUILD recipes. z/zstd are curl's
  # declared dependencies (depends=('openssl' 'zlib' 'zstd')).
  #
  # v9: these stopped being a proof-of-concept dependency and became THE
  # transport. ra/ra_net.c performs every RetroAchievements request -- the login
  # POST, every rc_client server call and every badge GET -- through
  # curl_easy_perform with CURLOPT_SSL_VERIFYPEER=1 and trust anchors supplied
  # from memory via CURLOPT_CAINFO_BLOB (ra/ra_ca.h). Nothing in this module
  # calls sceHttp*/sceSsl* any more.
  #
  # These are STATIC archives, not Sce stubs: they add no module import and
  # therefore cannot cause the loader to refuse adrenaline_user the way a
  # non-weak import on an absent library would (the black-screen failure mode
  # documented above). The only NEW module imports they introduce come via
  # newlib and land on SceNet/SceNetCtl/SceIofilemgr/SceLibKernel/SceRtc/
  # SceSysmodule — every one of which is already imported above.
  curl
  ssl
  crypto
  z
  zstd

  # newlib + libgcc, LAST inside the group. libc.a supplies the POSIX/socket
  # shims; libgcc supplies the soft-float and 64-bit helpers (__aeabi_l2d,
  # __popcountdi2) that OpenSSL's bignum code pulls in. Both are reached only
  # for symbols nothing earlier defined, so Sony's libc keeps priority.
  c
  gcc
  -Wl,--end-group
  # See the "LINK ORDER IS LOAD-BEARING" block above: 4 known duplicates,
  # first-definition-wins, winners asserted by check_link_winners.sh.
  -Wl,--allow-multiple-definition
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/adrenaline_user.map
)

# ---------------------------------------------------------------------------
# Guard the --allow-multiple-definition winners AND the symbol TYPE of __errno.
#
# --allow-multiple-definition silently accepts whatever comes first. That is
# correct today only because of the library ordering above, and library
# ordering is exactly the kind of thing a later edit changes without noticing.
# Parse the linker map and fail the build if the wrong provider won.
#
# ELFFILE additionally lets the guard assert symbol TYPES in the linked image.
# That check exists because of a class of bug the map alone cannot catch: a
# direct .obj defining `int __errno;` (a data object) satisfies every
# function-typed `bl __errno` reference from newlib/curl/OpenSSL. ld emits no
# warning -- the archive member is simply never extracted -- and the module
# links clean, then branches into non-executable .bss at runtime. See the
# comment where `int __errno;` used to live in main.c.
# ---------------------------------------------------------------------------
add_custom_command(TARGET adrenaline_user POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E env
    "MAPFILE=${CMAKE_CURRENT_BINARY_DIR}/adrenaline_user.map"
    "ELFFILE=$<TARGET_FILE:adrenaline_user>"
    "NM=${CMAKE_NM}"
    bash ${CMAKE_CURRENT_SOURCE_DIR}/check_link_winners.sh
  COMMENT "Verifying libc symbol resolution (fclose/strtol/strtoul -> SceLibc, __errno -> newlib fn)"
  VERBATIM
)

vita_create_self(adrenaline_user.suprx adrenaline_user CONFIG exports.yml UNSAFE)

#vita_create_stubs(ustubs adrenaline_user ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml)

#add_custom_target(copy
#  COMMAND cp adrenaline_user.suprx F:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(pkg
#  COMMAND cp adrenaline_user.suprx ../../bubble/pkg/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(updater
#  COMMAND bin2c adrenaline_user.suprx ../../cef/updater/adrenaline_user.h adrenaline_user
#  DEPENDS adrenaline_user.suprx
#)

add_custom_target(user_all
  ALL
  DEPENDS adrenaline_user.suprx-self
)
