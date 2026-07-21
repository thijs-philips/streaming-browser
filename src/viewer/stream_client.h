#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "src/common/protocol.h"
#include "src/common/win_handle.h"

namespace streaming::viewer {

struct NavigationState {
  std::string url;
  bool loading = false;
  bool can_go_back = false;
  bool can_go_forward = false;
};

class StreamClient final {
 public:
  using RingCallback = std::function<void(protocol::RingDefinition definition)>;
  using FrameCallback = std::function<void(protocol::FrameMetadata metadata)>;
  using NavigationCallback = std::function<void(NavigationState state)>;
  using StatusCallback = std::function<void(bool connected)>;
  using VisibilityCallback = std::function<void(bool visible)>;
  using CursorCallback = std::function<void(std::uint32_t cursor_type)>;

  StreamClient(RingCallback ring_callback,
               FrameCallback frame_callback,
               NavigationCallback navigation_callback,
               StatusCallback status_callback,
               VisibilityCallback visibility_callback,
               CursorCallback cursor_callback);
  ~StreamClient();

  StreamClient(const StreamClient&) = delete;
  StreamClient& operator=(const StreamClient&) = delete;

  bool Start();
  void Stop();
  void AcceptRing(bool accepted);
  bool ReleaseFrame(std::uint32_t slot, std::uint64_t frame_id);
  bool SendInput(const protocol::InputEvent& event);
  bool SendIme(const protocol::ImeEvent& event);
  bool SendCommand(protocol::MessageType type, std::string value = {});

 private:
  void ClientMain();
  bool ConnectAndRun();
  bool Send(protocol::MessageType type, std::vector<std::byte> payload);

  RingCallback ring_callback_;
  FrameCallback frame_callback_;
  NavigationCallback navigation_callback_;
  StatusCallback status_callback_;
  VisibilityCallback visibility_callback_;
  CursorCallback cursor_callback_;

  std::atomic_bool stopping_ = false;
  std::thread client_thread_;
  std::mutex write_mutex_;
  UniqueHandle pipe_;
  protocol::SessionId session_{};
  std::uint64_t generation_ = 0;
  std::atomic_uint64_t sequence_ = 1;

  std::mutex acceptance_mutex_;
  std::condition_variable acceptance_changed_;
  bool acceptance_received_ = false;
  bool ring_accepted_ = false;
};

}  // namespace streaming::viewer
