# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Locates the pinned, verified CEF binary distribution described in
# deps.lock.json. Configuration fails unless scripts/fetch_deps.py has verified
# the archive (the .verified marker must hold the pinned SHA-256).

set(TORFOX_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "Directory holding verified third-party dependencies")
set(TORFOX_CEF_FLAVOR "standard" CACHE STRING "CEF distribution flavor: standard or minimal")
set_property(CACHE TORFOX_CEF_FLAVOR PROPERTY STRINGS standard minimal)

file(READ "${CMAKE_SOURCE_DIR}/deps.lock.json" _torfox_lock)
string(JSON TORFOX_CEF_VERSION GET "${_torfox_lock}" cef version)
string(JSON TORFOX_CHROMIUM_VERSION GET "${_torfox_lock}" cef chromium_version)

if(WIN32)
  set(_torfox_cef_platform "windows64")
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_torfox_cef_platform "macosarm64")
  elseif(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set(_torfox_cef_platform "macosx64")
  elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_torfox_cef_platform "macosarm64")
  else()
    set(_torfox_cef_platform "macosx64")
  endif()
else()
  message(FATAL_ERROR "TorFox supports Windows and macOS only.")
endif()

set(_torfox_cef_key "${_torfox_cef_platform}-${TORFOX_CEF_FLAVOR}")
string(JSON _torfox_cef_archive ERROR_VARIABLE _torfox_json_err GET "${_torfox_lock}" cef files ${_torfox_cef_key} name)
if(_torfox_json_err)
  message(FATAL_ERROR "deps.lock.json has no CEF artifact '${_torfox_cef_key}'.")
endif()
string(JSON _torfox_cef_sha256 GET "${_torfox_lock}" cef files ${_torfox_cef_key} sha256)

string(REGEX REPLACE "\\.tar\\.(bz2|gz|xz)$" "" _torfox_cef_dirname "${_torfox_cef_archive}")
set(CEF_ROOT "${TORFOX_DEPS_DIR}/cef/${_torfox_cef_dirname}")
set(_torfox_cef_marker "${TORFOX_DEPS_DIR}/cef/${_torfox_cef_archive}.verified")

if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake" OR NOT EXISTS "${_torfox_cef_marker}")
  message(FATAL_ERROR
    "Verified CEF distribution not found at ${CEF_ROOT}.\n"
    "Run: python scripts/fetch_deps.py cef ${_torfox_cef_key}")
endif()
file(READ "${_torfox_cef_marker}" _torfox_marker_sha)
string(STRIP "${_torfox_marker_sha}" _torfox_marker_sha)
if(NOT _torfox_marker_sha STREQUAL _torfox_cef_sha256)
  message(FATAL_ERROR
    "CEF at ${CEF_ROOT} was verified against a different SHA-256 than deps.lock.json pins.\n"
    "Re-run: python scripts/fetch_deps.py cef ${_torfox_cef_key}")
endif()

message(STATUS "TorFox: using verified CEF ${TORFOX_CEF_VERSION} (${_torfox_cef_key})")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
