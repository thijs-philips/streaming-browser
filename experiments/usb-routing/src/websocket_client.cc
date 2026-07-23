#include "experiments/usb-routing/src/websocket_client.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace streaming::experiments::usb_routing {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace {

std::wstring ToWide(std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

}  // namespace

class WebSocketClient::Impl final {
 public:
  bool Start(WebSocketClientOptions options,
             StatusCallback status,
             std::wstring* error) {
    if (thread_.joinable()) {
      if (error != nullptr) *error = L"WebSocket client is already running";
      return false;
    }
    options_ = std::move(options);
    status_ = std::move(status);
    session_ = input_routing::GenerateSessionId();
    stopping_.store(false, std::memory_order_release);
    thread_ = std::thread(&Impl::Run, this);
    return true;
  }

  void Stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true, std::memory_order_release);
    ioc_.stop();
    thread_.join();
    connected_.store(false, std::memory_order_release);
    std::lock_guard lock(queue_mutex_);
    queue_.clear();
  }

  bool ClaimRoute(input_routing::RouteId route_id) {
    if (route_id == 0) return false;
    route_id_.store(route_id, std::memory_order_release);
    sequence_.store(1, std::memory_order_release);
    claimed_route_.store(route_id, std::memory_order_release);
    if (connected()) {
      SendClaim(route_id);
    }
    return true;
  }

  bool SendEvents(std::span<const input_routing::RoutedEvent> events) {
    if (events.empty() || !connected() ||
        route_id_.load(std::memory_order_acquire) == 0) {
      return false;
    }
    input_routing::Message message;
    message.header.type = input_routing::MessageType::kEventBatch;
    message.header.route_id = route_id_.load(std::memory_order_acquire);
    message.header.session = session_;
    message.header.sequence = sequence_.fetch_add(1, std::memory_order_acq_rel);
    message.payload = input_routing::SerializeEvents(events);
    return Enqueue(std::move(message));
  }

  bool SendSnapshot(const input_routing::StateSnapshot& snapshot) {
    if (route_id_.load(std::memory_order_acquire) == 0) return false;
    input_routing::Message message;
    message.header.type = input_routing::MessageType::kStateSnapshot;
    message.header.route_id = route_id_.load(std::memory_order_acquire);
    message.header.session = session_;
    message.header.sequence = sequence_.load(std::memory_order_acquire);
    message.payload = input_routing::SerializeStateSnapshot(snapshot);
    return Enqueue(std::move(message));
  }

  bool ReleaseAll() {
    const input_routing::RouteId intended =
        claimed_route_.exchange(0, std::memory_order_acq_rel);
    const input_routing::RouteId active =
        route_id_.exchange(0, std::memory_order_acq_rel);
    if (active == 0) {
      std::lock_guard lock(queue_mutex_);
      queue_.clear();
      return intended != 0;
    }
    input_routing::Message message;
    message.header.type = input_routing::MessageType::kReleaseAll;
    message.header.route_id = active;
    message.header.session = session_;
    return Enqueue(std::move(message));
  }

  [[nodiscard]] bool connected() const {
    return connected_.load(std::memory_order_acquire);
  }

  [[nodiscard]] input_routing::SessionId session() const { return session_; }

 private:
  bool Enqueue(input_routing::Message message) {
    std::vector<std::byte> wire = input_routing::SerializeMessage(message);
    if (wire.empty()) return false;
    {
      std::lock_guard lock(queue_mutex_);
      if (queue_.size() >= 256) {
        queue_.clear();
        route_id_.store(0, std::memory_order_release);
        Emit(L"WebSocket output queue overloaded; route was reset.");
        return false;
      }
      queue_.push_back(std::move(wire));
    }
    if (connected()) {
      net::post(ioc_, [this] { StartWrite(); });
    }
    return true;
  }

  void Run() {
    std::chrono::milliseconds backoff{100};
    while (!stopping_.load(std::memory_order_acquire)) {
      try {
        ioc_.restart();
        resolver_ = std::make_unique<tcp::resolver>(ioc_);
        stream_ = std::make_unique<websocket::stream<beast::tcp_stream>>(ioc_);
        const auto results = resolver_->resolve(
            options_.host, std::to_string(options_.port));
        beast::get_lowest_layer(*stream_).expires_after(options_.connect_timeout);
        beast::get_lowest_layer(*stream_).connect(results);
        beast::get_lowest_layer(*stream_).socket().set_option(tcp::no_delay(true));
        stream_->set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::client));
        stream_->read_message_max(input_routing::kMaxMessageSize);
        stream_->binary(true);
        stream_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& request) {
              request.set(http::field::sec_websocket_protocol,
                          "streaming-browser-input.v1");
            }));
        stream_->handshake(options_.host + ":" + std::to_string(options_.port),
                           options_.path);
        beast::get_lowest_layer(*stream_).expires_never();
        backoff = std::chrono::milliseconds(100);
        SendHello();
        connected_.store(true, std::memory_order_release);
        Emit(L"WebSocket input route connected to loopback receiver.");
        const input_routing::RouteId claimed =
          claimed_route_.load(std::memory_order_acquire);
        if (claimed != 0) {
          route_id_.store(claimed, std::memory_order_release);
          sequence_.store(1, std::memory_order_release);
          SendClaim(claimed);
        }
        StartRead();
        StartWrite();
        ScheduleHeartbeat();
        ioc_.run();
      } catch (const std::exception& exception) {
        Emit(L"WebSocket connection failed: " + ToWide(exception.what()));
      }
      connected_.store(false, std::memory_order_release);
      writing_ = false;
      read_buffer_.consume(read_buffer_.size());
      if (stopping_.load(std::memory_order_acquire)) break;
      const auto jitter = std::chrono::milliseconds(
          static_cast<int>(std::random_device{}() % 100U));
      std::this_thread::sleep_for(backoff + jitter);
      backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
    }
  }

  void SendHello() {
    input_routing::Message message;
    message.header.type = input_routing::MessageType::kHello;
    message.header.session = session_;
    message.payload.assign(
        reinterpret_cast<const std::byte*>("usb-routing-probe"),
        reinterpret_cast<const std::byte*>("usb-routing-probe") + 17);
    Enqueue(std::move(message));
  }

  void SendClaim(input_routing::RouteId route_id) {
    input_routing::Message message;
    message.header.type = input_routing::MessageType::kClaimRoute;
    message.header.route_id = route_id;
    message.header.session = session_;
    message.payload = input_routing::SerializeStateSnapshot({});
    Enqueue(std::move(message));
  }

  void ScheduleHeartbeat() {
    heartbeat_timer_.expires_after(std::chrono::seconds(2));
    heartbeat_timer_.async_wait([this](beast::error_code error) {
      if (error || !connected()) return;
      input_routing::Message message;
      message.header.type = input_routing::MessageType::kHeartbeat;
      message.header.session = session_;
      message.header.route_id = route_id_.load(std::memory_order_acquire);
      Enqueue(std::move(message));
      ScheduleHeartbeat();
    });
  }

  void StartRead() {
    if (!stream_ || !connected()) return;
    stream_->async_read(
        read_buffer_, [this](beast::error_code error, std::size_t) {
          if (error) {
            OnDisconnect(error);
            return;
          }
          std::vector<std::byte> bytes(read_buffer_.size());
          net::buffer_copy(net::buffer(bytes), read_buffer_.data());
          read_buffer_.consume(read_buffer_.size());
          input_routing::Message message;
          std::string parse_error;
          if (!input_routing::ParseMessage(bytes, &message, &parse_error)) {
            Emit(L"Receiver returned invalid input-routing message: " +
                 ToWide(parse_error));
            OnDisconnect(websocket::error::bad_data_frame);
            return;
          }
          StartRead();
        });
  }

  void StartWrite() {
    if (!stream_ || !connected() || writing_) return;
    std::shared_ptr<std::vector<std::byte>> current;
    {
      std::lock_guard lock(queue_mutex_);
      if (queue_.empty()) return;
      writing_ = true;
        current = std::make_shared<std::vector<std::byte>>(
          std::move(queue_.front()));
        queue_.pop_front();
    }
    stream_->async_write(
        net::buffer(*current),
        [this, current](beast::error_code error, std::size_t) {
          if (error) {
            {
              std::lock_guard lock(queue_mutex_);
              writing_ = false;
            }
            OnDisconnect(error);
            return;
          }
          {
            std::lock_guard lock(queue_mutex_);
            writing_ = false;
          }
          StartWrite();
        });
  }

  void OnDisconnect(beast::error_code error) {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;
    Emit(L"WebSocket input route disconnected: " + ToWide(error.message()));
    route_id_.store(0, std::memory_order_release);
    {
      std::lock_guard lock(queue_mutex_);
      queue_.clear();
    }
    ioc_.stop();
  }

  void Emit(std::wstring message) const {
    if (status_) status_(std::move(message));
  }

  WebSocketClientOptions options_;
  StatusCallback status_;
  input_routing::SessionId session_{};
  std::atomic<input_routing::RouteId> route_id_ = 0;
  std::atomic<input_routing::RouteId> claimed_route_ = 0;
  std::atomic<std::uint64_t> sequence_ = 1;
  std::atomic_bool connected_ = false;
  std::atomic_bool stopping_ = false;
  net::io_context ioc_;
  net::steady_timer heartbeat_timer_{ioc_};
  std::unique_ptr<tcp::resolver> resolver_;
  std::unique_ptr<websocket::stream<beast::tcp_stream>> stream_;
  beast::flat_buffer read_buffer_;
  mutable std::mutex queue_mutex_;
  std::deque<std::vector<std::byte>> queue_;
  bool writing_ = false;
  std::thread thread_;
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}
WebSocketClient::~WebSocketClient() { Stop(); }

bool WebSocketClient::Start(WebSocketClientOptions options,
                            StatusCallback status,
                            std::wstring* error) {
  return impl_->Start(std::move(options), std::move(status), error);
}
void WebSocketClient::Stop() { impl_->Stop(); }
bool WebSocketClient::ClaimRoute(input_routing::RouteId route_id) {
  return impl_->ClaimRoute(route_id);
}
bool WebSocketClient::SendEvents(
    std::span<const input_routing::RoutedEvent> events) {
  return impl_->SendEvents(events);
}
bool WebSocketClient::SendSnapshot(
    const input_routing::StateSnapshot& snapshot) {
  return impl_->SendSnapshot(snapshot);
}
bool WebSocketClient::ReleaseAll() { return impl_->ReleaseAll(); }
bool WebSocketClient::connected() const { return impl_->connected(); }
input_routing::SessionId WebSocketClient::session() const {
  return impl_->session();
}

}  // namespace streaming::experiments::usb_routing
