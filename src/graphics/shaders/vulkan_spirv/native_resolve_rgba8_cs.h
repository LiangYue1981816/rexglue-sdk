// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Google Shaderc over Glslang; 10
; Bound: 102
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 8 8 1
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_samplerless_texture_functions"
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpName %main "main"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %XeNativeResolveConstants "XeNativeResolveConstants"
               OpMemberName %XeNativeResolveConstants 0 "xe_native_resolve_mode"
               OpMemberName %XeNativeResolveConstants 1 "xe_native_resolve_scale"
               OpMemberName %XeNativeResolveConstants 2 "xe_native_resolve_source_offset"
               OpMemberName %XeNativeResolveConstants 3 "xe_native_resolve_dest_offset"
               OpMemberName %XeNativeResolveConstants 4 "xe_native_resolve_size"
               OpMemberName %XeNativeResolveConstants 5 "xe_native_resolve_fill"
               OpName %_ ""
               OpName %pixel "pixel"
               OpName %source_pixel "source_pixel"
               OpName %value "value"
               OpName %xe_native_resolve_source "xe_native_resolve_source"
               OpName %xe_native_resolve_dest "xe_native_resolve_dest"
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpMemberDecorate %XeNativeResolveConstants 0 Offset 0
               OpMemberDecorate %XeNativeResolveConstants 1 Offset 4
               OpMemberDecorate %XeNativeResolveConstants 2 Offset 8
               OpMemberDecorate %XeNativeResolveConstants 3 Offset 16
               OpMemberDecorate %XeNativeResolveConstants 4 Offset 24
               OpMemberDecorate %XeNativeResolveConstants 5 Offset 32
               OpDecorate %XeNativeResolveConstants Block
               OpDecorate %xe_native_resolve_source DescriptorSet 0
               OpDecorate %xe_native_resolve_source Binding 0
               OpDecorate %xe_native_resolve_dest DescriptorSet 0
               OpDecorate %xe_native_resolve_dest Binding 1
               OpDecorate %xe_native_resolve_dest NonReadable
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
     %v2uint = OpTypeVector %uint 2
      %float = OpTypeFloat 32
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
%XeNativeResolveConstants = OpTypeStruct %uint %float %v2int %v2int %v2uint %v2uint
%_ptr_PushConstant_XeNativeResolveConstants = OpTypePointer PushConstant %XeNativeResolveConstants
          %_ = OpVariable %_ptr_PushConstant_XeNativeResolveConstants PushConstant
      %int_4 = OpConstant %int 4
%_ptr_PushConstant_v2uint = OpTypePointer PushConstant %v2uint
       %bool = OpTypeBool
     %v2bool = OpTypeVector %bool 2
%_ptr_Function_v2int = OpTypePointer Function %v2int
      %int_5 = OpConstant %int 5
    %v4float = OpTypeVector %float 4
%_ptr_Function_v4float = OpTypePointer Function %v4float
         %46 = OpTypeImage %float 2D 0 0 0 1 Unknown
%_ptr_UniformConstant_46 = OpTypePointer UniformConstant %46
%xe_native_resolve_source = OpVariable %_ptr_UniformConstant_46 UniformConstant
      %int_2 = OpConstant %int 2
%_ptr_PushConstant_v2int = OpTypePointer PushConstant %v2int
      %int_0 = OpConstant %int 0
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_0 = OpConstant %uint 0
%_ptr_Function_float = OpTypePointer Function %float
      %int_1 = OpConstant %int 1
%_ptr_PushConstant_float = OpTypePointer PushConstant %float
    %float_0 = OpConstant %float 0
         %90 = OpTypeImage %float 2D 0 0 0 2 Rgba8
%_ptr_UniformConstant_90 = OpTypePointer UniformConstant %90
%xe_native_resolve_dest = OpVariable %_ptr_UniformConstant_90 UniformConstant
      %int_3 = OpConstant %int 3
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_8 %uint_8 %uint_1
       %main = OpFunction %void None %3
          %5 = OpLabel
      %pixel = OpVariable %_ptr_Function_v2int Function
