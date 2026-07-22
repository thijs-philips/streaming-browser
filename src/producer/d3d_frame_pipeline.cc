#include "src/producer/d3d_frame_pipeline.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <array>
#include <algorithm>
#include <string>

#include "src/common/logging.h"

namespace streaming::producer {
namespace {

constexpr auto kCopyWatchdog = std::chrono::milliseconds(250);

constexpr char kPopupVertexShader[] = R"HLSL(
cbuffer PopupConstants : register(b0) {
  float4 destination_rect;
  float4 source_rect;
};
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
  Output output;
  float2 corner = float2(id & 1, (id >> 1) & 1);
  float2 p = destination_rect.xy + corner * destination_rect.zw;
  output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  output.uv = source_rect.xy + corner * source_rect.zw;
  return output;
}
)HLSL";

constexpr char kPopupPixelShader[] = R"HLSL(
Texture2D popup_texture : register(t0);
SamplerState popup_sampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return popup_texture.Sample(popup_sampler, uv);
}
)HLSL";

struct PopupConstants {
  float destination[4];
  float source[4];
};

std::uint64_t MonotonicMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool IsSupportedColorType(cef_color_type_t format) {
  return format == CEF_COLOR_TYPE_RGBA_8888 ||
         format == CEF_COLOR_TYPE_BGRA_8888;
}

D3D11_TEXTURE2D_DESC MakeOwnedDescription(
    const D3D11_TEXTURE2D_DESC& source) {
  D3D11_TEXTURE2D_DESC result = source;
  result.Usage = D3D11_USAGE_DEFAULT;
  result.CPUAccessFlags = 0;
  result.MiscFlags = 0;
  result.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  return result;
}

}  // namespace

D3DFramePipeline::D3DFramePipeline(PublishCallback publish_callback,
                                   RingReadyCallback ring_ready_callback,
                                   bool alpha_probe_enabled)
    : publish_callback_(std::move(publish_callback)),
      ring_ready_callback_(std::move(ring_ready_callback)),
      alpha_probe_enabled_(alpha_probe_enabled) {
  const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
  if (FAILED(result)) {
    Log(LogLevel::kError, L"CreateDXGIFactory1 failed; accelerated capture cannot initialize");
  }
}

