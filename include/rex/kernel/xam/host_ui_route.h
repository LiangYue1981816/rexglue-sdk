#pragma once

#include <cstdint>

namespace rex::kernel::xam {

inline bool RouteDeviceSelectorToHostUi(bool command_found, uint32_t content_flags,
                                      uint32_t mask) {
  return command_found && (mask == 0 || (content_flags & mask) != 0);
}

}  // namespace rex::kernel::xam
