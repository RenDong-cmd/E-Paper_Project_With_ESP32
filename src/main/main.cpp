// ============================================================
// 合并版：ESP32-S3 AI语音助手 + 气象墨水屏工作站
// FreeRTOS 双核架构:
//   Core 1 (任务一): AI 语音对话、WebSocket、USB键盘注入
//   Core 0 (任务二): 墨水屏UI切换、天气数据刷新、按键检测
//
// 五个界面:
//   0 - 实时天气 + 空气质量 (来自第二套代码，真实API数据)
//   1 - 未来7天天气预报   (来自第二套代码，真实API数据)
//   2 - 未来空气质量预报  (来自第二套代码，真实API数据)
//   3 - AI代办事项列表   (来自第一套代码，AI生成)
//   4 - AI知识伴读卡片   (来自第一套代码，AI生成)
//
// 墨水屏驱动以第一套代码为主：GxEPD2_4G_4G（四灰度）
// ============================================================

// ================= 库资源引入 =================
#include <Arduino.h>
#include "base64.h"
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "HTTPClient.h"
#include <time.h>
#include <unistd.h>
#include "Audio1.h"
#include "Audio2.h"
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ArduinoZlib.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WebServer.h>

#include <Wire.h>
#include <RevEng_PAJ7620.h>

// 实例化手势传感器对象
RevEng_PAJ7620 gestureSensor = RevEng_PAJ7620();


// ================= 字库引入 =================
#include "fonts/u8g2_simsun_24_gb2312/u8g2_simsun_24_gb2312.h"
#include "fonts/u8g2_simsun_16_gb2312/u8g2_simsun_16_gb2312.h"
#include "fonts/u8g2_simsun_12_gb2312/u8g2_simsun_12_gb2312.h"


// ================= 图标库引入 (来自第二套代码) =================
#include "icons/icons_64x64.h"
#include "icons/icons_196x196.h"
#include "icons/icons_32x32.h"
#include "icons/icons_16x16.h"
#include "icons/32x32/qweather_icons_32x32.h"
#include "icons/196x196/qweather_icons_196x196.h"
#include "icons/64x64/qweather_icons_64x64.h"
#include "icons/16x16/qweather_icons_16x16.h"

#include <LittleFS.h>

// #include <SD.h>
#include <vector>
#include "esp_heap_caps.h"
// #include <u8g2_FangZhengkaiti_18_gb2312.h>   // 电子书正文字体
#include "u8g2_FangZhengkaiti_18_gb2312.h"

// 私密配置文件 (包含 QWEATHER_KEY, QWEATHER_HOST, WIFI_SSID, WIFI_PASS)
#include "config_private.h"

using namespace websockets;

#define I2C_SCL_PIN 42
#define I2C_SDA_PIN 41

// ================= 宏定义 =================
#define USE_MULTCORE 1  // 1=双核模式, 0=单核模式

// ================= 引脚定义 (两套代码引脚完全一致，无冲突) =================
// --- 语音与指示引脚 (来自第一套代码) ---
#define key           0    // 录音唤醒按键（接GND触发）
#define led3          38   // 发声指示灯（避开墨水屏引脚）

// --- 墨水屏控制引脚 (两套代码完全一致) ---
#define PIN_EPD_BUSY  6    // 忙碌状态引脚
#define PIN_EPD_RST   5    // 复位引脚
#define PIN_EPD_DC    4    // 数据/命令选择引脚
#define PIN_EPD_CS    10   // 片选线 (S3默认 SS)
#define PIN_EPD_SCK   12   // 时钟线 (S3默认 SCK)
#define PIN_EPD_SDI   11   // 数据线 (S3默认 MOSI)

// --- 界面切换按键引脚 (两套代码完全一致) ---
#define BUTTON_PIN    14   // 外接按键引脚，杜邦线短接此引脚与GND模拟按键
#define PIN_BUTTON    14   // 同上，别名保留兼容性

// --- 电池检测引脚 (来自第二套代码) ---
#define PIN_ADC_VOLTAGE 1  // 电池电压检测引脚 (ESP32-S3 的 ADC1_0)
#define PIN_IMAGE_BUTTON  13   // 画廊按键

// #define PIN_SD_CS   38    // D3
// #define PIN_SD_MISO 37    // D0
// #define PIN_SD_MOSI 36    //CMD
// #define PIN_SD_SCK  35    //CK

#include <GxEPD2_BW.h>

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT>* display = nullptr;

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

constexpr int SCREEN_W  = 480;   // 竖屏宽度
constexpr int SCREEN_H  = 800;   // 竖屏高度
constexpr int CONTENT_Y = 30;    // 内容起始Y
constexpr int CONTENT_H = SCREEN_H - CONTENT_Y;

// ================= WiFi 配置 (来自第一套代码，支持多WiFi切换) =================
const char *wifiData[][2] = {
    {"haha", "123321111"}, // 替换为自己常用的WiFi名和密码
};


// ================= 讯飞星火大模型配置 (来自第一套代码) =================
String APPID     = config_private::Config::APPID;
String APIKey    = config_private::Config::APIkey;
String APISecret = config_private::Config::APISecret;
const char *appId1  = config_private::Config::appId1;
const char *domain1 = config_private::Config::domain1;
const char *websockets_server  = config_private::Config::websockets_server;
const char *websockets_server1 = config_private::Config::websockets_server1;


// ================= 音频引脚配置 (来自第一套代码) =================
#define I2S_DOUT 21
#define I2S_BCLK 17
#define I2S_LRC  18

// ================= 全局状态变量 - AI相关 (来自第一套代码) =================
bool startPlay     = false;
bool lastsetence   = false;
bool isReady       = false;
unsigned long urlTime  = 0;
unsigned long pushTime = 0;
int mainStatus     = 0;    // 0=普通, 1=点歌, 2=代办, 3=知识卡片, 9=代码注入
int receiveFrame   = 0;
int noise          = 50;
int loopcount      = 0;

hw_timer_t *timer          = NULL;
uint8_t adc_start_flag     = 0;
uint8_t adc_complete_flag  = 0;

HTTPClient https;
Audio1 audio1;
Audio2 audio2(false, 3, I2S_NUM_1);

// ================= 全局状态变量 - UI相关 =================
// 屏幕管理
int currentScreen       = 0;        // 当前界面索引
const int TOTAL_SCREENS = 5;        // 总界面数量
bool needsUpdate        = true;     // 屏幕刷新标志（任何任务均可设置）
unsigned long lastButtonPress = 0;  // 按键去抖时间戳


// ── 电子书相关 ─────────────────────────────────────
bool isEbookMode              = false;  // 是否处于电子书模式
volatile bool needsEbookStart = false;  // 语音触发：需要启动电子书
volatile bool needsEbookStop = false;
// SPIClass sdSPI(FSPI);                   // SD 卡专用 SPI 总线


// AI生成内容 (来自第一套代码)
String todoListText  = "";
String flashcardText = "等待提问...";

// ── 音乐播放状态（跨核共享）────────────────────
volatile bool isMusicPlaying  = false;   // 是否正在播放音乐
String        currentSongName = "";      // 当前播放的歌名

// 全局刷新保护计数 (来自第二套代码)
RTC_DATA_ATTR int fullRefreshCount = 0;
volatile bool webNeedsAIConnect = false;

// 时间局刷变量 (来自第二套代码)
String lastDisplayedTime   = "--:--";
unsigned long lastTimeCheck = 0;

// WebSocket & AI 字符串 (来自第一套代码)
String url  = "";
String url1 = "";
String Date = "";
String askquestion = "";
String Answer = "";

DynamicJsonDocument* globalDoc;
JsonArray text;

WebsocketsClient webSocketClient;
WebsocketsClient webSocketClient1;
WebServer server(80);

// --- 画廊相关变量 ---
bool isImageMode               = false;
int  currentImageIndex         = 0;
const int TOTAL_IMAGES         = 3;
unsigned long lastImageButtonPress = 0;

// ── Web服务器相关 ──────────────────────────
String webPushTodo     = "";   // 手机推送的代办内容
String webPushQuestion = "";   // 手机推送的知识卡片问题
volatile bool webHasTodo     = false;  // 有新代办待处理
volatile bool webHasQuestion = false;  // 有新问题待处理

// ================= 天气数据结构体 (来自第二套代码) =================
const String LATITUDE  = "36.80";   // 纬度
const String LONGITUDE = "118.01";  // 经度
const int MAX_FUTURE_DAYS = 7;      // 最大预报天数

#include "pic_1.h"
#include "pic_3.h"
#include "pic_4.h"



// 全局JSON缓冲池
JsonDocument weatherDoc;

// 实时天气结构体
struct RealtimeWeather {
    String obsTime, temp, feelsLike, icon, text;
    String wind360, windDir, windScale, windSpeed;
    String humidity, precip, pressure, vis, cloud, dew;
};

// 未来天气结构体
struct FutureWeather {
    String fxdate, sunrise, sunset, moonrise, moonset, moonPhase, moonPhaseIcon;
    String tempMax, tempMin, iconDay, textDay, iconNight, textNight;
    String wind360Day, windDirDay, windScaleDay, windSpeedDay;
    String wind360Night, windDirNight, windScaleNight, windSpeedNight;
    String precip, uvIndex, humidity, pressure, vis, cloud;
};

// 实时空气质量结构体
struct AirQualityNow {
    String aqi, category, level, primary;
    String pm2p5, pm10, no2, co, so2, o3;
    String healthEffect, adviceGeneral, adviceSensitive;
};

// 未来空气质量预报结构体
struct AirQualityForecast {
    String fxdate, aqi, category, level, primary;
    String healthEffect, adviceGeneral, adviceSensitive;
};

// 全局数据容器实例化
RealtimeWeather todayWeather;
FutureWeather   forecastWeather[MAX_FUTURE_DAYS];
AirQualityNow   currentAir;
AirQualityForecast airForecasts[3];


// ================= 函数前置声明 =================
void getText(String role, String content);
void checkLen(JsonArray textArray);
int  getLength(JsonArray textArray);
float calculateRMS(uint8_t *buffer, int bufferSize);
void ConnServer();
void ConnServer1();
void AI();
void playNetEaseMusic(String songName);
void drawScreen1();
void drawScreen2();
void drawScreen3();

void drawStatusBar();      // ← 新增
void drawMusicScreen();    // ← 新增

void drawTodoList();
void drawFlashcard();
void drawImageScreen();
String getCurrentTime();
void writeFont(int16_t cursorX, int16_t cursorY, String str, const uint8_t font[]);
void drawDynamicWiFiIcon(int x, int y, int rssi);
void drawDynamicBatteryIcon(int x, int y, int batPercent);
void drawDynamicWeatherIcon(int x, int y, String iconCodeStr, int size);
void drawDynamicAQIIcon(int x, int y, String levelStr, int size);
String getWeekDay(String dateStr);
void updateScreenFull(bool forceReset = false);
void handleRoot();
void fetchAllData();
DynamicJsonDocument gen_params(const char *appid, const char *domain);
const uint8_t* getExactWindIcon(String wind360Str);


// ====== 音乐播放界面 ======
void drawMusicScreen() {
    drawStatusBar();

    // ── 外框 ──
    display->drawRoundRect(15, 48, 770, 422, 12, GxEPD_BLACK);
    display->drawRoundRect(17, 50, 766, 418, 10, GxEPD_BLACK);

    // ── 大音符图标区域（手绘像素音符）──
    // 外圆圈装饰
    display->drawCircle(400, 220, 120, GxEPD_BLACK);
    display->drawCircle(400, 220, 118, GxEPD_BLACK);

    // 手绘简化音符：♪  用矩形+圆近似
    // 音符竖线
    display->fillRect(380, 155, 6, 80, GxEPD_BLACK);
    display->fillRect(410, 140, 6, 80, GxEPD_BLACK);
    // 音符横梁
    display->fillRect(380, 155, 36, 8, GxEPD_BLACK);
    // 音符圆头
    display->fillCircle(374, 238, 14, GxEPD_BLACK);
    display->fillCircle(404, 222, 14, GxEPD_BLACK);
    // 内部白色（镂空效果）
    display->fillCircle(374, 238, 8, GxEPD_WHITE);
    display->fillCircle(404, 222, 8, GxEPD_WHITE);

    // ── 顶部标题 ──
    display->fillRect(28, 57, 6, 30, GxEPD_BLACK);   // 装饰竖条
    writeFont(44, 58, "正在播放", u8g2_simsun_24_gb2312);

    // ── 分割线 ──
    display->drawLine(15,  98, 785,  98, GxEPD_BLACK);
    display->drawLine(15, 101, 785, 101, GxEPD_BLACK);

    // ── 歌名（自动换行）──
    String displayName = currentSongName.length() > 0 ? currentSongName : "未知曲目";
    // 超长截断
    if (displayName.length() > 20) displayName = displayName.substring(0, 20) + "...";
    writeFont(44, 360, "♪  " + displayName, u8g2_simsun_24_gb2312);

    // ── 滚动动画提示文字 ──
    writeFont(44, 410, "按录音键可打断播放", u8g2_simsun_16_gb2312);

    // ── 右下角装饰 ──
    writeFont(600, 445, "网易云音乐 · 流媒体", u8g2_simsun_12_gb2312);
}

// ============================================================
// =================== 辅助算法函数 ==========================
// ============================================================

// UTF-8 自动排版换行算法 (来自第一套代码)
String autoWrapUTF8(String text, int maxCharsPerLine) {
    String result = "";
    int charCount = 0;
    for (int i = 0; i < text.length(); ) {
        // 吃掉所有原始换行符，由算法统一控制行距
        if (text[i] == '\n' || text[i] == '\r') { i++; continue; }
        if (i + 1 < text.length() && text[i] == '\\' && text[i+1] == 'n') { i += 2; continue; }
        // 精准识别UTF-8字符占用字节数
        uint8_t c = text[i];
        int charBytes = 1;
        if      ((c & 0x80) == 0x00) charBytes = 1; // 英文/数字
        else if ((c & 0xE0) == 0xC0) charBytes = 2; // 拉丁文
        else if ((c & 0xF0) == 0xE0) charBytes = 3; // 常见汉字
        else if ((c & 0xF8) == 0xF0) charBytes = 4; // 特殊符号
        result += text.substring(i, i + charBytes);
        charCount++;
        i += charBytes;
        if (charCount >= maxCharsPerLine) { result += '\n'; charCount = 0; }
    }
    return result;
}

// 日期字符串转星期 (来自第二套代码)
String getWeekDay(String dateStr) {
    int y, m, d;
    sscanf(dateStr.c_str(), "%d-%d-%d", &y, &m, &d);
    struct tm timeinfo = {0};
    timeinfo.tm_year = y - 1900;
    timeinfo.tm_mon  = m - 1;
    timeinfo.tm_mday = d;
    mktime(&timeinfo);
    const char* wdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6)
        return String(wdays[timeinfo.tm_wday]);
    return "未知";
}

// 获取NTP格式化时间 (来自第二套代码)
String getCurrentTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) return "--:--";
    char buffer[10];
    strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
    return String(buffer);
}

// URL编码 (来自第一套代码)
static String urlEncode(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); ++i) {
        uint8_t c = (uint8_t)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            sprintf(buf, "%%%.2X", c);
            out += buf;
        }
    }
    return out;
}

// RMS音量计算 (来自第一套代码)
float calculateRMS(uint8_t *buffer, int bufferSize) {
    float sum = 0;
    int16_t sample;
    for (int i = 0; i < bufferSize; i += 2) {
        sample = (buffer[i + 1] << 8) | buffer[i];
        sum += sample * sample;
    }
    sum /= (bufferSize / 2);
    return sqrt(sum);
}


// ============================================================
// =================== 墨水屏核心渲染 =========================
// ============================================================

// 通用字体绘制函数（含setFont，以第二套代码为准）
void writeFont(int16_t cursorX, int16_t cursorY, String str, const uint8_t font[]) {
    u8g2Fonts.setFont(font);
    int16_t ta = u8g2Fonts.getFontAscent();
    u8g2Fonts.setCursor(cursorX, cursorY + ta);
    u8g2Fonts.print(str);
}

// 带自动换行的文本绘制 (来自第二套代码)
int16_t drawTextWrap(int16_t x, int16_t y, String text, const uint8_t font[], int16_t maxWidth, int16_t lineHeight) {
    u8g2Fonts.setFont(font);
    String currentLine = "";
    int16_t currentY = y;
    for (int i = 0; i < text.length(); ) {
        int charLen = 1;
        uint8_t c = text[i];
        if      ((c & 0xE0) == 0xE0) charLen = 3;
        else if ((c & 0xC0) == 0xC0) charLen = 2;
        String nextChar  = text.substring(i, i + charLen);
        String testLine  = currentLine + nextChar;
        if (u8g2Fonts.getUTF8Width(testLine.c_str()) > maxWidth) {
            u8g2Fonts.setCursor(x, currentY);
            u8g2Fonts.print(currentLine);
            currentLine = nextChar;
            currentY += lineHeight;
        } else {
            currentLine = testLine;
        }
        i += charLen;
    }
    if (currentLine.length() > 0) {
        u8g2Fonts.setCursor(x, currentY);
        u8g2Fonts.print(currentLine);
    }
    return currentY;
}

// 墨水屏初始化 (四灰度，以第一套代码为主)
void initDisplay() {
    display->init(115200, true, 10, false);
    display->setRotation(0);
    u8g2Fonts.begin(*display); 
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
}



// ============================================================
// ============= 电子书模块 EPD 辅助函数（适配指针版display）==
// ============================================================

// 电子书专用字体指针（正文用）
static const uint8_t* EBOOK_FONT = u8g2_FangZhengkaiti_18_gb2312;

// 供电子书调用：测量字符像素宽度
int cachedGlyphWidth(const char* s) {
    return u8g2Fonts.getUTF8Width(s);
}
int getUTF8StringWidth_cached(const String& s) {
    return u8g2Fonts.getUTF8Width(s.c_str());
}
void BindMeasureFont() {
    u8g2Fonts.setFont(EBOOK_FONT);
    u8g2Fonts.begin(*display);
}

// 电子书全刷：显示单行文本
void EPD_FullBW_Text(const String& text, int16_t x, int16_t y,
                     bool, int16_t, int api, const uint8_t* font, const void*,
                     uint8_t rotation, uint16_t fg, uint16_t bg,
                     bool clear, bool, bool) {
    u8g2Fonts.begin(*display);
    u8g2Fonts.setFont(font ? font : EBOOK_FONT);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);
    display->setRotation(rotation);
    display->setFullWindow();
    display->firstPage();
    do {
        if (clear) display->fillScreen(bg);
        u8g2Fonts.setCursor(x, y);
        u8g2Fonts.print(text);
    } while (display->nextPage());
}

// 电子书局刷：显示单行文本
void EPD_PartialBW_Text(int16_t x, int16_t y, int16_t w, int16_t h,
                        const String& text, int, const uint8_t* font, const void*,
                        uint8_t rotation, uint16_t fg, uint16_t bg,
                        int16_t tx, int16_t ty, bool clear, bool, bool) {
    u8g2Fonts.begin(*display);
    u8g2Fonts.setFont(font ? font : EBOOK_FONT);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);
    display->setPartialWindow(x, y, w, h);
    display->firstPage();
    do {
        if (clear) display->fillRect(x, y, w, h, bg);
        u8g2Fonts.setCursor(x + tx, y + ty);
        u8g2Fonts.print(text);
    } while (display->nextPage());
}

// 电子书全刷：显示多行文本
void EPD_FullBW_MultiText(int16_t x, int16_t y, const String* lines, size_t count,
                          int16_t gap, int, const uint8_t* font, const void*,
                          uint8_t rotation, uint16_t fg, uint16_t bg,
                          int16_t tx, int16_t baseline, bool clear, bool, bool, bool) {
    u8g2Fonts.begin(*display);
    u8g2Fonts.setFont(font ? font : EBOOK_FONT);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);
    display->setRotation(rotation);
    display->setFullWindow();
    display->firstPage();
    do {
        if (clear) display->fillScreen(bg);
        for (size_t i = 0; i < count; i++) {
            u8g2Fonts.setCursor(x + tx, y + baseline + i * gap);
            u8g2Fonts.print(lines[i]);
        }
    } while (display->nextPage());
}

// 电子书局刷：显示多行文本
void EPD_PartialBW_MultiText(
    int16_t x, int16_t y,
    const String* lines,
    size_t count,
    int16_t gap,
    int,
    const uint8_t* font,
    const void*,
    uint8_t rotation,
    uint16_t fg,
    uint16_t bg,
    int16_t tx,
    int16_t baseline,
    bool clear,
    bool,
    bool
) {
    display->setRotation(rotation);

    u8g2Fonts.begin(*display);
    u8g2Fonts.setFont(font ? font : EBOOK_FONT);
    u8g2Fonts.setForegroundColor(fg);
    u8g2Fonts.setBackgroundColor(bg);

    const int16_t screenW = display->width();
    const int16_t screenH = display->height();

    const int16_t safeX = max<int16_t>(0, x);
    const int16_t safeY = max<int16_t>(0, y);

    const int16_t safeW = screenW - safeX;

    const int32_t requestedH =
        (int32_t)count * gap + 20;

    const int16_t safeH = min<int32_t>(
        requestedH,
        screenH - safeY
    );

    if (safeW <= 0 || safeH <= 0) {
        Serial.println("[Ebook] 局刷区域无效，已跳过。");
        return;
    }

    display->setPartialWindow(
        safeX,
        safeY,
        safeW,
        safeH
    );

    display->firstPage();

    do {
        if (clear) {
            display->fillRect(
                safeX,
                safeY,
                safeW,
                safeH,
                bg
            );
        }

        for (size_t i = 0; i < count; i++) {
            u8g2Fonts.setCursor(
                safeX + tx,
                safeY + baseline + i * gap
            );
            u8g2Fonts.print(lines[i]);
        }

    } while (display->nextPage());
}

void UI_RedrawStatusBar(bool) { /* 占位，电子书不需要天气状态栏 */ }

