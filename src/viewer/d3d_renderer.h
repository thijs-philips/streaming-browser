#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace streaming::viewer {

class D3DRenderer final {
 public:
  bool Initialize(HWND window);
  bool Resize(unsigned width, unsigned height);
  void RenderDisconnected();

 private:
  bool CreateBackBuffer();
  bool CreateShaders();

  HWND window_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
};

}  // namespace streaming::viewer
