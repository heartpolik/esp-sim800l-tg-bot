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

Font support: FreeSans covers Latin-1 only. Use `toDisplayStr()` (line ~699) to transliterate Cyrillic and strip emojis before printing user-supplied strings.

### Display layout

Two header rows, then status body, then footer.

**Row 1 (y=0–10):** Signal + operator + IP + battery
| Region | Position | Content |
|--------|----------|---------|
| GSM tower icon | x=1–5 | 5×5 px broadcast-tower pixel art |
| GSM bars | x=8–22 | 4 Nokia bars |
| Operator name | centred between GSM and WiFi | default 5×7 font |
| WiFi fan icon | dynamic (before bars) | 7×5 px fan-arc pixel art |
| WiFi bars | dynamic (before IP) | 4 Nokia bars |
| IP address | right-aligned, ends at x=188 | default font |
| Battery | x=190–208, y=1 | Nokia body 16×10, nub 3×6, 4 segments |
| Charging bolt | x=179–184, y=2 | 6×8 px red lightning bolt |

WiFi bars/icon x positions computed at render time: `wifiBarsStart = (battX-2-ip_width) - 17`. Operator centred between x=24 and WiFi icon left edge.

Separator line at y=11.

**Row 2 (y=12–22):** Date + USSD balance
| Region | Position | Content |
|--------|----------|---------|
| Date | x=2, baseline y=21 | DD.MM.YY |
| Clock icon + minutes | x=52+, y=15 | 5×5 px clock + "15m" |
| Calendar icon + nextPay | dynamic | 5×5 px calendar + "DD.MM" (month only, no year) |
| SIM icon + expiry | dynamic | 5×5 px SIM card + "DD.MM.YY" (full date — year matters) |

Balance items packed left from x=52. Max combined width ~116px — clears battery bolt at x=179.

Separator line at y=23.

**Body (y=24–85):**
- Status text ("IDLE" / "CALLING") in FreeSansBold12pt7b, baseline y=55, centred
- Subtext in FreeSans9pt7b, baseline y=73, centred

**Footer (y=95):** Last call — label @HH:MM username

Empty bars use `GxEPD_RED` outline (not black) — intentional visual distinction.

### Display task (non-blocking rendering)

Rendering runs on **Core 0** in `displayTask`. Core 1 (`loop()`) fills a `DisplaySnapshot` struct, signals via binary semaphore, and returns immediately. The 15–20 s SPI refresh never blocks `loop()`.

```cpp
xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);
```

`drawDisplay()` on Core 1: reads I2C (IP5306) and AT (modem), fills snapshot, gives semaphore.
`renderSnapshot()` on Core 0: reads only pre-cached snapshot values — NO I2C/AT inside.

### INT WDT fix

GxEPD2 `_waitWhileBusy` calls `delay(1)` ~30,000 times per 30 s refresh. This causes FreeRTOS spinlock contention between Core 0 (display task) and Core 1 (loop), triggering Interrupt WDT.

Fix: set busy-poll interval to 50 ms:
```cpp
display.epd2.setBusyCallback([](const void*){ delay(50); }, nullptr);
```
Reduces context switches from ~30,000 to ~600 per refresh.

### Busy timeout

UC8151 refresh can exceed GxEPD2's default 20 s timeout. Override via subclass:
```cpp
struct GxEPD2_213c_30s : public GxEPD2_213c {
    GxEPD2_213c_30s(...) : GxEPD2_213c(...) { _busy_timeout = 40000000; }
};
```
Do NOT edit `.pio/libdeps/` — gets overwritten on clean. Subclass in main.cpp only.

### BUSY stuck HIGH guard

If BUSY pin is HIGH before `display.init()`, the UC8151 is wedged (e.g. from brownout mid-refresh). `display.init()` would block forever. `initDisplay()` pre-checks and sets `displayOk = false` if HIGH — device continues without display until power-cycle.

---

## Telegram bot

`UniversalTelegramBot` GET-based polling works fine. Its `sendMessageWithInlineKeyboard` (POST) is broken — the SSL connection left open by `getUpdates` is in a bad state for subsequent POSTs.

Fix: custom `tgPost(method, body)` at line ~193 opens a **fresh `WiFiClientSecure`** per call, uses **HTTP/1.0** (no chunked encoding), reads until disconnected. All keyboard messages go through `tgPost`.

Bot is created after WiFi connects:
```cpp
bot = new UniversalTelegramBot(cfg.botToken, secureClient);
bot->getUpdates(bot->last_message_received + 1);  // drain stale messages
```

Polling only in STA mode (not AP mode) in `loop()`.

### Admin access control

`isAdmin(userId)` checks `cfg.adminUserId != 0 && userId == cfg.adminUserId`.

Admin-only keyboard buttons: Status, Balance (USSD), Reboot. "My ID" shown to everyone.

---

## USSD

TinyGSM `sendUSSDImpl` is broken for SIM800: some firmware sends `+CUSD:` before `OK`, so the first `waitResponse()` consumes `+CUSD:` and the second returns empty.

Fix: `ussdRaw(code)` at line ~449 — sends `AT+CUSD=1,"code"` raw, reads serial for up to 15 s, finds `+CUSD:` manually. Handles DCS=72 (UCS2 hex → UTF-8 decode) and DCS=15 (plain ASCII).

### Balance parsing

