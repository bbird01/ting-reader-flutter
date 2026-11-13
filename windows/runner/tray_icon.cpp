#include "tray_icon.h"

#include <shellapi.h>

#include <cwchar>

#include "resource.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr int kShowCommand = 1;
constexpr int kExitCommand = 2;
constexpr wchar_t kTooltip[] = L"Ting Reader";
}  // namespace

TrayIcon::TrayIcon(HWND window, Action on_show, Action on_exit)
    : window_(window),
      on_show_(std::move(on_show)),
      on_exit_(std::move(on_exit)),
      taskbar_created_(RegisterWindowMessageW(L"TaskbarCreated")) {}

TrayIcon::~TrayIcon() { Hide(); }

bool TrayIcon::Show() {
  if (visible_) return true;
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = kTrayIconId;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  data.hIcon =
      LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
  if (!data.hIcon) return false;
  wcscpy_s(data.szTip, kTooltip);
  if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
  visible_ = true;
  return true;
}

void TrayIcon::Hide() {
  if (!visible_) return;
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &data);
  visible_ = false;
}

bool TrayIcon::HandleMessage(UINT message, LPARAM lparam) {
  // explorer.exe 重启会清空通知区域，此时必须重新添加图标。
  if (taskbar_created_ != 0 && message == taskbar_created_) {
    if (visible_) {
      visible_ = false;
      Show();
    }
    return true;
  }
  if (message != kTrayCallbackMessage) return false;
  switch (lparam) {
    case WM_LBUTTONDBLCLK:
      if (on_show_) on_show_();
      return true;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
      ShowContextMenu();
      return true;
    default:
      return false;
  }
}

void TrayIcon::ShowContextMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, kShowCommand, L"显示主界面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExitCommand, L"退出软件");
  POINT cursor;
  GetCursorPos(&cursor);
  // 拥有者不在前台时，点击菜单外部不会关闭菜单。
  SetForegroundWindow(window_);
  const int command =
      TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x, cursor.y,
                     0, window_, nullptr);
  DestroyMenu(menu);
  if (command == kShowCommand && on_show_) on_show_();
  if (command == kExitCommand && on_exit_) on_exit_();
}