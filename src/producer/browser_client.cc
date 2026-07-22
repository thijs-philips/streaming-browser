#include "src/producer/browser_client.h"

#include <algorithm>
#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_menu_model.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "src/common/logging.h"
#include "src/producer/launcher_window.h"

namespace streaming::producer {

BrowserClient::BrowserClient(DWORD launcher_thread_id,
                             const ProducerConfiguration& configuration)
    : launcher_thread_id_(launcher_thread_id),
      view_width_(configuration.view_width),
      view_height_(configuration.view_height),
      force_transparency_(configuration.force_transparency),
      viewer_visible_(configuration.viewer_visible) {
  stream_server_ = std::make_unique<StreamServer>(
      [this](D3DFramePipeline::RingSnapshot* snapshot) {
        return pipeline_ && pipeline_->GetRingSnapshot(snapshot);
      },
      [this] {
        CefPostTask(TID_UI, base::BindOnce(&BrowserClient::OnViewerReady, this));
      },
      [this](std::uint32_t slot, std::uint64_t frame_id) {
        CefPostTask(TID_UI,
                    base::BindOnce(&BrowserClient::OnFrameReleased, this, slot,
                                   frame_id));
      },
      [this](const protocol::InputEvent& event) {
        CefPostTask(TID_UI,
                    base::BindOnce(&BrowserClient::OnViewerInput, this, event));
      },
      [this](protocol::ImeEvent event) {
        CefPostTask(TID_UI,
                    base::BindOnce(&BrowserClient::OnViewerIme, this,
                                   std::move(event)));
      },
      [this](protocol::MessageType type, std::string value) {
        CefPostTask(TID_UI,
                    base::BindOnce(&BrowserClient::OnViewerCommand, this, type,
                                   std::move(value)));
      },
      [this](std::uint32_t width, std::uint32_t height) {
        CefPostTask(TID_UI,
                    base::BindOnce(&BrowserClient::OnViewerViewport, this,
                                   width, height));
      },
      [this] {
        CefPostTask(
            TID_UI,
            base::BindOnce(&BrowserClient::OnViewerDisconnected, this));
      });
  pipeline_ = std::make_unique<D3DFramePipeline>(
      [this](const protocol::FrameMetadata& metadata) {
        const bool published =
            stream_server_ && stream_server_->PublishFrame(metadata);
        if (published &&
          (metadata.frame_id == 1 || metadata.frame_id % 30 == 0)) {
          PostThreadMessageW(launcher_thread_id_, kStreamFrameMessage,
                             static_cast<WPARAM>(metadata.frame_id), 0);
        }
        return published;
      },
      [this] {
        if (stream_server_) {
          stream_server_->NotifyRingReady();
          // A ring regenerated while a viewer is attached (for example after
          // a server-side viewport resize) requires a fresh handshake.
          if (stream_server_->connected()) {
            stream_server_->ResetStream();
          }
        }
      },
      configuration.alpha_probe_enabled);
  stream_server_->Start();
}

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  rect = CefRect(0, 0, view_width_, view_height_);
}

bool BrowserClient::GetRootScreenRect(CefRefPtr<CefBrowser>, CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  rect = CefRect(0, 0, view_width_, view_height_);
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
  screen_info.rect = CefRect(0, 0, view_width_, view_height_);
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
    const RectList& dirty_rects,
    const CefAcceleratedPaintInfo& info) {
  CEF_REQUIRE_UI_THREAD();
  if (!pipeline_->CopyFromCef(type, dirty_rects, info)) {
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
  Log(LogLevel::kInfo, show ? L"CEF popup shown" : L"CEF popup hidden");
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
  if (stream_server_) {
    stream_server_->Stop();
    stream_server_.reset();
  }
  browser_ = nullptr;
  pipeline_.reset();
  PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
}

void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser>,
                                         bool is_loading,
                                         bool can_go_back,
                                         bool can_go_forward) {
  CEF_REQUIRE_UI_THREAD();
  loading_ = is_loading;
  can_go_back_ = can_go_back;
  can_go_forward_ = can_go_forward;
  PublishNavigationState();
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
    current_url_ = url.ToString();
    PublishNavigationState();
    std::wstring message = L"Navigated to ";
    message.append(url.ToWString());
    Log(LogLevel::kInfo, message);
  }
}