// 4. 嵌入 EbookApp 模块
// ==========================================
namespace EbookApp 
{
static uint8_t* g_sdBuf = nullptr;
static size_t g_sdBufCap = 0;
static void ensureSDBuf(size_t cap = 64 * 1024) {
    if (g_sdBuf && g_sdBufCap >= cap) return;
    if (g_sdBuf) heap_caps_free(g_sdBuf);
    g_sdBuf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g_sdBufCap = cap;
}

// ==== forward declarations（为跨函数调用补齐可见性）====
static void renderMenu(bool fullRefresh = false);
static void renderCurrentPage(bool forceFull = false);
static void prefetchNextPage();
static void buildTOC();
static void renderReaderMenu(bool full = false);
static void renderTOC(bool full = false);
static void nextPage();
static void prevPage();
static bool openSelectedFile();
static void gotoOffset(uint32_t target);

static String   _tocPathFor(const String &filePath);
static uint64_t _fastFileFingerprint(File &f);
static bool     loadTOCFromCache(uint64_t fp, uint32_t fsize);
static bool     saveTOCToCache(uint64_t fp, uint32_t fsize);
static void     buildTOC_withCacheAndProgress();
static void     _showTOCProgress(uint32_t doneBytes, uint32_t totalBytes);

static String   _pgxPathFor(const String &filePath);
static uint32_t _layoutSignature(); 
static bool     loadPageOffsetsFromCache(uint64_t fp, uint32_t fsize, uint32_t layoutSig);
static bool     savePageOffsetsToCache(uint64_t fp, uint32_t fsize, uint32_t layoutSig);
static void     buildPageOffsets_withCacheAndProgress(); 
static int      findPageIndexForOffset(uint32_t target);  
static void     _showPGXProgress(uint32_t doneBytes, uint32_t totalBytes);

static String   _progressDir(); 
static String   _progressPathFor(const String &filePath);
static bool     _looksLikeChapter(const String &line);
static void     bindMeasureFont();
static uint32_t paginatePage(File &f, uint32_t startOffset, String *outLines, int &outCount);

// ===== 清理重复宏定义并使用 constexpr 保证编译期常数 =====
static constexpr int16_t MENU_START_Y    = CONTENT_Y + 200;
static constexpr int16_t MENU_MARGIN_X   = 8;
static constexpr int16_t MENU_LINE_H     = 26;
static constexpr int16_t MENU_BASE_OFF   = 22;
static constexpr int16_t MENU_TEXT_XOFF  = 6;

static constexpr int16_t READER_START_Y  = CONTENT_Y + 10;
static constexpr int16_t READER_MARGIN_X = 2;
static constexpr int16_t READER_LINE_H   = 26;
static constexpr int16_t READER_BASE_OFF = 22;
static constexpr int16_t READER_TEXT_XOFF= 0;
static constexpr int16_t READER_W        = SCREEN_W - READER_MARGIN_X * 2;
static constexpr int16_t READER_H        = CONTENT_H - 10;

static const uint8_t *FONT = EBOOK_FONT;

// 文件系统里使用英文文件名；屏幕显示使用这里的中文书名。
// 这样不依赖 LittleFS 对中文文件名的编码支持。
static String getBookDisplayName(const String& filePath) {
    if (filePath.endsWith("book_01.txt")) return "《凡人修仙传》";
    if (filePath.endsWith("book_02.txt")) return "《三体》";
    if (filePath.endsWith("book_03.txt")) return "《活着》";

    // 没有配置中文书名时，显示原本的英文文件名。
    int slashPos = filePath.lastIndexOf('/');
    String name = (slashPos >= 0)
        ? filePath.substring(slashPos + 1)
        : filePath;

    if (name.endsWith(".txt") || name.endsWith(".TXT")) {
        name.remove(name.length() - 4);
    }

    return name;
}

enum class Mode { MENU, READER, READER_MENU, TOC };
static Mode mode = Mode::MENU;

static std::vector<String> g_files;
static int g_cursor = 0;
static int g_menuTop = 0;

static File g_file;
static String g_currentFile;
static std::vector<uint32_t> g_pageOffsets;
static int g_pageIndex = 0;
static int g_pagesSinceFull = 0;

// 现在可以完美通过编译了
static constexpr int READER_MAX_LINES = READER_H / READER_LINE_H;
static String g_cachedNextLines[READER_MAX_LINES];
static int g_cachedNextCount = -1;

struct TocEntry { String title; uint32_t offset; };
static std::vector<TocEntry> g_toc;
static int g_tocCursor = 0;
static int g_tocTop = 0;

static const char *kReaderMenuItems[] = { "目录", "保存进度", "关闭" };
static int g_readerMenuCursor = 0;

static unsigned long g_lastInteractionMs = 0;
static bool g_poweredOff = false;  
static bool g_firstMenu  = true;   // ← 加这一行
static inline void touch() { g_lastInteractionMs = millis(); }

static uint64_t fnv1a64(const uint8_t* data, size_t len, uint64_t seed = 1469598103934665603ULL) {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t _fastFileFingerprint(File& f) {
    if (!f) return 0;
    const uint32_t sz = f.size();
    const size_t K = 4096;
    uint8_t buf[K];
    uint64_t h = 1469598103934665603ULL;

    f.seek(0);
    size_t n = f.read(buf, (sz < K) ? sz : K);
    h = fnv1a64(buf, n, h);

    if (sz > K) {
        uint32_t tailStart = sz - K;
        f.seek(tailStart);
        n = f.read(buf, K);
        h = fnv1a64(buf, n, h);
    }
    uint8_t szb[8];
    for (int i = 0; i < 8; ++i) szb[i] = (uint8_t)((sz >> (i * 8)) & 0xFF);
    h = fnv1a64(szb, 8, h);
    return h;
}

static String _progressDir() { return String("/.ebook"); }

static String _sanitizePathForCache(const String &filePath) {
    String name = filePath;
    if (name.length() && name[0] == '/') name.remove(0, 1);
    name.replace("/", "_");
    return name;
}

static String _progressPathFor(const String &filePath) {
    String base = _progressDir(); LittleFS.mkdir(base.c_str());
    return base + "/" + _sanitizePathForCache(filePath) + ".pg";
}
static String _tocPathFor(const String &filePath) {
    String base = _progressDir(); LittleFS.mkdir(base.c_str());
    return base + "/" + _sanitizePathForCache(filePath) + ".toc";
}
static String _pgxPathFor(const String &filePath) {
    String base = _progressDir(); LittleFS.mkdir(base.c_str());
    return base + "/" + _sanitizePathForCache(filePath) + ".pgx";
}

static int16_t g_ascii_w[128];
static bool    g_ascii_w_init = false;

static void initAsciiWidthLUT() {
    if (g_ascii_w_init) return;
    for (int i = 0; i < 128; ++i) {
        char s[2] = {(char)i, 0};
        g_ascii_w[i] = cachedGlyphWidth(s);
    }
    g_ascii_w_init = true;
}
static void bindMeasureFont() {
    BindMeasureFont();
    initAsciiWidthLUT();
}

static void _showTOCProgress(uint32_t doneBytes, uint32_t totalBytes) {
    int percent = (totalBytes > 0) ? (int)((doneBytes * 100ULL) / totalBytes) : 0;
    String msg = "构建目录中… " + String(percent) + "%";
    EPD_PartialBW_Text(8, CONTENT_Y + 70, 320, 24,
                       msg.c_str(), 0, FONT, nullptr,
                       1, GxEPD_BLACK, GxEPD_WHITE, 6, 22,
                       true, false, false);
}

static bool loadTOCFromCache(uint64_t fp, uint32_t fsize) {
    g_toc.clear();
    String path = _tocPathFor(g_currentFile);
    File f = LittleFS.open(path.c_str(), FILE_READ);
    if (!f) return false;

    String header;
    while (f.available()) { char c = f.read(); if (c == '\n') break; header += c; }
    if (!header.startsWith("EBOOK_TOC_V1")) { f.close(); return false; }

    int sPos = header.indexOf("size=");
    int fpPos = header.indexOf("fp=");
    if (sPos < 0 || fpPos < 0) { f.close(); return false; }

    uint32_t sizeCached = (uint32_t)header.substring(
        sPos + 5, header.indexOf(' ', sPos + 5) < 0 ? header.length() : header.indexOf(' ', sPos + 5)
    ).toInt();

    uint64_t fpCached = 0ULL;
    {
        String fps = header.substring(fpPos + 3);
        if (fps.startsWith("0x") || fps.startsWith("0X")) {
            fps.remove(0, 2);
            for (int i = 0; i < (int)fps.length(); ++i) {
                char ch = fps[i];
                uint8_t v = (ch >= '0' && ch <= '9') ? ch - '0' :
                            (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 :
                            (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : 0;
                fpCached = (fpCached << 4) | v;
            }
        } else {
            fpCached = (uint64_t)strtoull(fps.c_str(), nullptr, 10);
        }
    }
    if (sizeCached != fsize || fpCached != fp) { f.close(); return false; }

    String line;
    while (f.available()) {
        line = "";
        while (f.available()) { char c = f.read(); if (c == '\n') break; line += c; }
        if (line.length() == 0) continue;
        int tab = line.indexOf('\t'); if (tab <= 0) continue;
        uint32_t off = (uint32_t)line.substring(0, tab).toInt();
        String title = line.substring(tab + 1);
        g_toc.push_back({title, off});
    }
    f.close();
    if (g_toc.empty()) return false;
    g_tocCursor = 0; g_tocTop = 0;
    return true;
}
static bool saveTOCToCache(uint64_t fp, uint32_t fsize) {
    if (g_toc.empty()) return false;
    String path = _tocPathFor(g_currentFile);
    File f = LittleFS.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    f.printf("EBOOK_TOC_V1 size=%lu fp=0x%llX\n", (unsigned long)fsize, (unsigned long long)fp);
    for (const auto &e : g_toc) {
        f.printf("%lu\t", (unsigned long)e.offset);
        f.print(e.title); f.print('\n');
    }
    f.close();
    return true;
}

static void buildTOC_withCacheAndProgress() {
    ensureSDBuf(64 * 1024);

    g_toc.clear();
    if (!g_file) return;

    const uint32_t fsize = g_file.size();
    uint64_t fp = _fastFileFingerprint(g_file);
    if (loadTOCFromCache(fp, fsize)) return;

    EPD_FullBW_Text("首次打开：正在构建目录（仅需一次）", 8, CONTENT_Y + 40,
                    false, 26, 0, FONT, nullptr,
                    1, GxEPD_BLACK, GxEPD_WHITE, true, false, false);
    UI_RedrawStatusBar(true);
    _showTOCProgress(0, fsize);

    const size_t BUF = 4096;
    static uint8_t buf[BUF];
    String line; line.reserve(256);
    uint32_t absOff = 0, lineStartOff = 0, lastDraw = 0;
    g_file.seek(0);

    while (true) {
        size_t n = g_file.read(buf, BUF);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            char c = (char)buf[i];
            absOff++;
            if (c == '\r') continue;
            if (c == '\n') {
                if (_looksLikeChapter(line)) g_toc.push_back({line, lineStartOff});
                line = ""; line.reserve(256);
                lineStartOff = absOff;
            } else {
                if (line.length() < 256) line += c;
            }
        }
        const uint32_t MIN_CHUNK = 128 * 1024;
        uint32_t pctChunk = max<uint32_t>(MIN_CHUNK, fsize / 50); // 2%
        if (absOff - lastDraw >= pctChunk || absOff >= fsize) {
            _showTOCProgress(absOff, fsize);
            lastDraw = absOff;
        }
    }
    if (line.length() > 0 && _looksLikeChapter(line)) g_toc.push_back({line, lineStartOff});
    if (g_toc.empty()) g_toc.push_back({"开始", 0});

    saveTOCToCache(fp, fsize);

    EPD_PartialBW_Text(8, CONTENT_Y + 120, 320, 24,
                       "目录构建完成", 0, FONT, nullptr,
                       1, GxEPD_BLACK, GxEPD_WHITE, 6, 22,
                       true, false, false);
    g_tocCursor = 0; g_tocTop = 0;
}

static uint32_t _layoutSignature() {
    const uint32_t v = 2; 
    uint32_t sig = 0;
    auto mix = [&](uint32_t x) { sig = (sig * 16777619u) ^ x; };
    mix(v); mix((uint32_t)READER_W); mix((uint32_t)READER_LINE_H); mix((uint32_t)READER_TEXT_XOFF);
    return sig ? sig : 0xA5A5A5A5u;
}
static void _showPGXProgress(uint32_t doneBytes, uint32_t totalBytes) {
    int percent = (totalBytes > 0) ? (int)((doneBytes * 100ULL) / totalBytes) : 0;
    String msg = "预生成分页… " + String(percent) + "%";
    EPD_PartialBW_Text(8, CONTENT_Y + 100, 320, 24,
                       msg.c_str(), 0, FONT, nullptr,
                       1, GxEPD_BLACK, GxEPD_WHITE, 6, 22,
                       true, false, false);
}
static bool loadPageOffsetsFromCache(uint64_t fp, uint32_t fsize, uint32_t layoutSig) {
    g_pageOffsets.clear();
    String path = _pgxPathFor(g_currentFile);
    File f = LittleFS.open(path.c_str(), FILE_READ);
    if (!f) return false;

    String header;
    while (f.available()) { char c = f.read(); if (c == '\n') break; header += c; }
    if (!header.startsWith("EBOOK_PGX_V1")) { f.close(); return false; }

    auto nextToken = [&](const String &tok) -> String {
        int p = header.indexOf(tok); if (p < 0) return String();
        int s = p + tok.length(); int e = header.indexOf(' ', s); if (e < 0) e = header.length();
        return header.substring(s, e);
    };
    String sSize   = nextToken("size=");
    String sFp     = nextToken("fp=");
    String sLayout = nextToken("layout=");
    if (sSize.length()==0 || sFp.length()==0 || sLayout.length()==0) { f.close(); return false; }

    uint32_t sizeCached = (uint32_t)sSize.toInt();
    uint64_t fpCached = 0ULL;
    {
        String fps = sFp;
        if (fps.startsWith("0x") || fps.startsWith("0X")) {
            fps.remove(0, 2);
            for (int i = 0; i < (int)fps.length(); ++i) {
                char ch = fps[i];
                uint8_t v = (ch >= '0' && ch <= '9') ? ch - '0' :
                            (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 :
                            (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : 0;
                fpCached = (fpCached << 4) | v;
            }
        } else {
            fpCached = (uint64_t)strtoull(fps.c_str(), nullptr, 10);
        }
    }
    uint32_t layoutCached = (uint32_t)sLayout.toInt();
    if (sizeCached != fsize || fpCached != fp || layoutCached != layoutSig) { f.close(); return false; }

    String line;
    while (f.available()) {
        line = "";
        while (f.available()) { char c = f.read(); if (c == '\n') break; line += c; }
        line.trim();
        if (line.length() == 0) continue;
        uint32_t off = (uint32_t)line.toInt();
        if (!g_pageOffsets.empty() && off <= g_pageOffsets.back()) { f.close(); g_pageOffsets.clear(); return false; }
        g_pageOffsets.push_back(off);
    }
    f.close();

    if (g_pageOffsets.empty() || g_pageOffsets.front() != 0u) { g_pageOffsets.clear(); return false; }
    return true;
}
static bool savePageOffsetsToCache(uint64_t fp, uint32_t fsize, uint32_t layoutSig) {
    if (g_pageOffsets.empty() || g_pageOffsets.front() != 0u) return false;
    String path = _pgxPathFor(g_currentFile);
    File f = LittleFS.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    f.printf("EBOOK_PGX_V1 size=%lu fp=0x%llX layout=%lu\n",
             (unsigned long)fsize, (unsigned long long)fp, (unsigned long)layoutSig);
    for (uint32_t off : g_pageOffsets) f.printf("%lu\n", (unsigned long)off);
    f.close();
    return true;
}
static void buildPageOffsets_withCacheAndProgress() {
    ensureSDBuf(64 * 1024);
    if (!g_file) return;

    const uint32_t fsize = g_file.size();
    const uint64_t fp    = _fastFileFingerprint(g_file);
    const uint32_t sig   = _layoutSignature();

    if (loadPageOffsetsFromCache(fp, fsize, sig)) return;

    EPD_FullBW_Text("首次打开：正在预生成分页（仅需一次）", 8, CONTENT_Y + 70,
                    false, 26, 0, FONT, nullptr,
                    1, GxEPD_BLACK, GxEPD_WHITE, true, false, false);
    UI_RedrawStatusBar(true);
    _showPGXProgress(0, fsize);

    bindMeasureFont();

    g_pageOffsets.clear();
    g_pageOffsets.push_back(0);

    uint32_t off = 0, lastDraw = 0;
    String dummy[READER_MAX_LINES];
    int cnt = 0;

while (true) {
    cnt = 0;

    uint32_t nextOff = paginatePage(g_file, off, dummy, cnt);

    // 没有读到正文，或偏移没有推进，说明已经到文件末尾。
    if (cnt <= 0 || nextOff <= off) {
        break;
    }

    // nextOff == fsize 说明当前页已经是最后一页。
    // fsize 不能作为“下一页的起始位置”保存。
    if (nextOff >= fsize) {
        break;
    }

    g_pageOffsets.push_back(nextOff);
    off = nextOff;

    const uint32_t MIN_CHUNK = 128 * 1024;
    uint32_t pctChunk = max<uint32_t>(MIN_CHUNK, fsize / 50);

    if (off - lastDraw >= pctChunk) {
        _showPGXProgress(off, fsize);
        lastDraw = off;
    }
}

// 即使最后一页没有写入新的页首偏移，也让进度显示到 100%。
_showPGXProgress(fsize, fsize);

    savePageOffsetsToCache(fp, fsize, sig);

    EPD_PartialBW_Text(8, CONTENT_Y + 150, 320, 24,
                       "分页预生成完成", 0, FONT, nullptr,
                       1, GxEPD_BLACK, GxEPD_WHITE, 6, 22,
                       true, false, false);
}
static int findPageIndexForOffset(uint32_t target) {
    if (g_pageOffsets.empty()) return 0;
    int lo = 0, hi = (int)g_pageOffsets.size() - 1;
    while (lo < hi) { int mid = (lo + hi + 1) >> 1; if (g_pageOffsets[mid] <= target) lo = mid; else hi = mid - 1; }
    return lo;
}

static inline int nextUTF8(const uint8_t *buf, size_t len, size_t pos, char out[5]) {
    if (pos >= len) return 0;
    uint8_t b0 = buf[pos];
    int need = (b0 < 0x80) ? 1 : (b0 < 0xE0) ? 2 : (b0 < 0xF0) ? 3 : 4;
    if (pos + need > len) return -1;
    for (int i = 0; i < need; ++i) out[i] = (char)buf[pos + i];
    out[need] = 0;
    return need;
}

static bool isTxt(const char *name) {
    int n = strlen(name); if (n < 4) return false;
    const char *ext = name + n - 4;
    return (ext[0] == '.' &&
            (ext[1] == 't' || ext[1] == 'T') &&
            (ext[2] == 'x' || ext[2] == 'X') &&
            (ext[3] == 't' || ext[3] == 'T'));
}

static void scanTxtFiles() {
    g_files.clear();

    File root = LittleFS.open("/");
    if (!root) {
        Serial.println("[Ebook] LittleFS 根目录打开失败");
        return;
    }

    while (true) {
        File e = root.openNextFile();
        if (!e) break;

        if (!e.isDirectory() && isTxt(e.name())) {
            String path = String(e.name());

            // 防止部分 LittleFS 版本返回 /book_01.txt 后，
            // 代码又额外拼出 //book_01.txt。
            if (!path.startsWith("/")) {
                path = "/" + path;
            }

            Serial.println("[Ebook] 找到小说：" + path);
            g_files.push_back(path);
        }

        e.close();
    }

    root.close();

    if (g_cursor >= (int)g_files.size()) {
        g_cursor = max(0, (int)g_files.size() - 1);
    }

    g_menuTop = 0;
}

static String fitToPixelWidth(const String &s, int maxPx) {
    if (getUTF8StringWidth_cached(s) <= maxPx) return s;
    String ell = "…"; int ellW = getUTF8StringWidth_cached(ell);
    String out; int w = 0;
    for (size_t i = 0; i < s.length();) {
        uint8_t c = (uint8_t)s[i];
        int need = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (i + need > s.length()) break;
        String cp = s.substring(i, i + need);
        int cpw = getUTF8StringWidth_cached(cp);
        if (w + cpw + ellW > maxPx) break;
        out += cp; w += cpw; i += need;
    }
    out += ell;
    return out;
}

static uint32_t paginatePage(File &f, uint32_t startOffset, String *outLines, int &outCount)
{
    bindMeasureFont();  // ← 保证字宽 LUT 有效
    String line; line.reserve(256);
    int lineWidth = 0;
    int lastSpacePos = -1;
    int lastSpaceWidth = 0;
    const int SPACE_W = g_ascii_w[' ']; 

    outCount = 0;
    if (!f) return startOffset;

    const int maxWidth = READER_W - 2 * READER_TEXT_XOFF - 6 - 20;

    ensureSDBuf(64 * 1024);
    uint8_t* buf = g_sdBuf;
    size_t bufUsed = 0, bufPos = 0;

    auto refill = [&]() -> bool {
        bufUsed = f.read(buf, g_sdBufCap);
        bufPos  = 0;
        return bufUsed > 0;
    };

    f.seek(startOffset);
    if (!refill()) return startOffset;

    uint32_t absolutePos = startOffset;

    while (outCount < READER_MAX_LINES)
    {
        if (bufPos >= bufUsed) { if (!refill()) break; }

        uint8_t b0 = buf[bufPos];

        if (b0 == '\r') { bufPos++; absolutePos++; continue; }
        if (b0 == '\n') {
            outLines[outCount++] = line;
            line = ""; line.reserve(256);
            lineWidth = 0; lastSpacePos = -1; lastSpaceWidth = 0;
            bufPos++; absolutePos++;
            if (outCount >= READER_MAX_LINES) break;
            continue;
        }

        if (b0 < 0x80) {
            size_t p = bufPos;
            int    w = lineWidth;
            int    lastSpPos   = lastSpacePos;
            int    lastSpWidth = lastSpaceWidth;

            while (p < bufUsed) {
                uint8_t c = buf[p];
                if (c == '\n' || c == '\r' || c >= 0x80) break;

                int cw = g_ascii_w[c];
                if (c == ' ' || c == '\t') {
                    lastSpPos   = line.length() + (int)(p - bufPos);
                    lastSpWidth = w;
                }

                if (w + cw > maxWidth) {
                    const int consumed = (int)(p - bufPos);

                    if (lastSpPos >= 0) {
                        outLines[outCount++] =
                            line + String((const char*)buf + bufPos, lastSpPos - (int)line.length());

                        const int tailBytes =
                            consumed - (lastSpPos - (int)line.length()) - 1; 
                        line = String(
                            (const char*)buf + bufPos + (lastSpPos - (int)line.length()) + 1,
                            tailBytes);

                        w = w - lastSpWidth - SPACE_W;

                        bufPos      += consumed;
                        absolutePos += consumed;

                        lastSpPos    = -1;
                        lastSpWidth  = 0;
                        lineWidth    = w;
                        lastSpacePos = lastSpPos;
                        lastSpaceWidth = lastSpWidth;

                        if (outCount >= READER_MAX_LINES) return absolutePos;
                        continue; 
                    } else {
                        outLines[outCount++] =
                            line + String((const char*)buf + bufPos, consumed);
                        line = ""; w = 0;

                        bufPos      += consumed;
                        absolutePos += consumed;

                        lastSpPos    = -1;
                        lastSpWidth  = 0;
                        lineWidth    = w;
                        lastSpacePos = lastSpPos;
                        lastSpaceWidth = lastSpWidth;

                        if (outCount >= READER_MAX_LINES) return absolutePos;
                        continue;
                    }
                }

                w += cw; ++p;
            }

            if (p > bufPos) {
                const int consumed = (int)(p - bufPos);
                line += String((const char*)buf + bufPos, consumed);
                bufPos      += consumed;
                absolutePos += consumed;

                lineWidth       = w;
                lastSpacePos    = -1;
                lastSpaceWidth  = 0;
                continue;
            }
        }

        char cp[5];
        int adv = nextUTF8(buf, bufUsed, bufPos, cp);
        if (adv < 0) {
            f.seek(absolutePos);
            if (!refill()) break;
            adv = nextUTF8(buf, bufUsed, bufPos, cp);
            if (adv <= 0) break;
        }

        int wcp = cachedGlyphWidth(cp);
        bool breakable = (cp[0] == ' ' || cp[0] == '\t');
        if (breakable) { lastSpacePos = line.length(); lastSpaceWidth = lineWidth; }

        if (lineWidth + wcp > maxWidth) {
            if (lastSpacePos >= 0) {
                outLines[outCount++] = line.substring(0, lastSpacePos);
                line.remove(0, lastSpacePos + 1);
                lineWidth = getUTF8StringWidth_cached(line);
                lastSpacePos = -1; lastSpaceWidth = 0;
            } else {
                outLines[outCount++] = line;
                line = ""; line.reserve(256); lineWidth = 0;
            }
            if (outCount >= READER_MAX_LINES) continue;
        }

        line += cp; lineWidth += wcp;
        bufPos += adv; absolutePos += adv;
    }

    if (outCount < READER_MAX_LINES && line.length() > 0)
        outLines[outCount++] = line;

    return absolutePos;
}

static void saveProgress() {
    if (!g_file) return;
    String p = _progressPathFor(g_currentFile);
    File f = LittleFS.open(p.c_str(), FILE_WRITE);
    if (!f) return;
    uint32_t startOff = (g_pageIndex < (int)g_pageOffsets.size()) ? g_pageOffsets[g_pageIndex] : 0;
    f.printf("pageIndex=%d\nstartOffset=%lu\n", g_pageIndex, (unsigned long)startOff);
    f.close();
}
static bool loadProgress(int &pageIndexOut, uint32_t &offsetOut) {
    pageIndexOut = 0; offsetOut = 0;
    String p = _progressPathFor(g_currentFile);
    File f = LittleFS.open(p.c_str(), FILE_READ);
    if (!f) return false;
    String all;
    while (f.available()) { all += (char)f.read(); if (all.length() > 512) break; }
    f.close();
    int pi = all.indexOf("pageIndex=");
    int so = all.indexOf("startOffset=");
    if (pi >= 0) {
        int e = all.indexOf('\n', pi);
        pageIndexOut = all.substring(pi + 10, (e >= 0 ? e : all.length())).toInt();
    }
    if (so >= 0) {
        int e = all.indexOf('\n', so);
        offsetOut = (uint32_t)all.substring(so + 12, (e >= 0 ? e : all.length())).toInt();
    }
    return true;
}

static bool _looksLikeChapter(const String &line) {
    if (line.length() < 3) return false;
    int i = 0; while (i < (int)line.length() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= (int)line.length()) return false;
    String s = line.substring(i);
    if (s.startsWith("第") && (s.indexOf("章") > 0 || s.indexOf("卷") > 0)) return true;
    if (s.startsWith("章") || s.startsWith("卷")) return true;
    if (s.startsWith("Chapter ") || s.startsWith("CHAPTER ") || s.startsWith("Vol.") || s.startsWith("VOL.")) return true;
    return false;
}

static void buildTOC() {
    g_toc.clear();
    if (!g_file) return;

    uint32_t off = 0;
    String lines[READER_MAX_LINES];
    int cnt = 0;

    while (true) {
        uint32_t nextOff = paginatePage(g_file, off, lines, cnt);
        if (nextOff == off) break;

        for (int i = 0; i < cnt; ++i) {
            if (_looksLikeChapter(lines[i])) { g_toc.push_back({ lines[i], off }); break; }
        }
        off = nextOff;
        if ((int)g_toc.size() >= 4096) break;
    }
    if (g_toc.empty()) g_toc.push_back({ "开始", 0 });
    g_tocCursor = 0; g_tocTop = 0;
}

static int ensurePagesUpToOffset(uint32_t target) {
    if (g_pageOffsets.empty()) g_pageOffsets.push_back(0);
    uint32_t last = g_pageOffsets.back();
    String dummy[READER_MAX_LINES]; int d = 0;
    while (last < target) {
    d = 0;

    uint32_t nextOff = paginatePage(g_file, last, dummy, d);

    if (d <= 0 ||
        nextOff <= last ||
        nextOff >= g_file.size()) {
        break;
    }

    g_pageOffsets.push_back(nextOff);
    last = nextOff;
}
    int i = 0;
    for (; i + 1 < (int)g_pageOffsets.size(); ++i) {
        if (g_pageOffsets[i] <= target && target < g_pageOffsets[i + 1]) return i;
    }
    return max(0, (int)g_pageOffsets.size() - 1);
}

static void gotoOffset(uint32_t target) {
    int idx = ensurePagesUpToOffset(target);
    g_pageIndex = idx;
    g_pagesSinceFull++;
    renderCurrentPage(true);
    prefetchNextPage();
    saveProgress();
}

static void prefetchNextPage() {
    g_cachedNextCount = -1;

    if (!g_file) return;
    if (g_pageIndex < 0 || g_pageIndex >= (int)g_pageOffsets.size()) return;

    const uint32_t fileSize = g_file.size();

    // 当前尚未知道下一页页首：尝试从当前页推导。
    if ((int)g_pageOffsets.size() <= g_pageIndex + 1) {
        String currentLines[READER_MAX_LINES];
        int currentCount = 0;

        uint32_t currentStart = g_pageOffsets[g_pageIndex];
        uint32_t nextOff = paginatePage(
            g_file,
            currentStart,
            currentLines,
            currentCount
        );

        // 当前页就是最后一页，不存在下一页。
        if (currentCount <= 0 ||
            nextOff <= currentStart ||
            nextOff >= fileSize) {
            return;
        }

        g_pageOffsets.push_back(nextOff);
    }

    // 现在才真正读取“下一页”的内容到缓存。
    uint32_t nextStart = g_pageOffsets[g_pageIndex + 1];

    String tmp[READER_MAX_LINES];
    int cnt = 0;

    paginatePage(g_file, nextStart, tmp, cnt);

    // EOF 或异常情况下，不把空页当作可翻页页面。
    if (cnt <= 0) {
        return;
    }

    for (int i = 0; i < cnt; i++) {
        g_cachedNextLines[i] = tmp[i];
    }

    g_cachedNextCount = cnt;
}

static void renderMenu(bool fullRefresh) {
    if (fullRefresh) {
        EPD_FullBW_Text("选择 TXT 文件：中键打开，左右移动光标",
                        8, CONTENT_Y + 40,
                        /*first_init=*/false, 26,
                        0, FONT, nullptr,
                        1, GxEPD_BLACK, GxEPD_WHITE,
                        true, false, false);
        UI_RedrawStatusBar(true);
    }

    const int16_t x = MENU_MARGIN_X;
    const int16_t w = SCREEN_W - MENU_MARGIN_X * 2;
    const int16_t maxLines = (SCREEN_H - MENU_START_Y) / MENU_LINE_H;

    if (g_cursor < g_menuTop) g_menuTop = g_cursor;
    if (g_cursor >= g_menuTop + maxLines) g_menuTop = g_cursor - maxLines + 1;

    String rows[60]; int n = 0;
    for (int i = 0; i < maxLines; ++i) {
        int idx = g_menuTop + i;
        if (idx >= (int)g_files.size()) break;
        String bookName = getBookDisplayName(g_files[idx]);

        rows[n++] = (idx == g_cursor)
            ? String(">> ") + bookName
            : String("   ") + bookName;

            rows[n - 1] += "    ";
    }

    if (n > 0) {
        const int16_t maxPx = w - 2 * MENU_TEXT_XOFF;
        for (int i = 0; i < n; ++i) rows[i] = fitToPixelWidth(rows[i], maxPx);

        EPD_PartialBW_MultiText(x, MENU_START_Y, rows, n,
                                MENU_LINE_H, 0, FONT, nullptr,
                                1, GxEPD_BLACK, GxEPD_WHITE,
                                MENU_TEXT_XOFF, MENU_BASE_OFF,
                                true, false, false);
    }

    touch();
}

static void renderCurrentPage(bool forceFull) {
    if (!g_file) return;

    String linesArr[READER_MAX_LINES]; int cnt = 0;
    uint32_t startOff = g_pageOffsets[g_pageIndex];
    uint32_t nextOff = paginatePage(g_file, startOff, linesArr, cnt);


if (cnt <= 0) {
    Serial.println("[Ebook] 当前页没有可显示内容，拒绝渲染。");
    return;
}

    if ((int)g_pageOffsets.size() == g_pageIndex + 1 &&
    nextOff > startOff &&
    nextOff < g_file.size()) {

    g_pageOffsets.push_back(nextOff);
}

    bool doFull = forceFull || (g_pagesSinceFull % 5 == 0) || (cnt < READER_MAX_LINES);

    if (doFull) {
        EPD_FullBW_MultiText(READER_MARGIN_X, READER_START_Y,
                             linesArr, cnt, READER_LINE_H,
                             0, FONT, nullptr,
                             1, GxEPD_BLACK, GxEPD_WHITE,
                             READER_TEXT_XOFF, READER_BASE_OFF,
                             true, false, false,
                             /*first_init=*/false);
        UI_RedrawStatusBar(true);
    } else {
        EPD_PartialBW_MultiText(READER_MARGIN_X, READER_START_Y,
                                linesArr, cnt, READER_LINE_H,
                                0, FONT, nullptr,
                                1, GxEPD_BLACK, GxEPD_WHITE,
                                READER_TEXT_XOFF, READER_BASE_OFF,
                                true, false, false);
    }

    prefetchNextPage();
    touch();
}

static void renderReaderMenu(bool full) {
    if (full) {
        EPD_FullBW_Text("阅读菜单（左右选择，OK 确认）", 8, CONTENT_Y + 40,
                        false, 26, 0, FONT, nullptr,
                        1, GxEPD_BLACK, GxEPD_WHITE, true, false, false);
        UI_RedrawStatusBar(true);
    }
    String rows[8]; int n = 0;
    for (int i = 0; i < 3; ++i) {
        String item = (g_readerMenuCursor == i) ? ">> " : "   ";
        item += kReaderMenuItems[i];
        rows[n++] = item;
    }
    EPD_PartialBW_MultiText(MENU_MARGIN_X, MENU_START_Y, rows, n, MENU_LINE_H,
                            0, FONT, nullptr,
                            1, GxEPD_BLACK, GxEPD_WHITE,
                            MENU_TEXT_XOFF, MENU_BASE_OFF,
                            true, false, false);
    touch();
}

static void renderTOC(bool full) {
    if (full) {
        EPD_FullBW_Text("目录（左右移动，OK 跳转）",
                        8, CONTENT_Y + 40, false, 26,
                        0, FONT, nullptr,
                        1, GxEPD_BLACK, GxEPD_WHITE, true, false, false);
        UI_RedrawStatusBar(true);
    }
    const int16_t x = MENU_MARGIN_X;
    const int16_t w = SCREEN_W - MENU_MARGIN_X * 2;
    const int16_t maxLines = (SCREEN_H - MENU_START_Y) / MENU_LINE_H;

    if (g_tocCursor < g_tocTop) g_tocTop = g_tocCursor;
    if (g_tocCursor >= g_tocTop + maxLines) g_tocTop = g_tocCursor - maxLines + 1;

    String rows[60]; int n = 0;
    for (int i = 0; i < maxLines; ++i) {
        int idx = g_tocTop + i;
        if (idx >= (int)g_toc.size()) break;
        String title = g_toc[idx].title;
        int percent = 0;
        if (!g_pageOffsets.empty()) {
            uint32_t off = g_toc[idx].offset;
            uint32_t fileSize = g_file.size();
            percent = (int)((off * 100ULL) / (fileSize ? fileSize : 1));
        }
        String line = (idx == g_tocCursor ? ">> " : "   ");
        line += title + "  (" + String(percent) + "%)";
        rows[n++] = fitToPixelWidth(line, w - 2 * MENU_TEXT_XOFF);
    }

    if (n > 0) {
        EPD_PartialBW_MultiText(x, MENU_START_Y, rows, n, MENU_LINE_H,
                                0, FONT, nullptr,
                                1, GxEPD_BLACK, GxEPD_WHITE,
                                MENU_TEXT_XOFF, MENU_BASE_OFF,
                                true, false, false);
    }
    touch();
}

static void nextPage() {
    if (!g_file) return;

    if (g_pageIndex < 0 || g_pageIndex >= (int)g_pageOffsets.size()) {
        Serial.println("[Ebook] 页索引异常，拒绝翻页。");
        return;
    }

    // 优先使用已经预读好的下一页。
    // 注意必须 > 0，不能允许“0 行空页”进入。
    if (g_cachedNextCount > 0 &&
        (int)g_pageOffsets.size() > g_pageIndex + 1) {

        g_pageIndex++;
        g_pagesSinceFull++;

        bool doFull =
            (g_pagesSinceFull % 5 == 0) ||
            (g_cachedNextCount < READER_MAX_LINES);

        if (doFull) {
            EPD_FullBW_MultiText(
                READER_MARGIN_X, READER_START_Y,
                g_cachedNextLines, g_cachedNextCount, READER_LINE_H,
                0, FONT, nullptr,
                1, GxEPD_BLACK, GxEPD_WHITE,
                READER_TEXT_XOFF, READER_BASE_OFF,
                true, false, false,
                false
            );
            UI_RedrawStatusBar(true);
        } else {
            EPD_PartialBW_MultiText(
                READER_MARGIN_X, READER_START_Y,
                g_cachedNextLines, g_cachedNextCount, READER_LINE_H,
                0, FONT, nullptr,
                1, GxEPD_BLACK, GxEPD_WHITE,
                READER_TEXT_XOFF, READER_BASE_OFF,
                true, false, false
            );
        }

        prefetchNextPage();
        saveProgress();
        touch();
        return;
    }

    // 已经有下一页页首，但没有预读缓存时，重新渲染。
    if ((int)g_pageOffsets.size() > g_pageIndex + 1) {
        g_pageIndex++;
        g_pagesSinceFull++;
        renderCurrentPage();
        saveProgress();
        return;
    }

    // 没有已知的下一页，临时判断是否真的存在。
    String dummy[READER_MAX_LINES];
    int count = 0;

    uint32_t currentStart = g_pageOffsets[g_pageIndex];
    uint32_t nextOff = paginatePage(g_file, currentStart, dummy, count);

    // 当前页就是最后一页：不要制造一个 EOF 空白页。
    if (count <= 0 ||
        nextOff <= currentStart ||
        nextOff >= g_file.size()) {
        return;
    }

    g_pageOffsets.push_back(nextOff);

    g_pageIndex++;
    g_pagesSinceFull++;

    renderCurrentPage();
    saveProgress();
}

static void prevPage() {
    if (!g_file) return;
    if (g_pageIndex == 0) return;
    g_pageIndex--; g_pagesSinceFull++;
    renderCurrentPage();
    saveProgress();
}

void start() {
    mode = Mode::MENU;
    g_cursor = 0; g_menuTop = 0;
    g_pageIndex = 0; g_pagesSinceFull = 0;
    g_firstMenu = true;

    // 删掉 EbookApp::scanTxtFiles(); 这行，交给step_ext首次执行时扫描

    display->setRotation(1);
    u8g2Fonts.begin(*display);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

    EPD_FullBW_Text("电子书 - SD 文本浏览", 8, 70,
                    false, 26, 0, FONT, nullptr,
                    1, GxEPD_BLACK, GxEPD_WHITE, true, false, false);
}

static bool openSelectedFile() {
    if (g_files.empty()) return false;
    if (g_file) g_file.close();

    g_currentFile = g_files[g_cursor];
    g_file = LittleFS.open(g_currentFile.c_str(), FILE_READ);
    g_pageOffsets.clear(); g_pageOffsets.push_back(0);
    g_pageIndex = 0;
    g_pagesSinceFull = 0;
    g_toc.clear();

    if (g_file) {
        bindMeasureFont();

        buildPageOffsets_withCacheAndProgress();
        buildTOC_withCacheAndProgress();

        int savedIdx = 0; uint32_t savedOff = 0;
        if (loadProgress(savedIdx, savedOff)) {
            if (savedOff > 0 && !g_pageOffsets.empty()) {
                g_pageIndex = findPageIndexForOffset(savedOff);
            } else if (savedIdx > 0 && savedIdx < (int)g_pageOffsets.size()) {
                g_pageIndex = savedIdx;
            } else {
                g_pageIndex = min(savedIdx, max(0, (int)g_pageOffsets.size() - 1));
            }
        }
    }
    return (bool)g_file;
}

// 电子书主循环步进（按键由外部taskUICore传入）
void step_ext(bool nextPressed, bool okPressed) {
    if (mode == Mode::MENU) {
        if (g_firstMenu) { scanTxtFiles(); renderMenu(true); g_firstMenu = false; }

        // GPIO15：光标下移，循环
        if (nextPressed) {
            g_cursor = (g_cursor + 1) % max(1, (int)g_files.size());
            renderMenu();
        }
        // GPIO2：确认打开
        if (okPressed) {
            if (openSelectedFile()) {
                mode = Mode::READER;
                g_pagesSinceFull = 0;
                renderCurrentPage(true);
                prefetchNextPage();
            }
        }
    }
    else if (mode == Mode::READER) {
            if (nextPressed) {
                nextPage();
            }
            else if (okPressed) {
                prevPage();
            }
        }
    else if (mode == Mode::READER_MENU) {
        if (nextPressed) { 
            if (g_readerMenuCursor < 2) g_readerMenuCursor++; 
            else g_readerMenuCursor = 0;  // 循环
            renderReaderMenu(); 
        }
        if (okPressed) {
            if (g_readerMenuCursor == 0) {
                if (g_toc.empty()) buildTOC_withCacheAndProgress();
                mode = Mode::TOC;
                renderTOC(true);
            } else if (g_readerMenuCursor == 1) {
                saveProgress();
                EPD_PartialBW_Text(2, 30, 200, 24, "已保存进度",
                                   0, FONT, nullptr, 1,
                                   GxEPD_BLACK, GxEPD_WHITE, 6, 22, true, false, false);
                mode = Mode::READER;
            } else {
                if (g_file) g_file.close();
                mode = Mode::MENU;
                renderMenu(true);
            }
        }
    }
    else if (mode == Mode::TOC) {
        if (nextPressed) {
            g_tocCursor = (g_tocCursor + 1) % max(1, (int)g_toc.size());
            renderTOC();
        }
        if (okPressed) {
            uint32_t off = g_toc[g_tocCursor].offset;
            mode = Mode::READER;
            gotoOffset(off);
        }
    }
}

void stop() {
    saveProgress();

    if (g_file) {
        g_file.close();
    }

    g_cachedNextCount = -1;
    g_pageOffsets.clear();
    g_toc.clear();

    if (g_sdBuf) {
        heap_caps_free(g_sdBuf);
        g_sdBuf = nullptr;
        g_sdBufCap = 0;
    }
}
} // namespace 

// 全局刷新引擎（融合两套代码：第一套驱动参数 + 第二套保护机制）
void updateScreenFull(bool forceReset) {
    Serial.println(">>> [1] 进入 updateScreenFull");
    fullRefreshCount++;
    if (fullRefreshCount % 50 == 0) forceReset = true;

    Serial.println(">>> [2] 准备 display->init");
    if (forceReset) {
        display->init(115200, true, 10, false);
    } else {
        display->init(0, false);
    }
    Serial.println(">>> [3] display->init 完成");

    display->setFullWindow();
    Serial.println(">>> [4] setFullWindow 完成");

    display->firstPage();
    Serial.println(">>> [5] firstPage 完成");

    do {
        Serial.println(">>> [6] fillScreen");
        display->fillScreen(GxEPD_WHITE);
        Serial.println(">>> [7] 开始绘制界面");
        if (isImageMode) {
            // ← 新增这个分支
            drawImageScreen();
        }else if (isMusicPlaying) {
    // ← 新增：音乐播放时优先显示音乐界面
        drawMusicScreen();
        }else {
            switch (currentScreen) {
                case 0: drawScreen1();   break;
                case 1: drawScreen2();   break;
                case 2: drawScreen3();   break;
                case 3: drawTodoList();  break;
                case 4: drawFlashcard(); break;
            }
        }
    } while (display->nextPage());

if (isImageMode) {
    // 画廊模式：彻底休眠，后续不需要局刷
    Serial.println(">>> [9] hibernate (画廊模式)");
    display->hibernate();
} else {
    // 天气模式：软关闭电源，保留帧缓存供局刷使用
    Serial.println(">>> [9] powerOff (天气模式，保留帧缓存)");
    display->powerOff();
}
Serial.println(">>> [10] 刷新完成");
}

// 通用局刷引擎模板 (来自第二套代码)
template <typename DrawCallback>
void updatePartialRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, DrawCallback drawCallback) {
    display->setPartialWindow(x, y, w, h);
    display->firstPage();
    do {
        display->fillScreen(GxEPD_WHITE);
        drawCallback();
    } while (display->nextPage());
}

// 局刷左上角时间 (来自第二套代码)
void updateTimeOnly() {
    vTaskDelay(20 / portTICK_PERIOD_MS);
    uint16_t box_x = 10, box_y = 8, box_w = 100, box_h = 25;
    display->setPartialWindow(box_x, box_y, box_w, box_h);
    display->firstPage();
    do {
        display->fillScreen(GxEPD_WHITE);
        String timeStr = getCurrentTime();
        writeFont(box_x + 10, box_y, timeStr, u8g2_simsun_24_gb2312);
    } while (display->nextPage());
}

// 局刷右上角状态栏WiFi/电池 (来自第二套代码)
void updateStatusBarPartial() {
    vTaskDelay(20 / portTICK_PERIOD_MS);
    uint16_t boxX = 620, boxY = 0, boxW = 180, boxH = 40;
    int currentRSSI = WiFi.RSSI();
    int currentBat  = 100; // 待接入真实ADC检测后替换
    updatePartialRegion(boxX, boxY, boxW, boxH, [&]() {
        drawDynamicWiFiIcon(680, 3, currentRSSI);
        drawDynamicBatteryIcon(720, 3, currentBat);
        writeFont(750, 13, String(currentBat) + "%", u8g2_simsun_12_gb2312);
    });
}


// ============================================================
// =================== 动态图标绘制 ===========================
// ============================================================

// 动态WiFi信号图标 (来自第二套代码)
void drawDynamicWiFiIcon(int x, int y, int rssi) {
    const unsigned char* icon_array = wifi_off_32x32;
    if (WiFi.status() != WL_CONNECTED) {
        icon_array = wifi_x_32x32;
    } else {
        if      (rssi >= -55) icon_array = wifi_32x32;
        else if (rssi >= -70) icon_array = wifi_3_bar_32x32;
        else if (rssi >= -85) icon_array = wifi_2_bar_32x32;
        else                  icon_array = wifi_1_bar_32x32;
    }
    display->drawBitmap(x, y, icon_array, 32, 32, GxEPD_WHITE, GxEPD_BLACK);
}

// 动态电池图标 (来自第二套代码)
void drawDynamicBatteryIcon(int x, int y, int batPercent) {
    const unsigned char* icon_array = battery_0_bar_90deg_32x32;
    if      (batPercent >= 95) icon_array = battery_full_90deg_32x32;
    else if (batPercent >= 80) icon_array = battery_5_bar_90deg_32x32;
    else if (batPercent >= 60) icon_array = battery_4_bar_90deg_32x32;
    else if (batPercent >= 40) icon_array = battery_3_bar_90deg_32x32;
    else if (batPercent >= 20) icon_array = battery_2_bar_90deg_32x32;
    else if (batPercent >   5) icon_array = battery_1_bar_90deg_32x32;
    else                       icon_array = battery_alert_90deg_32x32;
    display->drawBitmap(x, y, icon_array, 32, 32, GxEPD_WHITE, GxEPD_BLACK);
}

// 动态天气图标（根据QWeather图标代码和尺寸绘制，来自第二套代码，完整版）
void drawDynamicWeatherIcon(int x, int y, String iconCodeStr, int size) {
    const unsigned char* icon_array = nullptr;
    int code = iconCodeStr.toInt(); 
    
    if (size == 196) {
        // --- 匹配 196x196 巨型图标 ---
        switch (code) {
            case 100: icon_array = qweather_100_196x196; break;
            case 101: icon_array = qweather_101_196x196; break;
            case 102: icon_array = qweather_102_196x196; break;
            case 103: icon_array = qweather_103_196x196; break;
            case 104: icon_array = qweather_104_196x196; break;
            case 150: icon_array = qweather_150_196x196; break;
            case 151: icon_array = qweather_151_196x196; break;
            case 152: icon_array = qweather_152_196x196; break;
            case 153: icon_array = qweather_153_196x196; break;
            case 300: icon_array = qweather_300_196x196; break;
            case 301: icon_array = qweather_301_196x196; break;
            case 302: icon_array = qweather_302_196x196; break;
            case 303: icon_array = qweather_303_196x196; break;
            case 304: icon_array = qweather_304_196x196; break;
            case 305: icon_array = qweather_305_196x196; break;
            case 306: icon_array = qweather_306_196x196; break;
            case 307: icon_array = qweather_307_196x196; break;
            case 308: icon_array = qweather_308_196x196; break;
            case 309: icon_array = qweather_309_196x196; break;
            case 310: icon_array = qweather_310_196x196; break;
            case 311: icon_array = qweather_311_196x196; break;
            case 312: icon_array = qweather_312_196x196; break;
            case 313: icon_array = qweather_313_196x196; break;
            case 314: icon_array = qweather_314_196x196; break;
            case 315: icon_array = qweather_315_196x196; break;
            case 316: icon_array = qweather_316_196x196; break;
            case 317: icon_array = qweather_317_196x196; break;
            case 318: icon_array = qweather_318_196x196; break;
            case 350: icon_array = qweather_350_196x196; break;
            case 351: icon_array = qweather_351_196x196; break;
            case 399: icon_array = qweather_399_196x196; break;
            case 400: icon_array = qweather_400_196x196; break;
            case 401: icon_array = qweather_401_196x196; break;
            case 402: icon_array = qweather_402_196x196; break;
            case 403: icon_array = qweather_403_196x196; break;
            case 404: icon_array = qweather_404_196x196; break;
            case 405: icon_array = qweather_405_196x196; break;
            case 406: icon_array = qweather_406_196x196; break;
            case 407: icon_array = qweather_407_196x196; break;
            case 408: icon_array = qweather_408_196x196; break;
            case 409: icon_array = qweather_409_196x196; break;
            case 410: icon_array = qweather_410_196x196; break;
            case 456: icon_array = qweather_456_196x196; break;
            case 457: icon_array = qweather_457_196x196; break;
            case 499: icon_array = qweather_499_196x196; break;
            case 500: icon_array = qweather_500_196x196; break;
            case 501: icon_array = qweather_501_196x196; break;
            case 502: icon_array = qweather_502_196x196; break;
            case 503: icon_array = qweather_503_196x196; break;
            case 504: icon_array = qweather_504_196x196; break;
            case 507: icon_array = qweather_507_196x196; break;
            case 508: icon_array = qweather_508_196x196; break;
            case 509: icon_array = qweather_509_196x196; break;
            case 510: icon_array = qweather_510_196x196; break;
            case 511: icon_array = qweather_511_196x196; break;
            case 512: icon_array = qweather_512_196x196; break;
            case 513: icon_array = qweather_513_196x196; break;
            case 514: icon_array = qweather_514_196x196; break;
            case 515: icon_array = qweather_515_196x196; break;
            case 800: icon_array = qweather_800_196x196; break;
            case 801: icon_array = qweather_801_196x196; break;
            case 802: icon_array = qweather_802_196x196; break;
            case 803: icon_array = qweather_803_196x196; break;
            case 804: icon_array = qweather_804_196x196; break;
            case 805: icon_array = qweather_805_196x196; break;
            case 806: icon_array = qweather_806_196x196; break;
            case 807: icon_array = qweather_807_196x196; break;
            case 900: icon_array = qweather_900_196x196; break;
            case 901: icon_array = qweather_901_196x196; break;
            case 999: icon_array = qweather_999_196x196; break;
            case 1001: icon_array = qweather_1001_196x196; break;
            case 1002: icon_array = qweather_1002_196x196; break;
            case 1003: icon_array = qweather_1003_196x196; break;
            case 1004: icon_array = qweather_1004_196x196; break;
            case 1005: icon_array = qweather_1005_196x196; break;
            case 1006: icon_array = qweather_1006_196x196; break;
            case 1007: icon_array = qweather_1007_196x196; break;
            case 1008: icon_array = qweather_1008_196x196; break;
            case 1009: icon_array = qweather_1009_196x196; break;
            case 1010: icon_array = qweather_1010_196x196; break;
            case 1011: icon_array = qweather_1011_196x196; break;
            case 1012: icon_array = qweather_1012_196x196; break;
            case 1013: icon_array = qweather_1013_196x196; break;
            case 1014: icon_array = qweather_1014_196x196; break;
            case 1015: icon_array = qweather_1015_196x196; break;
            case 1016: icon_array = qweather_1016_196x196; break;
            case 1017: icon_array = qweather_1017_196x196; break;
            case 1018: icon_array = qweather_1018_196x196; break;
            case 1019: icon_array = qweather_1019_196x196; break;
            case 1020: icon_array = qweather_1020_196x196; break;
            case 1021: icon_array = qweather_1021_196x196; break;
            case 1022: icon_array = qweather_1022_196x196; break;
            case 1023: icon_array = qweather_1023_196x196; break;
            case 1024: icon_array = qweather_1024_196x196; break;
            case 1025: icon_array = qweather_1025_196x196; break;
            case 1026: icon_array = qweather_1026_196x196; break;
            case 1027: icon_array = qweather_1027_196x196; break;
            case 1028: icon_array = qweather_1028_196x196; break;
            case 1029: icon_array = qweather_1029_196x196; break;
            case 1030: icon_array = qweather_1030_196x196; break;
            case 1031: icon_array = qweather_1031_196x196; break;
            case 1032: icon_array = qweather_1032_196x196; break;
            case 1033: icon_array = qweather_1033_196x196; break;
            case 1034: icon_array = qweather_1034_196x196; break;
            case 1035: icon_array = qweather_1035_196x196; break;
            case 1036: icon_array = qweather_1036_196x196; break;
            case 1037: icon_array = qweather_1037_196x196; break;
            case 1038: icon_array = qweather_1038_196x196; break;
            case 1039: icon_array = qweather_1039_196x196; break;
            case 1040: icon_array = qweather_1040_196x196; break;
            case 1041: icon_array = qweather_1041_196x196; break;
            case 1042: icon_array = qweather_1042_196x196; break;
            case 1043: icon_array = qweather_1043_196x196; break;
            case 1044: icon_array = qweather_1044_196x196; break;
            case 1045: icon_array = qweather_1045_196x196; break;
            case 1046: icon_array = qweather_1046_196x196; break;
            case 1047: icon_array = qweather_1047_196x196; break;
            case 1048: icon_array = qweather_1048_196x196; break;
            case 1049: icon_array = qweather_1049_196x196; break;
            case 1050: icon_array = qweather_1050_196x196; break;
            case 1051: icon_array = qweather_1051_196x196; break;
            case 1052: icon_array = qweather_1052_196x196; break;
            case 1053: icon_array = qweather_1053_196x196; break;
            case 1054: icon_array = qweather_1054_196x196; break;
            case 1055: icon_array = qweather_1055_196x196; break;
            case 1056: icon_array = qweather_1056_196x196; break;
            case 1057: icon_array = qweather_1057_196x196; break;
            case 1058: icon_array = qweather_1058_196x196; break;
            case 1059: icon_array = qweather_1059_196x196; break;
            case 1060: icon_array = qweather_1060_196x196; break;
            case 1061: icon_array = qweather_1061_196x196; break;
            case 1062: icon_array = qweather_1062_196x196; break;
            case 1063: icon_array = qweather_1063_196x196; break;
            case 1064: icon_array = qweather_1064_196x196; break;
            case 1065: icon_array = qweather_1065_196x196; break;
            case 1066: icon_array = qweather_1066_196x196; break;
            case 1067: icon_array = qweather_1067_196x196; break;
            case 1068: icon_array = qweather_1068_196x196; break;
            case 1069: icon_array = qweather_1069_196x196; break;
            case 1071: icon_array = qweather_1071_196x196; break;
            case 1072: icon_array = qweather_1072_196x196; break;
            case 1073: icon_array = qweather_1073_196x196; break;
            case 1074: icon_array = qweather_1074_196x196; break;
            case 1075: icon_array = qweather_1075_196x196; break;
            case 1076: icon_array = qweather_1076_196x196; break;
            case 1077: icon_array = qweather_1077_196x196; break;
            case 1078: icon_array = qweather_1078_196x196; break;
            case 1079: icon_array = qweather_1079_196x196; break;
            case 1080: icon_array = qweather_1080_196x196; break;
            case 1081: icon_array = qweather_1081_196x196; break;
            case 1082: icon_array = qweather_1082_196x196; break;
            case 1084: icon_array = qweather_1084_196x196; break;
            case 1085: icon_array = qweather_1085_196x196; break;
            case 1086: icon_array = qweather_1086_196x196; break;
            case 1087: icon_array = qweather_1087_196x196; break;
            case 1088: icon_array = qweather_1088_196x196; break;
            case 1089: icon_array = qweather_1089_196x196; break;
            case 1201: icon_array = qweather_1201_196x196; break;
            case 1202: icon_array = qweather_1202_196x196; break;
            case 1203: icon_array = qweather_1203_196x196; break;
            case 1204: icon_array = qweather_1204_196x196; break;
            case 1205: icon_array = qweather_1205_196x196; break;
            case 1206: icon_array = qweather_1206_196x196; break;
            case 1207: icon_array = qweather_1207_196x196; break;
            case 1208: icon_array = qweather_1208_196x196; break;
            case 1209: icon_array = qweather_1209_196x196; break;
            case 1210: icon_array = qweather_1210_196x196; break;
            case 1211: icon_array = qweather_1211_196x196; break;
            case 1212: icon_array = qweather_1212_196x196; break;
            case 1213: icon_array = qweather_1213_196x196; break;
            case 1214: icon_array = qweather_1214_196x196; break;
            case 1215: icon_array = qweather_1215_196x196; break;
            case 1216: icon_array = qweather_1216_196x196; break;
            case 1217: icon_array = qweather_1217_196x196; break;
            case 1218: icon_array = qweather_1218_196x196; break;
            case 1219: icon_array = qweather_1219_196x196; break;
            case 1221: icon_array = qweather_1221_196x196; break;
            case 1241: icon_array = qweather_1241_196x196; break;
            case 1242: icon_array = qweather_1242_196x196; break;
            case 1243: icon_array = qweather_1243_196x196; break;
            case 1244: icon_array = qweather_1244_196x196; break;
            case 1245: icon_array = qweather_1245_196x196; break;
            case 1246: icon_array = qweather_1246_196x196; break;
            case 1247: icon_array = qweather_1247_196x196; break;
            case 1248: icon_array = qweather_1248_196x196; break;
            case 1249: icon_array = qweather_1249_196x196; break;
            case 1250: icon_array = qweather_1250_196x196; break;
            case 1251: icon_array = qweather_1251_196x196; break;
            case 1271: icon_array = qweather_1271_196x196; break;
            case 1272: icon_array = qweather_1272_196x196; break;
            case 1273: icon_array = qweather_1273_196x196; break;
            case 1274: icon_array = qweather_1274_196x196; break;
            case 1601: icon_array = qweather_1601_196x196; break;
            case 1602: icon_array = qweather_1602_196x196; break;
            case 1603: icon_array = qweather_1603_196x196; break;
            case 1604: icon_array = qweather_1604_196x196; break;
            case 1605: icon_array = qweather_1605_196x196; break;
            case 1606: icon_array = qweather_1606_196x196; break;
            case 1607: icon_array = qweather_1607_196x196; break;
            case 1608: icon_array = qweather_1608_196x196; break;
            case 1609: icon_array = qweather_1609_196x196; break;
            case 1610: icon_array = qweather_1610_196x196; break;
            case 1701: icon_array = qweather_1701_196x196; break;
            case 1702: icon_array = qweather_1702_196x196; break;
            case 1703: icon_array = qweather_1703_196x196; break;
            case 1704: icon_array = qweather_1704_196x196; break;
            case 1705: icon_array = qweather_1705_196x196; break;
            case 1706: icon_array = qweather_1706_196x196; break;
            case 1707: icon_array = qweather_1707_196x196; break;
            case 1708: icon_array = qweather_1708_196x196; break;
            case 1709: icon_array = qweather_1709_196x196; break;
            case 1710: icon_array = qweather_1710_196x196; break;
            case 1801: icon_array = qweather_1801_196x196; break;
            case 1802: icon_array = qweather_1802_196x196; break;
            case 1803: icon_array = qweather_1803_196x196; break;
            case 1804: icon_array = qweather_1804_196x196; break;
            case 1805: icon_array = qweather_1805_196x196; break;
            case 2001: icon_array = qweather_2001_196x196; break;
            case 2002: icon_array = qweather_2002_196x196; break;
            case 2003: icon_array = qweather_2003_196x196; break;
            case 2004: icon_array = qweather_2004_196x196; break;
            case 2005: icon_array = qweather_2005_196x196; break;
            case 2006: icon_array = qweather_2006_196x196; break;
            case 2007: icon_array = qweather_2007_196x196; break;
            case 2029: icon_array = qweather_2029_196x196; break;
            case 2030: icon_array = qweather_2030_196x196; break;
            case 2031: icon_array = qweather_2031_196x196; break;
            case 2032: icon_array = qweather_2032_196x196; break;
            case 2033: icon_array = qweather_2033_196x196; break;
            case 2050: icon_array = qweather_2050_196x196; break;
            case 2051: icon_array = qweather_2051_196x196; break;
            case 2052: icon_array = qweather_2052_196x196; break;
            case 2053: icon_array = qweather_2053_196x196; break;
            case 2054: icon_array = qweather_2054_196x196; break;
            case 2070: icon_array = qweather_2070_196x196; break;
            case 2071: icon_array = qweather_2071_196x196; break;
            case 2072: icon_array = qweather_2072_196x196; break;
            case 2073: icon_array = qweather_2073_196x196; break;
            case 2074: icon_array = qweather_2074_196x196; break;
            case 2075: icon_array = qweather_2075_196x196; break;
            case 2076: icon_array = qweather_2076_196x196; break;
            case 2077: icon_array = qweather_2077_196x196; break;
            case 2078: icon_array = qweather_2078_196x196; break;
            case 2079: icon_array = qweather_2079_196x196; break;
            case 2080: icon_array = qweather_2080_196x196; break;
            case 2081: icon_array = qweather_2081_196x196; break;
            case 2082: icon_array = qweather_2082_196x196; break;
            case 2083: icon_array = qweather_2083_196x196; break;
            case 2084: icon_array = qweather_2084_196x196; break;
            case 2085: icon_array = qweather_2085_196x196; break;
            case 2100: icon_array = qweather_2100_196x196; break;
            case 2101: icon_array = qweather_2101_196x196; break;
            case 2102: icon_array = qweather_2102_196x196; break;
            case 2103: icon_array = qweather_2103_196x196; break;
            case 2104: icon_array = qweather_2104_196x196; break;
            case 2105: icon_array = qweather_2105_196x196; break;
            case 2106: icon_array = qweather_2106_196x196; break;
            case 2107: icon_array = qweather_2107_196x196; break;
            case 2108: icon_array = qweather_2108_196x196; break;
            case 2109: icon_array = qweather_2109_196x196; break;
            case 2111: icon_array = qweather_2111_196x196; break;
            case 2120: icon_array = qweather_2120_196x196; break;
            case 2121: icon_array = qweather_2121_196x196; break;
            case 2122: icon_array = qweather_2122_196x196; break;
            case 2123: icon_array = qweather_2123_196x196; break;
            case 2124: icon_array = qweather_2124_196x196; break;
            case 2125: icon_array = qweather_2125_196x196; break;
            case 2126: icon_array = qweather_2126_196x196; break;
            case 2127: icon_array = qweather_2127_196x196; break;
            case 2128: icon_array = qweather_2128_196x196; break;
            case 2129: icon_array = qweather_2129_196x196; break;
            case 2130: icon_array = qweather_2130_196x196; break;
            case 2131: icon_array = qweather_2131_196x196; break;
            case 2132: icon_array = qweather_2132_196x196; break;
            case 2133: icon_array = qweather_2133_196x196; break;
            case 2134: icon_array = qweather_2134_196x196; break;
            case 2135: icon_array = qweather_2135_196x196; break;
            case 2150: icon_array = qweather_2150_196x196; break;
            case 2151: icon_array = qweather_2151_196x196; break;
            case 2152: icon_array = qweather_2152_196x196; break;
            case 2153: icon_array = qweather_2153_196x196; break;
            case 2154: icon_array = qweather_2154_196x196; break;
            case 2155: icon_array = qweather_2155_196x196; break;
            case 2156: icon_array = qweather_2156_196x196; break;
            case 2157: icon_array = qweather_2157_196x196; break;
            case 2158: icon_array = qweather_2158_196x196; break;
            case 2159: icon_array = qweather_2159_196x196; break;
            case 2160: icon_array = qweather_2160_196x196; break;
            case 2161: icon_array = qweather_2161_196x196; break;
            case 2162: icon_array = qweather_2162_196x196; break;
            case 2163: icon_array = qweather_2163_196x196; break;
            case 2164: icon_array = qweather_2164_196x196; break;
            case 2165: icon_array = qweather_2165_196x196; break;
            case 2166: icon_array = qweather_2166_196x196; break;
            case 2167: icon_array = qweather_2167_196x196; break;
            case 2190: icon_array = qweather_2190_196x196; break;
            case 2191: icon_array = qweather_2191_196x196; break;
            case 2192: icon_array = qweather_2192_196x196; break;
            case 2193: icon_array = qweather_2193_196x196; break;
            case 2200: icon_array = qweather_2200_196x196; break;
            case 2201: icon_array = qweather_2201_196x196; break;
            case 2202: icon_array = qweather_2202_196x196; break;
            case 2203: icon_array = qweather_2203_196x196; break;
            case 2204: icon_array = qweather_2204_196x196; break;
            case 2205: icon_array = qweather_2205_196x196; break;
            case 2207: icon_array = qweather_2207_196x196; break;
            case 2208: icon_array = qweather_2208_196x196; break;
            case 2209: icon_array = qweather_2209_196x196; break;
            case 2210: icon_array = qweather_2210_196x196; break;
            case 2211: icon_array = qweather_2211_196x196; break;
            case 2212: icon_array = qweather_2212_196x196; break;
            case 2213: icon_array = qweather_2213_196x196; break;
            case 2214: icon_array = qweather_2214_196x196; break;
            case 2215: icon_array = qweather_2215_196x196; break;
            case 2216: icon_array = qweather_2216_196x196; break;
            case 2217: icon_array = qweather_2217_196x196; break;
            case 2218: icon_array = qweather_2218_196x196; break;
            case 2300: icon_array = qweather_2300_196x196; break;
            case 2301: icon_array = qweather_2301_196x196; break;
            case 2302: icon_array = qweather_2302_196x196; break;
            case 2303: icon_array = qweather_2303_196x196; break;
            case 2304: icon_array = qweather_2304_196x196; break;
            case 2305: icon_array = qweather_2305_196x196; break;
            case 2306: icon_array = qweather_2306_196x196; break;
            case 2307: icon_array = qweather_2307_196x196; break;
            case 2308: icon_array = qweather_2308_196x196; break;
            case 2309: icon_array = qweather_2309_196x196; break;
            case 2311: icon_array = qweather_2311_196x196; break;
            case 2312: icon_array = qweather_2312_196x196; break;
            case 2313: icon_array = qweather_2313_196x196; break;
            case 2314: icon_array = qweather_2314_196x196; break;
            case 2315: icon_array = qweather_2315_196x196; break;
            case 2316: icon_array = qweather_2316_196x196; break;
            case 2317: icon_array = qweather_2317_196x196; break;
            case 2318: icon_array = qweather_2318_196x196; break;
            case 2319: icon_array = qweather_2319_196x196; break;
            case 2320: icon_array = qweather_2320_196x196; break;
            case 2321: icon_array = qweather_2321_196x196; break;
            case 2322: icon_array = qweather_2322_196x196; break;
            case 2323: icon_array = qweather_2323_196x196; break;
            case 2324: icon_array = qweather_2324_196x196; break;
            case 2325: icon_array = qweather_2325_196x196; break;
            case 2326: icon_array = qweather_2326_196x196; break;
            case 2327: icon_array = qweather_2327_196x196; break;
            case 2328: icon_array = qweather_2328_196x196; break;
            case 2330: icon_array = qweather_2330_196x196; break;
            case 2331: icon_array = qweather_2331_196x196; break;
            case 2332: icon_array = qweather_2332_196x196; break;
            case 2333: icon_array = qweather_2333_196x196; break;
            case 2341: icon_array = qweather_2341_196x196; break;
            case 2343: icon_array = qweather_2343_196x196; break;
            case 2345: icon_array = qweather_2345_196x196; break;
            case 2346: icon_array = qweather_2346_196x196; break;
            case 2348: icon_array = qweather_2348_196x196; break;
            case 2349: icon_array = qweather_2349_196x196; break;
            case 2350: icon_array = qweather_2350_196x196; break;
            case 2351: icon_array = qweather_2351_196x196; break;
            case 2352: icon_array = qweather_2352_196x196; break;
            case 2353: icon_array = qweather_2353_196x196; break;
            case 2354: icon_array = qweather_2354_196x196; break;
            case 2355: icon_array = qweather_2355_196x196; break;
            case 2356: icon_array = qweather_2356_196x196; break;
            case 2357: icon_array = qweather_2357_196x196; break;
            case 2358: icon_array = qweather_2358_196x196; break;
            case 2359: icon_array = qweather_2359_196x196; break;
            case 2360: icon_array = qweather_2360_196x196; break;
            case 2361: icon_array = qweather_2361_196x196; break;
            case 2362: icon_array = qweather_2362_196x196; break;
            case 2363: icon_array = qweather_2363_196x196; break;
            case 2364: icon_array = qweather_2364_196x196; break;
            case 2365: icon_array = qweather_2365_196x196; break;
            case 2366: icon_array = qweather_2366_196x196; break;
            case 2367: icon_array = qweather_2367_196x196; break;
            case 2368: icon_array = qweather_2368_196x196; break;
            case 2369: icon_array = qweather_2369_196x196; break;
            case 2370: icon_array = qweather_2370_196x196; break;
            case 2371: icon_array = qweather_2371_196x196; break;
            case 2372: icon_array = qweather_2372_196x196; break;
            case 2373: icon_array = qweather_2373_196x196; break;
            case 2374: icon_array = qweather_2374_196x196; break;
            case 2375: icon_array = qweather_2375_196x196; break;
            case 2376: icon_array = qweather_2376_196x196; break;
            case 2377: icon_array = qweather_2377_196x196; break;
            case 2378: icon_array = qweather_2378_196x196; break;
            case 2379: icon_array = qweather_2379_196x196; break;
            case 2380: icon_array = qweather_2380_196x196; break;
            case 2381: icon_array = qweather_2381_196x196; break;
            case 2382: icon_array = qweather_2382_196x196; break;
            case 2383: icon_array = qweather_2383_196x196; break;
            case 2384: icon_array = qweather_2384_196x196; break;
            case 2385: icon_array = qweather_2385_196x196; break;
            case 2386: icon_array = qweather_2386_196x196; break;
            case 2387: icon_array = qweather_2387_196x196; break;
            case 2388: icon_array = qweather_2388_196x196; break;
            case 2389: icon_array = qweather_2389_196x196; break;
            case 2390: icon_array = qweather_2390_196x196; break;
            case 2391: icon_array = qweather_2391_196x196; break;
            case 2392: icon_array = qweather_2392_196x196; break;
            case 2393: icon_array = qweather_2393_196x196; break;
            case 2394: icon_array = qweather_2394_196x196; break;
            case 2395: icon_array = qweather_2395_196x196; break;
            case 2396: icon_array = qweather_2396_196x196; break;
            case 2397: icon_array = qweather_2397_196x196; break;
            case 2398: icon_array = qweather_2398_196x196; break;
            case 2399: icon_array = qweather_2399_196x196; break;
            case 2400: icon_array = qweather_2400_196x196; break;
            case 2409: icon_array = qweather_2409_196x196; break;
            case 2411: icon_array = qweather_2411_196x196; break;
            case 2412: icon_array = qweather_2412_196x196; break;
            case 2413: icon_array = qweather_2413_196x196; break;
            case 2414: icon_array = qweather_2414_196x196; break;
            case 2415: icon_array = qweather_2415_196x196; break;
            case 2416: icon_array = qweather_2416_196x196; break;
            case 2417: icon_array = qweather_2417_196x196; break;
            case 2418: icon_array = qweather_2418_196x196; break;
            case 2419: icon_array = qweather_2419_196x196; break;
            case 2420: icon_array = qweather_2420_196x196; break;
            case 2421: icon_array = qweather_2421_196x196; break;
            case 2422: icon_array = qweather_2422_196x196; break;
            case 2423: icon_array = qweather_2423_196x196; break;
            case 2424: icon_array = qweather_2424_196x196; break;
            case 2425: icon_array = qweather_2425_196x196; break;
            case 2426: icon_array = qweather_2426_196x196; break;
            case 2501: icon_array = qweather_2501_196x196; break;
            case 2502: icon_array = qweather_2502_196x196; break;
            case 2521: icon_array = qweather_2521_196x196; break;
            case 2522: icon_array = qweather_2522_196x196; break;
            case 2523: icon_array = qweather_2523_196x196; break;
            case 2524: icon_array = qweather_2524_196x196; break;
            case 2525: icon_array = qweather_2525_196x196; break;
            case 2526: icon_array = qweather_2526_196x196; break;
            case 2527: icon_array = qweather_2527_196x196; break;
            case 2528: icon_array = qweather_2528_196x196; break;
            case 2529: icon_array = qweather_2529_196x196; break;
            case 2530: icon_array = qweather_2530_196x196; break;
            case 2531: icon_array = qweather_2531_196x196; break;
            case 2532: icon_array = qweather_2532_196x196; break;
            case 2550: icon_array = qweather_2550_196x196; break;
            case 2551: icon_array = qweather_2551_196x196; break;
            case 2552: icon_array = qweather_2552_196x196; break;
            case 2553: icon_array = qweather_2553_196x196; break;
            case 2554: icon_array = qweather_2554_196x196; break;
            case 9999: icon_array = qweather_9999_196x196; break;
            
            // 如果 API 返回的 code 没有对应的图标，使用缺省图标防止报错
            default:  icon_array = qweather_101_196x196; break; 
        }
    } 
    else if (size == 64) {
        // --- 匹配 64x64 预报小图标 ---
        switch (code) {
            case 100: icon_array = qweather_100_64x64; break;
            case 101: icon_array = qweather_101_64x64; break;
            case 102: icon_array = qweather_102_64x64; break;
            case 103: icon_array = qweather_103_64x64; break;
            case 104: icon_array = qweather_104_64x64; break;
            case 150: icon_array = qweather_150_64x64; break;
            case 151: icon_array = qweather_151_64x64; break;
            case 152: icon_array = qweather_152_64x64; break;
            case 153: icon_array = qweather_153_64x64; break;
            case 300: icon_array = qweather_300_64x64; break;
            case 301: icon_array = qweather_301_64x64; break;
            case 302: icon_array = qweather_302_64x64; break;
            case 303: icon_array = qweather_303_64x64; break;
            case 304: icon_array = qweather_304_64x64; break;
            case 305: icon_array = qweather_305_64x64; break;
            case 306: icon_array = qweather_306_64x64; break;
            case 307: icon_array = qweather_307_64x64; break;
            case 308: icon_array = qweather_308_64x64; break;
            case 309: icon_array = qweather_309_64x64; break;
            case 310: icon_array = qweather_310_64x64; break;
            case 311: icon_array = qweather_311_64x64; break;
            case 312: icon_array = qweather_312_64x64; break;
            case 313: icon_array = qweather_313_64x64; break;
            case 314: icon_array = qweather_314_64x64; break;
            case 315: icon_array = qweather_315_64x64; break;
            case 316: icon_array = qweather_316_64x64; break;
            case 317: icon_array = qweather_317_64x64; break;
            case 318: icon_array = qweather_318_64x64; break;
            case 350: icon_array = qweather_350_64x64; break;
            case 351: icon_array = qweather_351_64x64; break;
            case 399: icon_array = qweather_399_64x64; break;
            case 400: icon_array = qweather_400_64x64; break;
            case 401: icon_array = qweather_401_64x64; break;
            case 402: icon_array = qweather_402_64x64; break;
            case 403: icon_array = qweather_403_64x64; break;
            case 404: icon_array = qweather_404_64x64; break;
            case 405: icon_array = qweather_405_64x64; break;
            case 406: icon_array = qweather_406_64x64; break;
            case 407: icon_array = qweather_407_64x64; break;
            case 408: icon_array = qweather_408_64x64; break;
            case 409: icon_array = qweather_409_64x64; break;
            case 410: icon_array = qweather_410_64x64; break;
            case 456: icon_array = qweather_456_64x64; break;
            case 457: icon_array = qweather_457_64x64; break;
            case 499: icon_array = qweather_499_64x64; break;
            case 500: icon_array = qweather_500_64x64; break;
            case 501: icon_array = qweather_501_64x64; break;
            case 502: icon_array = qweather_502_64x64; break;
            case 503: icon_array = qweather_503_64x64; break;
            case 504: icon_array = qweather_504_64x64; break;
            case 507: icon_array = qweather_507_64x64; break;
            case 508: icon_array = qweather_508_64x64; break;
            case 509: icon_array = qweather_509_64x64; break;
            case 510: icon_array = qweather_510_64x64; break;
            case 511: icon_array = qweather_511_64x64; break;
            case 512: icon_array = qweather_512_64x64; break;
            case 513: icon_array = qweather_513_64x64; break;
            case 514: icon_array = qweather_514_64x64; break;
            case 515: icon_array = qweather_515_64x64; break;
            case 800: icon_array = qweather_800_64x64; break;
            case 801: icon_array = qweather_801_64x64; break;
            case 802: icon_array = qweather_802_64x64; break;
            case 803: icon_array = qweather_803_64x64; break;
            case 804: icon_array = qweather_804_64x64; break;
            case 805: icon_array = qweather_805_64x64; break;
            case 806: icon_array = qweather_806_64x64; break;
            case 807: icon_array = qweather_807_64x64; break;
            case 900: icon_array = qweather_900_64x64; break;
            case 901: icon_array = qweather_901_64x64; break;
            case 999: icon_array = qweather_999_64x64; break;
            case 1001: icon_array = qweather_1001_64x64; break;
            case 1002: icon_array = qweather_1002_64x64; break;
            case 1003: icon_array = qweather_1003_64x64; break;
            case 1004: icon_array = qweather_1004_64x64; break;
            case 1005: icon_array = qweather_1005_64x64; break;
            case 1006: icon_array = qweather_1006_64x64; break;
            case 1007: icon_array = qweather_1007_64x64; break;
            case 1008: icon_array = qweather_1008_64x64; break;
            case 1009: icon_array = qweather_1009_64x64; break;
            case 1010: icon_array = qweather_1010_64x64; break;
            case 1011: icon_array = qweather_1011_64x64; break;
            case 1012: icon_array = qweather_1012_64x64; break;
            case 1013: icon_array = qweather_1013_64x64; break;
            case 1014: icon_array = qweather_1014_64x64; break;
            case 1015: icon_array = qweather_1015_64x64; break;
            case 1016: icon_array = qweather_1016_64x64; break;
            case 1017: icon_array = qweather_1017_64x64; break;
            case 1018: icon_array = qweather_1018_64x64; break;
            case 1019: icon_array = qweather_1019_64x64; break;
            case 1020: icon_array = qweather_1020_64x64; break;
            case 1021: icon_array = qweather_1021_64x64; break;
            case 1022: icon_array = qweather_1022_64x64; break;
            case 1023: icon_array = qweather_1023_64x64; break;
            case 1024: icon_array = qweather_1024_64x64; break;
            case 1025: icon_array = qweather_1025_64x64; break;
            case 1026: icon_array = qweather_1026_64x64; break;
            case 1027: icon_array = qweather_1027_64x64; break;
            case 1028: icon_array = qweather_1028_64x64; break;
            case 1029: icon_array = qweather_1029_64x64; break;
            case 1030: icon_array = qweather_1030_64x64; break;
            case 1031: icon_array = qweather_1031_64x64; break;
            case 1032: icon_array = qweather_1032_64x64; break;
            case 1033: icon_array = qweather_1033_64x64; break;
            case 1034: icon_array = qweather_1034_64x64; break;
            case 1035: icon_array = qweather_1035_64x64; break;
            case 1036: icon_array = qweather_1036_64x64; break;
            case 1037: icon_array = qweather_1037_64x64; break;
            case 1038: icon_array = qweather_1038_64x64; break;
            case 1039: icon_array = qweather_1039_64x64; break;
            case 1040: icon_array = qweather_1040_64x64; break;
            case 1041: icon_array = qweather_1041_64x64; break;
            case 1042: icon_array = qweather_1042_64x64; break;
            case 1043: icon_array = qweather_1043_64x64; break;
            case 1044: icon_array = qweather_1044_64x64; break;
            case 1045: icon_array = qweather_1045_64x64; break;
            case 1046: icon_array = qweather_1046_64x64; break;
            case 1047: icon_array = qweather_1047_64x64; break;
            case 1048: icon_array = qweather_1048_64x64; break;
            case 1049: icon_array = qweather_1049_64x64; break;
            case 1050: icon_array = qweather_1050_64x64; break;
            case 1051: icon_array = qweather_1051_64x64; break;
            case 1052: icon_array = qweather_1052_64x64; break;
            case 1053: icon_array = qweather_1053_64x64; break;
            case 1054: icon_array = qweather_1054_64x64; break;
            case 1055: icon_array = qweather_1055_64x64; break;
            case 1056: icon_array = qweather_1056_64x64; break;
            case 1057: icon_array = qweather_1057_64x64; break;
            case 1058: icon_array = qweather_1058_64x64; break;
            case 1059: icon_array = qweather_1059_64x64; break;
            case 1060: icon_array = qweather_1060_64x64; break;
            case 1061: icon_array = qweather_1061_64x64; break;
            case 1062: icon_array = qweather_1062_64x64; break;
            case 1063: icon_array = qweather_1063_64x64; break;
            case 1064: icon_array = qweather_1064_64x64; break;
            case 1065: icon_array = qweather_1065_64x64; break;
            case 1066: icon_array = qweather_1066_64x64; break;
            case 1067: icon_array = qweather_1067_64x64; break;
            case 1068: icon_array = qweather_1068_64x64; break;
            case 1069: icon_array = qweather_1069_64x64; break;
            case 1071: icon_array = qweather_1071_64x64; break;
            case 1072: icon_array = qweather_1072_64x64; break;
            case 1073: icon_array = qweather_1073_64x64; break;
            case 1074: icon_array = qweather_1074_64x64; break;
            case 1075: icon_array = qweather_1075_64x64; break;
            case 1076: icon_array = qweather_1076_64x64; break;
            case 1077: icon_array = qweather_1077_64x64; break;
            case 1078: icon_array = qweather_1078_64x64; break;
            case 1079: icon_array = qweather_1079_64x64; break;
            case 1080: icon_array = qweather_1080_64x64; break;
            case 1081: icon_array = qweather_1081_64x64; break;
            case 1082: icon_array = qweather_1082_64x64; break;
            case 1084: icon_array = qweather_1084_64x64; break;
            case 1085: icon_array = qweather_1085_64x64; break;
            case 1086: icon_array = qweather_1086_64x64; break;
            case 1087: icon_array = qweather_1087_64x64; break;
            case 1088: icon_array = qweather_1088_64x64; break;
            case 1089: icon_array = qweather_1089_64x64; break;
            case 1201: icon_array = qweather_1201_64x64; break;
            case 1202: icon_array = qweather_1202_64x64; break;
            case 1203: icon_array = qweather_1203_64x64; break;
            case 1204: icon_array = qweather_1204_64x64; break;
            case 1205: icon_array = qweather_1205_64x64; break;
            case 1206: icon_array = qweather_1206_64x64; break;
            case 1207: icon_array = qweather_1207_64x64; break;
            case 1208: icon_array = qweather_1208_64x64; break;
            case 1209: icon_array = qweather_1209_64x64; break;
            case 1210: icon_array = qweather_1210_64x64; break;
            case 1211: icon_array = qweather_1211_64x64; break;
            case 1212: icon_array = qweather_1212_64x64; break;
            case 1213: icon_array = qweather_1213_64x64; break;
            case 1214: icon_array = qweather_1214_64x64; break;
            case 1215: icon_array = qweather_1215_64x64; break;
            case 1216: icon_array = qweather_1216_64x64; break;
            case 1217: icon_array = qweather_1217_64x64; break;
            case 1218: icon_array = qweather_1218_64x64; break;
            case 1219: icon_array = qweather_1219_64x64; break;
            case 1221: icon_array = qweather_1221_64x64; break;
            case 1241: icon_array = qweather_1241_64x64; break;
            case 1242: icon_array = qweather_1242_64x64; break;
            case 1243: icon_array = qweather_1243_64x64; break;
            case 1244: icon_array = qweather_1244_64x64; break;
            case 1245: icon_array = qweather_1245_64x64; break;
            case 1246: icon_array = qweather_1246_64x64; break;
            case 1247: icon_array = qweather_1247_64x64; break;
            case 1248: icon_array = qweather_1248_64x64; break;
            case 1249: icon_array = qweather_1249_64x64; break;
            case 1250: icon_array = qweather_1250_64x64; break;
            case 1251: icon_array = qweather_1251_64x64; break;
            case 1271: icon_array = qweather_1271_64x64; break;
            case 1272: icon_array = qweather_1272_64x64; break;
            case 1273: icon_array = qweather_1273_64x64; break;
            case 1274: icon_array = qweather_1274_64x64; break;
            case 1601: icon_array = qweather_1601_64x64; break;
            case 1602: icon_array = qweather_1602_64x64; break;
            case 1603: icon_array = qweather_1603_64x64; break;
            case 1604: icon_array = qweather_1604_64x64; break;
            case 1605: icon_array = qweather_1605_64x64; break;
            case 1606: icon_array = qweather_1606_64x64; break;
            case 1607: icon_array = qweather_1607_64x64; break;
            case 1608: icon_array = qweather_1608_64x64; break;
            case 1609: icon_array = qweather_1609_64x64; break;
            case 1610: icon_array = qweather_1610_64x64; break;
            case 1701: icon_array = qweather_1701_64x64; break;
            case 1702: icon_array = qweather_1702_64x64; break;
            case 1703: icon_array = qweather_1703_64x64; break;
            case 1704: icon_array = qweather_1704_64x64; break;
            case 1705: icon_array = qweather_1705_64x64; break;
            case 1706: icon_array = qweather_1706_64x64; break;
            case 1707: icon_array = qweather_1707_64x64; break;
            case 1708: icon_array = qweather_1708_64x64; break;
            case 1709: icon_array = qweather_1709_64x64; break;
            case 1710: icon_array = qweather_1710_64x64; break;
            case 1801: icon_array = qweather_1801_64x64; break;
            case 1802: icon_array = qweather_1802_64x64; break;
            case 1803: icon_array = qweather_1803_64x64; break;
            case 1804: icon_array = qweather_1804_64x64; break;
            case 1805: icon_array = qweather_1805_64x64; break;
            case 2001: icon_array = qweather_2001_64x64; break;
            case 2002: icon_array = qweather_2002_64x64; break;
            case 2003: icon_array = qweather_2003_64x64; break;
            case 2004: icon_array = qweather_2004_64x64; break;
            case 2005: icon_array = qweather_2005_64x64; break;
            case 2006: icon_array = qweather_2006_64x64; break;
            case 2007: icon_array = qweather_2007_64x64; break;
            case 2029: icon_array = qweather_2029_64x64; break;
            case 2030: icon_array = qweather_2030_64x64; break;
            case 2031: icon_array = qweather_2031_64x64; break;
            case 2032: icon_array = qweather_2032_64x64; break;
            case 2033: icon_array = qweather_2033_64x64; break;
            case 2050: icon_array = qweather_2050_64x64; break;
            case 2051: icon_array = qweather_2051_64x64; break;
            case 2052: icon_array = qweather_2052_64x64; break;
            case 2053: icon_array = qweather_2053_64x64; break;
            case 2054: icon_array = qweather_2054_64x64; break;
            case 2070: icon_array = qweather_2070_64x64; break;
            case 2071: icon_array = qweather_2071_64x64; break;
            case 2072: icon_array = qweather_2072_64x64; break;
            case 2073: icon_array = qweather_2073_64x64; break;
            case 2074: icon_array = qweather_2074_64x64; break;
            case 2075: icon_array = qweather_2075_64x64; break;
            case 2076: icon_array = qweather_2076_64x64; break;
            case 2077: icon_array = qweather_2077_64x64; break;
            case 2078: icon_array = qweather_2078_64x64; break;
            case 2079: icon_array = qweather_2079_64x64; break;
            case 2080: icon_array = qweather_2080_64x64; break;
            case 2081: icon_array = qweather_2081_64x64; break;
            case 2082: icon_array = qweather_2082_64x64; break;
            case 2083: icon_array = qweather_2083_64x64; break;
            case 2084: icon_array = qweather_2084_64x64; break;
            case 2085: icon_array = qweather_2085_64x64; break;
            case 2100: icon_array = qweather_2100_64x64; break;
            case 2101: icon_array = qweather_2101_64x64; break;
            case 2102: icon_array = qweather_2102_64x64; break;
            case 2103: icon_array = qweather_2103_64x64; break;
            case 2104: icon_array = qweather_2104_64x64; break;
            case 2105: icon_array = qweather_2105_64x64; break;
            case 2106: icon_array = qweather_2106_64x64; break;
            case 2107: icon_array = qweather_2107_64x64; break;
            case 2108: icon_array = qweather_2108_64x64; break;
            case 2109: icon_array = qweather_2109_64x64; break;
            case 2111: icon_array = qweather_2111_64x64; break;
            case 2120: icon_array = qweather_2120_64x64; break;
            case 2121: icon_array = qweather_2121_64x64; break;
            case 2122: icon_array = qweather_2122_64x64; break;
            case 2123: icon_array = qweather_2123_64x64; break;
            case 2124: icon_array = qweather_2124_64x64; break;
            case 2125: icon_array = qweather_2125_64x64; break;
            case 2126: icon_array = qweather_2126_64x64; break;
            case 2127: icon_array = qweather_2127_64x64; break;
            case 2128: icon_array = qweather_2128_64x64; break;
            case 2129: icon_array = qweather_2129_64x64; break;
            case 2130: icon_array = qweather_2130_64x64; break;
            case 2131: icon_array = qweather_2131_64x64; break;
            case 2132: icon_array = qweather_2132_64x64; break;
            case 2133: icon_array = qweather_2133_64x64; break;
            case 2134: icon_array = qweather_2134_64x64; break;
            case 2135: icon_array = qweather_2135_64x64; break;
            case 2150: icon_array = qweather_2150_64x64; break;
            case 2151: icon_array = qweather_2151_64x64; break;
            case 2152: icon_array = qweather_2152_64x64; break;
            case 2153: icon_array = qweather_2153_64x64; break;
            case 2154: icon_array = qweather_2154_64x64; break;
            case 2155: icon_array = qweather_2155_64x64; break;
            case 2156: icon_array = qweather_2156_64x64; break;
            case 2157: icon_array = qweather_2157_64x64; break;
            case 2158: icon_array = qweather_2158_64x64; break;
            case 2159: icon_array = qweather_2159_64x64; break;
            case 2160: icon_array = qweather_2160_64x64; break;
            case 2161: icon_array = qweather_2161_64x64; break;
            case 2162: icon_array = qweather_2162_64x64; break;
            case 2163: icon_array = qweather_2163_64x64; break;
            case 2164: icon_array = qweather_2164_64x64; break;
            case 2165: icon_array = qweather_2165_64x64; break;
            case 2166: icon_array = qweather_2166_64x64; break;
            case 2167: icon_array = qweather_2167_64x64; break;
            case 2190: icon_array = qweather_2190_64x64; break;
            case 2191: icon_array = qweather_2191_64x64; break;
            case 2192: icon_array = qweather_2192_64x64; break;
            case 2193: icon_array = qweather_2193_64x64; break;
            case 2200: icon_array = qweather_2200_64x64; break;
            case 2201: icon_array = qweather_2201_64x64; break;
            case 2202: icon_array = qweather_2202_64x64; break;
            case 2203: icon_array = qweather_2203_64x64; break;
            case 2204: icon_array = qweather_2204_64x64; break;
            case 2205: icon_array = qweather_2205_64x64; break;
            case 2207: icon_array = qweather_2207_64x64; break;
            case 2208: icon_array = qweather_2208_64x64; break;
            case 2209: icon_array = qweather_2209_64x64; break;
            case 2210: icon_array = qweather_2210_64x64; break;
            case 2211: icon_array = qweather_2211_64x64; break;
            case 2212: icon_array = qweather_2212_64x64; break;
            case 2213: icon_array = qweather_2213_64x64; break;
            case 2214: icon_array = qweather_2214_64x64; break;
            case 2215: icon_array = qweather_2215_64x64; break;
            case 2216: icon_array = qweather_2216_64x64; break;
            case 2217: icon_array = qweather_2217_64x64; break;
            case 2218: icon_array = qweather_2218_64x64; break;
            case 2300: icon_array = qweather_2300_64x64; break;
            case 2301: icon_array = qweather_2301_64x64; break;
            case 2302: icon_array = qweather_2302_64x64; break;
            case 2303: icon_array = qweather_2303_64x64; break;
            case 2304: icon_array = qweather_2304_64x64; break;
            case 2305: icon_array = qweather_2305_64x64; break;
            case 2306: icon_array = qweather_2306_64x64; break;
            case 2307: icon_array = qweather_2307_64x64; break;
            case 2308: icon_array = qweather_2308_64x64; break;
            case 2309: icon_array = qweather_2309_64x64; break;
            case 2311: icon_array = qweather_2311_64x64; break;
            case 2312: icon_array = qweather_2312_64x64; break;
            case 2313: icon_array = qweather_2313_64x64; break;
            case 2314: icon_array = qweather_2314_64x64; break;
            case 2315: icon_array = qweather_2315_64x64; break;
            case 2316: icon_array = qweather_2316_64x64; break;
            case 2317: icon_array = qweather_2317_64x64; break;
            case 2318: icon_array = qweather_2318_64x64; break;
            case 2319: icon_array = qweather_2319_64x64; break;
            case 2320: icon_array = qweather_2320_64x64; break;
            case 2321: icon_array = qweather_2321_64x64; break;
            case 2322: icon_array = qweather_2322_64x64; break;
            case 2323: icon_array = qweather_2323_64x64; break;
            case 2324: icon_array = qweather_2324_64x64; break;
            case 2325: icon_array = qweather_2325_64x64; break;
            case 2326: icon_array = qweather_2326_64x64; break;
            case 2327: icon_array = qweather_2327_64x64; break;
            case 2328: icon_array = qweather_2328_64x64; break;
            case 2330: icon_array = qweather_2330_64x64; break;
            case 2331: icon_array = qweather_2331_64x64; break;
            case 2332: icon_array = qweather_2332_64x64; break;
            case 2333: icon_array = qweather_2333_64x64; break;
            case 2341: icon_array = qweather_2341_64x64; break;
            case 2343: icon_array = qweather_2343_64x64; break;
            case 2345: icon_array = qweather_2345_64x64; break;
            case 2346: icon_array = qweather_2346_64x64; break;
            case 2348: icon_array = qweather_2348_64x64; break;
            case 2349: icon_array = qweather_2349_64x64; break;
            case 2350: icon_array = qweather_2350_64x64; break;
            case 2351: icon_array = qweather_2351_64x64; break;
            case 2352: icon_array = qweather_2352_64x64; break;
            case 2353: icon_array = qweather_2353_64x64; break;
            case 2354: icon_array = qweather_2354_64x64; break;
            case 2355: icon_array = qweather_2355_64x64; break;
            case 2356: icon_array = qweather_2356_64x64; break;
            case 2357: icon_array = qweather_2357_64x64; break;
            case 2358: icon_array = qweather_2358_64x64; break;
            case 2359: icon_array = qweather_2359_64x64; break;
            case 2360: icon_array = qweather_2360_64x64; break;
            case 2361: icon_array = qweather_2361_64x64; break;
            case 2362: icon_array = qweather_2362_64x64; break;
            case 2363: icon_array = qweather_2363_64x64; break;
            case 2364: icon_array = qweather_2364_64x64; break;
            case 2365: icon_array = qweather_2365_64x64; break;
            case 2366: icon_array = qweather_2366_64x64; break;
            case 2367: icon_array = qweather_2367_64x64; break;
            case 2368: icon_array = qweather_2368_64x64; break;
            case 2369: icon_array = qweather_2369_64x64; break;
            case 2370: icon_array = qweather_2370_64x64; break;
            case 2371: icon_array = qweather_2371_64x64; break;
            case 2372: icon_array = qweather_2372_64x64; break;
            case 2373: icon_array = qweather_2373_64x64; break;
            case 2374: icon_array = qweather_2374_64x64; break;
            case 2375: icon_array = qweather_2375_64x64; break;
            case 2376: icon_array = qweather_2376_64x64; break;
            case 2377: icon_array = qweather_2377_64x64; break;
            case 2378: icon_array = qweather_2378_64x64; break;
            case 2379: icon_array = qweather_2379_64x64; break;
            case 2380: icon_array = qweather_2380_64x64; break;
            case 2381: icon_array = qweather_2381_64x64; break;
            case 2382: icon_array = qweather_2382_64x64; break;
            case 2383: icon_array = qweather_2383_64x64; break;
            case 2384: icon_array = qweather_2384_64x64; break;
            case 2385: icon_array = qweather_2385_64x64; break;
            case 2386: icon_array = qweather_2386_64x64; break;
            case 2387: icon_array = qweather_2387_64x64; break;
            case 2388: icon_array = qweather_2388_64x64; break;
            case 2389: icon_array = qweather_2389_64x64; break;
            case 2390: icon_array = qweather_2390_64x64; break;
            case 2391: icon_array = qweather_2391_64x64; break;
            case 2392: icon_array = qweather_2392_64x64; break;
            case 2393: icon_array = qweather_2393_64x64; break;
            case 2394: icon_array = qweather_2394_64x64; break;
            case 2395: icon_array = qweather_2395_64x64; break;
            case 2396: icon_array = qweather_2396_64x64; break;
            case 2397: icon_array = qweather_2397_64x64; break;
            case 2398: icon_array = qweather_2398_64x64; break;
            case 2399: icon_array = qweather_2399_64x64; break;
            case 2400: icon_array = qweather_2400_64x64; break;
            case 2409: icon_array = qweather_2409_64x64; break;
            case 2411: icon_array = qweather_2411_64x64; break;
            case 2412: icon_array = qweather_2412_64x64; break;
            case 2413: icon_array = qweather_2413_64x64; break;
            case 2414: icon_array = qweather_2414_64x64; break;
            case 2415: icon_array = qweather_2415_64x64; break;
            case 2416: icon_array = qweather_2416_64x64; break;
            case 2417: icon_array = qweather_2417_64x64; break;
            case 2418: icon_array = qweather_2418_64x64; break;
            case 2419: icon_array = qweather_2419_64x64; break;
            case 2420: icon_array = qweather_2420_64x64; break;
            case 2421: icon_array = qweather_2421_64x64; break;
            case 2422: icon_array = qweather_2422_64x64; break;
            case 2423: icon_array = qweather_2423_64x64; break;
            case 2424: icon_array = qweather_2424_64x64; break;
            case 2425: icon_array = qweather_2425_64x64; break;
            case 2426: icon_array = qweather_2426_64x64; break;
            case 2501: icon_array = qweather_2501_64x64; break;
            case 2502: icon_array = qweather_2502_64x64; break;
            case 2521: icon_array = qweather_2521_64x64; break;
            case 2522: icon_array = qweather_2522_64x64; break;
            case 2523: icon_array = qweather_2523_64x64; break;
            case 2524: icon_array = qweather_2524_64x64; break;
            case 2525: icon_array = qweather_2525_64x64; break;
            case 2526: icon_array = qweather_2526_64x64; break;
            case 2527: icon_array = qweather_2527_64x64; break;
            case 2528: icon_array = qweather_2528_64x64; break;
            case 2529: icon_array = qweather_2529_64x64; break;
            case 2530: icon_array = qweather_2530_64x64; break;
            case 2531: icon_array = qweather_2531_64x64; break;
            case 2532: icon_array = qweather_2532_64x64; break;
            case 2550: icon_array = qweather_2550_64x64; break;
            case 2551: icon_array = qweather_2551_64x64; break;
            case 2552: icon_array = qweather_2552_64x64; break;
            case 2553: icon_array = qweather_2553_64x64; break;
            case 2554: icon_array = qweather_2554_64x64; break;
            case 9999: icon_array = qweather_9999_64x64; break;
            default:  icon_array = qweather_101_64x64; break; 
        }
    }
    else if (size == 16) {
        // --- 匹配 16x16 迷你图标 (基于你截图新增的尺寸) ---
        switch (code) {
            case 100: icon_array = qweather_100_16x16; break;
            case 101: icon_array = qweather_101_16x16; break;
            case 102: icon_array = qweather_102_16x16; break;
            case 103: icon_array = qweather_103_16x16; break;
            case 104: icon_array = qweather_104_16x16; break;
            case 150: icon_array = qweather_150_16x16; break;
            case 151: icon_array = qweather_151_16x16; break;
            case 152: icon_array = qweather_152_16x16; break;
            case 153: icon_array = qweather_153_16x16; break;
            case 300: icon_array = qweather_300_16x16; break;
            case 301: icon_array = qweather_301_16x16; break;
            case 302: icon_array = qweather_302_16x16; break;
            case 303: icon_array = qweather_303_16x16; break;
            case 304: icon_array = qweather_304_16x16; break;
            case 305: icon_array = qweather_305_16x16; break;
            case 306: icon_array = qweather_306_16x16; break;
            case 307: icon_array = qweather_307_16x16; break;
            case 308: icon_array = qweather_308_16x16; break;
            case 309: icon_array = qweather_309_16x16; break;
            case 310: icon_array = qweather_310_16x16; break;
            case 311: icon_array = qweather_311_16x16; break;
            case 312: icon_array = qweather_312_16x16; break;
            case 313: icon_array = qweather_313_16x16; break;
            case 314: icon_array = qweather_314_16x16; break;
            case 315: icon_array = qweather_315_16x16; break;
            case 316: icon_array = qweather_316_16x16; break;
            case 317: icon_array = qweather_317_16x16; break;
            case 318: icon_array = qweather_318_16x16; break;
            case 350: icon_array = qweather_350_16x16; break;
            case 351: icon_array = qweather_351_16x16; break;
            case 399: icon_array = qweather_399_16x16; break;
            case 400: icon_array = qweather_400_16x16; break;
            case 401: icon_array = qweather_401_16x16; break;
            case 402: icon_array = qweather_402_16x16; break;
            case 403: icon_array = qweather_403_16x16; break;
            case 404: icon_array = qweather_404_16x16; break;
            case 405: icon_array = qweather_405_16x16; break;
            case 406: icon_array = qweather_406_16x16; break;
            case 407: icon_array = qweather_407_16x16; break;
            case 408: icon_array = qweather_408_16x16; break;
            case 409: icon_array = qweather_409_16x16; break;
            case 410: icon_array = qweather_410_16x16; break;
            case 456: icon_array = qweather_456_16x16; break;
            case 457: icon_array = qweather_457_16x16; break;
            case 499: icon_array = qweather_499_16x16; break;
            case 500: icon_array = qweather_500_16x16; break;
            case 501: icon_array = qweather_501_16x16; break;
            case 502: icon_array = qweather_502_16x16; break;
            case 503: icon_array = qweather_503_16x16; break;
            case 504: icon_array = qweather_504_16x16; break;
            case 507: icon_array = qweather_507_16x16; break;
            case 508: icon_array = qweather_508_16x16; break;
            case 509: icon_array = qweather_509_16x16; break;
            case 510: icon_array = qweather_510_16x16; break;
            case 511: icon_array = qweather_511_16x16; break;
            case 512: icon_array = qweather_512_16x16; break;
            case 513: icon_array = qweather_513_16x16; break;
            case 514: icon_array = qweather_514_16x16; break;
            case 515: icon_array = qweather_515_16x16; break;
            case 800: icon_array = qweather_800_16x16; break;
            case 801: icon_array = qweather_801_16x16; break;
            case 802: icon_array = qweather_802_16x16; break;
            case 803: icon_array = qweather_803_16x16; break;
            case 804: icon_array = qweather_804_16x16; break;
            case 805: icon_array = qweather_805_16x16; break;
            case 806: icon_array = qweather_806_16x16; break;
            case 807: icon_array = qweather_807_16x16; break;
            case 900: icon_array = qweather_900_16x16; break;
            case 901: icon_array = qweather_901_16x16; break;
            case 999: icon_array = qweather_999_16x16; break;
            case 1001: icon_array = qweather_1001_16x16; break;
            case 1002: icon_array = qweather_1002_16x16; break;
            case 1003: icon_array = qweather_1003_16x16; break;
            case 1004: icon_array = qweather_1004_16x16; break;
            case 1005: icon_array = qweather_1005_16x16; break;
            case 1006: icon_array = qweather_1006_16x16; break;
            case 1007: icon_array = qweather_1007_16x16; break;
            case 1008: icon_array = qweather_1008_16x16; break;
            case 1009: icon_array = qweather_1009_16x16; break;
            case 1010: icon_array = qweather_1010_16x16; break;
            case 1011: icon_array = qweather_1011_16x16; break;
            case 1012: icon_array = qweather_1012_16x16; break;
            case 1013: icon_array = qweather_1013_16x16; break;
            case 1014: icon_array = qweather_1014_16x16; break;
            case 1015: icon_array = qweather_1015_16x16; break;
            case 1016: icon_array = qweather_1016_16x16; break;
            case 1017: icon_array = qweather_1017_16x16; break;
            case 1018: icon_array = qweather_1018_16x16; break;
            case 1019: icon_array = qweather_1019_16x16; break;
            case 1020: icon_array = qweather_1020_16x16; break;
            case 1021: icon_array = qweather_1021_16x16; break;
            case 1022: icon_array = qweather_1022_16x16; break;
            case 1023: icon_array = qweather_1023_16x16; break;
            case 1024: icon_array = qweather_1024_16x16; break;
            case 1025: icon_array = qweather_1025_16x16; break;
            case 1026: icon_array = qweather_1026_16x16; break;
            case 1027: icon_array = qweather_1027_16x16; break;
            case 1028: icon_array = qweather_1028_16x16; break;
            case 1029: icon_array = qweather_1029_16x16; break;
            case 1030: icon_array = qweather_1030_16x16; break;
            case 1031: icon_array = qweather_1031_16x16; break;
            case 1032: icon_array = qweather_1032_16x16; break;
            case 1033: icon_array = qweather_1033_16x16; break;
            case 1034: icon_array = qweather_1034_16x16; break;
            case 1035: icon_array = qweather_1035_16x16; break;
            case 1036: icon_array = qweather_1036_16x16; break;
            case 1037: icon_array = qweather_1037_16x16; break;
            case 1038: icon_array = qweather_1038_16x16; break;
            case 1039: icon_array = qweather_1039_16x16; break;
            case 1040: icon_array = qweather_1040_16x16; break;
            case 1041: icon_array = qweather_1041_16x16; break;
            case 1042: icon_array = qweather_1042_16x16; break;
            case 1043: icon_array = qweather_1043_16x16; break;
            case 1044: icon_array = qweather_1044_16x16; break;
            case 1045: icon_array = qweather_1045_16x16; break;
            case 1046: icon_array = qweather_1046_16x16; break;
            case 1047: icon_array = qweather_1047_16x16; break;
            case 1048: icon_array = qweather_1048_16x16; break;
            case 1049: icon_array = qweather_1049_16x16; break;
            case 1050: icon_array = qweather_1050_16x16; break;
            case 1051: icon_array = qweather_1051_16x16; break;
            case 1052: icon_array = qweather_1052_16x16; break;
            case 1053: icon_array = qweather_1053_16x16; break;
            case 1054: icon_array = qweather_1054_16x16; break;
            case 1055: icon_array = qweather_1055_16x16; break;
            case 1056: icon_array = qweather_1056_16x16; break;
            case 1057: icon_array = qweather_1057_16x16; break;
            case 1058: icon_array = qweather_1058_16x16; break;
            case 1059: icon_array = qweather_1059_16x16; break;
            case 1060: icon_array = qweather_1060_16x16; break;
            case 1061: icon_array = qweather_1061_16x16; break;
            case 1062: icon_array = qweather_1062_16x16; break;
            case 1063: icon_array = qweather_1063_16x16; break;
            case 1064: icon_array = qweather_1064_16x16; break;
            case 1065: icon_array = qweather_1065_16x16; break;
            case 1066: icon_array = qweather_1066_16x16; break;
            case 1067: icon_array = qweather_1067_16x16; break;
            case 1068: icon_array = qweather_1068_16x16; break;
            case 1069: icon_array = qweather_1069_16x16; break;
            case 1071: icon_array = qweather_1071_16x16; break;
            case 1072: icon_array = qweather_1072_16x16; break;
            case 1073: icon_array = qweather_1073_16x16; break;
            case 1074: icon_array = qweather_1074_16x16; break;
            case 1075: icon_array = qweather_1075_16x16; break;
            case 1076: icon_array = qweather_1076_16x16; break;
            case 1077: icon_array = qweather_1077_16x16; break;
            case 1078: icon_array = qweather_1078_16x16; break;
            case 1079: icon_array = qweather_1079_16x16; break;
            case 1080: icon_array = qweather_1080_16x16; break;
            case 1081: icon_array = qweather_1081_16x16; break;
            case 1082: icon_array = qweather_1082_16x16; break;
            case 1084: icon_array = qweather_1084_16x16; break;
            case 1085: icon_array = qweather_1085_16x16; break;
            case 1086: icon_array = qweather_1086_16x16; break;
            case 1087: icon_array = qweather_1087_16x16; break;
            case 1088: icon_array = qweather_1088_16x16; break;
            case 1089: icon_array = qweather_1089_16x16; break;
            case 1201: icon_array = qweather_1201_16x16; break;
            case 1202: icon_array = qweather_1202_16x16; break;
            case 1203: icon_array = qweather_1203_16x16; break;
            case 1204: icon_array = qweather_1204_16x16; break;
            case 1205: icon_array = qweather_1205_16x16; break;
            case 1206: icon_array = qweather_1206_16x16; break;
            case 1207: icon_array = qweather_1207_16x16; break;
            case 1208: icon_array = qweather_1208_16x16; break;
            case 1209: icon_array = qweather_1209_16x16; break;
            case 1210: icon_array = qweather_1210_16x16; break;
            case 1211: icon_array = qweather_1211_16x16; break;
            case 1212: icon_array = qweather_1212_16x16; break;
            case 1213: icon_array = qweather_1213_16x16; break;
            case 1214: icon_array = qweather_1214_16x16; break;
            case 1215: icon_array = qweather_1215_16x16; break;
            case 1216: icon_array = qweather_1216_16x16; break;
            case 1217: icon_array = qweather_1217_16x16; break;
            case 1218: icon_array = qweather_1218_16x16; break;
            case 1219: icon_array = qweather_1219_16x16; break;
            case 1221: icon_array = qweather_1221_16x16; break;
            case 1241: icon_array = qweather_1241_16x16; break;
            case 1242: icon_array = qweather_1242_16x16; break;
            case 1243: icon_array = qweather_1243_16x16; break;
            case 1244: icon_array = qweather_1244_16x16; break;
            case 1245: icon_array = qweather_1245_16x16; break;
            case 1246: icon_array = qweather_1246_16x16; break;
            case 1247: icon_array = qweather_1247_16x16; break;
            case 1248: icon_array = qweather_1248_16x16; break;
            case 1249: icon_array = qweather_1249_16x16; break;
            case 1250: icon_array = qweather_1250_16x16; break;
            case 1251: icon_array = qweather_1251_16x16; break;
            case 1271: icon_array = qweather_1271_16x16; break;
            case 1272: icon_array = qweather_1272_16x16; break;
            case 1273: icon_array = qweather_1273_16x16; break;
            case 1274: icon_array = qweather_1274_16x16; break;
            case 1601: icon_array = qweather_1601_16x16; break;
            case 1602: icon_array = qweather_1602_16x16; break;
            case 1603: icon_array = qweather_1603_16x16; break;
            case 1604: icon_array = qweather_1604_16x16; break;
            case 1605: icon_array = qweather_1605_16x16; break;
            case 1606: icon_array = qweather_1606_16x16; break;
            case 1607: icon_array = qweather_1607_16x16; break;
            case 1608: icon_array = qweather_1608_16x16; break;
            case 1609: icon_array = qweather_1609_16x16; break;
            case 1610: icon_array = qweather_1610_16x16; break;
            case 1701: icon_array = qweather_1701_16x16; break;
            case 1702: icon_array = qweather_1702_16x16; break;
            case 1703: icon_array = qweather_1703_16x16; break;
            case 1704: icon_array = qweather_1704_16x16; break;
            case 1705: icon_array = qweather_1705_16x16; break;
            case 1706: icon_array = qweather_1706_16x16; break;
            case 1707: icon_array = qweather_1707_16x16; break;
            case 1708: icon_array = qweather_1708_16x16; break;
            case 1709: icon_array = qweather_1709_16x16; break;
            case 1710: icon_array = qweather_1710_16x16; break;
            case 1801: icon_array = qweather_1801_16x16; break;
            case 1802: icon_array = qweather_1802_16x16; break;
            case 1803: icon_array = qweather_1803_16x16; break;
            case 1804: icon_array = qweather_1804_16x16; break;
            case 1805: icon_array = qweather_1805_16x16; break;
            case 2001: icon_array = qweather_2001_16x16; break;
            case 2002: icon_array = qweather_2002_16x16; break;
            case 2003: icon_array = qweather_2003_16x16; break;
            case 2004: icon_array = qweather_2004_16x16; break;
            case 2005: icon_array = qweather_2005_16x16; break;
            case 2006: icon_array = qweather_2006_16x16; break;
            case 2007: icon_array = qweather_2007_16x16; break;
            case 2029: icon_array = qweather_2029_16x16; break;
            case 2030: icon_array = qweather_2030_16x16; break;
            case 2031: icon_array = qweather_2031_16x16; break;
            case 2032: icon_array = qweather_2032_16x16; break;
            case 2033: icon_array = qweather_2033_16x16; break;
            case 2050: icon_array = qweather_2050_16x16; break;
            case 2051: icon_array = qweather_2051_16x16; break;
            case 2052: icon_array = qweather_2052_16x16; break;
            case 2053: icon_array = qweather_2053_16x16; break;
            case 2054: icon_array = qweather_2054_16x16; break;
            case 2070: icon_array = qweather_2070_16x16; break;
            case 2071: icon_array = qweather_2071_16x16; break;
            case 2072: icon_array = qweather_2072_16x16; break;
            case 2073: icon_array = qweather_2073_16x16; break;
            case 2074: icon_array = qweather_2074_16x16; break;
            case 2075: icon_array = qweather_2075_16x16; break;
            case 2076: icon_array = qweather_2076_16x16; break;
            case 2077: icon_array = qweather_2077_16x16; break;
            case 2078: icon_array = qweather_2078_16x16; break;
            case 2079: icon_array = qweather_2079_16x16; break;
            case 2080: icon_array = qweather_2080_16x16; break;
            case 2081: icon_array = qweather_2081_16x16; break;
            case 2082: icon_array = qweather_2082_16x16; break;
            case 2083: icon_array = qweather_2083_16x16; break;
            case 2084: icon_array = qweather_2084_16x16; break;
            case 2085: icon_array = qweather_2085_16x16; break;
            case 2100: icon_array = qweather_2100_16x16; break;
            case 2101: icon_array = qweather_2101_16x16; break;
            case 2102: icon_array = qweather_2102_16x16; break;
            case 2103: icon_array = qweather_2103_16x16; break;
            case 2104: icon_array = qweather_2104_16x16; break;
            case 2105: icon_array = qweather_2105_16x16; break;
            case 2106: icon_array = qweather_2106_16x16; break;
            case 2107: icon_array = qweather_2107_16x16; break;
            case 2108: icon_array = qweather_2108_16x16; break;
            case 2109: icon_array = qweather_2109_16x16; break;
            case 2111: icon_array = qweather_2111_16x16; break;
            case 2120: icon_array = qweather_2120_16x16; break;
            case 2121: icon_array = qweather_2121_16x16; break;
            case 2122: icon_array = qweather_2122_16x16; break;
            case 2123: icon_array = qweather_2123_16x16; break;
            case 2124: icon_array = qweather_2124_16x16; break;
            case 2125: icon_array = qweather_2125_16x16; break;
            case 2126: icon_array = qweather_2126_16x16; break;
            case 2127: icon_array = qweather_2127_16x16; break;
            case 2128: icon_array = qweather_2128_16x16; break;
            case 2129: icon_array = qweather_2129_16x16; break;
            case 2130: icon_array = qweather_2130_16x16; break;
            case 2131: icon_array = qweather_2131_16x16; break;
            case 2132: icon_array = qweather_2132_16x16; break;
            case 2133: icon_array = qweather_2133_16x16; break;
            case 2134: icon_array = qweather_2134_16x16; break;
            case 2135: icon_array = qweather_2135_16x16; break;
            case 2150: icon_array = qweather_2150_16x16; break;
            case 2151: icon_array = qweather_2151_16x16; break;
            case 2152: icon_array = qweather_2152_16x16; break;
            case 2153: icon_array = qweather_2153_16x16; break;
            case 2154: icon_array = qweather_2154_16x16; break;
            case 2155: icon_array = qweather_2155_16x16; break;
            case 2156: icon_array = qweather_2156_16x16; break;
            case 2157: icon_array = qweather_2157_16x16; break;
            case 2158: icon_array = qweather_2158_16x16; break;
            case 2159: icon_array = qweather_2159_16x16; break;
            case 2160: icon_array = qweather_2160_16x16; break;
            case 2161: icon_array = qweather_2161_16x16; break;
            case 2162: icon_array = qweather_2162_16x16; break;
            case 2163: icon_array = qweather_2163_16x16; break;
            case 2164: icon_array = qweather_2164_16x16; break;
            case 2165: icon_array = qweather_2165_16x16; break;
            case 2166: icon_array = qweather_2166_16x16; break;
            case 2167: icon_array = qweather_2167_16x16; break;
            case 2190: icon_array = qweather_2190_16x16; break;
            case 2191: icon_array = qweather_2191_16x16; break;
            case 2192: icon_array = qweather_2192_16x16; break;
            case 2193: icon_array = qweather_2193_16x16; break;
            case 2200: icon_array = qweather_2200_16x16; break;
            case 2201: icon_array = qweather_2201_16x16; break;
            case 2202: icon_array = qweather_2202_16x16; break;
            case 2203: icon_array = qweather_2203_16x16; break;
            case 2204: icon_array = qweather_2204_16x16; break;
            case 2205: icon_array = qweather_2205_16x16; break;
            case 2207: icon_array = qweather_2207_16x16; break;
            case 2208: icon_array = qweather_2208_16x16; break;
            case 2209: icon_array = qweather_2209_16x16; break;
            case 2210: icon_array = qweather_2210_16x16; break;
            case 2211: icon_array = qweather_2211_16x16; break;
            case 2212: icon_array = qweather_2212_16x16; break;
            case 2213: icon_array = qweather_2213_16x16; break;
            case 2214: icon_array = qweather_2214_16x16; break;
            case 2215: icon_array = qweather_2215_16x16; break;
            case 2216: icon_array = qweather_2216_16x16; break;
            case 2217: icon_array = qweather_2217_16x16; break;
            case 2218: icon_array = qweather_2218_16x16; break;
            case 2300: icon_array = qweather_2300_16x16; break;
            case 2301: icon_array = qweather_2301_16x16; break;
            case 2302: icon_array = qweather_2302_16x16; break;
            case 2303: icon_array = qweather_2303_16x16; break;
            case 2304: icon_array = qweather_2304_16x16; break;
            case 2305: icon_array = qweather_2305_16x16; break;
            case 2306: icon_array = qweather_2306_16x16; break;
            case 2307: icon_array = qweather_2307_16x16; break;
            case 2308: icon_array = qweather_2308_16x16; break;
            case 2309: icon_array = qweather_2309_16x16; break;
            case 2311: icon_array = qweather_2311_16x16; break;
            case 2312: icon_array = qweather_2312_16x16; break;
            case 2313: icon_array = qweather_2313_16x16; break;
            case 2314: icon_array = qweather_2314_16x16; break;
            case 2315: icon_array = qweather_2315_16x16; break;
            case 2316: icon_array = qweather_2316_16x16; break;
            case 2317: icon_array = qweather_2317_16x16; break;
            case 2318: icon_array = qweather_2318_16x16; break;
            case 2319: icon_array = qweather_2319_16x16; break;
            case 2320: icon_array = qweather_2320_16x16; break;
            case 2321: icon_array = qweather_2321_16x16; break;
            case 2322: icon_array = qweather_2322_16x16; break;
            case 2323: icon_array = qweather_2323_16x16; break;
            case 2324: icon_array = qweather_2324_16x16; break;
            case 2325: icon_array = qweather_2325_16x16; break;
            case 2326: icon_array = qweather_2326_16x16; break;
            case 2327: icon_array = qweather_2327_16x16; break;
            case 2328: icon_array = qweather_2328_16x16; break;
            case 2330: icon_array = qweather_2330_16x16; break;
            case 2331: icon_array = qweather_2331_16x16; break;
            case 2332: icon_array = qweather_2332_16x16; break;
            case 2333: icon_array = qweather_2333_16x16; break;
            case 2341: icon_array = qweather_2341_16x16; break;
            case 2343: icon_array = qweather_2343_16x16; break;
            case 2345: icon_array = qweather_2345_16x16; break;
            case 2346: icon_array = qweather_2346_16x16; break;
            case 2348: icon_array = qweather_2348_16x16; break;
            case 2349: icon_array = qweather_2349_16x16; break;
            case 2350: icon_array = qweather_2350_16x16; break;
            case 2351: icon_array = qweather_2351_16x16; break;
            case 2352: icon_array = qweather_2352_16x16; break;
            case 2353: icon_array = qweather_2353_16x16; break;
            case 2354: icon_array = qweather_2354_16x16; break;
            case 2355: icon_array = qweather_2355_16x16; break;
            case 2356: icon_array = qweather_2356_16x16; break;
            case 2357: icon_array = qweather_2357_16x16; break;
            case 2358: icon_array = qweather_2358_16x16; break;
            case 2359: icon_array = qweather_2359_16x16; break;
            case 2360: icon_array = qweather_2360_16x16; break;
            case 2361: icon_array = qweather_2361_16x16; break;
            case 2362: icon_array = qweather_2362_16x16; break;
            case 2363: icon_array = qweather_2363_16x16; break;
            case 2364: icon_array = qweather_2364_16x16; break;
            case 2365: icon_array = qweather_2365_16x16; break;
            case 2366: icon_array = qweather_2366_16x16; break;
            case 2367: icon_array = qweather_2367_16x16; break;
            case 2368: icon_array = qweather_2368_16x16; break;
            case 2369: icon_array = qweather_2369_16x16; break;
            case 2370: icon_array = qweather_2370_16x16; break;
            case 2371: icon_array = qweather_2371_16x16; break;
            case 2372: icon_array = qweather_2372_16x16; break;
            case 2373: icon_array = qweather_2373_16x16; break;
            case 2374: icon_array = qweather_2374_16x16; break;
            case 2375: icon_array = qweather_2375_16x16; break;
            case 2376: icon_array = qweather_2376_16x16; break;
            case 2377: icon_array = qweather_2377_16x16; break;
            case 2378: icon_array = qweather_2378_16x16; break;
            case 2379: icon_array = qweather_2379_16x16; break;
            case 2380: icon_array = qweather_2380_16x16; break;
            case 2381: icon_array = qweather_2381_16x16; break;
            case 2382: icon_array = qweather_2382_16x16; break;
            case 2383: icon_array = qweather_2383_16x16; break;
            case 2384: icon_array = qweather_2384_16x16; break;
            case 2385: icon_array = qweather_2385_16x16; break;
            case 2386: icon_array = qweather_2386_16x16; break;
            case 2387: icon_array = qweather_2387_16x16; break;
            case 2388: icon_array = qweather_2388_16x16; break;
            case 2389: icon_array = qweather_2389_16x16; break;
            case 2390: icon_array = qweather_2390_16x16; break;
            case 2391: icon_array = qweather_2391_16x16; break;
            case 2392: icon_array = qweather_2392_16x16; break;
            case 2393: icon_array = qweather_2393_16x16; break;
            case 2394: icon_array = qweather_2394_16x16; break;
            case 2395: icon_array = qweather_2395_16x16; break;
            case 2396: icon_array = qweather_2396_16x16; break;
            case 2397: icon_array = qweather_2397_16x16; break;
            case 2398: icon_array = qweather_2398_16x16; break;
            case 2399: icon_array = qweather_2399_16x16; break;
            case 2400: icon_array = qweather_2400_16x16; break;
            case 2409: icon_array = qweather_2409_16x16; break;
            case 2411: icon_array = qweather_2411_16x16; break;
            case 2412: icon_array = qweather_2412_16x16; break;
            case 2413: icon_array = qweather_2413_16x16; break;
            case 2414: icon_array = qweather_2414_16x16; break;
            case 2415: icon_array = qweather_2415_16x16; break;
            case 2416: icon_array = qweather_2416_16x16; break;
            case 2417: icon_array = qweather_2417_16x16; break;
            case 2418: icon_array = qweather_2418_16x16; break;
            case 2419: icon_array = qweather_2419_16x16; break;
            case 2420: icon_array = qweather_2420_16x16; break;
            case 2421: icon_array = qweather_2421_16x16; break;
            case 2422: icon_array = qweather_2422_16x16; break;
            case 2423: icon_array = qweather_2423_16x16; break;
            case 2424: icon_array = qweather_2424_16x16; break;
            case 2425: icon_array = qweather_2425_16x16; break;
            case 2426: icon_array = qweather_2426_16x16; break;
            case 2501: icon_array = qweather_2501_16x16; break;
            case 2502: icon_array = qweather_2502_16x16; break;
            case 2521: icon_array = qweather_2521_16x16; break;
            case 2522: icon_array = qweather_2522_16x16; break;
            case 2523: icon_array = qweather_2523_16x16; break;
            case 2524: icon_array = qweather_2524_16x16; break;
            case 2525: icon_array = qweather_2525_16x16; break;
            case 2526: icon_array = qweather_2526_16x16; break;
            case 2527: icon_array = qweather_2527_16x16; break;
            case 2528: icon_array = qweather_2528_16x16; break;
            case 2529: icon_array = qweather_2529_16x16; break;
            case 2530: icon_array = qweather_2530_16x16; break;
            case 2531: icon_array = qweather_2531_16x16; break;
            case 2532: icon_array = qweather_2532_16x16; break;
            case 2550: icon_array = qweather_2550_16x16; break;
            case 2551: icon_array = qweather_2551_16x16; break;
            case 2552: icon_array = qweather_2552_16x16; break;
            case 2553: icon_array = qweather_2553_16x16; break;
            case 2554: icon_array = qweather_2554_16x16; break;
            case 9999: icon_array = qweather_9999_16x16; break;
            default:  icon_array = qweather_101_16x16; break; 
        }
    }

    // 如果成功匹配到图标，则调用底层绘制
    if (icon_array != nullptr) {
        display->drawBitmap(x, y, icon_array, size, size, GxEPD_BLACK);
    } else {
        // 如果遇到了没有存图标的奇葩天气，走这里：
        Serial.printf("【UI警告】缺少天气代码 %d 的图标，已跳过绘制防止死机！\n", code);
        
        // （可选）在原来的位置画一个黑框或者问号，提醒你自己去补图标
        display->drawRect(x, y, size, size, GxEPD_BLACK); 
    }
}

// 360度精确风向图标指针数组 (来自第二套代码，完整360条)
const uint8_t* wind_icons_map[360] = {
    wind_direction_meteorological_0deg_16x16,
    wind_direction_meteorological_1deg_16x16,
    wind_direction_meteorological_2deg_16x16,
    wind_direction_meteorological_3deg_16x16,
    wind_direction_meteorological_4deg_16x16,
    wind_direction_meteorological_5deg_16x16,
    wind_direction_meteorological_6deg_16x16,
    wind_direction_meteorological_7deg_16x16,
    wind_direction_meteorological_8deg_16x16,
    wind_direction_meteorological_9deg_16x16,
    wind_direction_meteorological_10deg_16x16,
    wind_direction_meteorological_11deg_16x16,
    wind_direction_meteorological_12deg_16x16,
    wind_direction_meteorological_13deg_16x16,
    wind_direction_meteorological_14deg_16x16,
    wind_direction_meteorological_15deg_16x16,
    wind_direction_meteorological_16deg_16x16,
    wind_direction_meteorological_17deg_16x16,
    wind_direction_meteorological_18deg_16x16,
    wind_direction_meteorological_19deg_16x16,
    wind_direction_meteorological_20deg_16x16,
    wind_direction_meteorological_21deg_16x16,
    wind_direction_meteorological_22deg_16x16,
    wind_direction_meteorological_23deg_16x16,
    wind_direction_meteorological_24deg_16x16,
    wind_direction_meteorological_25deg_16x16,
    wind_direction_meteorological_26deg_16x16,
    wind_direction_meteorological_27deg_16x16,
    wind_direction_meteorological_28deg_16x16,
    wind_direction_meteorological_29deg_16x16,
    wind_direction_meteorological_30deg_16x16,
    wind_direction_meteorological_31deg_16x16,
    wind_direction_meteorological_32deg_16x16,
    wind_direction_meteorological_33deg_16x16,
    wind_direction_meteorological_34deg_16x16,
    wind_direction_meteorological_35deg_16x16,
    wind_direction_meteorological_36deg_16x16,
    wind_direction_meteorological_37deg_16x16,
    wind_direction_meteorological_38deg_16x16,
    wind_direction_meteorological_39deg_16x16,
    wind_direction_meteorological_40deg_16x16,
    wind_direction_meteorological_41deg_16x16,
    wind_direction_meteorological_42deg_16x16,
    wind_direction_meteorological_43deg_16x16,
    wind_direction_meteorological_44deg_16x16,
    wind_direction_meteorological_45deg_16x16,
    wind_direction_meteorological_46deg_16x16,
    wind_direction_meteorological_47deg_16x16,
    wind_direction_meteorological_48deg_16x16,
    wind_direction_meteorological_49deg_16x16,
    wind_direction_meteorological_50deg_16x16,
    wind_direction_meteorological_51deg_16x16,
    wind_direction_meteorological_52deg_16x16,
    wind_direction_meteorological_53deg_16x16,
    wind_direction_meteorological_54deg_16x16,
    wind_direction_meteorological_55deg_16x16,
    wind_direction_meteorological_56deg_16x16,
    wind_direction_meteorological_57deg_16x16,
    wind_direction_meteorological_58deg_16x16,
    wind_direction_meteorological_59deg_16x16,
    wind_direction_meteorological_60deg_16x16,
    wind_direction_meteorological_61deg_16x16,
    wind_direction_meteorological_62deg_16x16,
    wind_direction_meteorological_63deg_16x16,
    wind_direction_meteorological_64deg_16x16,
    wind_direction_meteorological_65deg_16x16,
    wind_direction_meteorological_66deg_16x16,
    wind_direction_meteorological_67deg_16x16,
    wind_direction_meteorological_68deg_16x16,
    wind_direction_meteorological_69deg_16x16,
    wind_direction_meteorological_70deg_16x16,
    wind_direction_meteorological_71deg_16x16,
    wind_direction_meteorological_72deg_16x16,
    wind_direction_meteorological_73deg_16x16,
    wind_direction_meteorological_74deg_16x16,
    wind_direction_meteorological_75deg_16x16,
    wind_direction_meteorological_76deg_16x16,
    wind_direction_meteorological_77deg_16x16,
    wind_direction_meteorological_78deg_16x16,
    wind_direction_meteorological_79deg_16x16,
    wind_direction_meteorological_80deg_16x16,
    wind_direction_meteorological_81deg_16x16,
    wind_direction_meteorological_82deg_16x16,
    wind_direction_meteorological_83deg_16x16,
    wind_direction_meteorological_84deg_16x16,
    wind_direction_meteorological_85deg_16x16,
    wind_direction_meteorological_86deg_16x16,
    wind_direction_meteorological_87deg_16x16,
    wind_direction_meteorological_88deg_16x16,
    wind_direction_meteorological_89deg_16x16,
    wind_direction_meteorological_90deg_16x16,
    wind_direction_meteorological_91deg_16x16,
    wind_direction_meteorological_92deg_16x16,
    wind_direction_meteorological_93deg_16x16,
    wind_direction_meteorological_94deg_16x16,
    wind_direction_meteorological_95deg_16x16,
    wind_direction_meteorological_96deg_16x16,
    wind_direction_meteorological_97deg_16x16,
    wind_direction_meteorological_98deg_16x16,
    wind_direction_meteorological_99deg_16x16,
    wind_direction_meteorological_100deg_16x16,
    wind_direction_meteorological_101deg_16x16,
    wind_direction_meteorological_102deg_16x16,
    wind_direction_meteorological_103deg_16x16,
    wind_direction_meteorological_104deg_16x16,
    wind_direction_meteorological_105deg_16x16,
    wind_direction_meteorological_106deg_16x16,
    wind_direction_meteorological_107deg_16x16,
    wind_direction_meteorological_108deg_16x16,
    wind_direction_meteorological_109deg_16x16,
    wind_direction_meteorological_110deg_16x16,
    wind_direction_meteorological_111deg_16x16,
    wind_direction_meteorological_112deg_16x16,
    wind_direction_meteorological_113deg_16x16,
    wind_direction_meteorological_114deg_16x16,
    wind_direction_meteorological_115deg_16x16,
    wind_direction_meteorological_116deg_16x16,
    wind_direction_meteorological_117deg_16x16,
    wind_direction_meteorological_118deg_16x16,
    wind_direction_meteorological_119deg_16x16,
    wind_direction_meteorological_120deg_16x16,
    wind_direction_meteorological_121deg_16x16,
    wind_direction_meteorological_122deg_16x16,
    wind_direction_meteorological_123deg_16x16,
    wind_direction_meteorological_124deg_16x16,
    wind_direction_meteorological_125deg_16x16,
    wind_direction_meteorological_126deg_16x16,
    wind_direction_meteorological_127deg_16x16,
    wind_direction_meteorological_128deg_16x16,
    wind_direction_meteorological_129deg_16x16,
    wind_direction_meteorological_130deg_16x16,
    wind_direction_meteorological_131deg_16x16,
    wind_direction_meteorological_132deg_16x16,
    wind_direction_meteorological_133deg_16x16,
    wind_direction_meteorological_134deg_16x16,
    wind_direction_meteorological_135deg_16x16,
    wind_direction_meteorological_136deg_16x16,
    wind_direction_meteorological_137deg_16x16,
    wind_direction_meteorological_138deg_16x16,
    wind_direction_meteorological_139deg_16x16,
    wind_direction_meteorological_140deg_16x16,
    wind_direction_meteorological_141deg_16x16,
    wind_direction_meteorological_142deg_16x16,
    wind_direction_meteorological_143deg_16x16,
    wind_direction_meteorological_144deg_16x16,
    wind_direction_meteorological_145deg_16x16,
    wind_direction_meteorological_146deg_16x16,
    wind_direction_meteorological_147deg_16x16,
    wind_direction_meteorological_148deg_16x16,
    wind_direction_meteorological_149deg_16x16,
    wind_direction_meteorological_150deg_16x16,
    wind_direction_meteorological_151deg_16x16,
    wind_direction_meteorological_152deg_16x16,
    wind_direction_meteorological_153deg_16x16,
    wind_direction_meteorological_154deg_16x16,
    wind_direction_meteorological_155deg_16x16,
    wind_direction_meteorological_156deg_16x16,
    wind_direction_meteorological_157deg_16x16,
    wind_direction_meteorological_158deg_16x16,
    wind_direction_meteorological_159deg_16x16,
    wind_direction_meteorological_160deg_16x16,
    wind_direction_meteorological_161deg_16x16,
    wind_direction_meteorological_162deg_16x16,
    wind_direction_meteorological_163deg_16x16,
    wind_direction_meteorological_164deg_16x16,
    wind_direction_meteorological_165deg_16x16,
    wind_direction_meteorological_166deg_16x16,
    wind_direction_meteorological_167deg_16x16,
    wind_direction_meteorological_168deg_16x16,
    wind_direction_meteorological_169deg_16x16,
    wind_direction_meteorological_170deg_16x16,
    wind_direction_meteorological_171deg_16x16,
    wind_direction_meteorological_172deg_16x16,
    wind_direction_meteorological_173deg_16x16,
    wind_direction_meteorological_174deg_16x16,
    wind_direction_meteorological_175deg_16x16,
    wind_direction_meteorological_176deg_16x16,
    wind_direction_meteorological_177deg_16x16,
    wind_direction_meteorological_178deg_16x16,
    wind_direction_meteorological_179deg_16x16,
    wind_direction_meteorological_180deg_16x16,
    wind_direction_meteorological_181deg_16x16,
    wind_direction_meteorological_182deg_16x16,
    wind_direction_meteorological_183deg_16x16,
    wind_direction_meteorological_184deg_16x16,
    wind_direction_meteorological_185deg_16x16,
    wind_direction_meteorological_186deg_16x16,
    wind_direction_meteorological_187deg_16x16,
    wind_direction_meteorological_188deg_16x16,
    wind_direction_meteorological_189deg_16x16,
    wind_direction_meteorological_190deg_16x16,
    wind_direction_meteorological_191deg_16x16,
    wind_direction_meteorological_192deg_16x16,
    wind_direction_meteorological_193deg_16x16,
    wind_direction_meteorological_194deg_16x16,
    wind_direction_meteorological_195deg_16x16,
    wind_direction_meteorological_196deg_16x16,
    wind_direction_meteorological_197deg_16x16,
    wind_direction_meteorological_198deg_16x16,
    wind_direction_meteorological_199deg_16x16,
    wind_direction_meteorological_200deg_16x16,
    wind_direction_meteorological_201deg_16x16,
    wind_direction_meteorological_202deg_16x16,
    wind_direction_meteorological_203deg_16x16,
    wind_direction_meteorological_204deg_16x16,
    wind_direction_meteorological_205deg_16x16,
    wind_direction_meteorological_206deg_16x16,
    wind_direction_meteorological_207deg_16x16,
    wind_direction_meteorological_208deg_16x16,
    wind_direction_meteorological_209deg_16x16,
    wind_direction_meteorological_210deg_16x16,
    wind_direction_meteorological_211deg_16x16,
    wind_direction_meteorological_212deg_16x16,
    wind_direction_meteorological_213deg_16x16,
    wind_direction_meteorological_214deg_16x16,
    wind_direction_meteorological_215deg_16x16,
    wind_direction_meteorological_216deg_16x16,
    wind_direction_meteorological_217deg_16x16,
    wind_direction_meteorological_218deg_16x16,
    wind_direction_meteorological_219deg_16x16,
    wind_direction_meteorological_220deg_16x16,
    wind_direction_meteorological_221deg_16x16,
    wind_direction_meteorological_222deg_16x16,
    wind_direction_meteorological_223deg_16x16,
    wind_direction_meteorological_224deg_16x16,
    wind_direction_meteorological_225deg_16x16,
    wind_direction_meteorological_226deg_16x16,
    wind_direction_meteorological_227deg_16x16,
    wind_direction_meteorological_228deg_16x16,
    wind_direction_meteorological_229deg_16x16,
    wind_direction_meteorological_230deg_16x16,
    wind_direction_meteorological_231deg_16x16,
    wind_direction_meteorological_232deg_16x16,
    wind_direction_meteorological_233deg_16x16,
    wind_direction_meteorological_234deg_16x16,
    wind_direction_meteorological_235deg_16x16,
    wind_direction_meteorological_236deg_16x16,
    wind_direction_meteorological_237deg_16x16,
    wind_direction_meteorological_238deg_16x16,
    wind_direction_meteorological_239deg_16x16,
    wind_direction_meteorological_240deg_16x16,
    wind_direction_meteorological_241deg_16x16,
    wind_direction_meteorological_242deg_16x16,
    wind_direction_meteorological_243deg_16x16,
    wind_direction_meteorological_244deg_16x16,
    wind_direction_meteorological_245deg_16x16,
    wind_direction_meteorological_246deg_16x16,
    wind_direction_meteorological_247deg_16x16,
    wind_direction_meteorological_248deg_16x16,
    wind_direction_meteorological_249deg_16x16,
    wind_direction_meteorological_250deg_16x16,
    wind_direction_meteorological_251deg_16x16,
    wind_direction_meteorological_252deg_16x16,
    wind_direction_meteorological_253deg_16x16,
    wind_direction_meteorological_254deg_16x16,
    wind_direction_meteorological_255deg_16x16,
    wind_direction_meteorological_256deg_16x16,
    wind_direction_meteorological_257deg_16x16,
    wind_direction_meteorological_258deg_16x16,
    wind_direction_meteorological_259deg_16x16,
    wind_direction_meteorological_260deg_16x16,
    wind_direction_meteorological_261deg_16x16,
    wind_direction_meteorological_262deg_16x16,
    wind_direction_meteorological_263deg_16x16,
    wind_direction_meteorological_264deg_16x16,
    wind_direction_meteorological_265deg_16x16,
    wind_direction_meteorological_266deg_16x16,
    wind_direction_meteorological_267deg_16x16,
    wind_direction_meteorological_268deg_16x16,
    wind_direction_meteorological_269deg_16x16,
    wind_direction_meteorological_270deg_16x16,
    wind_direction_meteorological_271deg_16x16,
    wind_direction_meteorological_272deg_16x16,
    wind_direction_meteorological_273deg_16x16,
    wind_direction_meteorological_274deg_16x16,
    wind_direction_meteorological_275deg_16x16,
    wind_direction_meteorological_276deg_16x16,
    wind_direction_meteorological_277deg_16x16,
    wind_direction_meteorological_278deg_16x16,
    wind_direction_meteorological_279deg_16x16,
    wind_direction_meteorological_280deg_16x16,
    wind_direction_meteorological_281deg_16x16,
    wind_direction_meteorological_282deg_16x16,
    wind_direction_meteorological_283deg_16x16,
    wind_direction_meteorological_284deg_16x16,
    wind_direction_meteorological_285deg_16x16,
    wind_direction_meteorological_286deg_16x16,
    wind_direction_meteorological_287deg_16x16,
    wind_direction_meteorological_288deg_16x16,
    wind_direction_meteorological_289deg_16x16,
    wind_direction_meteorological_290deg_16x16,
    wind_direction_meteorological_291deg_16x16,
    wind_direction_meteorological_292deg_16x16,
    wind_direction_meteorological_293deg_16x16,
    wind_direction_meteorological_294deg_16x16,
    wind_direction_meteorological_295deg_16x16,
    wind_direction_meteorological_296deg_16x16,
    wind_direction_meteorological_297deg_16x16,
    wind_direction_meteorological_298deg_16x16,
    wind_direction_meteorological_299deg_16x16,
    wind_direction_meteorological_300deg_16x16,
    wind_direction_meteorological_301deg_16x16,
    wind_direction_meteorological_302deg_16x16,
    wind_direction_meteorological_303deg_16x16,
    wind_direction_meteorological_304deg_16x16,
    wind_direction_meteorological_305deg_16x16,
    wind_direction_meteorological_306deg_16x16,
    wind_direction_meteorological_307deg_16x16,
    wind_direction_meteorological_308deg_16x16,
    wind_direction_meteorological_309deg_16x16,
    wind_direction_meteorological_310deg_16x16,
    wind_direction_meteorological_311deg_16x16,
    wind_direction_meteorological_312deg_16x16,
    wind_direction_meteorological_313deg_16x16,
    wind_direction_meteorological_314deg_16x16,
    wind_direction_meteorological_315deg_16x16,
    wind_direction_meteorological_316deg_16x16,
    wind_direction_meteorological_317deg_16x16,
    wind_direction_meteorological_318deg_16x16,
    wind_direction_meteorological_319deg_16x16,
    wind_direction_meteorological_320deg_16x16,
    wind_direction_meteorological_321deg_16x16,
    wind_direction_meteorological_322deg_16x16,
    wind_direction_meteorological_323deg_16x16,
    wind_direction_meteorological_324deg_16x16,
    wind_direction_meteorological_325deg_16x16,
    wind_direction_meteorological_326deg_16x16,
    wind_direction_meteorological_327deg_16x16,
    wind_direction_meteorological_328deg_16x16,
    wind_direction_meteorological_329deg_16x16,
    wind_direction_meteorological_330deg_16x16,
    wind_direction_meteorological_331deg_16x16,
    wind_direction_meteorological_332deg_16x16,
    wind_direction_meteorological_333deg_16x16,
    wind_direction_meteorological_334deg_16x16,
    wind_direction_meteorological_335deg_16x16,
    wind_direction_meteorological_336deg_16x16,
    wind_direction_meteorological_337deg_16x16,
    wind_direction_meteorological_338deg_16x16,
    wind_direction_meteorological_339deg_16x16,
    wind_direction_meteorological_340deg_16x16,
    wind_direction_meteorological_341deg_16x16,
    wind_direction_meteorological_342deg_16x16,
    wind_direction_meteorological_343deg_16x16,
    wind_direction_meteorological_344deg_16x16,
    wind_direction_meteorological_345deg_16x16,
    wind_direction_meteorological_346deg_16x16,
    wind_direction_meteorological_347deg_16x16,
    wind_direction_meteorological_348deg_16x16,
    wind_direction_meteorological_349deg_16x16,
    wind_direction_meteorological_350deg_16x16,
    wind_direction_meteorological_351deg_16x16,
    wind_direction_meteorological_352deg_16x16,
    wind_direction_meteorological_353deg_16x16,
    wind_direction_meteorological_354deg_16x16,
    wind_direction_meteorological_355deg_16x16,
    wind_direction_meteorological_356deg_16x16,
    wind_direction_meteorological_357deg_16x16,
    wind_direction_meteorological_358deg_16x16,
    wind_direction_meteorological_359deg_16x16
};

// 精确风向图标获取（防越界）(来自第二套代码)
const uint8_t* getExactWindIcon(String wind360Str) {
    int angle = wind360Str.toInt();
    if (angle < 0)   angle = 0;
    if (angle > 359) angle = 359;
    return wind_icons_map[angle];
}


// ============================================================
// =================== 五个界面绘制函数 =======================
// ============================================================

// 通用顶部状态栏（时间 + WiFi + 电池）(来自第二套代码)
void drawStatusBar() {
    String timeStr = getCurrentTime();
    lastDisplayedTime = timeStr;
    if (timeStr == "--:--") {
        configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    }
    writeFont(20, 8, timeStr, u8g2_simsun_24_gb2312);
    drawDynamicWiFiIcon(680, 3, WiFi.RSSI());
    drawDynamicBatteryIcon(720, 3, 100);
    writeFont(750, 13, "100%", u8g2_simsun_12_gb2312);
    display->drawLine(10, 40, 790, 40, GxEPD_BLACK);
    display->drawLine(10, 41, 790, 41, GxEPD_BLACK);
}

// ====== 界面 0：实时天气 + 空气质量看板 (来自第二套代码 drawScreen1) ======
void drawScreen1() {
    drawStatusBar();

    // 左侧：实时天气 (X: 20~400)
    drawDynamicWeatherIcon(40, 60, todayWeather.icon, 196);
    writeFont(290, 79,  todayWeather.temp, u8g2_simsun_24_gb2312);
    writeFont(320, 80,  "℃", u8g2_simsun_24_gb2312);
    writeFont(310, 115, todayWeather.text, u8g2_simsun_24_gb2312);
    display->drawRoundRect(260, 155, 120, 30, 5, GxEPD_BLACK);
    writeFont(270, 162, "体感: " + todayWeather.feelsLike + "℃", u8g2_simsun_16_gb2312);

    int infoY = 270;
    display->drawBitmap(40,  infoY, house_humidity_16x16, 16, 16, GxEPD_BLACK);
    writeFont(65,  infoY, todayWeather.humidity + "%", u8g2_simsun_16_gb2312);
    display->drawBitmap(150, infoY, wi_raindrop_16x16, 16, 16, GxEPD_BLACK);
    writeFont(175, infoY, todayWeather.precip + "mm", u8g2_simsun_16_gb2312);
    const uint8_t* exactWindIcon = getExactWindIcon(todayWeather.wind360);
    display->drawBitmap(260, infoY, exactWindIcon, 16, 16, GxEPD_BLACK);
    writeFont(285, infoY, todayWeather.windDir + " " + todayWeather.windScale + "级", u8g2_simsun_16_gb2312);

    display->drawLine(40, 295, 380, 295, GxEPD_BLACK);
    display->drawBitmap(40,  324, wi_sunrise_16x16, 16, 16, GxEPD_BLACK);
    writeFont(65,  325, "日出 " + forecastWeather[0].sunrise, u8g2_simsun_16_gb2312);
    display->drawBitmap(200, 324, wi_sunset_16x16, 16, 16, GxEPD_BLACK);
    writeFont(225, 325, "日落 " + forecastWeather[0].sunset, u8g2_simsun_16_gb2312);

    // 中间分割线
    display->drawLine(400, 60, 400, 340, GxEPD_BLACK);

    // 右侧：空气质量 (X: 420~780)
    writeFont(430, 80,  "空气质量评估:", u8g2_simsun_24_gb2312);
    writeFont(640, 80,  currentAir.category, u8g2_simsun_24_gb2312);
    writeFont(700, 85,  "AQI: " + currentAir.aqi, u8g2_simsun_16_gb2312);
    writeFont(430, 130, "首要污染源: " + currentAir.primary, u8g2_simsun_16_gb2312);

    int gridX = 430, gridY = 160, gridW = 340, gridH = 150;
    display->drawRect(gridX, gridY, gridW, gridH, GxEPD_BLACK);
    display->drawLine(gridX, gridY + 50, gridX + gridW, gridY + 50, GxEPD_BLACK);
    display->drawLine(gridX, gridY + 100, gridX + gridW, gridY + 100, GxEPD_BLACK);
    display->drawLine(gridX + 170, gridY, gridX + 170, gridY + gridH, GxEPD_BLACK);
    writeFont(gridX + 10,  gridY + 18,  "PM2.5: " + currentAir.pm2p5, u8g2_simsun_16_gb2312);
    writeFont(gridX + 180, gridY + 18,  "PM10: "  + currentAir.pm10,  u8g2_simsun_16_gb2312);
    writeFont(gridX + 10,  gridY + 68,  "O3: "   + currentAir.o3,    u8g2_simsun_16_gb2312);
    writeFont(gridX + 180, gridY + 68,  "NO2: "  + currentAir.no2,   u8g2_simsun_16_gb2312);
    writeFont(gridX + 10,  gridY + 118, "SO2: "  + currentAir.so2,   u8g2_simsun_16_gb2312);
    writeFont(gridX + 180, gridY + 118, "CO: "   + currentAir.co,    u8g2_simsun_16_gb2312);

    // 底部健康建议横幅
    display->drawLine(20, 350, 780, 350, GxEPD_BLACK);
    display->drawLine(20, 352, 780, 352, GxEPD_BLACK);
    display->drawBitmap(20, 370, ionizing_radiation_symbol_64x64, 64, 64, GxEPD_BLACK);

    String safeEffect    = currentAir.healthEffect.length()    > 40 ? currentAir.healthEffect.substring(0, 40)    + "..." : currentAir.healthEffect;
    String safeGeneral   = currentAir.adviceGeneral.length()   > 40 ? currentAir.adviceGeneral.substring(0, 40)   + "..." : currentAir.adviceGeneral;
    String safeSensitive = currentAir.adviceSensitive.length() > 40 ? currentAir.adviceSensitive.substring(0, 40) + "..." : currentAir.adviceSensitive;
    writeFont(90, 385, "健康影响: " + safeEffect,    u8g2_simsun_16_gb2312);
    writeFont(90, 415, "一般人群: " + safeGeneral,   u8g2_simsun_16_gb2312);
    writeFont(90, 445, "敏感人群: " + safeSensitive, u8g2_simsun_16_gb2312);
}

// ====== 界面 1：未来7天详细预报 (来自第二套代码 drawScreen2) ======
void drawScreen2() {
    drawStatusBar();
    int colWidth = 114;
    int startY   = 40;
    display->drawLine(0, startY + 60,  800, startY + 60,  GxEPD_BLACK);
    display->drawLine(0, startY + 140, 800, startY + 140, GxEPD_BLACK);
    display->drawLine(0, startY + 280, 800, startY + 280, GxEPD_BLACK);

    for (int i = 0; i < MAX_FUTURE_DAYS; i++) {
        if (forecastWeather[i].fxdate == "") continue;
        int baseX = i * colWidth;
        if (i > 0) display->drawLine(baseX, startY, baseX, 480, GxEPD_BLACK);

        String shortDate = forecastWeather[i].fxdate.substring(5);
        String weekDay   = getWeekDay(forecastWeather[i].fxdate);
        writeFont(baseX + 25, startY + 14, shortDate, u8g2_simsun_16_gb2312);
        writeFont(baseX + 35, startY + 36, weekDay,   u8g2_simsun_16_gb2312);

        drawDynamicWeatherIcon(baseX + 25, startY + 70, forecastWeather[i].iconDay, 64);

        int infoY = startY + 150;
        String weatherStr = forecastWeather[i].textDay;
        if (forecastWeather[i].textDay != forecastWeather[i].textNight)
            weatherStr += "转" + forecastWeather[i].textNight;
        writeFont(baseX + 5, infoY,      weatherStr, u8g2_simsun_12_gb2312);
        writeFont(baseX + 15, infoY + 25, forecastWeather[i].tempMin + "~" + forecastWeather[i].tempMax + "℃", u8g2_simsun_12_gb2312);

        String windStr = forecastWeather[i].windDirDay;
        if (windStr.length() > 12) windStr = windStr.substring(0, 12);
        writeFont(baseX + 5, infoY + 50,  windStr + forecastWeather[i].windScaleDay + "级", u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, infoY + 75,  "降水: " + forecastWeather[i].precip + "mm",     u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, infoY + 100, "湿度: " + forecastWeather[i].humidity + "%",    u8g2_simsun_12_gb2312);

        int astroY = startY + 295;
        writeFont(baseX + 5, astroY,      "日出 " + forecastWeather[i].sunrise,              u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, astroY + 25, "日落 " + forecastWeather[i].sunset,               u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, astroY + 50, "月相: " + forecastWeather[i].moonPhase,           u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, astroY + 75, "UV指数: " + forecastWeather[i].uvIndex,           u8g2_simsun_12_gb2312);
        writeFont(baseX + 5, astroY + 100,"能见度: " + forecastWeather[i].vis + "km",        u8g2_simsun_12_gb2312);
    }
}

// ====== 界面 2：未来三天空气质量预报 (来自第二套代码 drawScreen3) ======
void drawScreen3() {
    drawStatusBar();
    int rowHeight = 146;
    int startY    = 40;

    for (int i = 0; i < 3; i++) {
        if (airForecasts[i].fxdate == "") continue;
        int currentY = startY + (i * rowHeight);
        if (i > 0) display->drawLine(0, currentY, 800, currentY, GxEPD_BLACK);
        display->drawLine(200, currentY, 200, currentY + rowHeight, GxEPD_BLACK);

        String shortDate = airForecasts[i].fxdate.substring(5);
        String weekDay   = getWeekDay(airForecasts[i].fxdate);
        writeFont(20, currentY + 15,  shortDate + " " + weekDay,      u8g2_simsun_24_gb2312);
        writeFont(20, currentY + 50,  "AQI: "  + airForecasts[i].aqi, u8g2_simsun_24_gb2312);
        writeFont(20, currentY + 87,  "级别: " + airForecasts[i].category, u8g2_simsun_16_gb2312);
        writeFont(20, currentY + 120, "首要污染: " + airForecasts[i].primary, u8g2_simsun_16_gb2312);

        int textStartX = 220, maxWidth = 565, lineHeight = 16;
        drawTextWrap(textStartX, currentY + 28,  "健康影响: " + airForecasts[i].healthEffect,    u8g2_simsun_12_gb2312, maxWidth, lineHeight);
        drawTextWrap(textStartX, currentY + 76,  "一般人群: " + airForecasts[i].adviceGeneral,   u8g2_simsun_12_gb2312, maxWidth, lineHeight);
        drawTextWrap(textStartX, currentY + 124, "敏感人群: " + airForecasts[i].adviceSensitive, u8g2_simsun_12_gb2312, maxWidth, lineHeight);
    }
}

// ====== 界面 3：AI代办事项 (来自第一套代码 drawTodoList) ======
void drawTodoList() {
    drawStatusBar();

    // 外框
    display->drawRoundRect(15, 48, 770, 422, 10, GxEPD_BLACK);

    // 标题
    writeFont(35, 58, "待办事项", u8g2_simsun_24_gb2312);
    display->drawLine(15,  98, 785,  98, GxEPD_BLACK);
    display->drawLine(15, 101, 785, 101, GxEPD_BLACK);

    // ── 核心参数 ──────────────────────────────────
    const int startY     = 110;  // 内容起始Y
    const int lineHeight = 55;   // 修正：从45改为55，给24pt字体足够空间
    const int maxItems   = 6;    // 修正：从8改为6，确保每条都完整显示
    // ─────────────────────────────────────────────

    int lineCount = 0;
    int startIdx  = 0;
    int endIdx    = todoListText.indexOf('\n');

    while (endIdx != -1 && lineCount < maxItems) {
        String line = todoListText.substring(startIdx, endIdx);
        int itemY   = startY + lineCount * lineHeight;

        // 左侧复选框（垂直居中在行内）
        display->drawRoundRect(25, itemY + 12, 18, 18, 3, GxEPD_BLACK);

        // 条目文字
        writeFont(54, itemY + 4, line, u8g2_simsun_24_gb2312);

        // 分割线：画在本条底部上方4px处
        // 距本条文字底部（约itemY+36）有14px间距
        // 距下条文字顶部（itemY+55+4）有19px间距
        // 修正：从 itemY+lineHeight-5 改为 itemY+lineHeight-6
        if (lineCount < maxItems - 1 || endIdx != -1)
            display->drawLine(25, itemY + lineHeight - 6,
                              772, itemY + lineHeight - 6, GxEPD_BLACK);

        startIdx = endIdx + 1;
        endIdx   = todoListText.indexOf('\n', startIdx);
        lineCount++;
    }

    // 最后一行（没有 \n 结尾的情况）
    if (lineCount < maxItems) {
        String lastLine = todoListText.substring(startIdx);
        if (lastLine.length() > 0) {
            int itemY = startY + lineCount * lineHeight;
            display->drawRoundRect(25, itemY + 12, 18, 18, 3, GxEPD_BLACK);
            writeFont(54, itemY + 4, lastLine, u8g2_simsun_24_gb2312);
            lineCount++;
        }
    }

    // 右下角条目计数
    String countStr = "共 " + String(lineCount) + " 项";
    writeFont(660, 448, countStr, u8g2_simsun_16_gb2312);
}

// ====== 界面 4：AI知识伴读卡片 (来自第一套代码 drawFlashcard) ======
void drawFlashcard() {
    drawStatusBar();

    // ── 外框 ──────────────────────────────────────
    display->drawRoundRect(15, 48, 770, 422, 10, GxEPD_BLACK);

    // ── 标题区 ────────────────────────────────────
    // 标题左侧装饰竖条
    display->fillRect(28, 57, 6, 30, GxEPD_BLACK);

    writeFont(44, 58, "AI 知识伴读", u8g2_simsun_24_gb2312);

    // 标题下双分割线
    display->drawLine(15, 98,  785, 98,  GxEPD_BLACK);
    display->drawLine(15, 101, 785, 101, GxEPD_BLACK);

    // ── 内容区左侧装饰线 ──────────────────────────
    display->drawLine(28, 112, 28, 455, GxEPD_BLACK);

    // ── 正文内容 ──────────────────────────────────
    String formattedText = autoWrapUTF8(flashcardText, 22);
    int startY     = 112;
    int lineHeight = 42;
    int lineCount  = 0;
    int startIdx   = 0;
    int endIdx     = formattedText.indexOf('\n');

    while (endIdx != -1 && lineCount < 8) {
        String line = formattedText.substring(startIdx, endIdx);
        writeFont(44, startY + lineCount * lineHeight, line, u8g2_simsun_24_gb2312);
        startIdx = endIdx + 1;
        endIdx   = formattedText.indexOf('\n', startIdx);
        lineCount++;
    }

    if (lineCount < 8) {
        String lastLine = formattedText.substring(startIdx);
        if (lastLine.length() > 0)
            writeFont(44, startY + lineCount * lineHeight, lastLine, u8g2_simsun_24_gb2312);
    }

    // ── 右下角装饰文字 ────────────────────────────
    writeFont(650, 445, "AI 生成 · 仅供参考", u8g2_simsun_12_gb2312);
}

// 界面5：图片画廊 (BW黑白显示)
void drawImageScreen() {
    const unsigned char* current_pic = nullptr;
    switch (currentImageIndex) {
        case 0: current_pic = pic_1; break;
        case 1: current_pic = pic_3; break;
        case 2: current_pic = pic_4; break;
        default: current_pic = pic_1; break;
    }
    if (current_pic == nullptr) {
        writeFont(300, 240, "图片未加载", u8g2_simsun_24_gb2312);
        return;
    }
    // 2-bit图手动转1-bit显示：取高位bit作为黑白
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 800; x++) {
            int byteIdx = (y * 800 + x) / 4;
            int bitShift = 6 - ((y * 800 + x) % 4) * 2;
            uint8_t gray = (current_pic[byteIdx] >> bitShift) & 0x03;
            if (gray < 2) {
                display->drawPixel(x, y, GxEPD_BLACK);
            }
        }
    }
}

// ============================================================
// =================== 网络请求函数 ===========================
// ============================================================

// 通用网络请求 + Gzip自动解压 (来自第二套代码)
String fetchAPIContent(String url) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("User-Agent", "ESP32-Epaper/1.0");
    uint32_t freeHeapBefore = ESP.getFreeHeap();
    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("请求失败，HTTP状态码: %d\n", httpCode);
        http.end();
        return "";
    }
    String payload = http.getString();
    http.end();
    Serial.printf("⬇️ 收到原始报文: %d 字节\n", payload.length());

    if (payload.length() > 2 && (uint8_t)payload[0] == 0x1F && (uint8_t)payload[1] == 0x8B) {
        Serial.println("📦 检测到Gzip压缩，正在解压...");
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return "";
        zs.next_in  = (Bytef*)payload.c_str();
        zs.avail_in = payload.length();
        size_t outBufSize = 6144;
        uint8_t* outBuf = (uint8_t*)malloc(outBufSize);
        if (!outBuf) { inflateEnd(&zs); return ""; }
        zs.next_out  = outBuf;
        zs.avail_out = outBufSize - 1;
        int ret = inflate(&zs, Z_NO_FLUSH);
        String result = "";
        if (ret == Z_OK || ret == Z_STREAM_END) {
            outBuf[zs.total_out] = '\0';
            result = String((char*)outBuf);
            Serial.printf("🔓 解压完毕: %d字节 (内存峰值消耗: %d字节)\n",
                          zs.total_out, freeHeapBefore - ESP.getFreeHeap());
        } else {
            Serial.printf("❌ 解压出错，错误码: %d\n", ret);
        }
        free(outBuf);
        inflateEnd(&zs);
        return result;
    }
    Serial.println("📄 数据未压缩，直接使用明文。");
    return payload;
}

