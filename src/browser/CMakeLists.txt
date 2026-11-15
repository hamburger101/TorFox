# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The TorFox application: CEF browser process, renderer hooks, Views shell and
# the compiled-in UI bundle.

find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(OS_WINDOWS)
  find_program(TORFOX_NPM NAMES npm.cmd npm REQUIRED)
else()
  find_program(TORFOX_NPM NAMES npm REQUIRED)
endif()

set(TORFOX_UI_DIR "${CMAKE_SOURCE_DIR}/ui")
if(NOT EXISTS "${TORFOX_UI_DIR}/node_modules/.package-lock.json")
  message(FATAL_ERROR
    "UI dependencies are not installed. Run: npm --prefix ui ci  (install scripts are disabled by ui/.npmrc)")
endif()

# ----- UI bundle -> resource table ------------------------------------------
set(TORFOX_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TORFOX_UI_DIST "${CMAKE_CURRENT_BINARY_DIR}/ui-dist")
file(GLOB_RECURSE TORFOX_UI_SOURCES CONFIGURE_DEPENDS
  "${TORFOX_UI_DIR}/src/*" "${TORFOX_UI_DIR}/pages/*")
set(TORFOX_UI_CONFIG
  "${TORFOX_UI_DIR}/package.json" "${TORFOX_UI_DIR}/package-lock.json"
  "${TORFOX_UI_DIR}/tsconfig.json" "${TORFOX_UI_DIR}/vite.config.ts")

add_custom_command(
  OUTPUT "${TORFOX_GEN_DIR}/ui.stamp"
  COMMAND "${TORFOX_NPM}" run build -- --outDir "${TORFOX_UI_DIST}" --emptyOutDir
  COMMAND "${CMAKE_COMMAND}" -E touch "${TORFOX_GEN_DIR}/ui.stamp"
  WORKING_DIRECTORY "${TORFOX_UI_DIR}"
  DEPENDS ${TORFOX_UI_SOURCES} ${TORFOX_UI_CONFIG}
  COMMENT "Building the TorFox UI (TypeScript + Vite)"
  VERBATIM)

add_custom_command(
  OUTPUT "${TORFOX_GEN_DIR}/resources.cc"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/scripts/gen_resources.py"
          "${TORFOX_UI_DIST}" "${TORFOX_GEN_DIR}/resources.cc"
  DEPENDS "${TORFOX_GEN_DIR}/ui.stamp" "${CMAKE_SOURCE_DIR}/scripts/gen_resources.py"
  COMMENT "Embedding the TorFox UI"
  VERBATIM)

file(READ "${CMAKE_SOURCE_DIR}/deps.lock.json" _torfox_deps_lock)
string(JSON TORFOX_TOR_BUNDLE_VERSION GET "${_torfox_deps_lock}" tor tor_browser_version)
configure_file(app/version.h.in "${TORFOX_GEN_DIR}/src/browser/app/version.h" @ONLY)

