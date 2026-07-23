#include "src/compositor/compositor_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include "src/common/logging.h"

namespace streaming::compositor {
namespace {

constexpr char kVertexShader[] = R"HLSL(
cbuffer Constants : register(b0) { float4 rect; float4 color; float4 uv_rect; }
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
  Output output;
  float2 corner = float2(id & 1, (id >> 1) & 1);
  float2 position = rect.xy + corner * rect.zw;
  output.position = float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  output.uv = uv_rect.xy + corner * uv_rect.zw;
  return output;
}
)HLSL";

constexpr char kSolidShader[] = R"HLSL(
cbuffer Constants : register(b0) { float4 rect; float4 color; float4 uv_rect; }
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return color;
}
)HLSL";

constexpr char kOverlayShader[] = R"HLSL(
Texture2D overlay_texture : register(t0);
SamplerState overlay_sampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return overlay_texture.Sample(overlay_sampler, uv);
}
)HLSL";

struct Constants {
  float rect[4];
  float color[4];
  float uv_rect[4];
};

bool Compile(const char* source, const char* profile, ID3DBlob** output) {
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT result = D3DCompile(
      source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
      D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &errors);
  if (FAILED(result) && errors) {
    const std::string text(
        static_cast<const char*>(errors->GetBufferPointer()),
        errors->GetBufferSize());
    Log(LogLevel::kError,
      L"Shader compiler: " + std::wstring(text.begin(), text.end()));
  }
  return SUCCEEDED(result);
}

D2D1_COLOR_F SourceColor(std::string_view source) {
  (void)source;
  return D2D1::ColorF(0.94F, 0.88F, 0.62F, 1.0F);
}

std::wstring ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), count);
  return result;
}

}  // namespace

bool CompositorRenderer::Initialize(HWND window,
                                    std::uint32_t output_width,
                                    std::uint32_t output_height) {
  window_ = window;
  output_width_ = output_width;
  output_height_ = output_height;
  if (!CreateDevice(nullptr)) {
    Log(LogLevel::kError, L"Compositor failed to create the D3D11 device");
    return false;
  }
  if (!CreateSwapChain()) {
    Log(LogLevel::kError, L"Compositor failed to create the swap chain");
    return false;
  }
  if (!CreateBackBuffer()) {
    Log(LogLevel::kError, L"Compositor failed to create back-buffer/text resources");
    return false;
  }
  if (!CreateShaders()) {
    Log(LogLevel::kError, L"Compositor failed to create shaders");
    return false;
  }
  return true;
}

bool CompositorRenderer::OpenRing(const protocol::RingDefinition& definition) {
  if (definition.width == 0 || definition.height == 0 ||
      definition.alpha_mode != 1 || definition.slots.empty()) {
    Log(LogLevel::kError,
        L"Compositor rejected overlay ring with invalid dimensions or alpha mode");
    for (const auto& slot : definition.slots) {
      CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(slot.handle)));
    }
    return false;
  }
  const LUID luid{definition.adapter_luid_low, definition.adapter_luid_high};
  ResetDevice();
  if (!CreateDevice(&luid) || !CreateSwapChain() || !CreateBackBuffer() ||
      !CreateShaders()) {
    return false;
  }

  std::vector<SharedSlot> slots;
  D3D11_TEXTURE2D_DESC description{};
  for (std::size_t index = 0; index < definition.slots.size(); ++index) {
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
        definition.slots[index].handle));
    SharedSlot slot;
    const HRESULT result = device1_->OpenSharedResource1(
        handle, IID_PPV_ARGS(&slot.texture));
    CloseHandle(handle);
    if (FAILED(result) || FAILED(slot.texture.As(&slot.keyed_mutex))) {
      for (std::size_t remaining = index + 1;
           remaining < definition.slots.size(); ++remaining) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
            definition.slots[remaining].handle)));
      }
      return false;
    }
    D3D11_TEXTURE2D_DESC current{};
    slot.texture->GetDesc(&current);
    if (index == 0) description = current;
    if (current.Width != definition.width ||
        current.Height != definition.height ||
        current.Format != static_cast<DXGI_FORMAT>(definition.dxgi_format)) {
      return false;
    }
    slots.push_back(std::move(slot));
  }
  if (!CreateLocalFrame(description)) return false;
  shared_slots_ = std::move(slots);
  source_width_ = definition.width;
  source_height_ = definition.height;
  frame_width_ = definition.width;
  frame_height_ = definition.height;
  has_frame_ = false;
  return true;
}

