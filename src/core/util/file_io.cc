// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/util/file_io.h"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#endif

namespace torfox {

namespace fs = std::filesystem;

namespace {

fs::path TempPathFor(const fs::path& path) {
  fs::path tmp = path;
  tmp += ".tmp";
  return tmp;
}

}  // namespace

#if defined(_WIN32)

ReadResult ReadFileLimited(const fs::path& path, std::size_t max_bytes) {
  ReadResult result;
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = ::GetLastError();
    result.status = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                        ? ReadStatus::kMissing
                        : ReadStatus::kError;
    return result;
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0) {
    ::CloseHandle(h);
    result.status = ReadStatus::kError;
    return result;
  }
  if (static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
    ::CloseHandle(h);
    result.status = ReadStatus::kTooLarge;
    return result;
  }
  result.data.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t total = 0;
  while (total < result.data.size()) {
    DWORD chunk = 0;
    DWORD want = static_cast<DWORD>(std::min<std::size_t>(result.data.size() - total, 1 << 20));
    if (!::ReadFile(h, result.data.data() + total, want, &chunk, nullptr)) {
      ::CloseHandle(h);
      result.data.clear();
      result.status = ReadStatus::kError;
      return result;
    }
    if (chunk == 0) {
      break;  // File shrank while reading.
    }
    total += chunk;
  }
  ::CloseHandle(h);
  result.data.resize(total);
  result.status = ReadStatus::kOk;
  return result;
}

bool RenameReplacing(const fs::path& from, const fs::path& to) {
  return ::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool WriteFileAtomically(const fs::path& path, std::string_view data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }
  const fs::path tmp = TempPathFor(path);
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::size_t total = 0;
  bool ok = true;
  while (total < data.size()) {
    DWORD written = 0;
    DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - total, 1 << 20));
    if (!::WriteFile(h, data.data() + total, want, &written, nullptr) || written == 0) {
      ok = false;
      break;
    }
    total += written;
  }
  if (ok && !::FlushFileBuffers(h)) {
    ok = false;
  }
  ::CloseHandle(h);
  if (ok) {
    ok = RenameReplacing(tmp, path);
  }
  if (!ok) {
    ::DeleteFileW(tmp.c_str());
  }
  return ok;
}

#else  // POSIX

ReadResult ReadFileLimited(const fs::path& path, std::size_t max_bytes) {
  ReadResult result;
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    result.status = (errno == ENOENT) ? ReadStatus::kMissing : ReadStatus::kError;
    return result;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 0) {
    ::close(fd);
    result.status = ReadStatus::kError;
    return result;
  }
  if (static_cast<unsigned long long>(st.st_size) > max_bytes) {
    ::close(fd);
    result.status = ReadStatus::kTooLarge;
    return result;
  }
  result.data.resize(static_cast<std::size_t>(st.st_size));
  std::size_t total = 0;
  while (total < result.data.size()) {
    ssize_t n = ::read(fd, result.data.data() + total, result.data.size() - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      result.data.clear();
      result.status = ReadStatus::kError;
      return result;
    }
    if (n == 0) {
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  ::close(fd);
  result.data.resize(total);
  result.status = ReadStatus::kOk;
  return result;
}

bool RenameReplacing(const fs::path& from, const fs::path& to) {
  return ::rename(from.c_str(), to.c_str()) == 0;
}

bool WriteFileAtomically(const fs::path& path, std::string_view data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }
  const fs::path tmp = TempPathFor(path);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t total = 0;
  bool ok = true;
  while (total < data.size()) {
    ssize_t n = ::write(fd, data.data() + total, data.size() - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  if (ok && ::fsync(fd) != 0) {
    ok = false;
  }
  if (::close(fd) != 0) {
    ok = false;
  }
  if (ok) {
    ok = RenameReplacing(tmp, path);
  }
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  // Persist the rename itself.
  if (path.has_parent_path()) {
    int dfd = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
  }
  return true;
}

#endif

}  // namespace torfox