bool D3DFramePipeline::CopyFromCef(
    CefRenderHandler::PaintElementType type,
  const CefRenderHandler::RectList& dirty_rects,
    const CefAcceleratedPaintInfo& info) {
  if (info.shared_texture_handle == nullptr || !IsSupportedColorType(info.format)) {
    Log(LogLevel::kError, L"CEF returned an unsupported accelerated frame");
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
  if (device1_) {
    const HRESULT open_result = device1_->OpenSharedResource1(
        info.shared_texture_handle, IID_PPV_ARGS(&source));
    if (FAILED(open_result)) {
      Log(LogLevel::kWarning, L"CEF shared texture no longer opens on the selected adapter");
      return false;
    }
  } else if (!DiscoverDeviceAndOpen(info.shared_texture_handle, &source)) {
    Log(LogLevel::kError, L"No hardware adapter could open the CEF shared texture");
    return false;
  }

  D3D11_TEXTURE2D_DESC source_desc{};
  source->GetDesc(&source_desc);
  if (source_desc.MipLevels != 1 || source_desc.ArraySize != 1 ||
      source_desc.SampleDesc.Count != 1) {
    Log(LogLevel::kError, L"CEF shared texture descriptor is unsupported");
    return false;
  }

  CaptureSlot* slot = nullptr;
  if (type == PET_POPUP) {
    if (!popup_slot_.texture || source_desc.Width != popup_desc_.Width ||
        source_desc.Height != popup_desc_.Height ||
        source_desc.Format != popup_desc_.Format) {
      if (!CreatePopupTarget(source_desc)) {
        return false;
      }
    }
    slot = &popup_slot_;
  } else {
    if (!view_slots_[0].texture) {
      if (!CreateViewRing(source_desc)) {
        return false;
      }
    } else if (source_desc.Width != view_desc_.Width ||
               source_desc.Height != view_desc_.Height ||
               source_desc.Format != view_desc_.Format) {
      Log(LogLevel::kError, L"CEF view texture changed descriptor; a new stream generation is required");
      return false;
    }
    slot = &view_slots_[next_view_slot_];
    latest_view_slot_ = next_view_slot_;
    next_view_slot_ = (next_view_slot_ + 1U) % view_slots_.size();
  }

  context_->CopyResource(slot->texture.Get(), source.Get());
  context_->End(slot->completion.Get());
  if (!WaitForCopy(slot->completion.Get())) {
    FailFastGpu(L"CEF frame copy did not complete inside OnAcceleratedPaint", E_FAIL);
  }

  slot->frame_id = ++frame_id_;
  latest_metadata_.frame_id = frame_id_;
  latest_metadata_.producer_timestamp_us = MonotonicMicroseconds();
  if (type == PET_VIEW) {
    latest_metadata_.cef_view_timestamp_us = info.extra.timestamp;
    latest_metadata_.cef_view_counter =
        info.extra.has_capture_counter ? info.extra.capture_counter : 0;
    latest_metadata_.width = source_desc.Width;
    latest_metadata_.height = source_desc.Height;
    latest_metadata_.damage.clear();
    if (info.extra.has_capture_update_rect) {
      const auto& rect = info.extra.capture_update_rect;
      latest_metadata_.damage.push_back(
          {rect.x, rect.y, rect.width, rect.height});
    } else {
      for (const CefRect& rect : dirty_rects) {
        if (latest_metadata_.damage.size() >= protocol::kMaxDamageRects) {
          break;
        }
        latest_metadata_.damage.push_back(
            {rect.x, rect.y, rect.width, rect.height});
      }
      if (latest_metadata_.damage.empty()) {
        latest_metadata_.damage.push_back(
            {0, 0, static_cast<std::int32_t>(source_desc.Width),
             static_cast<std::int32_t>(source_desc.Height)});
      }
    }
    if (alpha_probe_enabled_ && !alpha_probe_completed_) {
      if (!ProbeAlpha(slot->texture.Get())) {
        FailFastGpu(L"Deterministic alpha probe failed", E_FAIL);
      }
      alpha_probe_completed_ = true;
      Log(LogLevel::kInfo,
          L"Alpha probe passed: opaque, 50%, and transparent pixels preserved");
    }
  } else {
    latest_metadata_.damage.clear();
    latest_metadata_.damage.push_back(
        {popup_bounds_.x, popup_bounds_.y, popup_bounds_.width,
         popup_bounds_.height});
  }
  return PublishLatest();
}

void D3DFramePipeline::SetStreamingEnabled(bool enabled) {
  streaming_enabled_ = enabled;
  Log(LogLevel::kInfo,
      enabled ? L"Shared frame publication enabled"
              : L"Shared frame publication disabled");
  if (enabled) {
    PublishLatest();
  } else if (device_ && view_slots_[0].texture) {
    ++generation_;
    if (CreateOutputRing(view_desc_) && ring_ready_callback_) {
      ring_ready_callback_();
    }
  }
}

void D3DFramePipeline::SetPopupVisible(bool visible) {
  if (popup_visible_ == visible) {
    return;
  }
  const CefRect old_bounds = popup_bounds_;
  popup_visible_ = visible;
  latest_metadata_.frame_id = ++frame_id_;
  latest_metadata_.producer_timestamp_us = MonotonicMicroseconds();
  latest_metadata_.damage = {{old_bounds.x, old_bounds.y, old_bounds.width,
                              old_bounds.height}};
  PublishLatest();
}

void D3DFramePipeline::SetPopupBounds(const CefRect& bounds) {
  if (popup_bounds_ == bounds) {
    return;
  }
  const CefRect old_bounds = popup_bounds_;
  popup_bounds_ = bounds;
  latest_metadata_.frame_id = ++frame_id_;
  latest_metadata_.producer_timestamp_us = MonotonicMicroseconds();
  latest_metadata_.damage.clear();
  if (old_bounds.width > 0 && old_bounds.height > 0) {
    latest_metadata_.damage.push_back(
        {old_bounds.x, old_bounds.y, old_bounds.width, old_bounds.height});
  }
  if (bounds.width > 0 && bounds.height > 0) {
    latest_metadata_.damage.push_back(
        {bounds.x, bounds.y, bounds.width, bounds.height});
  }
  PublishLatest();
}

void D3DFramePipeline::ReleaseOutputSlot(std::uint32_t slot,
                                        std::uint64_t frame_id) {
  if (slot >= output_slots_.size()) {
    return;
  }
  OutputSlot& output = output_slots_[slot];
  if (output.state != OutputState::kPublished || output.frame_id != frame_id) {
    return;
  }
  output.state = OutputState::kAvailable;
  PublishLatest();
}

bool D3DFramePipeline::GetRingSnapshot(RingSnapshot* snapshot) const {
  if (snapshot == nullptr) {
    return false;
  }
  std::lock_guard lock(ring_mutex_);
  if (!output_slots_[0].texture) {
    return false;
  }
  snapshot->adapter_luid = adapter_luid_;
  snapshot->description = view_desc_;
  snapshot->generation = generation_;
  for (std::size_t i = 0; i < output_slots_.size(); ++i) {
    snapshot->handles[i] = output_slots_[i].shared_handle.get();
  }
  return true;
}

bool D3DFramePipeline::DiscoverDeviceAndOpen(
    HANDLE handle,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>* source) {
  if (!factory_ || source == nullptr) {
    return false;
  }

  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory_->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }

    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (FAILED(adapter->GetDesc1(&adapter_desc)) ||
        (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }

    constexpr std::array<D3D_FEATURE_LEVEL, 4> levels = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected_level{};
    HRESULT create_result = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device,
        &selected_level, &context);
#if defined(_DEBUG)
    if (create_result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
      flags &= ~D3D11_CREATE_DEVICE_DEBUG;
      create_result = D3D11CreateDevice(
          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels.data(),
          static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device,
          &selected_level, &context);
    }
#endif
    if (FAILED(create_result)) {
      continue;
    }

    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (FAILED(device.As(&device1))) {
      continue;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    if (FAILED(device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened)))) {
      continue;
    }

    adapter_ = adapter;
    device_ = device;
    device1_ = device1;
    context_ = context;
    adapter_luid_ = adapter_desc.AdapterLuid;
    *source = opened;

    std::wstring message = L"Selected CEF GPU adapter: ";
    message.append(adapter_desc.Description);
    Log(LogLevel::kInfo, message);
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (SUCCEEDED(adapter.As(&adapter3)) &&
      SUCCEEDED(adapter3->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
      Log(LogLevel::kInfo,
        L"Producer local GPU memory budget MiB=" +
          std::to_wstring(memory.Budget / (1024U * 1024U)) +
          L", usage MiB=" +
          std::to_wstring(memory.CurrentUsage / (1024U * 1024U)));
    }
    return true;
  }
  return false;
}

