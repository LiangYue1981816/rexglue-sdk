#include "texture_replacement.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

REXCVAR_DEFINE_BOOL(texture_dump, false, "GPU/Textures",
                    "Write each unique decoded guest texture to the pack dump folder (DDS)");
REXCVAR_DEFINE_STRING(texture_pack_dir, "", "GPU/Textures",
                      "Texture pack root. Empty means the \"textures\" folder next to the "
                      "executable; packs go in <root>/<TITLEID>/replace, dumps in "
                      "<root>/<TITLEID>/dump")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(texture_replace, false, "GPU/Textures",
                    "Substitute pack DDS files for matching guest textures. Applies to "
                    "newly created textures; clear caches or restart to apply everywhere");
REXCVAR_DEFINE_BOOL(texture_replace_skip_ui, false, "GPU/Textures",
                    "Do not replace textures without mip maps, which in this game are mostly "
                    "menus, HUD and text (experimental)");
REXCVAR_DEFINE_STRING(texture_replace_variant, "", "GPU/Textures",
                      "Name of a replacement variant: files of <TITLEID>/replace_<variant> replace "
                      "those of <TITLEID>/replace while it is set. Changing it reloads the textures "
                      "such files name, so a game can switch art at run time (keyboard or pad button "
                      "pictures)");
REXCVAR_DEFINE_BOOL(texture_replace_allow_larger, true, "GPU/Textures",
                    "Allow replacements larger than the guest texture. Same-size packs "
                    "are always safe; larger ones are sharper but can break shaders "
                    "doing texel math (see comment)");

namespace rex::graphics::d3d12 {

namespace {

// Larger replacements break getTextureWeights bilinear reconstruction and
// explicit texel-offset kernels, which compute in guest texels at LOD 0
// (design section 2.4). Usage is a per-shader tfetch property, invisible at
// texture creation time, so this stays a user-visible cvar instead of an
// automatic check. Same-size replacements are exact under every sampling mode.
constexpr size_t kTexpackUploadPageSize = size_t(64) << 20;
constexpr uint64_t kMaxRefusalLogs = 50;
constexpr uint32_t kMaxDdsDimension = 16384;
constexpr uint32_t kMaxDdsMips = 16;

constexpr uint32_t kDdsMagic = 0x20534444;
constexpr uint32_t kFourCC_DX10 = 0x30315844;
constexpr uint32_t kFourCC_DXT1 = 0x31545844;
constexpr uint32_t kFourCC_DXT3 = 0x33545844;
constexpr uint32_t kFourCC_DXT5 = 0x35545844;
constexpr uint32_t kFourCC_ATI1 = 0x31495441;
constexpr uint32_t kFourCC_ATI2 = 0x32495441;
constexpr uint32_t kFourCC_BC4U = 0x55344342;
constexpr uint32_t kFourCC_BC5U = 0x55354342;

constexpr uint32_t kDdsCapsTexture = 0x1000;
constexpr uint32_t kDdsCaps2Cubemap = 0x200;
constexpr uint32_t kDdsCaps2Volume = 0x200000;

struct DdsPixelFormat {
  uint32_t size;
  uint32_t flags;
  uint32_t four_cc;
  uint32_t rgb_bit_count;
  uint32_t r_mask;
  uint32_t g_mask;
  uint32_t b_mask;
  uint32_t a_mask;
};
static_assert(sizeof(DdsPixelFormat) == 32);

struct DdsHeader {
  uint32_t size;
  uint32_t flags;
  uint32_t height;
  uint32_t width;
  uint32_t pitch_or_linear_size;
  uint32_t depth;
  uint32_t mip_map_count;
  uint32_t reserved1[11];
  DdsPixelFormat pixel_format;
  uint32_t caps;
  uint32_t caps2;
  uint32_t caps3;
  uint32_t caps4;
  uint32_t reserved2;
};
static_assert(sizeof(DdsHeader) == 124);

struct DdsHeaderDXT10 {
  uint32_t dxgi_format;
  uint32_t resource_dimension;
  uint32_t misc_flag;
  uint32_t array_size;
  uint32_t misc_flags2;
};
static_assert(sizeof(DdsHeaderDXT10) == 20);

bool IsBlockDxgi(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
      return true;
    default:
      return false;
  }
}

uint32_t DxgiBlockBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
      return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
      return 16;
    default:
      return 0;
  }
}

