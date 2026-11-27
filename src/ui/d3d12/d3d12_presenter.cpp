/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_hdr.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/surface_win.h>
#include <wincodec.h>
#include <shlobj.h>
#include <dxgi1_6.h>

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#endif

REXCVAR_DEFINE_BOOL(d3d12_allow_variable_refresh_rate_and_tearing, true, "UI/D3D12",
                    "Allow variable refresh rate and tearing");

REXCVAR_DEFINE_INT32(present_screenshot_request, 0, "Display",
                      "Test hook: each change of this value saves one screenshot, like F12");

REXCVAR_DECLARE(bool, present_hdr);
REXCVAR_DECLARE(int32_t, present_hdr_paper_white_nits);
REXCVAR_DECLARE(int32_t, present_hdr_peak_nits);

namespace rex::ui::d3d12 {

namespace {

std::filesystem::path ScreenshotPath() {
  PWSTR pictures = nullptr;
  HRESULT result = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures);
  if (FAILED(result)) {
    throw std::runtime_error("Failed to locate the Pictures folder");
  }
  std::filesystem::path path = std::filesystem::path(pictures) / L"Gears of War 2 PC";
  CoTaskMemFree(pictures);
  std::filesystem::create_directories(path);
  auto now = std::chrono::system_clock::now();
  auto stamp = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_s(&local_time, &stamp);
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count() % 1000;
  std::wostringstream name;
  name << std::put_time(&local_time, L"gow2_%Y-%m-%d_%H-%M-%S-") << std::setw(3)
       << std::setfill(L'0') << millis << L".png";
  return path / name.str();
}

HRESULT WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& pixels,
                 uint32_t width, uint32_t height, uint32_t stride) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  Microsoft::WRL::ComPtr<IWICStream> stream;
  HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
  if (SUCCEEDED(result)) {
    result = factory->CreateStream(&stream);
  }
  if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (SUCCEEDED(result)) {
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
  if (SUCCEEDED(result)) result = frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
  if (SUCCEEDED(result)) {
    result = frame->WritePixels(height, stride, UINT(pixels.size()),
                                const_cast<BYTE*>(pixels.data()));
  }
  if (SUCCEEDED(result)) result = frame->Commit();
  if (SUCCEEDED(result)) result = encoder->Commit();
  return result;
}

void SaveScreenshotPng(std::vector<uint8_t> pixels, uint32_t width, uint32_t height,
                       uint32_t stride) {
  HRESULT init_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    std::filesystem::path path = ScreenshotPath();
    HRESULT result = WritePng(path, pixels, width, height, stride);
    if (SUCCEEDED(result)) {
      REXLOG_INFO("D3D12Presenter: Screenshot saved to {}", path.string());
    } else {
      REXLOG_ERROR("D3D12Presenter: Failed to save screenshot (0x{:08X})", uint32_t(result));
    }
  } catch (const std::exception& error) {
    REXLOG_ERROR("D3D12Presenter: Failed to save screenshot: {}", error.what());
  }
  if (SUCCEEDED(init_result)) CoUninitialize();
}

const char* HDRDisplayFailureReason(IDXGIFactory2* factory, HWND hwnd) {
  HMONITOR target_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  for (UINT adapter_index = 0;; ++adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(adapter_index, &adapter))) {
      break;
    }
    for (UINT output_index = 0;; ++output_index) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      if (FAILED(adapter->EnumOutputs(output_index, &output))) {
        break;
      }
      Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
      if (FAILED(output.As(&output6))) {
        continue;
      }
      DXGI_OUTPUT_DESC1 desc = {};
      if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == target_monitor) {
        return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                   ? nullptr
                   : "display not in HDR";
      }
    }
  }
  return "display not found";
}

}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#if defined(REX_HAS_FIDELITYFX_SDK)
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_dither_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#endif
#include "ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

D3D12Presenter::~D3D12Presenter() {
  // Await completion of the usage of everything before destroying anything.
  // From most likely the latest to most likely the earliest to be signaled, so
  // just one sleep will likely be needed.
  paint_context_.AwaitSwapChainUsageCompletion();
  paint_context_.DestroySwapChain();
  guest_output_resource_refresher_submission_tracker_.Shutdown();
  ui_submission_tracker_.Shutdown();
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  DestroyTemporalUpscalerContext();
#endif
}

void D3D12Presenter::SetSwapChainSource(SwapChainSource* source) {
  swap_chain_source_ = source;
}

Microsoft::WRL::ComPtr<IDXGISwapChain1> D3D12Presenter::CreateSwapChainForHwnd(
    IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1& desc) {
  if (swap_chain_source_) {
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain =
        swap_chain_source_->Create(factory, queue, hwnd, desc);
    if (swap_chain) {
      paint_context_.swap_chain_source = swap_chain_source_;
      paint_context_.swap_chain_from_source = true;
      return swap_chain;
    }
  }
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
  if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr,
                                                 &swap_chain))) {
    return swap_chain;
  }
  return nullptr;
}

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
void D3D12Presenter::DestroyTemporalUpscalerContext() {
  if (!temporal_upscaler_context_) {
    return;
  }
  ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
  ffxDestroyContext(context, nullptr);
  temporal_upscaler_context_ = nullptr;
  temporal_upscaler_max_render_width_ = 0;
  temporal_upscaler_max_render_height_ = 0;
  temporal_upscaler_max_output_width_ = 0;
  temporal_upscaler_max_output_height_ = 0;
}

bool D3D12Presenter::EnsureTemporalUpscalerContext(uint32_t render_width, uint32_t render_height,
                                                   uint32_t output_width, uint32_t output_height) {
  if (!temporal_upscaler_context_ || temporal_upscaler_max_render_width_ != render_width ||
      temporal_upscaler_max_render_height_ != render_height ||
      temporal_upscaler_max_output_width_ != output_width ||
      temporal_upscaler_max_output_height_ != output_height) {
    DestroyTemporalUpscalerContext();

    ffxCreateContextDescUpscale create_desc = {};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create_desc.header.pNext = nullptr;
    create_desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    create_desc.maxRenderSize.width = render_width;
    create_desc.maxRenderSize.height = render_height;
    create_desc.maxUpscaleSize.width = output_width;
    create_desc.maxUpscaleSize.height = output_height;
    create_desc.fpMessage = nullptr;

    ffxCreateBackendDX12Desc backend_desc = {};
    backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend_desc.header.pNext = nullptr;
    backend_desc.device = provider_.GetDevice();

    create_desc.header.pNext = &backend_desc.header;

    ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
    if (ffxCreateContext(context, &create_desc.header, nullptr) != FFX_API_RETURN_OK) {
      REXLOG_WARN("D3D12Presenter: Failed to create FidelityFX temporal upscaler context");
      temporal_upscaler_context_ = nullptr;
      return false;
    }

    temporal_upscaler_max_render_width_ = render_width;
    temporal_upscaler_max_render_height_ = render_height;
    temporal_upscaler_max_output_width_ = output_width;
    temporal_upscaler_max_output_height_ = output_height;
    temporal_upscaler_provider_logged_ = false;
  }

  if (!temporal_upscaler_provider_logged_) {
    ffxQueryGetProviderVersion provider_version = {};
    provider_version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    provider_version.header.pNext = nullptr;
    ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
    if (ffxQuery(context, &provider_version.header) == FFX_API_RETURN_OK &&
        provider_version.versionName) {
      REXLOG_INFO("D3D12Presenter: FidelityFX upscaler provider {}", provider_version.versionName);
    }
    temporal_upscaler_provider_logged_ = true;
  }

  return temporal_upscaler_context_ != nullptr;
}

