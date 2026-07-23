#include "src/producer/network_input_server.h"

#include "src/input_routing/routing_protocol.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace streaming::producer {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
namespace routing = input_routing;

namespace {

std::wstring ToWide(std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

class Session final : public std::enable_shared_from_this<Session> {
 public:
  using ClosedCallback = std::function<void()>;

  Session(tcp::socket socket,
          NetworkInputServerOptions options,
          NetworkInputServer::InputCallback input_callback,
      NetworkInputServer::RouteActivityCallback route_activity_callback,
          NetworkInputServer::StatusCallback status_callback,
          ClosedCallback closed_callback)
      : stream_(std::move(socket)),
        options_(std::move(options)),
        input_callback_(std::move(input_callback)),
        route_activity_callback_(std::move(route_activity_callback)),
        status_callback_(std::move(status_callback)),
        closed_callback_(std::move(closed_callback)),
        activity_timer_(stream_.get_executor()),
        route_state_(
            [this](const routing::RoutedEvent& event) { Forward(event); },
            [this](const routing::StateSnapshot& state) { Release(state); }) {}

  void Run() {
    http::async_read(stream_.next_layer(), buffer_, request_,
      [self = shared_from_this()](beast::error_code error, std::size_t) {
          if (error) {
            self->Close(error);
            return;
          }
          const auto protocol = self->request_[http::field::sec_websocket_protocol];
          if (self->request_.target() != "/input/v1" ||
              protocol != "streaming-browser-input.v1") {
            self->Emit(L"Rejected WebSocket input handshake with wrong path or subprotocol.");
            self->Close(websocket::error::bad_data_frame);
            return;
          }
          self->stream_.set_option(websocket::stream_base::timeout::suggested(
              beast::role_type::server));
          self->stream_.read_message_max(routing::kMaxMessageSize);
          self->stream_.binary(true);
          self->stream_.set_option(websocket::stream_base::decorator(
              [](websocket::response_type& response) {
                response.set(http::field::sec_websocket_protocol,
                             "streaming-browser-input.v1");
              }));
          self->stream_.async_accept(
              self->request_,
              [self](beast::error_code accept_error) {
                if (accept_error) {
                  self->Close(accept_error);
                  return;
                }
                self->Emit(L"Loopback WebSocket input client connected.");
                self->last_activity_ = std::chrono::steady_clock::now();
                self->CheckActivity();
                self->Read();
              });
        });
  }

  void Stop() {
    activity_timer_.cancel();
    beast::error_code ignored;
    stream_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    stream_.next_layer().close(ignored);
    route_state_.ReleaseAll();
    DeactivateRoute();
  }

 private:
  void Read() {
    stream_.async_read(
        buffer_, [self = shared_from_this()](beast::error_code error,
                                             std::size_t) {
          if (error) {
            self->Close(error);
            return;
          }
          std::vector<std::byte> bytes(self->buffer_.size());
          net::buffer_copy(net::buffer(bytes), self->buffer_.data());
          self->buffer_.consume(self->buffer_.size());
          if (!self->Process(bytes)) return;
          self->Read();
        });
  }

  bool Process(std::span<const std::byte> bytes) {
    last_activity_ = std::chrono::steady_clock::now();
    routing::Message message;
    std::string error;
    if (!routing::ParseMessage(bytes, &message, &error)) {
      Emit(L"Rejected malformed input-routing message: " + ToWide(error));
      Close(websocket::error::bad_data_frame);
      return false;
    }

    switch (message.header.type) {
      case routing::MessageType::kHello:
        if (hello_received_ || message.header.session == routing::SessionId{}) {
          Close(websocket::error::bad_data_frame);
          return false;
        }
        hello_received_ = true;
        hello_session_ = message.header.session;
        SendControl(routing::MessageType::kAccept, message.header);
        return true;
      case routing::MessageType::kClaimRoute: {
        routing::StateSnapshot snapshot;
        if (!hello_received_ || message.header.session != hello_session_ ||
            !routing::ParseStateSnapshot(message.payload, &snapshot, &error) ||
            !route_state_.Claim(message.header.route_id,
                                message.header.session)) {
          SendControl(routing::MessageType::kReject, message.header);
          return true;
        }
        route_state_.ApplySnapshot(message.header, snapshot);
        route_active_ = true;
        if (route_activity_callback_) route_activity_callback_(true);
        protocol::InputEvent focus;
        focus.kind = protocol::InputKind::kFocus;
        focus.value1 = 1;
        if (input_callback_) input_callback_(focus);
        Emit(L"Loopback input route claimed.");
        SendControl(routing::MessageType::kRouteStatus, message.header);
        return true;
      }
      case routing::MessageType::kEventBatch: {
        std::vector<routing::RoutedEvent> events;
        if (!routing::ParseEvents(message.payload, &events, &error)) {
          Close(websocket::error::bad_data_frame);
          return false;
        }
        const routing::ApplyResult result =
            route_state_.ApplyBatch(message.header, events);
        if (result == routing::ApplyResult::kApplied) {
          Emit(L"Loopback input batch applied; event count=" +
               std::to_wstring(events.size()));
        }
        if (result != routing::ApplyResult::kApplied &&
            result != routing::ApplyResult::kDuplicate) {
          DeactivateRoute();
          Emit(L"Loopback input batch rejected; result=" +
               std::to_wstring(static_cast<int>(result)));
          SendControl(routing::MessageType::kError, message.header);
        }
        return true;
      }
      case routing::MessageType::kStateSnapshot: {
        routing::StateSnapshot snapshot;
        if (!routing::ParseStateSnapshot(message.payload, &snapshot, &error)) {
          Close(websocket::error::bad_data_frame);
          return false;
        }
        route_state_.ApplySnapshot(message.header, snapshot);
        return true;
      }
      case routing::MessageType::kHeartbeat:
        SendControl(routing::MessageType::kHeartbeat, message.header);
        return true;
      case routing::MessageType::kReleaseAll:
        if (!route_state_.claimed() ||
            message.header.session != hello_session_ ||
            message.header.route_id != route_state_.route_id()) {
          SendControl(routing::MessageType::kError, message.header);
          return true;
        }
        route_state_.ReleaseAll();
        DeactivateRoute();
        Emit(L"Loopback input route released.");
        return true;
      case routing::MessageType::kAccept:
      case routing::MessageType::kReject:
      case routing::MessageType::kRouteStatus:
      case routing::MessageType::kError:
        Close(websocket::error::bad_data_frame);
        return false;
    }
    return false;
  }

  void SendControl(routing::MessageType type,
                   const routing::MessageHeader& request) {
    routing::Message response;
    response.header.type = type;
    response.header.route_id = request.route_id;
    response.header.session = request.session;
    response.header.sequence = request.sequence;
    std::vector<std::byte> wire = routing::SerializeMessage(response);
    if (wire.empty()) return;
    const bool start = writes_.empty();
    writes_.push_back(std::move(wire));
    if (start) Write();
  }

  void Write() {
    if (writes_.empty()) return;
    stream_.async_write(
        net::buffer(writes_.front()),
        [self = shared_from_this()](beast::error_code error, std::size_t) {
          if (error) {
            self->Close(error);
            return;
          }
          self->writes_.pop_front();
          if (!self->writes_.empty()) self->Write();
        });
  }

  void Forward(const routing::RoutedEvent& routed) {
    protocol::InputEvent event;
    event.modifiers = static_cast<std::uint16_t>(routed.modifiers & 0xFFFFU);
    event.x = cursor_x_;
    event.y = cursor_y_;
    switch (routed.kind) {
      case routing::EventKind::kKeyDown:
      case routing::EventKind::kKeyUp:
        event.kind = routed.kind == routing::EventKind::kKeyDown
                         ? protocol::InputKind::kKeyDown
                         : protocol::InputKind::kKeyUp;
        event.value1 = routed.value1;
        event.value2 = routed.value2;
        break;
      case routing::EventKind::kMouseMove:
        cursor_x_ = std::clamp(cursor_x_ + routed.value1, 0,
                               options_.view_width - 1);
        cursor_y_ = std::clamp(cursor_y_ + routed.value2, 0,
                               options_.view_height - 1);
        event.kind = protocol::InputKind::kMouseMove;
        event.x = cursor_x_;
        event.y = cursor_y_;
        break;
      case routing::EventKind::kMouseButtonDown:
      case routing::EventKind::kMouseButtonUp:
        event.kind = routed.kind == routing::EventKind::kMouseButtonDown
                         ? protocol::InputKind::kMouseDown
                         : protocol::InputKind::kMouseUp;
        event.x = cursor_x_;
        event.y = cursor_y_;
        event.value1 = routed.value1;
        event.value2 = 1;
        break;
      case routing::EventKind::kMouseWheel:
        event.kind = protocol::InputKind::kMouseWheel;
        event.x = cursor_x_;
        event.y = cursor_y_;
        event.value1 = routed.value1;
        event.value2 = routed.value2;
        break;
    }
    if (input_callback_) input_callback_(event);
  }

  void Release(const routing::StateSnapshot& state) {
    for (const std::uint32_t key : state.held_keys) {
      protocol::InputEvent event;
      event.kind = protocol::InputKind::kKeyUp;
      event.value1 = static_cast<std::int32_t>(key);
      if (input_callback_) input_callback_(event);
    }
    for (int button = 0; button < 3; ++button) {
      if ((state.mouse_buttons & (1U << static_cast<unsigned int>(button))) == 0)
        continue;
      protocol::InputEvent event;
      event.kind = protocol::InputKind::kMouseUp;
      event.x = cursor_x_;
      event.y = cursor_y_;
      event.value1 = button;
      event.value2 = 1;
      if (input_callback_) input_callback_(event);
    }
  }

  void Close(beast::error_code error) {
    if (closed_) return;
    closed_ = true;
    activity_timer_.cancel();
    route_state_.ReleaseAll();
    DeactivateRoute();
    if (error != websocket::error::closed && error != net::error::operation_aborted)
      Emit(L"Loopback WebSocket input client disconnected: " +
           ToWide(error.message()));
    beast::error_code ignored;
    stream_.next_layer().close(ignored);
    if (closed_callback_) closed_callback_();
  }

  void Emit(std::wstring message) const {
    if (status_callback_) status_callback_(std::move(message));
  }

  void DeactivateRoute() {
    if (!route_active_) return;
    route_active_ = false;
    protocol::InputEvent capture_lost;
    capture_lost.kind = protocol::InputKind::kCaptureLost;
    if (input_callback_) input_callback_(capture_lost);
    protocol::InputEvent focus;
    focus.kind = protocol::InputKind::kFocus;
    focus.value1 = 0;
    if (input_callback_) input_callback_(focus);
    if (route_activity_callback_) route_activity_callback_(false);
  }

  void CheckActivity() {
    activity_timer_.expires_after(std::chrono::milliseconds(500));
    activity_timer_.async_wait(
        [self = shared_from_this()](beast::error_code error) {
          if (error || self->closed_) return;
          const auto idle = std::chrono::steady_clock::now() -
                            self->last_activity_;
          if (self->route_active_ && idle > self->options_.heartbeat_timeout) {
            self->Emit(L"Loopback input heartbeat timed out; releasing route.");
            self->Close(net::error::timed_out);
            return;
          }
          self->CheckActivity();
        });
  }

  websocket::stream<tcp::socket> stream_;
  NetworkInputServerOptions options_;
  NetworkInputServer::InputCallback input_callback_;
  NetworkInputServer::RouteActivityCallback route_activity_callback_;
  NetworkInputServer::StatusCallback status_callback_;
  ClosedCallback closed_callback_;
  net::steady_timer activity_timer_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> request_;
  std::deque<std::vector<std::byte>> writes_;
  routing::RouteState route_state_;
  int cursor_x_ = options_.view_width / 2;
  int cursor_y_ = options_.view_height / 2;
  bool closed_ = false;
  bool route_active_ = false;
  bool hello_received_ = false;
  routing::SessionId hello_session_{};
  std::chrono::steady_clock::time_point last_activity_{};
};

}  // namespace

class NetworkInputServer::Impl final {
 public:
  Impl(NetworkInputServerOptions options,
       InputCallback input_callback,
       RouteActivityCallback route_activity_callback,
       StatusCallback status_callback)
      : options_(std::move(options)),
        input_callback_(std::move(input_callback)),
        route_activity_callback_(std::move(route_activity_callback)),
        status_callback_(std::move(status_callback)) {}

