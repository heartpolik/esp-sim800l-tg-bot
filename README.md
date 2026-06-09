# CallerBot

Telegram bot running on **LILYGO TTGO T-Call V1.4** (ESP32 + SIM800L) that makes GSM voice calls on command, with a Waveshare e-paper status display and a browser-based admin panel. No reflashing needed to change phone numbers, users, or settings.

---

## Hardware

| Component | Details |
|-----------|---------|
| Board | LILYGO TTGO T-Call V1.4 |
| MCU | ESP32 (dual-core 240 MHz, 4 MB flash) |
| Modem | SIM800L (2G GSM/GPRS) |
| Power IC | IP5306 (I2C, always-on boost enabled) |
| Display | Waveshare 2.13" e-Paper HAT (B) — Black/White/Red, 104×212 px |
| SIM | Standard micro SIM, **2G network required** |

> **Check 2G coverage before buying.** Many countries (US, AU, most of EU) have shut down 2G. SIM800L will not work there.

---

## Features

- **Telegram inline keyboard** — tap buttons, no typing
- **Multiple phone aliases**, each with its own button label and per-user allow list
- **Real-time call progress** via AT+CLCC polling — Dialing → Ringing → Answered → Ended
- **Auto-hangup** with configurable timeout (default 60 s)
- **E-paper status display** — signal bars, battery icon, call state, last command, date/IP
- **Web admin panel** — edit all config in browser, device restarts with new config
- **OTA firmware update** at `http://<device-ip>/update`
- **WiFi AP fallback** — device creates open hotspot when router is unreachable
- **NTP time sync** with configurable UTC offset
- `/status` — live GSM signal + battery level
- `/echo` — send a configurable USSD code (e.g. `*123#` for balance)
- `/getMyId` — returns your Telegram user ID (for initial setup)

---

## Bot Interface

The bot uses an inline keyboard — no commands to type. After `/start` or `/help` a menu appears with one button per alias plus utility buttons.

| Button / Command | Auth | Action |
|-----------------|------|--------|
| `<alias label>` | alias allow list | Dial the alias phone number |
| Hang Up | any authorized user | End active call |
| Status | any authorized user | GSM signal + battery |
| Echo | any authorized user | Send configured USSD |
| `/getMyId` | anyone | Print your Telegram user ID |

---

## E-Paper Display

**Header** (top strip):
- WiFi signal bars (4-bar icon)
- GSM signal bars (4-bar icon)
- Battery level icon with proportional fill

**Main area**: call state — `IDLE` (black) or `CALLING` + alias label (red)

**Footer**:
- STA mode: left = date, right = device IP / `"offline"` | second row = last call alias + time
- AP mode: SSID (`CallerBot-XXXXXXXX`) | second row = `192.168.4.1`

Alias labels containing Cyrillic are automatically transliterated to Latin for display (FreeSans font covers Latin-1 only).

---

## Project Structure

```
caller/
├── platformio.ini          # build config, library deps
├── src/
│   ├── config.h            # hardware pin constants (T-Call V1.4)
│   └── main.cpp            # full application
├── data/
│   ├── config.json         # runtime config (gitignored — create from example)
│   ├── config.example.json # template with all fields documented
│   └── index.html          # web admin UI (served from LittleFS)
└── .gitignore
```

---

## Setup

### 1. Install PlatformIO

