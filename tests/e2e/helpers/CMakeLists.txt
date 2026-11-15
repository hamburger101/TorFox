# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Helpers for the end-to-end tests (test builds on Windows only).

# Stands in for the installer in test_updates.py.
add_executable(torfox_fake_installer WIN32 fake_installer.c)
set_property(TARGET torfox_fake_installer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
