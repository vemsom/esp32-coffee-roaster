#pragma once

// Fan output. The API is a requested speed in percent: 0 = off, 1-100 = the
// logical speed the web UI, the profile steps, MQTT/HA and the fan interlock
// speak. What that percentage does NOT say any more is what the fan is doing
// right now: the duty ramps toward the request at FAN_SLEW_PCT_PER_SEC
// (config.h - the 24 V supply cut out on the fan's start current), so between
// the request and the fan actually being there, the reached speed is the only
// number that may be trusted. fan_update() advances the ramp and is called from
// loop().
void fan_init();
void fan_set_speed(int percent);   // 0-100, the requested speed
void fan_update();                 // advance the ramp, called from loop()

// The speed the fan has actually reached, 0-100. This is what the heat
// interlock has to judge on: a request of 100 % arriving at a fan still at 0 %
// is not airflow, and lighting the element on the request is how heat ends up
// on with the fan spinning up.
float fan_actual_pct();
// The last request, 0-100 - what is reported outward (status, HA, profiles).
float fan_requested_pct();
