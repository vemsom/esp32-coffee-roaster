#pragma once

// ---- WiFi ----
// TODO: fyll i ditt eget WiFi innan flashning
#define WIFI_SSID "TBD"
#define WIFI_PASSWORD "TBD"

// ---- Sensor-pinnar (SPI, MAX6675 x2) ----
// Delade CLK/MISO, separata CS. Verifiera/andra nar layouten ar klar.
#define PIN_MAX6675_CLK   18
#define PIN_MAX6675_MISO  19
#define PIN_MAX6675_CS_BT 5
#define PIN_MAX6675_CS_ET 17

// ---- Varme-SSR ----
#define PIN_SSR_HEATER    26

// ---- Flakt-MOSFET (PWM) ----
#define PIN_FAN_PWM        27
#define FAN_PWM_FREQ_HZ    20000
#define FAN_PWM_RESOLUTION 8

// ---- Kontrolloop-timing ----
#define SENSOR_READ_INTERVAL_MS   250
#define HEATER_WINDOW_MS          2000
#define SENSOR_FAULT_MAX_JUMP_C   20.0
#define SENSOR_FAULT_MAX_COUNT    5

// ---- PID-standardvarden ----
#define PID_KP  4.0
#define PID_KI  0.05
#define PID_KD  2.0

// ---- Profil-lagring ----
#define PROFILES_DIR "/profiles"
