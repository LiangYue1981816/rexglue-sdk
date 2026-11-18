#pragma once

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {

class SwapChainSource {
 public:
  virtual ~SwapChainSource() = default;
  virtual Microsoft::WRL::ComPtr<IDXGISwapChain1> Create(
      IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
      const DXGI_SWAP_CHAIN_DESC1& desc) = 0;
  virtual void Released() = 0;
  virtual void SetPictureRect(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
};

}  // namespace rex::ui::d3d12
