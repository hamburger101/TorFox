# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# torfox_core: platform logic with no CEF dependency, so it can be unit tested
# directly. Compiled with CEF's flags (no exceptions, no RTTI, same runtime).

torfox_verified_dep(TORFOX_JSON_DIR nlohmann_json header)

add_library(torfox_core STATIC
  json.cc json.h
  bridge/validator.cc bridge/validator.h
  cmdline/policy.cc cmdline/policy.h
  settings/schema.cc settings/schema.h
  settings/store.cc settings/store.h
  url/internal_url.cc url/internal_url.h
  url/nav_input.cc url/nav_input.h
  util/file_io.cc util/file_io.h
)
SET_COMMON_TARGET_PROPERTIES(torfox_core)
target_include_directories(torfox_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_include_directories(torfox_core SYSTEM PUBLIC "${TORFOX_JSON_DIR}")
target_compile_definitions(torfox_core PUBLIC
  JSON_NOEXCEPTION=1
  JSON_USE_IMPLICIT_CONVERSIONS=0
)