uint32_t DxgiTexelBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return 4;
    case DXGI_FORMAT_R8G8_UNORM:
      return 2;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

DXGI_FORMAT MapDxgiForReplace(uint32_t dxgi) {
  switch (dxgi) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
      return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
      return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
      return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_UNORM:
      return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_UNORM:
      return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC6H_UF16:
      return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC6H_SF16:
      return DXGI_FORMAT_BC6H_SF16;
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8_UNORM:
      return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_UNORM:
      return DXGI_FORMAT_R8G8_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT DumpDxgiForGuest(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
      return DXGI_FORMAT_BC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3:
      return DXGI_FORMAT_BC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5:
      return DXGI_FORMAT_BC3_UNORM;
    case xenos::TextureFormat::k_DXT5A:
      return DXGI_FORMAT_BC4_UNORM;
    case xenos::TextureFormat::k_DXN:
      return DXGI_FORMAT_BC5_UNORM;
    case xenos::TextureFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return DXGI_FORMAT_R8_UNORM;
    case xenos::TextureFormat::k_8_8:
      return DXGI_FORMAT_R8G8_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

const char* DumpFormatTag(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
      return "bc1";
    case DXGI_FORMAT_BC2_UNORM:
      return "bc2";
    case DXGI_FORMAT_BC3_UNORM:
      return "bc3";
    case DXGI_FORMAT_BC4_UNORM:
      return "bc4";
    case DXGI_FORMAT_BC5_UNORM:
      return "bc5";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return "rgba8";
    case DXGI_FORMAT_R8_UNORM:
      return "r8";
    case DXGI_FORMAT_R8G8_UNORM:
      return "rg8";
    default:
      return "raw";
  }
}

bool IsDumpableGuest(xenos::TextureFormat format) {
  return DumpDxgiForGuest(format) != DXGI_FORMAT_UNKNOWN;
}

// Conservative guest allowlist for replacement. Depth, YUV/video, MPEG,
// interlaced, EDRAM and integer/expand families sample differently enough
// from plain color data that a static DDS would corrupt them.
bool IsReplaceableGuest(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
    case xenos::TextureFormat::k_4_4_4_4:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_16_16:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_CTX1:
      return true;
    default:
      return false;
  }
}

uint32_t Log2Pow2(uint32_t value) {
  switch (value) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    default:
      return 4;
  }
}

}  // namespace

DXGI_FORMAT TypelessResourceFormatForReplacement(DXGI_FORMAT view_format) {
  switch (view_format) {
    case DXGI_FORMAT_BC1_UNORM:
      return DXGI_FORMAT_BC1_TYPELESS;
    case DXGI_FORMAT_BC2_UNORM:
      return DXGI_FORMAT_BC2_TYPELESS;
    case DXGI_FORMAT_BC3_UNORM:
      return DXGI_FORMAT_BC3_TYPELESS;
    case DXGI_FORMAT_BC4_UNORM:
      return DXGI_FORMAT_BC4_TYPELESS;
    case DXGI_FORMAT_BC5_UNORM:
      return DXGI_FORMAT_BC5_TYPELESS;
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
      return DXGI_FORMAT_BC6H_TYPELESS;
    case DXGI_FORMAT_BC7_UNORM:
      return DXGI_FORMAT_BC7_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R8_UNORM:
      return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_R8G8_UNORM:
      return DXGI_FORMAT_R8G8_TYPELESS;
    default:
      return view_format;
  }
}

DXGI_FORMAT TexpackSignedViewFormat(DXGI_FORMAT view_format) {
  switch (view_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R8_UNORM:
      return DXGI_FORMAT_R8_SNORM;
    case DXGI_FORMAT_R8G8_UNORM:
      return DXGI_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_BC4_UNORM:
      return DXGI_FORMAT_BC4_SNORM;
    case DXGI_FORMAT_BC5_UNORM:
      return DXGI_FORMAT_BC5_SNORM;
    default:
      return view_format;
  }
}

TextureReplacementManager::TextureReplacementManager() = default;
TextureReplacementManager::~TextureReplacementManager() = default;

