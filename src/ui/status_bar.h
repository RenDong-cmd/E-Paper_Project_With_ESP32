#pragma once
#include "../common/system_state.h"

namespace StatusBar {
  void render(const SystemStateSnapshot& s, bool clear_bg = true);
}