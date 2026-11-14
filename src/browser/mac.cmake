# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# macOS: TorFox.app, with the CEF framework and the five helper app bundles
# CEF expects in Contents/Frameworks. Follows CEF's cefsimple layout for this
# CEF version (tests/cefsimple/CMakeLists.txt in the CEF distribution).
# Included from src/browser/CMakeLists.txt.

if(NOT USE_SANDBOX)
  message(FATAL_ERROR "TorFox requires USE_SANDBOX=ON: every helper initialises the sandbox.")
endif()

set(TORFOX_BUNDLE_ID "io.github.hamburger101.torfox")

set(CEF_TARGET "TorFox")
set(CEF_HELPER_TARGET "TorFox_Helper")
set(CEF_HELPER_OUTPUT_NAME "TorFox Helper")
SET_CEF_TARGET_OUT_DIR()

set(CEF_APP "${CEF_TARGET_OUT_DIR}/${CEF_TARGET}.app")

# CEF's macros apply its compiler flags (C++20, no exceptions, no RTTI,
# warnings as errors) to C and C++ only; Objective-C++ files need the same.
macro(TORFOX_OBJCXX_FLAGS target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:${CEF_COMPILER_FLAGS} ${CEF_CXX_COMPILER_FLAGS}>
    $<$<AND:$<COMPILE_LANGUAGE:OBJCXX>,$<CONFIG:Debug>>:${CEF_COMPILER_FLAGS_DEBUG} ${CEF_CXX_COMPILER_FLAGS_DEBUG}>
    $<$<AND:$<COMPILE_LANGUAGE:OBJCXX>,$<CONFIG:Release>>:${CEF_COMPILER_FLAGS_RELEASE} ${CEF_CXX_COMPILER_FLAGS_RELEASE}>)
endmacro()

# All TorFox code, shared by the app and its helpers: the renderer helper runs
# TorFox's renderer-side code (the bridge, which only internal pages get).
set(TORFOX_MAC_SOURCES platform/platform_mac.mm)
set_source_files_properties(${TORFOX_MAC_SOURCES} main/mac_main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
add_library(torfox_browser STATIC ${TORFOX_BROWSER_SOURCES} ${TORFOX_MAC_SOURCES})
SET_COMMON_TARGET_PROPERTIES(torfox_browser)
TORFOX_OBJCXX_FLAGS(torfox_browser)
target_include_directories(torfox_browser PUBLIC "${CMAKE_SOURCE_DIR}" "${TORFOX_GEN_DIR}")
if(TORFOX_TEST_HOOKS)
  target_compile_definitions(torfox_browser PUBLIC TORFOX_TEST_HOOKS=1)
endif()
add_dependencies(torfox_browser libcef_dll_wrapper)
target_link_libraries(torfox_browser PUBLIC torfox_core libcef_dll_wrapper ${CEF_STANDARD_LIBS})

# ----- Main app bundle -------------------------------------------------------
# Variables referenced from mac/Info.plist.in.
set(EXECUTABLE_NAME "${CEF_TARGET}")
set(PRODUCT_NAME "${CEF_TARGET}")
set(BUNDLE_ID "${TORFOX_BUNDLE_ID}")

add_executable(${CEF_TARGET} MACOSX_BUNDLE main/mac_main.mm)
SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
TORFOX_OBJCXX_FLAGS(${CEF_TARGET})
target_link_libraries(${CEF_TARGET} PRIVATE torfox_browser)
set_target_properties(${CEF_TARGET} PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in")

# The CEF framework goes into Contents/Frameworks (loaded at run time by
# CefScopedLibraryLoader, as the macOS sandbox requires).
COPY_MAC_FRAMEWORK("${CEF_TARGET}" "${CEF_BINARY_DIR}" "${CEF_APP}")

# ----- Helper app bundles ----------------------------------------------------
foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
  string(REPLACE ":" ";" _suffix_list ${_suffix_list})
  list(GET _suffix_list 0 _name_suffix)
  list(GET _suffix_list 1 _target_suffix)
  list(GET _suffix_list 2 _plist_suffix)

  set(_helper_target "${CEF_HELPER_TARGET}${_target_suffix}")
  set(_helper_output_name "${CEF_HELPER_OUTPUT_NAME}${_name_suffix}")

  # One Info.plist per helper (configure_file would use global variables).
  set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mac/helper-Info.plist.in" _plist_contents)
  string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${BUNDLE_ID}" "${TORFOX_BUNDLE_ID}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${VERSION}" "${PROJECT_VERSION}" _plist_contents "${_plist_contents}")
  file(WRITE "${_helper_info_plist}" "${_plist_contents}")

  add_executable(${_helper_target} MACOSX_BUNDLE main/mac_helper.cc)
  SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
  target_link_libraries(${_helper_target} PRIVATE torfox_browser)
  set_target_properties(${_helper_target} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${_helper_info_plist}"
    OUTPUT_NAME "${_helper_output_name}")

  add_dependencies(${CEF_TARGET} "${_helper_target}")
  add_custom_command(
    TARGET ${CEF_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
            "${CEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
    VERBATIM)
endforeach()
