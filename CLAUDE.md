# CLAUDE.md — CallerBot project context

Read this before touching any code. Summarises every non-obvious decision so you don't re-derive them.

---

## Hardware

| Part | Details |
|------|---------|
| Board | LILYGO TTGO T-Call V1.4 |
| MCU | ESP32 (Arduino framework via PlatformIO) |
| Modem | SIM800L — 2G only, connected via UART1 (GPIO 26/27) |
| Power IC | IP5306 at I2C 0x75 — **must** set SYS_CTL0=0x37 at boot or SIM800L browns out on TX burst. Keep-alive write every 20 s prevents auto power-off on battery. |
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

Font support: FreeSans covers Latin-1 only. Use `toDisplayStr()` (line 531) to transliterate Cyrillic and strip emojis before printing user-supplied strings.

### Header layout (Nokia 3310 pixel-art style)

Status bar is 17 px tall. All values cached before `display.firstPage()` — no I2C/AT inside render loop.

| Region | x | Content |
|--------|---|---------|
| WiFi icon | 1..7 | 5×5 px fan-arc pixel art |
| WiFi bars | 8..22 | 4 Nokia bars (3 px wide, heights 2/4/6/8, filled=solid / empty=outline) |
| Antenna icon | 26..30 | 5×5 px broadcast-tower pixel art |
| GSM bars | 32..46 | same style |
| Date | centre | DD.MM.YY — default 5×7 font |
| Bolt | battX−11 | 6×8 px red pixel lightning bolt — shown when `ip5.charging` |
| Battery | 190..208 | Nokia body 16×10, nub 3×6, 4 internal 2-px segments (1=25%…4=100%) |

Empty bars use `GxEPD_RED` outline (not black) — intentional visual distinction.

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

### Modem watchdog (`loop()`)

AT ping every 30 s while IDLE. Two consecutive failures → `recoverModem()`:
- `modemSerial.begin()` (flush), `modem.init()`, optional `powerCycleModem()` if soft init fails
- Non-fatal: logs error and retries next cycle instead of halting
- On success: waits 15 s for network, calls `displayIdle("GSM restored")`

**Why needed**: IP5306 boost can cut SIM800L power while ESP32 survives via USB. Without watchdog, device is unresponsive until physical reset.

### IP5306 keep-alive (`loop()`)

SYS_CTL0=0x37 re-written every 20 s (via `Wire`). IP5306 auto-shuts-off boost after ~32 s of light load on battery; periodic write resets that internal timer.

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

## IP5306 battery reading

`readIP5306()` at line 320 reads two registers via I2C:

| Reg | Bit | Meaning |
|-----|-----|---------|
| 0x70 | 3 | `vinPresent` — external power connected |
| 0x78 | 3 | `chargeFull` — charge complete |
| 0x78 | 2 | (unused directly) |
| 0x78 | 1:0 | `battLevel` — `(~r2) & 0x03` → 0=25%, 1=50%, 2=75%, 3=100% |

`charging = vinPresent && !chargeFull`

**Battery level bit inversion**: bits[1:0] of reg 0x78 are inverted (0x03=25%, 0x00=100%) on this hardware — hence `(~r2) & 0x03`. Verify on hardware if level seems backwards; fix is one-liner.

Display refresh triggers only on `battLevel` change, NOT on `charging` change. Reason: `charging` flips immediately on VIN removal; a 15-20 s SPI cycle at that moment risks a brownout mid-transfer leaving BUSY stuck HIGH → infinite render loop.

`lastBattLevel` and `lastCharging` seeded from real IP5306 state in `setup()` (after `initPowerManagement()`) to prevent spurious refresh at first 20 s keep-alive tick.

---

## Function map (src/main.cpp)

| Line | Function | Purpose |
|------|----------|---------|
| 90 | `loadConfig()` | read LittleFS /config.json → cfg struct |
| 127 | `saveConfigFromJson()` | write raw JSON to LittleFS, reload cfg |
| 137 | `isAllowedForAlias()` | per-alias auth check |
| 151 | `tgPost()` | fresh-connection HTTPS POST to Telegram API |
| 200 | `tickCallProgress()` | poll AT+CLCC, send progress messages |
| 231 | `isAnyAllowed()` | true if user in any alias allow list |
| 239 | `buildKeyboard()` | JSON inline_keyboard for sendMenu |
| 279 | `sendMenu()` | send message + inline keyboard via tgPost |
| 289 | `hangupCall()` | AT hangup + state reset + displayIdle |
| 294 | `handleCall()` | dial alias, update state, displayCalling |
| 313 | `IP5306State` | struct: battLevel, charging, chargeFull, vinPresent |
| 320 | `readIP5306()` | read reg 0x70 + 0x78 → IP5306State |
| 341 | `sendStatus()` | GSM CSQ + IP5306 battery → Telegram message |
| 356 | `sendUSSD()` | send cfg.ussdCode, return response |
| 366 | `handleMessage()` | dispatch incoming Telegram messages |
| 450 | `checkAuth()` | HTTP Basic Auth for web routes |
| 458 | `initWebServer()` | register all routes + ElegantOTA |
| 500 | `initNTP()` | configTime() + wait for sync |
| 512 | `getTimeStr()` | HH:MM from NTP |
| 520 | `getDateStr()` | DD.MM.YY from NTP |
| 531 | `toDisplayStr()` | Cyrillic UTF-8 → Latin + strip emojis |
| 565 | `drawDisplay()` | full e-paper render (Nokia header, status, footer) |
| 723 | `displayIdle()` | wrapper: drawDisplay("IDLE", ..., false) |
| 727 | `displayCalling()` | wrapper: drawDisplay("CALLING", ..., true) |
| 731 | `initDisplay()` | HSPI + GxEPD2 init, "Starting..." screen |
| 742 | `initPowerManagement()` | IP5306 I2C — set SYS_CTL0=0x37 |
| 754 | `powerCycleModem()` | PWRKEY pulse sequence |
| 772 | `initModem()` | probe → optional power cycle → network wait (FATAL on failure) |
| 806 | `recoverModem()` | non-fatal modem recovery for loop() watchdog |
| 824 | `startAP()` | WiFi.softAP("CallerBot-XXXXXXXX") |
| 833 | `initWiFi()` | STA connect 30 s → fallback startAP() |
| 858 | `setup()` | full init sequence |
| 899 | `loop()` | OTA + watchdog + CLCC poll + keep-alive + bot poll |

---

## Known issues / watch-outs

- GSM network registration often fails (WARN after 60 s) if 2G coverage is marginal — device continues without GSM, calls don't work but web admin and WiFi do.
- 3-colour e-paper refresh is slow (15–20 s). Only update display on call start, call end, boot, and battery level change — never in polling loops.
- AP mode: Telegram polling is disabled (bot is null in AP mode). Only web admin works.
- `display_flip` in config requires restart to take effect (setRotation is called in initDisplay only).
- IP5306 battery level bit mapping (`(~r2) & 0x03`) was determined empirically. If levels show inverted on hardware, change to `r2 & 0x03` in `readIP5306()`.
- Modem watchdog adds ~1 s blocking AT call every 30 s during idle. If this causes Telegram message latency, increase the interval.
