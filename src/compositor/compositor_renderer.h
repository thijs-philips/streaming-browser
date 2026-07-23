#pragma once

#include "src/common/protocol.h"
#include "src/compositor/layout_protocol.h"

#include <d2d1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vector>

namespace streaming::compositor {

class CompositorRenderer final {
 public:
  bool Initialize(HWND window,
                  std::uint32_t output_width,
                  std::uint32_t output_height);
  bool OpenRing(const protocol::RingDefinition& definition);
  bool ConsumeFrame(const protocol::FrameMetadata& metadata);
  bool Resize(unsigned width, unsigned height);
  void SetServerScaling(bool enabled) { server_scaling_ = enabled; }
  void SetLayout(LayoutSnapshot snapshot);
  void Render();
  [[nodiscard]] std::uint32_t source_width() const { return source_width_; }
  [[nodiscard]] std::uint32_t source_height() const { return source_height_; }
  bool WindowToSource(int window_x, int window_y, int* source_x,
                      int* source_y) const;

 private:
  struct SharedSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
  };

  bool CreateDevice(const LUID* required_luid);
  bool CreateSwapChain();
  bool CreateBackBuffer();
  bool CreateShaders();
  bool CreateTextResources();
  bool CreateLocalFrame(const D3D11_TEXTURE2D_DESC& shared_description);
  bool WaitForCopy();
  void ResetDevice();
  D3D11_VIEWPORT ContentViewport() const;
  void DrawBlocks(const D3D11_VIEWPORT& viewport);
  void DrawLabels(const D3D11_VIEWPORT& viewport);

  HWND window_ = nullptr;
  std::uint32_t output_width_ = 0;
  std::uint32_t output_height_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Device1> device1_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> solid_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> overlay_shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> local_frame_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> local_frame_view_;
  Microsoft::WRL::ComPtr<ID3D11Query> copy_completion_;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> d2d_target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
  std::vector<SharedSlot> shared_slots_;
  LayoutSnapshot layout_;
  unsigned back_buffer_width_ = 0;
  unsigned back_buffer_height_ = 0;
  unsigned source_width_ = 0;
  unsigned source_height_ = 0;
  unsigned frame_width_ = 0;
  unsigned frame_height_ = 0;
  bool has_frame_ = false;
  bool server_scaling_ = false;
};

}  // namespace streaming::compositor
