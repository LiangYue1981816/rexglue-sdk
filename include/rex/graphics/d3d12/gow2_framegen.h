#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/d3d12/d3d12_swap_chain_source.h>

namespace rex::graphics::d3d12::gow2 {

class FrameGeneration final : public ui::d3d12::SwapChainSource {
 public:
  Microsoft::WRL::ComPtr<IDXGISwapChain1> Create(
      IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
      const DXGI_SWAP_CHAIN_DESC1& desc) override;
  void Released() override;
  void SetPictureRect(int32_t x, int32_t y, uint32_t width, uint32_t height) override;

  bool Prepare(ID3D12GraphicsCommandList* list, uint64_t frame_id, ID3D12Resource* depth,
               ID3D12Resource* motion, uint32_t render_width, uint32_t render_height,
               float jitter_x, float jitter_y, float frame_ms, float camera_near,
               float camera_far, float fov_y, bool reset, uint64_t submission);
  void ConfigureFrame(uint64_t frame_id);
  bool Dispatch(void* desc);
  void RetireCompleted(uint64_t completed_submission);
  void Shutdown();
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

 private:
  bool LoadProvider(ID3D12Device* device);
  bool EnsureContext(uint32_t render_width, uint32_t render_height, uint64_t submission);
  bool CreateContext(uint32_t render_width, uint32_t render_height);
  bool FallBackToFsr3(uint32_t render_width, uint32_t render_height);
  void DestroyContext();
  void RetireContext(uint64_t submission);
  void DisableInSwapChain();
  void DestroyRetiredContexts(uint64_t completed_submission);

  std::mutex mutex_;
  std::mutex dispatch_mutex_;  // FFX contexts are not thread-safe: Prepare vs Dispatch
  void* swap_chain_context_ = nullptr;
  void* frame_generation_context_ = nullptr;
  std::vector<std::pair<uint64_t, void*>> retired_contexts_;
  IDXGISwapChain4* swap_chain_ = nullptr;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;
  uint32_t context_display_width_ = 0;
  uint32_t context_display_height_ = 0;
  DXGI_FORMAT back_buffer_format_ = DXGI_FORMAT_UNKNOWN;
  bool hdr_ = false;
  std::atomic<bool> enabled_ = false;
  bool logged_ = false;
  bool provider_logged_ = false;
  bool configure_error_logged_ = false;
  bool prepared_ = false;
  uint64_t prepared_frame_id_ = 0;
  uint64_t provider_version_id_ = 0;
  std::string provider_name_;
  int provider_major_ = -1;
  bool picture_rect_set_ = false;
  int32_t picture_rect_x_ = 0;
  int32_t picture_rect_y_ = 0;
  uint32_t picture_rect_width_ = 0;
  uint32_t picture_rect_height_ = 0;
};

FrameGeneration& GetFrameGeneration();

}  // namespace rex::graphics::d3d12::gow2