%source_pixel = OpVariable %_ptr_Function_v2int Function
      %value = OpVariable %_ptr_Function_v4float Function
         %11 = OpLoad %v3uint %gl_GlobalInvocationID
         %12 = OpVectorShuffle %v2uint %11 %11 0 1
         %21 = OpAccessChain %_ptr_PushConstant_v2uint %_ %int_4
         %22 = OpLoad %v2uint %21
         %25 = OpUGreaterThanEqual %v2bool %12 %22
         %26 = OpAny %bool %25
               OpSelectionMerge %28 None
               OpBranchConditional %26 %27 %28
         %27 = OpLabel
               OpReturn
         %28 = OpLabel
         %32 = OpLoad %v3uint %gl_GlobalInvocationID
         %33 = OpVectorShuffle %v2uint %32 %32 0 1
         %34 = OpBitcast %v2int %33
               OpStore %pixel %34
         %36 = OpLoad %v3uint %gl_GlobalInvocationID
         %37 = OpVectorShuffle %v2uint %36 %36 0 1
         %39 = OpAccessChain %_ptr_PushConstant_v2uint %_ %int_5
         %40 = OpLoad %v2uint %39
         %41 = OpExtInst %v2uint %1 UMax %37 %40
         %42 = OpBitcast %v2int %41
               OpStore %source_pixel %42
         %49 = OpLoad %46 %xe_native_resolve_source
         %52 = OpAccessChain %_ptr_PushConstant_v2int %_ %int_2
         %53 = OpLoad %v2int %52
         %54 = OpLoad %v2int %source_pixel
         %55 = OpIAdd %v2int %53 %54
         %57 = OpImageFetch %v4float %49 %55 Lod %int_0
               OpStore %value %57
         %59 = OpAccessChain %_ptr_PushConstant_uint %_ %int_0
         %60 = OpLoad %uint %59
         %62 = OpIEqual %bool %60 %uint_1
               OpSelectionMerge %64 None
               OpBranchConditional %62 %63 %67
         %63 = OpLabel
         %65 = OpLoad %v4float %value
         %66 = OpVectorShuffle %v4float %65 %65 2 1 0 3
               OpStore %value %66
               OpBranch %64
         %67 = OpLabel
         %68 = OpAccessChain %_ptr_PushConstant_uint %_ %int_0
         %69 = OpLoad %uint %68
         %71 = OpIEqual %bool %69 %uint_2
               OpSelectionMerge %73 None
               OpBranchConditional %71 %72 %85
         %72 = OpLabel
         %76 = OpAccessChain %_ptr_Function_float %value %uint_0
         %77 = OpLoad %float %76
         %80 = OpAccessChain %_ptr_PushConstant_float %_ %int_1
         %81 = OpLoad %float %80
         %82 = OpFMul %float %77 %81
         %84 = OpCompositeConstruct %v4float %82 %float_0 %float_0 %float_0
               OpStore %value %84
               OpBranch %73
         %85 = OpLabel
         %86 = OpAccessChain %_ptr_PushConstant_float %_ %int_1
         %87 = OpLoad %float %86
         %88 = OpLoad %v4float %value
         %89 = OpVectorTimesScalar %v4float %88 %87
               OpStore %value %89
               OpBranch %73
         %73 = OpLabel
               OpBranch %64
         %64 = OpLabel
         %93 = OpLoad %90 %xe_native_resolve_dest
         %95 = OpAccessChain %_ptr_PushConstant_v2int %_ %int_3
         %96 = OpLoad %v2int %95
         %97 = OpLoad %v2int %pixel
         %98 = OpIAdd %v2int %96 %97
         %99 = OpLoad %v4float %value
               OpImageWrite %93 %98 %99
               OpReturn
               OpFunctionEnd
#endif

