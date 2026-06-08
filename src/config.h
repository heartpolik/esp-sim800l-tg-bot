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

// Call behaviour
#define CALL_TIMEOUT_MS  60000UL
#define BOT_POLL_MS       1000UL
