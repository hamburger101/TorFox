// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/platform/platform.h"

// winsock2.h before windows.h, which would otherwise bring in winsock 1.
#include <winsock2.h>
#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <atomic>
#include <map>
#include <set>
#include <thread>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "src/core/privacy/external_protocols.h"
#include "src/core/update/sha256.h"

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

// The path of the file open as |h|, with every junction and link resolved,
// as a normal drive or UNC path; "" if Windows cannot say.
std::wstring RealPath(HANDLE h) {
  std::wstring buf(1024, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n > 32767) {
      return std::wstring();
    }
    if (n < buf.size()) {
      buf.resize(n);
      constexpr std::wstring_view kUnc = L"\\\\?\\UNC\\";
      constexpr std::wstring_view kLocal = L"\\\\?\\";
      if (buf.rfind(kUnc, 0) == 0) {
        return L"\\\\" + buf.substr(kUnc.size());
      }
      if (buf.rfind(kLocal, 0) == 0) {
        return buf.substr(kLocal.size());
      }
      return std::wstring();  // Not a path ShellExecute understands.
    }
    buf.assign(n, L'\0');
  }
  return std::wstring();
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


// ----- Tor --------------------------------------------------------------------

namespace {

// One argument quoted the way CommandLineToArgvW (and the C runtime) read
// it back: backslashes are literal except before a quote.
void AppendQuoted(std::wstring& out, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    out += arg;
    return;
  }
  out.push_back(L'"');
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(arg[i]);
  }
  out.push_back(L'"');
}

class WinChildProcess : public ChildProcess {
 public:
  WinChildProcess(HANDLE process, HANDLE job, DWORD pid) : process_(process), job_(job), pid_(pid) {}
  ~WinChildProcess() override {
    Kill();
    ::CloseHandle(process_);
    ::CloseHandle(job_);  // Kill-on-close: anything tor started ends too.
  }
  long pid() const override { return static_cast<long>(pid_); }
  bool Exited(int* exit_code) override {
    if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
      return false;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process_, &code);
    if (exit_code) {
      *exit_code = static_cast<int>(code);
    }
    return true;
  }
  bool WaitForExit(int ms) override {
    return ::WaitForSingleObject(process_, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
  }
  void Kill() override {
    if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
      ::TerminateJobObject(job_, 1);
      ::WaitForSingleObject(process_, 5000);
    }
  }

 private:
  HANDLE process_;
  HANDLE job_;
  DWORD pid_;
};

std::filesystem::path ExecutableDir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) {
      return {};
    }
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  return std::filesystem::path(buf).parent_path();
}

}  // namespace

std::unique_ptr<ChildProcess> LaunchHidden(const std::filesystem::path& exe,
                                           const std::vector<std::string>& args,
                                           const std::filesystem::path& working_dir,
                                           std::string* error) {
  auto fail = [&](const char* why) -> std::unique_ptr<ChildProcess> {
    if (error) {
      *error = why;
    }
    return nullptr;
  };
  std::wstring command;
  AppendQuoted(command, exe.native());
  for (const std::string& a : args) {
    command.push_back(L' ');
    AppendQuoted(command, Widen(a));
  }
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    return fail("could not create a job object");
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    ::CloseHandle(job);
    return fail("could not configure the job object");
  }
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  // Suspended until it is in the job, so it cannot start anything outside it.
  if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        working_dir.c_str(), &si, &pi)) {
    ::CloseHandle(job);
    return fail("tor could not be started");
  }
  if (!::AssignProcessToJobObject(job, pi.hProcess)) {
    ::TerminateProcess(pi.hProcess, 1);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);
    return fail("tor could not be tied to TorFox's lifetime");
  }
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  return std::make_unique<WinChildProcess>(pi.hProcess, job, pi.dwProcessId);
}

void MarkRunning() {
  // Kept until the process exits; Windows releases it then, crash or not.
  static HANDLE running = ::CreateMutexW(nullptr, FALSE, L"TorFoxBrowserRunning");
  (void)running;
}

