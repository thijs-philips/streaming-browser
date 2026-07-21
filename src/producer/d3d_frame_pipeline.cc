#include "src/producer/d3d_frame_pipeline.h"

#include <windows.h>

#include <array>
#include <string>

#include "src/common/logging.h"

namespace streaming::producer {
namespace {

constexpr auto kCopyWatchdog = std::chrono::milliseconds(250);

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

D3DFramePipeline::D3DFramePipeline() {
  const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
  if (FAILED(result)) {
    Log(LogLevel::kError, L"CreateDXGIFactory1 failed; accelerated capture cannot initialize");
  }
}

bool D3DFramePipeline::CopyFromCef(
    CefRenderHandler::PaintElementType type,
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
    next_view_slot_ = (next_view_slot_ + 1U) % view_slots_.size();
  }

  context_->CopyResource(slot->texture.Get(), source.Get());
  context_->End(slot->completion.Get());
  if (!WaitForCopy(slot->completion.Get())) {
    FailFastGpu(L"CEF frame copy did not complete inside OnAcceleratedPaint", E_FAIL);
  }

  slot->frame_id = ++frame_id_;
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
  Log(LogLevel::kInfo, L"Private CEF view capture ring initialized");
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
  return true;
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
