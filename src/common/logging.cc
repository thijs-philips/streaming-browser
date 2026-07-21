#include "src/common/logging.h"

#include <windows.h>

#include <array>
#include <mutex>
#include <string>

namespace streaming {
namespace {

std::mutex g_log_mutex;

std::wstring_view Prefix(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo:
      return L"[streaming-browser][info] ";
    case LogLevel::kWarning:
      return L"[streaming-browser][warning] ";
    case LogLevel::kError:
      return L"[streaming-browser][error] ";
  }
  return L"[streaming-browser] ";
}

}  // namespace

void Log(LogLevel level, std::wstring_view message) {
  std::wstring line(Prefix(level));
  line.append(message);
  line.append(L"\r\n");
  OutputDebugStringW(line.c_str());

  std::lock_guard lock(g_log_mutex);
  std::array<wchar_t, MAX_PATH> local_app_data{};
  const DWORD path_length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data.data(),
      static_cast<DWORD>(local_app_data.size()));
  if (path_length == 0 ||
      path_length >= static_cast<DWORD>(local_app_data.size())) {
    return;
  }
  std::wstring directory(local_app_data.data(), path_length);
  directory.append(L"\\StreamingBrowser");
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring path = directory + L"\\app.log";
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  const int utf8_length = WideCharToMultiByte(
      CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0,
      nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                      utf8.data(), utf8_length, nullptr, nullptr);
  DWORD written = 0;
  WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
            nullptr);
  CloseHandle(file);
}

void LogLastError(LogLevel level, std::wstring_view operation) {
  const DWORD error = GetLastError();
  std::array<wchar_t, 512> buffer{};
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      error, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);

  std::wstring message(operation);
  message.append(L" failed (");
  message.append(std::to_wstring(error));
  message.append(L"): ");
  if (length != 0) {
    message.append(buffer.data(), length);
  } else {
    message.append(L"unknown error");
  }
  Log(level, message);
}

}  // namespace streaming