void TextureReplacementManager::Initialize(const std::filesystem::path& cache_root,
                                           uint32_t title_id,
                                           const ui::d3d12::D3D12Provider& provider) {
  title_id_ = title_id;
  if (!upload_pool_) {
    upload_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(provider,
                                                                     kTexpackUploadPageSize);
  }
  replace_paths_.clear();
  replace_images_.clear();
  dumped_.clear();
  replaced_.clear();
  refused_.clear();
  dumped_count_ = 0;
  refusal_logs_ = 0;
  variant_was_active_ = false;
  ResolveDirs();
  ScanReplaceDir();
  REXLOG_INFO("[texpack] total: {} replacements", replace_paths_.size());
}

void TextureReplacementManager::Reclaim(uint64_t completed_submission) {
  if (upload_pool_) {
    upload_pool_->Reclaim(completed_submission);
  }
}

void TextureReplacementManager::ClearCache() {
  if (upload_pool_) {
    upload_pool_->ClearCache();
  }
}

void TextureReplacementManager::ResolveDirs() {
  pack_dirs_.clear();
  std::string override = REXCVAR_GET(texture_pack_dir);
  // Next to the executable by default: that is where players look for mods.
  if (!override.empty()) {
    size_t start = 0;
    while (start < override.size()) {
      size_t end = override.find(';', start);
      if (end == std::string::npos) {
        end = override.size();
      }
      if (end != start) {
        pack_dirs_.emplace_back(override.substr(start, end - start));
      }
      start = end + 1;
    }
  }
  if (pack_dirs_.empty()) {
    pack_dirs_.push_back(rex::filesystem::GetExecutableFolder() / "textures");
  }
  char title[9];
  if (title_id_ != 0) {
    std::snprintf(title, sizeof(title), "%08X", title_id_);
  } else {
    std::snprintf(title, sizeof(title), "dump");
  }
  pack_dir_ = pack_dirs_.back();
  dump_dir_ = pack_dir_ / title / "dump";
  title_dirs_.clear();
  title_dirs_.reserve(pack_dirs_.size());
  for (const std::filesystem::path& pack_dir : pack_dirs_) {
    title_dirs_.push_back(pack_dir / title);
  }
}

void TextureReplacementManager::ScanReplaceDir() {
  for (size_t i = 0; i < title_dirs_.size(); ++i) {
    size_t previous_size = replace_paths_.size();
    ScanDir(title_dirs_[i] / "replace", &replace_paths_);
    REXLOG_INFO("[texpack] {}: {} replacements", rex::path_to_utf8(pack_dirs_[i]),
                replace_paths_.size() - previous_size);
  }
}

bool TextureReplacementManager::PollVariant(bool* out_skip_ui_changed) {
  *out_skip_ui_changed = false;
  const std::string wanted = REXCVAR_GET(texture_replace_variant);
  const bool skip_ui = REXCVAR_GET(texture_replace_skip_ui);
  const bool skip_ui_changed = skip_ui != active_skip_ui_;
  if (wanted == active_variant_ && !skip_ui_changed) {
    return false;
  }
  *out_skip_ui_changed = skip_ui_changed;
  active_skip_ui_ = skip_ui;
  active_variant_ = wanted;
  variant_paths_.clear();
  variant_images_.clear();
  if (!wanted.empty()) {
    for (size_t i = 0; i < title_dirs_.size(); ++i) {
      size_t previous_size = variant_paths_.size();
      ScanDir(title_dirs_[i] / ("replace_" + wanted), &variant_paths_);
      REXLOG_INFO("[texpack] {}: {} replacements", rex::path_to_utf8(pack_dirs_[i]),
                  variant_paths_.size() - previous_size);
    }
    variant_was_active_ = true;
  }
  ++variant_epoch_;
  REXLOG_INFO("[texpack] variant '{}' ({} replacements)", wanted, variant_paths_.size());
  return true;
}

void TextureReplacementManager::ScanDir(const std::filesystem::path& dir,
                                        std::unordered_map<uint64_t, std::filesystem::path>* out) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return;
  }
  std::filesystem::directory_iterator it(dir, ec);
  std::filesystem::directory_iterator end;
  while (!ec && it != end) {
    const std::filesystem::directory_entry entry = *it;  // a copy: increment() below replaces what the iterator holds
    it.increment(ec);
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      continue;
    }
    std::string ext = entry.path().extension().string();
    for (char& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext != ".dds") {
      continue;
    }
    std::string stem = entry.path().stem().string();
    // Dumps are named <hash>_<format>_<WxH>; both that name and the bare <hash> work.
    if (stem.size() < 16 || (stem.size() > 16 && stem[16] != '_')) {
      continue;
    }
    uint64_t hash = 0;
    const char* first = stem.data();
    const char* last = first + 16;
    auto result = std::from_chars(first, last, hash, 16);
    if (result.ec != std::errc() || result.ptr != last) {
      continue;
    }
    out->emplace(hash, entry.path());
  }
}

