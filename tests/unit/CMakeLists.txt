# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

torfox_verified_dep(TORFOX_GTEST_DIR googletest source)

add_library(torfox_gtest STATIC
  "${TORFOX_GTEST_DIR}/googletest/src/gtest-all.cc"
  "${TORFOX_GTEST_DIR}/googletest/src/gtest_main.cc"
)
target_include_directories(torfox_gtest SYSTEM PUBLIC "${TORFOX_GTEST_DIR}/googletest/include")
target_include_directories(torfox_gtest PRIVATE "${TORFOX_GTEST_DIR}/googletest")
torfox_third_party_flags(torfox_gtest)

add_executable(torfox_unit_tests
  bridge_validator_test.cc
  cmdline_policy_test.cc
  internal_url_test.cc
  json_test.cc
  nav_input_test.cc
  settings_test.cc
  test_util.h
)
SET_EXECUTABLE_TARGET_PROPERTIES(torfox_unit_tests)
target_link_libraries(torfox_unit_tests PRIVATE torfox_core torfox_gtest)
if(OS_WINDOWS)
  # Console test runner; CEF's default subsystem flags target GUI apps.
  set_property(TARGET torfox_unit_tests APPEND_STRING PROPERTY LINK_FLAGS " /SUBSYSTEM:CONSOLE")
endif()

add_test(NAME unit COMMAND torfox_unit_tests --gtest_brief=1)
