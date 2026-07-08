#pragma once
#include <Arduino.h>

struct SystemStateSnapshot {
  bool sd_ok = false;
  bool net_ok = false;
  uint32_t epoch = 0; // time(nullptr)
};

class SystemState {
public:
  void begin();
  SystemStateSnapshot sample();
  void setSDMounted(bool ok) { sd_mounted_ = ok; }
private:
  bool sd_mounted_ = false;
};