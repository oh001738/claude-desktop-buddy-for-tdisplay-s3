#ifdef ARDUINO_LILYGO_T_DISPLAY_S3
#include "hal.h"
#include <OneButton.h>

const int SCREEN_W = 170;
const int SCREEN_H = 320;

#define PIN_LCD_BL  38
#define PIN_POWER_ON 15
#define PIN_BUTTON_1 0
#define PIN_BUTTON_2 14
#define PIN_BAT_VOLT 4

TFT_eSPI tft = TFT_eSPI();
OneButton btnA(PIN_BUTTON_1, true);
OneButton btnB(PIN_BUTTON_2, true);

bool btnAClicked = false;
bool btnBClicked = false;
bool btnALongPressed = false;

// ST7789 commands for T-Display S3 initialization (from factory example)
typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
} lcd_cmd_t;

lcd_cmd_t lcd_st7789v[] = {
    {0x11, {0}, 0 | 0x80},
    {0x3A, {0X05}, 1},
    {0xB2, {0X0B, 0X0B, 0X00, 0X33, 0X33}, 5},
    {0xB7, {0X75}, 1},
    {0xBB, {0X28}, 1},
    {0xC0, {0X2C}, 1},
    {0xC2, {0X01}, 1},
    {0xC3, {0X1F}, 1},
    {0xC6, {0X13}, 1},
    {0xD0, {0XA7}, 1},
    {0xD0, {0XA4, 0XA1}, 2},
    {0xD6, {0XA1}, 1},
    {0xE0, {0XF0, 0X05, 0X0A, 0X06, 0X06, 0X03, 0X2B, 0X32, 0X43, 0X36, 0X11, 0X10, 0X2B, 0X32}, 14},
    {0xE1, {0XF0, 0X08, 0X0C, 0X0B, 0X09, 0X24, 0X2B, 0X22, 0X43, 0X38, 0X15, 0X16, 0X2F, 0X37}, 14},
};

void hal_init() {
    // Force backlight off immediately to prevent seeing hardware garbage
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);

    // Power on the screen
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);

    tft.begin();
    
    // Custom init sequence for T-Display S3
    for (uint8_t i = 0; i < (sizeof(lcd_st7789v) / sizeof(lcd_cmd_t)); i++) {
        tft.writecommand(lcd_st7789v[i].cmd);
        for (int j = 0; j < (lcd_st7789v[i].len & 0x7f); j++) {
            tft.writedata(lcd_st7789v[i].data[j]);
        }
        if (lcd_st7789v[i].len & 0x80) delay(120);
    }

    tft.setRotation(0); // Portrait (170x320)
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK); // Clear the internal frame buffer while backlight is still off
    
    // Backlight PWM initialization
    ledcSetup(0, 5000, 8); // Channel 0, 5000Hz, 8-bit resolution
    ledcAttachPin(PIN_LCD_BL, 0);
    ledcWrite(0, 0); // Keep it off until setup() is ready

    // Buttons
    pinMode(PIN_BUTTON_1, INPUT_PULLUP);
    pinMode(PIN_BUTTON_2, INPUT_PULLUP);
    btnA.setClickMs(150);
    btnB.setClickMs(150);
    btnA.setDebounceMs(30);
    btnB.setDebounceMs(30);
    btnA.attachClick([]() { btnAClicked = true; });
    btnB.attachClick([]() { btnBClicked = true; });
    btnA.attachLongPressStart([]() { btnALongPressed = true; });
}

void hal_loop() {
    btnAClicked = false;
    btnBClicked = false;
    btnALongPressed = false;
    btnA.tick();
    btnB.tick();
}

TFT_eSPI* hal_get_lcd() { return &tft; }

void hal_set_brightness(uint8_t level) {
    // level: 1..4. Map to 100..255 PWM.
    uint8_t pwm = 255;
    if (level <= 1) pwm = 100;
    else if (level == 2) pwm = 150;
    else if (level == 3) pwm = 200;
    else pwm = 255;
    ledcWrite(0, pwm);
}

void hal_screen_off() { ledcWrite(0, 0); digitalWrite(PIN_POWER_ON, LOW); }
void hal_screen_on() { digitalWrite(PIN_POWER_ON, HIGH); ledcWrite(0, 255); }

bool hal_btn_a_pressed() { return digitalRead(PIN_BUTTON_1) == LOW; }
bool hal_btn_b_pressed() { return digitalRead(PIN_BUTTON_2) == LOW; }
bool hal_btn_a_clicked() { return btnAClicked; }
bool hal_btn_b_clicked() { return btnBClicked; }
bool hal_btn_a_long_pressed() { return btnALongPressed; }

float hal_get_battery_voltage() {
    return analogRead(PIN_BAT_VOLT) * 2.0f * 3.3f / 4096.0f;
}

bool hal_is_on_usb() {
    return hal_get_battery_voltage() > 4.3f; 
}

void hal_power_off() {
    hal_screen_off();
    esp_deep_sleep_start();
}

void hal_beep(uint16_t freq, uint16_t dur) {
    // T-Display S3 doesn't have a buzzer. Stub.
}

void hal_get_time(int& h, int& m, int& s) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 0)){
        h=m=s=0;
        return;
    }
    h = timeinfo.tm_hour;
    m = timeinfo.tm_min;
    s = timeinfo.tm_sec;
}

void hal_get_date(int& y, int& mon, int& d, int& dow) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 0)){
        y=2024; mon=1; d=1; dow=0;
        return;
    }
    y = timeinfo.tm_year + 1900;
    mon = timeinfo.tm_mon + 1;
    d = timeinfo.tm_mday;
    dow = timeinfo.tm_wday;
}

void hal_set_time(int h, int m, int s) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) {
        timeinfo.tm_year = 2024 - 1900; timeinfo.tm_mon = 0; timeinfo.tm_mday = 1;
    }
    timeinfo.tm_hour = h;
    timeinfo.tm_min = m;
    timeinfo.tm_sec = s;
    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
}

void hal_set_date(int y, int mon, int d) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) {
        timeinfo.tm_hour = 0; timeinfo.tm_min = 0; timeinfo.tm_sec = 0;
    }
    timeinfo.tm_year = y - 1900;
    timeinfo.tm_mon = mon - 1;
    timeinfo.tm_mday = d;
    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
}

bool hal_is_face_down() { return false; } // No IMU
bool hal_check_shake() { return false; }   // No IMU
uint8_t hal_get_orientation() { return 0; } // Default portrait

#endif
