#include <rex/graphics/d3d12/gow2_framegen.h>

#include <string>
#include <algorithm>
#include <cstdint>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/gow2_fsr.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    gow2_frame_generation, "off", "Display",
    "Frame generation: off or fsr3 (on: FSR 4 frame generation where the driver offers it, FSR 3 "
    "otherwise). Requires temporal upscaling (FSR 3.1, FSR 4 or DLSS), adds about one frame of "
    "latency, and runs exclusive fullscreen as borderless.")
    .allowed({"off", "fsr3"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#ifdef GOW2_HAS_FFX
#define _WINDOWS 1
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>
#endif

namespace rex::graphics::d3d12::gow2 {

namespace {

FrameGeneration g_frame_generation;

void LogOffOnce(bool& logged, const std::string& reason) {
  if (!logged) {
    REXGPU_INFO("frame generation: off ({})", reason);
    logged = true;
  }
}

#ifdef GOW2_HAS_FFX
ffxFunctions* Functions() { return static_cast<ffxFunctions*>(FsrFunctionTable()); }

ffxReturnCode_t DispatchFrameGeneration(ffxDispatchDescFrameGeneration* desc, void* user) {
  return static_cast<FrameGeneration*>(user)->Dispatch(desc)
             ? FFX_API_RETURN_OK
             : FFX_API_RETURN_ERROR;
}
#endif

}  // namespace

FrameGeneration& GetFrameGeneration() { return g_frame_generation; }

Microsoft::WRL::ComPtr<IDXGISwapChain1> FrameGeneration::Create(
    IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1& desc) {
  std::lock_guard lock(mutex_);
  if (REXCVAR_GET(gow2_frame_generation) != "fsr3") {
    LogOffOnce(logged_, "cvar off");
    return nullptr;
  }
  if (rex::cvar::Query<std::string>("gow2_upscaler") == "off") {
    LogOffOnce(logged_, "no temporal upscaler");
    return nullptr;
  }
#ifndef GOW2_HAS_FFX
  LogOffOnce(logged_, "FidelityFX API not built");
  return nullptr;
#else
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
      !FsrLoaderInit(device.Get())) {
    LogOffOnce(logged_, "loader missing");
    return nullptr;
  }
  FsrProvider provider;
  if (!FsrFrameGenerationProvider(&provider)) {
    LogOffOnce(logged_, "frame generation provider missing");
    return nullptr;
  }
  ffxFunctions* fn = Functions();
  if (!fn || !fn->CreateContext || !fn->DestroyContext || !fn->Configure) {
    LogOffOnce(logged_, "loader missing");
    return nullptr;
  }

  if (swap_chain_context_) {
    DestroyContext();
    fn->DestroyContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), nullptr);
    swap_chain_context_ = nullptr;
    if (swap_chain_) swap_chain_->Release();
    swap_chain_ = nullptr;
    enabled_.store(false, std::memory_order_release);
  }
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device.Get();
  ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
  create.header.pNext = &backend.header;
  create.hwnd = hwnd;
  create.desc = const_cast<DXGI_SWAP_CHAIN_DESC1*>(&desc);
  create.fullscreenDesc = nullptr;
  create.dxgiFactory = factory;
  create.gameQueue = queue;
  IDXGISwapChain4* created_swap_chain = nullptr;
  create.swapchain = &created_swap_chain;
  ffxReturnCode_t create_result =
      fn->CreateContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), &create.header,
                        nullptr);
  if (create_result != FFX_API_RETURN_OK || !swap_chain_context_) {
    if (swap_chain_context_) {
      fn->DestroyContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), nullptr);
    }
    swap_chain_context_ = nullptr;
    if (created_swap_chain) created_swap_chain->Release();
    LogOffOnce(logged_, "swap chain creation failed");
    return nullptr;
  }
  swap_chain_ = created_swap_chain;
  if (!swap_chain_) {
    fn->DestroyContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), nullptr);
    swap_chain_context_ = nullptr;
    LogOffOnce(logged_, "swap chain creation failed");
    return nullptr;
  }
  display_width_ = desc.Width;
  display_height_ = desc.Height;
  back_buffer_format_ = desc.Format;
  hdr_ = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
  provider_version_id_ = provider.id;
  provider_name_ = std::move(provider.name);
  provider_major_ = provider.major;
  provider_logged_ = false;
  picture_rect_set_ = false;
  enabled_.store(true, std::memory_order_release);
  Microsoft::WRL::ComPtr<IDXGISwapChain1> result;
  if (FAILED(swap_chain_->QueryInterface(IID_PPV_ARGS(&result)))) {
    fn->DestroyContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), nullptr);
    swap_chain_context_ = nullptr;
    swap_chain_->Release();
    swap_chain_ = nullptr;
    enabled_.store(false, std::memory_order_release);
    LogOffOnce(logged_, "swap chain creation failed");
    return nullptr;
  }
  logged_ = true;
  REXGPU_INFO("frame generation: on (FSR {}, provider '{}')", provider_major_, provider_name_);
  REXGPU_INFO("frame generation: exclusive fullscreen uses borderless mode");
  return result;
