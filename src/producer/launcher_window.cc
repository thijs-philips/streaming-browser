#include "src/producer/launcher_window.h"

#include <string>

#include "src/common/logging.h"

namespace streaming::producer {
namespace {

constexpr wchar_t kWindowClass[] = L"StreamingBrowserProducerWindow";

}  // namespace

LauncherWindow::~LauncherWindow() {
  if (window_ != nullptr) {
    DestroyWindow(window_);
  }
}

bool LauncherWindow::Create(HINSTANCE instance,
                            CloseCallback close_callback,
                            VisibilityCallback visibility_callback,
                            void* callback_context,
                            bool initially_visible) {
  close_callback_ = close_callback;
  visibility_callback_ = visibility_callback;
  callback_context_ = callback_context;

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = &LauncherWindow::WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogLastError(LogLevel::kError, L"RegisterClassExW");
    return false;
  }

  window_ = CreateWindowExW(
      0, kWindowClass, L"Streaming Browser — Capture Spike",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 560, 180, nullptr, nullptr, instance, this);
  if (window_ == nullptr) {
    LogLastError(LogLevel::kError, L"CreateWindowExW");
    return false;
  }

  status_ = CreateWindowExW(
      0, L"STATIC",
      L"Initializing CEF accelerated off-screen rendering…",
      WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 24, 500, 72, window_, nullptr,
      instance, nullptr);
  visible_checkbox_ = CreateWindowW(
      L"BUTTON", L"Visible viewer", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      20, 108, 180, 24, window_, reinterpret_cast<HMENU>(
                                      static_cast<INT_PTR>(1001)),
      instance, nullptr);
  SendMessageW(visible_checkbox_, BM_SETCHECK,
               initially_visible ? BST_CHECKED : BST_UNCHECKED, 0);
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

int LauncherWindow::RunMessageLoop() {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.hwnd == nullptr && message.message == kShowLauncherMessage) {
      ShowWindow(window_, SW_RESTORE);
      SetForegroundWindow(window_);
      continue;
    }
    if (message.hwnd == nullptr && message.message == kCaptureFrameMessage) {
      const std::wstring frame = std::to_wstring(message.wParam);
      const std::wstring title =
          L"Streaming Browser — accelerated frame " + frame;
      SetWindowTextW(window_, title.c_str());
      const std::wstring status =
          L"Accelerated off-screen capture active. Frame " + frame +
          L" copied with callback-scoped GPU completion.";
      SetStatus(status.c_str());
      continue;
    }
    if (message.hwnd == nullptr && message.message == kCaptureFailureMessage) {
      SetWindowTextW(window_, L"Streaming Browser — accelerated capture failed");
      SetStatus(L"Accelerated CEF frame import/copy failed. See the debugger and CEF log.");
      continue;
    }
    if (message.hwnd == nullptr && message.message == kStreamFrameMessage) {
      const std::wstring frame = std::to_wstring(message.wParam);
      const std::wstring title =
          L"Streaming Browser — shared stream frame " + frame;
      SetWindowTextW(window_, title.c_str());
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  window_ = nullptr;
  return static_cast<int>(message.wParam);
}

void LauncherWindow::SetStatus(const wchar_t* status) {
  if (status_ != nullptr) {
    SetWindowTextW(status_, status);
  }
}

LRESULT CALLBACK LauncherWindow::WindowProc(HWND window,
                                             UINT message,
                                             WPARAM wparam,
                                             LPARAM lparam) {
  LauncherWindow* self = nullptr;
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<LauncherWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
    self->window_ = window;
  } else {
    self = reinterpret_cast<LauncherWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT LauncherWindow::HandleMessage(UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam) {
  switch (message) {
    case WM_CLOSE:
      if (!close_requested_) {
        close_requested_ = true;
        SetStatus(L"Stopping browser and CEF subprocesses…");
        EnableWindow(window_, FALSE);
        if (close_callback_ != nullptr) {
          close_callback_(callback_context_);
        } else {
          PostQuitMessage(0);
        }
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == 1001 && HIWORD(wparam) == BN_CLICKED &&
          visibility_callback_ != nullptr) {
        const bool visible =
            SendMessageW(visible_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        visibility_callback_(callback_context_, visible);
      }
      return 0;
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace streaming::producer
