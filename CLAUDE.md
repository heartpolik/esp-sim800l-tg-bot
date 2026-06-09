# CLAUDE.md — CallerBot project context

Read this before touching any code. Summarises every non-obvious decision so you don't re-derive them.

---

## Hardware

| Part | Details |
|------|---------|
| Board | LILYGO TTGO T-Call V1.4 |
| MCU | ESP32 (Arduino framework via PlatformIO) |
| Modem | SIM800L — 2G only, connected via UART1 (GPIO 26/27) |
| Power IC | IP5306 at I2C 0x75 — **must** set SYS_CTL0=0x37 at boot or SIM800L browns out on TX burst |
| Display | Waveshare 2.13" e-Paper HAT **(B)** — Black/White/Red, 104×212 px, UC8151 controller |

### GPIO conflicts (critical)

- GPIO 23 = `MODEM_POWER_ON` — cannot be used for VSPI MOSI
- Display therefore uses **HSPI** bus with remapped pins (CLK=18, MOSI=13, CS=15, DC=32, RST=33, BUSY=34)
- GPIO 34 is input-only — fine for BUSY (input)
- All pin constants live in `src/config.h` only; never hard-code in main.cpp

---

## Display driver

The correct GxEPD2 class is **`GxEPD2_213c`** (UC8151 / IL0373, GDEW0213Z16).

Do NOT use `GxEPD2_213_Z98c` — that is for SSD1680/GDEY0213Z98 (different controller, different resolution 122×250). When the wrong driver is loaded BUSY never clears after init.

Init quirk: if you manually pulse RST before calling `display.init()`, the double-reset leaves the controller in a broken state (BUSY stuck HIGH). Let the library do the reset exclusively.

SPI is initialised before passing to library:
```cpp
SPIClass epaperSPI(HSPI);
epaperSPI.begin(EPAPER_CLK, -1, EPAPER_MOSI, EPAPER_CS);
display.epd2.selectSPI(epaperSPI, SPISettings(1000000, MSBFIRST, SPI_MODE0));
display.init(0, true, 200, false);
display.setRotation(cfg.displayFlip ? 3 : 1);  // 1=landscape, 3=landscape flipped
```

3-colour full refresh takes **15–20 seconds** — do not expect faster.

**Never call modem AT commands inside the `do { } while (nextPage())` render loop** — it corrupts the SPI transaction. Cache all modem values before `display.firstPage()`.

Drawing coordinate system (rotation=1 or 3): width=212, height=104.

Font support: FreeSans covers Latin-1 only. Use `toDisplayStr()` (line 507) to transliterate Cyrillic and strip emojis before printing user-supplied strings.

---

## Telegram bot

`UniversalTelegramBot` GET-based polling works fine. Its `sendMessageWithInlineKeyboard` (POST) is broken — the SSL connection left open by `getUpdates` is in a bad state for subsequent POSTs.

Fix: custom `tgPost(method, body)` at line 146 opens a **fresh `WiFiClientSecure`** per call, uses **HTTP/1.0** (no chunked encoding), reads until disconnected. All keyboard messages go through `tgPost`.

Bot is created after WiFi connects:
```cpp
bot = new UniversalTelegramBot(cfg.botToken, secureClient);
bot->getUpdates(bot->last_message_received + 1);  // drain stale messages
```

Polling only in STA mode (not AP mode) in `loop()`.

---

## ElegantOTA

Requires `-DELEGANTOTA_USE_ASYNC_WEBSERVER=1` build flag or `begin()` type-mismatches (expects `WebServer*`, not `AsyncWebServer*`). Flag also removes the conflicting `HTTP_GET` macro from `http_parser.h`.

`ElegantOTA.loop()` must be called in `loop()`. OTA endpoint: `/update`.

Auth set to `("admin", cfg.adminPassword)` in `initWebServer()`.

---

## ESPAsyncWebServer

Library has a const-qualifier bug (`status()` not marked const). Suppressed with `-fpermissive` in build flags.

**Route registration order is critical**: API routes must be added before `serveStatic("/", ...)`. If `serveStatic` is first, it intercepts all paths including `/api/*` and returns 404 for everything.

---

## Modem init

SIM800L stays powered across ESP32 software resets. On boot, probe AT first:
```
if (!modem.init()) → powerCycleModem() → if (!modem.init()) → FATAL halt
```

`powerCycleModem()`: pull PWRKEY LOW ≥1500 ms, release, wait 3000 ms for RDY sequence.

Network registration waits up to 60 s, then warns and continues (non-fatal — device still works for WiFi/web even without GSM).

---

## Call progress

AT+CLCC polled every 4 s while `callState == CALLING`. `tickCallProgress()` at line 195.

GsmCallStat enum mirrors AT+CLCC stat field: NONE=-1, ACTIVE=0, HELD=1, DIALING=2, ALERTING=3, INCOMING=4, WAITING=5.

State transitions → Telegram messages via `sendMenu()` (sends menu + progress text). Auto-hangup checked in `loop()` after timeout.

---

## Config