`parseBalance(response)` at line ~425 extracts from USSD response:
- `minutes`: digits before first `"hv"` (e.g. `"15hv"` → `"15"`)
- `nextPay`: 5 chars (DD.MM) after `"poslug "` — year dropped to save display space
- `expiry`: 8 chars (DD.MM.YY) after `"diye do "` — year kept, matters for SIM validity

Stored in `lastBalance` global. Refreshed:
1. At boot (after modem + network ready) — silently, no Telegram message
2. After each Balance command from Telegram

After update, `displayIdle("")` is called to refresh the display.

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

`modemOk` flag: set to `true` after successful `modem.init()`. Guards all AT calls in `drawDisplay()` and `handleCall()`. Cleared in `recoverModem()` during recovery attempt.

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

AT+CLCC polled every 4 s while `callState == CALLING`. `tickCallProgress()` at line ~242.

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
| `admin_user_id` | long | 0 | Telegram user ID for admin commands; 0 = disabled |
| `ussd_code` | string | — | sent by Balance button and on boot |
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

`readIP5306()` at line ~389 reads two registers via I2C:

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
| 131 | `loadConfig()` | read LittleFS /config.json → cfg struct |
| 169 | `saveConfigFromJson()` | write raw JSON to LittleFS, reload cfg |
| 179 | `isAllowedForAlias()` | per-alias auth check |
| 193 | `tgPost()` | fresh-connection HTTPS POST to Telegram API |
| 242 | `tickCallProgress()` | poll AT+CLCC, send progress messages |
| 273 | `isAnyAllowed()` | true if user in any alias allow list |
| 279 | `isAdmin()` | true if userId == cfg.adminUserId |
| 285 | `buildKeyboard()` | JSON inline_keyboard; admin buttons gated by isAdmin() |
| 338 | `sendMenu()` | send message + inline keyboard via tgPost |
| 348 | `hangupCall()` | AT hangup + state reset + displayIdle |
| 353 | `handleCall()` | dial alias, update state, displayCalling |
| 382 | `IP5306State` | struct: battLevel, charging, chargeFull, vinPresent |
| 389 | `readIP5306()` | read reg 0x70 + 0x78 → IP5306State |
| 410 | `sendStatus()` | GSM CSQ + IP5306 battery → Telegram message |
| 425 | `parseBalance()` | extract minutes/nextPay/expiry from USSD response string |
| 449 | `ussdRaw()` | raw AT+CUSD send + 15 s serial read + DCS decode |
| 498 | `sendUSSD()` | isNetworkConnected guard + ussdRaw + parseBalance + displayIdle |
| 518 | `handleMessage()` | dispatch incoming Telegram messages |
| 618 | `checkAuth()` | HTTP Basic Auth for web routes |
| 626 | `initWebServer()` | register all routes + ElegantOTA |
| 668 | `initNTP()` | configTime() + wait for sync |
| 680 | `getTimeStr()` | HH:MM from NTP |
| 688 | `getDateStr()` | DD.MM.YY from NTP |
| 699 | `toDisplayStr()` | Cyrillic UTF-8 → Latin + strip emojis |
| 739 | `renderSnapshot()` | full e-paper render (called from display task, Core 0) |
| 939 | `displayTask()` | FreeRTOS task on Core 0: waits on snapSignal, calls renderSnapshot |
| 953 | `drawDisplay()` | fills DisplaySnapshot (I2C + AT on Core 1), signals display task |
| 998 | `displayIdle()` | wrapper: drawDisplay("IDLE", ..., false) |
| 1002 | `displayCalling()` | wrapper: drawDisplay("CALLING", ..., true) |
| 1006 | `initDisplay()` | HSPI + GxEPD2 init + busy callback + "Starting..." |
| 1031 | `initPowerManagement()` | I2C bus recovery + Wire.begin + IP5306 boost always-on |
| 1054 | `powerCycleModem()` | PWRKEY pulse sequence |
| 1072 | `initModem()` | probe → optional power cycle → network wait (FATAL on failure) |
| 1108 | `recoverModem()` | non-fatal modem recovery for loop() watchdog |
| 1128 | `startAP()` | WiFi.softAP("CallerBot-XXXXXXXX") |
| 1137 | `initWiFi()` | STA connect 30 s → fallback startAP() |
| 1162 | `setup()` | full init sequence + boot USSD balance fetch |
| 1216 | `loop()` | OTA + watchdog + CLCC poll + keep-alive + bot poll |

---

## Known issues / watch-outs

- GSM network registration often fails (WARN after 60 s) if 2G coverage is marginal — device continues without GSM, calls don't work but web admin and WiFi do.
- 3-colour e-paper refresh is slow (15–20 s). Only update display on call start, call end, boot, balance query, and battery level change — never in polling loops.
- AP mode: Telegram polling is disabled (bot is null in AP mode). Only web admin works. Balance not fetched in AP mode.
- `display_flip` in config requires restart to take effect (setRotation is called in initDisplay only).
- IP5306 battery level bit mapping (`(~r2) & 0x03`) was determined empirically. If levels show inverted on hardware, change to `r2 & 0x03` in `readIP5306()`.
- Modem watchdog adds ~1 s blocking AT call every 30 s during idle. If this causes Telegram message latency, increase the interval.
- Boot USSD fetch adds up to 15 s to startup time (ussdRaw timeout). Only runs if modem registered and ussd_code configured.
- Charging bolt (x=179–184) can visually overlap with IP address text in row 1 when charging AND IP ends near x=185+. Rare in practice — bolt only shown when charging.
