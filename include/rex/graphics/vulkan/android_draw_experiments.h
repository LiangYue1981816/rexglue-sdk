#pragma once
// Decisions behind the Android draw diagnostics (vulkan_android_depth_experiment bits, the shader
// hash lists, the draw index range and the color depth bias; docs/ANDROID_EXPERIMENTS.md in the
// game repository). Plain functions of a draw's facts, so host unit tests check them on the PC.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace rex::graphics::vulkan::android_experiments {

inline constexpr int32_t kDepthAlwaysForColorWithoutWrites = 1;
inline constexpr int32_t kSkipDepthOnlyFloat24Draws = 2;
inline constexpr int32_t kSkipPrepassMeshes = 128;
inline constexpr int32_t kDepthAlwaysForFloat24Color = 256;
inline constexpr int32_t kSkipMidFrameDepthClears = 512;
inline constexpr int32_t kPaintPrepass = 1024;

// xenos::CompareFunction values the color depth bias applies to (reverse Z: nearer is greater).
inline constexpr uint32_t kCompareGreater = 4;
inline constexpr uint32_t kCompareGreaterEqual = 6;
inline constexpr uint32_t kCompareAlways = 7;

enum class SkipReason {
  kNone,
  kDepthOnlyFloat24,
  kPrepassMesh,
  kMidFrameDepthClear,
  kShaderHash,
  kDrawIndex,
};

struct DrawFacts {
  bool has_pixel_shader = false;
  bool float24_depth_bound = false;
  bool rectangle_list = false;
  bool depth_test_enabled = false;
  uint32_t depth_compare = 0;
  bool depth_write = false;
  uint64_t vertex_shader_hash = 0;
  uint64_t pixel_shader_hash = 0;
  uint32_t frame_draw_index = 0;
  bool frame_saw_prepass_mesh = false;
};

struct DrawIndexRange {
  uint32_t first = 0;
  uint32_t last = 0;
  bool Contains(uint32_t index) const { return index >= first && index <= last; }
};

struct DrawSkipLists {
  std::vector<uint64_t> shader_hashes;
  std::optional<DrawIndexRange> draw_indices;
};

inline bool IsBitOn(int32_t bits, int32_t bit) { return (bits & bit) != 0; }

inline bool ContainsHash(const std::vector<uint64_t>& hashes, uint64_t hash) {
  if (!hash) {
    return false;
  }
  for (uint64_t listed : hashes) {
    if (listed == hash) {
      return true;
    }
  }
  return false;
}

inline bool IsListedShader(const std::vector<uint64_t>& hashes, uint64_t vertex_shader_hash,
                           uint64_t pixel_shader_hash) {
  return ContainsHash(hashes, vertex_shader_hash) || ContainsHash(hashes, pixel_shader_hash);
}

inline std::string_view TrimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