Runtime config in LittleFS at `/config.json`. Loaded at boot in `loadConfig()`. Saving via web admin calls `saveConfigFromJson()` → `loadConfig()` → delayed `ESP.restart()`.

`data/config.json` is gitignored. `data/config.example.json` is the template in version control.

All config fields:

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `wifi_ssid` | string | — | |
| `wifi_password` | string | — | |
| `bot_token` | string | — | from @BotFather |
| `admin_password` | string | `"admin"` | web + OTA auth |
| `ussd_code` | string | — | sent by Echo button |
| `call_timeout_s` | int | 60 | auto-hangup |
| `gmt_offset_h` | int | 2 | UTC offset for NTP |
| `display_flip` | bool | false | rotate display 180° |
| `aliases` | array | — | see below |

Alias fields: `command` (lowercase a-z/0-9/_), `label` (any text, emoji OK), `phone` (E.164), `allowed_users` (array of Telegram user IDs as longs).

---

## Web admin (index.html)

Served from LittleFS. Load page → `GET /api/config` → populate form. Save → `POST /api/config` with full JSON → device restarts.

`GET /api/status` returns `{csq, battery_pct, call}` for the live status panel.

`/update` — ElegantOTA UI.

To change the web UI: edit `data/index.html`, run `pio run -t uploadfs`. Does **not** require firmware reflash.

---

## Build commands

```bash
# build firmware only
~/.platformio/penv/bin/pio run

# build + upload firmware
~/.platformio/penv/bin/pio run -t upload

# upload filesystem (config.json + index.html)
~/.platformio/penv/bin/pio run -t uploadfs

# serial monitor
~/.platformio/penv/bin/pio device monitor

# combined: upload firmware then open monitor
~/.platformio/penv/bin/pio run -t upload && ~/.platformio/penv/bin/pio device monitor
```

If `pio` not in PATH: `export PATH="$HOME/.platformio/penv/bin:$PATH"`

---

## Function map (src/main.cpp)

| Line | Function | Purpose |
|------|----------|---------|
| 85 | `loadConfig()` | read LittleFS /config.json → cfg struct |
| 122 | `saveConfigFromJson()` | write raw JSON to LittleFS, reload cfg |
| 132 | `isAllowedForAlias()` | per-alias auth check |
| 146 | `tgPost()` | fresh-connection HTTPS POST to Telegram API |
| 195 | `tickCallProgress()` | poll AT+CLCC, send progress messages |
| 226 | `isAnyAllowed()` | true if user in any alias allow list |
| 234 | `buildKeyboard()` | JSON inline_keyboard for sendMenu |
| 274 | `sendMenu()` | send message + inline keyboard via tgPost |
| 284 | `hangupCall()` | AT hangup + state reset + displayIdle |
| 289 | `handleCall()` | dial alias, update state, displayCalling |
| 308 | `sendStatus()` | GSM CSQ + battery → Telegram message |
| 332 | `sendUSSD()` | send cfg.ussdCode, return response |
| 342 | `handleMessage()` | dispatch incoming Telegram messages |
| 426 | `checkAuth()` | HTTP Basic Auth for web routes |
| 434 | `initWebServer()` | register all routes + ElegantOTA |
| 476 | `initNTP()` | configTime() + wait for sync |
| 488 | `getTimeStr()` | HH:MM from NTP |
| 496 | `getDateStr()` | DD.MM.YY from NTP |
| 507 | `toDisplayStr()` | Cyrillic UTF-8 → Latin + strip emojis |
| 541 | `drawDisplay()` | full e-paper render (status, bars, battery, footer) |
| 639 | `displayIdle()` | wrapper: drawDisplay("IDLE", ..., false) |
| 643 | `displayCalling()` | wrapper: drawDisplay("CALLING", ..., true) |
| 647 | `initDisplay()` | HSPI + GxEPD2 init, "Starting..." screen |
| 658 | `initPowerManagement()` | IP5306 I2C — set SYS_CTL0=0x37 |
| 670 | `powerCycleModem()` | PWRKEY pulse sequence |
| 688 | `initModem()` | probe → optional power cycle → network wait |
| 719 | `startAP()` | WiFi.softAP("CallerBot-XXXXXXXX") |
| 728 | `initWiFi()` | STA connect 30 s → fallback startAP() |
| 753 | `setup()` | full init sequence |
| 789 | `loop()` | ElegantOTA + CLCC poll + auto-hangup + bot poll |

---

## Known issues / watch-outs

- GSM network registration often fails (WARN after 60 s) if 2G coverage is marginal — device continues without GSM, calls don't work but web admin and WiFi do.
- `modem.getBattPercent()` sometimes returns -1 (SIM800L doesn't always respond to AT+CBC). Battery icon shows empty but doesn't crash.
- 3-colour e-paper refresh is slow. Only update display on call start, call end, and boot — never in polling loops.
- AP mode: Telegram polling is disabled (bot is null in AP mode). Only web admin works.
- `display_flip` in config requires restart to take effect (setRotation is called in initDisplay only).
