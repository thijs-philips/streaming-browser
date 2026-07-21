#pragma once

#include <windows.h>

namespace streaming::producer {

inline constexpr UINT kShowLauncherMessage = WM_APP + 1;
inline constexpr UINT kCaptureFrameMessage = WM_APP + 2;
inline constexpr UINT kCaptureFailureMessage = WM_APP + 3;

class LauncherWindow final {
 public:
  using CloseCallback = void (*)(void* context);

  LauncherWindow() = default;
  ~LauncherWindow();

  bool Create(HINSTANCE instance,
              CloseCallback close_callback,
              void* close_context);
  int RunMessageLoop();
  void SetStatus(const wchar_t* status);

 private:
  static LRESULT CALLBACK WindowProc(HWND window,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  HWND window_ = nullptr;
  HWND status_ = nullptr;
  CloseCallback close_callback_ = nullptr;
  void* close_context_ = nullptr;
  bool close_requested_ = false;
};

}  // namespace streaming::producer
