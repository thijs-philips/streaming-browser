#include "src/producer/stream_server.h"

#include <rpc.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "src/common/logging.h"
#include "src/common/pipe_io.h"
#include "src/common/pipe_security.h"
#include "src/common/win_handle.h"

namespace streaming::producer {
namespace {

constexpr std::size_t kMaximumOutgoingMessages = 128;

protocol::SessionId CreateSessionId() {
  UUID uuid{};
  protocol::SessionId result{};
  const RPC_STATUS status = UuidCreate(&uuid);
  if (status == RPC_S_OK || status == RPC_S_UUID_LOCAL_ONLY) {
    static_assert(sizeof(uuid) == result.size());
    std::memcpy(result.data(), &uuid, sizeof(uuid));
  }
  return result;
}

std::vector<std::byte> SerializeNavigationState(std::string url,
                                                bool loading,
                                                bool can_go_back,
                                                bool can_go_forward) {
  protocol::ByteWriter writer;
  writer.WriteU16(loading ? 1 : 0);
  writer.WriteU16(can_go_back ? 1 : 0);
  writer.WriteU16(can_go_forward ? 1 : 0);
  writer.WriteU16(0);
  writer.WriteUtf8(url);
  return writer.Take();
}

void CloseRemoteHandle(HANDLE process, HANDLE remote_handle) {
  HANDLE local_copy = nullptr;
  if (DuplicateHandle(process, remote_handle, GetCurrentProcess(), &local_copy,
                      0, FALSE,
                      DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
    CloseHandle(local_copy);
  }
}

}  // namespace

StreamServer::StreamServer(RingProvider ring_provider,
                           ReadyCallback ready_callback,
                           ReleaseCallback release_callback,
                           InputCallback input_callback,
                           ImeCallback ime_callback,
                           CommandCallback command_callback,
                           ViewportCallback viewport_callback,
                           DisconnectCallback disconnect_callback)
    : ring_provider_(std::move(ring_provider)),
      ready_callback_(std::move(ready_callback)),
      release_callback_(std::move(release_callback)),
      input_callback_(std::move(input_callback)),
      ime_callback_(std::move(ime_callback)),
      command_callback_(std::move(command_callback)),
      viewport_callback_(std::move(viewport_callback)),
      disconnect_callback_(std::move(disconnect_callback)),
      session_(CreateSessionId()) {
  ring_ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

StreamServer::~StreamServer() {
  Stop();
  if (ring_ready_event_ != nullptr) {
    CloseHandle(ring_ready_event_);
  }
}

bool StreamServer::Start() {
  if (ring_ready_event_ == nullptr || server_thread_.joinable()) {
    return false;
  }
  stopping_.store(false, std::memory_order_release);
  server_thread_ = std::thread(&StreamServer::ServerMain, this);
  return true;
}

void StreamServer::Stop() {
  stopping_.store(true, std::memory_order_release);
  if (HANDLE pipe = active_pipe_.load(std::memory_order_acquire)) {
    CancelIoEx(pipe, nullptr);
  }
  {
    std::lock_guard lock(queue_mutex_);
    session_running_ = false;
  }
  queue_changed_.notify_all();
  if (server_thread_.joinable()) {
    CancelSynchronousIo(server_thread_.native_handle());
  }
  if (writer_thread_.joinable()) {
    CancelSynchronousIo(writer_thread_.native_handle());
    writer_thread_.join();
  }
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
}

void StreamServer::NotifyRingReady() {
  SetEvent(ring_ready_event_);
}

bool StreamServer::PublishFrame(const protocol::FrameMetadata& metadata) {
  if (!connected_.load(std::memory_order_acquire)) {
    return true;
  }
  const bool queued = QueueMessage(protocol::MessageType::kFrameReady,
                                   protocol::SerializeFrameMetadata(metadata),
                                   true);
  if (!queued || metadata.frame_id == 1 || metadata.frame_id % 300 == 0) {
    Log(queued ? LogLevel::kInfo : LogLevel::kError,
        queued ? L"Queued shared frame for viewer"
               : L"Failed to queue critical shared frame");
  }
  return queued;
}

bool StreamServer::SendNavigationState(std::string url,
                                       bool loading,
                                       bool can_go_back,
                                       bool can_go_forward) {
  return QueueMessage(protocol::MessageType::kNavigationState,
                      SerializeNavigationState(std::move(url), loading,
                                               can_go_back, can_go_forward),
                      false);
}

bool StreamServer::SetViewerVisible(bool visible) {
  return QueueMessage(visible ? protocol::MessageType::kShowViewer
                              : protocol::MessageType::kHideViewer,
                      {}, false);
}

bool StreamServer::SendCursorState(std::uint32_t cursor_type) {
  protocol::ByteWriter writer;
  writer.WriteU32(cursor_type);
  return QueueMessage(protocol::MessageType::kCursorState, writer.Take(),
                      false);
}

bool StreamServer::ResetStream() {
  if (!connected_.load(std::memory_order_acquire)) {
    return true;
  }
  Log(LogLevel::kInfo,
      L"Requesting viewer stream reset for a new ring generation");
  return QueueMessage(protocol::MessageType::kStreamReset, {}, true);
}

void StreamServer::ServerMain() {
  PipeSecurity security;
  const std::wstring pipe_name = DiscoveryPipeName();
  if (pipe_name.empty() || !security.InitializeForCurrentLogon()) {
    Log(LogLevel::kError, L"Could not initialize streaming pipe security");
    return;
  }

  bool first_instance = true;
  while (!stopping_.load(std::memory_order_acquire)) {
    DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first_instance) {
      open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    UniqueHandle pipe(CreateNamedPipeW(
        pipe_name.c_str(), open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        2, 64 * 1024, 64 * 1024, 5000, security.attributes()));
    first_instance = false;
    if (!pipe) {
      LogLastError(LogLevel::kError, L"CreateNamedPipeW");
      return;
    }
    active_pipe_.store(pipe.get(), std::memory_order_release);

    UniqueHandle connect_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connect_operation{};
    connect_operation.hEvent = connect_event.get();
    bool connected = ConnectNamedPipe(pipe.get(), &connect_operation) != FALSE;
    if (!connected) {
      active_pipe_.store(nullptr, std::memory_order_release);
      const DWORD connect_error = GetLastError();
      if (connect_error == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (connect_error == ERROR_IO_PENDING &&
                 WaitForSingleObject(connect_event.get(), INFINITE) ==
                     WAIT_OBJECT_0) {
        DWORD ignored = 0;
        connected = GetOverlappedResult(pipe.get(), &connect_operation,
                                        &ignored, FALSE) != FALSE;
      }
    }
    if (!connected) {
      if (!stopping_.load(std::memory_order_acquire)) {
        LogLastError(LogLevel::kWarning, L"ConnectNamedPipe");
      }
      continue;
    }
    if (connected_.exchange(true, std::memory_order_acq_rel)) {
      protocol::MessageHeader reject;
      reject.type = protocol::MessageType::kReject;
      reject.session = session_;
      std::wstring ignored;
      WriteMessage(pipe.get(), reject, {}, &ignored);
      DisconnectNamedPipe(pipe.get());
      active_pipe_.store(nullptr, std::memory_order_release);
      continue;
    }
    session_ = CreateSessionId();

    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &client_pid) ||
        !PerformHandshake(pipe.get(), client_pid)) {
      Log(LogLevel::kWarning, L"Viewer handshake failed");
      EndSession();
      active_pipe_.store(nullptr, std::memory_order_release);
      DisconnectNamedPipe(pipe.get());
      continue;
    }

    {
      std::lock_guard lock(queue_mutex_);
      session_running_ = true;
    }
    writer_thread_ = std::thread(&StreamServer::WriterMain, this, pipe.get());
    if (ready_callback_) {
      Log(LogLevel::kInfo, L"Viewer accepted shared texture generation");
      ready_callback_();
    }
    ReaderMain(pipe.get());
    EndSession();
    active_pipe_.store(nullptr, std::memory_order_release);
    DisconnectNamedPipe(pipe.get());
    if (writer_thread_.joinable()) {
      CancelSynchronousIo(writer_thread_.native_handle());
      writer_thread_.join();
    }
  }
}

bool StreamServer::PerformHandshake(HANDLE pipe, DWORD client_process_id) {
  ReceivedMessage hello;
  std::wstring error;
  if (!ReadMessage(pipe, &hello, &error) ||
      hello.header.type != protocol::MessageType::kHello) {
    Log(LogLevel::kError,
        L"Handshake failed: expected Hello (" + error + L")");
    return false;
  }

  const DWORD ring_wait = WaitForSingleObject(ring_ready_event_, 15000);
  if (ring_wait != WAIT_OBJECT_0) {
    Log(LogLevel::kError,
        L"Handshake failed: shared texture ring was not ready within 15s");
    return false;
  }
  D3DFramePipeline::RingSnapshot snapshot;
  if (!ring_provider_ || !ring_provider_(&snapshot)) {
    Log(LogLevel::kError,
        L"Handshake failed: no ring snapshot available");
    return false;
  }
  generation_ = snapshot.generation;

  UniqueHandle client_process(OpenProcess(
      PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
      FALSE, client_process_id));
  if (!client_process) {
    LogLastError(LogLevel::kError, L"Handshake OpenProcess(viewer)");
    return false;
  }

  protocol::RingDefinition definition;
  definition.producer_process_id = GetCurrentProcessId();
  definition.adapter_luid_low = snapshot.adapter_luid.LowPart;
  definition.adapter_luid_high = snapshot.adapter_luid.HighPart;
  definition.width = snapshot.description.Width;
  definition.height = snapshot.description.Height;
  definition.dxgi_format = static_cast<std::uint32_t>(snapshot.description.Format);
  definition.slots.reserve(snapshot.handles.size());
  std::vector<HANDLE> remote_handles;
  for (HANDLE local_handle : snapshot.handles) {
    HANDLE remote_handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), local_handle, client_process.get(),
                         &remote_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      LogLastError(LogLevel::kError,
                   L"Handshake DuplicateHandle(shared texture)");
      for (HANDLE duplicated : remote_handles) {
        CloseRemoteHandle(client_process.get(), duplicated);
      }
      return false;
    }
    remote_handles.push_back(remote_handle);
    definition.slots.push_back(
        {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            remote_handle))});
  }

  protocol::MessageHeader ring_header;
  ring_header.type = protocol::MessageType::kRingDefinition;
  ring_header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
  ring_header.generation = generation_;
  ring_header.session = session_;
  const auto payload = protocol::SerializeRingDefinition(definition);
  if (!WriteMessage(pipe, ring_header, payload, &error)) {
    Log(LogLevel::kError,
        L"Handshake failed writing ring definition: " + error);
    for (HANDLE duplicated : remote_handles) {
      CloseRemoteHandle(client_process.get(), duplicated);
    }
    return false;
  }

  ReceivedMessage accepted;
    const bool success =
      ReadMessage(pipe, &accepted, &error) &&
      accepted.header.type == protocol::MessageType::kGenerationAccepted &&
      accepted.header.session == session_ &&
      accepted.header.generation == generation_;
    Log(success ? LogLevel::kInfo : LogLevel::kWarning,
      success ? L"Shared ring handshake completed"
          : L"Shared ring acceptance was invalid");
    return success;
}

