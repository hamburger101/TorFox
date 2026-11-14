// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/test_log.h"

#if defined(TORFOX_TEST_HOOKS)
#include <chrono>
#include <cstdio>
#include <mutex>
#include <utility>
#if defined(_WIN32)
#include <windows.h>

#include <csignal>
#include <cstdlib>
#endif
#endif

namespace torfox::test_log {

#if defined(TORFOX_TEST_HOOKS)

namespace {
std::mutex g_lock;
std::FILE* g_file = nullptr;
}  // namespace

void Open(const std::string& path) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_file || path.empty()) {
    return;
  }
#if defined(_WIN32)
  // Paths arrive as UTF-8.
  std::wstring wide;
  {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    wide.resize(static_cast<size_t>(n));
    ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), n);
  }
  g_file = ::_wfopen(wide.c_str(), L"ab");
#else
  g_file = std::fopen(path.c_str(), "ab");
#endif
}

#if defined(_WIN32)
namespace {

// Writes the current call stack as "module+0xoffset" entries. Runs inside a
// crash handler, so it avoids the JSON library and the log mutex.
void WriteStack(const char* why) {
  if (!g_file) {
    return;
  }
  void* frames[48];
  const USHORT count = ::CaptureStackBackTrace(1, 48, frames, nullptr);
  std::fprintf(g_file, "{\"ev\":\"crash_trace\",\"why\":\"%s\",\"frames\":[", why);
  for (USHORT i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCSTR>(frames[i]), &module)) {
      char path[MAX_PATH];
      const DWORD n = ::GetModuleFileNameA(module, path, MAX_PATH);
      const char* base = path;
      for (DWORD j = 0; j < n; ++j) {
        if (path[j] == '\\' || path[j] == '/') {
          base = path + j + 1;
        }
      }
      std::snprintf(name, sizeof(name), "%s", n ? base : "?");
    }
    const auto offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
    std::fprintf(g_file, "%s\"%s+0x%llx\"", i ? "," : "", name,
                 static_cast<unsigned long long>(offset));
  }
  std::fprintf(g_file, "]}\n");
  std::fflush(g_file);
}

void OnAbort(int) {
  WriteStack("abort");
}

void OnPureCall() {
  WriteStack("purecall");
  std::abort();
}

}  // namespace
#endif

void InstallCrashTrace() {
#if defined(_WIN32)
  std::signal(SIGABRT, OnAbort);
  ::_set_purecall_handler(OnPureCall);
#endif
}

void Close() {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_file) {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

bool Enabled() {
  std::lock_guard<std::mutex> guard(g_lock);
  return g_file != nullptr;
}

void Event(std::string_view name, Json fields) {
  if (!fields.is_object()) {
    fields = Json::object();
  }
  fields["ev"] = std::string(name);
  fields["t_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::string line = DumpJson(fields) + "\n";
  std::lock_guard<std::mutex> guard(g_lock);
  if (!g_file) {
    return;
  }
  std::fwrite(line.data(), 1, line.size(), g_file);
  std::fflush(g_file);
}

#else  // !TORFOX_TEST_HOOKS

void Open(const std::string&) {}
void Close() {}
bool Enabled() {
  return false;
}
void Event(std::string_view, Json) {}
void InstallCrashTrace() {}

#endif

}  // namespace torfox::test_log
