#pragma once

#include "src/input_routing/routing_protocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace streaming::experiments::usb_routing {

struct WebSocketClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 17831;
  std::string path = "/input/v1";
  std::chrono::milliseconds connect_timeout{1000};
};

class WebSocketClient final {
 public:
  using StatusCallback = std::function<void(std::wstring)>;

  WebSocketClient();
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  bool Start(WebSocketClientOptions options,
             StatusCallback status,
             std::wstring* error);
  void Stop();

  bool ClaimRoute(input_routing::RouteId route_id);
  bool SendEvents(std::span<const input_routing::RoutedEvent> events);
  bool SendSnapshot(const input_routing::StateSnapshot& snapshot);
  bool ReleaseAll();

  [[nodiscard]] bool connected() const;
  [[nodiscard]] input_routing::SessionId session() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace streaming::experiments::usb_routing
