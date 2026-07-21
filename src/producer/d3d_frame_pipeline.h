#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "include/cef_render_handler.h"

namespace streaming::producer {

class D3DFramePipeline final {
 public:
  static constexpr std::size_t kCaptureSlots = 4;

  D3DFramePipeline();
  ~D3DFramePipeline() = default;

  D3DFramePipeline(const D3DFramePipeline&) = delete;
  D3DFramePipeline& operator=(const D3DFramePipeline&) = delete;

  bool CopyFromCef(CefRenderHandler::PaintElementType type,
                   const CefAcceleratedPaintInfo& info);
  void SetPopupVisible(bool visible) { popup_visible_ = visible; }
  void SetPopupBounds(const CefRect& bounds) { popup_bounds_ = bounds; }

  [[nodiscard]] bool initialized() const { return device_ != nullptr; }
  [[nodiscard]] std::uint64_t frame_id() const { return frame_id_; }
  [[nodiscard]] LUID adapter_luid() const { return adapter_luid_; }
  [[nodiscard]] const D3D11_TEXTURE2D_DESC& view_description() const {
    return view_desc_;
  }

 private:
  struct CaptureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    std::uint64_t frame_id = 0;
  };

  bool DiscoverDeviceAndOpen(HANDLE handle,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D>* source);
  bool CreateViewRing(const D3D11_TEXTURE2D_DESC& source_desc);
  bool CreatePopupTarget(const D3D11_TEXTURE2D_DESC& source_desc);
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
  D3D11_TEXTURE2D_DESC view_desc_{};
  D3D11_TEXTURE2D_DESC popup_desc_{};
  std::size_t next_view_slot_ = 0;
  std::uint64_t frame_id_ = 0;
  bool popup_visible_ = false;
  CefRect popup_bounds_{};
};

}  // namespace streaming::producer
