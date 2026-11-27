// Local GoW2 menu-only Escape routing; gameplay keeps the native pause binding.
#pragma once
namespace rex::input::gow2_menu_escape {
inline unsigned short Route(unsigned short buttons, bool enabled, bool native_keys,
                            bool emulated_keys, bool escape_down, unsigned short start,
                            unsigned short back) {
  if (enabled && native_keys && emulated_keys && escape_down)
    return static_cast<unsigned short>((buttons & ~start) | back);
  return buttons;
}
}  // namespace rex::input::gow2_menu_escape
