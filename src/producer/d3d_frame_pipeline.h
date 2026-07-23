#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "include/cef_render_handler.h"
#include "src/common/protocol.h"
#include "src/common/win_handle.h"

namespace streaming::producer {

class D3DFramePipeline final {
 public:
  static constexpr std::size_t kCaptureSlots = 4;
  static constexpr std::size_t kOutputSlots = 4;

  struct RingSnapshot {
    LUID adapter_luid{};
    D3D11_TEXTURE2D_DESC description{};
    std::uint64_t generation = 0;
    std::array<HANDLE, kOutputSlots> handles{};
  };

  using PublishCallback =
      std::function<bool(const protocol::FrameMetadata& metadata)>;
  using RingReadyCallback = std::function<void()>;

  D3DFramePipeline(PublishCallback publish_callback,
                   RingReadyCallback ring_ready_callback,
                   bool alpha_probe_enabled);
  ~D3DFramePipeline() = default;

  D3DFramePipeline(const D3DFramePipeline&) = delete;
  D3DFramePipeline& operator=(const D3DFramePipeline&) = delete;

  bool CopyFromCef(CefRenderHandler::PaintElementType type,
                   const CefRenderHandler::RectList& dirty_rects,
                   const CefAcceleratedPaintInfo& info);
  void SetPopupVisible(bool visible);
  void SetPopupBounds(const CefRect& bounds);
  void SetStreamingEnabled(bool enabled);
  void ReleaseOutputSlot(std::uint32_t slot, std::uint64_t frame_id);
  [[nodiscard]] bool GetRingSnapshot(RingSnapshot* snapshot) const;

  [[nodiscard]] bool initialized() const { return device_ != nullptr; }
  [[nodiscard]] std::uint64_t frame_id() const { return frame_id_; }
  [[nodiscard]] LUID adapter_luid() const { return adapter_luid_; }
  [[nodiscard]] const D3D11_TEXTURE2D_DESC& view_description() const {
    return view_desc_;
  }
  [[nodiscard]] std::uint32_t content_width() const { return content_width_; }
  [[nodiscard]] std::uint32_t content_height() const {
    return content_height_;
  }

 private:
  struct CaptureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    std::uint64_t frame_id = 0;
  };

  enum class OutputState { kAvailable, kPublished };

  struct OutputSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    UniqueHandle shared_handle;
    OutputState state = OutputState::kAvailable;
    std::uint64_t frame_id = 0;
  };

  bool DiscoverDeviceAndOpen(HANDLE handle,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D>* source);
  bool CreateViewRing(const D3D11_TEXTURE2D_DESC& source_desc);
  bool CreatePopupTarget(const D3D11_TEXTURE2D_DESC& source_desc);
  bool CreateOutputRing(const D3D11_TEXTURE2D_DESC& source_desc);
  bool CreateCompositorResources();
  bool PublishLatest();
  bool CompositePopup(ID3D11Texture2D* destination);
  bool ProbeAlpha(ID3D11Texture2D* texture);
  bool WaitForCopy(ID3D11Query* completion);
  [[noreturn]] void FailFastGpu(const wchar_t* reason, HRESULT result) const;

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Device1> device1_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  LUID adapter_luid_{};

  std::array<CaptureSlot, kCaptureSlots> view_slots_{};
  CaptureSlot popup_slot_{};
  std::array<OutputSlot, kOutputSlots> output_slots_{};
  D3D11_TEXTURE2D_DESC view_desc_{};
  D3D11_TEXTURE2D_DESC popup_desc_{};
  std::uint32_t content_width_ = 0;
  std::uint32_t content_height_ = 0;
  std::size_t next_view_slot_ = 0;
  std::size_t latest_view_slot_ = 0;
  std::uint64_t frame_id_ = 0;
  std::uint64_t published_frame_id_ = 0;
  std::uint64_t generation_ = 1;
  bool streaming_enabled_ = false;
  bool popup_visible_ = false;
  bool alpha_probe_enabled_ = false;
  bool alpha_probe_completed_ = false;
  CefRect popup_bounds_{};
  protocol::FrameMetadata latest_metadata_{};

  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> popup_srv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> popup_vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> popup_pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> popup_constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> popup_sampler_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> premultiplied_blend_;

  PublishCallback publish_callback_;
  RingReadyCallback ring_ready_callback_;
  mutable std::mutex ring_mutex_;
};

}  // namespace streaming::producer