bool D3D12Presenter::DispatchTemporalUpscaler(ID3D12GraphicsCommandList* command_list,
                                              ID3D12Resource* input_resource, uint32_t input_width,
                                              uint32_t input_height,
                                              ID3D12Resource* output_resource,
                                              uint32_t output_width, uint32_t output_height,
                                              const GuestOutputPaintConfig& config) {
  if (!command_list || !input_resource || !output_resource || !input_width || !input_height ||
      !output_width || !output_height) {
    return false;
  }
  if (!EnsureTemporalUpscalerContext(input_width, input_height, output_width, output_height)) {
    return false;
  }

  ffxDispatchDescUpscale dispatch_desc = {};
  dispatch_desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  dispatch_desc.header.pNext = nullptr;
  dispatch_desc.commandList = command_list;
  dispatch_desc.color =
      ffxApiGetResourceDX12(input_resource, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch_desc.depth =
      ffxApiGetResourceDX12(input_resource, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                            FFX_API_RESOURCE_USAGE_DEPTHTARGET);
  dispatch_desc.motionVectors =
      ffxApiGetResourceDX12(input_resource, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch_desc.exposure =
      ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch_desc.reactive =
      ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch_desc.transparencyAndComposition =
      ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch_desc.output = ffxApiGetResourceDX12(
      output_resource, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
  dispatch_desc.jitterOffset.x = 0.0f;
  dispatch_desc.jitterOffset.y = 0.0f;
  dispatch_desc.motionVectorScale.x = float(input_width);
  dispatch_desc.motionVectorScale.y = float(input_height);
  dispatch_desc.renderSize.width = input_width;
  dispatch_desc.renderSize.height = input_height;
  dispatch_desc.upscaleSize.width = output_width;
  dispatch_desc.upscaleSize.height = output_height;
  dispatch_desc.enableSharpening = true;
  dispatch_desc.sharpness = std::clamp(1.0f - config.GetFsrSharpnessReduction() * 0.5f, 0.0f, 1.0f);
  // The presenter path doesn't currently provide accurate temporal inputs,
  // so run in reset mode each frame to avoid history artifacts.
  dispatch_desc.reset = true;
  dispatch_desc.frameTimeDelta = 16.666f;
  dispatch_desc.preExposure = 1.0f;
  dispatch_desc.cameraNear = 0.1f;
  dispatch_desc.cameraFar = 1000.0f;
  dispatch_desc.cameraFovAngleVertical = 1.0472f;
  dispatch_desc.viewSpaceToMetersFactor = 1.0f;
  dispatch_desc.flags = 0;

  ffxContext* context = reinterpret_cast<ffxContext*>(&temporal_upscaler_context_);
  if (ffxDispatch(context, &dispatch_desc.header) != FFX_API_RETURN_OK) {
    return false;
  }

  return true;
}
#endif

Surface::TypeFlags D3D12Presenter::GetSupportedSurfaceTypes() const {
  Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= Surface::kTypeFlag_Win32Hwnd;
#endif
  return types;
}

bool D3D12Presenter::CaptureGuestOutput(RawImage& image_out) {
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_resource;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_resource = guest_output_resources_[guest_output_mailbox_index].second;
    }
    // Incremented the reference count of the guest output resource - safe to
    // leave the consumer critical section now.
  }
  if (!guest_output_resource) {
    return false;
  }

  ID3D12Device* device = provider_.GetDevice();

  D3D12_RESOURCE_DESC texture_desc = guest_output_resource->GetDesc();
  D3D12_TEXTURE_COPY_LOCATION copy_dest;
  UINT64 copy_dest_size;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &copy_dest.PlacedFootprint, nullptr,
                                nullptr, &copy_dest_size);

  D3D12_RESOURCE_DESC buffer_desc;
  util::FillBufferResourceDesc(buffer_desc, copy_dest_size, D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  // Create zeroed not to leak data in the row padding.
  if (FAILED(device->CreateCommittedResource(&util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&buffer)))) {
    REXLOG_ERROR("D3D12Presenter: Failed to create the guest output capture buffer");
    return false;
  }

  {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&command_allocator)))) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output capturing command "
          "allocator");
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&command_list)))) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output capturing command "
          "list");
      return false;
    }

    D3D12_RESOURCE_BARRIER barrier;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = guest_output_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = kGuestOutputInternalState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if constexpr (kGuestOutputInternalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
      command_list->ResourceBarrier(1, &barrier);
    }
    copy_dest.pResource = buffer.Get();
    copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_TEXTURE_COPY_LOCATION copy_source;
    copy_source.pResource = guest_output_resource.Get();
    copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_source.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, nullptr);
    if constexpr (kGuestOutputInternalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      command_list->ResourceBarrier(1, &barrier);
    }
    if (FAILED(command_list->Close())) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to close the guest output capturing command "
          "list");
      return false;
    }

    ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();

    // Make sure that if any work is submitted, any `return` will cause an await
    // before releasing the command allocator / list and the resource the RAII
    // way in the destruction of the submission tracker - so create after the
    // objects referenced in the submission - but don't submit anything if
    // failed to initialize the fence.
    D3D12SubmissionTracker submission_tracker;
    if (!submission_tracker.Initialize(device, direct_queue)) {
      return false;
    }
    ID3D12CommandList* execute_command_list = command_list.Get();
    direct_queue->ExecuteCommandLists(1, &execute_command_list);
    if (!submission_tracker.NextSubmission()) {
      REXLOG_ERROR("D3D12Presenter: Failed to signal the guest output capturing fence");
      return false;
    }
    if (!submission_tracker.AwaitAllSubmissionsCompletion()) {
      REXLOG_ERROR("D3D12Presenter: Failed to await the guest output capturing fence");
      return false;
    }
  }

  D3D12_RANGE read_range;
  read_range.Begin = copy_dest.PlacedFootprint.Offset;
  read_range.End = copy_dest_size;
  void* mapping;
  if (FAILED(buffer->Map(0, &read_range, &mapping))) {
    REXLOG_ERROR("D3D12Presenter: Failed to map the guest output capture buffer");
    return false;
  }
  image_out.width = uint32_t(texture_desc.Width);
  image_out.height = uint32_t(texture_desc.Height);
  image_out.stride = sizeof(uint32_t) * image_out.width;
  image_out.data.resize(image_out.stride * image_out.height);
  uint32_t* image_out_pixels = reinterpret_cast<uint32_t*>(image_out.data.data());
  for (uint32_t y = 0; y < image_out.height; ++y) {
    uint32_t* dest_row = &image_out_pixels[size_t(image_out.width) * y];
    const uint32_t* source_row = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(mapping) + copy_dest.PlacedFootprint.Offset +
        size_t(copy_dest.PlacedFootprint.Footprint.RowPitch) * y);
    for (uint32_t x = 0; x < image_out.width; ++x) {
      dest_row[x] = Packed10bpcRGBTo8bpcBytes(source_row[x]);
    }
  }
  // Unmapping will be done implicitly when the resource goes out of scope and
  // gets destroyed.
  return true;
}

void D3D12Presenter::RequestScreenshot() {
  screenshot_requested_.store(true, std::memory_order_release);
}