[VS Code extension](https://platformio.org/install/ide?install=vscode) or CLI:

```bash
pip install platformio
```

If `pio` not in PATH after install (macOS):

```bash
echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### 2. Create config

```bash
cp data/config.example.json data/config.json
```

Edit `data/config.json` — minimum required fields:

```json
{
  "wifi_ssid":      "YourWiFi",
  "wifi_password":  "yourpassword",
  "bot_token":      "123456789:ABCxxxxx",
  "admin_password": "changeme",
  "aliases": [
    {
      "command":       "home",
      "label":         "Home",
      "phone":         "+380XXXXXXXXX",
      "allowed_users": [123456789]
    }
  ]
}
```

**Get your Telegram user ID:** start the bot and tap the `/getMyId` button.  
**Get a bot token:** talk to [@BotFather](https://t.me/BotFather).

### 3. Flash firmware

```bash
pio run -t upload
```

### 4. Upload filesystem (config + web admin)

```bash
pio run -t uploadfs
```

Both steps required on first flash. After that, config changes can be made through the web admin without reflashing firmware. Only reflash filesystem (`uploadfs`) if you modify `index.html`.

### 5. Find device IP

Open serial monitor:

```bash
pio device monitor
```

Look for:

```
Web admin: http://192.168.1.XX
```

---

## Web Admin

Available at `http://<device-ip>` — login: username `admin`, password = `admin_password` from config.

| Section | Editable fields |
|---------|----------------|
| Device Status | Live CSQ, battery, call state (read-only) |
| Credentials | WiFi SSID/password, bot token, admin password |
| Call Settings | USSD code, auto-hangup timeout, display flip |
| Phone Aliases | Add / edit / delete aliases (command, label, phone, allowed users) |

Save → device restarts with new config. No reflash needed.

### OTA Firmware Update

Available at `http://<device-ip>/update` — same credentials (username `admin`).

Upload a `.bin` file built with `pio run` (found in `.pio/build/ttgo-t-call-v14/firmware.bin`).

---

## AP Fallback Mode

When the device cannot reach the configured WiFi (wrong credentials, router offline, or no SSID set) it starts an open hotspot:

| Property | Value |
|----------|-------|
| SSID | `CallerBot-XXXXXXXX` (unique per chip) |
| IP | `192.168.4.1` |
| Password | none (open) |

Connect and open `http://192.168.4.1` to configure WiFi. After save the device restarts and connects to the router. The display shows the SSID and IP while in AP mode.

---

## Config Reference

```jsonc
{
  "wifi_ssid":       "string  — WiFi network name",
  "wifi_password":   "string  — WiFi password",
  "bot_token":       "string  — Telegram bot token from @BotFather",
  "admin_password":  "string  — web admin + OTA login password (username: admin)",
  "ussd_code":       "string  — USSD code for Echo button, e.g. '*123#'",
  "call_timeout_s":  60,       // auto-hangup after N seconds
  "gmt_offset_h":    2,        // UTC offset for NTP display (e.g. 2 = UTC+2)
  "display_flip":    false,    // true = rotate display 180°
  "aliases": [
    {
      "command":       "home",        // bot command name (lowercase a-z, 0-9, _)
      "label":         "Home",        // button label shown in Telegram (any text, emoji OK)
      "phone":         "+380XXXXXXX", // E.164 format
      "allowed_users": [123456789]    // Telegram user IDs allowed to use this alias
    }
  ]
}
```

---

## Hardware Pins

### T-Call V1.4 — Modem + Power

Defined in `src/config.h`.

| Signal | GPIO |
|--------|------|
| MODEM_TX | 27 |
| MODEM_RX | 26 |
| MODEM_PWRKEY | 4 |
| MODEM_RST | 5 |
| MODEM_POWER_ON | 23 |
| I2C SDA (IP5306) | 21 |
| I2C SCL (IP5306) | 22 |

### Waveshare 2.13" e-Paper HAT (B) — HSPI bus

GPIO 23 (VSPI MOSI) is taken by `MODEM_POWER_ON`, so the display uses the HSPI bus with remapped pins.

| Display pin | GPIO | Notes |
|-------------|------|-------|
| CLK | 18 | HSPI SCK |
| DIN | 13 | HSPI MOSI |
| CS | 15 | |
| DC | 32 | |
| RST | 33 | |
| BUSY | 34 | input-only GPIO |
| VCC | 3.3 V | **not 5 V** |
| GND | GND | |

---

## Libraries

| Library | Version | Purpose |
|---------|---------|---------|
| [TinyGSM](https://github.com/vshymanskyy/TinyGSM) | ^0.11.7 | SIM800L AT commands |
| [Universal Arduino Telegram Bot](https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot) | git | Telegram polling |
| [ArduinoJson](https://arduinojson.org) | ^6.21 | JSON config + API |
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | ^1.2.3 | Non-blocking web admin |
| [AsyncTCP](https://github.com/me-no-dev/AsyncTCP) | ^1.1.1 | TCP backend for above |
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) | ^1.6.0 | E-paper display driver |
| [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) | ^1.11 | Graphics primitives + fonts |
| [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) | ^3.1.0 | OTA firmware update UI |

---

## Troubleshooting

**Modem not responding on first boot**  
Power-cycle the board. On software reset the SIM800L stays powered; firmware probes AT first before attempting a cold power cycle.

**`WARN: not registered after 60s`**  
Check SIM is inserted correctly and 2G is available. Serial log shows CSQ — below 10 is too weak.

**Display shows nothing**  
Verify wiring (especially VCC = 3.3 V, not 5 V). The driver is `GxEPD2_213c` (UC8151, GDEW0213Z16) for the HAT (B). A full 3-color refresh takes 15–20 seconds — wait before concluding failure.

**Display text garbled / Cyrillic missing**  
The FreeSans font covers Latin-1 only. Cyrillic in alias labels is auto-transliterated to Latin (e.g. "Дім" → "DiM"). Emojis are stripped.

**`/api/config` returns 404**  
Filesystem not uploaded. Run `pio run -t uploadfs`.

**Bot receives message but no keyboard appears**  
The library's HTTP POST has a known issue. A custom `tgPost()` using a fresh `WiFiClientSecure` connection is used instead. Check serial for `tgPost err:` lines.

**OTA upload page not reachable**  
Navigate to `http://<device-ip>/update`. Requires `ELEGANTOTA_USE_ASYNC_WEBSERVER=1` build flag (already set in `platformio.ini`).