void StreamServer::ReaderMain(HANDLE pipe) {
  std::uint64_t stale_messages = 0;
  while (!stopping_.load(std::memory_order_acquire)) {
    ReceivedMessage message;
    std::wstring error;
    if (!ReadMessage(pipe, &message, &error)) {
      if (!stopping_.load(std::memory_order_acquire)) {
        Log(LogLevel::kInfo,
            L"Viewer pipe read ended: " +
                (error.empty() ? L"connection closed" : error));
      }
      break;
    }
    if (message.header.session != session_ ||
        message.header.generation != generation_) {
      // Expected briefly around stream resets; log only the first per session.
      if (++stale_messages == 1) {
        Log(LogLevel::kWarning,
            L"Ignoring viewer message from a stale session/generation");
      }
      continue;
    }
    switch (message.header.type) {
      case protocol::MessageType::kFrameReleased: {
        protocol::FrameRelease release;
        std::string parse_error;
        if (protocol::ParseFrameRelease(message.payload, &release,
                                        &parse_error)) {
          if (release_callback_) {
            release_callback_(release.slot, release.frame_id);
          }
        } else {
          Log(LogLevel::kError, L"Rejected malformed FrameReleased message");
        }
        break;
      }
      case protocol::MessageType::kInputEvent: {
        protocol::InputEvent event;
        std::string parse_error;
        if (protocol::ParseInputEvent(message.payload, &event, &parse_error) &&
            input_callback_) {
          input_callback_(event);
        }
        break;
      }
      case protocol::MessageType::kImeEvent: {
        protocol::ImeEvent event;
        std::string parse_error;
        if (protocol::ParseImeEvent(message.payload, &event, &parse_error) &&
            ime_callback_) {
          ime_callback_(std::move(event));
        }
        break;
      }
      case protocol::MessageType::kNavigate: {
        protocol::ByteReader reader(message.payload);
        std::string url;
        if (reader.ReadUtf8(&url, 64 * 1024) && reader.empty() &&
            command_callback_) {
          Log(LogLevel::kInfo, L"Producer received URL navigation command");
          command_callback_(message.header.type, std::move(url));
        }
        break;
      }
      case protocol::MessageType::kBack:
      case protocol::MessageType::kForward:
      case protocol::MessageType::kReload:
      case protocol::MessageType::kStopLoad:
      case protocol::MessageType::kShowViewer:
      case protocol::MessageType::kHideViewer:
        if (command_callback_) {
          command_callback_(message.header.type, {});
        }
        break;
      case protocol::MessageType::kViewportSize: {
        protocol::ViewportSize size;
        std::string parse_error;
        if (protocol::ParseViewportSize(message.payload, &size,
                                        &parse_error)) {
          if (viewport_callback_) {
            viewport_callback_(size.width, size.height);
          }
        } else {
          Log(LogLevel::kError, L"Rejected malformed ViewportSize message");
        }
        break;
      }
      case protocol::MessageType::kPing:
        QueueMessage(protocol::MessageType::kPong, {}, false);
        break;
      case protocol::MessageType::kShutdown:
        return;
      default:
        break;
    }
  }
}

