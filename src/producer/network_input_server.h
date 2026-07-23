#pragma once

#include "src/common/protocol.h"
#include "src/input_routing/route_state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace streaming::producer {

struct NetworkInputServerOptions {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 17831;
  int view_width = 3840;
  int view_height = 2160;
  std::size_t max_connections = 1;
  std::chrono::milliseconds heartbeat_timeout{6000};
};

class NetworkInputServer final {
 public:
  using InputCallback = std::function<void(protocol::InputEvent)>;
  using StatusCallback = std::function<void(std::wstring)>;
  using RouteActivityCallback = std::function<void(bool)>;

  NetworkInputServer(NetworkInputServerOptions options,
                     InputCallback input_callback,
                     RouteActivityCallback route_activity_callback,
                     StatusCallback status_callback);
  ~NetworkInputServer();

  NetworkInputServer(const NetworkInputServer&) = delete;
  NetworkInputServer& operator=(const NetworkInputServer&) = delete;

  bool Start(std::wstring* error);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace streaming::producer