Presenter::SurfacePaintConnectResult
D3D12Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                uint32_t new_surface_width,
                                                                uint32_t new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  uint32_t new_swap_chain_width =
      std::min(new_surface_width, uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  uint32_t new_swap_chain_height =
      std::min(new_surface_height, uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  // ConnectOrReconnectPaintingToSurfaceFromUIThread may be called only for the
  // surface of the current swap chain or when the old swap chain has already
  // been destroyed, if the surface is the same, try resizing.
  if (paint_context_.swap_chain) {
    if (was_paintable && paint_context_.swap_chain_width == new_swap_chain_width &&
        paint_context_.swap_chain_height == new_swap_chain_height) {
      is_vsync_implicit_out = false;
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    paint_context_.AwaitSwapChainUsageCompletion();
    paint_context_.hdr_picture.Reset();
    paint_context_.hdr_ui.Reset();
    // Using the current swap_chain_allows_tearing_ value that's consistent with
    // the creation of the swap chain because ResizeBuffers can't toggle the
    // tearing flag.
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref :
         paint_context_.swap_chain_buffers) {
      swap_chain_buffer_ref.Reset();
    }
    bool swap_chain_resized = SUCCEEDED(paint_context_.swap_chain->ResizeBuffers(
        0, UINT(new_swap_chain_width), UINT(new_swap_chain_height), DXGI_FORMAT_UNKNOWN,
        paint_context_.swap_chain_allows_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    if (swap_chain_resized) {
      for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
        if (FAILED(paint_context_.swap_chain->GetBuffer(
                i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
          swap_chain_resized = false;
          break;
        }
      }
      if (swap_chain_resized) {
        paint_context_.swap_chain_width = new_swap_chain_width;
        paint_context_.swap_chain_height = new_swap_chain_height;
      }
    }
    if (!swap_chain_resized) {
      REXLOG_ERROR("D3D12Presenter: Failed to resize a swap chain");
      // Failed to resize, retry creating from scratch.
      paint_context_.DestroySwapChain();
    }
  }

  if (!paint_context_.swap_chain) {
    // Create a new swap chain.
    Surface::TypeIndex surface_type = new_surface.GetType();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    swap_chain_desc.Width = UINT(new_swap_chain_width);
    swap_chain_desc.Height = UINT(new_swap_chain_height);
    bool hdr_requested = REXCVAR_GET(present_hdr);
    bool hdr_enabled = false;
    std::string hdr_reason = "cvar off";
    swap_chain_desc.Stereo = FALSE;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = UINT(PaintContext::kSwapChainBufferCount);
    // DXGI_SCALING_STRETCH may cause the content to "shake" while resizing,
    // with relayout done for the guest output twice visually rather than once,
    // and the UI becoming stretched and then jumping to normal. If it's
    // possible to cover the entire surface without stretching, don't stretch.
    // After resizing, the presenter repaints as soon as possible anyway, so
    swap_chain_desc.Scaling =
        (new_swap_chain_width == new_surface_width && new_swap_chain_height == new_surface_height)
            ? DXGI_SCALING_NONE
            : DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_chain_desc.Flags = 0;
    if (REXCVAR_GET(d3d12_allow_variable_refresh_rate_and_tearing) && dxgi_supports_tearing_) {
      // Allow tearing in borderless fullscreen to support variable refresh
      // rate.
      swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    IDXGIFactory2* dxgi_factory = provider_.GetDXGIFactory();
    ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
    HWND surface_hwnd = nullptr;
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case Surface::kTypeIndex_Win32Hwnd: {
        surface_hwnd = static_cast<const Win32HwndSurface&>(new_surface).hwnd();
        if (hdr_requested) {
          if (!hdr_compose_pipeline_.pipeline) {
            hdr_reason = "shader compile failed";
          } else {
            const char* display_failure = HDRDisplayFailureReason(dxgi_factory, surface_hwnd);
            if (display_failure) {
              hdr_reason = display_failure;
            } else {
              hdr_enabled = true;
            }
          }
        }
        swap_chain_desc.Format = hdr_enabled ? DXGI_FORMAT_R10G10B10A2_UNORM : kSwapChainFormat;
        swap_chain_1 = CreateSwapChainForHwnd(dxgi_factory, direct_queue, surface_hwnd,
                                              swap_chain_desc);
        if (!swap_chain_1) {
          REXLOG_ERROR("D3D12Presenter: Failed to create a swap chain for the HWND");
          return SurfacePaintConnectResult::kFailure;
        }
        // Disable automatic Alt+Enter handling - DXGI fullscreen doesn't
        // support ALLOW_TEARING, and the window implementation provides
        // borderless fullscreen anyway with better state tracking.
        dxgi_factory->MakeWindowAssociation(surface_hwnd, DXGI_MWA_NO_ALT_ENTER);
      } break;
#endif
      default:
        assert_unhandled_case(surface_type);
        REXLOG_ERROR(
            "D3D12Presenter: Tried to create a swap chain for an unsupported "
            "Xenia surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_3;
    HRESULT swap_chain_result = swap_chain_1.As(&swap_chain_3);
    if (FAILED(swap_chain_result)) {
      REXLOG_ERROR("D3D12Presenter: Failed to get version 3 of the swap chain interface");
      swap_chain_1.Reset();
      paint_context_.DestroySwapChain();
      return SurfacePaintConnectResult::kFailure;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swap_chain_4;
    if (SUCCEEDED(swap_chain_result) && hdr_enabled) {
      swap_chain_result = swap_chain_1.As(&swap_chain_4);
      if (FAILED(swap_chain_result)) {
        hdr_reason = "HDR swap chain interface unsupported";
      }
    }
    if (SUCCEEDED(swap_chain_result) && hdr_enabled) {
      UINT support = 0;
      swap_chain_result = swap_chain_4->CheckColorSpaceSupport(
          DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
      if (SUCCEEDED(swap_chain_result) &&
          (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        swap_chain_result = swap_chain_4->SetColorSpace1(
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (FAILED(swap_chain_result)) {
          hdr_reason = "SetColorSpace1 failed";
        }
      } else {
        hdr_reason = "colour space unsupported";
        if (SUCCEEDED(swap_chain_result)) {
          swap_chain_result = DXGI_ERROR_UNSUPPORTED;
        }
      }
      if (SUCCEEDED(swap_chain_result)) {
        uint32_t paper_white = uint32_t(REXCVAR_GET(present_hdr_paper_white_nits));
        uint32_t peak = uint32_t(REXCVAR_GET(present_hdr_peak_nits));
        swap_chain_result = hdr::SetMetadata(swap_chain_4.Get(), paper_white, peak);
        if (SUCCEEDED(swap_chain_result)) {
          paint_context_.hdr_paper_white_nits = paper_white;
          paint_context_.hdr_peak_nits = peak;
        } else {
          hdr_reason = "HDR metadata setup failed";
        }
      }
    }
    if (hdr_enabled && FAILED(swap_chain_result)) {
      REXLOG_WARN("D3D12Presenter: HDR10 swap chain setup failed (0x{:08X}); retrying SDR",
                  uint32_t(swap_chain_result));
      hdr_enabled = false;
      swap_chain_4.Reset();
      swap_chain_3.Reset();
      swap_chain_1.Reset();
      swap_chain_desc.Format = kSwapChainFormat;
      if (paint_context_.swap_chain_from_source && paint_context_.swap_chain_source) {
        paint_context_.swap_chain_source->Released();
        paint_context_.swap_chain_from_source = false;
        paint_context_.swap_chain_source = nullptr;
      }
      swap_chain_1 = CreateSwapChainForHwnd(dxgi_factory, direct_queue, surface_hwnd,
                                            swap_chain_desc);
      if (!swap_chain_1 || FAILED(swap_chain_1.As(&swap_chain_3))) {
        REXLOG_ERROR("D3D12Presenter: Failed to recreate the swap chain in SDR");
        swap_chain_1.Reset();
        paint_context_.DestroySwapChain();
        return SurfacePaintConnectResult::kFailure;
      }
      swap_chain_result = S_OK;
    }
    paint_context_.swap_chain = swap_chain_3;
    if (paint_context_.swap_chain_from_source) {
      paint_context_.swap_chain_source = swap_chain_source_;
    }
    if (hdr_enabled) {
      paint_context_.hdr_swap_chain = swap_chain_4;
    } else {
      paint_context_.hdr_swap_chain.Reset();
    }
    paint_context_.hdr_enabled = hdr_enabled;
    if (hdr_enabled) {
      REXLOG_INFO("HDR10: on (paper white {} nits, peak {} nits)",
                  REXCVAR_GET(present_hdr_paper_white_nits),
                  REXCVAR_GET(present_hdr_peak_nits));
    } else {
      REXLOG_INFO("HDR10: off ({})", hdr_requested ? hdr_reason : "cvar off");
    }
    // From now on, in case of any failure, DestroySwapChain must be called
    // before returning.
    paint_context_.swap_chain_width = new_swap_chain_width;
    paint_context_.swap_chain_height = new_swap_chain_height;
    paint_context_.swap_chain_allows_tearing =
        (swap_chain_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
      if (FAILED(paint_context_.swap_chain->GetBuffer(
              i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
        REXLOG_ERROR("D3D12Presenter: Failed to get buffer {} of a {}-buffer swap chain", i,
                     PaintContext::kSwapChainBufferCount);
        paint_context_.DestroySwapChain();
        return SurfacePaintConnectResult::kFailure;
      }
    }
  }

  ID3D12Device* device = provider_.GetDevice();

  if (paint_context_.hdr_enabled) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
        paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE view_heap_start =
        paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart();
    auto create_hdr_target = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& target,
                                 PaintContext::RTVIndex rtv_index,
                                 PaintContext::ViewIndex srv_index) {
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = new_swap_chain_width;
      desc.Height = new_swap_chain_height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = kSwapChainFormat;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_CLEAR_VALUE clear_value = {};
      clear_value.Format = kSwapChainFormat;
      clear_value.Color[3] = 0.0f;
      if (FAILED(device->CreateCommittedResource(
              &util::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(), &desc,
              D3D12_RESOURCE_STATE_COMMON, &clear_value, IID_PPV_ARGS(&target)))) {
        return false;
      }
      D3D12_RENDER_TARGET_VIEW_DESC target_rtv = {};
      target_rtv.Format = kSwapChainFormat;
      target_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      device->CreateRenderTargetView(
          target.Get(), &target_rtv,
          provider_.OffsetRTVDescriptor(rtv_heap_start, uint32_t(rtv_index)));
      D3D12_SHADER_RESOURCE_VIEW_DESC target_srv = {};
      target_srv.Format = kSwapChainFormat;
      target_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      target_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      target_srv.Texture2D.MipLevels = 1;
      device->CreateShaderResourceView(
          target.Get(), &target_srv,
          provider_.OffsetViewDescriptor(view_heap_start, uint32_t(srv_index)));
      return true;
    };
    paint_context_.hdr_picture.Reset();
    paint_context_.hdr_ui.Reset();
    if (!create_hdr_target(paint_context_.hdr_picture, PaintContext::kRTVIndexHDRPicture,
                           PaintContext::kViewIndexHDRPictureSrv) ||
        !create_hdr_target(paint_context_.hdr_ui, PaintContext::kRTVIndexHDRUI,
                           PaintContext::kViewIndexHDRUISrv)) {
      REXLOG_ERROR("D3D12Presenter: Failed to create HDR composition textures");
      paint_context_.DestroySwapChain();
      return SurfacePaintConnectResult::kFailure;
    }
  }

  // Create the RTV descriptors.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
  rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  rtv_desc.Texture2D.MipSlice = 0;
  rtv_desc.Texture2D.PlaneSlice = 0;
  for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
    ID3D12Resource* swap_chain_buffer = paint_context_.swap_chain_buffers[i].Get();
    rtv_desc.Format = paint_context_.hdr_enabled ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                 : kSwapChainFormat;
    device->CreateRenderTargetView(
        swap_chain_buffer, &rtv_desc,
        provider_.OffsetRTVDescriptor(rtv_heap_start, PaintContext::kRTVIndexSwapChainBuffer0 + i));
  }

  is_vsync_implicit_out = false;
  return SurfacePaintConnectResult::kSuccess;
}

void D3D12Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapChain();
}

bool D3D12Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>& guest_output_resource_ref =
      guest_output_resources_[mailbox_index];
  if (guest_output_resource_ref.second) {
    D3D12_RESOURCE_DESC guest_output_resource_current_desc =
        guest_output_resource_ref.second->GetDesc();
    if (guest_output_resource_current_desc.Width != frontbuffer_width ||
        guest_output_resource_current_desc.Height != frontbuffer_height) {
      // Main target painting has its own reference to the textures for reading
      // in its own submission tracker timeline, safe to release here.
      guest_output_resource_refresher_submission_tracker_.AwaitSubmissionCompletion(
          guest_output_resource_ref.first);
      guest_output_resource_ref.second.Reset();
    }
  }
  if (!guest_output_resource_ref.second) {
    ID3D12Device* device = provider_.GetDevice();
    D3D12_RESOURCE_DESC guest_output_resource_new_desc;
    guest_output_resource_new_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    guest_output_resource_new_desc.Alignment = 0;
    guest_output_resource_new_desc.Width = frontbuffer_width;
    guest_output_resource_new_desc.Height = frontbuffer_height;
    guest_output_resource_new_desc.DepthOrArraySize = 1;
    guest_output_resource_new_desc.MipLevels = 1;
    guest_output_resource_new_desc.Format = kGuestOutputFormat;
    guest_output_resource_new_desc.SampleDesc.Count = 1;
    guest_output_resource_new_desc.SampleDesc.Quality = 0;
    guest_output_resource_new_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    guest_output_resource_new_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(
            &util::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(),
            &guest_output_resource_new_desc, kGuestOutputInternalState, nullptr,
            IID_PPV_ARGS(&guest_output_resource_ref.second)))) {
      REXLOG_ERROR("D3D12Presenter: Failed to create the guest output {}x{} texture",
                   frontbuffer_width, frontbuffer_height);
      return false;
    }
  }
  D3D12GuestOutputRefreshContext context(is_8bpc_out_ref, guest_output_resource_ref.second.Get());
  bool refresher_succeeded = refresher(context);
  // Even if the refresher has returned false, it still might have submitted
  // some commands referencing the resource. It's better to put an excessive
  // signal and wait slightly longer, for nothing important, while shutting down
  // than to destroy the resource while it's still in use.
  guest_output_resource_ref.first =
      guest_output_resource_refresher_submission_tracker_.GetCurrentSubmission();
  guest_output_resource_refresher_submission_tracker_.NextSubmission();
  return refresher_succeeded;
}

void D3D12Presenter::PaintContext::DestroySwapChain() {
  if (!swap_chain && !swap_chain_from_source) {
    return;
  }
  AwaitSwapChainUsageCompletion();
  hdr_picture.Reset();
  hdr_ui.Reset();
  for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref : swap_chain_buffers) {
    swap_chain_buffer_ref.Reset();
  }
  swap_chain.Reset();
  hdr_swap_chain.Reset();
  if (swap_chain_from_source && swap_chain_source) {
    swap_chain_source->Released();
  }
  swap_chain_source = nullptr;
  swap_chain_from_source = false;
  picture_rect_valid = false;
  picture_rect_x = 0;
  picture_rect_y = 0;
  picture_rect_width = 0;
  picture_rect_height = 0;
  swap_chain_allows_tearing = false;
  hdr_enabled = false;
  hdr_paper_white_nits = 0;
  hdr_peak_nits = 0;
  swap_chain_height = 0;
  swap_chain_width = 0;
}

Presenter::PaintResult D3D12Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  Microsoft::WRL::ComPtr<ID3D12Resource> screenshot_buffer;
  Microsoft::WRL::ComPtr<ID3D12Fence> screenshot_fence;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT screenshot_footprint = {};
  UINT64 screenshot_buffer_size = 0;
  uint32_t screenshot_width = 0;
  uint32_t screenshot_height = 0;
  UINT64 screenshot_fence_value = 1;
  // Begin the command list with the command allocator not currently potentially
  // used on the GPU.
  UINT64 current_paint_submission = paint_context_.paint_submission_tracker.GetCurrentSubmission();
  UINT64 command_allocator_count = UINT64(paint_context_.command_allocators.size());
  if (current_paint_submission >= command_allocator_count) {
    paint_context_.paint_submission_tracker.AwaitSubmissionCompletion(current_paint_submission -
                                                                      command_allocator_count);
  }
  ID3D12CommandAllocator* command_allocator =
      paint_context_.command_allocators[current_paint_submission % command_allocator_count].Get();
  command_allocator->Reset();
  ID3D12GraphicsCommandList* command_list = paint_context_.command_list.Get();
  command_list->Reset(command_allocator, nullptr);

  ID3D12Device* device = provider_.GetDevice();

  // Obtain the RTV heap and the back buffer.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  UINT back_buffer_index = paint_context_.swap_chain->GetCurrentBackBufferIndex();
  ID3D12Resource* present_buffer = paint_context_.swap_chain_buffers[back_buffer_index].Get();
  D3D12_CPU_DESCRIPTOR_HANDLE present_buffer_rtv = provider_.OffsetRTVDescriptor(
      rtv_heap_start, PaintContext::kRTVIndexSwapChainBuffer0 + back_buffer_index);
  ID3D12Resource* back_buffer = paint_context_.hdr_enabled
                                    ? paint_context_.hdr_picture.Get()
                                    : present_buffer;
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv = paint_context_.hdr_enabled
                                                    ? provider_.OffsetRTVDescriptor(
                                                          rtv_heap_start,
                                                          PaintContext::kRTVIndexHDRPicture)
                                                    : present_buffer_rtv;
  bool back_buffer_acquired = false;
  bool back_buffer_bound = false;
  bool back_buffer_clear_needed = true;
  const float kBackBufferClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};

  // Draw the guest output.

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_resource;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(ConsumeGuestOutput(
        guest_output_mailbox_index, &guest_output_properties, &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_resource = guest_output_resources_[guest_output_mailbox_index].second;
    }
    // Incremented the reference count of the guest output resource - safe to
    // leave the consumer critical section now as everything here either will be
    // using the new reference or is exclusively owned by main target painting
    // (and multiple threads can't paint the main target at the same time).
  }

  if (guest_output_resource) {
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swap_chain_width, paint_context_.swap_chain_height,
        D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        guest_output_paint_config);

    // Check if all guest output paint effects are supported by the
    // implementation.
    if (guest_output_flow.effect_count) {
      if (!guest_output_paint_final_pipelines_[size_t(
              guest_output_flow.effects[guest_output_flow.effect_count - 1])]) {
        guest_output_flow.effect_count = 0;
      }
      for (size_t i = 0; i + 1 < guest_output_flow.effect_count; ++i) {
        if (!guest_output_paint_intermediate_pipelines_[size_t(guest_output_flow.effects[i])]) {
          guest_output_flow.effect_count = 0;
          break;
        }
      }
    }

    if (guest_output_flow.effect_count) {
      ID3D12DescriptorHeap* view_heap = paint_context_.view_heap.Get();
      D3D12_CPU_DESCRIPTOR_HANDLE view_heap_cpu_start =
          view_heap->GetCPUDescriptorHandleForHeapStart();

      // Store the main target reference to the guest output texture so it's not
      // destroyed while it's still potentially in use by main target painting
      // queued on the GPU.
      size_t guest_output_resource_paint_ref_index = SIZE_MAX;
      size_t guest_output_resource_paint_ref_new_index = SIZE_MAX;
      // Try to find the existing reference to the same texture, or an already
      // released (or a taken, but never actually used) slot.
      for (size_t i = 0; i < paint_context_.guest_output_resource_paint_refs.size(); ++i) {
        const std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>&
            guest_output_resource_paint_ref = paint_context_.guest_output_resource_paint_refs[i];
        if (guest_output_resource_paint_ref.second == guest_output_resource) {
          guest_output_resource_paint_ref_index = i;
          break;
        }
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX &&
            (!guest_output_resource_paint_ref.second || !guest_output_resource_paint_ref.first)) {
          guest_output_resource_paint_ref_new_index = i;
        }
      }
      if (guest_output_resource_paint_ref_index == SIZE_MAX) {
        // New texture - store the reference and create the descriptors.
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX) {
          // Replace the earliest used reference.
          guest_output_resource_paint_ref_new_index = 0;
          for (size_t i = 1; i < paint_context_.guest_output_resource_paint_refs.size(); ++i) {
            if (paint_context_.guest_output_resource_paint_refs[i].first <
                paint_context_
                    .guest_output_resource_paint_refs[guest_output_resource_paint_ref_new_index]
                    .first) {
              guest_output_resource_paint_ref_new_index = i;
            }
          }
          // Await the completion of the usage of the old guest output
          // resource and its SRV descriptors.
          paint_context_.paint_submission_tracker.AwaitSubmissionCompletion(
              paint_context_
                  .guest_output_resource_paint_refs[guest_output_resource_paint_ref_new_index]
                  .first);
        }
        guest_output_resource_paint_ref_index = guest_output_resource_paint_ref_new_index;
        // The actual submission index will be set if the texture is actually
        // used, not dropped due to some error.
        paint_context_.guest_output_resource_paint_refs[guest_output_resource_paint_ref_index] =
            std::make_pair(UINT64(0), guest_output_resource);
        // Create the SRV descriptor of the new texture.
        D3D12_SHADER_RESOURCE_VIEW_DESC guest_output_resource_srv_desc;
        guest_output_resource_srv_desc.Format = kGuestOutputFormat;
        guest_output_resource_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        guest_output_resource_srv_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        guest_output_resource_srv_desc.Texture2D.MostDetailedMip = 0;
        guest_output_resource_srv_desc.Texture2D.MipLevels = 1;
        guest_output_resource_srv_desc.Texture2D.PlaneSlice = 0;
        guest_output_resource_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(
            guest_output_resource.Get(), &guest_output_resource_srv_desc,
            provider_.OffsetViewDescriptor(view_heap_cpu_start,
                                           PaintContext::kViewIndexGuestOutput0Srv +
                                               uint32_t(guest_output_resource_paint_ref_index)));
      }

      // Make sure intermediate textures of the needed size are available, and
      // unneeded intermediate textures are destroyed.
      for (size_t i = 0; i < kMaxGuestOutputPaintEffects - 1; ++i) {
        std::pair<uint32_t, uint32_t> intermediate_needed_size(0, 0);
        if (i + 1 < guest_output_flow.effect_count) {
          intermediate_needed_size = guest_output_flow.effect_output_sizes[i];
        }
        Microsoft::WRL::ComPtr<ID3D12Resource>& intermediate_texture_ptr_ref =
            paint_context_.guest_output_intermediate_textures[i];
        std::pair<uint32_t, uint32_t> intermediate_current_size(0, 0);
        if (intermediate_texture_ptr_ref) {
          D3D12_RESOURCE_DESC intermediate_current_desc = intermediate_texture_ptr_ref->GetDesc();
          intermediate_current_size.first = uint32_t(intermediate_current_desc.Width);
          intermediate_current_size.second = intermediate_current_desc.Height;
        }
        if (intermediate_current_size != intermediate_needed_size) {
          if (intermediate_needed_size.first && intermediate_needed_size.second) {
            // Need to replace immediately as a new texture with the requested
            // size is needed.
            if (intermediate_texture_ptr_ref) {
              paint_context_.paint_submission_tracker.AwaitSubmissionCompletion(
                  paint_context_.guest_output_intermediate_texture_last_usage);
              intermediate_texture_ptr_ref.Reset();
            }
            // Resource.
            D3D12_RESOURCE_DESC intermediate_desc;
            intermediate_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            intermediate_desc.Alignment = 0;
            intermediate_desc.Width = intermediate_needed_size.first;
            intermediate_desc.Height = intermediate_needed_size.second;
            intermediate_desc.DepthOrArraySize = 1;
            intermediate_desc.MipLevels = 1;
            intermediate_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_desc.SampleDesc.Count = 1;
            intermediate_desc.SampleDesc.Quality = 0;
            intermediate_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            intermediate_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &util::kHeapPropertiesDefault, provider_.GetHeapFlagCreateNotZeroed(),
                    &intermediate_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&intermediate_texture_ptr_ref)))) {
              REXLOG_ERROR(
                  "D3D12Presenter: Failed to create a guest output "
                  "presentation intermediate texture");
              // Don't display the guest output, and don't try to create more
              // intermediate textures (only destroy them).
              guest_output_flow.effect_count = 0;
              continue;
            }
            ID3D12Resource* intermediate_texture = intermediate_texture_ptr_ref.Get();
            // SRV.
            D3D12_SHADER_RESOURCE_VIEW_DESC intermediate_srv_desc;
            intermediate_srv_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            intermediate_srv_desc.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            intermediate_srv_desc.Texture2D.MostDetailedMip = 0;
            intermediate_srv_desc.Texture2D.MipLevels = 1;
            intermediate_srv_desc.Texture2D.PlaneSlice = 0;
            intermediate_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(
                intermediate_texture, &intermediate_srv_desc,
                provider_.OffsetViewDescriptor(
                    view_heap_cpu_start,
                    uint32_t(PaintContext::kViewIndexGuestOutputIntermediate0Srv + i)));
            // RTV.
            D3D12_RENDER_TARGET_VIEW_DESC intermediate_rtv_desc;
            intermediate_rtv_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            intermediate_rtv_desc.Texture2D.MipSlice = 0;
            intermediate_rtv_desc.Texture2D.PlaneSlice = 0;
            device->CreateRenderTargetView(
                intermediate_texture, &intermediate_rtv_desc,
                provider_.OffsetRTVDescriptor(
                    rtv_heap_start, uint32_t(PaintContext::kRTVIndexGuestOutputIntermediate0 + i)));
          } else {
            // Was previously needed, but not anymore - destroy when possible.
            if (intermediate_texture_ptr_ref &&
                paint_context_.paint_submission_tracker.GetCompletedSubmission() >=
                    paint_context_.guest_output_intermediate_texture_last_usage) {
              intermediate_texture_ptr_ref.Reset();
            }
          }
        }
      }

      if (guest_output_flow.effect_count) {
        paint_context_.guest_output_resource_paint_refs[guest_output_resource_paint_ref_index]
            .first = current_paint_submission;
        if (guest_output_flow.effect_count > 1) {
          paint_context_.guest_output_intermediate_texture_last_usage = current_paint_submission;
        }

        command_list->SetDescriptorHeaps(1, &view_heap);
      }

      // This effect loop must not be aborted so the states of the resources
      // involved are consistent.
      D3D12_GPU_DESCRIPTOR_HANDLE view_heap_gpu_start =
          view_heap->GetGPUDescriptorHandleForHeapStart();
      bool temporal_effect_selected = false;
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      temporal_effect_selected =
          guest_output_paint_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
          guest_output_paint_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3;
