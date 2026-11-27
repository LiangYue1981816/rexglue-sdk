#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rex::graphics::native_surfaces {

struct SurfaceKind {
  bool is_depth;
  uint32_t format;
  uint32_t msaa_samples_log2;
};

// k_16_16 as a colour render target resource format.
constexpr uint32_t kColorFormat16_16 = 4;

// Which EDRAM ownership copies survive in the native surfaces mode: depth to depth (the game reads
// depth back across the two depth formats at the same EDRAM base), and depth to and from k_16_16
// colour: the game reads depth through a k_16_16 view of the same EDRAM (the 21:9 light shafts
// read a depth surface it first wrote as k_16_16, and without depth to k_16_16 the motion blur
// smears objects that move with the camera, issue #26).
inline bool KeepTransfer(const SurfaceKind& source, const SurfaceKind& dest) {
  if (source.is_depth && dest.is_depth) {
    return true;
  }
  if (!source.is_depth && source.format == kColorFormat16_16 && dest.is_depth) {
    return true;
  }
  if (source.is_depth && !dest.is_depth && dest.format == kColorFormat16_16) {
    return true;
  }
  return false;
}

struct TransferSide {
  bool any_kind;
  bool is_depth;
  bool any_format;
  uint32_t format;
};

struct TransferRule {
  TransferSide source;
  TransferSide dest;
};

struct TransferPolicy {
  bool use_default = true;
  std::vector<TransferRule> rules;
};

inline bool TransferSideMatches(const TransferSide& side, const SurfaceKind& kind) {
  if (side.any_kind) {
    return true;
  }
  return side.is_depth == kind.is_depth &&
         (side.any_format || side.format == kind.format);
}

inline bool KeepTransferWithPolicy(const TransferPolicy& policy, const SurfaceKind& source,
                                   const SurfaceKind& dest) {
  if (policy.use_default && KeepTransfer(source, dest)) {
    return true;
  }
  for (const TransferRule& rule : policy.rules) {
    if (TransferSideMatches(rule.source, source) && TransferSideMatches(rule.dest, dest)) {
      return true;
    }
  }
  return false;
}

inline bool IsTransferSeparator(char c) {
  return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

inline bool ParseTransferSide(std::string_view text, TransferSide& side_out) {
  if (text == "*") {
    side_out = TransferSide{true, false, true, 0};
    return true;
  }
  if (text.empty()) {
    return false;
  }
  bool is_depth;
  char head = text[0];
  if (head == 'C' || head == 'c') {
    is_depth = false;
  } else if (head == 'D' || head == 'd') {
    is_depth = true;
  } else {
    return false;
  }
  if (text.size() == 1) {
    side_out = TransferSide{false, is_depth, true, 0};
    return true;
  }
  uint32_t format = 0;
  for (size_t i = 1; i < text.size(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') {
      return false;
    }
    format = format * 10 + uint32_t(c - '0');
    if (format > 63) {
      return false;
    }
  }
  side_out = TransferSide{false, is_depth, false, format};
  return true;
}

inline bool ParseTransferToken(std::string_view token, TransferRule& rule_out) {
  size_t sep = token.find('>');
  if (sep == std::string_view::npos) {
    return false;
  }
  if (token.find('>', sep + 1) != std::string_view::npos) {
    return false;
  }
  TransferSide source{false, false, true, 0};
  TransferSide dest{false, false, true, 0};
  if (!ParseTransferSide(token.substr(0, sep), source)) {
    return false;
  }
  if (!ParseTransferSide(token.substr(sep + 1), dest)) {
    return false;
  }
  rule_out = TransferRule{source, dest};
  return true;
}

inline TransferPolicy ParseTransferPolicy(std::string_view text) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsTransferSeparator(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    size_t start = i;
    while (i < text.size() && !IsTransferSeparator(text[i])) {
      ++i;
    }
    tokens.push_back(text.substr(start, i - start));
  }
  if (tokens.empty()) {
    return TransferPolicy{};
  }
  if (tokens.size() == 1) {
    if (tokens[0] == "default") {
      return TransferPolicy{};
    }
    if (tokens[0] == "all") {
      TransferPolicy policy;
      policy.use_default = false;
      TransferSide any{true, false, true, 0};
      policy.rules.push_back(TransferRule{any, any});
      return policy;
    }
    if (tokens[0] == "none") {
      TransferPolicy policy;
      policy.use_default = false;
      return policy;
    }
  }
  TransferPolicy policy;
  policy.use_default = false;
  for (std::string_view token : tokens) {
    if (token == "default") {
      policy.use_default = true;
      continue;
    }
    TransferRule rule{{false, false, true, 0}, {false, false, true, 0}};
    if (!ParseTransferToken(token, rule)) {
      return TransferPolicy{};
    }
    policy.rules.push_back(rule);
  }
  return policy;
}

class DropCounters {
 public:
  void Record(const SurfaceKind& source, const SurfaceKind& dest, uint32_t tiles) {
    Totals& totals = counters_[Key(source, dest)];
    ++totals.count;
    totals.tiles += tiles;
  }

  std::string ReportAndReset(double seconds) {
    std::vector<std::pair<uint64_t, std::string>> entries;
    for (const auto& [key, totals] : counters_) {
      entries.emplace_back(totals.tiles, FormatEntry(key, totals, seconds));
    }
    counters_.clear();
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::string report;
    for (const auto& entry : entries) {
      if (!report.empty()) {
        report += "; ";
      }
      report += entry.second;
    }
    return report;
  }

  bool empty() const { return counters_.empty(); }

 private:
  using KindKey = std::tuple<bool, uint32_t, uint32_t>;
  using PairKey = std::pair<KindKey, KindKey>;

  struct Totals {
    uint64_t count = 0;
    uint64_t tiles = 0;
  };

  static KindKey ToKey(const SurfaceKind& kind) {
    return {kind.is_depth, kind.format, kind.msaa_samples_log2};
  }

  static PairKey Key(const SurfaceKind& source, const SurfaceKind& dest) {
    return {ToKey(source), ToKey(dest)};
  }

  static std::string FormatKind(const KindKey& kind) {
    const auto& [is_depth, format, msaa_samples_log2] = kind;
    return std::string(is_depth ? "D" : "C") + std::to_string(format) + "x" +
           std::to_string(1u << msaa_samples_log2);
  }

  static std::string FormatEntry(const PairKey& key, const Totals& totals, double seconds) {
    char rates[64];
    std::snprintf(rates, sizeof(rates), " %.1f/s %.0f tiles/s", totals.count / seconds,
                  totals.tiles / seconds);
    return FormatKind(key.first) + "->" + FormatKind(key.second) + rates;
  }

  std::map<PairKey, Totals> counters_;
};

}  // namespace rex::graphics::native_surfaces
