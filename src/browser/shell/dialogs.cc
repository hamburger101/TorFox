// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/shell/dialogs.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/browser/app/test_log.h"

namespace torfox::dialogs {

namespace {

std::string g_choice_file;

// Reads and deletes the choice file. nullopt if the hook is off.
std::optional<std::string> ConsumeChoice() {
  if (!kTestHooks || g_choice_file.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path file = platform::PathFromUtf8(g_choice_file);
  std::string choice;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      std::stringstream ss;
      ss << in.rdbuf();
      choice = ss.str();
    }
  }
  while (!choice.empty() && (choice.back() == '\n' || choice.back() == '\r' || choice.back() == ' ')) {
    choice.pop_back();
  }
  std::error_code ec;
  std::filesystem::remove(file, ec);
  return choice;
}

// One answer, posted to the UI thread at most once.
class Reply {
 public:
  explicit Reply(std::function<void(bool)> done) : done_(std::move(done)) {}

  void Post(bool ok) {
    if (!done_) {
      return;
    }
    std::function<void(bool)> f = std::move(done_);
    done_ = nullptr;
    CefPostTask(TID_UI, base::BindOnce([](std::function<void(bool)> g, bool v) { g(v); },
                                       std::move(f), ok));
  }

  bool answered() const { return !done_; }

 private:
  std::function<void(bool)> done_;
};

// The scripted prompt of test builds.
class TestPrompt : public platform::PromptHandle {
 public:
  TestPrompt(std::string kind, std::function<void(bool)> done)
      : kind_(std::move(kind)), reply_(std::move(done)) {}

  void Answer(bool ok) { reply_.Post(ok); }

  void Cancel() override {
    CEF_REQUIRE_UI_THREAD();
    if (!reply_.answered()) {
      test_log::Event("dialog_cancelled", {{"kind", kind_}});
      reply_.Post(false);
    }
  }

 private:
  std::string kind_;
  Reply reply_;
};

// Wraps a real prompt so that cancelling it always answers, even if the
// native dialog never reports back (for example when its window is gone).
class RealPrompt : public platform::PromptHandle {
 public:
  explicit RealPrompt(std::function<void(bool)> done)
      : reply_(std::make_shared<Reply>(std::move(done))) {}

  std::function<void(bool)> Callback() {
    std::weak_ptr<Reply> weak = reply_;
    return [weak](bool ok) {
      if (auto r = weak.lock()) {
        r->Post(ok);
      }
    };
  }

  void Attach(std::shared_ptr<platform::PromptHandle> native) { native_ = std::move(native); }

  void Cancel() override {
    CEF_REQUIRE_UI_THREAD();
    if (native_) {
      native_->Cancel();
    }
    reply_->Post(false);
  }

 private:
  std::shared_ptr<Reply> reply_;
  std::shared_ptr<platform::PromptHandle> native_;
};

}  // namespace

void ConfigureTestChoice(const std::string& path) {
  if (kTestHooks) {
    g_choice_file = path;
  }
}

std::shared_ptr<platform::PromptHandle> Confirm(CefRefPtr<CefWindow> window,
                                                const char* kind,
                                                const platform::ConfirmPrompt& prompt,
                                                std::function<void(bool)> done) {
  CEF_REQUIRE_UI_THREAD();
  if (std::optional<std::string> choice = ConsumeChoice()) {
    test_log::Event("dialog_shown", {{"kind", kind},
                                     {"title", prompt.title},
                                     {"heading", prompt.heading},
                                     {"details", prompt.details},
                                     {"warning", prompt.warning},
                                     {"accept", prompt.accept},
                                     {"cancel", prompt.cancel},
                                     {"choice", *choice}});
    auto test = std::make_shared<TestPrompt>(kind, std::move(done));
    if (choice->rfind("accept-after:", 0) == 0) {
      // Accepted later, as a user would: other prompts can be answered meanwhile.
      const int ms = std::clamp(std::atoi(choice->c_str() + 13), 0, 30000);
      CefPostDelayedTask(TID_UI, base::BindOnce([](std::shared_ptr<TestPrompt> t) { t->Answer(true); },
                                                test),
                         ms);
    } else if (*choice != "hold") {
      test->Answer(*choice == "accept");
    }
    return test;
  }
  auto real = std::make_shared<RealPrompt>(std::move(done));
  std::shared_ptr<platform::PromptHandle> native =
      platform::ShowConfirmPrompt(window, prompt, real->Callback());
  if (!native) {
    test_log::Event("dialog_failed", {{"kind", kind}});
    real->Cancel();  // Fail closed: no dialog means no.
    return real;
  }
  real->Attach(std::move(native));
  return real;
}

std::optional<std::vector<std::string>> TestFileChoice(const char* kind, int mode) {
  std::optional<std::string> choice = ConsumeChoice();
  if (!choice) {
    return std::nullopt;
  }
  test_log::Event("dialog_shown", {{"kind", kind}, {"mode", mode}, {"choice", *choice}});
  if (choice->rfind("file:", 0) == 0 && choice->size() > 5) {
    return std::vector<std::string>{choice->substr(5)};
  }
  return std::vector<std::string>{};
}

}  // namespace torfox::dialogs
