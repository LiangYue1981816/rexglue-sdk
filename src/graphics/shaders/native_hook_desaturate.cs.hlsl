/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// gpu_native_hook_example: desaturates a texture written by a native resolve in
// place (D3D12TextureCache::ApplyNativeResolveHooks), rgb = Rec. 709 luminance,
// alpha unchanged.
// Each thread handles one host pixel (already scaled with resolution scaling).

cbuffer XeNativeHookDesaturateConstants : register(b0) {
  uint2 xe_native_hook_size;
};

RWTexture2D<float4> xe_native_hook_texture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= xe_native_hook_size)) {
    return;
  }
  float4 value = xe_native_hook_texture[xe_thread_id.xy];
  value.rgb = dot(value.rgb, float3(0.2126, 0.7152, 0.0722));
  xe_native_hook_texture[xe_thread_id.xy] = value;
}
