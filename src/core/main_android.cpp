/**
 * @file        core/main_android.cpp
 * @brief       Android platform helpers for the ReXGlue runtime.
 *
 * @copyright   Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <android/api-level.h>

#include <rex/main_android.h>

namespace rex {

int GetAndroidApiLevel() { return android_get_device_api_level(); }

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
