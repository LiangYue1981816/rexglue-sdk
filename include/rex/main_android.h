/**
 * @file        rex/main_android.h
 * @brief       Android platform helpers for the ReXGlue runtime.
 *
 * @copyright   Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Android-only (step 1 of the Android port). Everything here is
 *              guarded so non-Android builds are unaffected.
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

namespace rex {

// Returns the running device's API level, via android_get_device_api_level().
// This is the *runtime* level, not the __ANDROID_API__ the binary was built
// against, so one binary behaves correctly on newer devices too. The Android
// port targets API 29+, so callers can rely on APIs from level 29 and below
// being present; higher levels must still be probed before use.
int GetAndroidApiLevel();

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
