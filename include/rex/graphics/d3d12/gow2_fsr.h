// Adapted from gears-judgement-recomp (BSD-3-Clause).
#pragma once
#include <cstdint>
#include <string>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12::gow2 {

struct FsrEval {
  ID3D12Resource* color = nullptr;   // SRV state, render region at the top-left
  DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;  // typed view of color (UpscalerColorFormat)
  ID3D12Resource* depth = nullptr;   // SRV state, inverted depth (1 - z)
  ID3D12Resource* motion = nullptr;  // SRV state, render-size RG16F pixels, current -> previous
  ID3D12Resource* output = nullptr;  // UAV state, full output size
  uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
  float jitter_x = 0, jitter_y = 0;  // render pixels
  bool reset = false;
  float frame_ms = 16.7f;
  float camera_near = 10.0f, camera_far = 8000.0f, fov_y = 1.0f;
  // 3 = FSR 3.1, 4 = FSR 4 (when the loader reports it), 0 = whatever the loader picks
  int wanted_major = 3;
  uint64_t submission = 0;  // command processor submission that records this dispatch
};

struct FsrProvider {
  uint64_t id = 0;
  std::string name;
  int major = -1;
};

// Loads amd_fidelityfx_loader_dx12.dll from the game folder and enumerates the providers.
bool FsrInit(ID3D12Device* device);
bool FsrLoaderInit(ID3D12Device* device);
// Name of the provider actually used by the current context ("" if none yet).
const char* FsrProviderName();
// True when the loader offers an FSR 4 provider on this system.
bool FsrHasFsr4();
// Frame generation provider with the highest major version this GPU supports (FSR 4 where the
// driver offers it, FSR 3 elsewhere), or only that major when wanted_major is set.
bool FsrFrameGenerationProvider(FsrProvider* provider, int wanted_major = 0);
void* FsrFunctionTable();
ID3D12Device* FsrDevice();
bool FsrEvaluate(ID3D12GraphicsCommandList* list, const FsrEval& eval);
void FsrRetireCompleted(uint64_t completed_submission);
void FsrShutdown();

}  // namespace rex::graphics::d3d12::gow2