inline std::optional<uint64_t> ParseHexHash(std::string_view text) {
  text = TrimSpaces(text);
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// "C84DBBA59A174733, 0x5E9C75A1B61CFC48" -> both values; items that are not hex are dropped.
inline std::vector<uint64_t> ParseShaderHashList(std::string_view list) {
  std::vector<uint64_t> hashes;
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::optional<uint64_t> hash = ParseHexHash(list.substr(0, comma));
    if (hash && *hash) {
      hashes.push_back(*hash);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    list.remove_prefix(comma + 1);
  }
  return hashes;
}

inline std::optional<uint32_t> ParseDecimal(std::string_view text) {
  text = TrimSpaces(text);
  uint32_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// "575" or "576-2460" (inclusive); anything else, or a range that runs backwards, is no range.
inline std::optional<DrawIndexRange> ParseDrawIndexRange(std::string_view text) {
  const size_t dash = text.find('-');
  const std::optional<uint32_t> first = ParseDecimal(text.substr(0, dash));
  if (!first) {
    return std::nullopt;
  }
  if (dash == std::string_view::npos) {
    return DrawIndexRange{*first, *first};
  }
  const std::optional<uint32_t> last = ParseDecimal(text.substr(dash + 1));
  if (!last || *last < *first) {
    return std::nullopt;
  }
  return DrawIndexRange{*first, *last};
}

inline bool IsDepthOnlyFloat24Draw(const DrawFacts& draw) {
  return !draw.has_pixel_shader && draw.float24_depth_bound;
}

// A mesh of the depth prepass: depth only, on the float24 scene depth, not a rectangle list.
inline bool IsPrepassMesh(const DrawFacts& draw) {
  return IsDepthOnlyFloat24Draw(draw) && !draw.rectangle_list;
}

// A depth clear drawn as a rectangle: depth only, compare ALWAYS, depth written.
inline bool IsDepthClearRectangle(const DrawFacts& draw) {
  return IsDepthOnlyFloat24Draw(draw) && draw.rectangle_list && draw.depth_test_enabled &&
         draw.depth_compare == kCompareAlways && draw.depth_write;
}

inline SkipReason GetExperimentSkipReason(int32_t bits, const DrawFacts& draw) {
  if (IsBitOn(bits, kSkipDepthOnlyFloat24Draws) && IsDepthOnlyFloat24Draw(draw)) {
    return SkipReason::kDepthOnlyFloat24;
  }
  if (IsBitOn(bits, kSkipPrepassMeshes) && IsPrepassMesh(draw)) {
    return SkipReason::kPrepassMesh;
  }
  if (IsBitOn(bits, kSkipMidFrameDepthClears) && draw.frame_saw_prepass_mesh &&
      IsDepthClearRectangle(draw)) {
    return SkipReason::kMidFrameDepthClear;
  }
  return SkipReason::kNone;
}

inline SkipReason GetDrawSkipReason(int32_t bits, const DrawSkipLists& lists,
                                    const DrawFacts& draw) {
  const SkipReason experiment = GetExperimentSkipReason(bits, draw);
  if (experiment != SkipReason::kNone) {
    return experiment;
  }
  if (IsListedShader(lists.shader_hashes, draw.vertex_shader_hash, draw.pixel_shader_hash)) {
    return SkipReason::kShaderHash;
  }
  if (lists.draw_indices && lists.draw_indices->Contains(draw.frame_draw_index)) {
    return SkipReason::kDrawIndex;
  }
  return SkipReason::kNone;
}

inline const char* SkipReasonName(SkipReason reason) {
  switch (reason) {
    case SkipReason::kDepthOnlyFloat24:
      return "depth-only";
    case SkipReason::kPrepassMesh:
      return "prepass-mesh";
    case SkipReason::kMidFrameDepthClear:
      return "mid-frame-depth-clear";
    case SkipReason::kShaderHash:
      return "shader-hash";
    case SkipReason::kDrawIndex:
      return "draw-index";
    case SkipReason::kNone:
      break;
  }
  return "none";
}

// The frame's prepass has begun once the guest issued a prepass mesh, skipped or not.
inline bool FrameSawPrepassMeshAfter(const DrawFacts& draw) {
  return draw.frame_saw_prepass_mesh || IsPrepassMesh(draw);
}

inline bool IsColorDepthTestRelaxed(int32_t bits, bool has_pixel_shader, uint32_t color_mask,
                                    bool depth_write, bool float24_depth_bound) {
  const bool color_draw = has_pixel_shader && color_mask;
  if (IsBitOn(bits, kDepthAlwaysForColorWithoutWrites) && color_draw && !depth_write) {
    return true;
  }
  return IsBitOn(bits, kDepthAlwaysForFloat24Color) && color_draw && float24_depth_bound;
}

// Bit 1024: a depth-only draw on the float24 depth with color target 0 bound writes magenta.
inline bool IsPrepassPainted(int32_t bits, bool has_pixel_shader, bool float24_depth_bound,
                             bool color_0_bound) {
  return IsBitOn(bits, kPaintPrepass) && !has_pixel_shader && float24_depth_bound && color_0_bound;
}

inline float GetColorDepthBias(float bias, bool has_pixel_shader, uint32_t color_mask,
                               bool float24_depth_bound, bool depth_test_enabled,
                               uint32_t depth_compare) {
  const bool nearer_passes =
      depth_compare == kCompareGreaterEqual || depth_compare == kCompareGreater;
  if (!has_pixel_shader || !color_mask || !float24_depth_bound || !depth_test_enabled ||
      !nearer_passes) {
    return 0.0f;
  }
  return bias;
}

inline constexpr uint32_t kQualcommVendorId = 0x5143;

// The cvar default (16) covers an Adreno compiler mismatch; other vendors get 0 unless set.
inline float ResolveColorDepthBias(float configured, bool set_by_user, uint32_t vendor_id) {
  if (set_by_user || vendor_id == kQualcommVendorId) {
    return configured;
  }
  return 0.0f;
}

}  // namespace rex::graphics::vulkan::android_experiments
