# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/texture_replacement.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
        d3d12/gow2_temporal.cpp
        d3d12/gow2_dlss.cpp
        d3d12/gow2_upscale.cpp
        d3d12/gow2_fsr.cpp
        d3d12/gow2_framegen.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(rexgpu-xenos PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable)

option(GOW2_DEV_TOOLS "Frame tracing, dumps and a D3D12 debug-layer switch (development only)" OFF)
if(GOW2_DEV_TOOLS)
    target_compile_definitions(rexgpu-xenos PRIVATE GOW2_DEV_TOOLS=1)
endif()

# Optional AMD FidelityFX upscaler (FSR) support: public API headers only (loaded at run time).
set(GOW2_FFX_SDK "${REXGLUE_ROOT}/../ffx-sdk/Kits/FidelityFX" CACHE PATH "AMD FidelityFX SDK Kits/FidelityFX folder")
if(WIN32 AND EXISTS "${GOW2_FFX_SDK}/api/include/ffx_api.h")
    target_include_directories(rexgpu-xenos PRIVATE "${GOW2_FFX_SDK}/api/include"
                               "${GOW2_FFX_SDK}/upscalers/include"
                               "${GOW2_FFX_SDK}/framegeneration/include")
    target_compile_definitions(rexgpu-xenos PRIVATE GOW2_HAS_FFX=1)
    message(STATUS "FSR (FidelityFX API) support: ON (${GOW2_FFX_SDK})")
else()
    message(STATUS "FSR (FidelityFX API) support: OFF")
endif()

# Optional NVIDIA DLSS (NGX) support: headers and import library come from the public NVIDIA/DLSS
# repository checked out next to this tree. Nothing from NVIDIA ends up in the source tree.
set(GOW2_DLSS_SDK "${REXGLUE_ROOT}/../dlss-sdk" CACHE PATH "NVIDIA DLSS SDK folder")
if(WIN32 AND EXISTS "${GOW2_DLSS_SDK}/include/nvsdk_ngx.h")
    target_include_directories(rexgpu-xenos PRIVATE "${GOW2_DLSS_SDK}/include")
    target_link_libraries(rexgpu-xenos PRIVATE "${GOW2_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_compile_definitions(rexgpu-xenos PRIVATE GOW2_HAS_DLSS=1)
    message(STATUS "DLSS (NGX) support: ON (${GOW2_DLSS_SDK})")
else()
    message(STATUS "DLSS (NGX) support: OFF")
endif()

rex_target_pch(rexgpu-xenos
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    xxHash::xxhash
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
    )
endif()