#endif
}

void FrameGeneration::Released() {
  std::lock_guard lock(mutex_);
#ifdef GOW2_HAS_FFX
  DestroyContext();
  DestroyRetiredContexts(UINT64_MAX);
  ffxFunctions* fn = Functions();
  if (swap_chain_context_ && fn && fn->DestroyContext) {
    fn->DestroyContext(reinterpret_cast<ffxContext*>(&swap_chain_context_), nullptr);
  }
#endif
  swap_chain_context_ = nullptr;
  if (swap_chain_) swap_chain_->Release();
  swap_chain_ = nullptr;
  display_width_ = 0;
  display_height_ = 0;
  render_width_ = 0;
  render_height_ = 0;
  context_display_width_ = 0;
  context_display_height_ = 0;
  enabled_.store(false, std::memory_order_release);
  prepared_ = false;
  picture_rect_set_ = false;
  picture_rect_x_ = 0;
  picture_rect_y_ = 0;
  picture_rect_width_ = 0;
  picture_rect_height_ = 0;
}

void FrameGeneration::SetPictureRect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  std::lock_guard lock(mutex_);
  if (swap_chain_) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (SUCCEEDED(swap_chain_->GetDesc1(&desc))) {
      display_width_ = desc.Width;
      display_height_ = desc.Height;
    }
  }
  int64_t left = std::clamp<int64_t>(x, 0, display_width_);
  int64_t top = std::clamp<int64_t>(y, 0, display_height_);
  int64_t right = std::clamp<int64_t>(int64_t(x) + width, 0, display_width_);
  int64_t bottom = std::clamp<int64_t>(int64_t(y) + height, 0, display_height_);
  picture_rect_x_ = int32_t(left);
  picture_rect_y_ = int32_t(top);
  picture_rect_width_ = uint32_t(std::max<int64_t>(0, right - left));
  picture_rect_height_ = uint32_t(std::max<int64_t>(0, bottom - top));
  picture_rect_set_ = true;
}

void FrameGeneration::DestroyContext() {
#ifdef GOW2_HAS_FFX
  ffxFunctions* fn = Functions();
  if (frame_generation_context_ && fn && fn->DestroyContext) {
    fn->DestroyContext(reinterpret_cast<ffxContext*>(&frame_generation_context_), nullptr);
  }
#endif
  frame_generation_context_ = nullptr;
  render_width_ = 0;
  render_height_ = 0;
  context_display_width_ = 0;
  context_display_height_ = 0;
}

void FrameGeneration::RetireContext(uint64_t submission) {
  if (frame_generation_context_) {
    DisableInSwapChain();
    retired_contexts_.emplace_back(submission, frame_generation_context_);
    frame_generation_context_ = nullptr;
  }
  render_width_ = 0;
  render_height_ = 0;
  context_display_width_ = 0;
  context_display_height_ = 0;
}

void FrameGeneration::DisableInSwapChain() {
#ifdef GOW2_HAS_FFX
  ffxFunctions* fn = Functions();
  if (!frame_generation_context_ || !swap_chain_ || !fn || !fn->Configure) return;
  ffxConfigureDescFrameGeneration configure = {};
  configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  configure.swapChain = swap_chain_;
  configure.frameGenerationCallback = &DispatchFrameGeneration;
  configure.frameGenerationCallbackUserContext = this;
  configure.frameGenerationEnabled = false;
  configure.allowAsyncWorkloads = false;
  configure.generationRect = {0, 0, int32_t(display_width_), int32_t(display_height_)};
  configure.frameID = prepared_frame_id_;
  // Waits for in-flight presents; afterwards the swap chain stops calling Dispatch.
  ffxReturnCode_t result = fn->Configure(
      reinterpret_cast<ffxContext*>(&frame_generation_context_), &configure.header);
  if (result != FFX_API_RETURN_OK && !configure_error_logged_) {
    REXGPU_WARN("frame generation: disable before context change failed ({})", uint32_t(result));
    configure_error_logged_ = true;
  }
#endif
}

