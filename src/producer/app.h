#pragma once

#include <windows.h>

#include <atomic>
#include <string>

#include "include/base/cef_lock.h"
#include "include/cef_app.h"
#include "src/common/configuration.h"
#include "src/producer/browser_client.h"

namespace streaming::producer {

class ProducerApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  ProducerApp(DWORD launcher_thread_id, ProducerConfiguration config);

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnContextInitialized() override;
  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override;

  void CloseBrowser();
  void SetViewerVisible(bool visible);
  void SetParentWindow(HWND window) { parent_window_ = window; }

 private:
  DWORD launcher_thread_id_ = 0;
  ProducerConfiguration config_;
  std::atomic_bool closing_ = false;
  HWND parent_window_ = nullptr;
  base::Lock client_lock_;
  CefRefPtr<BrowserClient> client_;

  IMPLEMENT_REFCOUNTING(ProducerApp);
  DISALLOW_COPY_AND_ASSIGN(ProducerApp);
};

}  // namespace streaming::producer
