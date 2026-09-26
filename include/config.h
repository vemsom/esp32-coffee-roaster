#pragma once

// ---- WiFi ----
// Uppgifterna ligger i include/secrets.h (okommiterad). Fyll i där,
// eller låt dessa TBD-varden styra om secrets.h saknas.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "TBD"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "TBD"
#endif

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

// ---- Sakerhetsgranser (hardkodade skydd, oberoende av PID och profil) ----
// Hard grans for bontemperaturen. Nar den nas slas varmet av och ett larm
// last i safety.cpp - larmet kan inte tystas, det sjalper forst nar
// temperaturen ar tillbaka under gransen med SAFETY_CLEAR_MARGIN_C marginal
// och sensorerna rapporterar friska varden igen.
#define SAFETY_MAX_TEMP_C          260.0
// Hard grans for miljotemperaturen (ET), hogre an BT-gransen med avsikt.
#define SAFETY_MAX_ET_TEMP_C       300.0
// Marginal under gransen som maste uppnas innan larmet sjalper.
#define SAFETY_CLEAR_MARGIN_C       10.0
// Antal felfria sampel i rad som kravs innan ett larm sjalper (10 x 250 ms).
#define SAFETY_CLEAR_STREAK         10
// Plausibilitetsfonster for en enskild MAX6675-avlasning. Utanfor detta raknas
// avlasningen som ett sensorfel (en frilagd ingang kan ge 0 C utan NaN).
#define SENSOR_MIN_VALID_C        (-10.0)
#define SENSOR_MAX_VALID_C         400.0

// ---- MQTT (Home Assistant MQTT-discovery) ----
// Anslutningsuppgifterna laggs i include/secrets.h (okommiterad) sa att de
// aldrig hamnar i repot. Sa lange MQTT_HOST ar "TBD" ar MQTT avstangt.
#ifndef MQTT_HOST
#define MQTT_HOST        "TBD"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT        1883
#endif
#ifndef MQTT_USER
#define MQTT_USER        ""
#endif
// MQTT_PASS is accepted as an alias: tooling that fills in secrets.h often
// writes that name instead of MQTT_PASSWORD.
#ifndef MQTT_PASSWORD
#ifdef MQTT_PASS
#define MQTT_PASSWORD    MQTT_PASS
#else
#define MQTT_PASSWORD    ""
#endif
#endif
#define MQTT_BASE_TOPIC             "coffee_roaster"
#define MQTT_DISCOVERY_PREFIX       "homeassistant"
#define MQTT_DEVICE_ID              "coffee_roaster"
#define MQTT_CLIENT_ID              "coffee_roaster_esp32"
#define MQTT_PUBLISH_INTERVAL_MS    2000
#define MQTT_RECONNECT_INTERVAL_MS  5000
#define MQTT_MAX_PROFILE_OPTIONS    12
// Must stay below the PubSubClient buffer (1024) with room for topic + packet
// header, otherwise the client refuses the publish. Payloads are serialized
// into a fixed buffer instead of a heap String, and an oversized payload is
// reported as an error rather than dropped silently.
#define MQTT_MAX_PAYLOAD_BYTES      900
#define MQTT_CLIENT_BUFFER_BYTES    1024

// ---- Firmware-version (rapporteras till Home Assistant) ----
#define FW_VERSION "0.3.0"

// ---- PID-standardvarden ----
#define PID_KP  4.0
#define PID_KI  0.05
#define PID_KD  2.0

// ---- Profil-lagring ----
#define PROFILES_DIR "/profiles"
