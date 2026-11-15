# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.22)

# CEF supports exactly these configurations.
set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "Supported configurations" FORCE)

if(APPLE)
  project(TorFox VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
  project(TorFox VERSION 0.1.0 LANGUAGES C CXX)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(TORFOX_BUILD_APP "Build the TorFox application" ON)
option(TORFOX_BUILD_TESTS "Build unit tests" ON)
option(TORFOX_TEST_HOOKS
  "Compile in test-only hooks (loopback remote debugging, test log, test resolver rules). Never ship."
  OFF)
option(TORFOX_BUILD_SPIKES "Build the Stage 0 CEF feasibility spikes" OFF)

include(cmake/TorFoxCEF.cmake)
include(cmake/TorFoxDeps.cmake)
find_package(CEF REQUIRED)
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

if(OS_WINDOWS)
  # Logical target used to link libcef.lib.
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

add_subdirectory(src/core)

if(TORFOX_BUILD_APP)
  add_subdirectory(src/browser)
endif()

if(TORFOX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
  if(TORFOX_TEST_HOOKS AND OS_WINDOWS)
    add_subdirectory(tests/e2e/helpers)
  endif()
endif()

if(TORFOX_BUILD_SPIKES)
  add_subdirectory(spikes/cefspike)
endif()