  bool Start(std::wstring* error) {
    if (thread_.joinable()) {
      if (error != nullptr) *error = L"Network input server is already running";
      return false;
    }
    try {
      const auto address = net::ip::make_address(options_.bind_address);
      if (!address.is_loopback()) {
        if (error != nullptr)
          *error = L"Prototype network input server only permits loopback";
        return false;
      }
      acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
      acceptor_->open(address.is_v6() ? tcp::v6() : tcp::v4());
      acceptor_->set_option(tcp::acceptor::reuse_address(true));
      acceptor_->bind({address, options_.port});
      acceptor_->listen(static_cast<int>(options_.max_connections));
      Accept();
      thread_ = std::thread([this] { ioc_.run(); });
      Emit(L"Loopback WebSocket input server listening on " +
           ToWide(options_.bind_address) + L":" +
           std::to_wstring(options_.port));
      return true;
    } catch (const std::exception& exception) {
      if (error != nullptr) *error = ToWide(exception.what());
      return false;
    }
  }

  void Stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true, std::memory_order_release);
    std::promise<void> stopped;
    std::future<void> stopped_future = stopped.get_future();
    net::post(ioc_, [this, &stopped] {
      if (session_) session_->Stop();
      beast::error_code ignored;
      if (acceptor_) acceptor_->close(ignored);
      stopped.set_value();
    });
    stopped_future.wait_for(std::chrono::seconds(2));
    ioc_.stop();
    thread_.join();
    session_.reset();
    acceptor_.reset();
  }

 private:
  void Accept() {
    acceptor_->async_accept([this](beast::error_code error,
                                   tcp::socket socket) {
      if (stopping_.load(std::memory_order_acquire)) return;
      if (!error) {
        if (session_) {
          beast::error_code ignored;
          socket.close(ignored);
        } else {
          session_ = std::make_shared<Session>(
              std::move(socket), options_, input_callback_,
              route_activity_callback_, status_callback_,
              [this] { net::post(ioc_, [this] { session_.reset(); }); });
          session_->Run();
        }
      }
      if (acceptor_ && acceptor_->is_open()) Accept();
    });
  }

  void Emit(std::wstring message) const {
    if (status_callback_) status_callback_(std::move(message));
  }

  NetworkInputServerOptions options_;
  InputCallback input_callback_;
  RouteActivityCallback route_activity_callback_;
  StatusCallback status_callback_;
  net::io_context ioc_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::shared_ptr<Session> session_;
  std::thread thread_;
  std::atomic_bool stopping_ = false;
};

NetworkInputServer::NetworkInputServer(NetworkInputServerOptions options,
                                       InputCallback input_callback,
                     RouteActivityCallback route_activity_callback,
                                       StatusCallback status_callback)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(input_callback),
                   std::move(route_activity_callback),
                                   std::move(status_callback))) {}
NetworkInputServer::~NetworkInputServer() { Stop(); }
bool NetworkInputServer::Start(std::wstring* error) { return impl_->Start(error); }
void NetworkInputServer::Stop() { impl_->Stop(); }

}  // namespace streaming::producer