bool CompositorRenderer::ConsumeFrame(const protocol::FrameMetadata& metadata) {
  if (metadata.slot >= shared_slots_.size() || !local_frame_) return false;
  auto& slot = shared_slots_[metadata.slot];
  const HRESULT acquired = slot.keyed_mutex->AcquireSync(1, 100);
  if (acquired != S_OK) return false;
  context_->CopyResource(local_frame_.Get(), slot.texture.Get());
  context_->End(copy_completion_.Get());
  const bool copied = WaitForCopy();
  const HRESULT released = slot.keyed_mutex->ReleaseSync(0);
  if (!copied || released != S_OK) return false;
  if (metadata.width != 0 && metadata.height != 0 &&
      metadata.width <= source_width_ && metadata.height <= source_height_) {
    if (metadata.width != frame_width_ || metadata.height != frame_height_) {
      Log(LogLevel::kInfo,
          L"Compositor received CEF frame " +
              std::to_wstring(metadata.width) + L"x" +
              std::to_wstring(metadata.height) + L" for client " +
              std::to_wstring(back_buffer_width_) + L"x" +
              std::to_wstring(back_buffer_height_));
    }
    frame_width_ = metadata.width;
    frame_height_ = metadata.height;
  }
  has_frame_ = true;
  return true;
}

bool CompositorRenderer::Resize(unsigned width, unsigned height) {
  if (!swap_chain_ || width == 0 || height == 0) return false;
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  d2d_target_.Reset();
  text_brush_.Reset();
  render_target_.Reset();
  const HRESULT result =
      swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  return SUCCEEDED(result) && CreateBackBuffer();
}

void CompositorRenderer::SetLayout(LayoutSnapshot snapshot) {
  layout_ = std::move(snapshot);
}

void CompositorRenderer::Render() {
  if (!render_target_) return;
  context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
  constexpr float black[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  context_->ClearRenderTargetView(render_target_.Get(), black);
  const D3D11_VIEWPORT viewport = ContentViewport();
  if (viewport.Width <= 0.0F || viewport.Height <= 0.0F) return;
  context_->RSSetViewports(1, &viewport);
  DrawBlocks(viewport);
  DrawLabels(viewport);

  if (has_frame_ && local_frame_view_) {
    context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
    context_->RSSetViewports(1, &viewport);
    // The shared ring stays allocated at its maximum size; the live CEF
    // content occupies only the top-left sub-rectangle reported per frame.
    const float uv_width =
        source_width_ != 0
            ? static_cast<float>(frame_width_) / static_cast<float>(source_width_)
            : 1.0F;
    const float uv_height =
        source_height_ != 0 ? static_cast<float>(frame_height_) /
                                  static_cast<float>(source_height_)
                            : 1.0F;
    Constants constants{{0.0F, 0.0F, 1.0F, 1.0F},
                        {1.0F, 1.0F, 1.0F, 1.0F},
                        {0.0F, 0.0F, uv_width, uv_height}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                                0, &mapped))) {
      *static_cast<Constants*>(mapped.pData) = constants;
      context_->Unmap(constants_.Get(), 0);
      const float factors[4]{};
      context_->OMSetBlendState(overlay_blend_.Get(), factors, 0xFFFFFFFFU);
      context_->IASetInputLayout(nullptr);
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
      context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
      context_->PSSetShader(overlay_shader_.Get(), nullptr, 0);
      context_->PSSetShaderResources(0, 1, local_frame_view_.GetAddressOf());
      context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
      context_->Draw(4, 0);
      ID3D11ShaderResourceView* null_view = nullptr;
      context_->PSSetShaderResources(0, 1, &null_view);
      context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    }
  }

  swap_chain_->Present(1, 0);
}