const char* UpdatePlatform() {
  return "windows-x64";
}

namespace {

// Shared with packaging/windows/TorFox.iss.
constexpr wchar_t kUpdateInstallingMutex[] = L"TorFoxUpdateInstalling";
// Set by the setup once it holds kUpdateInstallingMutex (InitializeSetup).
constexpr wchar_t kUpdateStartedEvent[] = L"TorFoxUpdateStarted";
constexpr wchar_t kTorFoxKey[] = L"Software\\TorFox";
constexpr wchar_t kStartAfterUpdateValue[] = L"RelaunchAfterUpdate";
// Well under the minute the setup waits for TorFox to exit.
constexpr DWORD kSetupStartWaitMs = 20000;

HANDLE g_update_installing = nullptr;

}  // namespace

bool LaunchUpdate(const std::filesystem::path& file, const std::string& sha256, uint64_t size,
                  bool relaunch, std::string* error) {
  // Read access, shared for reading only: nobody can write, rename or delete
  // the file while TorFox holds this handle, so what runs is what was hashed.
  HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *error = "the update could not be opened";
    return false;
  }
  update::Sha256 hash;
  uint64_t total = 0;
  std::vector<char> buf(1 << 16);
  bool read_ok = true;
  for (;;) {
    DWORD n = 0;
    if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
      read_ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    total += n;
    if (total > size) {
      break;
    }
    hash.Update(std::string_view(buf.data(), n));
  }
  if (!read_ok || total != size || hash.FinalHex() != sha256) {
    ::CloseHandle(h);
    *error = "the update changed after it was checked";
    return false;
  }
  // Start the file by its real path, as Windows resolved it for this handle,
  // never by |file|: a junction or link anywhere in |file| could be pointed
  // elsewhere after the check. The real path has none, and while TorFox
  // holds the handle Windows refuses to rename or delete any folder on it
  // (docs/STAGE9_FINDINGS.md §3.1).
  const std::wstring real = RealPath(h);
  if (real.empty()) {
    ::CloseHandle(h);
    *error = "the update's location could not be checked";
    return false;
  }
  // The setup installs for this user only, so it runs without a UAC prompt,
  // and waits for TorFox to exit (MarkRunning). After "Relaunch to update" it
  // shows its progress and starts TorFox again; when TorFox was simply
  // closed with an update waiting, it shows nothing and does not.
  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = real.c_str();
  info.lpParameters =
      relaunch ? L"/SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE"
               : L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE /TORFOXNORELAUNCH";
  info.nShow = SW_SHOWNORMAL;
  // TorFox holds the setup's mutex (HoldUpdateInstalling) until the setup
  // says it holds it too, so that no TorFox can start in between. Created
  // before the setup starts, unset.
  HANDLE setup_started = ::CreateEventW(nullptr, TRUE, FALSE, kUpdateStartedEvent);
  if (setup_started) {
    ::ResetEvent(setup_started);
  }
  const bool started = ::ShellExecuteExW(&info) != FALSE;
  ::CloseHandle(h);
  if (started && setup_started && info.hProcess) {
    // Until the setup is ready, or it has exited (it failed), or long enough.
    HANDLE wait[2] = {setup_started, info.hProcess};
    ::WaitForMultipleObjects(2, wait, FALSE, kSetupStartWaitMs);
  }
  if (info.hProcess) {
    ::CloseHandle(info.hProcess);
  }
  if (setup_started) {
    ::CloseHandle(setup_started);
  }
  if (!started) {
    *error = "the installer could not be started";
    return false;
  }
  return true;
}

bool InstallsUpdatesOnExit() {
  return true;
}

bool SessionEnding() {
  return ::GetSystemMetrics(SM_SHUTTINGDOWN) != 0;
}

bool UpdateBeingInstalled() {
  HANDLE h = ::OpenMutexW(SYNCHRONIZE, FALSE, kUpdateInstallingMutex);
  if (h) {
    ::CloseHandle(h);
    return true;
  }
  // It exists, but this process may not open it: still being installed.
  return ::GetLastError() == ERROR_ACCESS_DENIED;
}

