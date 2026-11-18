// Adapted from gears-judgement-recomp (BSD-3-Clause).
#pragma once
// Project-local temporal-upscaling support for the D3D12 backend (Gears of War 2).
//
// What this module assumes about the game (measured on Gears of War: Judgment; MUST be
// validated for Gears of War 2, Unreal Engine 3, 2008):
//   * One frame = scene draws into one HDR render target, then a depth resolve, particle draws
//     that sample the resolved depth, a colour resolve (scene colour texture), and one full-screen
//     tonemap draw that samples only that colour texture, then the HUD.
//   * Scene vertex shaders read the ViewProjection matrix from vertex float constants c0..c3
//     (cvar gow2_vp_register) and the camera world position from c13 (cvar gow2_campos_register).
#include <cstdint>
#include <cstdio>

#include <rex/graphics/d3d12/gow2_trace.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12TextureCache;

namespace gow2 {

// Floats of VS constants cN..cN+15 (N = cvar gow2_vp_register) captured from the first
// qualifying scene draw of a frame.
struct SceneCamera {
  bool valid = false;
  float c[16][4] = {};
  uint32_t draws_matching = 0;  // later draws of the frame whose VP rows equal the captured ones
  uint32_t draws_other = 0;     // qualifying draws whose VP rows differ
};

struct FrameState {
  uint32_t frame = 0;
  SceneCamera camera;       // this frame
  SceneCamera prev_camera;  // previous frame
  uint32_t depth_resolve_dst = 0;
  uint32_t depth_dsts[4] = {};  // every depth resolve destination of this frame
  uint32_t depth_dst_count = 0;
  uint32_t color_resolve_dst = 0;  // scene colour (largest non-frontbuffer colour resolve)
  uint32_t depth_resolve_area = 0;  // width*height of the chosen resolves, this frame
  uint32_t color_resolve_area = 0;
  uint32_t color_resolve_w = 0, color_resolve_h = 0;
  uint32_t last_front = 0;
  uint32_t scale_x = 1, scale_y = 1;
  uint32_t last_upscaled_frame = 0xFFFFFFFFu;
  // The frame that just ended (BeginFrame runs at the swap, after its draws): its camera, the camera
  // before it and the frame number its jitter was taken from. The end-of-frame upscaler uses these.
  SceneCamera ended_camera, ended_prev_camera;
  uint32_t ended_frame = 0;
  // Sub-pixel jitter is applied only while the previous frame went through the upscaler, so menus
  // and loading screens are left alone.
  bool ended_jittered = false;  // whether the frame that just ended was jittered
  bool jitter_this_frame = false;
  bool jitter_next = false;  // set by a successful end-of-frame upscale: jitter the next frames
  // Decided once per frame (BeginFrame): rasterise world geometry into the render region only
  // when the previous frame went through the upscaler, so menus and loading screens are left alone.
  bool shrink_this_frame = false;
  uint32_t guest_w = 0, guest_h = 0;  // scene colour resolve size of the last finished frame
};

FrameState& State();

// Dump capture: create "gow2_dump.now" in the working directory. The next two frames write
// gow2_dump/f<k>_{depth,color}.bin and f<k>_cam.txt.
void BeginFrame(D3D12CommandProcessor& cp, uint32_t front_ptr);
void NoteResolve(bool is_depth, uint32_t dst, uint32_t width, uint32_t height, uint32_t fmt);
// Called for each draw with a pixel shader. vs_consts: pointer to 4*256 floats of VS constants.
void NoteSceneDraw(const float* vs_consts, uint32_t color_fmt, uint32_t index_count);
// Text for the FPS overlay (cvar gow2_upscale_status); only touches the cvar when the text changes.
void SetUpscaleStatus(const char* text);
// Create "gow2_shot.now": the next presented frame's final guest output is written to
// gow2_dump/f0_shot<N>.bin (R10G10B10A2, same header as the other dumps). Headless screenshots.
void CaptureOutputIfRequested(D3D12CommandProcessor& cp, ID3D12Resource* output,
                              D3D12_RESOURCE_STATES state);
// Master switch (cvar gow2_temporal).
bool Enabled();
// True for draws that should be sub-pixel jittered: world-geometry vertex shaders (read the
// ViewProjection matrix at cvar gow2_vp_register) drawing into the scene target (surface pitch
// == guest scene width). vs_float_bitmap: the 256-bit VS float-constant bitmap (4 words).
bool ShouldJitter(const uint64_t* vs_float_bitmap, uint32_t surface_pitch, uint32_t color_fmt);
// Jitter for this frame in NDC units (add to the NDC offset); host_w/h = host render size.
void JitterNdc(uint32_t host_w, uint32_t host_h, float out[2]);
// Jitter for this frame in render-target pixels, as handed to an upscaler (x right, y down).
void JitterPixels(float out[2]);
void JitterPixelsForFrame(uint32_t frame, float out[2]);
// Render-resolution scale of the scene (1.0 = native). World geometry is rasterised into the
// top-left (fx*W, fy*H) region of the target and the upscaler reconstructs the full size.
// Only active while the upscaler is usable. out_w/out_h = full host target size.
bool RenderScale(uint32_t out_w, uint32_t out_h, float* fx, float* fy, uint32_t* rw, uint32_t* rh);
// 0 = off, 1 = DLSS, 2 = FSR 3.1, 3 = FSR 4 (cvar gow2_upscaler).
int SelectedUpscaler();
// Upscaler (DLSS) hooks. UpscaleEnabled(): cvar gow2_dlss && temporal pipeline on.
bool UpscaleEnabled();
void NoteDepthTexture(ID3D12Resource* depth);
// Typed view format both upscalers read the finished frame with: the frame texture is TYPELESS
// (Gears of War 2: R8G8B8A8_TYPELESS) and neither NGX nor FidelityFX can view that.
// DXGI_FORMAT_UNKNOWN = a frame format the upscalers do not take.
DXGI_FORMAT UpscalerColorFormat(DXGI_FORMAT frame_format);
// End-of-frame upscaling. Called at the swap with the finished guest frame (colour: the frontbuffer
// texture, in_w x in_h active pixels). On success the upscaled picture is in `out` (output size,
// R10G10B10A2, ready to be sampled) and the presenter should use it instead of the guest frame.
struct FinalImage {
  ID3D12Resource* resource = nullptr;
  uint32_t w = 0, h = 0;
};
bool UpscaleFinal(D3D12CommandProcessor& cp, ID3D12Resource* color, uint32_t in_w, uint32_t in_h,
                  FinalImage& out);
// Index of the first VS constant register at which 4 consecutive rows equal this frame's camera
// matrix (0..252), or -1.
int FindCameraMatrix(const float* vs_consts);
// Called after textures for the draw were requested. fetch constants listed in used_mask.
void OnDrawTextures(D3D12CommandProcessor& cp, D3D12TextureCache& tc,
                    const RegisterFile& regs, uint32_t used_mask);

}  // namespace gow2
}  // namespace rex::graphics::d3d12