bool CompositorRenderer::WindowToSource(int window_x,
                                        int window_y,
                                        int* source_x,
                                        int* source_y) const {
  if (source_x == nullptr || source_y == nullptr) return false;
  const D3D11_VIEWPORT viewport = ContentViewport();
  if (window_x < viewport.TopLeftX || window_y < viewport.TopLeftY ||
      window_x >= viewport.TopLeftX + viewport.Width ||
      window_y >= viewport.TopLeftY + viewport.Height) {
    return false;
  }
  const unsigned logical_width = frame_width_ != 0 ? frame_width_ : source_width_;
  const unsigned logical_height =
      frame_height_ != 0 ? frame_height_ : source_height_;
  if (logical_width == 0 || logical_height == 0) return false;
  *source_x = std::clamp(static_cast<int>(
                 (window_x - viewport.TopLeftX) * logical_width /
                             viewport.Width),
               0, static_cast<int>(logical_width) - 1);
  *source_y = std::clamp(static_cast<int>(
                 (window_y - viewport.TopLeftY) * logical_height /
                             viewport.Height),
               0, static_cast<int>(logical_height) - 1);
  return true;
}

bool CompositorRenderer::CreateDevice(const LUID* required_luid) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
  if (required_luid != nullptr) {
    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 description{};
      adapter->GetDesc1(&description);
      if (description.AdapterLuid.LowPart == required_luid->LowPart &&
          description.AdapterLuid.HighPart == required_luid->HighPart) {
        selected = adapter;
        break;
      }
    }
    if (!selected) return false;
  }
  constexpr std::array levels = {D3D_FEATURE_LEVEL_11_1,
                                 D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL level{};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  HRESULT result = D3D11CreateDevice(
      selected.Get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, flags, levels.data(), static_cast<UINT>(levels.size()),
      D3D11_SDK_VERSION, &device_, &level, &context_);
#if defined(_DEBUG)
  if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    result = D3D11CreateDevice(
        selected.Get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, levels.data(), static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION, &device_, &level, &context_);
  }
#endif
  return SUCCEEDED(result) && SUCCEEDED(device_.As(&device1_));
}

bool CompositorRenderer::CreateSwapChain() {
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
  description.Width = std::max<LONG>(rect.right, 1);
  description.Height = std::max<LONG>(rect.bottom, 1);
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = 2;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  description.Scaling = DXGI_SCALING_STRETCH;
  if (FAILED(factory->CreateSwapChainForHwnd(
          device_.Get(), window_, &description, nullptr, nullptr,
          &swap_chain_))) {
    return false;
  }
  factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
  return true;
}

bool CompositorRenderer::CreateBackBuffer() {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
      FAILED(device_->CreateRenderTargetView(buffer.Get(), nullptr,
                                              &render_target_))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC description{};
  buffer->GetDesc(&description);
  back_buffer_width_ = description.Width;
  back_buffer_height_ = description.Height;
  return CreateTextResources();
}

