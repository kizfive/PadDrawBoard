#include "pdb/app/config.h"
#include "pdb/app/desktop_server.h"
#include "pdb/app/tray_commands.h"
#include "pdb/video/monitor_enumerator.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;

struct TrayContext {
  std::unique_ptr<pdb::app::DesktopServer> server;
  NOTIFYICONDATAW icon{};
  HICON app_icon{};
  bool icon_added{};
  std::vector<pdb::video::MonitorInfo> menu_monitors;
};

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != nullptr) CloseHandle(handle_);
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_;
};

std::wstring InstanceMutexName() {
  DWORD session_id = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id) == FALSE) return {};
  return L"Local\\PadDrawBoard.Tray.Session." + std::to_wstring(session_id);
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) return L"（不可用）";
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
  return result;
}

std::wstring TrimForMenu(std::wstring value, std::size_t maximum = 100) {
  if (value.size() <= maximum) return value;
  value.resize(maximum - 1);
  value += L"...";
  return value;
}

TrayContext* Context(HWND window) {
  return reinterpret_cast<TrayContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void UpdateTooltip(TrayContext& context) {
  const pdb::app::ServerStatus status = context.server->Status();
  std::wstring tip = L"PadDrawBoard — " + pdb::app::ServerPhaseText(status.phase);
  tip = TrimForMenu(std::move(tip), _countof(context.icon.szTip) - 1);
  wcsncpy_s(context.icon.szTip, tip.c_str(), _TRUNCATE);
  context.icon.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &context.icon);
}

void ShowStatusMenu(HWND window, TrayContext& context) {
  const pdb::app::ServerStatus status = context.server->Status();
  const pdb::app::AppConfig config = context.server->CurrentConfig();
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;
  const std::wstring phase = L"状态：" + pdb::app::ServerPhaseText(status.phase);
  const std::wstring channels = L"连接：控制 " + std::wstring(status.control_connected ? L"正常" : L"断开") +
      L"｜画面 " + std::wstring(status.video_connected ? L"正常" : L"断开") +
      L"｜输入 " + std::wstring(status.input_connected ? L"正常" : L"断开");
  const std::wstring video = L"画面：已发送 " + std::to_wstring(status.video_frames_sent) +
      L" 帧｜重置 " + std::to_wstring(status.stream_resets) + L" 次";
  const std::wstring latency = L"延迟：连接 " + std::to_wstring(status.client_rtt_us / 1000) +
      L" 毫秒｜画面 " + std::to_wstring(status.client_video_latency_us / 1000) + L" 毫秒";
  const std::wstring injection = L"Windows Ink 输入：" +
      std::wstring(status.input_injection_available ? L"可用" : L"不可用");
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, phase.c_str());
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, channels.c_str());
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, video.c_str());
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, latency.c_str());
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, injection.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  context.menu_monitors.clear();
  (void)pdb::video::MonitorEnumerator::Enumerate(&context.menu_monitors);
  HMENU monitor_menu = CreatePopupMenu();
  if (monitor_menu != nullptr) {
    if (context.menu_monitors.empty()) {
      AppendMenuW(monitor_menu, MF_STRING | MF_GRAYED, 0, L"没有可捕获的显示器");
    } else {
      for (std::size_t index = 0; index < context.menu_monitors.size(); ++index) {
        const auto& monitor = context.menu_monitors[index];
        const int width = monitor.desktop_rect.right - monitor.desktop_rect.left;
        const int height = monitor.desktop_rect.bottom - monitor.desktop_rect.top;
        std::wstring label = monitor.device_name + L" (" + std::to_wstring(width) +
                              L"x" + std::to_wstring(height) + L")";
        const bool checked = config.selected_monitor_id == monitor.id.ToString();
        AppendMenuW(monitor_menu, MF_STRING | (checked ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(pdb::app::TrayMonitorCommand(index)), label.c_str());
      }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(monitor_menu), L"显示器");
  }

  HMENU bitrate_menu = CreatePopupMenu();
  if (bitrate_menu != nullptr) {
    for (const std::uint32_t bitrate : {20u, 40u, 80u, 120u}) {
      AppendMenuW(bitrate_menu,
                  MF_STRING | (config.bitrate_mbps == bitrate ? MF_CHECKED : 0),
                  static_cast<UINT_PTR>(pdb::app::TrayBitrateCommand(bitrate)),
                  (std::to_wstring(bitrate) + L" Mbps").c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bitrate_menu), L"画质（码率）");
  }
  AppendMenuW(menu, MF_STRING | (config.palm_guard_enabled ? MF_CHECKED : 0),
              pdb::app::kTrayTogglePalmCommand, L"笔尖接触时防误触");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU diagnostic_menu = CreatePopupMenu();
  if (diagnostic_menu != nullptr) {
    AppendMenuW(diagnostic_menu, MF_STRING, pdb::app::kTrayOpenConfigCommand, L"打开配置文件夹");
    AppendMenuW(diagnostic_menu, MF_STRING, pdb::app::kTrayOpenTelemetryCommand, L"打开运行日志");
    AppendMenuW(diagnostic_menu, MF_STRING, pdb::app::kTrayExportTelemetryCommand, L"导出运行日志…");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(diagnostic_menu), L"诊断工具");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, pdb::app::kTrayExitCommand, L"退出 PadDrawBoard");
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
  DestroyMenu(menu);
}

