// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <thread>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "src/core/privacy/external_protocols.h"

namespace torfox::platform {

namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return std::wstring();
  }
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& s) {
  if (s.empty()) {
    return std::string();
  }
  int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

}  // namespace

std::filesystem::path DefaultUserDataDir() {
  PWSTR raw = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) ||
      !raw) {
    return {};
  }
  std::filesystem::path dir(raw);
  ::CoTaskMemFree(raw);
  return dir / L"TorFox";
}

CefRefPtr<CefCommandLine> RawCommandLine() {
  CefRefPtr<CefCommandLine> cl = CefCommandLine::CreateCommandLine();
  cl->InitFromString(::GetCommandLineW());
  return cl;
}

void ShowFatalError(const std::string& title, const std::string& message) {
  ::MessageBoxW(nullptr, Widen(message).c_str(), Widen(title).c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
}

void ApplyWindowTheme(CefRefPtr<CefWindow> window, bool dark) {
  if (!window) {
    return;
  }
  HWND hwnd = window->GetWindowHandle();
  if (!hwnd) {
    return;
  }
  // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+ and Windows 11).
  constexpr DWORD kUseImmersiveDarkMode = 20;
  BOOL value = dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof(value));
}

bool SystemPrefersDark() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  LSTATUS status = ::RegGetValueW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return status == ERROR_SUCCESS && value == 0;
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(Widen(utf8));
}

std::string PathToUtf8(const std::filesystem::path& path) {
  return Narrow(path.native());
}

bool CopyTextToClipboard(const std::string& utf8) {
  const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
  if (chars <= 0 && !utf8.empty()) {
    return false;
  }
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(chars) + 1) * sizeof(wchar_t));
  if (!memory) {
    return false;
  }
  wchar_t* text = static_cast<wchar_t*>(GlobalLock(memory));
  if (!text) {
    GlobalFree(memory);
    return false;
  }
  if (chars > 0) {
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        text, chars);
  }
  text[chars] = L'\0';
  GlobalUnlock(memory);
  if (!OpenClipboard(nullptr)) {
    GlobalFree(memory);
    return false;
  }
  EmptyClipboard();
  const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
  CloseClipboard();
  if (!ok) {
    GlobalFree(memory);  // Ownership passes to the clipboard only on success.
  }
  return ok;
}


std::filesystem::path DownloadsDir() {
  PWSTR raw = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) {
    return {};
  }
  std::filesystem::path dir(raw);
  ::CoTaskMemFree(raw);
  return dir;
}

bool RevealInFolder(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()) {
    return false;
  }
  // Shell calls can block (network folders, slow disks), so they run on their
  // own COM thread. Only the folder view is opened; the file is not run.
  std::thread([path]() {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
      return;
    }
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(path.c_str())) {
      ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
      ::ILFree(item);
    }
    ::CoUninitialize();
  }).detach();
  return true;
}

bool OpenExternal(const std::string& url) {
  if (external_protocols::LaunchableScheme(url).empty()) {
    return false;
  }
  // The shell finds the registered handler; it can block, so it runs on its
  // own COM thread.
  std::thread([wide = Widen(url)]() {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
      return;
    }
    ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ::CoUninitialize();
  }).detach();
  return true;
}

namespace {

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
constexpr int kAcceptButton = 100;

class WinPrompt : public PromptHandle {
 public:
  void Cancel() override {
    cancelled_ = true;
    if (HWND dialog = dialog_.load()) {
      ::PostMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
    }
  }

  static HRESULT CALLBACK Callback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data) {
    auto* self = reinterpret_cast<WinPrompt*>(data);
    if (msg == TDN_CREATED) {
      self->dialog_ = hwnd;
      if (self->cancelled_) {
        ::PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
      }
    } else if (msg == TDN_DESTROYED) {
      self->dialog_ = nullptr;
    }
    return S_OK;
  }

  bool cancelled() const { return cancelled_; }

 private:
  std::atomic<HWND> dialog_{nullptr};
  std::atomic<bool> cancelled_{false};
};

}  // namespace

std::shared_ptr<PromptHandle> ShowConfirmPrompt(CefRefPtr<CefWindow> window,
                                                const ConfirmPrompt& prompt,
                                                std::function<void(bool)> done) {
  HMODULE comctl = ::GetModuleHandleW(L"comctl32.dll");
  if (!comctl) {
    comctl = ::LoadLibraryW(L"comctl32.dll");
  }
  // TaskDialogIndirect exists only in Common Controls 6, which TorFox.exe's
  // manifest selects.
  auto task_dialog = comctl ? reinterpret_cast<TaskDialogIndirectFn>(
                                  ::GetProcAddress(comctl, "TaskDialogIndirect"))
                            : nullptr;
  if (!task_dialog || !window || !done) {
    return nullptr;
  }
  HWND owner = window->GetWindowHandle();
  auto handle = std::make_shared<WinPrompt>();
  // Held until the UI thread runs it; leaked (never run off the UI thread) if
  // TorFox is already shutting down and the task can't be posted.
  auto* reply = new std::function<void(bool)>(std::move(done));
  struct Text {
    std::wstring title, heading, details, warning, accept, cancel;
  } text{Widen(prompt.title),   Widen(prompt.heading), Widen(prompt.details),
         Widen(prompt.warning), Widen(prompt.accept),  Widen(prompt.cancel)};
  // A dialog thread of its own (as Chromium does for file dialogs): the
  // dialog's message loop never nests inside CEF's UI thread.
  std::thread([handle, owner, task_dialog, reply, text = std::move(text)]() {
    TASKDIALOG_BUTTON buttons[] = {{kAcceptButton, text.accept.c_str()},
                                   {IDCANCEL, text.cancel.c_str()}};
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = text.title.c_str();
    config.pszMainIcon = text.warning.empty() ? TD_INFORMATION_ICON : TD_WARNING_ICON;
    config.pszMainInstruction = text.heading.c_str();
    config.pszContent = text.details.c_str();
    if (!text.warning.empty()) {
      config.pszFooter = text.warning.c_str();
      config.pszFooterIcon = TD_WARNING_ICON;
    }
    config.pButtons = buttons;
    config.cButtons = 2;
    config.nDefaultButton = IDCANCEL;
    config.pfCallback = &WinPrompt::Callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(handle.get());
    int button = IDCANCEL;
    const HRESULT hr = task_dialog(&config, &button, nullptr, nullptr);
    const bool accepted = SUCCEEDED(hr) && button == kAcceptButton && !handle->cancelled();
    CefPostTask(TID_UI, base::BindOnce(
                            [](std::function<void(bool)>* f, bool ok) {
                              (*f)(ok);
                              delete f;
                            },
                            reply, accepted));
  }).detach();
  return handle;
}

}  // namespace torfox::platform
