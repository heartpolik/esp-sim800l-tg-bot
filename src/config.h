#pragma once

// LILYGO TTGO T-Call V1.4 hardware pins
#define MODEM_TX             27
#define MODEM_RX             26
#define MODEM_PWRKEY          4
#define MODEM_RST             5
#define MODEM_POWER_ON       23
#define I2C_SDA              21
#define I2C_SCL              22

// IP5306 power management IC (I2C address + registers)
#define IP5306_ADDR          0x75
#define IP5306_REG_SYS_CTL0  0x00
#define IP5306_REG_READ2     0x78

// E-Paper display — Waveshare 2.13" (C), 212x104, 4-line SPI
// Uses HSPI (bus 2) — VSPI MOSI GPIO 23 is taken by MODEM_POWER_ON
#define EPAPER_CLK   18
#define EPAPER_MOSI  13
#define EPAPER_CS    15
#define EPAPER_DC    32
#define EPAPER_RST   33
#define EPAPER_BUSY  34   // input-only GPIO

// Call behaviour
#define CALL_TIMEOUT_MS  60000UL
#define BOT_POLL_MS       1000UL
