/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Stands in for TorFox's installer in tests/e2e/test_updates.py. It is served
 * by the fake GitHub as a release file. When TorFox starts it after shutting
 * down, it records in the file named by TORFOX_FAKE_INSTALLER_MARKER (UTF-8,
 * one item per line, written at once when it is done):
 *   1. its command line;
 *   2. its own path;
 *   3. "installing-mutex=1" if TorFoxUpdateInstalling existed when it started
 *      (TorFox holds it until the setup does);
 *   4. "parent-waited=1" if TorFox, which started it, was still running 1.5
 *      seconds later (it waits for the setup to say it holds the mutex);
 *   5. "started-event=1" if it could then say so, as the real setup does
 *      (TorFoxUpdateStarted, packaging/windows/TorFox.iss);
 *   6. "parent-exited=1" if TorFox then exited within 15 seconds.
 * It installs nothing. Test builds only. */

#include <windows.h>

#include <tlhelp32.h>

#include <stdio.h>
#include <wchar.h>

static wchar_t g_text[70000];

static void Append(const wchar_t* line) {
  wcsncat_s(g_text, 70000, line, _TRUNCATE);
  wcsncat_s(g_text, 70000, L"\n", _TRUNCATE);
}

static DWORD ParentId(void) {
  DWORD parent = 0;
  PROCESSENTRY32W entry;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return 0;
  }
  entry.dwSize = sizeof entry;
  if (Process32FirstW(snap, &entry)) {
    do {
      if (entry.th32ProcessID == GetCurrentProcessId()) {
        parent = entry.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return parent;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdline, int show) {
  wchar_t marker[32768];
  wchar_t temp[32800];
  wchar_t self[32768];
  HANDLE file;
  HANDLE mutex;
  HANDLE parent = NULL;
  HANDLE started;
  DWORD parent_id;
  int mutex_held;
  int parent_waited = 0;
  int event_set = 0;
  int parent_exited = 0;
  static char buf[200000];
  DWORD written = 0;
  int n8;
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
  mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"TorFoxUpdateInstalling");
  mutex_held = mutex != NULL || GetLastError() == ERROR_ACCESS_DENIED;
  if (mutex) {
    CloseHandle(mutex);
  }
  parent_id = ParentId();
  if (parent_id) {
    parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
  }
  Sleep(1500);
  if (parent) {
    parent_waited = WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
  }
  started = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"TorFoxUpdateStarted");
  if (started) {
    event_set = SetEvent(started) != 0;
    CloseHandle(started);
  }
  if (parent) {
    parent_exited = WaitForSingleObject(parent, 15000) == WAIT_OBJECT_0;
    CloseHandle(parent);
  }

  Append(GetCommandLineW());
  Append(self);
  Append(mutex_held ? L"installing-mutex=1" : L"installing-mutex=0");
  Append(parent_waited ? L"parent-waited=1" : L"parent-waited=0");
  Append(event_set ? L"started-event=1" : L"started-event=0");
  Append(parent_exited ? L"parent-exited=1" : L"parent-exited=0");
  n8 = WideCharToMultiByte(CP_UTF8, 0, g_text, -1, buf, (int)sizeof buf, NULL, NULL);
  if (n8 <= 1) {
    return 4;
  }
  /* Whole or not at all: the test reads the marker as soon as it appears. */
  if (swprintf(temp, 32800, L"%ls.tmp", marker) < 0) {
    return 5;
  }
  file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return 6;
  }
  if (!WriteFile(file, buf, (DWORD)(n8 - 1), &written, NULL) || written != (DWORD)(n8 - 1)) {
    CloseHandle(file);
    return 7;
  }
  CloseHandle(file);
  if (!MoveFileExW(temp, marker, MOVEFILE_REPLACE_EXISTING)) {
    return 8;
  }
  return 0;
}
