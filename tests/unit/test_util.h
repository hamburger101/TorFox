// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_TESTS_UNIT_TEST_UTIL_H_
#define TORFOX_TESTS_UNIT_TEST_UTIL_H_

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace torfox::testing {

// A fresh directory under the system temp dir, removed on destruction.
class ScopedTempDir {
 public:
  ScopedTempDir() {
    std::random_device rd;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) /
            ("torfox-unit-" + std::to_string(now) + "-" + std::to_string(rd()));
    std::filesystem::create_directories(path_, ec);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace torfox::testing

#endif  // TORFOX_TESTS_UNIT_TEST_UTIL_H_