bool TextureReplacementManager::HashGuest(const GuestTextureDesc& desc, const uint8_t* base_bytes,
                                          const uint8_t* mips_bytes, bool gpu_written,
                                          uint64_t* out_hash) const {
  bool want_dump = REXCVAR_GET(texture_dump);
  bool want_replace = REXCVAR_GET(texture_replace);
  if (!want_dump && !want_replace) {
    return false;
  }
  if (desc.scaled_resolve || gpu_written || desc.force_3d_tiling) {
    return false;
  }
  if (desc.dimension != xenos::DataDimension::k2DOrStacked || desc.depth_or_array != 1) {
    return false;
  }
  if (desc.signed_separate_format || !IsReplaceableGuest(desc.format)) {
    return false;
  }
  if (!want_replace && !IsDumpableGuest(desc.format)) {
    return false;
  }
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t level_tag = 0;
  if (desc.base_size != 0 && base_bytes != nullptr) {
    data = base_bytes;
    size = desc.base_size;
    level_tag = 0;
  } else if (desc.mips_size != 0 && mips_bytes != nullptr) {
    data = mips_bytes;
    size = desc.mips_size;
    level_tag = 1;
  } else {
    return false;
  }
  // Raw guest bytes (still tiled and big-endian) hash stably across runs;
  // key fields disambiguate different views of the same bytes.
  uint64_t content = XXH3_64bits(data, size);
  uint64_t words[6];
  words[0] = content;
  words[1] = uint64_t(desc.width) | (uint64_t(desc.height) << 32);
  words[2] = uint64_t(desc.depth_or_array) | (uint64_t(desc.pitch) << 32);
  words[3] = uint64_t(desc.format) | (uint64_t(desc.endianness) << 32);
  words[4] = uint64_t(desc.dimension) | (uint64_t(desc.tiled) << 32) |
             (uint64_t(desc.packed_mips) << 33) | (uint64_t(desc.mip_max_level) << 34) |
             (uint64_t(level_tag) << 40);
  words[5] = uint64_t(desc.base_size) | (uint64_t(desc.mips_size) << 32);
  *out_hash = XXH3_64bits(words, sizeof(words));
  return true;
}

namespace {

bool DecodeGuestBase(const GuestTextureDesc& desc, const uint8_t* base_bytes,
                     uint32_t base_row_pitch_bytes, DXGI_FORMAT* out_format,
                     std::vector<uint8_t>* out_linear) {
  DXGI_FORMAT dxgi = DumpDxgiForGuest(desc.format);
  if (dxgi == DXGI_FORMAT_UNKNOWN || base_bytes == nullptr || desc.base_size == 0) {
    return false;
  }
  const FormatInfo* info = FormatInfo::Get(desc.format);
  uint32_t block_w = info->block_width;
  uint32_t block_h = info->block_height;
  uint32_t bytes_per_block = info->bytes_per_block();
  if (!block_w || !block_h || !bytes_per_block) {
    return false;
  }
  if (IsBlockDxgi(dxgi) && ((desc.width & 3) != 0 || (desc.height & 3) != 0)) {
    return false;
  }
  uint32_t width_blocks = (desc.width + block_w - 1) / block_w;
  uint32_t height_blocks = (desc.height + block_h - 1) / block_h;
  if (!width_blocks || !height_blocks) {
    return false;
  }
  size_t total = size_t(width_blocks) * height_blocks * bytes_per_block;
  out_linear->resize(total);
  uint8_t* dst = out_linear->data();
  const uint8_t* end = base_bytes + desc.base_size;
  uint32_t row_bytes = width_blocks * bytes_per_block;
  if (!desc.tiled) {
    if (base_row_pitch_bytes < row_bytes) {
      return false;
    }
    for (uint32_t y = 0; y < height_blocks; ++y) {
      const uint8_t* src = base_bytes + size_t(y) * base_row_pitch_bytes;
      if (src < base_bytes || src + row_bytes > end) {
        return false;
      }
      std::memcpy(dst + size_t(y) * row_bytes, src, row_bytes);
    }
  } else {
    if (base_row_pitch_bytes == 0 || base_row_pitch_bytes % bytes_per_block != 0) {
      return false;
    }
    uint32_t pitch_blocks = base_row_pitch_bytes / bytes_per_block;
    uint32_t log2_bpb = Log2Pow2(bytes_per_block);
    for (uint32_t y = 0; y < height_blocks; ++y) {
      for (uint32_t x = 0; x < width_blocks; ++x) {
        int32_t offset =
            texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2_bpb);
        if (offset < 0 || uint64_t(offset) + bytes_per_block > desc.base_size) {
          return false;
        }
        std::memcpy(dst + (size_t(y) * width_blocks + x) * bytes_per_block,
                    base_bytes + offset, bytes_per_block);
      }
    }
  }
  if (desc.endianness != xenos::Endian::kNone) {
    texture_conversion::CopySwapBlock(desc.endianness, dst, dst, total);
  }
  *out_format = dxgi;
  return true;
}