// 获取实时天气 (来自第二套代码)
bool fetchWeatherData() {
    String url = "https://" + String(QWEATHER_HOST) + "/v7/weather/now?location=" + LONGITUDE + "," + LATITUDE + "&key=" + String(QWEATHER_KEY);
    Serial.println("正在请求实时天气...");
    String json = fetchAPIContent(url);
    if (json == "") return false;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (!error && doc["code"] == "200") {
        JsonObject now = doc["now"];
        todayWeather.temp      = now["temp"].as<String>();
        todayWeather.feelsLike = now["feelsLike"].as<String>();
        todayWeather.icon      = now["icon"].as<String>();
        todayWeather.text      = now["text"].as<String>();
        todayWeather.wind360   = now["wind360"].as<String>();
        todayWeather.windDir   = now["windDir"].as<String>();
        todayWeather.windScale = now["windScale"].as<String>();
        todayWeather.windSpeed = now["windSpeed"].as<String>();
        todayWeather.humidity  = now["humidity"].as<String>();
        todayWeather.precip    = now["precip"].as<String>();
        todayWeather.pressure  = now["pressure"].as<String>();
        todayWeather.vis       = now["vis"]   | "--";
        todayWeather.cloud     = now["cloud"] | "--";
        todayWeather.dew       = now["dew"]   | "--";
        Serial.println("✅ 实时天气解析成功！温度: " + todayWeather.temp + "℃");
        return true;
    }
    Serial.println("❌ 实时天气解析失败: " + String(error.c_str()));
    return false;
}

