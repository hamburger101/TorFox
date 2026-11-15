# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# torfox_core: platform logic with no CEF dependency, so it can be unit tested
# directly. Compiled with CEF's flags (no exceptions, no RTTI, same runtime).

torfox_verified_dep(TORFOX_JSON_DIR nlohmann_json header)
torfox_verified_dep(TORFOX_SQLITE_DIR sqlite source)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# libsodium, for the updater's signature check (Ed25519 and BLAKE2b). The
# official release, verified by fetch_deps.py with libsodium's minisign key
# and pinned by SHA-256: on Windows its prebuilt static MSVC library (static
# runtime, like CEF); on macOS built here from the source release.
if(WIN32)
  torfox_verified_dep(TORFOX_SODIUM_DIR libsodium windows-msvc)
  set(_sodium "${TORFOX_SODIUM_DIR}/libsodium")
  add_library(torfox_sodium STATIC IMPORTED GLOBAL)
  set_target_properties(torfox_sodium PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_LOCATION_RELEASE "${_sodium}/x64/Release/v143/static/libsodium.lib"
    IMPORTED_LOCATION_DEBUG "${_sodium}/x64/Debug/v143/static/libsodium.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_sodium}/include"
    INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC=1")
elseif(APPLE)
  torfox_verified_dep(TORFOX_SODIUM_SRC libsodium source)
  include(ExternalProject)
  set(_sodium "${CMAKE_CURRENT_BINARY_DIR}/libsodium")
  set(_sodium_cflags "-O2 -fPIC")
  foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
    string(APPEND _sodium_cflags " -arch ${_arch}")
  endforeach()
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    string(APPEND _sodium_cflags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  ExternalProject_Add(torfox_sodium_build
    SOURCE_DIR "${TORFOX_SODIUM_SRC}"
    BINARY_DIR "${_sodium}/build"
    INSTALL_DIR "${_sodium}"
    CONFIGURE_COMMAND "${TORFOX_SODIUM_SRC}/configure" --prefix=<INSTALL_DIR> --disable-shared
                      --enable-static --disable-dependency-tracking "CFLAGS=${_sodium_cflags}"
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS "${_sodium}/lib/libsodium.a")
  file(MAKE_DIRECTORY "${_sodium}/include")
  add_library(torfox_sodium STATIC IMPORTED GLOBAL)
  set_target_properties(torfox_sodium PROPERTIES
    IMPORTED_LOCATION "${_sodium}/lib/libsodium.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_sodium}/include")
  add_dependencies(torfox_sodium torfox_sodium_build)
else()
  message(FATAL_ERROR "TorFox builds for Windows and macOS only.")
endif()

# SQLite, for history and bookmarks. Deleted rows are overwritten
# (SECURE_DELETE); extensions and URI file names are compiled out.
add_library(torfox_sqlite STATIC "${TORFOX_SQLITE_DIR}/sqlite3.c")
torfox_third_party_flags(torfox_sqlite)
# Expose only sqlite3.h: the release folder also holds a file named VERSION,
# which would shadow the C++ <version> header on case-insensitive file systems.
configure_file("${TORFOX_SQLITE_DIR}/sqlite3.h" "${CMAKE_CURRENT_BINARY_DIR}/sqlite-include/sqlite3.h" COPYONLY)
target_include_directories(torfox_sqlite SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/sqlite-include")
target_compile_definitions(torfox_sqlite PRIVATE
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
  SQLITE_OMIT_DEPRECATED=1
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_OMIT_SHARED_CACHE=1
  SQLITE_SECURE_DELETE=1
  SQLITE_THREADSAFE=1
  SQLITE_USE_URI=0
)

# The Public Suffix List, vendored in third_party/publicsuffix and compiled in.
set(TORFOX_CORE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_psl "${CMAKE_SOURCE_DIR}/third_party/publicsuffix")
add_custom_command(
  OUTPUT "${TORFOX_CORE_GEN_DIR}/psl_data.inc" "${TORFOX_CORE_GEN_DIR}/psl_vectors.inc"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TORFOX_CORE_GEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/scripts/gen_psl.py"
          "${TORFOX_CORE_GEN_DIR}/psl_data.inc" --tests "${TORFOX_CORE_GEN_DIR}/psl_vectors.inc"
  DEPENDS "${CMAKE_SOURCE_DIR}/scripts/gen_psl.py" "${_psl}/public_suffix_list.dat"
          "${_psl}/test_psl.txt" "${_psl}/SOURCE.json"
  COMMENT "Generating the Public Suffix List tables"
)
set(_trackers "${CMAKE_SOURCE_DIR}/third_party/easyprivacy")
add_custom_command(
  OUTPUT "${TORFOX_CORE_GEN_DIR}/trackers_data.inc"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TORFOX_CORE_GEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/scripts/gen_trackers.py"
          "${TORFOX_CORE_GEN_DIR}/trackers_data.inc"
  DEPENDS "${CMAKE_SOURCE_DIR}/scripts/gen_trackers.py" "${_trackers}/domains.txt"
          "${_trackers}/sources.lock.json"
  COMMENT "Generating the tracker domain table (EasyPrivacy)"
)
set(_easylist "${CMAKE_SOURCE_DIR}/third_party/easylist")
add_custom_command(
  OUTPUT "${TORFOX_CORE_GEN_DIR}/adblock_data.inc"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TORFOX_CORE_GEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/scripts/gen_adblock.py"
          "${TORFOX_CORE_GEN_DIR}/adblock_data.inc"
  DEPENDS "${CMAKE_SOURCE_DIR}/scripts/gen_adblock.py" "${_easylist}/filters.txt"
          "${_easylist}/sources.lock.json"
  COMMENT "Generating the ad-blocking rules (EasyList)"
)

add_library(torfox_core STATIC
  "${TORFOX_CORE_GEN_DIR}/psl_data.inc"
  "${TORFOX_CORE_GEN_DIR}/psl_vectors.inc"
  "${TORFOX_CORE_GEN_DIR}/trackers_data.inc"
  "${TORFOX_CORE_GEN_DIR}/adblock_data.inc"
  json.cc json.h
  adblock/filters.cc adblock/filters.h
  adblock/sites.cc adblock/sites.h
  browsing/chromium_profile.cc browsing/chromium_profile.h
  browsing/context_menu.cc browsing/context_menu.h
  browsing/places.cc browsing/places.h
  browsing/session.cc browsing/session.h
  browsing/shortcuts.cc browsing/shortcuts.h
  browsing/tabs.cc browsing/tabs.h
  browsing/zoom.cc browsing/zoom.h
  bridge/validator.cc bridge/validator.h
  cmdline/policy.cc cmdline/policy.h
  crypto/os_crypto.cc crypto/os_crypto.h
  downloads/policy.cc downloads/policy.h
  privacy/clear_data.cc privacy/clear_data.h
  privacy/external_protocols.cc privacy/external_protocols.h
  privacy/https_first.cc privacy/https_first.h
  privacy/trackers.cc privacy/trackers.h
  settings/schema.cc settings/schema.h
  settings/store.cc settings/store.h
  tor/bridges.cc tor/bridges.h
  tor/control.cc tor/control.h
  tor/controller.cc tor/controller.h
  tor/loopback_channel.cc
  tor/onion.cc tor/onion.h
  tor/torrc.cc tor/torrc.h
  update/minisign.cc update/minisign.h
  update/release.cc update/release.h
  update/sha256.cc update/sha256.h
  update/whats_new.cc update/whats_new.h
  memory/report.cc memory/report.h
  url/guards.cc url/guards.h
  url/internal_url.cc url/internal_url.h
  url/nav_input.cc url/nav_input.h
  url/public_suffix.cc url/public_suffix.h
  url/search.cc url/search.h
  util/file_io.cc util/file_io.h
  util/utf8.cc util/utf8.h
)
SET_COMMON_TARGET_PROPERTIES(torfox_core)
if(MSVC)
  # One section per EasyList rule string (about 78,000): more than an object file
  # holds without /bigobj.
  set_source_files_properties(adblock/filters.cc PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()
target_include_directories(torfox_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_include_directories(torfox_core SYSTEM PUBLIC "${TORFOX_JSON_DIR}")
target_include_directories(torfox_core PUBLIC "${TORFOX_CORE_GEN_DIR}")
target_link_libraries(torfox_core PUBLIC torfox_sqlite)
target_link_libraries(torfox_core PRIVATE torfox_sodium)
if(WIN32)
  # Sockets (tor's control port) and CNG (HMAC, random numbers).
  target_link_libraries(torfox_core PUBLIC ws2_32 bcrypt)
elseif(APPLE)
  target_link_libraries(torfox_core PUBLIC "-framework Security")
endif()
target_compile_definitions(torfox_core PUBLIC
  JSON_NOEXCEPTION=1
  JSON_USE_IMPLICIT_CONVERSIONS=0
)