bool WriteDdsFile(const std::filesystem::path& path, DXGI_FORMAT format, uint32_t width,
                  uint32_t height, const std::vector<uint8_t>& bytes) {
  FILE* file = rex::filesystem::OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  bool ok = false;
  do {
    uint32_t magic = kDdsMagic;
    DdsHeader header = {};
    header.size = sizeof(header);
    header.flags = 0x1 | 0x2 | 0x4 | 0x1000;
    header.height = height;
    header.width = width;
    header.depth = 1;
    header.mip_map_count = 1;
    header.pixel_format.size = sizeof(header.pixel_format);
    header.pixel_format.flags = 0x4;
    header.pixel_format.four_cc = kFourCC_DX10;
    header.caps = kDdsCapsTexture;
    DdsHeaderDXT10 dx10 = {};
    dx10.dxgi_format = uint32_t(format);
    dx10.resource_dimension = 3;
    dx10.array_size = 1;
    if (std::fwrite(&magic, sizeof(magic), 1, file) != 1 ||
        std::fwrite(&header, sizeof(header), 1, file) != 1 ||
        std::fwrite(&dx10, sizeof(dx10), 1, file) != 1) {
      break;
    }
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
      break;
    }
    ok = true;
  } while (false);
  std::fclose(file);
  return ok;
}

}  // namespace

void TextureReplacementManager::MaybeDump(const GuestTextureDesc& desc, const uint8_t* base_bytes,
                                          uint32_t base_row_pitch_bytes, uint64_t hash) {
  if (!REXCVAR_GET(texture_dump) || base_bytes == nullptr || desc.base_size == 0) {
    return;
  }
  if (dumped_.find(hash) != dumped_.end()) {
    return;
  }
  if (dump_dir_.empty()) {
    ResolveDirs();
  }
  DXGI_FORMAT dxgi = DumpDxgiForGuest(desc.format);
  if (dxgi == DXGI_FORMAT_UNKNOWN) {
    return;
  }
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx_%s_%ux%u.dds",
                static_cast<unsigned long long>(hash), DumpFormatTag(dxgi), desc.width,
                desc.height);
  std::filesystem::path path = dump_dir_ / name;
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && !ec) {
    dumped_.insert(hash);
    return;
  }
  // CPU untile plus endian swap yields bytes identical to host mip 0 for the
  // supported formats, without stalling the GPU for a readback.
  std::vector<uint8_t> linear;
  DXGI_FORMAT decoded = DXGI_FORMAT_UNKNOWN;
  if (!DecodeGuestBase(desc, base_bytes, base_row_pitch_bytes, &decoded, &linear)) {
    return;
  }
  std::filesystem::create_directories(dump_dir_, ec);
  if (!WriteDdsFile(path, decoded, desc.width, desc.height, linear)) {
    REXGPU_ERROR("[texpack] failed to write {}", rex::path_to_utf8(path));
    return;
  }
  dumped_.insert(hash);
  ++dumped_count_;
  REXLOG_INFO("[texpack] dumped {:016x} {} {}x{} (#{})", hash, DumpFormatTag(decoded),
              desc.width, desc.height, dumped_count_);
}

