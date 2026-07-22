#include <windows.h>
#include <shlobj.h>

#include <charconv>
#include <filesystem>
#include <string>

#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_version_info.h"
#include "src/common/configuration.h"
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

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()), result.data(), count);
  return result;
}

bool ReadIntegerSwitch(CefRefPtr<CefCommandLine> command_line,
                       const char* name,
                       int minimum,
                       int maximum,
                       int* value,
                       std::string* error) {
  if (!command_line->HasSwitch(name)) {
    return true;
  }
  const std::string text = command_line->GetSwitchValue(name);
  int parsed = 0;
  const auto [end, code] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (code != std::errc{} || end != text.data() + text.size() ||
      parsed < minimum || parsed > maximum) {
    *error = std::string("--") + name + " must be between " +
             std::to_string(minimum) + " and " + std::to_string(maximum);
    return false;
  }
  *value = parsed;
  return true;
}

void ApplyBooleanSwitch(CefRefPtr<CefCommandLine> command_line,
                        const char* enabled_name,
                        const char* disabled_name,
                        bool* value) {
  if (command_line->HasSwitch(enabled_name)) {
    *value = true;
  }
  if (command_line->HasSwitch(disabled_name)) {
    *value = false;
  }
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

  streaming::ApplicationConfiguration file_configuration;
  streaming::ProducerConfiguration config;
  std::string configuration_error;
  if (command_line->HasSwitch("config")) {
    const std::wstring path = command_line->GetSwitchValue("config").ToWString();
    if (path.empty() ||
        !streaming::LoadConfigurationYaml(path, &file_configuration,
                                          &configuration_error)) {
      if (path.empty()) {
        configuration_error = "--config requires a YAML file path";
      }
      const std::wstring message =
          L"Could not load producer configuration: " +
          Utf8ToWide(configuration_error);
      streaming::Log(streaming::LogLevel::kError, message);
      MessageBoxW(nullptr, message.c_str(), L"Streaming Browser configuration",
                  MB_OK | MB_ICONERROR);
      return 4;
    }
    config = file_configuration.producer;
    streaming::Log(streaming::LogLevel::kInfo,
                   L"Loaded producer YAML configuration from " + path);
  }

  const std::string url = command_line->GetSwitchValue("url");
  if (!url.empty()) {
    config.url = url;
  }
  ApplyBooleanSwitch(command_line, "force-transparency",
                     "no-force-transparency", &config.force_transparency);
  ApplyBooleanSwitch(command_line, "visible", "hidden",
                     &config.viewer_visible);
  ApplyBooleanSwitch(command_line, "alpha-probe", "no-alpha-probe",
                     &config.alpha_probe_enabled);
  if (!ReadIntegerSwitch(command_line, "width", 320, 16384,
                         &config.view_width, &configuration_error) ||
      !ReadIntegerSwitch(command_line, "height", 240, 16384,
                         &config.view_height, &configuration_error) ||
      !ReadIntegerSwitch(command_line, "frame-rate", 1, 60,
                         &config.frame_rate, &configuration_error)) {
    const std::wstring message = Utf8ToWide(configuration_error);
    streaming::Log(streaming::LogLevel::kError, message);
    MessageBoxW(nullptr, message.c_str(), L"Streaming Browser configuration",
                MB_OK | MB_ICONERROR);
    return 4;
  }

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
