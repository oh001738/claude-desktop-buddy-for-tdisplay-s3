#ifndef ARDUINO_LILYGO_T_DISPLAY_S3
#include "hal.h"
#include <M5StickCPlus.h>

const int SCREEN_W = 135;
const int SCREEN_H = 240;

void hal_init() {
    M5.begin();
    M5.Lcd.setRotation(0);
}

void hal_loop() {
    M5.update();
}

TFT_eSPI* hal_get_lcd() { return &M5.Lcd; }

void hal_set_brightness(uint8_t level) {
    M5.Axp.ScreenBreath(20 + level * 20);
}

void hal_screen_off() {
    M5.Axp.SetLDO2(false);
}

void hal_screen_on() {
    M5.Axp.SetLDO2(true);
}

bool hal_btn_a_pressed() { return M5.BtnA.isPressed(); }
bool hal_btn_b_pressed() { return M5.BtnB.isPressed(); }
bool hal_btn_a_clicked() { return M5.BtnA.wasClicked(); }
bool hal_btn_b_clicked() { return M5.BtnB.wasClicked(); }
bool hal_btn_a_long_pressed() { return M5.BtnA.pressedFor(800); }

float hal_get_battery_voltage() { return M5.Axp.GetBatVoltage(); }
bool hal_is_on_usb() { return M5.Axp.GetVBusVoltage() > 4.0f; }
void hal_power_off() { M5.Axp.PowerOff(); }

void hal_beep(uint16_t freq, uint16_t dur) {
    M5.Beep.tone(freq, dur);
}

void hal_get_time(int& h, int& m, int& s) {
    RTC_TimeTypeDef t;
    M5.Rtc.GetTime(&t);
    h = t.Hours; m = t.Minutes; s = t.Seconds;
}

void hal_get_date(int& y, int& mon, int& d, int& dow) {
    RTC_DateTypeDef dt;
    M5.Rtc.GetDate(&dt);
    y = dt.Year; mon = dt.Month; d = dt.Date; dow = dt.WeekDay;
}

void hal_set_time(int h, int m, int s) {
    RTC_TimeTypeDef t;
    t.Hours = h; t.Minutes = m; t.Seconds = s;
    M5.Rtc.SetTime(&t);
}

void hal_set_date(int y, int mon, int d) {
    RTC_DateTypeDef dt;
    dt.Year = y; dt.Month = mon; dt.Date = d;
    M5.Rtc.SetDate(&dt);
}

bool hal_is_face_down() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

bool hal_check_shake() {
  static float accelBaseline = 1.0f;
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

uint8_t hal_get_orientation() {
    float ax, ay, az;
    M5.Imu.getAccelData(&ax, &ay, &az);
    if (fabsf(ax) > 0.7f) return (ax > 0) ? 1 : 3;
    return 0;
}

#endif