// 获取未来7天预报 (来自第二套代码)
bool fetchForecastData() {
    String url = "https://" + String(QWEATHER_HOST) + "/v7/weather/7d?location=" + LONGITUDE + "," + LATITUDE + "&key=" + String(QWEATHER_KEY);
    Serial.println("正在请求未来预报...");
    String json = fetchAPIContent(url);
    if (json == "") return false;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (!error && doc["code"] == "200") {
        JsonArray daily = doc["daily"];
        int dayIndex = 0;
        for (JsonObject day : daily) {
            if (dayIndex >= MAX_FUTURE_DAYS) break;
            forecastWeather[dayIndex].fxdate         = day["fxDate"].as<String>();
            forecastWeather[dayIndex].tempMax         = day["tempMax"].as<String>();
            forecastWeather[dayIndex].tempMin         = day["tempMin"].as<String>();
            forecastWeather[dayIndex].iconDay         = day["iconDay"].as<String>();
            forecastWeather[dayIndex].textDay         = day["textDay"].as<String>();
            forecastWeather[dayIndex].iconNight       = day["iconNight"].as<String>();
            forecastWeather[dayIndex].textNight       = day["textNight"].as<String>();
            forecastWeather[dayIndex].wind360Day      = day["wind360Day"].as<String>();
            forecastWeather[dayIndex].windDirDay      = day["windDirDay"].as<String>();
            forecastWeather[dayIndex].windScaleDay    = day["windScaleDay"].as<String>();
            forecastWeather[dayIndex].windSpeedDay    = day["windSpeedDay"].as<String>();
            forecastWeather[dayIndex].wind360Night    = day["wind360Night"].as<String>();
            forecastWeather[dayIndex].windDirNight    = day["windDirNight"].as<String>();
            forecastWeather[dayIndex].windScaleNight  = day["windScaleNight"].as<String>();
            forecastWeather[dayIndex].windSpeedNight  = day["windSpeedNight"].as<String>();
            forecastWeather[dayIndex].precip          = day["precip"].as<String>();
            forecastWeather[dayIndex].uvIndex         = day["uvIndex"].as<String>();
            forecastWeather[dayIndex].humidity        = day["humidity"].as<String>();
            forecastWeather[dayIndex].pressure        = day["pressure"].as<String>();
            forecastWeather[dayIndex].vis             = day["vis"].as<String>();
            forecastWeather[dayIndex].cloud           = day["cloud"]     | "--";
            forecastWeather[dayIndex].sunrise         = day["sunrise"]   | "--";
            forecastWeather[dayIndex].sunset          = day["sunset"]    | "--";
            forecastWeather[dayIndex].moonrise        = day["moonrise"]  | "--";
            forecastWeather[dayIndex].moonset         = day["moonset"]   | "--";
            forecastWeather[dayIndex].moonPhase       = day["moonPhase"] | "--";
            forecastWeather[dayIndex].moonPhaseIcon   = day["moonPhaseIcon"] | "--";
            dayIndex++;
        }
        Serial.printf("✅ 未来预报解析成功，共 %d 天\n", dayIndex);
        return true;
    }
    Serial.println("❌ 未来预报解析失败: " + String(error.c_str()));
    return false;
}

