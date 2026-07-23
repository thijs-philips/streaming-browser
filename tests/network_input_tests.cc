#include "src/producer/network_input_server.h"
#include "src/input_routing/routing_protocol.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
namespace routing = streaming::input_routing;
namespace {

std::vector<std::byte> Wire(routing::Message message) {
  return routing::SerializeMessage(message);
}

void Send(websocket::stream<tcp::socket>* socket, routing::Message message) {
  const std::vector<std::byte> wire = Wire(std::move(message));
  socket->binary(true);
  socket->write(net::buffer(wire));
}

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<streaming::protocol::InputEvent> received;
  std::vector<bool> route_activity;

  streaming::producer::NetworkInputServerOptions options;
  options.port = 17839;
  streaming::producer::NetworkInputServer server(
      options,
      [&](streaming::protocol::InputEvent event) {
        {
          std::lock_guard lock(mutex);
          received.push_back(event);
        }
        changed.notify_all();
      },
      [&](bool active) {
        {
          std::lock_guard lock(mutex);
          route_activity.push_back(active);
        }
        changed.notify_all();
      },
      [](std::wstring) {});
  std::wstring error;
  if (!server.Start(&error)) return Fail("server did not start");

  net::io_context ioc;
  websocket::stream<tcp::socket> socket(ioc);
  tcp::resolver resolver(ioc);
  net::connect(socket.next_layer(), resolver.resolve("127.0.0.1", "17839"));
  socket.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& request) {
        request.set(beast::http::field::sec_websocket_protocol,
                    "streaming-browser-input.v1");
      }));
  socket.handshake("127.0.0.1:17839", "/input/v1");

  const routing::SessionId session = routing::GenerateSessionId();
  routing::Message hello;
  hello.header.type = routing::MessageType::kHello;
  hello.header.session = session;
  Send(&socket, hello);

  routing::Message claim;
  claim.header.type = routing::MessageType::kClaimRoute;
  claim.header.route_id = 1;
  claim.header.session = session;
  claim.payload = routing::SerializeStateSnapshot({});
  Send(&socket, claim);

  routing::RoutedEvent down;
  down.kind = routing::EventKind::kKeyDown;
  down.value1 = 'A';
  down.value2 = 0x1E;
  routing::RoutedEvent move;
  move.kind = routing::EventKind::kMouseMove;
  move.value1 = 20;
  move.value2 = -10;
  routing::Message batch;
  batch.header.type = routing::MessageType::kEventBatch;
  batch.header.route_id = 1;
  batch.header.session = session;
  batch.header.sequence = 1;
  const std::array events{down, move};
  batch.payload = routing::SerializeEvents(events);
  Send(&socket, batch);

  {
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3),
                          [&] {
                            return std::ranges::any_of(
                                received, [](const auto& event) {
                                  return event.kind ==
                                      streaming::protocol::InputKind::kMouseMove;
                                });
                          })) {
      return Fail("server did not forward routed events");
    }
    const bool saw_key_down = std::ranges::any_of(
        received, [](const auto& event) {
          return event.kind == streaming::protocol::InputKind::kKeyDown &&
                 event.value1 == 'A';
        });
    const bool saw_motion = std::ranges::any_of(
        received, [](const auto& event) {
          return event.kind == streaming::protocol::InputKind::kMouseMove &&
                 event.x == 1940 && event.y == 1070;
        });
    if (!saw_key_down || !saw_motion) {
      return Fail("forwarded event conversion mismatch");
    }
  }

  beast::error_code ignored;
  socket.next_layer().close(ignored);
  {
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3), [&] {
          return route_activity.size() >= 2 &&
                 std::ranges::any_of(received, [](const auto& event) {
                   return event.kind ==
                       streaming::protocol::InputKind::kCaptureLost;
                 });
        })) {
      return Fail("disconnect did not release receiver-held state");
    }
    bool saw_key_up = false;
    bool saw_capture_lost = false;
    for (const auto& event : received) {
      saw_key_up |= event.kind == streaming::protocol::InputKind::kKeyUp &&
                    event.value1 == 'A';
      saw_capture_lost |=
          event.kind == streaming::protocol::InputKind::kCaptureLost;
    }
    if (!saw_key_up || !saw_capture_lost)
      return Fail("disconnect safety events missing");
    if (route_activity.size() < 2 || !route_activity.front() ||
        route_activity.back())
      return Fail("route activity lifecycle mismatch");
  }

  server.Stop();

  streaming::producer::NetworkInputServerOptions unsafe;
  unsafe.bind_address = "0.0.0.0";
  unsafe.port = 17840;
  streaming::producer::NetworkInputServer rejected(
      unsafe, [](streaming::protocol::InputEvent) {}, [](bool) {},
      [](std::wstring) {});
  if (rejected.Start(&error)) return Fail("non-loopback server was accepted");

  streaming::producer::NetworkInputServerOptions handshake_options;
  handshake_options.port = 17841;
  streaming::producer::NetworkInputServer handshake_server(
      handshake_options, [](streaming::protocol::InputEvent) {},
      [](bool) {}, [](std::wstring) {});
  if (!handshake_server.Start(&error))
    return Fail("handshake test server did not start");
  bool bad_handshake_rejected = false;
  try {
    net::io_context bad_ioc;
    websocket::stream<tcp::socket> bad_socket(bad_ioc);
    tcp::resolver bad_resolver(bad_ioc);
    net::connect(bad_socket.next_layer(),
                 bad_resolver.resolve("127.0.0.1", "17841"));
    bad_socket.handshake("127.0.0.1:17841", "/wrong");
  } catch (const std::exception&) {
    bad_handshake_rejected = true;
  }
  handshake_server.Stop();
  if (!bad_handshake_rejected)
    return Fail("wrong WebSocket path/subprotocol was accepted");

  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_changed;
  std::vector<bool> heartbeat_activity;
  streaming::producer::NetworkInputServerOptions heartbeat_options;
  heartbeat_options.port = 17842;
  heartbeat_options.heartbeat_timeout = std::chrono::milliseconds(100);
  streaming::producer::NetworkInputServer heartbeat_server(
      heartbeat_options, [](streaming::protocol::InputEvent) {},
      [&](bool active) {
        {
          std::lock_guard lock(heartbeat_mutex);
          heartbeat_activity.push_back(active);
        }
        heartbeat_changed.notify_all();
      },
      [](std::wstring) {});
  if (!heartbeat_server.Start(&error))
    return Fail("heartbeat test server did not start");
  net::io_context heartbeat_ioc;
  websocket::stream<tcp::socket> heartbeat_socket(heartbeat_ioc);
  tcp::resolver heartbeat_resolver(heartbeat_ioc);
  net::connect(heartbeat_socket.next_layer(),
               heartbeat_resolver.resolve("127.0.0.1", "17842"));
  heartbeat_socket.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& request) {
        request.set(beast::http::field::sec_websocket_protocol,
                    "streaming-browser-input.v1");
      }));
  heartbeat_socket.handshake("127.0.0.1:17842", "/input/v1");
  const routing::SessionId heartbeat_session = routing::GenerateSessionId();
  hello.header.session = heartbeat_session;
  Send(&heartbeat_socket, hello);
  claim.header.session = heartbeat_session;
  Send(&heartbeat_socket, claim);
  {
    std::unique_lock lock(heartbeat_mutex);
    if (!heartbeat_changed.wait_for(
            lock, std::chrono::seconds(3),
            [&] {
              return heartbeat_activity.size() >= 2 &&
                     heartbeat_activity.front() &&
                     !heartbeat_activity.back();
            })) {
      return Fail("missing heartbeat did not revoke route");
    }
  }
  heartbeat_server.Stop();

  std::cout << "network input tests passed\n";
  return 0;
}
