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

configure_file(app/version.h.in "${TORFOX_GEN_DIR}/src/browser/app/version.h" @ONLY)

# ----- Sources ---------------------------------------------------------------
set(TORFOX_BROWSER_SOURCES
  app/app_common.cc app/app_common.h
  app/browser_app.cc app/browser_app.h
  app/browser_registry.cc app/browser_registry.h
  app/fault_injection.cc app/fault_injection.h
  app/context_manager.cc app/context_manager.h
  app/renderer_app.cc app/renderer_app.h
  app/services.h
  app/places_service.cc app/places_service.h
  app/settings_service.cc app/settings_service.h
  app/test_log.cc app/test_log.h
  bridge/bridge_handler.cc bridge/bridge_handler.h
  platform/platform.h
  scheme/internal_scheme.cc scheme/internal_scheme.h
  scheme/resources.h
  shell/browser_window.cc shell/browser_window.h
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
    dwmapi shell32 ole32 advapi32 user32)

  # bootstrap.exe, renamed, loads TorFox.dll and initialises the sandbox.
  COPY_SINGLE_FILE(${CEF_TARGET} "${CEF_BINARY_DIR}/bootstrap.exe" "${CEF_TARGET_OUT_DIR}/TorFox.exe")
  COPY_FILES("${CEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
  COPY_FILES("${CEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
  # Lets the AppContainer-sandboxed network service read the install folder.
  SET_LPAC_ACLS("${CEF_TARGET}")
elseif(OS_MAC)
  include(mac.cmake)
endif()