// 辅助函数：从V1接口数组中提取污染物浓度 (来自第二套代码)
String getPollutantValue(JsonDocument& doc, const char* targetCode) {
    JsonArray pollutants = doc["pollutants"];
    for (JsonObject pollutant : pollutants) {
        const char* code = pollutant["code"];
        if (code != nullptr && strcmp(code, targetCode) == 0) {
            return pollutant["concentration"]["value"].as<String>();
        }
    }
    return "--";
}

// 获取实时空气质量 (来自第二套代码，适配V1接口)
bool fetchAirQualityData() {
    String url = "https://" + String(QWEATHER_HOST) + "/airquality/v1/current/" + String(LATITUDE) + "/" + String(LONGITUDE) + "?key=" + String(QWEATHER_KEY);
    Serial.println("正在请求实时空气质量...");
    String json = fetchAPIContent(url);
    if (json == "") return false;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (!error && !doc["indexes"].isNull()) {
        JsonObject aqiIndex = doc["indexes"][0];
        currentAir.aqi      = aqiIndex["aqi"].as<String>();
        currentAir.category = aqiIndex["category"].as<String>();
        currentAir.level    = aqiIndex["level"].as<String>();
        if (!aqiIndex["primaryPollutant"].isNull())
            currentAir.primary = aqiIndex["primaryPollutant"]["name"] | "无";
        else
            currentAir.primary = "无";
        currentAir.pm2p5 = getPollutantValue(doc, "pm2p5");
        currentAir.pm10  = getPollutantValue(doc, "pm10");
        currentAir.no2   = getPollutantValue(doc, "no2");
        currentAir.co    = getPollutantValue(doc, "co");
        currentAir.so2   = getPollutantValue(doc, "so2");
        currentAir.o3    = getPollutantValue(doc, "o3");
        if (!aqiIndex["health"].isNull()) {
            JsonObject health = aqiIndex["health"];
            currentAir.healthEffect = health["effect"] | "无";
            if (!health["advice"].isNull()) {
                currentAir.adviceGeneral   = health["advice"]["generalPopulation"]   | "无";
                currentAir.adviceSensitive = health["advice"]["sensitivePopulation"] | "无";
            } else {
                currentAir.adviceGeneral = currentAir.adviceSensitive = "无";
            }
        } else {
            currentAir.healthEffect = currentAir.adviceGeneral = currentAir.adviceSensitive = "无";
        }
        Serial.println("✅ 实时空气解析成功！AQI: " + currentAir.aqi);
        return true;
    }
    Serial.println("❌ 实时空气解析失败: " + String(error.c_str()));
    return false;
}

