#include "src/common/logging.h"

#include <windows.h>

#include <array>
#include <string>

namespace streaming {
namespace {

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
