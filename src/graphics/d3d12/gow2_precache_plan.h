// Bounded, history-driven planning for optional replacement-texture preloading.
#pragma once
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
namespace rex::graphics::d3d12::precache {
inline bool ParseHash(std::string_view token, uint64_t* hash) {
  if (!hash || token.size() != 16)
    return false;
  const auto r = std::from_chars(token.data(), token.data() + token.size(), *hash, 16);
  return r.ec == std::errc{} && r.ptr == token.data() + token.size();
}
inline std::vector<uint64_t> ParseHistory(std::string_view log,
                                          const std::vector<std::string>& roots) {
  bool matches = false;
  for (const auto& root : roots)
    if (log.find("[texpack] " + root + ": ") != std::string_view::npos)
      matches = true;
  if (!matches)
    return {};
  std::vector<uint64_t> hashes;
  constexpr std::string_view marker = "[texpack] replace ";
  size_t pos = 0;
  while (hashes.size() < 8192 && (pos = log.find(marker, pos)) != std::string_view::npos) {
    pos += marker.size();
    if (pos + 22 > log.size() || log.substr(pos + 16, 6) != " with ")
      continue;
    uint64_t hash = 0;
    if (ParseHash(log.substr(pos, 16), &hash))
      hashes.push_back(hash);
  }
  return hashes;
}
// Most recently visited resources of the latest matching sessions get priority.
// Repeated requests for one hash never consume the plan's budget twice.
inline std::vector<uint64_t> Prioritize(const std::vector<std::vector<uint64_t>>& sessions,
                                        size_t maximum = 4096) {
  std::vector<uint64_t> out;
  std::unordered_set<uint64_t> seen;
  for (const auto& session : sessions) {
    for (auto it = session.rbegin(); it != session.rend(); ++it) {
      if (out.size() >= maximum)
        return out;
      if (seen.insert(*it).second)
        out.push_back(*it);
    }
  }
  return out;
}
inline uint64_t BudgetBytes(uint64_t configured_mb, uint64_t available_bytes) {
  if (available_bytes < (uint64_t(1) << 30))
    return 0;
  const uint64_t configured = std::min(configured_mb, uint64_t(3072)) << 20;
  return std::min(configured, available_bytes / 3);
}
inline void CopyMipRows(uint8_t* dest, const uint8_t* source, size_t row_pitch,
                        size_t aligned_pitch, size_t rows) {
  if (row_pitch == aligned_pitch)
    std::memcpy(dest, source, row_pitch * rows);
  else
    for (size_t row = 0; row < rows; ++row)
      std::memcpy(dest + row * aligned_pitch, source + row * row_pitch, row_pitch);
}
inline bool Fits(uint64_t used, uint64_t bytes, uint64_t budget) {
  return bytes != 0 && used <= budget && bytes <= budget - used;
}
}  // namespace rex::graphics::d3d12::precache
