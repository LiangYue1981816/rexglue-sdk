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

#include <rex/ui/d3d12/d3d12_hdr.h>

#include <cstring>

#include <d3dcompiler.h>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::ui::d3d12::hdr {
namespace {

constexpr char kComposeShader[] = R"(
Texture2D<float4> picture : register(t0);
Texture2D<float4> ui : register(t1);
cbuffer Constants : register(b0) { float paperWhite; float peakNits; }

struct VertexOutput { float4 position : SV_Position; };

VertexOutput VSMain(uint vertexId : SV_VertexID) {
  VertexOutput output;
  output.position = float4(vertexId == 2 ? 3.0 : -1.0,
                           vertexId == 1 ? 3.0 : -1.0, 0.0, 1.0);
  return output;
}

float3 SRGBToLinear(float3 value) {
  return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4),
              step(0.04045, value));
}

float3 LinearToPQ(float3 value) {
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 32.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 128.0;
  const float c3 = 2392.0 / 128.0;
  float3 p = pow(saturate(value), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float4 PSMain(VertexOutput input) : SV_Target {
  int3 location = int3(int2(input.position.xy), 0);
  float3 encoded = picture.Load(location).rgb;
  float3 rgb = SRGBToLinear(encoded);
  float luminance = dot(rgb, float3(0.2126, 0.7152, 0.0722));
  float peak = max(peakNits, paperWhite);
  float3 nits = rgb * paperWhite;
  if (luminance > 0.6) {
    float t = (luminance - 0.6) / 0.4;
    float outputLuminance = paperWhite * luminance +
                            (peak - paperWhite) * t * t;
    nits = rgb * (outputLuminance / luminance);
  }

  // The UI is premultiplied: blending the encoded values gives what SDR shows.
  float4 overlay = ui.Load(location);
  float3 blended = SRGBToLinear(overlay.rgb + encoded * (1.0 - overlay.a)) * paperWhite;
  nits = blended + (nits - rgb * paperWhite) * (1.0 - overlay.a);

  float3 bt2020 = mul(float3x3(
      0.627404, 0.329283, 0.043313,
      0.069097, 0.919540, 0.011362,
      0.016391, 0.088013, 0.895595), nits);
  return float4(LinearToPQ(bt2020 / 10000.0), 1.0);
}
)";

using D3DCompileFunction = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
                                             const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                             LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFunction GetD3DCompile() {
  static HMODULE library = LoadLibraryW(L"d3dcompiler_47.dll");
  return library ? reinterpret_cast<D3DCompileFunction>(GetProcAddress(library, "D3DCompile"))
                 : nullptr;
}

bool CompileShader(const char* entry_point, const char* target,
                   Microsoft::WRL::ComPtr<ID3DBlob>& bytecode) {
  D3DCompileFunction compile = GetD3DCompile();
  if (!compile) {
    REXLOG_ERROR("D3D12Presenter: d3dcompiler_47.dll is unavailable");
    return false;
  }
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT result = compile(kComposeShader, std::strlen(kComposeShader), "D3D12 HDR compose",
                           nullptr, nullptr, entry_point, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0, &bytecode, &errors);
  if (FAILED(result)) {
    REXLOG_ERROR("D3D12Presenter: HDR compose shader {} failed: {}", entry_point,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
    return false;
  }
  return true;
}

}  // namespace

bool CreateComposePipeline(const D3D12Provider& provider, ComposePipeline& pipeline) {
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  for (UINT i = 0; i < 2; ++i) {
    ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = i;
    ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  }
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[2].Constants.ShaderRegister = 0;
  parameters[2].Constants.Num32BitValues = 2;
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 3;
  root_desc.pParameters = parameters;
  pipeline.root_signature.Attach(util::CreateRootSignature(provider, root_desc));
  if (!pipeline.root_signature) {
    return false;
  }

  Microsoft::WRL::ComPtr<ID3DBlob> vertex_shader;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_shader;
  if (!CompileShader("VSMain", "vs_5_0", vertex_shader) ||
      !CompileShader("PSMain", "ps_5_0", pixel_shader)) {
    pipeline.root_signature.Reset();
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = pipeline.root_signature.Get();
  desc.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
  desc.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
  desc.BlendState.AlphaToCoverageEnable = FALSE;
  desc.BlendState.IndependentBlendEnable = FALSE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.InputLayout = {};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  if (FAILED(provider.GetDevice()->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&pipeline.pipeline)))) {
    REXLOG_ERROR("D3D12Presenter: Failed to create the HDR compose pipeline");
    pipeline.root_signature.Reset();
    return false;
  }
  return true;
}

HRESULT SetMetadata(IDXGISwapChain4* swap_chain, uint32_t paper_white_nits,
                    uint32_t peak_nits) {
  DXGI_HDR_METADATA_HDR10 metadata = {};
  metadata.RedPrimary[0] = 35400;
  metadata.RedPrimary[1] = 14600;
  metadata.GreenPrimary[0] = 8500;
  metadata.GreenPrimary[1] = 39850;
  metadata.BluePrimary[0] = 6550;
  metadata.BluePrimary[1] = 2300;
  metadata.WhitePoint[0] = 15635;
  metadata.WhitePoint[1] = 16450;
  metadata.MaxMasteringLuminance = peak_nits;
  metadata.MaxContentLightLevel = uint16_t(peak_nits);
  metadata.MaxFrameAverageLightLevel = uint16_t(paper_white_nits);
  return swap_chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
}

}  // namespace rex::ui::d3d12::hdr
