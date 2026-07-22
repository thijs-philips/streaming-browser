#include "src/viewer/d3d_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

constexpr char kCheckerShader[] = R"HLSL(
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  uint2 cell = uint2(position.xy) / 16;
  float shade = ((cell.x + cell.y) & 1) ? 0.72 : 0.48;
  return float4(shade, shade, shade, 1.0);
}
)HLSL";

constexpr char kFrameShader[] = R"HLSL(
Texture2D frame_texture : register(t0);
SamplerState frame_sampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 frame = frame_texture.Sample(frame_sampler, uv);
  uint2 cell = uint2(position.xy) / 16;
  float shade = ((cell.x + cell.y) & 1) ? 0.72 : 0.48;
  return float4(frame.rgb + shade.xxx * (1.0 - frame.a), 1.0);
}
)HLSL";

bool Compile(const char* source, const char* profile, ID3DBlob** output) {
  Microsoft::WRL::ComPtr<ID3DBlob> error;
  const HRESULT result = D3DCompile(
      source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
      D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &error);
  if (FAILED(result) && error) {
    OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
  }
  return SUCCEEDED(result);
}

}  // namespace

bool D3DRenderer::Initialize(HWND window) {
  window_ = window;
  return CreateDevice(nullptr) && CreateSwapChain() && CreateBackBuffer() &&
         CreateShaders();
}

