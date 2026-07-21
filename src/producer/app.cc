#include "src/producer/app.h"

#include <utility>

#include "include/wrapper/cef_helpers.h"
#include "src/common/logging.h"
#include "src/producer/launcher_window.h"

namespace streaming::producer {

ProducerApp::ProducerApp(DWORD launcher_thread_id, ProducerConfig config)
    : launcher_thread_id_(launcher_thread_id), config_(std::move(config)) {}

void ProducerApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  if (closing_.load(std::memory_order_acquire)) {
    PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
    return;
  }

  CefRefPtr<BrowserClient> client =
      new BrowserClient(launcher_thread_id_, config_.force_transparency);
  {
    base::AutoLock lock(client_lock_);
    client_ = client;
  }

  CefWindowInfo window_info;
  window_info.SetAsWindowless(nullptr);
  window_info.shared_texture_enabled = true;
  window_info.external_begin_frame_enabled = false;

  CefBrowserSettings browser_settings;
  browser_settings.windowless_frame_rate = 30;
  browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);

  if (!CefBrowserHost::CreateBrowser(window_info, client, config_.url,
                                     browser_settings, nullptr, nullptr)) {
    Log(LogLevel::kError, L"CefBrowserHost::CreateBrowser rejected the request");
    PostThreadMessageW(launcher_thread_id_, WM_QUIT, 1, 0);
  }
}

bool ProducerApp::OnAlreadyRunningAppRelaunch(
    CefRefPtr<CefCommandLine>,
    const CefString&) {
  CEF_REQUIRE_UI_THREAD();
  Log(LogLevel::kWarning, L"A second producer launch was redirected to this instance");
  PostThreadMessageW(launcher_thread_id_, kShowLauncherMessage, 0, 0);
  return true;
}

void ProducerApp::CloseBrowser() {
  closing_.store(true, std::memory_order_release);
  CefRefPtr<BrowserClient> client;
  {
    base::AutoLock lock(client_lock_);
    client = client_;
  }
  if (client) {
    client->CloseBrowser();
  } else {
    PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
  }
}

}  // namespace streaming::producer
