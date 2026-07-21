#pragma once

#include <windows.h>

#include <atomic>
#include <string>

#include "include/base/cef_lock.h"
#include "include/cef_app.h"
#include "src/producer/browser_client.h"

namespace streaming::producer {

struct ProducerConfig {
  std::string url = "https://example.com";
  bool force_transparency = false;
};

class ProducerApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  ProducerApp(DWORD launcher_thread_id, ProducerConfig config);

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnContextInitialized() override;
  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override;

  void CloseBrowser();

 private:
  DWORD launcher_thread_id_ = 0;
  ProducerConfig config_;
  std::atomic_bool closing_ = false;
  base::Lock client_lock_;
  CefRefPtr<BrowserClient> client_;

  IMPLEMENT_REFCOUNTING(ProducerApp);
  DISALLOW_COPY_AND_ASSIGN(ProducerApp);
};

}  // namespace streaming::producer
