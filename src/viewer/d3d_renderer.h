#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "src/common/protocol.h"

namespace streaming::viewer {

class D3DRenderer final {
 public:
  bool Initialize(HWND window);
  bool OpenRing(const protocol::RingDefinition& definition);
  bool ConsumeFrame(const protocol::FrameMetadata& metadata);
  bool Resize(unsigned width, unsigned height);
  void Render();
  void SetContentTop(unsigned pixels) { content_top_ = pixels; }
  void SetPixelPerfect(bool enabled);
  void Pan(float delta_x, float delta_y);
  [[nodiscard]] bool pixel_perfect() const { return pixel_perfect_; }

  [[nodiscard]] unsigned source_width() const { return source_width_; }
  [[nodiscard]] unsigned source_height() const { return source_height_; }
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
  bool CreateLocalFrame(const D3D11_TEXTURE2D_DESC& shared_description);
  bool WaitForCopy();
  void ResetDevice();
  D3D11_VIEWPORT FrameViewport() const;

  HWND window_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Device1> device1_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> checker_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> frame_shader_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> local_frame_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> local_frame_view_;
  Microsoft::WRL::ComPtr<ID3D11Query> copy_completion_;
  std::vector<SharedSlot> shared_slots_;
  unsigned back_buffer_width_ = 0;
  unsigned back_buffer_height_ = 0;
  unsigned content_top_ = 0;
  unsigned source_width_ = 0;
  unsigned source_height_ = 0;
  bool has_frame_ = false;
  bool pixel_perfect_ = false;
  float pan_x_ = 0.0F;
  float pan_y_ = 0.0F;
};

}  // namespace streaming::viewer