// 获取未来空气质量预报 (来自第二套代码，适配V1接口)
bool fetchAirForecastData() {
    String url = "https://" + String(QWEATHER_HOST) + "/airquality/v1/daily/" + String(LATITUDE) + "/" + String(LONGITUDE) + "?key=" + String(QWEATHER_KEY);
    Serial.println("正在请求未来空气质量预报...");
    String json = fetchAPIContent(url);
    if (json == "") return false;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (!error && !doc["days"].isNull()) {
        JsonArray days = doc["days"];
        int i = 0;
        for (JsonObject day : days) {
            if (i >= 3) break;
            String fullDate = day["forecastStartTime"].as<String>();
            airForecasts[i].fxdate = (fullDate.length() >= 10) ? fullDate.substring(0, 10) : "--";
            JsonObject aqiIndex    = day["indexes"][0];
            airForecasts[i].aqi      = aqiIndex["aqi"].as<String>();
            airForecasts[i].category = aqiIndex["category"] | "--";
            airForecasts[i].level    = aqiIndex["level"]    | "--";
            if (!aqiIndex["primaryPollutant"].isNull())
                airForecasts[i].primary = aqiIndex["primaryPollutant"]["name"] | "无";
            else
                airForecasts[i].primary = "无";
            if (!aqiIndex["health"].isNull()) {
                JsonObject health = aqiIndex["health"];
                airForecasts[i].healthEffect = health["effect"] | "无";
                if (!health["advice"].isNull()) {
                    airForecasts[i].adviceGeneral   = health["advice"]["generalPopulation"]   | "无";
                    airForecasts[i].adviceSensitive = health["advice"]["sensitivePopulation"] | "无";
                } else {
                    airForecasts[i].adviceGeneral = airForecasts[i].adviceSensitive = "无";
                }
            } else {
                airForecasts[i].healthEffect = airForecasts[i].adviceGeneral = airForecasts[i].adviceSensitive = "无";
            }
            i++;
        }
        Serial.printf("✅ 未来空气预报解析成功，共 %d 天\n", i);
        return true;
    }
    Serial.println("❌ 未来空气预报解析失败: " + String(error.c_str()));
    return false;
}

