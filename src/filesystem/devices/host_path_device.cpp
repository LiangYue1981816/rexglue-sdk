/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

namespace {

// Lower-cases and turns '\' into '/' so guest paths compare the same no
// matter how the host or the environment spelled them.
std::string NormalizeOverrideKey(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '\\') {
      out.push_back('/');
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

// GOW2_VFS_OVERRIDES_FILE names a UTF-8 file of "guest=host" lines, for lists
// too long for one environment variable.
void AddOverridesFromFile(std::unordered_map<std::string, std::string>& table) {
#ifdef _WIN32
  // Wide read so a cache folder under a non-ASCII user name survives.
  const wchar_t* raw = _wgetenv(L"GOW2_VFS_OVERRIDES_FILE");
#else
  const char* raw = std::getenv("GOW2_VFS_OVERRIDES_FILE");
#endif
  if (!raw || !*raw) {
    return;
  }
  std::filesystem::path path(raw);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_WARN("vfs overrides file: cannot open {}", rex::path_to_utf8(path));
    return;
  }
  size_t count = 0;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = NormalizeOverrideKey(std::string_view(line).substr(0, eq));
    std::string value = line.substr(eq + 1);
    if (key.empty() || value.empty()) {
      continue;
    }
    table[key] = value;
    ++count;
  }
  REXLOG_INFO("vfs overrides file: {} entries from {}", count, rex::path_to_utf8(path));
}

// GOW2_VFS_OVERRIDES="guest/relative/path=C:\host\file;other/path=D:\x\y".
// Parsed once; later edits of the variable do not re-read it.
const std::unordered_map<std::string, std::string>& GetVfsOverrides() {
  static const std::unordered_map<std::string, std::string> overrides = [] {
    std::unordered_map<std::string, std::string> table;
    // File first so the variable wins on a duplicate key.
    AddOverridesFromFile(table);
    const char* raw = std::getenv("GOW2_VFS_OVERRIDES");
    if (!raw || !*raw) {
      return table;
    }
    std::string_view rest(raw);
    while (!rest.empty()) {
      size_t semi = rest.find(';');
      std::string_view item = rest.substr(0, semi);
      rest = (semi == std::string_view::npos) ? std::string_view() : rest.substr(semi + 1);
      if (item.empty()) {
        continue;
      }
      size_t eq = item.find('=');
      if (eq == std::string_view::npos) {
        continue;
      }
      std::string key = NormalizeOverrideKey(item.substr(0, eq));
      std::string value(item.substr(eq + 1));
      if (key.empty() || value.empty()) {
        continue;
      }
      table[key] = value;
    }
    return table;
  }();
  return overrides;
}

}  // namespace

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               bool allow_share_delete)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      allow_share_delete_(allow_share_delete) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      // Stat the exact name first: enumerating a directory of hundreds of
      // archives costs milliseconds a walk. Only casing mismatch reaches
      // the scan below.
      const auto exact_path = current_entry->host_path() / rex::to_path(part);
      rex::filesystem::FileInfo exact_info;
      if (rex::filesystem::GetInfo(exact_path, &exact_info)) {
        auto* exact_child = HostPathEntry::Create(this, current_entry, exact_path, exact_info);
        if (!exact_child) {
          return nullptr;
        }
        current_entry->children_.push_back(std::unique_ptr<Entry>(exact_child));
        current_entry = static_cast<HostPathEntry*>(exact_child);
        continue;
      }

      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  const auto& overrides = GetVfsOverrides();
  const bool allow_overrides = read_only_ && !overrides.empty();
  for (auto& child_info : child_infos) {
    std::filesystem::path child_host_path = parent_entry->host_path() / child_info.name;
    rex::filesystem::FileInfo effective_info = child_info;
    if (allow_overrides && child_info.type != rex::filesystem::FileInfo::Type::kDirectory) {
      std::error_code ec;
      std::filesystem::path relative =
          std::filesystem::relative(child_host_path, host_path_, ec);
      if (!ec) {
        auto it = overrides.find(NormalizeOverrideKey(rex::path_to_utf8(relative)));
        if (it != overrides.end()) {
          std::filesystem::path override_host_path = rex::to_path(it->second);
          rex::filesystem::FileInfo override_info;
          if (rex::filesystem::GetInfo(override_host_path, &override_info)) {
            // Keep the original child name so the game still sees its file.
            override_info.name = child_info.name;
            effective_info = override_info;
            child_host_path = override_host_path;
            REXLOG_INFO("vfs override: {} -> {}", rex::path_to_utf8(relative), it->second);
          }
        }
      }
    }
    auto child = HostPathEntry::Create(this, parent_entry, child_host_path, effective_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

}  // namespace rex::filesystem
