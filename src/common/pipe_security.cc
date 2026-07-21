#include "src/common/pipe_security.h"

#include <sddl.h>

#include <vector>

#include "src/common/logging.h"
#include "src/common/win_handle.h"

namespace streaming {

std::wstring CurrentLogonSidString() {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    LogLastError(LogLevel::kError, L"OpenProcessToken");
    return {};
  }
  UniqueHandle token(raw_token);

  DWORD required = 0;
  GetTokenInformation(token.get(), TokenLogonSid, nullptr, 0, &required);
  if (required == 0) {
    LogLastError(LogLevel::kError, L"GetTokenInformation(size)");
    return {};
  }

  std::vector<std::byte> storage(required);
  if (!GetTokenInformation(token.get(), TokenLogonSid, storage.data(), required,
                           &required)) {
    LogLastError(LogLevel::kError, L"GetTokenInformation(TokenLogonSid)");
    return {};
  }

  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(storage.data());
  if (groups->GroupCount == 0) {
    return {};
  }

  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &sid_text)) {
    LogLastError(LogLevel::kError, L"ConvertSidToStringSidW");
    return {};
  }
  std::wstring result(sid_text);
  LocalFree(sid_text);
  return result;
}

std::wstring DiscoveryPipeName() {
  std::wstring sid = CurrentLogonSidString();
  if (sid.empty()) {
    return {};
  }
  return L"\\\\.\\pipe\\StreamingBrowser.Discovery." + sid;
}

PipeSecurity::~PipeSecurity() {
  if (descriptor_ != nullptr) {
    LocalFree(descriptor_);
  }
}

bool PipeSecurity::InitializeForCurrentLogon() {
  const std::wstring sid = CurrentLogonSidString();
  if (sid.empty()) {
    return false;
  }

  const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
    LogLastError(LogLevel::kError,
                 L"ConvertStringSecurityDescriptorToSecurityDescriptorW");
    return false;
  }

  attributes_.nLength = sizeof(attributes_);
  attributes_.lpSecurityDescriptor = descriptor_;
  attributes_.bInheritHandle = FALSE;
  initialized_ = true;
  return true;
}

}  // namespace streaming
