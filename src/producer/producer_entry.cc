#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_version_info.h"
#include "src/common/logging.h"
#include "src/producer/app.h"
#include "src/producer/launcher_window.h"

namespace {

std::wstring LocalDataDirectory() {
  PWSTR known_path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &known_path))) {
    return {};
  }
  std::wstring result(known_path);
  CoTaskMemFree(known_path);
  result.append(L"\\StreamingBrowser");
  std::error_code error;
  std::filesystem::create_directories(result, error);
  return result;
}

void CloseProducer(void* context) {
  auto* app = static_cast<streaming::producer::ProducerApp*>(context);
  app->CloseBrowser();
}

void SetViewerVisible(void* context, bool visible) {
  auto* app = static_cast<streaming::producer::ProducerApp*>(context);
  app->SetViewerVisible(visible);
}

int RunMain(HINSTANCE instance,
            LPTSTR,
            int,
            void* sandbox_info) {
  CefMainArgs main_args(instance);
  const int process_exit = CefExecuteProcess(main_args, nullptr, sandbox_info);
  if (process_exit >= 0) {
    return process_exit;
  }

  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(GetCommandLineW());

  streaming::producer::ProducerConfig config;
  const std::string url = command_line->GetSwitchValue("url");
  if (!url.empty()) {
    config.url = url;
  }
  config.force_transparency = command_line->HasSwitch("force-transparency");
  config.viewer_visible = command_line->HasSwitch("visible");

  const DWORD main_thread_id = GetCurrentThreadId();
  CefRefPtr<streaming::producer::ProducerApp> app(
      new streaming::producer::ProducerApp(main_thread_id, config));

  streaming::producer::LauncherWindow launcher;
  if (!launcher.Create(instance, &CloseProducer, &SetViewerVisible, app.get(),
                       config.viewer_visible)) {
    return 3;
  }
  app->SetParentWindow(launcher.window());

  CefSettings settings;
  settings.multi_threaded_message_loop = true;
  settings.windowless_rendering_enabled = true;
  settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  if (sandbox_info == nullptr) {
    settings.no_sandbox = true;
  }

  const std::wstring data_directory = LocalDataDirectory();
  if (data_directory.empty()) {
    streaming::Log(streaming::LogLevel::kError,
                   L"Could not resolve LocalAppData for CEF cache");
    return 2;
  }
  CefString(&settings.root_cache_path) = data_directory;
  CefString(&settings.cache_path) = data_directory + L"\\Profile";
  CefString(&settings.log_file) = data_directory + L"\\cef.log";
  settings.log_severity = LOGSEVERITY_INFO;

  if (!CefInitialize(main_args, settings, app, sandbox_info)) {
    return CefGetExitCode();
  }

  const int result = launcher.RunMessageLoop();

  CefShutdown();
  return result;
}

}  // namespace

#if defined(CEF_USE_BOOTSTRAP)

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance,
                                    LPTSTR command_line,
                                    int show_command,
                                    void* sandbox_info,
                                    cef_version_info_t*) {
  return RunMain(instance, command_line, show_command, sandbox_info);
}

#else

int APIENTRY wWinMain(HINSTANCE instance,
                      HINSTANCE,
                      LPTSTR command_line,
                      int show_command) {
  return RunMain(instance, command_line, show_command, nullptr);
}

#endif
