#include "src/viewer/d3d_renderer.h"

#include <d3dcompiler.h>

#include <array>
#include <cstring>

#include "src/common/logging.h"

namespace streaming::viewer {
namespace {

constexpr char kVertexShader[] = R"HLSL(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
  Output output;
  float2 position = float2((id << 1) & 2, id & 2);
  output.uv = position;
  output.position = float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return output;
}
)HLSL";

constexpr char kPixelShader[] = R"HLSL(
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  uint2 cell = uint2(position.xy) / 16;
  float shade = ((cell.x + cell.y) & 1) ? 0.72 : 0.48;
  return float4(shade, shade, shade, 1.0);
}
)HLSL";

bool Compile(const char* source,
             const char* profile,
             ID3DBlob** output) {
  Microsoft::WRL::ComPtr<ID3DBlob> error;
  const HRESULT result = D3DCompile(
      source, strlen(source), nullptr, nullptr, nullptr, "main", profile,
      D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &error);
  if (FAILED(result) && error) {
    OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
  }
  return SUCCEEDED(result);
}

}  // namespace

bool D3DRenderer::Initialize(HWND window) {
  window_ = window;
  constexpr std::array<D3D_FEATURE_LEVEL, 2> levels = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL selected{};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(),
      static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device_, &selected,
      &context_);
#if defined(_DEBUG)
  if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device_, &selected,
        &context_);
  }
#endif
  if (FAILED(result)) {
    Log(LogLevel::kError, L"Viewer D3D11 device creation failed");
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  if (FAILED(device_.As(&dxgi_device)) ||
      FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    return false;
  }

  RECT rect{};
  GetClientRect(window_, &rect);
  DXGI_SWAP_CHAIN_DESC1 description{};
  description.Width = static_cast<UINT>(rect.right - rect.left);
  description.Height = static_cast<UINT>(rect.bottom - rect.top);
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = 2;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  description.Scaling = DXGI_SCALING_STRETCH;

  result = factory->CreateSwapChainForHwnd(device_.Get(), window_, &description,
                                            nullptr, nullptr, &swap_chain_);
  if (FAILED(result)) {
    return false;
  }
  factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
  return CreateBackBuffer() && CreateShaders();
}

bool D3DRenderer::Resize(unsigned width, unsigned height) {
  if (!swap_chain_ || width == 0 || height == 0) {
    return false;
  }
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  render_target_.Reset();
  const HRESULT result = swap_chain_->ResizeBuffers(
      0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  return SUCCEEDED(result) && CreateBackBuffer();
}

void D3DRenderer::RenderDisconnected() {
  if (!render_target_) {
    return;
  }
  context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context_->Draw(3, 0);
  swap_chain_->Present(1, 0);
}

bool D3DRenderer::CreateBackBuffer() {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
      FAILED(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                              &render_target_))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC description{};
  back_buffer->GetDesc(&description);
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(description.Width);
  viewport.Height = static_cast<float>(description.Height);
  viewport.MinDepth = 0.0F;
  viewport.MaxDepth = 1.0F;
  context_->RSSetViewports(1, &viewport);
  return true;
}

bool D3DRenderer::CreateShaders() {
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
  if (!Compile(kVertexShader, "vs_5_0", &vertex_blob) ||
      !Compile(kPixelShader, "ps_5_0", &pixel_blob)) {
    return false;
  }
  return SUCCEEDED(device_->CreateVertexShader(
             vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
             nullptr, &vertex_shader_)) &&
         SUCCEEDED(device_->CreatePixelShader(
             pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
             nullptr, &pixel_shader_));
}

}  // namespace streaming::viewer
