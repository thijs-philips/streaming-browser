#include "src/producer/browser_client.h"

#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_menu_model.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/common/logging.h"
#include "src/producer/launcher_window.h"

namespace streaming::producer {
namespace {

constexpr int kViewWidth = 3840;
constexpr int kViewHeight = 2160;

}  // namespace

BrowserClient::BrowserClient(DWORD launcher_thread_id, bool force_transparency)
    : launcher_thread_id_(launcher_thread_id),
      force_transparency_(force_transparency),
      pipeline_(std::make_unique<D3DFramePipeline>()) {}

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  rect = CefRect(0, 0, kViewWidth, kViewHeight);
}

bool BrowserClient::GetRootScreenRect(CefRefPtr<CefBrowser>, CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  rect = CefRect(0, 0, kViewWidth, kViewHeight);
  return true;
}

bool BrowserClient::GetScreenPoint(CefRefPtr<CefBrowser>,
                                   int view_x,
                                   int view_y,
                                   int& screen_x,
                                   int& screen_y) {
  CEF_REQUIRE_UI_THREAD();
  screen_x = view_x;
  screen_y = view_y;
  return true;
}

bool BrowserClient::GetScreenInfo(CefRefPtr<CefBrowser>,
                                  CefScreenInfo& screen_info) {
  CEF_REQUIRE_UI_THREAD();
  screen_info.device_scale_factor = 1.0F;
  screen_info.depth = 32;
  screen_info.depth_per_component = 8;
  screen_info.is_monochrome = false;
  screen_info.rect = CefRect(0, 0, kViewWidth, kViewHeight);
  screen_info.available_rect = screen_info.rect;
  return true;
}

void BrowserClient::OnPaint(CefRefPtr<CefBrowser>,
                            PaintElementType,
                            const RectList&,
                            const void*,
                            int,
                            int) {
  CEF_REQUIRE_UI_THREAD();
  if (!software_fallback_reported_) {
    software_fallback_reported_ = true;
    Log(LogLevel::kError,
        L"CEF invoked software OnPaint; the GPU-only 4K30 path is unavailable");
      PostThreadMessageW(launcher_thread_id_, kCaptureFailureMessage, 0, 0);
  }
}

void BrowserClient::OnAcceleratedPaint(
    CefRefPtr<CefBrowser>,
    PaintElementType type,
    const RectList&,
    const CefAcceleratedPaintInfo& info) {
  CEF_REQUIRE_UI_THREAD();
  if (!pipeline_->CopyFromCef(type, info)) {
    Log(LogLevel::kError, L"Accelerated frame import/copy failed");
    PostThreadMessageW(launcher_thread_id_, kCaptureFailureMessage, 0, 0);
  } else if (type == PET_VIEW &&
             (pipeline_->frame_id() == 1 || pipeline_->frame_id() % 30 == 0)) {
    PostThreadMessageW(launcher_thread_id_, kCaptureFrameMessage,
                       static_cast<WPARAM>(pipeline_->frame_id()), 0);
  }
}

void BrowserClient::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
  CEF_REQUIRE_UI_THREAD();
  pipeline_->SetPopupVisible(show);
}

void BrowserClient::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  pipeline_->SetPopupBounds(rect);
}

bool BrowserClient::OnBeforePopup(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame>,
    int,
    const CefString& target_url,
    const CefString&,
    CefLifeSpanHandler::WindowOpenDisposition,
    bool user_gesture,
    const CefPopupFeatures&,
    CefWindowInfo&,
    CefRefPtr<CefClient>&,
    CefBrowserSettings&,
    CefRefPtr<CefDictionaryValue>&,
    bool*) {
  CEF_REQUIRE_UI_THREAD();
  if (user_gesture && !target_url.empty()) {
    browser->GetMainFrame()->LoadURL(target_url);
  }
  return true;
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = browser;
  Log(LogLevel::kInfo, L"CEF windowless browser created");
}

bool BrowserClient::DoClose(CefRefPtr<CefBrowser>) {
  CEF_REQUIRE_UI_THREAD();
  return false;
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser>) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = nullptr;
  pipeline_.reset();
  PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
}

void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser>,
                                         bool is_loading,
                                         bool,
                                         bool) {
  CEF_REQUIRE_UI_THREAD();
  Log(LogLevel::kInfo, is_loading ? L"Page loading" : L"Page load complete");
}

void BrowserClient::OnLoadError(CefRefPtr<CefBrowser>,
                                CefRefPtr<CefFrame>,
                                ErrorCode error_code,
                                const CefString& error_text,
                                const CefString&) {
  CEF_REQUIRE_UI_THREAD();
  if (error_code == ERR_ABORTED) {
    return;
  }
  std::wstring message = L"Page load failed: ";
  message.append(error_text.ToWString());
  Log(LogLevel::kError, message);
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                              TerminationStatus,
                                              int,
                                              const CefString&) {
  CEF_REQUIRE_UI_THREAD();
  Log(LogLevel::kError, L"CEF renderer terminated; reloading once");
  browser->Reload();
}

void BrowserClient::OnDocumentAvailableInMainFrame(
    CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (!force_transparency_) {
    return;
  }
  constexpr char kScript[] = R"JS((() => {
    let style = document.getElementById('__streaming_browser_transparency');
    if (!style) {
      style = document.createElement('style');
      style.id = '__streaming_browser_transparency';
      style.textContent = 'html,body{background:transparent!important;}';
      (document.head || document.documentElement).appendChild(style);
    }
  })();)JS";
  browser->GetMainFrame()->ExecuteJavaScript(kScript,
                                              browser->GetMainFrame()->GetURL(),
                                              0);
}

void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser>,
                                    CefRefPtr<CefFrame> frame,
                                    const CefString& url) {
  CEF_REQUIRE_UI_THREAD();
  if (frame->IsMain()) {
    std::wstring message = L"Navigated to ";
    message.append(url.ToWString());
    Log(LogLevel::kInfo, message);
  }
}

bool BrowserClient::OnCursorChange(CefRefPtr<CefBrowser>,
                                   CefCursorHandle,
                                   cef_cursor_type_t,
                                   const CefCursorInfo&) {
  CEF_REQUIRE_UI_THREAD();
  return true;
}

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>,
                                        CefRefPtr<CefFrame>,
                                        CefRefPtr<CefContextMenuParams>,
                                        CefRefPtr<CefMenuModel> model) {
  CEF_REQUIRE_UI_THREAD();
  model->Clear();
}

bool BrowserClient::OnJSDialog(CefRefPtr<CefBrowser>,
                               const CefString&,
                               JSDialogType,
                               const CefString&,
                               const CefString&,
                               CefRefPtr<CefJSDialogCallback>,
                               bool& suppress_message) {
  CEF_REQUIRE_UI_THREAD();
  suppress_message = true;
  return false;
}

bool BrowserClient::OnBeforeUnloadDialog(
    CefRefPtr<CefBrowser>,
    const CefString&,
    bool,
    CefRefPtr<CefJSDialogCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  callback->Continue(true, CefString());
  return true;
}

void BrowserClient::CloseBrowser() {
  if (CefCurrentlyOn(TID_UI)) {
    CloseBrowserOnUi();
    return;
  }
  CefPostTask(TID_UI,
              base::BindOnce(&BrowserClient::CloseBrowserOnUi, this));
}

void BrowserClient::CloseBrowserOnUi() {
  CEF_REQUIRE_UI_THREAD();
  if (browser_) {
    browser_->GetHost()->CloseBrowser(false);
  } else {
    PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
  }
}

}  // namespace streaming::producer