void OpenPath(const std::filesystem::path& path) {
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowExportDialog(HWND window, TrayContext& context) {
  wchar_t filename[MAX_PATH] = L"PadDrawBoard-telemetry.jsonl";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"JSON Lines 日志 (*.jsonl)\0*.jsonl\0所有文件 (*.*)\0*.*\0";
  dialog.lpstrFile = filename;
  dialog.nMaxFile = _countof(filename);
  dialog.lpstrDefExt = L"jsonl";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (GetSaveFileNameW(&dialog) == FALSE) return;
  std::string error;
  if (!context.server->ExportTelemetry(filename, &error)) {
    MessageBoxW(window, Utf8ToWide(error).c_str(), L"PadDrawBoard 日志导出失败",
                MB_OK | MB_ICONERROR);
  }
}

bool ApplyTrayConfig(HWND window, TrayContext& context, pdb::app::AppConfig config) {
  std::string error;
  if (context.server->Reconfigure(std::move(config), &error)) return true;
  MessageBoxW(window, Utf8ToWide(error).c_str(), L"PadDrawBoard 配置失败",
              MB_OK | MB_ICONERROR);
  return false;
}

void HandleTrayCommand(HWND window, TrayContext& context, int command) {
  const pdb::app::TrayCommand decoded = pdb::app::DecodeTrayCommand(command);
  switch (decoded.kind) {
    case pdb::app::TrayCommandKind::kExit:
      DestroyWindow(window);
      return;
    case pdb::app::TrayCommandKind::kOpenConfigFolder:
      OpenPath(context.server->ConfigPath().parent_path());
      return;
    case pdb::app::TrayCommandKind::kOpenTelemetry:
      OpenPath(context.server->TelemetryPath());
      return;
    case pdb::app::TrayCommandKind::kExportTelemetry:
      ShowExportDialog(window, context);
      return;
    case pdb::app::TrayCommandKind::kTogglePalmGuard: {
      pdb::app::AppConfig config = context.server->CurrentConfig();
      config.palm_guard_enabled = !config.palm_guard_enabled;
      (void)ApplyTrayConfig(window, context, std::move(config));
      return;
    }
    case pdb::app::TrayCommandKind::kBitrate:
      if (pdb::app::IsTrayBitratePreset(decoded.value)) {
        pdb::app::AppConfig config = context.server->CurrentConfig();
        config.bitrate_mbps = decoded.value;
        (void)ApplyTrayConfig(window, context, std::move(config));
      }
      return;
    case pdb::app::TrayCommandKind::kMonitor:
      if (decoded.value < context.menu_monitors.size()) {
        pdb::app::AppConfig config = context.server->CurrentConfig();
        config.selected_monitor_id = context.menu_monitors[decoded.value].id.ToString();
        (void)ApplyTrayConfig(window, context, std::move(config));
      }
      return;
    case pdb::app::TrayCommandKind::kNone:
      return;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  TrayContext* context = Context(window);
  switch (message) {
    case WM_CREATE:
      if (context != nullptr) {
        context->icon.cbSize = sizeof(context->icon);
        context->icon.hWnd = window;
        context->icon.uID = kTrayIconId;
        context->icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        context->icon.uCallbackMessage = kTrayCallbackMessage;
        context->icon.hIcon = context->app_icon;
        wcsncpy_s(context->icon.szTip, L"PadDrawBoard 正在启动", _TRUNCATE);
        context->icon_added = Shell_NotifyIconW(NIM_ADD, &context->icon) != FALSE;
        (void)context->server->Start();
        UpdateTooltip(*context);
      }
      return 0;
    case kTrayCallbackMessage:
      if (context != nullptr && (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU || lparam == WM_LBUTTONUP)) {
        UpdateTooltip(*context);
        ShowStatusMenu(window, *context);
      }
      return 0;
    case WM_COMMAND:
      if (context != nullptr) HandleTrayCommand(window, *context, LOWORD(wparam));
      return 0;
    case WM_DESTROY:
      if (context != nullptr) {
        context->server->Stop();
        if (context->icon_added) Shell_NotifyIconW(NIM_DELETE, &context->icon);
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

std::filesystem::path ModuleDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length == path.size()) return std::filesystem::current_path();
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const std::wstring mutex_name = InstanceMutexName();
  if (mutex_name.empty()) return 1;
  ScopedHandle instance_mutex(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
  if (instance_mutex.get() == nullptr) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  pdb::app::ConfigStore store;
  const pdb::app::ConfigLoadResult loaded = store.Load();
  if (!loaded.loaded_from_disk) {
    std::string ignored;
    (void)store.Save(loaded.config, &ignored);
  }
  pdb::app::ServerOptions options;
  options.config = loaded.config;
  options.config_store = store;
  options.executable_directory = ModuleDirectory();
  TrayContext context;
  context.server = std::make_unique<pdb::app::DesktopServer>(std::move(options));
  context.app_icon = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(IDI_PADDRAWBOARD), IMAGE_ICON, 0, 0,
      LR_DEFAULTSIZE | LR_SHARED));
  if (context.app_icon == nullptr) {
    context.app_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  }

  const wchar_t* class_name = L"PadDrawBoardTrayWindow";
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.hIcon = context.app_icon;
  window_class.hIconSm = context.app_icon;
  window_class.lpfnWndProc = WindowProc;
  window_class.lpszClassName = class_name;
  if (RegisterClassExW(&window_class) == 0) return 1;
  HWND window = CreateWindowExW(0, class_name, L"PadDrawBoard", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, instance, &context);
  if (window == nullptr) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