void StreamServer::WriterMain(HANDLE pipe) {
  Log(LogLevel::kInfo, L"Shared stream writer started");
  for (;;) {
    OutgoingMessage message;
    {
      std::unique_lock lock(queue_mutex_);
      queue_changed_.wait(lock, [this] {
        return !session_running_ || !outgoing_.empty() ||
               stopping_.load(std::memory_order_acquire);
      });
      if ((!session_running_ || stopping_.load(std::memory_order_acquire)) &&
          outgoing_.empty()) {
        return;
      }
      message = std::move(outgoing_.front());
      outgoing_.pop_front();
    }
    if (message.header.type == protocol::MessageType::kFrameReady) {
      // Frame transitions are never coalesced or dropped after publication.
    }
    std::wstring error;
    if (!WriteMessage(pipe, message.header, message.payload, &error)) {
      Log(LogLevel::kError, L"Shared stream writer failed");
      CancelIoEx(pipe, nullptr);
      return;
    }
    if (message.header.type == protocol::MessageType::kFrameReady &&
        message.header.sequence % 300 == 0) {
      Log(LogLevel::kInfo, L"Wrote FrameReady to viewer pipe");
    }
  }
}

bool StreamServer::QueueMessage(protocol::MessageType type,
                                std::vector<std::byte> payload,
                                bool critical) {
  if (!connected_.load(std::memory_order_acquire)) {
    return !critical;
  }
  OutgoingMessage message;
  message.header.type = type;
  message.header.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
  message.header.generation = generation_;
  message.header.session = session_;
  message.payload = std::move(payload);

  {
    std::lock_guard lock(queue_mutex_);
    if (!session_running_) {
      return !critical;
    }
    if (outgoing_.size() >= kMaximumOutgoingMessages) {
      if (critical) {
        return false;
      }
      for (auto iterator = outgoing_.begin(); iterator != outgoing_.end();
           ++iterator) {
        if (!protocol::IsCritical(iterator->header.type)) {
          outgoing_.erase(iterator);
          break;
        }
      }
      if (outgoing_.size() >= kMaximumOutgoingMessages) {
        return true;
      }
    }
    outgoing_.push_back(std::move(message));
  }
  queue_changed_.notify_one();
  return true;
}

void StreamServer::EndSession() {
  connected_.store(false, std::memory_order_release);
  ResetEvent(ring_ready_event_);
  {
    std::lock_guard lock(queue_mutex_);
    session_running_ = false;
    outgoing_.clear();
  }
  queue_changed_.notify_all();
  if (disconnect_callback_) {
    disconnect_callback_();
  }
}

}  // namespace streaming::producer
