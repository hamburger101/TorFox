# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# torfox_verified_dep(<out_var> <kind> <key>)
#
# Sets <out_var> to the directory of a dependency that scripts/fetch_deps.py
# has verified against deps.lock.json. Configuration fails if the dependency
# is missing or was verified against a different SHA-256.

function(torfox_verified_dep out_var kind key)
  file(READ "${CMAKE_SOURCE_DIR}/deps.lock.json" _lock)
  string(JSON _name ERROR_VARIABLE _err GET "${_lock}" ${kind} files ${key} name)
  if(_err)
    message(FATAL_ERROR "deps.lock.json has no ${kind} artifact '${key}'.")
  endif()
  string(JSON _sha GET "${_lock}" ${kind} files ${key} sha256)
  string(JSON _install ERROR_VARIABLE _ierr GET "${_lock}" ${kind} files ${key} install_as)
  if(_ierr OR "${_install}" STREQUAL "")
    string(REGEX REPLACE "\\.(tar\\.(bz2|gz|xz)|tgz)$" "" _root "${_name}")
  else()
    string(REGEX REPLACE "/.*$" "" _root "${_install}")
  endif()

  set(_dir "${TORFOX_DEPS_DIR}/${kind}/${_root}")
  set(_marker "${TORFOX_DEPS_DIR}/${kind}/${_name}.verified")
  if(NOT EXISTS "${_dir}" OR NOT EXISTS "${_marker}")
    message(FATAL_ERROR
      "Verified dependency ${kind}/${key} not found at ${_dir}.\n"
      "Run: python scripts/fetch_deps.py ${kind} ${key}")
  endif()
  file(READ "${_marker}" _marker_sha)
  string(STRIP "${_marker_sha}" _marker_sha)
  if(NOT _marker_sha STREQUAL _sha)
    message(FATAL_ERROR
      "${kind}/${key} at ${_dir} was verified against a different SHA-256 than deps.lock.json pins.\n"
      "Re-run: python scripts/fetch_deps.py ${kind} ${key}")
  endif()
  set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()

# Applies CEF's compiler settings (runtime library, no exceptions, no RTTI,
# C++20) to a third-party target without CEF's warnings-as-errors. Covers C
# sources too (SQLite), so every object uses the same runtime library.
function(torfox_third_party_flags target)
  set(_flags ${CEF_COMPILER_FLAGS})
  list(REMOVE_ITEM _flags /WX /W4 -Werror -Wall -Wextra)
  set(_cflags ${CEF_C_COMPILER_FLAGS})
  list(REMOVE_ITEM _cflags /WX /W4 -Werror -Wall -Wextra)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${_flags} ${_cflags}>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Debug>>:${CEF_COMPILER_FLAGS_DEBUG} ${CEF_C_COMPILER_FLAGS_DEBUG}>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Release>>:${CEF_COMPILER_FLAGS_RELEASE} ${CEF_C_COMPILER_FLAGS_RELEASE}>
    $<$<COMPILE_LANGUAGE:CXX>:${_flags} ${CEF_CXX_COMPILER_FLAGS}>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:${CEF_COMPILER_FLAGS_DEBUG} ${CEF_CXX_COMPILER_FLAGS_DEBUG}>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:${CEF_COMPILER_FLAGS_RELEASE} ${CEF_CXX_COMPILER_FLAGS_RELEASE}>)
  target_compile_definitions(${target} PRIVATE ${CEF_COMPILER_DEFINES}
    $<$<CONFIG:Debug>:${CEF_COMPILER_DEFINES_DEBUG}>
    $<$<CONFIG:Release>:${CEF_COMPILER_DEFINES_RELEASE}>)
  if(OS_MAC)
    set_target_properties(${target} PROPERTIES
      OSX_ARCHITECTURES_DEBUG "${CMAKE_OSX_ARCHITECTURES}"
      OSX_ARCHITECTURES_RELEASE "${CMAKE_OSX_ARCHITECTURES}")
  endif()
endfunction()
