#include "system_state.h"
#include <WiFi.h>
#include <time.h>
#include <SD.h>

void SystemState::begin(){
  // 若使用 NTP，可在联网后调用：configTzTime("CST-8", "pool.ntp.org", "time.google.com");
}

SystemStateSnapshot SystemState::sample(){
  SystemStateSnapshot s;
  s.sd_ok  = sd_mounted_ || (SD.cardType() != CARD_NONE);
  s.net_ok = (WiFi.status() == WL_CONNECTED);
  s.epoch  = (uint32_t)time(nullptr);
  return s;
}