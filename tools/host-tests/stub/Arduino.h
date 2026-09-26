#pragma once
// Minimal Arduino stub so safety.cpp and heater_control.cpp can be compiled and
// exercised on the host (no ESP32 needed).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using std::isnan;
using std::isinf;
using std::fabs;

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);
unsigned long millis();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);

struct SerialStub {
  void begin(unsigned long) {}
  template <typename T> void print(T) {}
  template <typename T> void println(T) {}
  void println() {}
};
extern SerialStub Serial;