// ============================================================
// =================== Web服务器处理 ==========================
// ============================================================

// 主页：返回手机端操作界面
void handleRoot() {
    String html = R"rawhtml(
<!DOCTYPE html>
<html lang='zh'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>
<title>墨水屏控制台</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, sans-serif;
    background: #f5f5f7;
    color: #1d1d1f;
    padding: 20px;
    max-width: 500px;
    margin: 0 auto;
  }
  h2 {
    font-size: 22px;
    font-weight: 600;
    margin: 20px 0 6px;
    color: #1d1d1f;
  }
  .card {
    background: #fff;
    border-radius: 16px;
    padding: 18px;
    margin-bottom: 16px;
    box-shadow: 0 1px 4px rgba(0,0,0,0.08);
  }
  .card h3 {
    font-size: 15px;
    color: #86868b;
    margin-bottom: 12px;
    font-weight: 500;
  }
  textarea, input[type=text] {
    width: 100%;
    padding: 12px;
    border: 1.5px solid #d2d2d7;
    border-radius: 10px;
    font-size: 15px;
    outline: none;
    resize: none;
    font-family: inherit;
    transition: border-color 0.2s;
  }
  textarea:focus, input[type=text]:focus {
    border-color: #0071e3;
  }
  button {
    width: 100%;
    padding: 14px;
    border: none;
    border-radius: 12px;
    font-size: 16px;
    font-weight: 600;
    cursor: pointer;
    margin-top: 10px;
    transition: opacity 0.15s;
  }
  button:active { opacity: 0.7; }
  .btn-blue  { background: #0071e3; color: #fff; }
  .btn-green { background: #34c759; color: #fff; }
  .screen-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .btn-screen {
    background: #f5f5f7;
    color: #1d1d1f;
    border: 1.5px solid #d2d2d7;
    border-radius: 12px;
    padding: 14px 8px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    text-align: center;
    transition: all 0.15s;
  }
  .btn-screen:active {
    background: #0071e3;
    color: #fff;
    border-color: #0071e3;
  }
  .status-bar {
    background: #fff;
    border-radius: 16px;
    padding: 14px 18px;
    margin-bottom: 16px;
    box-shadow: 0 1px 4px rgba(0,0,0,0.08);
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 14px;
    color: #86868b;
  }
  .status-val { color: #1d1d1f; font-weight: 600; }
  .toast {
    position: fixed;
    bottom: 30px;
    left: 50%;
    transform: translateX(-50%);
    background: rgba(0,0,0,0.75);
    color: #fff;
    padding: 10px 22px;
    border-radius: 20px;
    font-size: 14px;
    display: none;
    z-index: 999;
  }
</style>
</head>
<body>

<h2>墨水屏控制台</h2>

<!-- 状态栏 -->
<div class='status-bar'>
  <span>当前温度</span>
  <span class='status-val' id='temp'>加载中...</span>
  <span>空气质量</span>
  <span class='status-val' id='aqi'>加载中...</span>
</div>

<!-- 代办事项 -->
<div class='card'>
  <h3>📋 推送代办事项到屏幕</h3>
  <textarea id='todoInput' rows='5'
    placeholder='每行一条，例如：&#10;买菜&#10;回复邮件&#10;下午3点开会'></textarea>
  <button class='btn-blue' onclick='pushTodo()'>同步到墨水屏</button>
</div>

<!-- 知识卡片 -->
<div class='card'>
  <h3>💡 发送问题生成知识卡片</h3>
  <input type='text' id='questionInput' placeholder='例如：解释一下什么是MQTT'>
  <button class='btn-green' onclick='pushQuestion()'>生成知识卡片</button>
</div>

<!-- 切换界面 -->
<div class='card'>
  <h3>🖥️ 切换墨水屏界面</h3>
  <div class='screen-grid'>
    <div class='btn-screen' onclick='switchScreen(0)'>🌤️ 实时天气</div>
    <div class='btn-screen' onclick='switchScreen(1)'>📅 7天预报</div>
    <div class='btn-screen' onclick='switchScreen(2)'>🌫️ 空气质量</div>
    <div class='btn-screen' onclick='switchScreen(3)'>📋 代办事项</div>
    <div class='btn-screen' onclick='switchScreen(4)'>💡 知识卡片</div>
    <div class='btn-screen' onclick='switchScreen(5)'>🖼️ 图片画廊</div>
  </div>
</div>

<div class='toast' id='toast'></div>

<script>
function showToast(msg) {
  var t = document.getElementById('toast');
  t.innerText = msg;
  t.style.display = 'block';
  setTimeout(function(){ t.style.display = 'none'; }, 2000);
}

function pushTodo() {
  var text = document.getElementById('todoInput').value.trim();
  if (!text) { showToast('请输入待办内容'); return; }
  fetch('/push_todo', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'content=' + encodeURIComponent(text)
  }).then(function(r){ return r.text(); })
    .then(function(){ showToast('✅ 已同步到墨水屏'); })
    .catch(function(){ showToast('❌ 发送失败'); });
}

function pushQuestion() {
  var q = document.getElementById('questionInput').value.trim();
  if (!q) { showToast('请输入问题'); return; }
  fetch('/push_question', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'content=' + encodeURIComponent(q)
  }).then(function(r){ return r.text(); })
    .then(function(){ showToast('✅ 正在生成知识卡片...'); })
    .catch(function(){ showToast('❌ 发送失败'); });
}

function switchScreen(n) {
  fetch('/switch?screen=' + n)
    .then(function(){ showToast('✅ 界面已切换'); })
    .catch(function(){ showToast('❌ 切换失败'); });
}

// 自动加载天气状态
function loadStatus() {
  fetch('/status')
    .then(function(r){ return r.json(); })
    .then(function(d){
      document.getElementById('temp').innerText = d.temp + '°C';
      document.getElementById('aqi').innerText  = d.aqi + ' ' + d.category;
    }).catch(function(){});
}
loadStatus();
</script>
</body>
</html>
)rawhtml";
    server.send(200, "text/html; charset=utf-8", html);
}

// 接收手机推送的代办事项
void handlePushTodo() {
    if (server.hasArg("content")) {
        webPushTodo = server.arg("content");
        // 把手机输入的换行符统一处理
        webPushTodo.replace("\r\n", "\n");
        webPushTodo.replace("\r", "\n");

        // 自动给每行加序号（如果没有的话）
        String numbered = "";
        int idx = 1;
        int s = 0, e = webPushTodo.indexOf('\n');
        while (e != -1) {
            String line = webPushTodo.substring(s, e);
            line.trim();
            if (line.length() > 0) {
                // 检查是否已有数字开头，没有则自动加
                if (!isDigit(line[0]))
                    numbered += String(idx++) + ". " + line + "\n";
                else
                    numbered += line + "\n";
            }
            s = e + 1;
            e = webPushTodo.indexOf('\n', s);
        }
        String last = webPushTodo.substring(s);
        last.trim();
        if (last.length() > 0) {
            if (!isDigit(last[0]))
                numbered += String(idx++) + ". " + last;
            else
                numbered += last;
        }

        webPushTodo  = numbered;
        webHasTodo   = true;
        server.send(200, "text/plain", "ok");
    } else {
        server.send(400, "text/plain", "missing content");
    }
}

// 接收手机推送的知识卡片问题
void handlePushQuestion() {
    if (server.hasArg("content")) {
        webPushQuestion = server.arg("content");
        webHasQuestion  = true;
        server.send(200, "text/plain", "ok");
    } else {
        server.send(400, "text/plain", "missing content");
    }
}

// 远程切换界面
void handleSwitchScreen() {
    if (server.hasArg("screen")) {
        int s = server.arg("screen").toInt();
        if (s >= 0 && s <= 4) {
            currentScreen = s;
            isImageMode   = false;
            needsUpdate   = true;
            server.send(200, "text/plain", "ok");
        } else if (s == 5) {
            isImageMode       = true;
            currentImageIndex = 0;
            needsUpdate       = true;
            server.send(200, "text/plain", "ok");
        } else {
            server.send(400, "text/plain", "invalid screen");
        }
    }
}

// 返回当前天气状态给手机
void handleStatus() {
    String json = "{";
    json += "\"temp\":\""     + todayWeather.temp     + "\",";
    json += "\"aqi\":\""      + currentAir.aqi        + "\",";
    json += "\"category\":\"" + currentAir.category   + "\",";
    json += "\"weather\":\""  + todayWeather.text     + "\",";
    json += "\"humidity\":\"" + todayWeather.humidity + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// 初始化Web服务器路由
void initWebServer() {
    server.on("/",              HTTP_GET,  handleRoot);
    server.on("/push_todo",     HTTP_POST, handlePushTodo);
    server.on("/push_question", HTTP_POST, handlePushQuestion);
    server.on("/switch",        HTTP_GET,  handleSwitchScreen);
    server.on("/status",        HTTP_GET,  handleStatus);

        server.on("/favicon.ico",   HTTP_GET,  [](){
        server.send(204, "text/plain", "");
        });

    server.begin();
    Serial.println("Web服务器已启动");
}

// 统一数据拉取入口 (来自第二套代码)
void fetchAllData() {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("网络就绪，开始拉取所有天气数据...");
        fetchWeatherData();
        fetchForecastData();
        fetchAirQualityData();
        fetchAirForecastData();
        Serial.println("🎉 数据拉取完成！");
    } else {
        Serial.println("⚠️ WiFi未连接，跳过数据拉取。");
    }
}

// 调试：打印所有数据到串口 (来自第二套代码)
void printAllDebugData() {
    Serial.println("\n========================================");
    Serial.println(">>> 当前全局结构体数据展示 <<<");
    Serial.println("\n[实时天气]");
    Serial.printf("温度:%s℃ | 体感:%s℃ | 状况:%s(图标代码:%s) | 湿度:%s%%\n",
                  todayWeather.temp.c_str(), todayWeather.feelsLike.c_str(),
                  todayWeather.text.c_str(), todayWeather.icon.c_str(), todayWeather.humidity.c_str());
    Serial.printf("风向:%s(%s°) %s级 %skm/h | 压强:%shPa | 能见度:%skm\n",
                  todayWeather.windDir.c_str(), todayWeather.wind360.c_str(),
                  todayWeather.windScale.c_str(), todayWeather.windSpeed.c_str(),
                  todayWeather.pressure.c_str(), todayWeather.vis.c_str());
    Serial.println("\n[实时空气质量]");
    Serial.printf("AQI:%s | 级别:%s(%s级) | 首要污染物:%s\n",
                  currentAir.aqi.c_str(), currentAir.category.c_str(),
                  currentAir.level.c_str(), currentAir.primary.c_str());
    Serial.printf("PM2.5:%s | PM10:%s | NO2:%s | SO2:%s | O3:%s | CO:%s\n",
                  currentAir.pm2p5.c_str(), currentAir.pm10.c_str(), currentAir.no2.c_str(),
                  currentAir.so2.c_str(), currentAir.o3.c_str(), currentAir.co.c_str());
    Serial.println("\n[未来天气预报]");
    for (int i = 0; i < MAX_FUTURE_DAYS; i++) {
        if (forecastWeather[i].fxdate == "") break;
        String weekDay = getWeekDay(forecastWeather[i].fxdate);
        Serial.printf("  Day %d [%s %s]: %s~%s℃ | 白天:%s | 夜间:%s\n",
                      i+1, forecastWeather[i].fxdate.c_str(), weekDay.c_str(),
                      forecastWeather[i].tempMin.c_str(), forecastWeather[i].tempMax.c_str(),
                      forecastWeather[i].textDay.c_str(), forecastWeather[i].textNight.c_str());
    }
    Serial.println("\n[未来空气预报]");
    for (int i = 0; i < 3; i++) {
        if (airForecasts[i].fxdate == "") break;
        String weekDay = getWeekDay(airForecasts[i].fxdate);
        Serial.printf("  Day %d [%s %s]: AQI:%s | %s | 首要:%s\n",
                      i+1, airForecasts[i].fxdate.c_str(), weekDay.c_str(),
                      airForecasts[i].aqi.c_str(), airForecasts[i].category.c_str(),
                      airForecasts[i].primary.c_str());
    }
    Serial.println("========================================\n");
}


// ============================================================
// =================== AI相关函数 =============================
// ============================================================

// HMAC-SHA256鉴权URL生成 (来自第一套代码)
String getUrl(String Spark_url, String host, String path, String Date) {
    String signature_origin = "host: " + host + "\n";
    signature_origin += "date: " + Date + "\n";
    signature_origin += "GET " + path + " HTTP/1.1";
    unsigned char hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    const size_t messageLength = signature_origin.length();
    const size_t keyLength     = APISecret.length();
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)APISecret.c_str(), keyLength);
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)signature_origin.c_str(), messageLength);
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    String signature_sha_base64 = base64::encode(hmac, sizeof(hmac) / sizeof(hmac[0]));
    Date.replace(",", "%2C");
    Date.replace(" ", "+");
    Date.replace(":", "%3A");
    String authorization_origin = "api_key=\"" + APIKey + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + signature_sha_base64 + "\"";
    String authorization = base64::encode(authorization_origin);
    String url = Spark_url + '?' + "authorization=" + authorization + "&date=" + Date + "&host=" + host;
    return url;
}