void FrameGeneration::DestroyRetiredContexts(uint64_t completed_submission) {
#ifdef GOW2_HAS_FFX
  ffxFunctions* fn = Functions();
  if (fn && fn->DestroyContext) {
    std::erase_if(retired_contexts_, [&](auto& entry) {
      if (entry.first <= completed_submission) {
        fn->DestroyContext(reinterpret_cast<ffxContext*>(&entry.second), nullptr);
        return true;
      }
      return false;
    });
  }
#else
  (void)completed_submission;
#endif
}

void FrameGeneration::RetireCompleted(uint64_t completed_submission) {
  std::lock_guard lock(mutex_);
  DestroyRetiredContexts(completed_submission);
}

bool FrameGeneration::LoadProvider(ID3D12Device* device) {
  return device && FsrLoaderInit(device) && !provider_name_.empty();
}

bool FrameGeneration::CreateContext(uint32_t render_width, uint32_t render_height) {
#ifdef GOW2_HAS_FFX
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = FsrDevice();
  ffxOverrideVersion version = {};
  version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  version.versionId = provider_version_id_;
  backend.header.pNext = &version.header;
  ffxCreateContextDescFrameGeneration create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  create.header.pNext = &backend.header;
  create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
  if (hdr_) create.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
  create.displaySize = {display_width_, display_height_};
  create.maxRenderSize = {render_width, render_height};
  create.backBufferFormat = ffxApiGetSurfaceFormatDX12(back_buffer_format_);
  ffxFunctions* fn = Functions();
  if (fn && fn->CreateContext &&
      fn->CreateContext(reinterpret_cast<ffxContext*>(&frame_generation_context_), &create.header,
                        nullptr) == FFX_API_RETURN_OK) {
    return true;
  }
  frame_generation_context_ = nullptr;
#endif
  return false;
}

bool FrameGeneration::FallBackToFsr3(uint32_t render_width, uint32_t render_height) {
  FsrProvider fsr3;
  if (provider_major_ == 3 || !FsrFrameGenerationProvider(&fsr3, 3)) return false;
  REXGPU_WARN("frame generation: provider '{}' failed, using '{}'", provider_name_, fsr3.name);
  provider_version_id_ = fsr3.id;
  provider_name_ = std::move(fsr3.name);
  provider_major_ = fsr3.major;
  return CreateContext(render_width, render_height);
}

bool FrameGeneration::EnsureContext(uint32_t render_width, uint32_t render_height,
                                    uint64_t submission) {
#ifdef GOW2_HAS_FFX
  if (!enabled() || !swap_chain_ || !LoadProvider(FsrDevice())) return false;
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  if (FAILED(swap_chain_->GetDesc1(&swap_chain_desc))) return false;
  display_width_ = swap_chain_desc.Width;
  display_height_ = swap_chain_desc.Height;
  back_buffer_format_ = swap_chain_desc.Format;
  hdr_ = back_buffer_format_ == DXGI_FORMAT_R10G10B10A2_UNORM;
  if (frame_generation_context_ && render_width_ == render_width &&
      render_height_ == render_height && context_display_width_ == display_width_ &&
      context_display_height_ == display_height_) {
    return true;
  }
  RetireContext(submission);
  if (!CreateContext(render_width, render_height) && !FallBackToFsr3(render_width, render_height)) {
    return false;
  }
  render_width_ = render_width;
  render_height_ = render_height;
  context_display_width_ = display_width_;
  context_display_height_ = display_height_;
  if (!provider_logged_) {
    REXGPU_INFO("frame generation: provider '{}', render {}x{}, display {}x{}", provider_name_,
                render_width_, render_height_, display_width_, display_height_);
    provider_logged_ = true;
  }
  return true;
#else
  return false;
#endif
}

