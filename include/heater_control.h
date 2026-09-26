#pragma once

void heater_init();
void heater_set_duty(float dutyPercent);
void heater_update();
// Latches the SSR off. Stays latched until heater_clear_emergency() is called,
// which only the safety module is allowed to do (and only once the fault that
// tripped it is gone).
void heater_emergency_off();
void heater_clear_emergency();
bool heater_emergency_active();
