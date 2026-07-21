#include "src/viewer/stream_client.h"

#include <chrono>
#include <utility>

#include "src/common/logging.h"
#include "src/common/pipe_io.h"
#include "src/common/pipe_security.h"

namespace streaming::viewer {

StreamClient::StreamClient(RingCallback ring_callback,
                           FrameCallback frame_callback,
                           NavigationCallback navigation_callback,
                           StatusCallback status_callback,
                           VisibilityCallback visibility_callback)
    : ring_callback_(std::move(ring_callback)),
      frame_callback_(std::move(frame_callback)),
      navigation_callback_(std::move(navigation_callback)),
      status_callback_(std::move(status_callback)),
      visibility_callback_(std::move(visibility_callback)) {}

StreamClient::~StreamClient() {
  Stop();
}

bool StreamClient::Start() {
  if (client_thread_.joinable()) {
    return false;
  }
  stopping_.store(false, std::memory_order_release);
  client_thread_ = std::thread(&StreamClient::ClientMain, this);
  return true;
}

void StreamClient::Stop() {
  stopping_.store(true, std::memory_order_release);
  acceptance_changed_.notify_all();
  {
    std::lock_guard lock(write_mutex_);
    if (pipe_) {
      CancelIoEx(pipe_.get(), nullptr);
    }
  }
  if (client_thread_.joinable()) {
    CancelSynchronousIo(client_thread_.native_handle());
    client_thread_.join();
  }
  std::lock_guard lock(write_mutex_);
  pipe_.reset();
}

void StreamClient::AcceptRing(bool accepted) {
  {
    std::lock_guard lock(acceptance_mutex_);
    acceptance_received_ = true;
    ring_accepted_ = accepted;
  }
  acceptance_changed_.notify_one();
}

bool StreamClient::ReleaseFrame(std::uint32_t slot, std::uint64_t frame_id) {
  return Send(protocol::MessageType::kFrameReleased,
              protocol::SerializeFrameRelease({frame_id, slot}));
}

bool StreamClient::SendInput(const protocol::InputEvent& event) {
  return Send(protocol::MessageType::kInputEvent,
              protocol::SerializeInputEvent(event));
}

bool StreamClient::SendCommand(protocol::MessageType type, std::string value) {
  protocol::ByteWriter writer;
  if (type == protocol::MessageType::kNavigate) {
    writer.WriteUtf8(value);
  }
  return Send(type, writer.Take());
}

void StreamClient::ClientMain() {
  while (!stopping_.load(std::memory_order_acquire)) {
    if (!ConnectAndRun() && !stopping_.load(std::memory_order_acquire)) {
      if (status_callback_) {
        status_callback_(false);
      }
      for (int i = 0; i < 10 && !stopping_.load(std::memory_order_acquire);
           ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }
}

bool StreamClient::ConnectAndRun() {
  const std::wstring pipe_name = DiscoveryPipeName();
  if (pipe_name.empty()) {
    return false;
  }
  if (!WaitNamedPipeW(pipe_name.c_str(), 1000)) {
    return false;
  }

  UniqueHandle pipe(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr));
  if (!pipe) {
    return false;
  }
  {
    std::lock_guard lock(write_mutex_);
    pipe_.reset(pipe.release());
  }

  protocol::MessageHeader hello;
  hello.type = protocol::MessageType::kHello;
  std::wstring error;
  {
    std::lock_guard lock(write_mutex_);
    if (!WriteMessage(pipe_.get(), hello, {}, &error)) {
      pipe_.reset();
      return false;
    }
  }

  ReceivedMessage ring_message;
  if (!ReadMessage(pipe_.get(), &ring_message, &error) ||
      ring_message.header.type != protocol::MessageType::kRingDefinition) {
    std::lock_guard lock(write_mutex_);
    pipe_.reset();
    return false;
  }
  protocol::RingDefinition definition;
  std::string parse_error;
  if (!protocol::ParseRingDefinition(ring_message.payload, &definition,
                                     &parse_error)) {
    std::lock_guard lock(write_mutex_);
    pipe_.reset();
    return false;
  }
  session_ = ring_message.header.session;
  generation_ = ring_message.header.generation;
  {
    std::lock_guard lock(acceptance_mutex_);
    acceptance_received_ = false;
    ring_accepted_ = false;
  }
  if (ring_callback_) {
    ring_callback_(std::move(definition));
  }

  {
    std::unique_lock lock(acceptance_mutex_);
    if (!acceptance_changed_.wait_for(lock, std::chrono::seconds(10), [this] {
          return acceptance_received_ ||
                 stopping_.load(std::memory_order_acquire);
        }) ||
        !ring_accepted_) {
      std::lock_guard write_lock(write_mutex_);
      pipe_.reset();
      return false;
    }
  }

  if (!Send(protocol::MessageType::kGenerationAccepted, {})) {
    return false;
  }
  if (status_callback_) {
    status_callback_(true);
  }

  while (!stopping_.load(std::memory_order_acquire)) {
    ReceivedMessage message;
    if (!ReadMessage(pipe_.get(), &message, &error)) {
      break;
    }
    if (message.header.session != session_ ||
        message.header.generation != generation_) {
      continue;
    }
    switch (message.header.type) {
      case protocol::MessageType::kFrameReady: {
        protocol::FrameMetadata metadata;
        if (protocol::ParseFrameMetadata(message.payload, &metadata,
                                         &parse_error) &&
            frame_callback_) {
          if (metadata.frame_id == 1 || metadata.frame_id % 300 == 0) {
            Log(LogLevel::kInfo, L"Received shared frame metadata");
          }
          frame_callback_(std::move(metadata));
        }
        break;
      }
      case protocol::MessageType::kNavigationState: {
        protocol::ByteReader reader(message.payload);
        std::uint16_t loading = 0;
        std::uint16_t back = 0;
        std::uint16_t forward = 0;
        std::uint16_t reserved = 0;
        NavigationState state;
        if (reader.ReadU16(&loading) && reader.ReadU16(&back) &&
            reader.ReadU16(&forward) && reader.ReadU16(&reserved) &&
            reader.ReadUtf8(&state.url, 64 * 1024) && reader.empty()) {
          state.loading = loading != 0;
          state.can_go_back = back != 0;
          state.can_go_forward = forward != 0;
          if (navigation_callback_) {
            navigation_callback_(std::move(state));
          }
        }
        break;
      }
      case protocol::MessageType::kPing:
        Send(protocol::MessageType::kPong, {});
        break;
      case protocol::MessageType::kShowViewer:
      case protocol::MessageType::kHideViewer:
        if (visibility_callback_) {
          visibility_callback_(message.header.type ==
                               protocol::MessageType::kShowViewer);
        }
        break;
      case protocol::MessageType::kStreamReset:
      case protocol::MessageType::kShutdown:
      case protocol::MessageType::kError:
        goto disconnected;
      default:
        break;
    }
  }

disconnected:
  if (status_callback_) {
    status_callback_(false);
  }
  std::lock_guard lock(write_mutex_);
  pipe_.reset();
  return true;
}

bool StreamClient::Send(protocol::MessageType type,
                        std::vector<std::byte> payload) {
  std::lock_guard lock(write_mutex_);
  if (!pipe_) {
    return false;
  }
  protocol::MessageHeader header;
  header.type = type;
  header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
  header.generation = generation_;
  header.session = session_;
  std::wstring error;
  return WriteMessage(pipe_.get(), header, payload, &error);
}

}  // namespace streaming::viewer