# This version's release notes (docs/release-notes/<version>.txt) for the
# What's new page. A version without notes (a development build) has none;
# scripts/release.py refuses to publish one. Adding or editing the file makes
# CMake configure again.
set(_torfox_notes_dir "${CMAKE_SOURCE_DIR}/docs/release-notes")
set(_torfox_notes "${_torfox_notes_dir}/${PROJECT_VERSION}.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_torfox_notes_dir}")
set(TORFOX_RELEASE_NOTES_BYTES "")
set(TORFOX_RELEASE_NOTES_SIZE 0)
if(EXISTS "${_torfox_notes}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_torfox_notes}")
  file(SIZE "${_torfox_notes}" TORFOX_RELEASE_NOTES_SIZE)
  if(TORFOX_RELEASE_NOTES_SIZE GREATER 16384)
    message(FATAL_ERROR "${_torfox_notes} is longer than 16 KiB.")
  endif()
  file(READ "${_torfox_notes}" _torfox_notes_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," TORFOX_RELEASE_NOTES_BYTES "${_torfox_notes_hex}")
endif()
configure_file(app/release_notes.h.in "${TORFOX_GEN_DIR}/src/browser/app/release_notes.h" @ONLY)

# The updater's repository and public key (config/release.json; never secrets).
file(READ "${CMAKE_SOURCE_DIR}/config/release.json" _torfox_release)
string(JSON _owner GET "${_torfox_release}" github_owner)
string(JSON _repo GET "${_torfox_release}" github_repo)
string(JSON TORFOX_UPDATE_PUBLIC_KEY GET "${_torfox_release}" update_public_key)
if(NOT _owner MATCHES "^[A-Za-z0-9._-]*$" OR NOT _repo MATCHES "^[A-Za-z0-9._-]*$")
  message(FATAL_ERROR "config/release.json: github_owner and github_repo may hold only letters, digits, '.', '-' and '_'.")
endif()
if(_owner STREQUAL "" OR _repo STREQUAL "")
  set(TORFOX_UPDATE_REPOSITORY "")
else()
  set(TORFOX_UPDATE_REPOSITORY "${_owner}/${_repo}")
endif()
if(NOT TORFOX_UPDATE_PUBLIC_KEY STREQUAL "" AND NOT TORFOX_UPDATE_PUBLIC_KEY MATCHES "^RW[A-Za-z0-9+/]+$")
  message(FATAL_ERROR "config/release.json: update_public_key must be the RW... line of a minisign public key.")
endif()
if(TORFOX_TEST_HOOKS)
  # Test builds never look for real updates: the update tests point them at
  # a fake GitHub with a throw-away key (--torfox-test-update-config), and no
  # other test may reach the real one.
  set(TORFOX_UPDATE_PUBLIC_KEY "")
  set(TORFOX_UPDATE_REPOSITORY "")
  message(STATUS "TorFox updates: off in this build (test build)")
elseif(TORFOX_UPDATE_PUBLIC_KEY STREQUAL "" OR TORFOX_UPDATE_REPOSITORY STREQUAL "")
  message(STATUS "TorFox updates: off in this build (config/release.json has no update_public_key)")
else()
  message(STATUS "TorFox updates: ${TORFOX_UPDATE_REPOSITORY}, key ${TORFOX_UPDATE_PUBLIC_KEY}")
endif()
configure_file(app/release_config.h.in "${TORFOX_GEN_DIR}/src/browser/app/release_config.h" @ONLY)

# ----- Sources ---------------------------------------------------------------
set(TORFOX_BROWSER_SOURCES
  app/app_common.cc app/app_common.h
  app/browser_app.cc app/browser_app.h
  app/browser_registry.cc app/browser_registry.h
  app/fault_injection.cc app/fault_injection.h
  app/content_filter.cc app/content_filter.h
  app/context_manager.cc app/context_manager.h
  app/download_service.cc app/download_service.h
  app/renderer_app.cc app/renderer_app.h
  app/services.h
  app/places_service.cc app/places_service.h
  app/privacy_service.cc app/privacy_service.h
  app/settings_service.cc app/settings_service.h
  app/test_log.cc app/test_log.h
  app/session_service.cc app/session_service.h
  app/update_service.cc app/update_service.h
  app/tor_service.cc app/tor_service.h
  bridge/bridge_handler.cc bridge/bridge_handler.h
  platform/platform.h
  scheme/internal_scheme.cc scheme/internal_scheme.h
  scheme/resources.h
  shell/browser_window.cc shell/browser_window.h
  shell/dialogs.cc shell/dialogs.h
  shell/menus.cc shell/menus.h
  shell/tab_client.cc shell/tab_client.h
  "${TORFOX_GEN_DIR}/resources.cc"
)

if(OS_WINDOWS)
  if(NOT USE_SANDBOX)
    message(FATAL_ERROR "TorFox requires USE_SANDBOX=ON (CEF bootstrap executable).")
  endif()

  set(CEF_TARGET "TorFox")
  SET_CEF_TARGET_OUT_DIR()

  add_library(${CEF_TARGET} SHARED
    ${TORFOX_BROWSER_SOURCES}
    main/win_entry.cc
    platform/platform_win.cc
  )
  SET_LIBRARY_TARGET_PROPERTIES(${CEF_TARGET})
  target_include_directories(${CEF_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}" "${TORFOX_GEN_DIR}")
  if(TORFOX_TEST_HOOKS)
    target_compile_definitions(${CEF_TARGET} PRIVATE TORFOX_TEST_HOOKS=1)
  endif()
  # Release builds also produce a PDB (kept out of installers) so crashes can
  # be symbolized; optimisation settings are unchanged.
  target_compile_options(${CEF_TARGET} PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(${CEF_TARGET} PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>)
  add_dependencies(${CEF_TARGET} libcef_dll_wrapper)
  target_link_libraries(${CEF_TARGET} PRIVATE
    torfox_core libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS}
    dwmapi shell32 ole32 advapi32 user32 ws2_32)

  # bootstrap.exe, renamed, loads TorFox.dll and initialises the sandbox.
  COPY_SINGLE_FILE(${CEF_TARGET} "${CEF_BINARY_DIR}/bootstrap.exe" "${CEF_TARGET_OUT_DIR}/TorFox.exe")
  COPY_FILES("${CEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
  COPY_FILES("${CEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
  # Lets the AppContainer-sandboxed network service read the install folder.
  SET_LPAC_ACLS("${CEF_TARGET}")

  # The official Tor Expert Bundle (OpenPGP-verified, then pinned by SHA-256
  # in deps.lock.json): tor, its pluggable transports, the GeoIP files and
  # the licences, in <TorFox>/tor. tor-gencert is a relay tool: left out.
  torfox_verified_dep(TORFOX_TOR_DIR tor windows-x86_64)
  set(_tor_out "${CEF_TARGET_OUT_DIR}/tor")
  add_custom_command(TARGET ${CEF_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_tor_out}/data" "${_tor_out}/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${TORFOX_TOR_DIR}/tor" "${_tor_out}"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_tor_out}/tor-gencert.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TORFOX_TOR_DIR}/data/geoip" "${TORFOX_TOR_DIR}/data/geoip6" "${_tor_out}/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${TORFOX_TOR_DIR}/docs" "${_tor_out}/docs"
    VERBATIM)
elseif(OS_MAC)
  include(mac.cmake)
endif()
