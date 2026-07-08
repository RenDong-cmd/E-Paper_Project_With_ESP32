#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <GxEPD2_4G_4G.h>
#ifdef GxEPD_YELLOW
  #undef GxEPD_YELLOW
#endif
#include <GxEPD2_BW.h>


// 字体 API 开关
enum class FontAPI : uint8_t { U8G2, GFX };

// 向外暴露的中文字体指针（由 main.cpp 定义）
extern const uint8_t* FONT_U8G2;

// 屏幕尺寸（由 main.cpp 返回）
int  EPD_ScreenW();
int  EPD_ScreenH();

// 宽度测量（供状态栏/分页等使用）
int  cachedGlyphWidth(const char* utf8_cp);
int  getUTF8StringWidth_cached(const String& s);
void BindMeasureFont();

// === 新增：让任何 App 在“全刷后”立刻重绘顶栏 ===
void UI_RedrawStatusBar(bool clear_bg = true);

// 新增：屏幕休眠 / 唤醒
void EPD_Sleep();
void EPD_Wake();

// ===== 通用渲染函数（仅声明，实现见 main.cpp） =====
void EPD_FullBW_Text(const String& text,
                     int16_t x, int16_t y,
                     bool first_init = false,
                     int16_t line_gap = 34,
                     FontAPI api = FontAPI::U8G2,
                     const uint8_t* u8g2FontPtr = nullptr,
                     const GFXfont* gfxFont = nullptr,
                     uint8_t rotation = 1,
                     uint16_t fg = GxEPD_BLACK,
                     uint16_t bg = GxEPD_WHITE,
                     bool clear_before = true,
                     bool power_save = true,
                     bool hibernate = false);

void EPD_PartialBW_Text(int16_t x, int16_t y, int16_t w, int16_t h,
                        const String& text,
                        FontAPI api = FontAPI::U8G2,
                        const uint8_t* u8g2FontPtr = nullptr,
                        const GFXfont* gfxFont = nullptr,
                        uint8_t rotation = 1,
                        uint16_t fg = GxEPD_BLACK,
                        uint16_t bg = GxEPD_WHITE,
                        int16_t text_x_offset = 6,
                        int16_t text_y_offset = 24,
                        bool clear_rect = true,
                        bool power_save = true,
                        bool hibernate = false);

void EPD_Full4G_Image(const uint8_t* img,
                      int16_t iw, int16_t ih,
                      uint8_t rotation = 0,
                      bool invert = true,
                      bool mirror = false,
                      bool inProgmem = true,
                      bool clear_before = false,
                      bool power_save = true);

void EPD_FullBW_MultiText(int16_t x, int16_t y,
                          const String* lines, size_t count,
                          int16_t line_gap = 26,
                          FontAPI api = FontAPI::U8G2,
                          const uint8_t* u8g2FontPtr = nullptr,
                          const GFXfont* gfxFont = nullptr,
                          uint8_t rotation = 1,
                          uint16_t fg = GxEPD_BLACK,
                          uint16_t bg = GxEPD_WHITE,
                          int16_t text_x_offset = 6,
                          int16_t first_baseline_offset = 22,
                          bool clear_before = true,
                          bool power_save = true,
                          bool hibernate = false,
                          bool first_init = false);

void EPD_PartialBW_MultiText(int16_t x, int16_t y,
                             const String* lines, size_t count,
                             int16_t line_gap = 26,
                             FontAPI api = FontAPI::U8G2,
                             const uint8_t* u8g2FontPtr = nullptr,
                             const GFXfont* gfxFont = nullptr,
                             uint8_t rotation = 1,
                             uint16_t fg = GxEPD_BLACK,
                             uint16_t bg = GxEPD_WHITE,
                             int16_t text_x_offset = 6,
                             int16_t first_baseline_offset = 22,
                             bool clear_before = true,
                             bool power_save = true,
                             bool hibernate = false);
