#pragma once

#include <windows.h>

#include <string>

namespace streaming {

std::wstring CurrentLogonSidString();
std::wstring DiscoveryPipeName();

class PipeSecurity final {
 public:
  PipeSecurity() = default;
  ~PipeSecurity();

  PipeSecurity(const PipeSecurity&) = delete;
  PipeSecurity& operator=(const PipeSecurity&) = delete;

  bool InitializeForCurrentLogon();
  [[nodiscard]] SECURITY_ATTRIBUTES* attributes() {
    return initialized_ ? &attributes_ : nullptr;
  }

 private:
  SECURITY_ATTRIBUTES attributes_{};
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  bool initialized_ = false;
};

}  // namespace streaming