const uint32_t native_resolve_rgba8_cs[] = {
    0x07230203, 0x00010000, 0x000D000A, 0x00000066, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000004, 0x6E69616D, 0x00000000, 0x00000009, 0x00060010, 0x00000004,
    0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002, 0x000001CC, 0x000B0004,
    0x455F4C47, 0x735F5458, 0x6C706D61, 0x656C7265, 0x745F7373, 0x75747865, 0x665F6572, 0x74636E75,
    0x736E6F69, 0x00000000, 0x000A0004, 0x475F4C47, 0x4C474F4F, 0x70635F45, 0x74735F70, 0x5F656C79,
    0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004, 0x475F4C47, 0x4C474F4F, 0x6E695F45,
    0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6E69616D, 0x00000000,
    0x00080005, 0x00000009, 0x475F6C67, 0x61626F6C, 0x766E496C, 0x7461636F, 0x496E6F69, 0x00000044,
    0x00090005, 0x00000010, 0x614E6558, 0x65766974, 0x6F736552, 0x4365766C, 0x74736E6F, 0x73746E61,
    0x00000000, 0x00090006, 0x00000010, 0x00000000, 0x6E5F6578, 0x76697461, 0x65725F65, 0x766C6F73,
    0x6F6D5F65, 0x00006564, 0x00090006, 0x00000010, 0x00000001, 0x6E5F6578, 0x76697461, 0x65725F65,
    0x766C6F73, 0x63735F65, 0x00656C61, 0x000B0006, 0x00000010, 0x00000002, 0x6E5F6578, 0x76697461,
    0x65725F65, 0x766C6F73, 0x6F735F65, 0x65637275, 0x66666F5F, 0x00746573, 0x000B0006, 0x00000010,
    0x00000003, 0x6E5F6578, 0x76697461, 0x65725F65, 0x766C6F73, 0x65645F65, 0x6F5F7473, 0x65736666,
    0x00000074, 0x00090006, 0x00000010, 0x00000004, 0x6E5F6578, 0x76697461, 0x65725F65, 0x766C6F73,
    0x69735F65, 0x0000657A, 0x00090006, 0x00000010, 0x00000005, 0x6E5F6578, 0x76697461, 0x65725F65,
    0x766C6F73, 0x69665F65, 0x00006C6C, 0x00030005, 0x00000012, 0x00000000, 0x00040005, 0x0000001F,
    0x65786970, 0x0000006C, 0x00060005, 0x00000023, 0x72756F73, 0x705F6563, 0x6C657869, 0x00000000,
    0x00040005, 0x0000002D, 0x756C6176, 0x00000065, 0x00090005, 0x00000030, 0x6E5F6578, 0x76697461,
    0x65725F65, 0x766C6F73, 0x6F735F65, 0x65637275, 0x00000000, 0x00080005, 0x0000005C, 0x6E5F6578,
    0x76697461, 0x65725F65, 0x766C6F73, 0x65645F65, 0x00007473, 0x00040047, 0x00000009, 0x0000000B,
    0x0000001C, 0x00050048, 0x00000010, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000010,
    0x00000001, 0x00000023, 0x00000004, 0x00050048, 0x00000010, 0x00000002, 0x00000023, 0x00000008,
    0x00050048, 0x00000010, 0x00000003, 0x00000023, 0x00000010, 0x00050048, 0x00000010, 0x00000004,
    0x00000023, 0x00000018, 0x00050048, 0x00000010, 0x00000005, 0x00000023, 0x00000020, 0x00030047,
    0x00000010, 0x00000002, 0x00040047, 0x00000030, 0x00000022, 0x00000000, 0x00040047, 0x00000030,
    0x00000021, 0x00000000, 0x00040047, 0x0000005C, 0x00000022, 0x00000000, 0x00040047, 0x0000005C,
    0x00000021, 0x00000001, 0x00030047, 0x0000005C, 0x00000019, 0x00040047, 0x00000065, 0x0000000B,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000003, 0x00040020, 0x00000008,
    0x00000001, 0x00000007, 0x0004003B, 0x00000008, 0x00000009, 0x00000001, 0x00040017, 0x0000000A,
    0x00000006, 0x00000002, 0x00030016, 0x0000000D, 0x00000020, 0x00040015, 0x0000000E, 0x00000020,
    0x00000001, 0x00040017, 0x0000000F, 0x0000000E, 0x00000002, 0x0008001E, 0x00000010, 0x00000006,
    0x0000000D, 0x0000000F, 0x0000000F, 0x0000000A, 0x0000000A, 0x00040020, 0x00000011, 0x00000009,
    0x00000010, 0x0004003B, 0x00000011, 0x00000012, 0x00000009, 0x0004002B, 0x0000000E, 0x00000013,
    0x00000004, 0x00040020, 0x00000014, 0x00000009, 0x0000000A, 0x00020014, 0x00000017, 0x00040017,
    0x00000018, 0x00000017, 0x00000002, 0x00040020, 0x0000001E, 0x00000007, 0x0000000F, 0x0004002B,
    0x0000000E, 0x00000026, 0x00000005, 0x00040017, 0x0000002B, 0x0000000D, 0x00000004, 0x00040020,
    0x0000002C, 0x00000007, 0x0000002B, 0x00090019, 0x0000002E, 0x0000000D, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x0000002F, 0x00000000, 0x0000002E,
    0x0004003B, 0x0000002F, 0x00000030, 0x00000000, 0x0004002B, 0x0000000E, 0x00000032, 0x00000002,
    0x00040020, 0x00000033, 0x00000009, 0x0000000F, 0x0004002B, 0x0000000E, 0x00000038, 0x00000000,
    0x00040020, 0x0000003A, 0x00000009, 0x00000006, 0x0004002B, 0x00000006, 0x0000003D, 0x00000001,
    0x0004002B, 0x00000006, 0x00000046, 0x00000002, 0x0004002B, 0x00000006, 0x0000004A, 0x00000000,
    0x00040020, 0x0000004B, 0x00000007, 0x0000000D, 0x0004002B, 0x0000000E, 0x0000004E, 0x00000001,
    0x00040020, 0x0000004F, 0x00000009, 0x0000000D, 0x0004002B, 0x0000000D, 0x00000053, 0x00000000,
    0x00090019, 0x0000005A, 0x0000000D, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002,
    0x00000004, 0x00040020, 0x0000005B, 0x00000000, 0x0000005A, 0x0004003B, 0x0000005B, 0x0000005C,
    0x00000000, 0x0004002B, 0x0000000E, 0x0000005E, 0x00000003, 0x0004002B, 0x00000006, 0x00000064,
    0x00000008, 0x0006002C, 0x00000007, 0x00000065, 0x00000064, 0x00000064, 0x0000003D, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200F8, 0x00000005, 0x0004003B, 0x0000001E,
    0x0000001F, 0x00000007, 0x0004003B, 0x0000001E, 0x00000023, 0x00000007, 0x0004003B, 0x0000002C,
    0x0000002D, 0x00000007, 0x0004003D, 0x00000007, 0x0000000B, 0x00000009, 0x0007004F, 0x0000000A,
    0x0000000C, 0x0000000B, 0x0000000B, 0x00000000, 0x00000001, 0x00050041, 0x00000014, 0x00000015,
    0x00000012, 0x00000013, 0x0004003D, 0x0000000A, 0x00000016, 0x00000015, 0x000500AE, 0x00000018,
    0x00000019, 0x0000000C, 0x00000016, 0x0004009A, 0x00000017, 0x0000001A, 0x00000019, 0x000300F7,
    0x0000001C, 0x00000000, 0x000400FA, 0x0000001A, 0x0000001B, 0x0000001C, 0x000200F8, 0x0000001B,
    0x000100FD, 0x000200F8, 0x0000001C, 0x0004003D, 0x00000007, 0x00000020, 0x00000009, 0x0007004F,
    0x0000000A, 0x00000021, 0x00000020, 0x00000020, 0x00000000, 0x00000001, 0x0004007C, 0x0000000F,
    0x00000022, 0x00000021, 0x0003003E, 0x0000001F, 0x00000022, 0x0004003D, 0x00000007, 0x00000024,
    0x00000009, 0x0007004F, 0x0000000A, 0x00000025, 0x00000024, 0x00000024, 0x00000000, 0x00000001,
    0x00050041, 0x00000014, 0x00000027, 0x00000012, 0x00000026, 0x0004003D, 0x0000000A, 0x00000028,
    0x00000027, 0x0007000C, 0x0000000A, 0x00000029, 0x00000001, 0x00000029, 0x00000025, 0x00000028,
    0x0004007C, 0x0000000F, 0x0000002A, 0x00000029, 0x0003003E, 0x00000023, 0x0000002A, 0x0004003D,
    0x0000002E, 0x00000031, 0x00000030, 0x00050041, 0x00000033, 0x00000034, 0x00000012, 0x00000032,
    0x0004003D, 0x0000000F, 0x00000035, 0x00000034, 0x0004003D, 0x0000000F, 0x00000036, 0x00000023,
    0x00050080, 0x0000000F, 0x00000037, 0x00000035, 0x00000036, 0x0007005F, 0x0000002B, 0x00000039,
    0x00000031, 0x00000037, 0x00000002, 0x00000038, 0x0003003E, 0x0000002D, 0x00000039, 0x00050041,
    0x0000003A, 0x0000003B, 0x00000012, 0x00000038, 0x0004003D, 0x00000006, 0x0000003C, 0x0000003B,
    0x000500AA, 0x00000017, 0x0000003E, 0x0000003C, 0x0000003D, 0x000300F7, 0x00000040, 0x00000000,
    0x000400FA, 0x0000003E, 0x0000003F, 0x00000043, 0x000200F8, 0x0000003F, 0x0004003D, 0x0000002B,
    0x00000041, 0x0000002D, 0x0009004F, 0x0000002B, 0x00000042, 0x00000041, 0x00000041, 0x00000002,
    0x00000001, 0x00000000, 0x00000003, 0x0003003E, 0x0000002D, 0x00000042, 0x000200F9, 0x00000040,
    0x000200F8, 0x00000043, 0x00050041, 0x0000003A, 0x00000044, 0x00000012, 0x00000038, 0x0004003D,
    0x00000006, 0x00000045, 0x00000044, 0x000500AA, 0x00000017, 0x00000047, 0x00000045, 0x00000046,
    0x000300F7, 0x00000049, 0x00000000, 0x000400FA, 0x00000047, 0x00000048, 0x00000055, 0x000200F8,
    0x00000048, 0x00050041, 0x0000004B, 0x0000004C, 0x0000002D, 0x0000004A, 0x0004003D, 0x0000000D,
    0x0000004D, 0x0000004C, 0x00050041, 0x0000004F, 0x00000050, 0x00000012, 0x0000004E, 0x0004003D,
    0x0000000D, 0x00000051, 0x00000050, 0x00050085, 0x0000000D, 0x00000052, 0x0000004D, 0x00000051,
    0x00070050, 0x0000002B, 0x00000054, 0x00000052, 0x00000053, 0x00000053, 0x00000053, 0x0003003E,
    0x0000002D, 0x00000054, 0x000200F9, 0x00000049, 0x000200F8, 0x00000055, 0x00050041, 0x0000004F,
    0x00000056, 0x00000012, 0x0000004E, 0x0004003D, 0x0000000D, 0x00000057, 0x00000056, 0x0004003D,
    0x0000002B, 0x00000058, 0x0000002D, 0x0005008E, 0x0000002B, 0x00000059, 0x00000058, 0x00000057,
    0x0003003E, 0x0000002D, 0x00000059, 0x000200F9, 0x00000049, 0x000200F8, 0x00000049, 0x000200F9,
    0x00000040, 0x000200F8, 0x00000040, 0x0004003D, 0x0000005A, 0x0000005D, 0x0000005C, 0x00050041,
    0x00000033, 0x0000005F, 0x00000012, 0x0000005E, 0x0004003D, 0x0000000F, 0x00000060, 0x0000005F,
    0x0004003D, 0x0000000F, 0x00000061, 0x0000001F, 0x00050080, 0x0000000F, 0x00000062, 0x00000060,
    0x00000061, 0x0004003D, 0x0000002B, 0x00000063, 0x0000002D, 0x00040063, 0x0000005D, 0x00000062,
    0x00000063, 0x000100FD, 0x00010038,
};
