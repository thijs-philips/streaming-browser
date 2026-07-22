#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "src/common/protocol.h"
#include "src/producer/d3d_frame_pipeline.h"

namespace streaming::producer {

class StreamServer final {
 public:
  using RingProvider =
      std::function<bool(D3DFramePipeline::RingSnapshot* snapshot)>;
  using ReadyCallback = std::function<void()>;
  using ReleaseCallback =
      std::function<void(std::uint32_t slot, std::uint64_t frame_id)>;
  using InputCallback = std::function<void(const protocol::InputEvent& event)>;
  using ImeCallback = std::function<void(protocol::ImeEvent event)>;
  using CommandCallback = std::function<void(protocol::MessageType type,
                                             std::string value)>;
  using ViewportCallback =
      std::function<void(std::uint32_t width, std::uint32_t height)>;
  using DisconnectCallback = std::function<void()>;

  StreamServer(RingProvider ring_provider,
               ReadyCallback ready_callback,
               ReleaseCallback release_callback,
               InputCallback input_callback,
               ImeCallback ime_callback,
               CommandCallback command_callback,
               ViewportCallback viewport_callback,
               DisconnectCallback disconnect_callback);
  ~StreamServer();

  StreamServer(const StreamServer&) = delete;
  StreamServer& operator=(const StreamServer&) = delete;

  bool Start();
  void Stop();
  void NotifyRingReady();
  bool PublishFrame(const protocol::FrameMetadata& metadata);
  bool SendNavigationState(std::string url,
                           bool loading,
                           bool can_go_back,
                           bool can_go_forward);
  bool SetViewerVisible(bool visible);
  bool SendCursorState(std::uint32_t cursor_type);
  // Asks the connected viewer to drop the session and re-handshake so it
  // picks up a regenerated ring (for example after a viewport resize).
  bool ResetStream();

  [[nodiscard]] bool connected() const {
    return connected_.load(std::memory_order_acquire);
  }

 private:
  struct OutgoingMessage {
    protocol::MessageHeader header;
    std::vector<std::byte> payload;
  };

  void ServerMain();
  bool PerformHandshake(HANDLE pipe, DWORD client_process_id);
  void ReaderMain(HANDLE pipe);
  void WriterMain(HANDLE pipe);
  bool QueueMessage(protocol::MessageType type,
                    std::vector<std::byte> payload,
                    bool critical);
  void EndSession();

  RingProvider ring_provider_;
  ReadyCallback ready_callback_;
  ReleaseCallback release_callback_;
  InputCallback input_callback_;
  ImeCallback ime_callback_;
  CommandCallback command_callback_;
  ViewportCallback viewport_callback_;
  DisconnectCallback disconnect_callback_;

  std::atomic_bool stopping_ = false;
  std::atomic_bool connected_ = false;
  std::atomic<HANDLE> active_pipe_ = nullptr;
  std::atomic_uint64_t sequence_ = 1;
  std::thread server_thread_;
  std::thread writer_thread_;

  HANDLE ring_ready_event_ = nullptr;
  protocol::SessionId session_{};
  std::uint64_t generation_ = 0;

  std::mutex queue_mutex_;
  std::condition_variable queue_changed_;
  std::deque<OutgoingMessage> outgoing_;
  bool session_running_ = false;
};

}  // namespace streaming::producer
