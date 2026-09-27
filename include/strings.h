#pragma once

// ---------------------------------------------------------------------------
// User-visible text the firmware speaks: one entry per string, one block per
// language. The web page cannot be here - a LittleFS file cannot include a
// header - so its own table lives in data/index.html and follows FW_LANG_CODE,
// which /api/status reports; tools/host-tests/check_web_i18n.js keeps that
// table honest (every key in both languages, nothing hard-coded).
//
// English is the DEFAULT so that anyone who builds this project without
// reading anything gets English in Home Assistant and in the web UI. Swedish
// is an opt-in: set FW_LANG_EN to 0 in include/config.h - that line is the
// whole installation toggle - or build with -DFW_LANG_EN=0.
//
// What must NOT live here: unique_id, object_id, topics, entity_ids. Home
// Assistant keys the entity on those, and a language switch may change the
// pretty name only, never where the entity lives. tools/host-tests/
// test_mqtt_discovery is compiled once per language and proves exactly that:
// both variants publish the same unique_id.
//
// Serial log lines are deliberately not here either: they are developer
// text, identical English in both builds.
//
// The default below repeats the one in include/config.h so that a test
// translation unit which includes only this header still behaves like the
// firmware does.
// ---------------------------------------------------------------------------

#ifndef FW_LANG_EN
#define FW_LANG_EN 1
#endif

#if FW_LANG_EN

#define FW_LANG_CODE "en"
#define STR_DEVICE_NAME "Coffee roaster"
#define STR_NAME_BT "Bean temperature"
#define STR_NAME_ET "Environment temperature"
#define STR_NAME_HEATER "Heater"
#define STR_NAME_FAN "Fan"
#define STR_NAME_MODE "Mode"
#define STR_NAME_ELAPSED "Roast time"
#define STR_NAME_SAFETY "Safety alarm"
#define STR_NAME_FAN_FAULT "Fan interlock"
#define STR_NAME_PROFILE "Profile"

#else

#define FW_LANG_CODE "sv"
#define STR_DEVICE_NAME "Kafferostaren"
#define STR_NAME_BT "Böntemperatur"
#define STR_NAME_ET "Miljötemperatur"
#define STR_NAME_HEATER "Värmelement"
#define STR_NAME_FAN "Fläkt"
#define STR_NAME_MODE "Läge"
#define STR_NAME_ELAPSED "Rosttid"
#define STR_NAME_SAFETY "Säkerhetslarm"
#define STR_NAME_FAN_FAULT "Fläktspärr"
#define STR_NAME_PROFILE "Profil"

#endif
