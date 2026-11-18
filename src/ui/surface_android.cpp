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

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <android/native_window.h>

#include <SDL3/SDL.h>

#include <rex/ui/surface_android.h>

namespace rex {
namespace ui {

std::unique_ptr<AndroidNativeWindowSurface> AndroidNativeWindowSurface::Create(
    SDL_Window* sdl_window) {
  if (!sdl_window) {
    return nullptr;
  }
  // SDL clears the property under this lock before it releases the window on the Java thread,
  // so a pointer read under the lock is still alive while the reference is taken.
  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
  if (!props || !SDL_LockProperties(props)) {
    return nullptr;
  }
  auto* window = static_cast<ANativeWindow*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
  if (window) {
    ANativeWindow_acquire(window);
  }
  SDL_UnlockProperties(props);
  if (!window) {
    return nullptr;
  }
  return std::unique_ptr<AndroidNativeWindowSurface>(
      new AndroidNativeWindowSurface(window, sdl_window));
}

AndroidNativeWindowSurface::~AndroidNativeWindowSurface() {
  ANativeWindow_release(window_);
}

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out,
                                             uint32_t& height_out) const {
  int w = 0, h = 0;
  if (!sdl_window_ || !SDL_GetWindowSizeInPixels(sdl_window_, &w, &h) || w <= 0 || h <= 0) {
    return false;
  }
  width_out = uint32_t(w);
  height_out = uint32_t(h);
  return true;
}

}  // namespace ui
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
