/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
#include <dlfcn.h>

// SteamOS ships librenderdoc.so, so loading it by name put RenderDoc's capture layer, its overlay
// and GPU debug markers into every run.
REXCVAR_DEFINE_BOOL(load_renderdoc, false, "GPU",
                    "Load RenderDoc whenever it is installed, not only when the game was started "
                    "from RenderDoc. Shows RenderDoc's overlay (frame time, F12 to capture) and "
                    "costs some performance.");
#endif

namespace rex {
namespace ui {

namespace {

bool LoadRenderDocLibrary(platform::DynamicLibrary& library) {
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
  if (!REXCVAR_GET(load_renderdoc)) {
    // RTLD_NOLOAD only finds a RenderDoc that is already in the process (started from RenderDoc).
    void* handle = dlopen(platform::lib_names::kRenderDoc, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
      return false;
    }
    library.Adopt(handle);
    return true;
  }
#endif
  return library.Load(platform::lib_names::kRenderDoc);
}

}  // namespace

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  // The vendored RenderDoc app API header doesn't expose a macOS path.
  // Keep RenderDoc optional and simply report "not connected" on this platform.
  return nullptr;
#else
  std::unique_ptr<RenderDocAPI> renderdoc_api(new RenderDocAPI());

  pRENDERDOC_GetAPI get_api = nullptr;

  // Try to load the RenderDoc library. If RenderDoc is attached, the library
  // should already be loaded into the process and this will increment the
  // reference count. If not attached, the load will fail and we return nullptr.
  if (!LoadRenderDocLibrary(renderdoc_api->library_)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  // get_api will be null if RenderDoc is not connected, or the API isn't
  // available on this platform, or there was an error.
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, (void**)&renderdoc_api->api_1_0_0_) ||
      !renderdoc_api->api_1_0_0_) {
    return nullptr;
  }

  REXLOG_INFO("RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace ui
}  // namespace rex