bool D3DFramePipeline::CreateViewRing(
    const D3D11_TEXTURE2D_DESC& source_desc) {
  const D3D11_TEXTURE2D_DESC owned_desc = MakeOwnedDescription(source_desc);
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};

  std::array<CaptureSlot, kCaptureSlots> new_slots{};
  for (auto& slot : new_slots) {
    HRESULT result = device_->CreateTexture2D(&owned_desc, nullptr, &slot.texture);
    if (FAILED(result)) {
      Log(LogLevel::kError, L"Failed to allocate the private 4K capture ring");
      return false;
    }
    result = device_->CreateQuery(&query_desc, &slot.completion);
    if (FAILED(result)) {
      Log(LogLevel::kError, L"Failed to allocate a D3D11 completion query");
      return false;
    }
  }

  view_slots_ = std::move(new_slots);
  view_desc_ = source_desc;
  next_view_slot_ = 0;
  latest_view_slot_ = 0;
  if (!CreateOutputRing(source_desc) || !CreateCompositorResources()) {
    return false;
  }
  Log(LogLevel::kInfo, L"Private CEF view capture ring initialized");
  if (ring_ready_callback_) {
    ring_ready_callback_();
  }
  return true;
}

bool D3DFramePipeline::CreatePopupTarget(
    const D3D11_TEXTURE2D_DESC& source_desc) {
  CaptureSlot new_slot;
  const D3D11_TEXTURE2D_DESC owned_desc = MakeOwnedDescription(source_desc);
  HRESULT result = device_->CreateTexture2D(&owned_desc, nullptr, &new_slot.texture);
  if (FAILED(result)) {
    Log(LogLevel::kError, L"Failed to allocate popup capture texture");
    return false;
  }
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
  result = device_->CreateQuery(&query_desc, &new_slot.completion);
  if (FAILED(result)) {
    return false;
  }
  popup_slot_ = std::move(new_slot);
  popup_desc_ = source_desc;
  popup_srv_.Reset();
  return SUCCEEDED(
      device_->CreateShaderResourceView(popup_slot_.texture.Get(), nullptr,
                                        &popup_srv_));
}

