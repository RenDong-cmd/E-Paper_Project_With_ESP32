#include "ui_compositor.h"
#include "status_bar.h"
#include "../common/epd_exports.h"

void UiCompositor::firstFullPaint(const SystemStateSnapshot& s){
  // 首帧全刷（不关电）
  EPD_FullBW_Text("系统启动中…", 8, 40,
                  /*first_init=*/true, 26,
                  FontAPI::U8G2, FONT_U8G2, nullptr,
                  1, GxEPD_BLACK, GxEPD_WHITE,
                  true, false, false);
  // 立刻绘制一次顶栏
  StatusBar::render(s, /*clear_bg=*/true);
}

void UiCompositor::repaintStatusBar(const SystemStateSnapshot& s){
  StatusBar::render(s, /*clear_bg=*/true);
}