bool FrameGeneration::Prepare(ID3D12GraphicsCommandList* list, uint64_t frame_id,
                              ID3D12Resource* depth, ID3D12Resource* motion,
                              uint32_t render_width, uint32_t render_height, float jitter_x,
                              float jitter_y, float frame_ms, float camera_near, float camera_far,
                              float fov_y, bool reset, uint64_t submission) {
  if (!enabled()) return false;
  std::lock_guard lock(mutex_);
#ifdef GOW2_HAS_FFX
  if (!enabled()) return false;
  if (!list || !depth || !motion || !EnsureContext(render_width, render_height, submission)) {
    return false;
  }
  ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = frame_id;
  prepare.commandList = list;
  prepare.renderSize = {render_width, render_height};
  prepare.jitterOffset = {jitter_x, jitter_y};
  prepare.motionVectorScale = {1.0f, 1.0f};
  prepare.frameTimeDelta = frame_ms;
  prepare.reset = reset;
  prepare.cameraNear = camera_near;
  prepare.cameraFar = camera_far;
  prepare.cameraFovAngleVertical = fov_y;
  prepare.viewSpaceToMetersFactor = 0.01f;
  prepare.depth = ffxApiGetResourceDX12(depth,
                                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  prepare.motionVectors = ffxApiGetResourceDX12(
      motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  ffxFunctions* fn = Functions();
  bool result = false;
  {
    std::lock_guard dispatch_lock(dispatch_mutex_);
    result = fn && fn->Dispatch &&
             fn->Dispatch(reinterpret_cast<ffxContext*>(&frame_generation_context_),
                          &prepare.header) == FFX_API_RETURN_OK;
  }
  prepared_ = result;
  prepared_frame_id_ = frame_id;
  return result;
#else
  return false;
#endif
}

void FrameGeneration::ConfigureFrame(uint64_t frame_id) {
  if (!enabled()) return;
  std::lock_guard lock(mutex_);
#ifdef GOW2_HAS_FFX
  if (!enabled() || !frame_generation_context_ || !swap_chain_context_) return;
  ffxConfigureDescFrameGeneration configure = {};
  configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  configure.swapChain = swap_chain_;
  configure.frameGenerationCallback = &DispatchFrameGeneration;
  configure.frameGenerationCallbackUserContext = this;
  configure.frameGenerationEnabled = prepared_ && prepared_frame_id_ == frame_id;
  configure.allowAsyncWorkloads = false;
  if (swap_chain_) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (SUCCEEDED(swap_chain_->GetDesc1(&desc))) {
      display_width_ = desc.Width;
      display_height_ = desc.Height;
    }
  }
  int32_t rect_x = 0;
  int32_t rect_y = 0;
  uint32_t rect_width = display_width_;
  uint32_t rect_height = display_height_;
  if (swap_chain_ && picture_rect_set_) {
    int64_t left = std::clamp<int64_t>(picture_rect_x_, 0, display_width_);
    int64_t top = std::clamp<int64_t>(picture_rect_y_, 0, display_height_);
    int64_t right = std::clamp<int64_t>(int64_t(picture_rect_x_) + picture_rect_width_,
                                        0, display_width_);
    int64_t bottom = std::clamp<int64_t>(int64_t(picture_rect_y_) + picture_rect_height_,
                                         0, display_height_);
    if (right > left && bottom > top) {
      rect_x = int32_t(left);
      rect_y = int32_t(top);
      rect_width = uint32_t(right - left);
      rect_height = uint32_t(bottom - top);
    }
  }
  configure.generationRect = {rect_x, rect_y, int32_t(rect_width), int32_t(rect_height)};
  configure.frameID = frame_id;
  ffxFunctions* fn = Functions();
  if (fn && fn->Configure) {
    ffxReturnCode_t result = fn->Configure(
        reinterpret_cast<ffxContext*>(&frame_generation_context_), &configure.header);
    if (result != FFX_API_RETURN_OK && !configure_error_logged_) {
      REXGPU_WARN("frame generation: configure failed ({})", uint32_t(result));
      configure_error_logged_ = true;
    }
  }
#else
  (void)frame_id;
#endif
  prepared_ = false;
}

bool FrameGeneration::Dispatch(void* desc) {
  // Not mutex_: Present holds the FFX swap chain lock, which ConfigureFrame takes under mutex_.
  std::lock_guard lock(dispatch_mutex_);
#ifdef GOW2_HAS_FFX
  if (!desc || !frame_generation_context_) return false;
  ffxFunctions* fn = Functions();
  auto* dispatch = static_cast<ffxDispatchDescFrameGeneration*>(desc);
  return fn && fn->Dispatch &&
         fn->Dispatch(reinterpret_cast<ffxContext*>(&frame_generation_context_),
                      &dispatch->header) == FFX_API_RETURN_OK;
#else
  (void)desc;
  return false;
#endif
}

void FrameGeneration::Shutdown() { Released(); }

}  // namespace rex::graphics::d3d12::gow2