bool D3DFramePipeline::CreateOutputRing(
    const D3D11_TEXTURE2D_DESC& source_desc) {
  D3D11_TEXTURE2D_DESC description = MakeOwnedDescription(source_desc);
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
  std::array<OutputSlot, kOutputSlots> new_slots{};

  for (auto& slot : new_slots) {
    HRESULT result =
        device_->CreateTexture2D(&description, nullptr, &slot.texture);
    if (FAILED(result) ||
        FAILED(slot.texture.As(&slot.keyed_mutex)) ||
        FAILED(device_->CreateQuery(&query_desc, &slot.completion))) {
      return false;
    }
    const HRESULT initial_acquire = slot.keyed_mutex->AcquireSync(0, 0);
    if (initial_acquire != S_OK || slot.keyed_mutex->ReleaseSync(0) != S_OK) {
      Log(LogLevel::kError,
          L"Failed to initialize shared output slot to producer key 0");
      return false;
    }
    context_->Flush();
    Microsoft::WRL::ComPtr<IDXGIResource1> resource;
    if (FAILED(slot.texture.As(&resource))) {
      return false;
    }
    HANDLE handle = nullptr;
    result = resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &handle);
    if (FAILED(result)) {
      return false;
    }
    slot.shared_handle.reset(handle);
  }

  {
    std::lock_guard lock(ring_mutex_);
    output_slots_ = std::move(new_slots);
  }
  // A fresh ring (new generation/session) must republish the latest frame
  // even if its ID was already delivered to a previous viewer session.
  published_frame_id_ = 0;
  return true;
}

bool D3DFramePipeline::CreateCompositorResources() {
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT result = D3DCompile(
      kPopupVertexShader, sizeof(kPopupVertexShader), nullptr, nullptr, nullptr,
      "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex_blob,
      &errors);
  if (FAILED(result)) {
    return false;
  }
  result = D3DCompile(
      kPopupPixelShader, sizeof(kPopupPixelShader), nullptr, nullptr, nullptr,
      "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel_blob,
      &errors);
  if (FAILED(result) ||
      FAILED(device_->CreateVertexShader(
          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
          nullptr, &popup_vertex_shader_)) ||
      FAILED(device_->CreatePixelShader(
          pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr,
          &popup_pixel_shader_))) {
    return false;
  }

  D3D11_BUFFER_DESC constant_desc{};
  constant_desc.ByteWidth = sizeof(PopupConstants);
  constant_desc.Usage = D3D11_USAGE_DYNAMIC;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateBuffer(&constant_desc, nullptr,
                                    &popup_constants_))) {
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc{};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device_->CreateSamplerState(&sampler_desc, &popup_sampler_))) {
    return false;
  }

  D3D11_BLEND_DESC blend_desc{};
  auto& target = blend_desc.RenderTarget[0];
  target.BlendEnable = TRUE;
  target.SrcBlend = D3D11_BLEND_ONE;
  target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOp = D3D11_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D11_BLEND_ONE;
  target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  return SUCCEEDED(
      device_->CreateBlendState(&blend_desc, &premultiplied_blend_));
}