namespace {

bool ReadDdsFile(const std::filesystem::path& path, ReplacementImage* out, std::string* out_error) {
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  if (!file) {
    *out_error = "open failed";
    return false;
  }
  if (!rex::filesystem::Seek(file, 0, SEEK_END)) {
    std::fclose(file);
    *out_error = "seek failed";
    return false;
  }
  int64_t file_size = rex::filesystem::Tell(file);
  if (file_size < int64_t(4 + sizeof(DdsHeader)) ||
      !rex::filesystem::Seek(file, 0, SEEK_SET)) {
    std::fclose(file);
    *out_error = "truncated header";
    return false;
  }
  uint32_t magic = 0;
  DdsHeader header = {};
  bool ok = false;
  do {
    if (std::fread(&magic, sizeof(magic), 1, file) != 1 ||
        std::fread(&header, sizeof(header), 1, file) != 1) {
      *out_error = "truncated header";
      break;
    }
    if (magic != kDdsMagic || header.size != sizeof(header) ||
        header.pixel_format.size != sizeof(header.pixel_format)) {
      *out_error = "bad magic";
      break;
    }
    if ((header.caps2 & (kDdsCaps2Cubemap | kDdsCaps2Volume)) != 0) {
      *out_error = "cube/volume not supported";
      break;
    }
    if (header.width == 0 || header.height == 0 || header.width > kMaxDdsDimension ||
        header.height > kMaxDdsDimension) {
      *out_error = "bad dimensions";
      break;
    }
    uint32_t mip_count = header.mip_map_count == 0 ? 1 : header.mip_map_count;
    mip_count = std::min(mip_count, kMaxDdsMips);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool swap_rb = false;
    bool force_opaque = false;
    size_t data_offset = 4 + sizeof(header);
    if ((header.pixel_format.flags & 0x4) != 0 &&
        header.pixel_format.four_cc == kFourCC_DX10) {
      DdsHeaderDXT10 dx10 = {};
      if (file_size < int64_t(data_offset + sizeof(dx10)) ||
          std::fread(&dx10, sizeof(dx10), 1, file) != 1) {
        *out_error = "truncated DX10 header";
        break;
      }
      data_offset += sizeof(dx10);
      if (dx10.resource_dimension != 3 || dx10.array_size != 1) {
        *out_error = "only 2D single images supported";
        break;
      }
      format = MapDxgiForReplace(dx10.dxgi_format);
    } else if ((header.pixel_format.flags & 0x4) != 0) {
      switch (header.pixel_format.four_cc) {
        case kFourCC_DXT1:
          format = DXGI_FORMAT_BC1_UNORM;
          break;
        case kFourCC_DXT3:
          format = DXGI_FORMAT_BC2_UNORM;
          break;
        case kFourCC_DXT5:
          format = DXGI_FORMAT_BC3_UNORM;
          break;
        case kFourCC_ATI1:
        case kFourCC_BC4U:
          format = DXGI_FORMAT_BC4_UNORM;
          break;
        case kFourCC_ATI2:
        case kFourCC_BC5U:
          format = DXGI_FORMAT_BC5_UNORM;
          break;
        default:
          break;
      }
    } else if ((header.pixel_format.flags & 0x40) != 0) {
      uint32_t bits = header.pixel_format.rgb_bit_count;
      uint32_t r = header.pixel_format.r_mask;
      uint32_t g = header.pixel_format.g_mask;
      uint32_t b = header.pixel_format.b_mask;
      uint32_t a = header.pixel_format.a_mask;
      if (bits == 32 && r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF &&
          (a == 0xFF000000 || a == 0)) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_rb = true;
        force_opaque = (a == 0);
      } else if (bits == 32 && r == 0x000000FF && g == 0x0000FF00 && b == 0x00FF0000 &&
                 a == 0xFF000000) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
      } else if (bits == 16 && r == 0x00FF && g == 0xFF00) {
        format = DXGI_FORMAT_R8G8_UNORM;
      } else if (bits == 8 && r == 0xFF) {
        format = DXGI_FORMAT_R8_UNORM;
      }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
      *out_error = "unsupported pixel format";
      break;
    }
    bool block = IsBlockDxgi(format);
    uint32_t block_bytes = block ? DxgiBlockBytes(format) : DxgiTexelBytes(format);
    if (!block_bytes) {
      *out_error = "unsupported pixel format";
      break;
    }
    if (!rex::filesystem::Seek(file, int64_t(data_offset), SEEK_SET)) {
      *out_error = "seek failed";
      break;
    }
    ReplacementImage image;
    image.format = format;
    image.width = header.width;
    image.height = header.height;
    size_t file_bytes = size_t(file_size);
    for (uint32_t level = 0; level < mip_count; ++level) {
      uint32_t w = std::max(header.width >> level, uint32_t(1));
      uint32_t h = std::max(header.height >> level, uint32_t(1));
      uint32_t tight_pitch = block ? ((w + 3) / 4) * block_bytes : w * block_bytes;
      uint32_t rows = block ? (h + 3) / 4 : h;
      size_t slice = size_t(tight_pitch) * rows;
      if (data_offset + slice > file_bytes) {
        break;
      }
      ReplacementImage::Mip mip;
      mip.row_pitch = tight_pitch;
      mip.slice_pitch = uint32_t(slice);
      mip.bytes.resize(slice);
      if (slice != 0 && std::fread(mip.bytes.data(), 1, slice, file) != slice) {
        break;
      }
      data_offset += slice;
      if (swap_rb) {
        for (size_t i = 0; i + 4 <= mip.bytes.size(); i += 4) {
          std::swap(mip.bytes[i], mip.bytes[i + 2]);
          if (force_opaque) {
            mip.bytes[i + 3] = 0xFF;
          }
        }
      }
      image.mips.push_back(std::move(mip));
    }
    if (image.mips.empty()) {
      *out_error = "truncated mips";
      break;
    }
    image.mip_levels = uint32_t(image.mips.size());
    *out = std::move(image);
    ok = true;
  } while (false);
  std::fclose(file);
  return ok;
}