bool RequestStartAfterUpdate() {
  const DWORD one = 1;
  return ::RegSetKeyValueW(HKEY_CURRENT_USER, kTorFoxKey, kStartAfterUpdateValue, REG_DWORD,
                           &one, sizeof one) == ERROR_SUCCESS;
}

void ClearStartAfterUpdate() {
  ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kTorFoxKey, kStartAfterUpdateValue);
}

void HoldUpdateInstalling() {
  if (!g_update_installing) {
    g_update_installing = ::CreateMutexW(nullptr, FALSE, kUpdateInstallingMutex);
  }
}

void ReleaseUpdateInstalling() {
  if (g_update_installing) {
    ::CloseHandle(g_update_installing);
    g_update_installing = nullptr;
  }
}

long CurrentProcessId() {
  return static_cast<long>(::GetCurrentProcessId());
}

namespace {

uint64_t CreationTime(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return 0;
  }
  return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

std::wstring ImagePath(HANDLE process) {
  std::wstring buf(32768, L'\0');
  DWORD n = static_cast<DWORD>(buf.size());
  if (!::QueryFullProcessImageNameW(process, 0, buf.data(), &n)) {
    return {};
  }
  buf.resize(n);
  return buf;
}

// |path| is inside |folder| (compared as Windows compares file names).
bool InFolder(const std::wstring& path, const std::wstring& folder) {
  if (folder.empty() || path.size() <= folder.size() + 1 || path[folder.size()] != L'\\') {
    return false;
  }
  return ::CompareStringOrdinal(path.c_str(), static_cast<int>(folder.size()), folder.c_str(),
                                static_cast<int>(folder.size()), TRUE) == CSTR_EQUAL;
}

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809 and later), which the SDK
// declares only for NTDDI_WIN10_CU and later targets.
struct MemoryCountersEx2 {
  DWORD cb;
  DWORD PageFaultCount;
  SIZE_T PeakWorkingSetSize;
  SIZE_T WorkingSetSize;
  SIZE_T QuotaPeakPagedPoolUsage;
  SIZE_T QuotaPagedPoolUsage;
  SIZE_T QuotaPeakNonPagedPoolUsage;
  SIZE_T QuotaNonPagedPoolUsage;
  SIZE_T PagefileUsage;
  SIZE_T PeakPagefileUsage;
  SIZE_T PrivateUsage;
  SIZE_T PrivateWorkingSetSize;
  ULONG64 SharedCommitUsage;
};

// The private working set: what Task Manager's Memory column shows. Older
// Windows 10 releases only know the private bytes committed.
uint64_t PrivateBytes(HANDLE process) {
  MemoryCountersEx2 counters = {};
  counters.cb = sizeof counters;
  if (::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof counters)) {
    return counters.PrivateWorkingSetSize;
  }
  PROCESS_MEMORY_COUNTERS_EX older = {};
  older.cb = sizeof older;
  if (::GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&older),
                             sizeof older)) {
    return older.PrivateUsage;
  }
  return 0;
}

}  // namespace

int ReservedLoopbackPort() {
  static const int port = [] {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return 0;
    }
    // Kept open until TorFox exits; Windows closes it then.
    const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
      return 0;
    }
    BOOL exclusive = TRUE;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Any free port.
    int len = sizeof(addr);
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) != 0 ||
        bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        listen(s, SOMAXCONN) != 0) {
      closesocket(s);
      return 0;
    }
    // Each connection is reset as soon as it arrives (SO_LINGER 0), before
    // anything is read from it.
    std::thread([s] {
      for (;;) {
        const SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
          const int e = WSAGetLastError();
          if (e == WSAENOTSOCK || e == WSAEINVAL || e == WSANOTINITIALISED) {
            return;
          }
          Sleep(50);
          continue;
        }
        linger reset = {1, 0};
        setsockopt(c, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&reset), sizeof(reset));
        closesocket(c);
      }
    }).detach();
    return static_cast<int>(ntohs(addr.sin_port));
  }();
  return port;
}