bool BrowserClient::OnCursorChange(CefRefPtr<CefBrowser>,
                                   CefCursorHandle,
                                   cef_cursor_type_t type,
                                   const CefCursorInfo&) {
  CEF_REQUIRE_UI_THREAD();
  if (stream_server_) {
    stream_server_->SendCursorState(static_cast<std::uint32_t>(type));
  }
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

void BrowserClient::SetViewerVisible(bool visible) {
  viewer_visible_ = visible;
  if (stream_server_) {
    stream_server_->SetViewerVisible(visible);
  }
}

void BrowserClient::CloseBrowserOnUi() {
  CEF_REQUIRE_UI_THREAD();
  if (browser_) {
    browser_->GetHost()->CloseBrowser(false);
  } else {
    PostThreadMessageW(launcher_thread_id_, WM_QUIT, 0, 0);
  }
}

void BrowserClient::OnViewerReady() {
  CEF_REQUIRE_UI_THREAD();
  if (pipeline_) {
    pipeline_->SetStreamingEnabled(true);
  }
  if (stream_server_) {
    stream_server_->SetViewerVisible(viewer_visible_);
  }
  PublishNavigationState();
}

void BrowserClient::OnFrameReleased(std::uint32_t slot,
                                    std::uint64_t frame_id) {
  CEF_REQUIRE_UI_THREAD();
  if (pipeline_) {
    pipeline_->ReleaseOutputSlot(slot, frame_id);
  }
}

void BrowserClient::OnViewerInput(protocol::InputEvent event) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser_ || !browser_->IsValid()) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = browser_->GetHost();
  CefMouseEvent mouse;
  mouse.x = std::clamp(event.x, 0, view_width_ - 1);
  mouse.y = std::clamp(event.y, 0, view_height_ - 1);
  mouse.modifiers = event.modifiers;
  switch (event.kind) {
    case protocol::InputKind::kMouseMove:
      host->SendMouseMoveEvent(mouse, false);
      break;
    case protocol::InputKind::kMouseLeave:
      host->SendMouseMoveEvent(mouse, true);
      break;
    case protocol::InputKind::kMouseDown:
    case protocol::InputKind::kMouseUp: {
      const auto button = static_cast<CefBrowserHost::MouseButtonType>(
          std::clamp(event.value1, 0, 2));
      host->SendMouseClickEvent(mouse, button,
                                event.kind == protocol::InputKind::kMouseUp,
                                std::max(event.value2, 1));
      break;
    }
    case protocol::InputKind::kMouseWheel:
      host->SendMouseWheelEvent(mouse, event.value1, event.value2);
      break;
    case protocol::InputKind::kKeyDown:
    case protocol::InputKind::kKeyUp:
    case protocol::InputKind::kCharacter: {
      CefKeyEvent key;
      key.modifiers = event.modifiers;
      key.windows_key_code = event.value1;
      key.native_key_code = event.value2;
      if (event.kind == protocol::InputKind::kKeyDown) {
        key.type = KEYEVENT_RAWKEYDOWN;
      } else if (event.kind == protocol::InputKind::kKeyUp) {
        key.type = KEYEVENT_KEYUP;
      } else {
        key.type = KEYEVENT_CHAR;
        key.character = static_cast<char16_t>(event.value1);
        key.unmodified_character = key.character;
      }
      host->SendKeyEvent(key);
      break;
    }
    case protocol::InputKind::kFocus:
      host->SetFocus(event.value1 != 0);
      break;
    case protocol::InputKind::kCaptureLost:
      host->SendCaptureLostEvent();
      break;
  }
}

void BrowserClient::OnViewerCommand(protocol::MessageType type,
                                    std::string value) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser_ || !browser_->IsValid()) {
    return;
  }
  switch (type) {
    case protocol::MessageType::kNavigate:
      browser_->GetMainFrame()->LoadURL(value);
      break;
    case protocol::MessageType::kBack:
      browser_->GoBack();
      break;
    case protocol::MessageType::kForward:
      browser_->GoForward();
      break;
    case protocol::MessageType::kReload:
      browser_->Reload();
      break;
    case protocol::MessageType::kStopLoad:
      browser_->StopLoad();
      break;
    case protocol::MessageType::kHideViewer:
      browser_->GetHost()->SendCaptureLostEvent();
      browser_->GetHost()->SetFocus(false);
      browser_->GetHost()->ImeCancelComposition();
      break;
    default:
      break;
  }
}

void BrowserClient::OnViewerIme(protocol::ImeEvent event) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser_ || !browser_->IsValid()) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = browser_->GetHost();
  const CefString text(event.text);
  switch (event.kind) {
    case protocol::ImeKind::kComposition: {
      std::vector<CefCompositionUnderline> underlines;
      if (!event.text.empty()) {
        CefCompositionUnderline underline;
        underline.range =
          CefRange(0, static_cast<std::uint32_t>(event.text.size()));
        underline.color = CefColorSetARGB(255, 0, 0, 0);
        underline.background_color = CefColorSetARGB(0, 0, 0, 0);
        underline.thick = false;
        underline.style = CEF_CUS_SOLID;
        underlines.push_back(underline);
      }
      host->ImeSetComposition(
          text, underlines, CefRange::InvalidRange(),
          CefRange(static_cast<std::uint32_t>(event.text.size()),
                   static_cast<std::uint32_t>(event.text.size())));
      break;
    }
    case protocol::ImeKind::kCommit:
      host->ImeCommitText(text, CefRange::InvalidRange(), 0);
      break;
    case protocol::ImeKind::kFinish:
      host->ImeFinishComposingText(false);
      break;
    case protocol::ImeKind::kCancel:
      host->ImeCancelComposition();
      break;
  }
}

void BrowserClient::OnViewerViewport(std::uint32_t width,
                                     std::uint32_t height) {
  CEF_REQUIRE_UI_THREAD();
  const int new_width = static_cast<int>(width);
  const int new_height = static_cast<int>(height);
  if (new_width == view_width_ && new_height == view_height_) {
    return;
  }
  view_width_ = new_width;
  view_height_ = new_height;
  Log(LogLevel::kInfo,
      L"Server-side scaling: resizing browser viewport to " +
          std::to_wstring(new_width) + L"x" + std::to_wstring(new_height));
  if (browser_ && browser_->IsValid()) {
    browser_->GetHost()->WasResized();
  }
}

void BrowserClient::OnViewerDisconnected() {
  CEF_REQUIRE_UI_THREAD();
  if (pipeline_) {
    pipeline_->SetStreamingEnabled(false);
  }
  if (browser_ && browser_->IsValid()) {
    browser_->GetHost()->SendCaptureLostEvent();
    browser_->GetHost()->SetFocus(false);
    browser_->GetHost()->ImeCancelComposition();
  }
}

void BrowserClient::PublishNavigationState() {
  if (stream_server_) {
    stream_server_->SendNavigationState(current_url_, loading_, can_go_back_,
                                        can_go_forward_);
  }
}

}  // namespace streaming::producer