bool D3DFramePipeline::PublishLatest() {
  if (!streaming_enabled_ || !view_slots_[latest_view_slot_].texture ||
      !publish_callback_) {
    return true;
  }
  if (latest_metadata_.frame_id <= published_frame_id_) {
    // The latest frame already reached the viewer; republishing it would
    // create a producer/viewer feedback loop that floods the pipe and GPU.
    return true;
  }

  for (std::size_t index = 0; index < output_slots_.size(); ++index) {
    OutputSlot& output = output_slots_[index];
    if (output.state != OutputState::kAvailable) {
      continue;
    }
    const HRESULT acquire = output.keyed_mutex->AcquireSync(0, 0);
    if (acquire == WAIT_TIMEOUT) {
      if (latest_metadata_.frame_id % 300 == 0) {
        Log(LogLevel::kWarning,
            L"Output slot AcquireSync(0) returned WAIT_TIMEOUT");
      }
      continue;
    }
    if (acquire == WAIT_ABANDONED || acquire != S_OK) {
      Log(LogLevel::kError,
          L"Output slot AcquireSync failed with status " +
              std::to_wstring(static_cast<long long>(acquire)));
      return false;
    }

    context_->CopyResource(output.texture.Get(),
                           view_slots_[latest_view_slot_].texture.Get());
    if (!CompositePopup(output.texture.Get())) {
      output.keyed_mutex->ReleaseSync(0);
      return false;
    }
    context_->End(output.completion.Get());
    if (!WaitForCopy(output.completion.Get())) {
      FailFastGpu(L"Final shared-frame composition did not complete", E_FAIL);
    }
    if (output.keyed_mutex->ReleaseSync(1) != S_OK) {
      return false;
    }

    protocol::FrameMetadata publication = latest_metadata_;
    publication.slot = static_cast<std::uint32_t>(index);
    output.frame_id = publication.frame_id;
    output.state = OutputState::kPublished;
    published_frame_id_ = publication.frame_id;
    if (!publish_callback_(publication)) {
      Log(LogLevel::kError, L"Critical shared frame publication failed");
      return false;
    }
    if (publication.frame_id == 1 || publication.frame_id % 300 == 0) {
      Log(LogLevel::kInfo, L"Published final shared texture slot");
    }
    return true;
  }
  if (latest_metadata_.frame_id % 300 == 0) {
    Log(LogLevel::kWarning,
        L"No keyed-mutex output slot was available for latest frame");
  }
  return true;
}

