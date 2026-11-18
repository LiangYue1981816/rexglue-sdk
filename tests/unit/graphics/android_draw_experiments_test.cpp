// The Android draw diagnostics: which draws each vulkan_android_depth_experiment bit, shader hash
// list and draw range selects.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/graphics/vulkan/android_draw_experiments.h>

namespace ax = rex::graphics::vulkan::android_experiments;

namespace {

constexpr uint32_t kCompareLess = 1;
constexpr uint64_t kVertexShader = 0x1111222233334444ull;
constexpr uint64_t kPixelShader = 0xC84DBBA59A174733ull;

ax::DrawFacts PrepassMesh() {
  ax::DrawFacts draw;
  draw.float24_depth_bound = true;
  draw.depth_test_enabled = true;
  draw.depth_compare = ax::kCompareGreaterEqual;
  draw.depth_write = true;
  draw.vertex_shader_hash = kVertexShader;
  return draw;
}

ax::DrawFacts DepthClearRectangle() {
  ax::DrawFacts draw;
  draw.float24_depth_bound = true;
  draw.rectangle_list = true;
  draw.depth_test_enabled = true;
  draw.depth_compare = ax::kCompareAlways;
  draw.depth_write = true;
  draw.vertex_shader_hash = kVertexShader;
  return draw;
}

ax::DrawFacts BasePassDraw() {
  ax::DrawFacts draw = PrepassMesh();
  draw.has_pixel_shader = true;
  draw.pixel_shader_hash = kPixelShader;
  return draw;
}

// Numbers a frame's draws as the command processor does and returns each one's skip reason.
std::vector<ax::SkipReason> SkipReasonsOfFrame(int32_t bits, const ax::DrawSkipLists& lists,
                                               std::vector<ax::DrawFacts> frame) {
  std::vector<ax::SkipReason> reasons;
  bool saw_prepass_mesh = false;
  for (uint32_t i = 0; i < frame.size(); ++i) {
    frame[i].frame_draw_index = i;
    frame[i].frame_saw_prepass_mesh = saw_prepass_mesh;
    saw_prepass_mesh = ax::FrameSawPrepassMeshAfter(frame[i]);
    reasons.push_back(ax::GetDrawSkipReason(bits, lists, frame[i]));
  }
  return reasons;
}

}  // namespace

TEST_CASE("shader hash lists take FRAMETRACE's hex and drop the rest", "[android][draw]") {
  const std::vector<uint64_t> hashes =
      ax::ParseShaderHashList(" C84DBBA59A174733, 0x5e9c75a1b61cfc48 ,junk,,0,12345678901234567");
  REQUIRE(hashes.size() == 2);
  CHECK(hashes[0] == 0xC84DBBA59A174733ull);
  CHECK(hashes[1] == 0x5E9C75A1B61CFC48ull);
  CHECK(ax::ParseShaderHashList("").empty());
}

TEST_CASE("a draw range is one number or first-last", "[android][draw]") {
  const auto single = ax::ParseDrawIndexRange("575");
  REQUIRE(single.has_value());
  CHECK(single->first == 575);
  CHECK(single->last == 575);
  const auto range = ax::ParseDrawIndexRange(" 576 - 2460 ");
  REQUIRE(range.has_value());
  CHECK(range->Contains(576));
  CHECK(range->Contains(2460));
  CHECK_FALSE(range->Contains(575));
  CHECK_FALSE(range->Contains(2461));
  CHECK_FALSE(ax::ParseDrawIndexRange("").has_value());
  CHECK_FALSE(ax::ParseDrawIndexRange("10-5").has_value());
  CHECK_FALSE(ax::ParseDrawIndexRange("a-b").has_value());
  CHECK_FALSE(ax::ParseDrawIndexRange("3-").has_value());
}

TEST_CASE("bit 128 skips the prepass meshes and keeps the depth clears", "[android][draw]") {
  const auto reasons = SkipReasonsOfFrame(
      ax::kSkipPrepassMeshes, {}, {DepthClearRectangle(), PrepassMesh(), DepthClearRectangle()});
  CHECK(reasons == std::vector<ax::SkipReason>{ax::SkipReason::kNone, ax::SkipReason::kPrepassMesh,
                                               ax::SkipReason::kNone});
}

TEST_CASE("bit 2 skips every depth-only draw on the float24 depth", "[android][draw]") {
  const auto reasons = SkipReasonsOfFrame(ax::kSkipDepthOnlyFloat24Draws, {},
                                          {DepthClearRectangle(), PrepassMesh(), BasePassDraw()});
  CHECK(reasons == std::vector<ax::SkipReason>{ax::SkipReason::kDepthOnlyFloat24,
                                               ax::SkipReason::kDepthOnlyFloat24,
                                               ax::SkipReason::kNone});
}

TEST_CASE("bit 512 keeps the frame's first depth clear and skips the ones after the prepass",
          "[android][draw]") {
  ax::DrawFacts depth_tested_rectangle = DepthClearRectangle();
  depth_tested_rectangle.depth_compare = ax::kCompareGreaterEqual;
  ax::DrawFacts colored_rectangle = DepthClearRectangle();
  colored_rectangle.has_pixel_shader = true;
  const auto reasons = SkipReasonsOfFrame(
      ax::kSkipMidFrameDepthClears, {},
      {DepthClearRectangle(), PrepassMesh(), PrepassMesh(), DepthClearRectangle(), BasePassDraw(),
       depth_tested_rectangle, colored_rectangle, DepthClearRectangle()});
  CHECK(reasons == std::vector<ax::SkipReason>{
                       ax::SkipReason::kNone, ax::SkipReason::kNone, ax::SkipReason::kNone,
                       ax::SkipReason::kMidFrameDepthClear, ax::SkipReason::kNone,
                       ax::SkipReason::kNone, ax::SkipReason::kNone,
                       ax::SkipReason::kMidFrameDepthClear});
}

