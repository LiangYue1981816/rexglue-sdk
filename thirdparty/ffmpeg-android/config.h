/*
 * FFmpeg configuration for the Android arm64 build of the ReXGlue runtime.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors
 *
 * Android only: thirdparty/CMakeLists.txt puts this folder ahead of thirdparty/FFmpeg on the
 * include path of libavutil and libavcodec, so FFmpeg's `#include "config.h"` finds this file.
 * It takes FFmpeg's own Android aarch64 configuration and turns the aarch64 assembly paths off:
 * the .S sources are not built on Android, so the generic C code must be selected.
 */
#ifndef REX_FFMPEG_ANDROID_CONFIG_H
#define REX_FFMPEG_ANDROID_CONFIG_H

#if !defined(__ANDROID__) || !defined(__aarch64__)
#error "thirdparty/ffmpeg-android/config.h is only for Android arm64 builds"
#endif

#include "config_android_aarch64.h"

#undef ARCH_AARCH64
#define ARCH_AARCH64 0

#undef HAVE_ARMV8
#define HAVE_ARMV8 0
#undef HAVE_NEON
#define HAVE_NEON 0
#undef HAVE_VFP
#define HAVE_VFP 0

#undef HAVE_ARMV8_EXTERNAL
#define HAVE_ARMV8_EXTERNAL 0
#undef HAVE_NEON_EXTERNAL
#define HAVE_NEON_EXTERNAL 0
#undef HAVE_VFP_EXTERNAL
#define HAVE_VFP_EXTERNAL 0

#undef HAVE_ARMV8_INLINE
#define HAVE_ARMV8_INLINE 0
#undef HAVE_NEON_INLINE
#define HAVE_NEON_INLINE 0
#undef HAVE_VFP_INLINE
#define HAVE_VFP_INLINE 0

#endif  // REX_FFMPEG_ANDROID_CONFIG_H
