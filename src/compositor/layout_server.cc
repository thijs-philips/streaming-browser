#include "src/compositor/layout_server.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace streaming::compositor {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace {

std::wstring ToWide(std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

class Session final : public std::enable_shared_from_this<Session> {
 public:
  using ClosedCallback = std::function<void()>;

  Session(tcp::socket socket,
          LayoutServerOptions options,
          LayoutServer::LayoutCallback layout_callback,
          LayoutServer::StatusCallback status_callback,
          ClosedCallback closed_callback)
      : stream_(std::move(socket)),
        options_(std::move(options)),
        layout_callback_(std::move(layout_callback)),
        status_callback_(std::move(status_callback)),
        closed_callback_(std::move(closed_callback)) {}

  void Run() {
    http::async_read(
        stream_.next_layer(), buffer_, request_,
        [self = shared_from_this()](beast::error_code error, std::size_t) {
          if (error) return self->Close(error);
          const auto requested_protocol =
              self->request_[http::field::sec_websocket_protocol];
          if (self->request_.target() != self->options_.path ||
              requested_protocol != "flexvision-layout.v1") {
            self->Emit(L"Rejected layout WebSocket with wrong path or subprotocol.");
            return self->Close(websocket::error::bad_data_frame);
          }
          self->stream_.set_option(websocket::stream_base::timeout::suggested(
              beast::role_type::server));
          websocket::permessage_deflate compression;
          compression.server_enable = false;
          compression.client_enable = false;
          self->stream_.set_option(compression);
          self->stream_.read_message_max(kMaximumLayoutMessageSize);
          self->stream_.text(true);
          self->stream_.set_option(websocket::stream_base::decorator(
              [](websocket::response_type& response) {
                response.set(http::field::sec_websocket_protocol,
                             "flexvision-layout.v1");
              }));
          self->stream_.async_accept(
              self->request_, [self](beast::error_code accept_error) {
                if (accept_error) return self->Close(accept_error);
                self->connected_ = true;
                self->Emit(L"Layout WebSocket client connected.");
                self->Queue(SerializeHello());
                self->Read();
              });
        });
  }

  void Stop() {
    beast::error_code ignored;
    stream_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    stream_.next_layer().close(ignored);
  }

 private:
  void Read() {
    stream_.async_read(
        buffer_, [self = shared_from_this()](beast::error_code error,
                                             std::size_t) {
          if (error) return self->Close(error);
          if (!self->stream_.got_text()) {
            self->Queue(SerializeLayoutError("text-required"));
            return self->Read();
          }
          const std::string text = beast::buffers_to_string(self->buffer_.data());
          self->buffer_.consume(self->buffer_.size());
          LayoutSnapshot snapshot;
          std::string parse_error;
          if (!ParseLayoutMessage(text, &snapshot, &parse_error)) {
            self->Emit(L"Rejected layout message: " + ToWide(parse_error));
            self->Queue(SerializeLayoutError("invalid-layout"));
            return self->Read();
          }
          if (!self->have_revision_ || snapshot.revision > self->last_revision_) {
            self->last_revision_ = snapshot.revision;
            self->have_revision_ = true;
            if (self->layout_callback_) {
              self->layout_callback_(std::move(snapshot));
            }
          }
          self->Queue(SerializeApplied(self->last_revision_));
          self->Read();
        });
  }

  void Queue(std::string text) {
    if (writes_.size() >= 32) {
      return Close(websocket::error::bad_data_frame);
    }
    const bool idle = writes_.empty();
    writes_.push_back(std::move(text));
    if (idle) Write();
  }

  void Write() {
    stream_.text(true);
    stream_.async_write(
        net::buffer(writes_.front()),
        [self = shared_from_this()](beast::error_code error, std::size_t) {
          if (error) return self->Close(error);
          self->writes_.pop_front();
          if (!self->writes_.empty()) self->Write();
        });
  }

  void Close(beast::error_code error) {
    if (closed_) return;
    closed_ = true;
    if (connected_ && error != websocket::error::closed &&
        error != net::error::operation_aborted) {
      Emit(L"Layout WebSocket disconnected: " + ToWide(error.message()));
    }
    beast::error_code ignored;
    stream_.next_layer().close(ignored);
    if (closed_callback_) closed_callback_();
  }

  void Emit(std::wstring message) const {
    if (status_callback_) status_callback_(std::move(message));
  }

  websocket::stream<tcp::socket> stream_;
  LayoutServerOptions options_;
  LayoutServer::LayoutCallback layout_callback_;
  LayoutServer::StatusCallback status_callback_;
  ClosedCallback closed_callback_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> request_;
  std::deque<std::string> writes_;
  std::uint64_t last_revision_ = 0;
  bool have_revision_ = false;
  bool connected_ = false;
  bool closed_ = false;
};

}  // namespace

class LayoutServer::Impl final {
 public:
  Impl(LayoutServerOptions options,
       LayoutCallback layout_callback,
       StatusCallback status_callback)
      : options_(std::move(options)),
        layout_callback_(std::move(layout_callback)),
        status_callback_(std::move(status_callback)) {}

  bool Start(std::wstring* error) {
    if (thread_.joinable()) {
      if (error != nullptr) *error = L"Layout server is already running";
      return false;
    }
    try {
      const auto address = net::ip::make_address(options_.bind_address);
      if (!address.is_loopback()) {
        if (error != nullptr) *error = L"Layout server only permits loopback";
        return false;
      }
      acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
      acceptor_->open(address.is_v6() ? tcp::v6() : tcp::v4());
      acceptor_->set_option(tcp::acceptor::reuse_address(true));
      acceptor_->bind({address, options_.port});
      acceptor_->listen(1);
      Accept();
      thread_ = std::thread([this] { ioc_.run(); });
      Emit(L"Layout server listening on " + ToWide(options_.bind_address) +
           L":" + std::to_wstring(options_.port) +
           ToWide(options_.path));
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
    auto future = stopped.get_future();
    net::post(ioc_, [this, &stopped] {
      if (session_) session_->Stop();
      beast::error_code ignored;
      if (acceptor_) acceptor_->close(ignored);
      stopped.set_value();
    });
    future.wait_for(std::chrono::seconds(2));
    ioc_.stop();
    thread_.join();
    session_.reset();
    acceptor_.reset();
  }

 private:
  void Accept() {
    acceptor_->async_accept([this](beast::error_code error, tcp::socket socket) {
      if (stopping_.load(std::memory_order_acquire)) return;
      if (!error) {
        socket.set_option(tcp::no_delay(true));
        if (session_) {
          beast::error_code ignored;
          socket.close(ignored);
          Emit(L"Rejected an additional layout WebSocket client.");
        } else {
          session_ = std::make_shared<Session>(
              std::move(socket), options_, layout_callback_, status_callback_,
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

  LayoutServerOptions options_;
  LayoutCallback layout_callback_;
  StatusCallback status_callback_;
  net::io_context ioc_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::shared_ptr<Session> session_;
  std::atomic_bool stopping_ = false;
  std::thread thread_;
};

LayoutServer::LayoutServer(LayoutServerOptions options,
                           LayoutCallback layout_callback,
                           StatusCallback status_callback)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(layout_callback),
                                   std::move(status_callback))) {}

LayoutServer::~LayoutServer() { Stop(); }

bool LayoutServer::Start(std::wstring* error) { return impl_->Start(error); }
void LayoutServer::Stop() { impl_->Stop(); }

}  // namespace streaming::compositor
