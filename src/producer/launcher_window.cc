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
                            void* close_context) {
  close_callback_ = close_callback;
  close_context_ = close_context;

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
          L"Accelerated 3840×2160 capture active. Frame " + frame +
          L" copied with callback-scoped GPU completion.";
      SetStatus(status.c_str());
      continue;
    }
    if (message.hwnd == nullptr && message.message == kCaptureFailureMessage) {
      SetWindowTextW(window_, L"Streaming Browser — accelerated capture failed");
      SetStatus(L"Accelerated CEF frame import/copy failed. See the debugger and CEF log.");
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
          close_callback_(close_context_);
        } else {
          PostQuitMessage(0);
        }
      }
      return 0;
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace streaming::producer