#endif
      auto is_temporal_easu_effect = [&]([[maybe_unused]] size_t effect_index) {
#if defined(REX_HAS_FIDELITYFX_SDK)
        return temporal_effect_selected &&
               guest_output_flow.effects[effect_index] == GuestOutputPaintEffect::kFsrEasu;
#else
        return false;
#endif
      };
      for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
        bool is_final_effect = i + 1 >= guest_output_flow.effect_count;

        GuestOutputPaintEffect effect = guest_output_flow.effects[i];
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
        bool use_temporal_upscaler = is_temporal_easu_effect(i);
#else
        bool use_temporal_upscaler = false;
#endif

        ID3D12Resource* effect_dest_resource;
        int32_t effect_rect_x, effect_rect_y;
        D3D12_RESOURCE_STATES non_final_write_state = use_temporal_upscaler
                                                          ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                          : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (is_final_effect) {
          effect_dest_resource = back_buffer;
          if (!back_buffer_acquired) {
            D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
            barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_present_to_rtv.Transition.pResource = back_buffer;
            barrier_present_to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_present_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier_present_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            command_list->ResourceBarrier(1, &barrier_present_to_rtv);
            back_buffer_acquired = true;
          }
          effect_rect_x = guest_output_flow.output_x;
          effect_rect_y = guest_output_flow.output_y;
        } else {
          effect_dest_resource = paint_context_.guest_output_intermediate_textures[i].Get();
          if (!i) {
            // If this is not the first effect, the transition has been done at
            // the end of the previous effect in a single command.
            D3D12_RESOURCE_BARRIER barrier_srv_to_write;
            barrier_srv_to_write.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_srv_to_write.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_srv_to_write.Transition.pResource = effect_dest_resource;
            barrier_srv_to_write.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_srv_to_write.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier_srv_to_write.Transition.StateAfter = non_final_write_state;
            command_list->ResourceBarrier(1, &barrier_srv_to_write);
          }
          if (!use_temporal_upscaler) {
            command_list->DiscardResource(effect_dest_resource, nullptr);
          }
          effect_rect_x = 0;
          effect_rect_y = 0;
        }

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
        bool temporal_dispatch_done = false;
        if (use_temporal_upscaler) {
          uint32_t effect_input_width = 0, effect_input_height = 0;
          guest_output_flow.GetEffectInputSize(i, effect_input_width, effect_input_height);
          ID3D12Resource* effect_source_resource =
              i ? paint_context_.guest_output_intermediate_textures[i - 1].Get()
                : guest_output_resource.Get();
          temporal_dispatch_done = DispatchTemporalUpscaler(
              command_list, effect_source_resource, effect_input_width, effect_input_height,
              effect_dest_resource, guest_output_flow.effect_output_sizes[i].first,
              guest_output_flow.effect_output_sizes[i].second, guest_output_paint_config);
          if (!temporal_dispatch_done) {
            D3D12_RESOURCE_BARRIER barrier_uav_to_rtv;
            barrier_uav_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_uav_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_uav_to_rtv.Transition.pResource = effect_dest_resource;
            barrier_uav_to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_uav_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier_uav_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            command_list->ResourceBarrier(1, &barrier_uav_to_rtv);
          }
        }
