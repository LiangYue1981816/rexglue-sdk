/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460
#extension GL_EXT_samplerless_texture_functions : require

// gpu_native_resolves: writes a resolve rectangle from a host render target
// straight into a texture cache texture when the resolve is not a plain copy of
// the render target bits. Vulkan version of native_resolve.cs.hlsl.
// Each thread handles one host pixel (already scaled with resolution scaling).
//
// Modes:
// 0 - out = in * xe_native_resolve_scale (exponent bias of float formats).
// 1 - out = in.bgra (red and blue swapped).
// 2 - out.r = depth * xe_native_resolve_scale (the source depth view returns the depth in the red
//     component).
//
// xe_native_resolve_fill is the number of host pixel columns and rows at the
// left and the top of the rectangle that are taken from the first surely
// covered column and row instead (the resolve shaders' half-pixel offset gap
// fill with resolution scaling, ResolveEdramInfo::fill_half_pixel_offset), so
// the source pixel is max(pixel, xe_native_resolve_fill).
//
// Storage image writes need the format qualifier without the
// shaderStorageImageWriteWithoutFormat feature, so this is compiled once per
// destination format with XE_NATIVE_RESOLVE_DEST_FORMAT defined to rgba16f
// (native_resolve_rgba16f_cs), rgba8 (native_resolve_rgba8_cs), r32f
// (native_resolve_r32f_cs) or rg16f (native_resolve_rg16f_cs).

#ifndef XE_NATIVE_RESOLVE_DEST_FORMAT
#define XE_NATIVE_RESOLVE_DEST_FORMAT rgba16f
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeNativeResolveConstants {
  uint xe_native_resolve_mode;
  float xe_native_resolve_scale;
  ivec2 xe_native_resolve_source_offset;
  ivec2 xe_native_resolve_dest_offset;
  uvec2 xe_native_resolve_size;
  uvec2 xe_native_resolve_fill;
};

layout(set = 0, binding = 0) uniform texture2D xe_native_resolve_source;
layout(set = 0, binding = 1, XE_NATIVE_RESOLVE_DEST_FORMAT) uniform writeonly
    image2D xe_native_resolve_dest;

void main() {
  if (any(greaterThanEqual(gl_GlobalInvocationID.xy, xe_native_resolve_size))) {
    return;
  }
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  ivec2 source_pixel =
      ivec2(max(gl_GlobalInvocationID.xy, xe_native_resolve_fill));
  vec4 value = texelFetch(xe_native_resolve_source,
                          xe_native_resolve_source_offset + source_pixel, 0);
  if (xe_native_resolve_mode == 1u) {
    value = value.bgra;
  } else if (xe_native_resolve_mode == 2u) {
    value = vec4(value.r * xe_native_resolve_scale, 0.0, 0.0, 0.0);
  } else {
    value *= xe_native_resolve_scale;
  }
  imageStore(xe_native_resolve_dest, xe_native_resolve_dest_offset + pixel, value);
}
