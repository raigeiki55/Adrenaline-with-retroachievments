# Third-Party Notices — Adrenaline+ (RetroAchievements build)

This file lists the third-party free/open-source software components that are
**actually shipped** in, or statically linked into, the distributed binaries of
this Adrenaline+ RetroAchievements build (`Adrenaline-trophy-*.vpk`), together
with each component's license and upstream location. It satisfies the
attribution/notice obligations of the MIT, zlib, BSD-2-Clause and curl/OpenSSL
licenses that accompany those components.

Adrenaline itself is distributed under the **GNU General Public License v3.0**;
see the root [`LICENSE`](./LICENSE) file. The GPL source-availability obligation
for the copyleft components is met by publishing this tree.

Where a component carries a permissive license whose full text must be
redistributed, the verbatim notice is vendored next to the component in this
tree; the "Notice in tree" column points at it.

| Component | Version / pin | License | Upstream | Notice in tree |
|---|---|---|---|---|
| Adrenaline | upstream (TheFloW) | GPL-3.0-or-later | https://github.com/TheOfficialFloW/Adrenaline | [`LICENSE`](./LICENSE) (root) |
| Adrenaline+ fork (isage) | as shipped | GPL-3.0-or-later | https://github.com/isage/Adrenaline | [`LICENSE`](./LICENSE) (root) |
| Adrenaline+ trophy/RA fork | this tree ("by Cat and GrayJack") | GPL-3.0-or-later | (this repository) | [`LICENSE`](./LICENSE) (root) |
| rcheevos | 12.4.0 | MIT | https://github.com/RetroAchievements/rcheevos | [`user/rcheevos/LICENSE`](./user/rcheevos/LICENSE) |
| LodePNG | 20250506 | zlib | https://github.com/lvandeve/lodepng | [`user/lodepng/LICENSE`](./user/lodepng/LICENSE) |
| lz4 (vendored `lib` sources) | 1.8.x | BSD-2-Clause | https://github.com/lz4/lz4 | [`user/lz4/LICENSE`](./user/lz4/LICENSE) |
| vita2dlib (frangarcj `fbo` fork) | git tag `fbo` | MIT | https://github.com/frangarcj/vita2dlib | fetched by `ExternalProject_Add` (`user/CMakeLists.txt`); upstream `LICENSE` retained under `build/user/libvita2dfbo/LICENSE` at build time |
| vita-shader-collection | release `master-0.2` | see upstream | https://github.com/VitaArchive/vita-shader-collection | fetched by `ExternalProject_Add` (`user/CMakeLists.txt`) |
| psp-cfw-sdk | commit `9a6a90b047156f353a6749ff967c2b7f207b063b` | mixed per sub-component | https://github.com/psp2dev/psp-cfw-sdk | see component: `external/psp-cfw-sdk/src/BootLoadEx/LICENSE` (GPL-3.0), `external/psp-cfw-sdk/src/iplsdk/LICENSE` (MIT), `external/psp-cfw-sdk/src/LibPspExploit/LICENSE.txt` (WTFPL-2.0) |
| libcurl | 8.17.0 | curl (MIT-like) | https://curl.se/ | linked from the VitaSDK package; full text: https://curl.se/docs/copyright.html |
| OpenSSL | 1.0.2i | OpenSSL + SSLeay (dual) | https://www.openssl.org/ | linked from the VitaSDK package; full text ships with OpenSSL `LICENSE` |
| Epinephrine CFW cores | as shipped in `cef/core/` | per component (pentazemin GPL-3.0) | https://github.com/psp2dev (Epinephrine) | `cef/core/pentazemin/LICENSE` (GPL-3.0); other cores under `cef/core/*` — see each component's own headers |

## Notes

- **rcheevos** is the RetroAchievements client runtime (`rc_client`, `rc_hash`,
  `rc_api`) vendored under `user/rcheevos/`. Its MIT notice is reproduced
  verbatim in [`user/rcheevos/LICENSE`](./user/rcheevos/LICENSE), copied from the
  upstream `LICENSE` at the repository root.
- **LodePNG** is vendored under `user/lodepng/`. Its zlib notice — already
  embedded at the top of `user/lodepng/lodepng.h` — is reproduced verbatim in
  [`user/lodepng/LICENSE`](./user/lodepng/LICENSE).
- **lz4** is vendored under `user/lz4/` (`lz4.c`, `lz4.h`). The upstream
  project is dual-licensed (BSD-2-Clause for `lib/`, GPL-2.0 otherwise); only
  the BSD-2-Clause `lib` sources are vendored and shipped here, as recorded in
  the file headers and [`user/lz4/LICENSE`](./user/lz4/LICENSE).
- **vita2dlib** and **vita-shader-collection** are not vendored as source in
  this tree; they are fetched at build time by CMake `ExternalProject_Add`
  (see `user/CMakeLists.txt`). Their licenses remain those of their upstream
  repositories and are preserved in their respective build/download
  directories.
- The **curl** and **OpenSSL** static libraries come from the VitaSDK package
  set in the build container and are statically linked into
  `adrenaline_user.suprx`. They are not vendored as source here; their license
  texts are available from the upstream locations linked above.
- This build contains **no** monetization, in-app purchase, paid tier,
  analytics SDK, or third-party advertising/social-login component.

This notice file was added in Adrenaline+ v32. It does not include a privacy
policy; that document is intentionally deferred pending an authoritative
data-residency/retention statement from RetroAchievements (see the v32 build
report).
