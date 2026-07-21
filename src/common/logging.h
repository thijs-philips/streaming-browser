#pragma once

#include <string_view>

namespace streaming {

enum class LogLevel {
  kInfo,
  kWarning,
  kError,
};

void Log(LogLevel level, std::wstring_view message);
void LogLastError(LogLevel level, std::wstring_view operation);

}  // namespace streaming