bool ValidateReplacement(const GuestTextureDesc& desc, const ReplacementImage& image,
                          uint32_t max_width_height, bool unaligned_bc_supported,
                          std::string* out_reason) {
  uint32_t guest_levels = desc.mip_max_level + 1;
  if (image.mip_levels < guest_levels) {
    char reason[160];
    std::snprintf(reason, sizeof(reason),
                  "only %u mips for %u guest levels (save the DDS with a full mip chain, e.g. "
                  "texconv -m 0)",
                  image.mip_levels, guest_levels);
    *out_reason = reason;
    return false;
  }
  if (image.width > max_width_height || image.height > max_width_height) {
    *out_reason = "exceeds device texture limits";
    return false;
  }
  if (IsBlockDxgi(image.format) && ((image.width & 3) != 0 || (image.height & 3) != 0) &&
      !unaligned_bc_supported) {
    *out_reason = "unaligned block texture unsupported by device";
    return false;
  }
  if (!REXCVAR_GET(texture_replace_allow_larger) &&
      (image.width != desc.width || image.height != desc.height)) {
    *out_reason = "larger than guest (texture_replace_allow_larger is off)";
    return false;
  }
  if (image.width < desc.width || image.height < desc.height) {
    *out_reason = "smaller than guest";
    return false;
  }
  uint32_t block_bytes =
      IsBlockDxgi(image.format) ? DxgiBlockBytes(image.format) : DxgiTexelBytes(image.format);
  uint32_t tight =
      IsBlockDxgi(image.format) ? ((image.width + 3) / 4) * block_bytes : image.width * block_bytes;
  uint32_t rows = IsBlockDxgi(image.format) ? (image.height + 3) / 4 : image.height;
  if (size_t(rex::align(tight, uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT))) * rows >
      kTexpackUploadPageSize) {
    *out_reason = "mip 0 too large to stage";
    return false;
  }
  return true;
}

}  // namespace

void TextureReplacementManager::Refuse(uint64_t hash, const std::string& reason) {
  if (!refused_.insert(hash).second) {
    return;
  }
  if (refusal_logs_ < kMaxRefusalLogs) {
    REXLOG_INFO("[texpack] replace {:016x} refused: {}", hash, reason);
  } else if (refusal_logs_ == kMaxRefusalLogs) {
    REXLOG_INFO("[texpack] further replace refusals suppressed");
  }
  ++refusal_logs_;
}

