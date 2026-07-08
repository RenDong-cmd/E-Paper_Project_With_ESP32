#pragma once
#include "../common/system_state.h"

// 屏幕与布局（竖屏）
static constexpr int SCREEN_W     = 480;
static constexpr int SCREEN_H     = 800;
static constexpr int STATUSBAR_H  = 32;
static constexpr int CONTENT_Y    = STATUSBAR_H;
static constexpr int CONTENT_H    = SCREEN_H - STATUSBAR_H;

namespace UiCompositor {
  void firstFullPaint(const SystemStateSnapshot& s);
  void repaintStatusBar(const SystemStateSnapshot& s);
}