bool CompositorRenderer::CreateShaders() {
  Microsoft::WRL::ComPtr<ID3DBlob> vertex;
  Microsoft::WRL::ComPtr<ID3DBlob> solid;
  Microsoft::WRL::ComPtr<ID3DBlob> overlay;
  if (!Compile(kVertexShader, "vs_5_0", &vertex) ||
      !Compile(kSolidShader, "ps_5_0", &solid) ||
      !Compile(kOverlayShader, "ps_5_0", &overlay)) {
    Log(LogLevel::kError, L"Compositor shader compilation failed");
    return false;
  }
  if (FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(),
                                          vertex->GetBufferSize(), nullptr,
                                          &vertex_shader_)) ||
      FAILED(device_->CreatePixelShader(solid->GetBufferPointer(),
                                         solid->GetBufferSize(), nullptr,
                                         &solid_shader_)) ||
      FAILED(device_->CreatePixelShader(overlay->GetBufferPointer(),
                                         overlay->GetBufferSize(), nullptr,
                                         &overlay_shader_))) {
    Log(LogLevel::kError, L"Compositor compiled shaders could not be created");
    return false;
  }
  D3D11_BUFFER_DESC constant_description{};
  constant_description.ByteWidth = sizeof(Constants);
  constant_description.Usage = D3D11_USAGE_DYNAMIC;
  constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateBuffer(&constant_description, nullptr, &constants_))) {
    Log(LogLevel::kError, L"Compositor constant buffer creation failed");
    return false;
  }
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device_->CreateSamplerState(&sampler, &sampler_))) {
    Log(LogLevel::kError, L"Compositor sampler creation failed");
    return false;
  }
  D3D11_BLEND_DESC blend{};
  auto& target = blend.RenderTarget[0];
  target.BlendEnable = TRUE;
  target.SrcBlend = D3D11_BLEND_ONE;
  target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOp = D3D11_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D11_BLEND_ONE;
  target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device_->CreateBlendState(&blend, &overlay_blend_))) {
    Log(LogLevel::kError, L"Compositor blend-state creation failed");
    return false;
  }
  return true;
}

bool CompositorRenderer::CreateTextResources() {
  if (!d2d_factory_ && FAILED(D2D1CreateFactory(
                           D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           d2d_factory_.GetAddressOf()))) {
    return false;
  }
  if (!dwrite_factory_ &&
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(
                                     dwrite_factory_.GetAddressOf())))) {
    return false;
  }
  if (!text_format_ &&
      FAILED(dwrite_factory_->CreateTextFormat(
          L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0F, L"en-US",
          &text_format_))) {
    return false;
  }
  text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  Microsoft::WRL::ComPtr<IDXGISurface> surface;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_IGNORE));
  if (FAILED(d2d_factory_->CreateDxgiSurfaceRenderTarget(
          surface.Get(), &properties, &d2d_target_)) ||
      FAILED(d2d_target_->CreateSolidColorBrush(
          D2D1::ColorF(0.13F, 0.12F, 0.08F, 0.92F), &text_brush_))) {
    return false;
  }
  return true;
}

bool CompositorRenderer::CreateLocalFrame(
    const D3D11_TEXTURE2D_DESC& shared_description) {
  D3D11_TEXTURE2D_DESC local = shared_description;
  local.MiscFlags = 0;
  local.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  local.CPUAccessFlags = 0;
  if (FAILED(device_->CreateTexture2D(&local, nullptr, &local_frame_)) ||
      FAILED(device_->CreateShaderResourceView(local_frame_.Get(), nullptr,
                                                &local_frame_view_))) {
    return false;
  }
  D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
  return SUCCEEDED(device_->CreateQuery(&query, &copy_completion_));
}

bool CompositorRenderer::WaitForCopy() {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  BOOL complete = FALSE;
  while (std::chrono::steady_clock::now() < deadline) {
    const HRESULT result = context_->GetData(copy_completion_.Get(), &complete,
                                             sizeof(complete), 0);
    if (result == S_OK && complete == TRUE) return true;
    if (result != S_FALSE) return false;
    SwitchToThread();
  }
  return false;
}

