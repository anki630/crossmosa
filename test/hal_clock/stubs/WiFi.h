#pragma once
#include "FakeClockHw.h"
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
struct WiFiStub {
  int status() const { return fake::wifiConnected ? WL_CONNECTED : WL_DISCONNECTED; }
};
inline WiFiStub WiFi;