// 从百度服务器获取时间戳（用于AI鉴权） (来自第一套代码)
void getTimeFromServer() {
    String timeurl = "https://www.baidu.com";
    HTTPClient http;
    http.begin(timeurl);
    const char *headerKeys[] = {"Date"};
    http.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));
    int httpCode = http.GET();
    Date = http.header("Date");
    Serial.println("鉴权时间戳: " + Date);
    http.end();
}

// 添加对话历史 (来自第一套代码)
void getText(String role, String content) {
    checkLen(text);
    DynamicJsonDocument jsoncon(1024);
    jsoncon["role"]    = role;
    jsoncon["content"] = content;
    text.add(jsoncon);
    jsoncon.clear();
    String serialized;
    serializeJson(text, serialized);
    Serial.print("text: "); Serial.println(serialized);
}

// 计算对话历史总长度 (来自第一套代码)
int getLength(JsonArray textArray) {
    int length = 0;
    for (JsonObject content : textArray) {
        const char *temp = content["content"];
        length += strlen(temp);
    }
    return length;
}

// 超长时裁剪对话历史 (来自第一套代码)
void checkLen(JsonArray textArray) {
    while (getLength(textArray) > 3000) {
        textArray.remove(0);
    }
}

// 构建星火API请求体 (来自第一套代码)
DynamicJsonDocument gen_params(const char *appid, const char *domain) {
    DynamicJsonDocument data(2048);
    JsonObject header     = data.createNestedObject("header");
    header["app_id"] = appid;
    header["uid"]    = "1234";
    JsonObject parameter = data.createNestedObject("parameter");
    JsonObject chat      = parameter.createNestedObject("chat");
    chat["domain"]      = domain;
    chat["temperature"] = 0.2;
    chat["max_tokens"]  = 256;
    JsonObject payload   = data.createNestedObject("payload");
    JsonObject message   = payload.createNestedObject("message");
    JsonArray textArray  = message.createNestedArray("text");
    for (const auto &item : text) textArray.add(item);
    return data;
}


// ============================================================
// =================== WebSocket 回调函数 =====================
// ============================================================

// 回调：接收星火大模型的AI回答 (来自第一套代码，完整保留所有功能)
void onMessageCallback(WebsocketsMessage message) {
    StaticJsonDocument<4096> jsonDocument;
    DeserializationError error = deserializeJson(jsonDocument, message.data());
    if (!error) {
        int code = jsonDocument["header"]["code"];
        if (code != 0) {
            Serial.print("sth is wrong: "); Serial.println(code);
            Serial.println(message.data());
            webSocketClient.close();
        } else {
            receiveFrame++;
            Serial.print("receiveFrame:"); Serial.println(receiveFrame);
            JsonObject choices = jsonDocument["payload"]["choices"];
            int status         = choices["status"];
            const char *content = choices["text"][0]["content"];
            Serial.println(content);
            Answer += content;
            String answer = "";

            // 流式TTS播放（代办/代码注入模式下屏蔽）
            if (Answer.length() >= 120 && (audio2.isplaying == 0) && mainStatus == 0) {
                String subAnswer = Answer.substring(0, 120);
                int lastPeriodIndex = subAnswer.lastIndexOf("。");
                if (lastPeriodIndex != -1) {
                    answer = Answer.substring(0, lastPeriodIndex + 1);
                    Answer = Answer.substring(lastPeriodIndex + 2);
                    audio2.connecttospeech(answer.c_str(), "zh");
                } else {
                    const char *chinesePunctuation = "？，：；,.";
                    int lastChineseSentenceIndex = -1;
                    for (int i = 0; i < Answer.length(); ++i) {
                        if (strchr(chinesePunctuation, Answer.charAt(i)) != NULL)
                            lastChineseSentenceIndex = i;
                    }
                    if (lastChineseSentenceIndex != -1) {
                        answer = Answer.substring(0, lastChineseSentenceIndex + 1);
                        audio2.connecttospeech(answer.c_str(), "zh");
                        Answer = Answer.substring(lastChineseSentenceIndex + 2);
                    } else {
                        answer = Answer.substring(0, 120);
                        audio2.connecttospeech(answer.c_str(), "zh");
                        Answer = Answer.substring(121);
                    }
                }
                startPlay = true;
            }

            // 接收完毕处理
            if (status == 2) {
                getText("assistant", Answer);

                // 普通聊天尾音
                if (Answer.length() > 0 && (audio2.isplaying == 0) && mainStatus == 0)
                    audio2.connecttospeech(Answer.c_str(), "zh");

                // 代办事项完成：触发屏幕刷新 + 播报通知
                if (mainStatus == 2) {
                    todoListText       = Answer;
                    currentScreen      = 3;
                    needsUpdate        = true;   // UI任务负责刷屏
                    mainStatus         = 0;
                }

                // 知识卡片完成：触发屏幕刷新
                if (mainStatus == 3) {
                    flashcardText          = Answer;
                    currentScreen          = 4;
                    needsUpdate            = true;
                    mainStatus             = 0;
                }
            }
        }
    }
}

// 回调：星火API连接事件 (来自第一套代码)
void onEventsCallback(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Send message to server0!");
        DynamicJsonDocument jsonData = gen_params(appId1, domain1);
        String jsonString;
        serializeJson(jsonData, jsonString);
        Serial.println(jsonString);
        webSocketClient.send(jsonString);
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection0 Closed");
    }
}

// 回调：接收讯飞语音听写结果 (来自第一套代码，完整保留所有意图识别)
void onMessageCallback1(WebsocketsMessage message) {
    StaticJsonDocument<4096> jsonDocument;
    DeserializationError error = deserializeJson(jsonDocument, message.data());
    if (!error) {
        int code = jsonDocument["code"];
        if (code != 0) {
            Serial.println(code); Serial.println(message.data());
            webSocketClient1.close();
        } else {
            Serial.println("xunfeiyun return message:");
            Serial.println(message.data());
            JsonArray ws = jsonDocument["data"]["result"]["ws"].as<JsonArray>();
            for (JsonVariant i : ws)
                for (JsonVariant w : i["cw"].as<JsonArray>())
                    askquestion += w["w"].as<String>();
            Serial.println(askquestion);

            int status = jsonDocument["data"]["status"];
            if (status == 2) {
                Serial.println("status == 2");
                webSocketClient1.close();

                if (askquestion == "") {
                    askquestion = "sorry, i can't hear you";
                    // audio2.connecttospeech(askquestion.c_str(), "zh");
                }
                // --- 点歌意图 ---
                else if (askquestion.substring(0, 9) == "唱歌了" || askquestion.substring(0, 9) == "唱歌啦") {
                    if (askquestion.substring(0, 12) == "唱歌了，" || askquestion.substring(0, 12) == "唱歌啦，") {
                        String song = askquestion.substring(12, askquestion.length() - 3);
                        playNetEaseMusic(song);
                    } else if (askquestion.substring(9) == "。") {
                        askquestion = "好啊, 你想听什么歌？";
                        mainStatus = 1;
                        audio2.connecttospeech(askquestion.c_str(), "zh");
                    } else {
                        String song = askquestion.substring(9, askquestion.length() - 3);
                        playNetEaseMusic(song);
                    }
                }
                // --- 点歌状态接收歌名 ---
                else if (mainStatus == 1) {
                    askquestion.trim();
                    if (askquestion.endsWith("。"))      askquestion = askquestion.substring(0, askquestion.length() - 3);
                    else if (askquestion.endsWith(".") || askquestion.endsWith("?"))
                                                          askquestion = askquestion.substring(0, askquestion.length() - 1);
                    playNetEaseMusic(askquestion);
                    mainStatus = 0;
                }
                // --- 代办事项意图 ---
                else if (askquestion.indexOf("提醒我") != -1 || askquestion.indexOf("记一下") != -1
                        || askquestion.indexOf("追加") != -1) {
                    String currentList = (todoListText == "") ? "目前为空" : todoListText;
                    String prompt = "我目前的待办列表是：\n" + currentList +
                                    "\n现在我的新语音指令是：\"" + askquestion +
                                    "\"。请根据我的新指令，对原列表进行追加或修改。" +
                                    "请直接输出更新后的完整编号列表(例如: 1. xxx \n 2. xxx)，绝对不要输出任何废话。";
                    getText("user", prompt);
                    Answer = ""; lastsetence = false; isReady = true;
                    mainStatus = 2;
                    ConnServer();
                }
                // --- 知识卡片意图 ---
                else if (askquestion.indexOf("解释一下") != -1 ) {
                    String prompt = "请扮演一名资深工程师，向初学者通俗解释：\"" + askquestion + "\"。" +
                                    "要求语言精炼、直击核心。总字数控制在 150 字左右，不要废话。";
                    getText("user", prompt);
                    Answer = ""; lastsetence = false; isReady = true;
                    mainStatus = 3;
                    ConnServer();
                }
                // --- 电子书意图 ---
                else if (askquestion.indexOf("打开电子书") != -1) {
                    needsEbookStart = true;
                }
                else if (askquestion.indexOf("退出电子书") != -1 || askquestion.indexOf("关闭电子书") != -1) {
                    needsEbookStop = true;
                }
                // --- 代码注入意图 ---
                // --- 普通问答 ---
                else {
                    getText("user", "请回答"" + askquestion + ""尽量简短,20字以内");
                    Answer = ""; lastsetence = false; isReady = true;
                    mainStatus = 0;
                    ConnServer();
                }
            }
        }
    } else {
        Serial.println("error: "); Serial.println(error.c_str());
        Serial.println(message.data());
    }
}

// 回调：讯飞语音听写连接事件 (来自第一套代码，完整保留录音逻辑)
void onEventsCallback1(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Send message to xunfeiyun");
        int silence = 0, firstframe = 1, j = 0, voicebegin = 0, voice = 0;
        DynamicJsonDocument doc(2500);
        while (1) {
            doc.clear();
            JsonObject dataObj = doc.createNestedObject("data");
            audio1.Record();
            float rms = calculateRMS((uint8_t *)audio1.wavData[0], 1280);

            if (rms < noise) {
                if (voicebegin == 1) silence++;
            } else {
                voice++;
                if (voice >= 5) voicebegin = 1;
                else            voicebegin = 0;
                silence = 0;
            }

            if (silence == 6) {
                dataObj["status"]   = 2;
                dataObj["format"]   = "audio/L16;rate=8000";
                dataObj["audio"]    = base64::encode((byte *)audio1.wavData[0], 1280);
                dataObj["encoding"] = "raw";
                j++;
                String jsonString; serializeJson(doc, jsonString);
                webSocketClient1.send(jsonString);
                delay(40); break;
            }

            if (firstframe == 1) {
                dataObj["status"]   = 0;
                dataObj["format"]   = "audio/L16;rate=8000";
                dataObj["audio"]    = base64::encode((byte *)audio1.wavData[0], 1280);
                dataObj["encoding"] = "raw";
                j++;
                JsonObject common   = doc.createNestedObject("common");
                common["app_id"]    = appId1;
                JsonObject business = doc.createNestedObject("business");
                business["domain"]   = "iat";
                business["language"] = "zh_cn";
                business["accent"]   = "mandarin";
                business["vinfo"]    = 1;
                business["vad_eos"]  = 1000;
                String jsonString; serializeJson(doc, jsonString);
                webSocketClient1.send(jsonString);
                firstframe = 0;
                delay(40);
            } else {
                dataObj["status"]   = 1;
                dataObj["format"]   = "audio/L16;rate=8000";
                dataObj["audio"]    = base64::encode((byte *)audio1.wavData[0], 1280);
                dataObj["encoding"] = "raw";
                String jsonString; serializeJson(doc, jsonString);
                webSocketClient1.send(jsonString);
                delay(40);
            }
        }
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection1 Closed");
    }
}


// ============================================================
// =================== 网络/音频辅助函数 ======================
// ============================================================

// 连接星火AI WebSocket (来自第一套代码)
void ConnServer() {
    Serial.println("url:" + url);
    webSocketClient.onMessage(onMessageCallback);
    webSocketClient.onEvent(onEventsCallback);
    Serial.println("Begin connect to server0......");
    if (webSocketClient.connect(url.c_str())) Serial.println("Connected to server0!");
    else                                       Serial.println("Failed to connect to server0!");
}

// 连接讯飞语音听写 WebSocket (来自第一套代码)
void ConnServer1() {
    webSocketClient1.onMessage(onMessageCallback1);
    webSocketClient1.onEvent(onEventsCallback1);
    Serial.println("Begin connect to server1......");
    if (webSocketClient1.connect(url1.c_str())) Serial.println("Connected to server1!");
    else                                         Serial.println("Failed to connect to server1!");
}

// 流式语音分段播放 (来自第一套代码)
void voicePlay() {
    if ((audio2.isplaying == 0) && Answer != "") {
        int firstPeriodIndex  = Answer.indexOf("。");
        int secondPeriodIndex = 0;
        if (firstPeriodIndex != -1) {
            secondPeriodIndex = Answer.indexOf("。", firstPeriodIndex + 1);
            if (secondPeriodIndex == -1) secondPeriodIndex = firstPeriodIndex;
        } else {
            secondPeriodIndex = firstPeriodIndex;
        }
        if (secondPeriodIndex != -1) {
            String answer = Answer.substring(0, secondPeriodIndex + 1);
            Answer = Answer.substring(secondPeriodIndex + 2);
            audio2.connecttospeech(answer.c_str(), "zh");
        } else {
            const char *chinesePunctuation = "？，：；,.";
            int lastChineseSentenceIndex = -1;
            for (int i = 0; i < Answer.length(); ++i)
                if (strchr(chinesePunctuation, Answer.charAt(i)) != NULL)
                    lastChineseSentenceIndex = i;
            if (lastChineseSentenceIndex != -1) {
                String answer = Answer.substring(0, lastChineseSentenceIndex + 1);
                audio2.connecttospeech(answer.c_str(), "zh");
                Answer = Answer.substring(lastChineseSentenceIndex + 2);
            }
        }
        startPlay = true;
    }
}

// WiFi连接（支持多SSID + 强制公共DNS，融合两套代码）
void wifiConnect(const char *wifiData[][2], int numNetworks) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);


    for (int i = 0; i < numNetworks; ++i) {
        const char *ssid     = wifiData[i][0];
        const char *password = wifiData[i][1];
        Serial.print("Connecting to "); Serial.println(ssid);
        WiFi.begin(ssid, password);
        int count = 0;
        while (WiFi.status() != WL_CONNECTED) {
            Serial.print(".");
            count++;
            if (count >= 40) { Serial.println("\r\n-- wifi connect fail! --"); break; }
            vTaskDelay(200);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\r\n-- wifi connect success! --\r\n");
            Serial.print("IP address: "); Serial.println(WiFi.localIP());
            Serial.println("Free Heap: " + String(ESP.getFreeHeap()));
            // 强制配置公共DNS，防止路由器默认DNS解析失败 (来自第二套代码优化)
            return;
        }
    }
    Serial.println("所有WiFi均连接失败！");
}

// 网易云音乐搜索+播放 (来自第一套代码)
void playNetEaseMusic(String songName) {
    Serial.println("\n正在网易云搜索: " + songName);
    String searchUrl = "http://music.163.com/api/search/get/web?s=" + urlEncode(songName) + "&type=1&limit=1";
    HTTPClient http;
    http.begin(searchUrl);
    http.addHeader("User-Agent", "Mozilla/5.0");
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument jsonDoc(4096);
        DeserializationError error = deserializeJson(jsonDoc, payload);
        if (!error) {
            long songId = jsonDoc["result"]["songs"][0]["id"];
            if (songId > 0) {
                String playUrl = "http://music.163.com/song/media/outer/url?id=" + String(songId) + ".mp3";
                Serial.println("搜索成功！播放链接: " + playUrl);

            // ← 新增：记录歌名，标记正在播放
                currentSongName = songName;
                isMusicPlaying  = true;
                needsUpdate     = true;   // 通知UI任务刷屏

                audio2.connecttohost(playUrl.c_str());
            } else {
                isMusicPlaying  = false;
                currentSongName = "";
                Serial.println("未找到该歌曲");
                audio2.connecttospeech("抱歉，没有搜到这首歌", "zh");
            }
        } else {
            Serial.println("JSON解析失败");
        }
    } else {
        Serial.println("请求网易云失败，错误码: " + String(httpCode));
        audio2.connecttospeech("网络请求失败", "zh");
    }
    http.end();
}


// ============================================================
// =================== AI核心函数 =============================
// ============================================================
// 注意：此函数运行在Core 1的AI任务中
// 不涉及任何显示操作（显示由Core 0的UI任务负责）
// ============================================================

static bool lastKeyState = HIGH;
bool currentKeyState = digitalRead(key);

void AI() {
        // ── 新增：处理来自Core 0的AI连接请求 ──
    if (webNeedsAIConnect) {
        webNeedsAIConnect = false;
        ConnServer();  // 在Core 1里执行，与poll()同核
    }
    webSocketClient.poll();
    webSocketClient1.poll();

    // 流式音频播放
    if (startPlay) voicePlay();

    audio2.loop();

    // 播放指示灯控制
    if (audio2.isplaying == 1) {
        digitalWrite(led3, HIGH);
    } else {
        digitalWrite(led3, LOW);

            // ← 新增：检测音乐播放结束，恢复原界面
        if (isMusicPlaying) {
            isMusicPlaying  = false;
            currentSongName = "";
            needsUpdate     = true;   // 通知UI任务恢复原界面
            }

        // 定期刷新鉴权URL（每4分钟）
        if ((urlTime + 240000 < millis()) && (audio2.isplaying == 0)) {
            urlTime = millis();
            getTimeFromServer();
            url  = getUrl("ws://spark-api.xf-yun.com/v4.0/chat", "spark-api.xf-yun.com", "/v4.0/chat", Date);
            url1 = getUrl("ws://ws-api.xfyun.cn/v2/iat", "ws-api.xfyun.cn", "/v2/iat", Date);
        }
    }

    // 录音唤醒按键（GPIO 0，接GND触发）
    if (digitalRead(key) == 0) {
            // 真正停止当前音频流：音乐和普通语音播报都会被打断
    if (audio2.isplaying) {
        audio2.stopSong();
    }

    // 只有正在播放音乐时，才退出音乐页面
    if (isMusicPlaying) {
        Serial.println("[Music] 录音键：停止音乐播放");

        isMusicPlaying  = false;
        currentSongName = "";
        needsUpdate     = true;
    }
        startPlay = false;
        isReady   = false;
        Answer    = "";
        Serial.printf("Start recognition\r\n\r\n");
        adc_start_flag = 1;

        if (urlTime + 240000 < millis()) {
            urlTime = millis();
            getTimeFromServer();
            url  = getUrl("wss://spark-api.xf-yun.com/v4.0/chat", "spark-api.xf-yun.com", "/v4.0/chat", Date);
            url1 = getUrl("ws://ws-api.xfyun.cn/v2/iat", "ws-api.xfyun.cn", "/v2/iat", Date);
        }
        askquestion = "";
        ConnServer1();
        adc_complete_flag = 0;
    }
}

// ============================================================
// =================== FreeRTOS 任务 ==========================
// ============================================================

// 任务一：AI 语音对话（运行在 Core 1）
// 负责：WebSocket轮询、语音播放、USB键盘注入、AI标志管理
void taskAICore(void *pvParameters) {
    while (1) {
        AI();
        vTaskDelay(10);
    }
}

// 任务二：墨水屏UI切换（运行在 Core 0）
// 负责：按键检测、界面切换、屏幕刷新、天气数据定时刷新、时间局刷
// 任务二：墨水屏UI切换（运行在 Core 0）
void taskUICore(void *pvParameters) {
    // 等待系统完全稳定
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    bool firstBoot = true;
    unsigned long lastPartialUpdate = 0;
    unsigned long lastWeatherFetch  = 0;

    // --- 核心：记录上一帧状态，用于边缘检测 ---
    static bool lastNextBtnState = HIGH;
    static bool lastOkBtnState   = HIGH;

    while (1) {
        // --- 1. 统一触发信号（本循环全局可用） ---
        bool nextTriggered = false;
        bool okTriggered   = false;

        // --- 2. 读取并处理物理按键 (下降沿触发防死锁) ---
        bool currentNextBtn = digitalRead(BUTTON_PIN);
        bool currentOkBtn   = digitalRead(PIN_IMAGE_BUTTON);

        // 只有从“松开(HIGH)”变成“按下(LOW)”的瞬间才算触发
        if (currentNextBtn == LOW && lastNextBtnState == HIGH) {
            nextTriggered = true;
        }
        if (currentOkBtn == LOW && lastOkBtnState == HIGH) {
            okTriggered = true;
        }
        
        // 保存当前物理按键状态供下一帧对比
        lastNextBtnState = currentNextBtn;
        lastOkBtnState   = currentOkBtn;

        // --- 3. 读取并处理手势传感器 ---
Gesture ges_data = gestureSensor.readGesture();
        
        if (ges_data != GES_NONE) { 
            if (ges_data == GES_UP) {
                nextTriggered = true; // 手势 UP 等效于按下了 BUTTON_PIN (14)
                Serial.println("👆 识别到手势: UP (等效切屏/下一页)");
            } else if (ges_data == GES_DOWN) {
                okTriggered = true;   // 手势 DOWN 等效于按下了 PIN_IMAGE_BUTTON (13)
                Serial.println("👇 识别到手势: DOWN (等效画廊/上一页)");
            } else {
                // 如果你想看其他被丢弃的手势，可以取消下面这行的注释
                // Serial.println("识别到其他手势");
            }
        }

        // =========================================================
        // 下方的所有业务逻辑，统一只认 nextTriggered 和 okTriggered 
        // 无论是按键还是手势，只要有一个触发了，就会执行！
        // =========================================================

        if (needsEbookStop) {
    needsEbookStop = false;

    if (isEbookMode) {
        EbookApp::stop();
        isEbookMode = false;

        // 恢复天气页面所需的横向旋转。
        display->setRotation(0);

        u8g2Fonts.begin(*display);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

        needsUpdate = true;
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
    continue;
}

        // ── 电子书模式 ─────────────────────────────────
        if (isEbookMode) {
            bool nextPressed = false;
            bool okPressed   = false;

            static unsigned long lastEbookKey = 0;
            if (millis() - lastEbookKey > 300) {
                if (nextTriggered) {  
                    nextPressed = true;
                    lastEbookKey = millis();
                }
                else if (okTriggered) {    
                    okPressed = true;
                    lastEbookKey = millis();
                }
            }

            EbookApp::step_ext(nextPressed, okPressed);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        // ── 检测语音触发启动电子书 ─────────────────────
        if (needsEbookStart) {
            needsEbookStart = false;
            isEbookMode     = true;
            EbookApp::start();
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        // ── 处理Web服务器请求 ─────────────────────
        server.handleClient();
        if (webHasTodo) {
            webHasTodo    = false;
            todoListText  = webPushTodo;
            currentScreen = 3;
            isImageMode   = false;
            needsUpdate   = true;
        }

        if (webHasQuestion) {
            webHasQuestion = false;
            String prompt = "请扮演一名资深工程师，向初学者通俗解释：\""
                            + webPushQuestion +
                            "\"。要求语言精炼、直击核心。总字数控制在150字左右，不要废话。";
            getText("user", prompt);
            Answer     = "";
            mainStatus = 3;
            webNeedsAIConnect = true; 
        }

        // ── 导航按键 / UP手势检测（防抖300ms） ──
        if (nextTriggered) {
            if (millis() - lastButtonPress > 300) {
                if (isImageMode) {
                    isImageMode = false;
                } else {
                    currentScreen = (currentScreen + 1) % TOTAL_SCREENS;
                }
                needsUpdate     = true;
                lastButtonPress = millis();
            }
        }

        // ── 检测刷新标志 ──
        if (needsUpdate) {
            updateScreenFull(firstBoot);
            firstBoot   = false;
            needsUpdate = false;
        }

        // ── 画廊按键 / DOWN手势 ──
        if (okTriggered) {
            if (millis() - lastImageButtonPress > 300) {
                if (!isImageMode) {
                    isImageMode       = true;
                    currentImageIndex = 0;
                } else {
                    currentImageIndex = (currentImageIndex + 1) % TOTAL_IMAGES;
                }
                needsUpdate = true;
                lastImageButtonPress = millis();
            }
        }

        // ── 定时局刷与数据拉取 ──
        if ((millis() - lastPartialUpdate > 60000) && (currentScreen < 3) && !isImageMode) {
            lastPartialUpdate = millis();
            updateTimeOnly();
            updateStatusBarPartial();
        }

        if (millis() - lastWeatherFetch > 3600000UL) {
            lastWeatherFetch = millis();
            fetchAllData();
            if (currentScreen < 3) needsUpdate = true;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}


// ============================================================
// =================== 系统初始化 =============================
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println("===== BOOT START =====");

// --- 新增：初始化 PAJ7620 手势识别 ---
// --- 初始化 PAJ7620 手势识别 ---
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!gestureSensor.begin()) {
        Serial.println("⚠️ PAJ7620 初始化失败 (请检查接线)");
    } else {
        Serial.println("✅ PAJ7620 手势识别模块就绪");
    }

    // ✅ 先初始化PSRAM
    if (!psramInit()) {
        Serial.println("❌ PSRAM初始化失败！");
    } else {
        Serial.printf("✅ PSRAM就绪，可用: %d 字节\n", ESP.getFreePsram());
    }

    // ✅ 再构造display对象
display = new GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT>(
    GxEPD2_750_GDEY075T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);
    delay(500);

    // --- 初始化引脚 ---
    pinMode(key,        INPUT_PULLUP); // 录音唤醒键
    pinMode(BUTTON_PIN, INPUT_PULLUP); // 界面切换键
    pinMode(led3,       OUTPUT);       // 播放指示灯
    pinMode(PIN_IMAGE_BUTTON, INPUT_PULLUP);
    
    Serial.println("Step 2: Display init");
    // --- 初始化墨水屏（四灰度驱动）---
    initDisplay();
    Serial.println(">>> 墨水屏初始化完成");

    // --- 初始化AI对话历史（来自第一套代码）---
    Serial.println("Step 3: PSRAM alloc");
    globalDoc = new DynamicJsonDocument(4000);
    text = globalDoc->to<JsonArray>();

    // --- 初始化音频系统（来自第一套代码）---
    audio1.init();
    audio2.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio2.setVolume(10);
    Serial.println(">>> 音频系统初始化完成");

    // --- WiFi连接（融合两套代码）---
    int numNetworks = sizeof(wifiData) / sizeof(wifiData[0]);
    Serial.println("Step 4: WiFi");
    wifiConnect(wifiData, numNetworks);

    // --- NTP时间同步（来自第二套代码）---
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    Serial.println(">>> NTP时间同步配置完成");

    // --- 获取AI鉴权时间戳（来自第一套代码）---
    getTimeFromServer();
    url  = getUrl("ws://spark-api.xf-yun.com/v4.0/chat", "spark-api.xf-yun.com", "/v4.0/chat", Date);
    url1 = getUrl("ws://ws-api.xfyun.cn/v2/iat", "ws-api.xfyun.cn", "/v2/iat", Date);
    urlTime = millis();
    Serial.println(">>> AI鉴权URL生成完成");

    // ── 初始化 SD 卡 ─────────────────────────────────
    Serial.println("Step 5: SD Card init");
    if (!LittleFS.begin(true)) {
        Serial.println("⚠️ LittleFS 挂载失败！请检查 platformio.ini 的分区表配置。");
    } else {
        Serial.println("✅ 内部 Flash 文件系统就绪");
        Serial.printf("总空间: %u 字节, 已用: %u 字节\n", LittleFS.totalBytes(), LittleFS.usedBytes());
    }

    initWebServer();
    Serial.print("手机访问地址: http://");
    Serial.println(WiFi.localIP());

    // --- 拉取所有天气数据（来自第二套代码）---
    Serial.println(">>> 开始拉取天气数据...");
    fetchAllData();
    printAllDebugData(); // 打印到串口，方便调试

    // --- 首次全局刷新（开机深度复位）---
    // Serial.println(">>> 执行首次屏幕刷新...");
    // updateScreenFull(true); // true = 深度复位
    // needsUpdate = false;
    needsUpdate = true;   // 让UI任务去刷
    Serial.println(">>> 首次刷新将由UI任务执行...");
    // --- 创建FreeRTOS任务 ---
    Serial.println(">>> 创建FreeRTOS双核任务...");

#if USE_MULTCORE
    // 双核模式：AI任务绑定Core 1，UI任务绑定Core 0
    xTaskCreatePinnedToCore(
        taskAICore, "AICore",
        32768,      // 32KB栈空间（含HTTPS/WebSocket/JSON/音频）
        NULL, 2,    // 优先级2，保证语音播放流畅
        NULL, 1);   // 绑定Core 1

    xTaskCreatePinnedToCore(
        taskUICore, "UICore",
        32768,      // 16KB栈空间（含HTTP请求/JSON解析/墨水屏渲染）
        NULL, 1,    // 优先级1
        NULL, 0);   // 绑定Core 0
#else
    // 单核模式：由系统自由调度
    xTaskCreate(taskAICore, "AICore", 32768, NULL, 2, NULL);
    xTaskCreate(taskUICore, "UICore", 16384, NULL, 1, NULL);
#endif

    Serial.println(">>> 所有任务已启动，系统运行中...");
}


// ============================================================
// =================== 主循环（已交由FreeRTOS接管）============
// ============================================================
void loop() {
    // 所有业务逻辑由FreeRTOS任务负责，主loop休眠即可
    // 主loop本身也是一个FreeRTOS任务，长延时将其挂起
    while (1) {
        delay(10000);
    }
}
