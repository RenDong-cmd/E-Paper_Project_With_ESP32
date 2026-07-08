#include "status_bar.h"
#include "ui_compositor.h"
#include "../common/epd_exports.h"

namespace StatusBar {

static String fmtTime(uint32_t epoch){
  if (!epoch) return String("--:--");
  uint32_t t = epoch % 86400; uint32_t hh = t/3600; uint32_t mm = (t/60)%60;
  char buf[8]; snprintf(buf,sizeof(buf),"%02u:%02u", (unsigned)hh,(unsigned)mm); return String(buf);
}

void render(const SystemStateSnapshot& s, bool clear_bg){
  String left  = String("SD") + (s.sd_ok?"v ":"x ") + "NET" + (s.net_ok?"v ":"x ");
  String right = fmtTime(s.epoch);

  // 左半区
  EPD_PartialBW_Text(/*x=*/0, /*y=*/0, /*w=*/SCREEN_W/2, /*h=*/STATUSBAR_H,
    left, FontAPI::U8G2, FONT_U8G2, nullptr,
    1, GxEPD_BLACK, GxEPD_WHITE,
    /*xoff=*/6, /*yoff=*/22,
    /*clear=*/clear_bg, /*power_save=*/false, /*hiber=*/false);

  // 右半区（精确右对齐）
  int textW = getUTF8StringWidth_cached(right);
  int xoff  = (SCREEN_W/2) - textW - 6; // 右侧内边距 6px
  if (xoff < 0) xoff = 0;
  EPD_PartialBW_Text(/*x=*/SCREEN_W/2, /*y=*/0, /*w=*/SCREEN_W/2, /*h=*/STATUSBAR_H,
    right, FontAPI::U8G2, FONT_U8G2, nullptr,
    1, GxEPD_BLACK, GxEPD_WHITE,
    /*xoff=*/xoff, /*yoff=*/22,
    /*clear=*/false, /*power_save=*/false, /*hiber=*/false);
}

} // namespace StatusBar