TEST_CASE("a prepass mesh skipped by bit 128 still starts the prepass for bit 512",
          "[android][draw]") {
  const auto reasons =
      SkipReasonsOfFrame(ax::kSkipPrepassMeshes | ax::kSkipMidFrameDepthClears, {},
                         {DepthClearRectangle(), PrepassMesh(), DepthClearRectangle()});
  CHECK(reasons == std::vector<ax::SkipReason>{ax::SkipReason::kNone, ax::SkipReason::kPrepassMesh,
                                               ax::SkipReason::kMidFrameDepthClear});
}

TEST_CASE("listed shaders and the draw range skip draws with no experiment bit",
          "[android][draw]") {
  ax::DrawSkipLists by_pixel_shader;
  by_pixel_shader.shader_hashes = {kPixelShader};
  CHECK(SkipReasonsOfFrame(0, by_pixel_shader, {PrepassMesh(), BasePassDraw()}) ==
        std::vector<ax::SkipReason>{ax::SkipReason::kNone, ax::SkipReason::kShaderHash});

  ax::DrawSkipLists by_vertex_shader;
  by_vertex_shader.shader_hashes = {kVertexShader};
  CHECK(SkipReasonsOfFrame(0, by_vertex_shader, {PrepassMesh(), BasePassDraw()}) ==
        std::vector<ax::SkipReason>{ax::SkipReason::kShaderHash, ax::SkipReason::kShaderHash});

  ax::DrawSkipLists by_index;
  by_index.draw_indices = ax::DrawIndexRange{1, 1};
  CHECK(SkipReasonsOfFrame(0, by_index, {PrepassMesh(), PrepassMesh(), BasePassDraw()}) ==
        std::vector<ax::SkipReason>{ax::SkipReason::kNone, ax::SkipReason::kDrawIndex,
                                    ax::SkipReason::kNone});
}

TEST_CASE("a draw without a pixel shader never matches a listed hash through its zero hash",
          "[android][draw]") {
  ax::DrawSkipLists lists;
  lists.shader_hashes = {0, kPixelShader};
  CHECK_FALSE(ax::IsListedShader(lists.shader_hashes, 0x9999ull, 0));
}

TEST_CASE("bit 1 relaxes color draws without depth writes, bit 256 also the writers on float24",
          "[android][draw]") {
  constexpr uint32_t kColor = 0xF;
  CHECK(ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForColorWithoutWrites, true, kColor, false,
                                    true));
  CHECK_FALSE(ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForColorWithoutWrites, true, kColor,
                                          true, true));
  CHECK(ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForFloat24Color, true, kColor, true, true));
  CHECK_FALSE(
      ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForFloat24Color, true, kColor, true, false));
  CHECK_FALSE(ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForFloat24Color, false, 0, true, true));
  CHECK_FALSE(ax::IsColorDepthTestRelaxed(ax::kDepthAlwaysForFloat24Color, true, 0, true, true));
  CHECK_FALSE(ax::IsColorDepthTestRelaxed(0, true, kColor, false, true));
}

TEST_CASE("bit 1024 paints depth-only float24 draws where color target 0 is bound",
          "[android][draw]") {
  CHECK(ax::IsPrepassPainted(ax::kPaintPrepass, false, true, true));
  CHECK_FALSE(ax::IsPrepassPainted(ax::kPaintPrepass, false, true, false));
  CHECK_FALSE(ax::IsPrepassPainted(ax::kPaintPrepass, true, true, true));
  CHECK_FALSE(ax::IsPrepassPainted(ax::kPaintPrepass, false, false, true));
  CHECK_FALSE(ax::IsPrepassPainted(0, false, true, true));
}

TEST_CASE("the color depth bias reaches only nearer-passing color draws on float24",
          "[android][draw]") {
  constexpr float kBias = 16.0f;
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, true, true, ax::kCompareGreaterEqual) == kBias);
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, true, true, ax::kCompareGreater) == kBias);
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, true, true, kCompareLess) == 0.0f);
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, true, true, ax::kCompareAlways) == 0.0f);
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, true, false, ax::kCompareGreaterEqual) == 0.0f);
  CHECK(ax::GetColorDepthBias(kBias, true, 0, true, true, ax::kCompareGreaterEqual) == 0.0f);
  CHECK(ax::GetColorDepthBias(kBias, false, 0xF, true, true, ax::kCompareGreaterEqual) == 0.0f);
  CHECK(ax::GetColorDepthBias(kBias, true, 0xF, false, true, ax::kCompareGreaterEqual) == 0.0f);
}

TEST_CASE("the color depth bias default applies only on Qualcomm unless the user sets it",
          "[android][draw]") {
  constexpr float kDefault = 16.0f;
  CHECK(ax::ResolveColorDepthBias(kDefault, false, ax::kQualcommVendorId) == kDefault);
  CHECK(ax::ResolveColorDepthBias(kDefault, false, 0x13B5) == 0.0f);  // ARM Mali
  CHECK(ax::ResolveColorDepthBias(kDefault, false, 0x1002) == 0.0f);  // AMD (Samsung Xclipse)
  CHECK(ax::ResolveColorDepthBias(kDefault, false, 0x1010) == 0.0f);  // Imagination PowerVR
  CHECK(ax::ResolveColorDepthBias(8.0f, true, 0x13B5) == 8.0f);
  CHECK(ax::ResolveColorDepthBias(0.0f, true, ax::kQualcommVendorId) == 0.0f);
  CHECK(ax::ResolveColorDepthBias(-4.0f, true, ax::kQualcommVendorId) == -4.0f);
}
