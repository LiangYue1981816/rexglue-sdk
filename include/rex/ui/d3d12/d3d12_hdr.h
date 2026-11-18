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

#pragma once

#include <cstdint>

#include <rex/ui/d3d12/d3d12_provider.h>

namespace rex::ui::d3d12::hdr {

struct ComposePipeline {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};

bool CreateComposePipeline(const D3D12Provider& provider, ComposePipeline& pipeline);
HRESULT SetMetadata(IDXGISwapChain4* swap_chain, uint32_t paper_white_nits,
                    uint32_t peak_nits);

}  // namespace rex::ui::d3d12::hdr
