#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {
class D3D12Provider;
class D3D12UploadBufferPool;
}  // namespace rex::ui::d3d12

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

// Plain guest description so the pack manager never touches the protected
// TextureCache::TextureKey type.
struct GuestTextureDesc {
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
  xenos::Endian endianness = xenos::Endian::kNone;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth_or_array = 1;
  uint32_t pitch = 0;
  bool tiled = false;
  bool packed_mips = false;
  uint32_t mip_max_level = 0;
  bool scaled_resolve = false;
  // True when the backend keeps separate unsigned/signed host resources.
  bool signed_separate_format = false;
  // True for 3D-as-2D wrapper textures sharing a 3D key.
  bool force_3d_tiling = false;
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint32_t base_size = 0;
  uint32_t mips_size = 0;
};

struct ReplacementImage {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_levels = 0;
  struct Mip {
    uint32_t row_pitch = 0;
    uint32_t slice_pitch = 0;
    std::vector<uint8_t> bytes;
  };
  std::vector<Mip> mips;
};

// Typeless resource format matching a replacement view format, so unsigned and
// signed views can share one resource.
DXGI_FORMAT TypelessResourceFormatForReplacement(DXGI_FORMAT view_format);
// Signed view for a replacement resource, or the input when no signed variant
// exists (BC1/2/3/6H/7, BGRA8).
DXGI_FORMAT TexpackSignedViewFormat(DXGI_FORMAT view_format);

// Single-threaded (GPU thread only) dump + replacement manager.
class TextureReplacementManager {
 public:
  TextureReplacementManager();
  ~TextureReplacementManager();

  void Initialize(const std::filesystem::path& cache_root, uint32_t title_id,
                  const ui::d3d12::D3D12Provider& provider);
  void Reclaim(uint64_t completed_submission);
  void ClearCache();

  bool HashGuest(const GuestTextureDesc& desc, const uint8_t* base_bytes,
                 const uint8_t* mips_bytes, bool gpu_written, uint64_t* out_hash) const;
  void MaybeDump(const GuestTextureDesc& desc, const uint8_t* base_bytes,
                 uint32_t base_row_pitch_bytes, uint64_t hash);
  bool FindReplacement(uint64_t hash, const GuestTextureDesc& desc, uint32_t max_width_height,
                       bool unaligned_bc_supported, const ReplacementImage** out_image);
  // Replacement variants: the files of <TITLEID>/replace_<variant> (named like the main ones)
  // win over <TITLEID>/replace while the texture_replace_variant setting names that variant. A
  // change is noticed here, once per submission; true means textures must be reloaded.
  bool PollVariant(bool* out_skip_ui_changed);
  // Differs after every variant change, so a texture can tell that its replacement is stale.
  uint32_t variant_epoch() const { return variant_epoch_; }
  // Whether the active variant has a file for this content hash.
  bool HasVariantFile(uint64_t hash) const { return variant_paths_.find(hash) != variant_paths_.end(); }
  // Whether any variant folder has been read; the hashes it names matter for reloads.
  bool HasVariantFiles() const { return !variant_paths_.empty() || variant_was_active_; }
  bool GetReplacementImage(uint64_t hash, const ReplacementImage** out_image) const;
  bool GetReplacementView(uint64_t hash, DXGI_FORMAT* out_format,
                          uint32_t* out_mip_levels) const;
  // Dest must already be in COPY_DEST with barriers submitted.
  bool UploadReplacement(D3D12CommandProcessor& command_processor, ID3D12Resource* dest,
                         const ReplacementImage& image);

 private:
  void ResolveDirs();
  void ScanReplaceDir();
  void ScanDir(const std::filesystem::path& dir,
               std::unordered_map<uint64_t, std::filesystem::path>* out);
  void Refuse(uint64_t hash, const std::string& reason);

  uint32_t title_id_ = 0;
  std::filesystem::path pack_dir_;
  std::filesystem::path dump_dir_;
  std::vector<std::filesystem::path> pack_dirs_;
  std::vector<std::filesystem::path> title_dirs_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> upload_pool_;

  std::unordered_map<uint64_t, std::filesystem::path> replace_paths_;
  std::unordered_map<uint64_t, ReplacementImage> replace_images_;
  std::unordered_map<uint64_t, std::filesystem::path> variant_paths_;
  std::unordered_map<uint64_t, ReplacementImage> variant_images_;
  std::string active_variant_;
  uint32_t variant_epoch_ = 0;
  bool active_skip_ui_ = false;
  bool variant_was_active_ = false;
  std::unordered_set<uint64_t> dumped_;
  std::unordered_set<uint64_t> replaced_;
  std::unordered_set<uint64_t> refused_;
  uint64_t dumped_count_ = 0;
  uint64_t refusal_logs_ = 0;
};

}  // namespace rex::graphics::d3d12