#else
        bool temporal_dispatch_done = false;
#endif

        bool drew_graphics_effect = false;
        if (!temporal_dispatch_done) {
          if (is_final_effect) {
            if (!back_buffer_bound) {
              command_list->OMSetRenderTargets(1, &back_buffer_rtv, TRUE, nullptr);
              back_buffer_bound = true;
            }
          } else {
            D3D12_CPU_DESCRIPTOR_HANDLE intermediate_rtv = provider_.OffsetRTVDescriptor(
                rtv_heap_start, uint32_t(PaintContext::kRTVIndexGuestOutputIntermediate0 + i));
            command_list->OMSetRenderTargets(1, &intermediate_rtv, TRUE, nullptr);
            back_buffer_bound = false;
          }
          if (is_final_effect) {
            back_buffer_bound = true;
          }
          D3D12_RESOURCE_DESC effect_dest_resource_desc = effect_dest_resource->GetDesc();
          D3D12_VIEWPORT viewport;
          viewport.TopLeftX = 0.0f;
          viewport.TopLeftY = 0.0f;
          viewport.Width = float(effect_dest_resource_desc.Width);
          viewport.Height = float(effect_dest_resource_desc.Height);
          viewport.MinDepth = 0.0f;
          viewport.MaxDepth = 1.0f;
          command_list->RSSetViewports(1, &viewport);
          D3D12_RECT scissor;
          scissor.left = 0;
          scissor.top = 0;
          scissor.right = LONG(effect_dest_resource_desc.Width);
          scissor.bottom = LONG(effect_dest_resource_desc.Height);
          command_list->RSSetScissorRects(1, &scissor);

          command_list->SetPipelineState(
              is_final_effect ? guest_output_paint_final_pipelines_[size_t(effect)].Get()
                              : guest_output_paint_intermediate_pipelines_[size_t(effect)].Get());
          GuestOutputPaintRootSignatureIndex guest_output_paint_root_signature_index =
              GetGuestOutputPaintRootSignatureIndex(effect);
          command_list->SetGraphicsRootSignature(
              guest_output_paint_root_signatures_[size_t(guest_output_paint_root_signature_index)]
                  .Get());

          UINT effect_src_view_index =
              UINT(i ? (PaintContext::kViewIndexGuestOutputIntermediate0Srv + (i - 1))
                     : (PaintContext::kViewIndexGuestOutput0Srv +
                        guest_output_resource_paint_ref_index));
          command_list->SetGraphicsRootDescriptorTable(
              UINT(GuestOutputPaintRootParameter::kSource),
              provider_.OffsetViewDescriptor(view_heap_gpu_start, effect_src_view_index));

          GuestOutputPaintRectangleConstants effect_rect_constants;
          float effect_x_to_ndc = 2.0f / viewport.Width;
          float effect_y_to_ndc = 2.0f / viewport.Height;
          effect_rect_constants.x = -1.0f + float(effect_rect_x) * effect_x_to_ndc;
          // +Y is -V.
          effect_rect_constants.y = 1.0f - float(effect_rect_y) * effect_y_to_ndc;
          effect_rect_constants.width =
              float(guest_output_flow.effect_output_sizes[i].first) * effect_x_to_ndc;
          effect_rect_constants.height =
              -float(guest_output_flow.effect_output_sizes[i].second) * effect_y_to_ndc;
          command_list->SetGraphicsRoot32BitConstants(
              UINT(GuestOutputPaintRootParameter::kRectangle),
              sizeof(effect_rect_constants) / sizeof(uint32_t), &effect_rect_constants, 0);

          UINT effect_constants_size = 0;
          union {
            BilinearConstants bilinear;
#if defined(REX_HAS_FIDELITYFX_SDK)
            CasSharpenConstants cas_sharpen;
            CasResampleConstants cas_resample;
            FsrEasuConstants fsr_easu;
            FsrRcasConstants fsr_rcas;
#endif
          } effect_constants;
          switch (guest_output_paint_root_signature_index) {
            case kGuestOutputPaintRootSignatureIndexBilinear: {
              effect_constants_size = sizeof(effect_constants.bilinear);
              effect_constants.bilinear.Initialize(guest_output_flow, i);
            } break;
#if defined(REX_HAS_FIDELITYFX_SDK)
            case kGuestOutputPaintRootSignatureIndexCasSharpen: {
              effect_constants_size = sizeof(effect_constants.cas_sharpen);
              effect_constants.cas_sharpen.Initialize(guest_output_flow, i,
                                                      guest_output_paint_config);
            } break;
            case kGuestOutputPaintRootSignatureIndexCasResample: {
              effect_constants_size = sizeof(effect_constants.cas_resample);
              effect_constants.cas_resample.Initialize(guest_output_flow, i,
                                                       guest_output_paint_config);
            } break;
            case kGuestOutputPaintRootSignatureIndexFsrEasu: {
              effect_constants_size = sizeof(effect_constants.fsr_easu);
              effect_constants.fsr_easu.Initialize(guest_output_flow, i);
            } break;
            case kGuestOutputPaintRootSignatureIndexFsrRcas: {
              effect_constants_size = sizeof(effect_constants.fsr_rcas);
              effect_constants.fsr_rcas.Initialize(guest_output_flow, i, guest_output_paint_config);
            } break;
#endif
            default:
              break;
          }
          if (effect_constants_size) {
            command_list->SetGraphicsRoot32BitConstants(
                UINT(GuestOutputPaintRootParameter::kEffectConstants),
                effect_constants_size / sizeof(uint32_t), &effect_constants, 0);
          }

          command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          command_list->DrawInstanced(4, 1, 0, 0);
          drew_graphics_effect = true;
        }

        if (is_final_effect) {
          if (drew_graphics_effect) {
            // Clear the letterbox around the guest output if the guest output
            // doesn't cover the entire back buffer.
            if (guest_output_flow.letterbox_clear_rectangle_count) {
              D3D12_RECT
              letterbox_clear_d3d12_rectangles[GuestOutputPaintFlow::kMaxClearRectangles];
              for (size_t i = 0; i < guest_output_flow.letterbox_clear_rectangle_count; ++i) {
                D3D12_RECT& letterbox_clear_d3d12_rectangle = letterbox_clear_d3d12_rectangles[i];
                const GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle =
                    guest_output_flow.letterbox_clear_rectangles[i];
                letterbox_clear_d3d12_rectangle.left = LONG(letterbox_clear_rectangle.x);
                letterbox_clear_d3d12_rectangle.top = LONG(letterbox_clear_rectangle.y);
                letterbox_clear_d3d12_rectangle.right =
                    LONG(letterbox_clear_rectangle.x + letterbox_clear_rectangle.width);
                letterbox_clear_d3d12_rectangle.bottom =
                    LONG(letterbox_clear_rectangle.y + letterbox_clear_rectangle.height);
              }
              command_list->ClearRenderTargetView(
                  back_buffer_rtv, kBackBufferClearColor,
                  UINT(guest_output_flow.letterbox_clear_rectangle_count),
                  letterbox_clear_d3d12_rectangles);
            }
            back_buffer_clear_needed = false;
          }
        } else {
          D3D12_RESOURCE_BARRIER barriers[2];
          UINT barrier_count = 0;
          D3D12_RESOURCE_STATES effect_write_state = drew_graphics_effect
                                                         ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                         : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          // Transition the newly written intermediate image to SRV for use as
          // the source in the next effect.
          {
            assert_true(barrier_count < rex::countof(barriers));
            D3D12_RESOURCE_BARRIER& barrier_write_to_srv = barriers[barrier_count++];
            barrier_write_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_write_to_srv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_write_to_srv.Transition.pResource = effect_dest_resource;
            barrier_write_to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_write_to_srv.Transition.StateBefore = effect_write_state;
            barrier_write_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          }
          // Merge the current destination > next source transition with the
          // acquisition of the destination for the next effect.
          if (i + 2 < guest_output_flow.effect_count) {
            // The next effect won't be the last - transition the next
            // intermediate destination to its write state.
            D3D12_RESOURCE_STATES next_write_state = is_temporal_easu_effect(i + 1)
                                                         ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                         : D3D12_RESOURCE_STATE_RENDER_TARGET;
            assert_true(barrier_count < rex::countof(barriers));
            D3D12_RESOURCE_BARRIER& barrier_srv_to_write = barriers[barrier_count++];
            barrier_srv_to_write.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_srv_to_write.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_srv_to_write.Transition.pResource =
                paint_context_.guest_output_intermediate_textures[i + 1].Get();
            barrier_srv_to_write.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_srv_to_write.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier_srv_to_write.Transition.StateAfter = next_write_state;
          } else {
            // The next effect draws to the back buffer - merge into one
            // ResourceBarrier command.
            if (!back_buffer_acquired) {
              assert_true(barrier_count < rex::countof(barriers));
              D3D12_RESOURCE_BARRIER& barrier_present_to_rtv = barriers[barrier_count++];
              barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
              barrier_present_to_rtv.Transition.pResource = back_buffer;
              barrier_present_to_rtv.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              barrier_present_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
              barrier_present_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
              back_buffer_acquired = true;
            }
          }
          if (barrier_count) {
            command_list->ResourceBarrier(barrier_count, barriers);
          }
        }
        if (is_final_effect && paint_context_.swap_chain_from_source &&
            paint_context_.swap_chain_source) {
          int32_t picture_x = effect_rect_x;
          int32_t picture_y = effect_rect_y;
          uint32_t picture_width = guest_output_flow.effect_output_sizes[i].first;
          uint32_t picture_height = guest_output_flow.effect_output_sizes[i].second;
          if (!paint_context_.picture_rect_valid ||
              paint_context_.picture_rect_x != picture_x ||
              paint_context_.picture_rect_y != picture_y ||
              paint_context_.picture_rect_width != picture_width ||
              paint_context_.picture_rect_height != picture_height) {
            paint_context_.swap_chain_source->SetPictureRect(
                picture_x, picture_y, picture_width, picture_height);
            paint_context_.picture_rect_valid = true;
            paint_context_.picture_rect_x = picture_x;
            paint_context_.picture_rect_y = picture_y;
            paint_context_.picture_rect_width = picture_width;
            paint_context_.picture_rect_height = picture_height;
          }
        }
      }
    }
  }

  // Release main target guest output texture references that aren't needed
  // anymore (this is done after various potential guest-output-related main
  // target submission tracker waits so the completed submission value is the
  // most actual).
  UINT64 completed_paint_submission =
      paint_context_.paint_submission_tracker.GetCompletedSubmission();
  for (std::pair<UINT64, Microsoft::WRL::ComPtr<ID3D12Resource>>& guest_output_resource_paint_ref :
       paint_context_.guest_output_resource_paint_refs) {
    if (!guest_output_resource_paint_ref.second ||
        guest_output_resource_paint_ref.second == guest_output_resource) {
      continue;
    }
    if (completed_paint_submission >= guest_output_resource_paint_ref.first) {
      guest_output_resource_paint_ref.second.Reset();
    }
  }

  // If no guest output has been drawn, the transitioned of the back buffer to
  // RTV hasn't been done yet, and it's needed to clear it, and optionally to
  // draw the UI.
  if (!back_buffer_acquired) {
    D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
    barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_present_to_rtv.Transition.pResource = back_buffer;
    barrier_present_to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_present_to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier_present_to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &barrier_present_to_rtv);
    back_buffer_acquired = true;
  }

  if (back_buffer_clear_needed) {
    command_list->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor, 0, nullptr);
    back_buffer_clear_needed = false;
  }

  if (REXCVAR_GET(present_screenshot_request) != present_screenshot_request_seen_) {
    present_screenshot_request_seen_ = REXCVAR_GET(present_screenshot_request);
    screenshot_requested_.store(true, std::memory_order_release);
  }
  if (screenshot_requested_.exchange(false, std::memory_order_acq_rel)) {
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    screenshot_width = uint32_t(desc.Width);
    screenshot_height = desc.Height;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &screenshot_footprint, nullptr, nullptr,
                                  &screenshot_buffer_size);
    D3D12_RESOURCE_DESC readback_desc;
    util::FillBufferResourceDesc(readback_desc, screenshot_buffer_size,
                                 D3D12_RESOURCE_FLAG_NONE);
    if (SUCCEEDED(device->CreateCommittedResource(
            &util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&screenshot_buffer))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&screenshot_fence)))) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = back_buffer;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      command_list->ResourceBarrier(1, &barrier);
      D3D12_TEXTURE_COPY_LOCATION source = {};
      source.pResource = back_buffer;
      source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION dest = {};
      dest.pResource = screenshot_buffer.Get();
      dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dest.PlacedFootprint = screenshot_footprint;
      command_list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      command_list->ResourceBarrier(1, &barrier);
    } else {
      screenshot_buffer.Reset();
      screenshot_fence.Reset();
      REXLOG_ERROR("D3D12Presenter: Failed to allocate screenshot readback resources");
    }
  }

  if (!paint_context_.hdr_enabled) {
    if (execute_ui_drawers) {
      if (!back_buffer_bound) {
        command_list->OMSetRenderTargets(1, &back_buffer_rtv, TRUE, nullptr);
        back_buffer_bound = true;
      }
      D3D12UIDrawContext ui_draw_context(*this, paint_context_.swap_chain_width,
                                         paint_context_.swap_chain_height, command_list,
                                         ui_submission_tracker_.GetCurrentSubmission(),
                                         ui_submission_tracker_.GetCompletedSubmission());
      ExecuteUIDrawersFromUIThread(ui_draw_context);
    }
    D3D12_RESOURCE_BARRIER barrier_rtv_to_present = {};
    barrier_rtv_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_rtv_to_present.Transition.pResource = back_buffer;
    barrier_rtv_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_rtv_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier_rtv_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier_rtv_to_present);
  } else {
    D3D12_RESOURCE_BARRIER ui_to_render_target = {};
    ui_to_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ui_to_render_target.Transition.pResource = paint_context_.hdr_ui.Get();
    ui_to_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ui_to_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ui_to_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &ui_to_render_target);
    D3D12_CPU_DESCRIPTOR_HANDLE ui_rtv =
        provider_.OffsetRTVDescriptor(rtv_heap_start, PaintContext::kRTVIndexHDRUI);
    const float clear_ui[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_list->ClearRenderTargetView(ui_rtv, clear_ui, 0, nullptr);
    if (execute_ui_drawers) {
      command_list->OMSetRenderTargets(1, &ui_rtv, TRUE, nullptr);
      D3D12UIDrawContext ui_draw_context(*this, paint_context_.swap_chain_width,
                                         paint_context_.swap_chain_height, command_list,
                                         ui_submission_tracker_.GetCurrentSubmission(),
                                         ui_submission_tracker_.GetCompletedSubmission());
      ExecuteUIDrawersFromUIThread(ui_draw_context);
    }

    D3D12_RESOURCE_BARRIER compose_barriers[3] = {};
    compose_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    compose_barriers[0].Transition.pResource = paint_context_.hdr_picture.Get();
    compose_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    compose_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    compose_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    compose_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    compose_barriers[1].Transition.pResource = paint_context_.hdr_ui.Get();
    compose_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    compose_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    compose_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    compose_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    compose_barriers[2].Transition.pResource = present_buffer;
    compose_barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    compose_barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    compose_barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(3, compose_barriers);

    ID3D12DescriptorHeap* view_heap = paint_context_.view_heap.Get();
    command_list->SetDescriptorHeaps(1, &view_heap);
    command_list->SetGraphicsRootSignature(hdr_compose_pipeline_.root_signature.Get());
    command_list->SetPipelineState(hdr_compose_pipeline_.pipeline.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE view_heap_start = view_heap->GetGPUDescriptorHandleForHeapStart();
    command_list->SetGraphicsRootDescriptorTable(
        0, provider_.OffsetViewDescriptor(view_heap_start, PaintContext::kViewIndexHDRPictureSrv));
    command_list->SetGraphicsRootDescriptorTable(
        1, provider_.OffsetViewDescriptor(view_heap_start, PaintContext::kViewIndexHDRUISrv));
    float hdr_constants[2] = {float(REXCVAR_GET(present_hdr_paper_white_nits)),
                              float(REXCVAR_GET(present_hdr_peak_nits))};
    command_list->SetGraphicsRoot32BitConstants(2, 2, hdr_constants, 0);
    D3D12_VIEWPORT compose_viewport = {};
    compose_viewport.Width = float(paint_context_.swap_chain_width);
    compose_viewport.Height = float(paint_context_.swap_chain_height);
    compose_viewport.MinDepth = 0.0f;
    compose_viewport.MaxDepth = 1.0f;
    command_list->RSSetViewports(1, &compose_viewport);
    D3D12_RECT compose_scissor = {0, 0, LONG(paint_context_.swap_chain_width),
                                  LONG(paint_context_.swap_chain_height)};
    command_list->RSSetScissorRects(1, &compose_scissor);
    command_list->OMSetRenderTargets(1, &present_buffer_rtv, TRUE, nullptr);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER finish_barriers[3] = {};
    finish_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    finish_barriers[0].Transition.pResource = paint_context_.hdr_picture.Get();
    finish_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    finish_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    finish_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    finish_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    finish_barriers[1].Transition.pResource = paint_context_.hdr_ui.Get();
    finish_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    finish_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    finish_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    finish_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    finish_barriers[2].Transition.pResource = present_buffer;
    finish_barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    finish_barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    finish_barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(3, finish_barriers);

    uint32_t paper_white = uint32_t(REXCVAR_GET(present_hdr_paper_white_nits));
    uint32_t peak = uint32_t(REXCVAR_GET(present_hdr_peak_nits));
    if (paper_white != paint_context_.hdr_paper_white_nits ||
        peak != paint_context_.hdr_peak_nits) {
      HRESULT metadata_result =
          hdr::SetMetadata(paint_context_.hdr_swap_chain.Get(), paper_white, peak);
      if (FAILED(metadata_result)) {
        REXLOG_WARN("D3D12Presenter: Failed to update HDR10 metadata (0x{:08X})",
                    uint32_t(metadata_result));
      }
      paint_context_.hdr_paper_white_nits = paper_white;
      paint_context_.hdr_peak_nits = peak;
    }
  }

  // Execute and present.
  command_list->Close();
  ID3D12CommandList* execute_command_list = command_list;
  provider_.GetDirectQueue()->ExecuteCommandLists(1, &execute_command_list);
  if (execute_ui_drawers) {
    ui_submission_tracker_.NextSubmission();
  }
  paint_context_.paint_submission_tracker.NextSubmission();
  if (screenshot_buffer && screenshot_fence) {
    ID3D12CommandQueue* queue = provider_.GetDirectQueue();
    if (SUCCEEDED(queue->Signal(screenshot_fence.Get(), screenshot_fence_value))) {
      auto buffer = screenshot_buffer;
      auto fence = screenshot_fence;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = screenshot_footprint;
      uint64_t buffer_size = screenshot_buffer_size;
      uint32_t width = screenshot_width;
      uint32_t height = screenshot_height;
      std::thread([buffer, fence, footprint, buffer_size, width, height]() {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(fence->SetEventOnCompletion(1, event)) ||
            WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
          if (event) CloseHandle(event);
          REXLOG_ERROR("D3D12Presenter: Failed while waiting for screenshot readback");
          return;
        }
        CloseHandle(event);
        D3D12_RANGE range{0, SIZE_T(buffer_size)};
        void* mapped = nullptr;
        if (FAILED(buffer->Map(0, &range, &mapped))) {
          REXLOG_ERROR("D3D12Presenter: Failed to map screenshot readback");
          return;
        }
        uint32_t stride = width * 4;
        std::vector<uint8_t> pixels(size_t(stride) * height);
        for (uint32_t y = 0; y < height; ++y) {
          std::memcpy(pixels.data() + size_t(stride) * y,
                      static_cast<const uint8_t*>(mapped) + footprint.Offset +
                          size_t(footprint.Footprint.RowPitch) * y,
                      stride);
        }
        // The back buffer's alpha is not coverage; a PNG would show it as transparency.
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;
        D3D12_RANGE no_write{0, 0};
        buffer->Unmap(0, &no_write);
        SaveScreenshotPng(std::move(pixels), width, height, stride);
      }).detach();
    } else {
      REXLOG_ERROR("D3D12Presenter: Failed to signal screenshot readback fence");
    }
  }
  // Present as soon as possible, without waiting for vsync (the host refresh
  // rate may be something like 144 Hz, which is not a multiple of the common
  // 30 Hz or 60 Hz guest refresh rate), and allowing dropping outdated queued
  // frames for lower latency. Also, if possible, allowing tearing to use
  // variable refresh rate in borderless fullscreen (note that if DXGI
  // fullscreen is ever used in, the allow tearing flag must not be passed in
  // fullscreen, but DXGI fullscreen is largely unneeded with the flip
  // presentation model used in Direct3D 12).
  // With vsync on, also wait for the host display: presenting at once with tearing allowed made
  // the vsync setting look broken on high refresh rate screens without variable refresh rate.
  // vsync is defined in the GPU plugin, another module: look it up by name.
  const bool host_vsync = rex::cvar::Query<bool>("vsync");
  HRESULT present_result = paint_context_.swap_chain->Present(
      host_vsync ? 1 : 0,
      DXGI_PRESENT_RESTART |
          (!host_vsync && paint_context_.swap_chain_allows_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
  // Even if presentation has failed, work might have been enqueued anyway
  // internally before the failure according to Jesse Natalie from the DirectX
  // Discord server.
  paint_context_.present_submission_tracker.NextSubmission();
  switch (present_result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGpuLostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGpuLostResponsible;
    default:
      return SUCCEEDED(present_result) ? PaintResult::kPresented : PaintResult::kNotPresented;
  }
}

bool D3D12Presenter::InitializeSurfaceIndependent() {
  // Check if DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported.
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> dxgi_factory_5;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&dxgi_factory_5)))) {
      BOOL tearing_feature_data;
      dxgi_supports_tearing_ = SUCCEEDED(dxgi_factory_5->CheckFeatureSupport(
                                   DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_feature_data,
                                   sizeof(tearing_feature_data))) &&
                               tearing_feature_data;
    }
  }

  if (REXCVAR_GET(present_hdr)) {
    hdr::CreateComposePipeline(provider_, hdr_compose_pipeline_);
  }

  ID3D12Device* device = provider_.GetDevice();

  // Initialize static guest output painting objects.

  // Guest output painting root signatures.
  // One (texture) for bilinear, two (texture and constants) for AMD FidelityFX
  // CAS and FSR.
  D3D12_ROOT_PARAMETER
  guest_output_paint_root_parameters[UINT(GuestOutputPaintRootParameter::kCount)];
  // Source texture.
  D3D12_DESCRIPTOR_RANGE guest_output_paint_root_descriptor_range_source;
  guest_output_paint_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  guest_output_paint_root_descriptor_range_source.NumDescriptors = 1;
  guest_output_paint_root_descriptor_range_source.BaseShaderRegister = 0;
  guest_output_paint_root_descriptor_range_source.RegisterSpace = 0;
  guest_output_paint_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_source =
        guest_output_paint_root_parameters[UINT(GuestOutputPaintRootParameter::kSource)];
    guest_output_paint_root_parameter_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    guest_output_paint_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    guest_output_paint_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &guest_output_paint_root_descriptor_range_source;
    guest_output_paint_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  // Rectangle.
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_rect =
        guest_output_paint_root_parameters[UINT(GuestOutputPaintRootParameter::kRectangle)];
    guest_output_paint_root_parameter_rect.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    guest_output_paint_root_parameter_rect.Constants.ShaderRegister = 0;
    guest_output_paint_root_parameter_rect.Constants.RegisterSpace = 0;
    guest_output_paint_root_parameter_rect.Constants.Num32BitValues =
        sizeof(GuestOutputPaintRectangleConstants) / sizeof(uint32_t);
    guest_output_paint_root_parameter_rect.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  // Pixel shader constants.
  D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_effect_constants =
      guest_output_paint_root_parameters[UINT(GuestOutputPaintRootParameter::kEffectConstants)];
  guest_output_paint_root_parameter_effect_constants.ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  guest_output_paint_root_parameter_effect_constants.Constants.ShaderRegister = 0;
  guest_output_paint_root_parameter_effect_constants.Constants.RegisterSpace = 0;
  guest_output_paint_root_parameter_effect_constants.ShaderVisibility =
      D3D12_SHADER_VISIBILITY_PIXEL;
  // Bilinear sampler.
  D3D12_STATIC_SAMPLER_DESC guest_output_paint_root_sampler;
  guest_output_paint_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  guest_output_paint_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.MipLODBias = 0.0f;
  guest_output_paint_root_sampler.MaxAnisotropy = 1;
  guest_output_paint_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  guest_output_paint_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  guest_output_paint_root_sampler.MinLOD = 0.0f;
  guest_output_paint_root_sampler.MaxLOD = 0.0f;
  guest_output_paint_root_sampler.ShaderRegister = 0;
  guest_output_paint_root_sampler.RegisterSpace = 0;
  guest_output_paint_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC guest_output_paint_root_signature_desc;
  guest_output_paint_root_signature_desc.NumParameters =
      UINT(GuestOutputPaintRootParameter::kCount);
  guest_output_paint_root_signature_desc.pParameters = guest_output_paint_root_parameters;
  guest_output_paint_root_signature_desc.NumStaticSamplers = 1;
  guest_output_paint_root_signature_desc.pStaticSamplers = &guest_output_paint_root_sampler;
  guest_output_paint_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  // Bilinear filtering (needs the sampler).
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(BilinearConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output bilinear "
          "filtering presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexBilinear]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
#if defined(REX_HAS_FIDELITYFX_SDK)
  // EASU (needs the sampler).
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(FsrEasuConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "FSR EASU presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexFsrEasu]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // RCAS and CAS don't need the sampler.
  guest_output_paint_root_signature_desc.NumStaticSamplers = 0;
  // RCAS.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(FsrRcasConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "FSR RCAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexFsrRcas]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // CAS, sharpening only.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(CasSharpenConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "CAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexCasSharpen]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // CAS, resampling.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(CasResampleConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_, guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create the guest output resampling AMD "
          "FidelityFX CAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_[kGuestOutputPaintRootSignatureIndexCasResample]
          .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
#endif  // defined(REX_HAS_FIDELITYFX_SDK)

  // Guest output painting pipelines.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC guest_output_paint_pipeline_desc = {};
  guest_output_paint_pipeline_desc.VS.pShaderBytecode =
      shaders::guest_output_triangle_strip_rect_vs;
  guest_output_paint_pipeline_desc.VS.BytecodeLength =
      sizeof(shaders::guest_output_triangle_strip_rect_vs);
  guest_output_paint_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  guest_output_paint_pipeline_desc.SampleMask = UINT_MAX;
  guest_output_paint_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  guest_output_paint_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  guest_output_paint_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  guest_output_paint_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  guest_output_paint_pipeline_desc.NumRenderTargets = 1;
  guest_output_paint_pipeline_desc.SampleDesc.Count = 1;
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    GuestOutputPaintEffect guest_output_paint_effect = GuestOutputPaintEffect(i);
    switch (guest_output_paint_effect) {
      case GuestOutputPaintEffect::kBilinear:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode = shaders::guest_output_bilinear_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_bilinear_ps);
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_bilinear_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_bilinear_dither_ps);
        break;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_sharpen_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_sharpen_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpenDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_sharpen_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_sharpen_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasResample:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_resample_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_resample_ps);
        break;
      case GuestOutputPaintEffect::kCasResampleDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_resample_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_resample_dither_ps);
        break;
      case GuestOutputPaintEffect::kFsrEasu:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode = shaders::guest_output_ffx_fsr_easu_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_easu_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode = shaders::guest_output_ffx_fsr_rcas_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_rcas_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcasDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_fsr_rcas_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_rcas_dither_ps);
        break;