bool D3DRenderer::OpenRing(const protocol::RingDefinition& definition) {
  const LUID luid{definition.adapter_luid_low, definition.adapter_luid_high};
  ResetDevice();
  if (!CreateDevice(&luid) || !CreateSwapChain() || !CreateBackBuffer() ||
      !CreateShaders()) {
    Log(LogLevel::kError, L"Viewer could not create device on producer adapter");
    return false;
  }

  std::vector<SharedSlot> slots;
  slots.reserve(definition.slots.size());
  D3D11_TEXTURE2D_DESC shared_description{};
  for (std::size_t i = 0; i < definition.slots.size(); ++i) {
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
        definition.slots[i].handle));
    SharedSlot slot;
    const HRESULT open_result = device1_->OpenSharedResource1(
        handle, IID_PPV_ARGS(&slot.texture));
    CloseHandle(handle);
    if (FAILED(open_result) || FAILED(slot.texture.As(&slot.keyed_mutex))) {
      for (std::size_t j = i + 1; j < definition.slots.size(); ++j) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(
            definition.slots[j].handle)));
      }
      return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    slot.texture->GetDesc(&description);
    if (i == 0) {
      shared_description = description;
    } else if (description.Width != shared_description.Width ||
               description.Height != shared_description.Height ||
               description.Format != shared_description.Format ||
               description.MiscFlags != shared_description.MiscFlags) {
      return false;
    }
    slots.push_back(std::move(slot));
  }

  if (shared_description.Width != definition.width ||
      shared_description.Height != definition.height ||
        static_cast<std::uint32_t>(shared_description.Format) !=
          definition.dxgi_format ||
      shared_description.MipLevels != 1 || shared_description.ArraySize != 1 ||
      shared_description.SampleDesc.Count != 1 ||
      shared_description.Usage != D3D11_USAGE_DEFAULT ||
      shared_description.CPUAccessFlags != 0 ||
      (shared_description.MiscFlags &
       (D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) !=
          (D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) {
    return false;
  }
  if (!CreateLocalFrame(shared_description)) {
    return false;
  }
  shared_slots_ = std::move(slots);
  source_width_ = definition.width;
  source_height_ = definition.height;
  has_frame_ = false;
  Log(LogLevel::kInfo, L"Viewer opened shared keyed-mutex texture ring");
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
  if (SUCCEEDED(device_.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter.As(&adapter3)) &&
      SUCCEEDED(adapter3->QueryVideoMemoryInfo(
          0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
    Log(LogLevel::kInfo,
        L"Viewer local GPU memory budget MiB=" +
            std::to_wstring(memory.Budget / (1024U * 1024U)) +
            L", usage MiB=" +
            std::to_wstring(memory.CurrentUsage / (1024U * 1024U)));
  }
  return true;
}

bool D3DRenderer::ConsumeFrame(const protocol::FrameMetadata& metadata) {
  if (metadata.slot >= shared_slots_.size() || !local_frame_) {
    return false;
  }
  SharedSlot& slot = shared_slots_[metadata.slot];
  const HRESULT acquired = slot.keyed_mutex->AcquireSync(1, 100);
  if (acquired == WAIT_TIMEOUT) {
    Log(LogLevel::kWarning, L"Viewer timed out acquiring shared frame");
    return false;
  }
  if (acquired == WAIT_ABANDONED || acquired != S_OK) {
    Log(LogLevel::kError, L"Viewer failed to acquire shared frame mutex");
    return false;
  }

  context_->CopyResource(local_frame_.Get(), slot.texture.Get());
  context_->End(copy_completion_.Get());
  if (!WaitForCopy()) {
    Log(LogLevel::kError, L"Viewer local frame copy timed out");
    return false;
  }
  if (slot.keyed_mutex->ReleaseSync(0) != S_OK) {
    Log(LogLevel::kError, L"Viewer failed to release shared frame mutex");
    return false;
  }
  has_frame_ = true;
  return true;
}

bool D3DRenderer::Resize(unsigned width, unsigned height) {
  if (!swap_chain_ || width == 0 || height == 0) {
    return false;
  }
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  render_target_.Reset();
  const HRESULT result =
      swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  return SUCCEEDED(result) && CreateBackBuffer();
}

void D3DRenderer::Render() {
  if (!render_target_) {
    return;
  }
  context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
  constexpr float kWindowChrome[4] = {0.94F, 0.95F, 0.97F, 1.0F};
  context_->ClearRenderTargetView(render_target_.Get(), kWindowChrome);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);

  D3D11_VIEWPORT full{};
  full.TopLeftY = static_cast<float>(content_top_);
  full.Width = static_cast<float>(back_buffer_width_);
  full.Height = content_top_ < back_buffer_height_
                    ? static_cast<float>(back_buffer_height_ - content_top_)
                    : 0.0F;
  full.MaxDepth = 1.0F;
  if (full.Height > 0.0F) {
    context_->RSSetViewports(1, &full);
    context_->PSSetShader(checker_shader_.Get(), nullptr, 0);
    context_->Draw(3, 0);
  }

  if (has_frame_ && local_frame_view_) {
    const D3D11_VIEWPORT frame_viewport = FrameViewport();
    context_->RSSetViewports(1, &frame_viewport);
    context_->PSSetShader(frame_shader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, local_frame_view_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* null_view = nullptr;
    context_->PSSetShaderResources(0, 1, &null_view);
  }
  swap_chain_->Present(1, 0);
}

bool D3DRenderer::WindowToSource(int window_x, int window_y, int* source_x,
                                 int* source_y) const {
  if (!has_frame_ || source_x == nullptr || source_y == nullptr) {
    return false;
  }
  const D3D11_VIEWPORT viewport = FrameViewport();
  if (window_x < viewport.TopLeftX || window_y < viewport.TopLeftY ||
      window_x >= viewport.TopLeftX + viewport.Width ||
      window_y >= viewport.TopLeftY + viewport.Height) {
    return false;
  }
  *source_x = std::clamp(
      static_cast<int>((window_x - viewport.TopLeftX) * source_width_ /
                       viewport.Width),
      0, static_cast<int>(source_width_) - 1);
  *source_y = std::clamp(
      static_cast<int>((window_y - viewport.TopLeftY) * source_height_ /
                       viewport.Height),
      0, static_cast<int>(source_height_) - 1);
  return true;
}

void D3DRenderer::SetPixelPerfect(bool enabled) {
  pixel_perfect_ = enabled;
  pan_x_ = 0.0F;
  pan_y_ = 0.0F;
}

void D3DRenderer::Pan(float delta_x, float delta_y) {
  if (pixel_perfect_) {
    pan_x_ += delta_x;
    pan_y_ += delta_y;
  }
}

bool D3DRenderer::CreateDevice(const LUID* required_luid) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> selected_adapter;
  if (required_luid != nullptr) {
    for (UINT index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_ADAPTER_DESC1 description{};
      adapter->GetDesc1(&description);
      if (description.AdapterLuid.LowPart == required_luid->LowPart &&
          description.AdapterLuid.HighPart == required_luid->HighPart) {
        selected_adapter = adapter;
        break;
      }
    }
    if (!selected_adapter) {
      return false;
    }
  }

  constexpr std::array<D3D_FEATURE_LEVEL, 2> levels = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL selected_level{};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  const D3D_DRIVER_TYPE driver =
      selected_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
  HRESULT result = D3D11CreateDevice(
      selected_adapter.Get(), driver, nullptr, flags, levels.data(),
      static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device_,
      &selected_level, &context_);
#if defined(_DEBUG)
  if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    result = D3D11CreateDevice(
        selected_adapter.Get(), driver, nullptr, flags, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device_,
        &selected_level, &context_);
  }
#endif
  return SUCCEEDED(result) && SUCCEEDED(device_.As(&device1_));
}

bool D3DRenderer::CreateSwapChain() {
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
  description.Width = std::max<LONG>(rect.right - rect.left, 1);
  description.Height = std::max<LONG>(rect.bottom - rect.top, 1);
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

bool D3DRenderer::CreateBackBuffer() {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
      FAILED(device_->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                              &render_target_))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC description{};
  back_buffer->GetDesc(&description);
  back_buffer_width_ = description.Width;
  back_buffer_height_ = description.Height;
  return true;
}

bool D3DRenderer::CreateShaders() {
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> checker_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> frame_blob;
  if (!Compile(kVertexShader, "vs_5_0", &vertex_blob) ||
      !Compile(kCheckerShader, "ps_5_0", &checker_blob) ||
      !Compile(kFrameShader, "ps_5_0", &frame_blob) ||
      FAILED(device_->CreateVertexShader(
          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
          nullptr, &vertex_shader_)) ||
      FAILED(device_->CreatePixelShader(
          checker_blob->GetBufferPointer(), checker_blob->GetBufferSize(),
          nullptr, &checker_shader_)) ||
      FAILED(device_->CreatePixelShader(
          frame_blob->GetBufferPointer(), frame_blob->GetBufferSize(), nullptr,
          &frame_shader_))) {
    return false;
  }
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  return SUCCEEDED(device_->CreateSamplerState(&sampler, &sampler_));
}

bool D3DRenderer::CreateLocalFrame(
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

bool D3DRenderer::WaitForCopy() {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  BOOL complete = FALSE;
  for (;;) {
    const HRESULT result = context_->GetData(copy_completion_.Get(), &complete,
                                             sizeof(complete), 0);
    if (result == S_OK && complete == TRUE) {
      return true;
    }
    if (result != S_FALSE || std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    SwitchToThread();
  }
}

void D3DRenderer::ResetDevice() {
  shared_slots_.clear();
  copy_completion_.Reset();
  local_frame_view_.Reset();
  local_frame_.Reset();
  sampler_.Reset();
  frame_shader_.Reset();
  checker_shader_.Reset();
  vertex_shader_.Reset();
  render_target_.Reset();
  swap_chain_.Reset();
  context_.Reset();
  device1_.Reset();
  device_.Reset();
  has_frame_ = false;
}

D3D11_VIEWPORT D3DRenderer::FrameViewport() const {
  D3D11_VIEWPORT viewport{};
  if (source_width_ == 0 || source_height_ == 0 || back_buffer_width_ == 0 ||
      back_buffer_height_ == 0 || content_top_ >= back_buffer_height_) {
    return viewport;
  }
  const float content_height =
      static_cast<float>(back_buffer_height_ - content_top_);
  const float scale = pixel_perfect_
                          ? 1.0F
                          : std::min(
                                static_cast<float>(back_buffer_width_) /
                                    source_width_,
                                content_height / source_height_);
  viewport.Width = std::max(1.0F, source_width_ * scale);
  viewport.Height = std::max(1.0F, source_height_ * scale);
  viewport.TopLeftX = (back_buffer_width_ - viewport.Width) * 0.5F + pan_x_;
  viewport.TopLeftY = static_cast<float>(content_top_) +
                      (content_height - viewport.Height) * 0.5F + pan_y_;
  viewport.MaxDepth = 1.0F;
  return viewport;
}

}  // namespace streaming::viewer
