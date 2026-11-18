/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Santiago Quiroz Upegui and the gow2-recomp contributors, 2026 - Adapted
 *              from Xenia's ui/surface_android for the SDL3 window of the ReXGlue Android port
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/platform.h>
#include <rex/ui/surface.h>

#if REX_PLATFORM_ANDROID

struct ANativeWindow;
struct SDL_Window;

namespace rex {
namespace ui {

// Presentation surface wrapping the ANativeWindow of the SDL3 window. Android destroys that
// window whenever the app leaves the screen, and SDL drops its reference on the Java thread
// right then, so this surface holds a reference of its own: the pointer stays valid for as
// long as the presenter can still reach it.
class AndroidNativeWindowSurface final : public Surface {
 public:
  // Null while Android has no surface for the window (the app is in the background).
  static std::unique_ptr<AndroidNativeWindowSurface> Create(SDL_Window* sdl_window);
  ~AndroidNativeWindowSurface() override;
  AndroidNativeWindowSurface(const AndroidNativeWindowSurface&) = delete;
  AndroidNativeWindowSurface& operator=(const AndroidNativeWindowSurface&) = delete;
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  AndroidNativeWindowSurface(ANativeWindow* acquired_window, SDL_Window* sdl_window)
      : window_(acquired_window), sdl_window_(sdl_window) {}

  ANativeWindow* window_;
  SDL_Window* sdl_window_;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