#endif
      default:
        // Not supported by this implementation.
        continue;
    }
    guest_output_paint_pipeline_desc.pRootSignature =
        guest_output_paint_root_signatures_[GetGuestOutputPaintRootSignatureIndex(
                                                guest_output_paint_effect)]
            .Get();
    if (CanGuestOutputPaintEffectBeIntermediate(guest_output_paint_effect)) {
      guest_output_paint_pipeline_desc.RTVFormats[0] = kGuestOutputIntermediateFormat;
      if (FAILED(device->CreateGraphicsPipelineState(
              &guest_output_paint_pipeline_desc,
              IID_PPV_ARGS(&guest_output_paint_intermediate_pipelines_[i])))) {
        REXLOG_ERROR(
            "D3D12Presenter: Failed to create the guest output painting "
            "pipeline for effect {} writing to an intermediate texture",
            i);
        return false;
      }
    }
    if (CanGuestOutputPaintEffectBeFinal(guest_output_paint_effect)) {
      guest_output_paint_pipeline_desc.RTVFormats[0] = kSwapChainFormat;
      if (FAILED(device->CreateGraphicsPipelineState(
              &guest_output_paint_pipeline_desc,
              IID_PPV_ARGS(&guest_output_paint_final_pipelines_[i])))) {
        REXLOG_ERROR(
            "D3D12Presenter: Failed to create the guest output painting "
            "pipeline for effect {} writing to a swap chain buffer",
            i);
        return false;
      }
    }
  }

  // Initialize connection-independent parts of the painting context.

  ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();

  // Paint submission trackers.
  if (!paint_context_.paint_submission_tracker.Initialize(device, direct_queue) ||
      !paint_context_.present_submission_tracker.Initialize(device, direct_queue)) {
    return false;
  }

  // Paint command allocators and command list.
  for (Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& paint_command_allocator_ref :
       paint_context_.command_allocators) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&paint_command_allocator_ref)))) {
      REXLOG_ERROR(
          "D3D12Presenter: Failed to create a command allocator for drawing to "
          "a swap chain");
      return false;
    }
  }
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       paint_context_.command_allocators[0].Get(), nullptr,
                                       IID_PPV_ARGS(&paint_context_.command_list)))) {
    REXLOG_ERROR(
        "D3D12Presenter: Failed to create the command list for drawing to a "
        "swap chain");
    return false;
  }
  // Command lists are created in an open state.
  paint_context_.command_list->Close();

  // RTV descriptor heap.
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = PaintContext::kRTVCount;
  rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtv_heap_desc.NodeMask = 0;
  if (FAILED(
          device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&paint_context_.rtv_heap)))) {
    REXLOG_ERROR(
        "D3D12Presenter: Failed to create an RTV descriptor heap with {} "
        "descriptors",
        rtv_heap_desc.NumDescriptors);
    return false;
  }

  // CBV/SRV/UAV descriptor heap.
  D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc;
  view_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  view_heap_desc.NumDescriptors = PaintContext::kViewCount;
  view_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  view_heap_desc.NodeMask = 0;
  if (FAILED(
          device->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&paint_context_.view_heap)))) {
    REXLOG_ERROR(
        "D3D12Presenter: Failed to create a shader-visible CBV/SRV/UAV "
        "descriptor heap with {} descriptors",
        view_heap_desc.NumDescriptors);
    return false;
  }

  if (!guest_output_resource_refresher_submission_tracker_.Initialize(device, direct_queue)) {
    return false;
  }

  if (!ui_submission_tracker_.Initialize(device, direct_queue)) {
    return false;
  }

  return InitializeCommonSurfaceIndependent();
}

}  // namespace rex::ui::d3d12
