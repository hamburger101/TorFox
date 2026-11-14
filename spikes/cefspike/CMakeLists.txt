# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Stage 0 feasibility spike (Windows only; not shipped).

if(NOT OS_WINDOWS)
  return()
endif()
if(NOT USE_SANDBOX)
  message(FATAL_ERROR "The spike requires USE_SANDBOX=ON (bootstrap.exe).")
endif()

set(CEF_TARGET "cefspike")
SET_CEF_TARGET_OUT_DIR()

add_library(${CEF_TARGET} SHARED spike.cc spike.h spike_win.cc)
SET_LIBRARY_TARGET_PROPERTIES(${CEF_TARGET})
target_include_directories(${CEF_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}")
COPY_SINGLE_FILE(${CEF_TARGET} ${CEF_BINARY_DIR}/bootstrap.exe ${CEF_TARGET_OUT_DIR}/${CEF_TARGET}.exe)

add_dependencies(${CEF_TARGET} libcef_dll_wrapper)
target_link_libraries(${CEF_TARGET} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})

COPY_FILES("${CEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES("${CEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
SET_LPAC_ACLS("${CEF_TARGET}")
