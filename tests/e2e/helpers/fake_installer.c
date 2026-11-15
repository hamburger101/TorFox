/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Stands in for TorFox's installer in tests/e2e/test_updates.py. It is served
 * by the fake GitHub as a release file; when TorFox starts it after shutting
 * down, it records how it was started (its command line and its own path,
 * UTF-8, one per line) in the file named by TORFOX_FAKE_INSTALLER_MARKER, and
 * exits. It installs nothing. Test builds only. */

#include <windows.h>

#include <stdio.h>

static int WriteUtf8(HANDLE file, const wchar_t* text) {
  char buf[65536];
  DWORD written = 0;
  int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, (int)sizeof buf, NULL, NULL);
  if (n <= 1) {
    return 0;
  }
  return WriteFile(file, buf, (DWORD)(n - 1), &written, NULL) && WriteFile(file, "\n", 1, &written, NULL);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdline, int show) {
  wchar_t marker[32768];
  wchar_t self[32768];
  HANDLE file;
  DWORD n = GetEnvironmentVariableW(L"TORFOX_FAKE_INSTALLER_MARKER", marker, 32768);
  (void)instance;
  (void)prev;
  (void)cmdline;
  (void)show;
  if (n == 0 || n >= 32768) {
    return 2;
  }
  n = GetModuleFileNameW(NULL, self, 32768);
  if (n == 0 || n >= 32768) {
    return 3;
  }
  file = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return 4;
  }
  if (!WriteUtf8(file, GetCommandLineW()) || !WriteUtf8(file, self)) {
    CloseHandle(file);
    return 5;
  }
  CloseHandle(file);
  return 0;
}