bool D3DFramePipeline::CompositePopup(ID3D11Texture2D* destination) {
  if (!popup_visible_ || !popup_srv_ || popup_bounds_.width <= 0 ||
      popup_bounds_.height <= 0) {
    return true;
  }

  const int left = std::clamp(popup_bounds_.x, 0,
                              static_cast<int>(view_desc_.Width));
  const int top = std::clamp(popup_bounds_.y, 0,
                             static_cast<int>(view_desc_.Height));
  const int right = std::clamp(popup_bounds_.x + popup_bounds_.width, 0,
                               static_cast<int>(view_desc_.Width));
  const int bottom = std::clamp(popup_bounds_.y + popup_bounds_.height, 0,
                                static_cast<int>(view_desc_.Height));
  if (right <= left || bottom <= top) {
    return true;
  }

  PopupConstants constants{};
  constants.destination[0] = static_cast<float>(left) / view_desc_.Width;
  constants.destination[1] = static_cast<float>(top) / view_desc_.Height;
  constants.destination[2] = static_cast<float>(right - left) / view_desc_.Width;
  constants.destination[3] = static_cast<float>(bottom - top) / view_desc_.Height;
  constants.source[0] =
      static_cast<float>(left - popup_bounds_.x) / popup_bounds_.width;
  constants.source[1] =
      static_cast<float>(top - popup_bounds_.y) / popup_bounds_.height;
  constants.source[2] = static_cast<float>(right - left) / popup_bounds_.width;
  constants.source[3] = static_cast<float>(bottom - top) / popup_bounds_.height;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(popup_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                           0, &mapped))) {
    return false;
  }
  *static_cast<PopupConstants*>(mapped.pData) = constants;
  context_->Unmap(popup_constants_.Get(), 0);

  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device_->CreateRenderTargetView(destination, nullptr, &target))) {
    return false;
  }
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(view_desc_.Width);
  viewport.Height = static_cast<float>(view_desc_.Height);
  viewport.MaxDepth = 1.0F;
  context_->RSSetViewports(1, &viewport);
  context_->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
  const float blend_factor[4]{};
  context_->OMSetBlendState(premultiplied_blend_.Get(), blend_factor,
                            0xFFFFFFFFU);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context_->VSSetShader(popup_vertex_shader_.Get(), nullptr, 0);
  context_->VSSetConstantBuffers(0, 1, popup_constants_.GetAddressOf());
  context_->PSSetShader(popup_pixel_shader_.Get(), nullptr, 0);
  context_->PSSetShaderResources(0, 1, popup_srv_.GetAddressOf());
  context_->PSSetSamplers(0, 1, popup_sampler_.GetAddressOf());
  context_->Draw(4, 0);

  ID3D11ShaderResourceView* null_srv = nullptr;
  ID3D11RenderTargetView* null_target = nullptr;
  context_->PSSetShaderResources(0, 1, &null_srv);
  context_->OMSetRenderTargets(1, &null_target, nullptr);
  context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
  return true;
}

bool D3DFramePipeline::ProbeAlpha(ID3D11Texture2D* texture) {
  if (view_desc_.Width <= 1000 || view_desc_.Height <= 1000) {
    return false;
  }
  D3D11_TEXTURE2D_DESC staging_desc = view_desc_;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  Microsoft::WRL::ComPtr<ID3D11Query> completion;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
  if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr, &staging)) ||
      FAILED(device_->CreateQuery(&query_desc, &completion))) {
    return false;
  }
  context_->CopyResource(staging.Get(), texture);
  context_->End(completion.Get());
  if (!WaitForCopy(completion.Get())) {
    return false;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  const auto alpha_at = [&mapped](std::uint32_t x, std::uint32_t y) {
    const auto* row = static_cast<const std::byte*>(mapped.pData) +
                      static_cast<std::size_t>(mapped.RowPitch) * y;
    return std::to_integer<std::uint8_t>(row[x * 4U + 3U]);
  };
  const std::uint8_t opaque = alpha_at(10, 10);
  const std::uint8_t half = alpha_at(300, 100);
  const std::uint8_t transparent = alpha_at(1000, 1000);
  context_->Unmap(staging.Get(), 0);
  return opaque >= 250 && half >= 120 && half <= 136 && transparent <= 5;
}

bool D3DFramePipeline::WaitForCopy(ID3D11Query* completion) {
  const auto deadline = std::chrono::steady_clock::now() + kCopyWatchdog;
  BOOL complete = FALSE;
  for (;;) {
    const HRESULT result = context_->GetData(completion, &complete,
                                             sizeof(complete), 0);
    if (result == S_OK && complete == TRUE) {
      return true;
    }
    if (result != S_FALSE) {
      FailFastGpu(L"D3D11 event query failed", result);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    SwitchToThread();
  }
}

[[noreturn]] void D3DFramePipeline::FailFastGpu(const wchar_t* reason,
                                                HRESULT result) const {
  std::wstring message(reason);
  message.append(L" (HRESULT=");
  message.append(std::to_wstring(static_cast<long long>(result)));
  message.append(L")");
  Log(LogLevel::kError, message);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
  __assume(false);
}

}  // namespace streaming::producer
