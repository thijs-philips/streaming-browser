#pragma once

#include "src/compositor/layout_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace streaming::compositor {

struct LayoutServerOptions {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 8765;
  std::string path = "/layout/v1";
};

class LayoutServer final {
 public:
  using LayoutCallback = std::function<void(LayoutSnapshot)>;
  using StatusCallback = std::function<void(std::wstring)>;

  LayoutServer(LayoutServerOptions options,
               LayoutCallback layout_callback,
               StatusCallback status_callback);
  ~LayoutServer();

  LayoutServer(const LayoutServer&) = delete;
  LayoutServer& operator=(const LayoutServer&) = delete;

  bool Start(std::wstring* error);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace streaming::compositor