bool TextureReplacementManager::FindReplacement(uint64_t hash, const GuestTextureDesc& desc,
                                                uint32_t max_width_height,
                                                bool unaligned_bc_supported,
                                                const ReplacementImage** out_image) {
  *out_image = nullptr;
  if (!REXCVAR_GET(texture_replace)) {
    return false;
  }
  if (REXCVAR_GET(texture_replace_skip_ui) && desc.mip_max_level == 0) {
    return false;
  }
  // A file of the active variant wins; its refusals and log lines are kept apart from the main
  // file's by this salt.
  const bool variant = variant_paths_.find(hash) != variant_paths_.end();
  auto& images = variant ? variant_images_ : replace_images_;
  auto& paths = variant ? variant_paths_ : replace_paths_;
  const uint64_t key = variant ? hash ^ UINT64_C(0x5a5a5a5a5a5a5a5a) : hash;
  auto cached = images.find(hash);
  if (cached != images.end()) {
    *out_image = &cached->second;
    return true;
  }
  if (refused_.find(key) != refused_.end()) {
    return false;
  }
  auto found = paths.find(hash);
  if (found == paths.end()) {
    return false;
  }
  ReplacementImage image;
  std::string error;
  if (!ReadDdsFile(found->second, &image, &error)) {
    Refuse(key, "bad DDS (" + error + ")");
    return false;
  }
  if (!ValidateReplacement(desc, image, max_width_height, unaligned_bc_supported, &error)) {
    Refuse(key, error);
    return false;
  }
  auto inserted = images.emplace(hash, std::move(image));
  *out_image = &inserted.first->second;
  if (replaced_.insert(key).second) {
    REXLOG_INFO("[texpack] replace {:016x} with {}x{} ({} mips) for guest {}x{}", hash,
                (*out_image)->width, (*out_image)->height, (*out_image)->mip_levels, desc.width,
                desc.height);
  }
  return true;
}

bool TextureReplacementManager::GetReplacementImage(uint64_t hash,
                                                    const ReplacementImage** out_image) const {
  *out_image = nullptr;
  auto variant = variant_images_.find(hash);
  if (variant != variant_images_.end() && HasVariantFile(hash)) {
    *out_image = &variant->second;
    return true;
  }
  auto it = replace_images_.find(hash);
  if (it == replace_images_.end()) {
    return false;
  }
  *out_image = &it->second;
  return true;
}

bool TextureReplacementManager::GetReplacementView(uint64_t hash, DXGI_FORMAT* out_format,
                                                   uint32_t* out_mip_levels) const {
  auto variant = variant_images_.find(hash);
  auto it = replace_images_.find(hash);
  if (variant != variant_images_.end() && HasVariantFile(hash)) {
    it = variant;
  } else if (it == replace_images_.end()) {
    return false;
  }
  *out_format = it->second.format;
  *out_mip_levels = it->second.mip_levels;
  return true;
}

bool TextureReplacementManager::UploadReplacement(D3D12CommandProcessor& command_processor,
                                                  ID3D12Resource* dest,
                                                  const ReplacementImage& image) {
  if (!upload_pool_ || !dest || image.mips.empty()) {
    return false;
  }
  D3D12_RESOURCE_DESC dest_desc = dest->GetDesc();
  DeferredCommandList& command_list = command_processor.GetDeferredCommandList();
  uint64_t submission = command_processor.GetCurrentSubmission();
  for (uint32_t level = 0; level < image.mip_levels && level < image.mips.size(); ++level) {
    const ReplacementImage::Mip& mip = image.mips[level];
    uint32_t width = std::max(image.width >> level, uint32_t(1));
    uint32_t height = std::max(image.height >> level, uint32_t(1));
    bool block = IsBlockDxgi(image.format);
    uint32_t rows = block ? (height + 3) / 4 : height;
    if (rows == 0 || mip.row_pitch == 0) {
      return false;
    }
    if (mip.bytes.size() < size_t(mip.row_pitch) * rows) {
      return false;
    }
    uint32_t aligned_pitch =
        rex::align(mip.row_pitch, uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    size_t staged_size = size_t(aligned_pitch) * rows;
    ID3D12Resource* upload_buffer = nullptr;
    size_t upload_offset = 0;
    uint8_t* mapping = upload_pool_->Request(
        submission, staged_size, size_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT), &upload_buffer,
        &upload_offset, nullptr);
    if (!mapping || !upload_buffer) {
      return false;
    }
    for (uint32_t row = 0; row < rows; ++row) {
      std::memcpy(mapping + size_t(row) * aligned_pitch,
                  mip.bytes.data() + size_t(row) * mip.row_pitch, mip.row_pitch);
    }
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = upload_offset;
    src.PlacedFootprint.Footprint.Format = dest_desc.Format;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = aligned_pitch;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = dest;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = level;
    command_list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  return true;
}

}  // namespace rex::graphics::d3d12