std::vector<memory::Process> TorFoxProcesses(const std::filesystem::path& tor_dir) {
  std::vector<memory::Process> out;
  const std::wstring program = ExecutableDir().native();
  const std::wstring tor = tor_dir.native();
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (program.empty() || snapshot == INVALID_HANDLE_VALUE) {
    if (snapshot != INVALID_HANDLE_VALUE) {
      ::CloseHandle(snapshot);
    }
    return out;
  }
  std::multimap<DWORD, DWORD> children;
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof entry;
  for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok; ok = ::Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID != 0 && entry.th32ProcessID != entry.th32ParentProcessID) {
      children.emplace(entry.th32ParentProcessID, entry.th32ProcessID);
    }
  }
  ::CloseHandle(snapshot);

  const DWORD self = ::GetCurrentProcessId();
  out.push_back({static_cast<long>(self), memory::Process::Kind::kBrowser,
                 PrivateBytes(::GetCurrentProcess())});
  struct Pending {
    DWORD pid;
    uint64_t parent_created;
  };
  std::vector<Pending> todo;
  const uint64_t self_created = CreationTime(::GetCurrentProcess());
  for (auto [it, end] = children.equal_range(self); it != end; ++it) {
    todo.push_back({it->second, self_created});
  }
  std::set<DWORD> seen = {self};
  while (!todo.empty() && out.size() < 1024) {
    const Pending p = todo.back();
    todo.pop_back();
    if (!seen.insert(p.pid).second) {
      continue;
    }
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (!h) {
      continue;
    }
    // A process whose parent has exited still names its parent's ID, which
    // Windows may since have given to another process: only a process
    // started after its parent can be its child.
    const uint64_t created = CreationTime(h);
    const std::wstring image = ImagePath(h);
    memory::Process::Kind kind = memory::Process::Kind::kHelper;
    bool ours = created != 0 && created >= p.parent_created;
    if (ours && InFolder(image, tor)) {
      kind = memory::Process::Kind::kTor;
    } else if (!ours || !InFolder(image, program)) {
      ours = false;  // Not TorFox's: for example an app opened for a link.
    }
    if (ours) {
      out.push_back({static_cast<long>(p.pid), kind, PrivateBytes(h)});
      for (auto [it, end] = children.equal_range(p.pid); it != end; ++it) {
        todo.push_back({it->second, created});
      }
    }
    ::CloseHandle(h);
  }
  return out;
}

TorPaths BundledTorPaths() {
  TorPaths p;
  p.dir = ExecutableDir() / L"tor";
  p.exe = p.dir / L"tor.exe";
  p.pt_client = "pluggable_transports\\lyrebird.exe";
  p.geoip = p.dir / L"data" / L"geoip";
  p.geoip6 = p.dir / L"data" / L"geoip6";
  p.pt_config = p.dir / L"pluggable_transports" / L"pt_config.json";
  return p;
}

std::filesystem::path AsciiPath(const std::filesystem::path& path) {
  const std::wstring& w = path.native();
  bool ascii = true;
  for (wchar_t c : w) {
    ascii = ascii && c < 0x7F && c >= 0x20;
  }
  if (ascii) {
    return path;
  }
  const DWORD n = ::GetShortPathNameW(w.c_str(), nullptr, 0);
  if (n == 0) {
    return {};
  }
  std::wstring out(n, L'\0');
  const DWORD m = ::GetShortPathNameW(w.c_str(), out.data(), n);
  if (m == 0 || m >= n) {
    return {};
  }
  out.resize(m);
  for (wchar_t c : out) {
    if (c >= 0x7F || c < 0x20) {
      return {};  // 8.3 names are off on this volume.
    }
  }
  return std::filesystem::path(out);
}

}  // namespace torfox::platform
