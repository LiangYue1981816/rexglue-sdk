/*
 * Android-only (arm64) link stubs for the bundled FFmpeg.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors
 *
 * On Android the aarch64 assembly sources are not built (pure C fallback; see
 * thirdparty/CMakeLists.txt) and ffmpeg-android/config.h sets ARCH_AARCH64=0, so the generic C
 * code is used. These no-op definitions of the ff_*_init_aarch64() entry points remain for
 * unoptimized builds, where an `if (0)`-guarded call in float_dsp.c, fft_template.c or
 * mpegaudiodsp.c may still emit a reference. Only added to the build when ANDROID is set.
 */
#if defined(__ANDROID__) && defined(__aarch64__)

#include "libavutil/attributes.h"
#include "libavutil/float_dsp.h"
#include "libavcodec/fft.h"
#include "libavcodec/mpegaudiodsp.h"

av_cold void ff_float_dsp_init_aarch64(AVFloatDSPContext* fdsp) {
  (void)fdsp;
}

av_cold void ff_fft_init_aarch64(FFTContext* s) {
  (void)s;
}

av_cold void ff_mpadsp_init_aarch64(MPADSPContext* s) {
  (void)s;
}

#endif  // defined(__ANDROID__) && defined(__aarch64__)
