// Minimal host-side Arduino surface for the native_controls unit tests.
// Time is driven explicitly: each test TU defines millis() over a counter it
// advances itself, so nothing here touches the wall clock.
#pragma once
#include <cstdint>

unsigned long millis();

// ESP-IDF logging reaches native TUs through SimpleKalmanFilter's Arduino.h
// include; the tests have no log sink.
#ifndef ESP_LOGI
#define ESP_LOGI(...) ((void)0)
#endif
