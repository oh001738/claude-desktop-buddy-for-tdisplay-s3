#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

struct HAL_Time {
    uint8_t Hours;
    uint8_t Minutes;
    uint8_t Seconds;
};

struct HAL_Date {
    uint8_t WeekDay;
    uint8_t Month;
    uint8_t Date;
    uint16_t Year;
};

// Screen dimensions
extern const int SCREEN_W;
extern const int SCREEN_H;

// Colors
#define HAL_COLOR_BLACK   0x0000
#define HAL_COLOR_WHITE   0xFFFF
#define HAL_COLOR_RED     0xF800
#define HAL_COLOR_GREEN   0x07E0
#define HAL_COLOR_BLUE    0x001F

// Initialization
void hal_init();
void hal_loop();

// Display
TFT_eSPI* hal_get_lcd();
void hal_set_brightness(uint8_t level); // 0-4
void hal_screen_off();
void hal_screen_on();

// Buttons
bool hal_btn_a_pressed();
bool hal_btn_b_pressed();
bool hal_btn_a_clicked();
bool hal_btn_b_clicked();
bool hal_btn_a_long_pressed();

// Power
float hal_get_battery_voltage();
bool hal_is_on_usb();
void hal_power_off();

// Sound
void hal_beep(uint16_t freq, uint16_t dur);

// RTC
void hal_get_time(int& h, int& m, int& s);
void hal_get_date(int& y, int& mon, int& d, int& dow);
void hal_set_time(int h, int m, int s);
void hal_set_date(int y, int mon, int d);

// IMU
bool hal_is_face_down();
bool hal_check_shake();
uint8_t hal_get_orientation(); // 0: port, 1: land1, 3: land2