void CompositorRenderer::ResetDevice() {
  shared_slots_.clear();
  copy_completion_.Reset();
  local_frame_view_.Reset();
  local_frame_.Reset();
  text_brush_.Reset();
  d2d_target_.Reset();
  text_format_.Reset();
  dwrite_factory_.Reset();
  d2d_factory_.Reset();
  overlay_blend_.Reset();
  sampler_.Reset();
  constants_.Reset();
  overlay_shader_.Reset();
  solid_shader_.Reset();
  vertex_shader_.Reset();
  render_target_.Reset();
  swap_chain_.Reset();
  context_.Reset();
  device1_.Reset();
  device_.Reset();
  has_frame_ = false;
}

D3D11_VIEWPORT CompositorRenderer::ContentViewport() const {
  D3D11_VIEWPORT viewport{};
  if (server_scaling_ && back_buffer_width_ != 0 && back_buffer_height_ != 0) {
    // While CEF catches up with a live resize, stretch the most recent frame
    // over the current client area. Once CEF delivers that size this becomes
    // a 1:1 presentation without ever exposing stale-aspect letterboxing.
    viewport.Width = static_cast<float>(back_buffer_width_);
    viewport.Height = static_cast<float>(back_buffer_height_);
    viewport.MaxDepth = 1.0F;
    return viewport;
  }
  const unsigned logical_width = frame_width_ != 0 ? frame_width_ : output_width_;
  const unsigned logical_height = frame_height_ != 0 ? frame_height_ : output_height_;
  if (logical_width == 0 || logical_height == 0 || back_buffer_width_ == 0 ||
      back_buffer_height_ == 0) {
    return viewport;
  }
  const float scale = std::min(static_cast<float>(back_buffer_width_) /
                                   static_cast<float>(logical_width),
                               static_cast<float>(back_buffer_height_) /
                                   static_cast<float>(logical_height));
  viewport.Width = logical_width * scale;
  viewport.Height = logical_height * scale;
  viewport.TopLeftX = (back_buffer_width_ - viewport.Width) * 0.5F;
  viewport.TopLeftY = (back_buffer_height_ - viewport.Height) * 0.5F;
  viewport.MaxDepth = 1.0F;
  return viewport;
}

void CompositorRenderer::DrawBlocks(const D3D11_VIEWPORT&) {
  context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
  context_->PSSetShader(solid_shader_.Get(), nullptr, 0);
  context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
  for (const auto& viewport : layout_.viewports) {
    const D2D1_COLOR_F source_color = SourceColor(viewport.source_id);
    Constants constants{
        {static_cast<float>(viewport.rect.x), static_cast<float>(viewport.rect.y),
         static_cast<float>(viewport.rect.width),
         static_cast<float>(viewport.rect.height)},
        {source_color.r, source_color.g, source_color.b, 1.0F}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped))) {
      continue;
    }
    *static_cast<Constants*>(mapped.pData) = constants;
    context_->Unmap(constants_.Get(), 0);
    context_->Draw(4, 0);
  }
}

void CompositorRenderer::DrawLabels(const D3D11_VIEWPORT& viewport) {
  if (!d2d_target_ || !text_format_ || !text_brush_) return;
  d2d_target_->BeginDraw();
  for (const auto& item : layout_.viewports) {
    const std::wstring label = ToWide(item.label);
    const D2D1_RECT_F rect = D2D1::RectF(
        viewport.TopLeftX + static_cast<float>(item.rect.x) * viewport.Width,
        viewport.TopLeftY + static_cast<float>(item.rect.y) * viewport.Height,
        viewport.TopLeftX +
            static_cast<float>(item.rect.x + item.rect.width) * viewport.Width,
        viewport.TopLeftY +
            static_cast<float>(item.rect.y + item.rect.height) * viewport.Height);
    d2d_target_->DrawTextW(label.data(), static_cast<UINT32>(label.size()),
                           text_format_.Get(), rect, text_brush_.Get());
  }
  const HRESULT result = d2d_target_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET) {
    text_brush_.Reset();
    d2d_target_.Reset();
    CreateTextResources();
  }
}

}  // namespace streaming::compositor
