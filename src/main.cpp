#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <UniversalTelegramBot.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ElegantOTA.h>
#include <time.h>
#include <vector>

// E-Paper — must come after Arduino.h
#include <GxEPD2_3C.h>
#include <epd3c/GxEPD2_213c.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

// ─── Config structs ───────────────────────────────────────────────────────────

struct Alias {
    String command;
    String label;    // display name on keyboard button
    String phone;
    std::vector<long> allowedUsers;
};

struct AppConfig {
    String wifiSsid;
    String wifiPassword;
    String botToken;
    String adminPassword;
    String ussdCode;
    unsigned long callTimeoutMs = CALL_TIMEOUT_MS;
    int gmtOffsetH = 2;
    bool displayFlip = false;
    std::vector<Alias> aliases;
};

AppConfig cfg;

// ─── Globals ──────────────────────────────────────────────────────────────────

HardwareSerial        modemSerial(1);
TinyGsm               modem(modemSerial);
WiFiClientSecure      secureClient;
UniversalTelegramBot* bot = nullptr;
AsyncWebServer        webServer(80);

// SIM800 AT+CLCC stat field values
enum class GsmCallStat { NONE = -1, ACTIVE = 0, HELD = 1, DIALING = 2, ALERTING = 3, INCOMING = 4, WAITING = 5 };

enum class CallState { IDLE, CALLING };
CallState     callState   = CallState::IDLE;
GsmCallStat   lastGsmStat = GsmCallStat::NONE;
unsigned long callStartMs = 0;
String        lastChatId  = "";
long          lastUserId  = 0;

unsigned long lastPollMs    = 0;
unsigned long lastClccMs    = 0;
const unsigned long CLCC_INTERVAL_MS = 4000UL;

bool          shouldRestart  = false;
unsigned long restartAfterMs = 0;
bool          apMode         = false;

// E-Paper display (HSPI — avoids GPIO 23 conflict with MODEM_POWER_ON)
SPIClass epaperSPI(HSPI);
GxEPD2_3C<GxEPD2_213c, GxEPD2_213c::HEIGHT> display(
    GxEPD2_213c(EPAPER_CS, EPAPER_DC, EPAPER_RST, EPAPER_BUSY));

// Last-call tracking
String lastCmdLabel = "";
String lastCmdTime  = "";

// ─── Config IO ───────────────────────────────────────────────────────────────

void loadConfig() {
    if (!LittleFS.exists("/config.json")) {
        Serial.println("WARN: /config.json not found — using empty defaults");
        cfg.adminPassword = "admin";
        return;
    }
    File f = LittleFS.open("/config.json", "r");
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.println("ERROR: config.json parse failed: " + String(err.c_str()));
        return;
    }
    cfg.wifiSsid       = doc["wifi_ssid"]        | "";
    cfg.wifiPassword   = doc["wifi_password"]    | "";
    cfg.botToken       = doc["bot_token"]        | "";
    cfg.adminPassword  = doc["admin_password"]   | "admin";
    cfg.ussdCode       = doc["ussd_code"]        | "";
    int timeoutSec     = doc["call_timeout_s"]   | 0;
    cfg.callTimeoutMs  = timeoutSec > 0 ? (unsigned long)timeoutSec * 1000 : CALL_TIMEOUT_MS;
    cfg.gmtOffsetH     = doc["gmt_offset_h"]     | 2;
    cfg.displayFlip    = doc["display_flip"]     | false;
    cfg.aliases.clear();
    for (JsonObject a : doc["aliases"].as<JsonArray>()) {
        Alias alias;
        alias.command = a["command"] | "";
        alias.label   = a["label"]   | alias.command;  // fallback to command if no label
        alias.phone   = a["phone"]   | "";
        for (JsonVariant u : a["allowed_users"].as<JsonArray>())
            alias.allowedUsers.push_back(u.as<long>());
        if (alias.command.length() && alias.phone.length())
            cfg.aliases.push_back(alias);
    }
    Serial.printf("Config loaded: %d alias(es)\n", cfg.aliases.size());
}

void saveConfigFromJson(JsonVariant& json) {
    File f = LittleFS.open("/config.json", "w");
    if (!f) { Serial.println("ERROR: cannot open config.json for write"); return; }
    serializeJson(json, f);
    f.close();
    loadConfig();
}

// ─── Auth helpers ─────────────────────────────────────────────────────────────

bool isAllowedForAlias(long userId, const Alias& alias) {
    for (long uid : alias.allowedUsers)
        if (uid == userId) return true;
    return false;
}

// forward declarations
void sendMenu(const String& chatId, long userId, const String& text);
void displayIdle(const String& sub = "Waiting for command");
void displayCalling(const String& label);
String getTimeStr();

// ─── Direct Telegram POST (bypasses library's broken sendPostMessage) ─────────

bool tgPost(const String& method, const String& body) {
    WiFiClientSecure c;
    c.setInsecure();
    if (!c.connect("api.telegram.org", 443, 10000)) {
        Serial.println("tgPost: connect fail");
        return false;
    }
    String header = "POST /bot" + cfg.botToken + "/" + method + " HTTP/1.0\r\n"
                    "Host: api.telegram.org\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + String(body.length()) + "\r\n\r\n";
    c.print(header);
    c.print(body);

    unsigned long t = millis();
    String resp;
    while (millis() - t < 8000) {
        while (c.available()) { resp += (char)c.read(); t = millis(); }
        if (!c.connected()) break;
        delay(1);
    }
    c.stop();
    bool ok = resp.indexOf("\"ok\":true") >= 0;
    if (!ok) Serial.println("tgPost err: " + resp.substring(0, 150));
    return ok;
}

// ─── CLCC call progress ───────────────────────────────────────────────────────

// Polls AT+CLCC, returns current GSM call stat or NONE if no active call.
GsmCallStat pollClcc() {
    modem.sendAT(GF("+CLCC"));
    String resp;
    // waitResponse fills resp with everything up to OK/ERROR
    if (modem.waitResponse(1000L, resp) != 1) return GsmCallStat::NONE;
    // +CLCC: idx,dir,stat,mode,mpty[,number,type]
    int clccIdx = resp.indexOf("+CLCC:");
    if (clccIdx < 0) return GsmCallStat::NONE;  // no active call
    // extract stat field (3rd comma-delimited value after +CLCC:)
    String entry = resp.substring(clccIdx + 7);
    int f1 = entry.indexOf(',');
    int f2 = entry.indexOf(',', f1 + 1);
    int f3 = entry.indexOf(',', f2 + 1);
    if (f1 < 0 || f2 < 0) return GsmCallStat::NONE;
    int stat = entry.substring(f2 + 1, f3 < 0 ? entry.length() : f3).toInt();
    return (GsmCallStat)stat;
}

// Called from loop() every CLCC_INTERVAL_MS while a call is active.
void tickCallProgress() {
    GsmCallStat stat = pollClcc();

    if (stat == GsmCallStat::NONE && lastGsmStat != GsmCallStat::NONE) {
        // call dropped (no answer, busy, remote hangup)
        callState   = CallState::IDLE;
        lastGsmStat = GsmCallStat::NONE;
        unsigned long dur = (millis() - callStartMs) / 1000;
        String endMsg = "📵 Call ended after " + String(dur) + "s.";
        if (lastChatId.length() && bot)
            sendMenu(lastChatId, lastUserId, endMsg);
        displayIdle("Call ended " + String(dur) + "s");
        return;
    }

    if (stat == lastGsmStat) return;  // no state change, skip message

    lastGsmStat = stat;
    if (!lastChatId.length() || !bot) return;

    switch (stat) {
        case GsmCallStat::DIALING:
            bot->sendMessage(lastChatId, "⏳ Dialing...", "");    break;
        case GsmCallStat::ALERTING:
            bot->sendMessage(lastChatId, "📳 Ringing...", "");    break;
        case GsmCallStat::ACTIVE:
            bot->sendMessage(lastChatId, "✅ Call answered.", ""); break;
        default: break;
    }
}

bool isAnyAllowed(long userId) {
    for (const auto& alias : cfg.aliases)
        if (isAllowedForAlias(userId, alias)) return true;
    return false;
}

// ─── Keyboard builder ─────────────────────────────────────────────────────────

String buildKeyboard(long userId) {
    DynamicJsonDocument doc(2048);
    JsonArray rows = doc.createNestedArray("inline_keyboard");

    // Call buttons — 2 per row
    JsonArray row;
    int col = 0;
    for (const auto& alias : cfg.aliases) {
        if (!isAllowedForAlias(userId, alias)) continue;
        if (col % 2 == 0) row = rows.createNestedArray();
        JsonObject btn = row.createNestedObject();
        btn["text"]          = "📞 " + (alias.label.length() ? alias.label : alias.command);
        btn["callback_data"] = "call:" + alias.command;
        col++;
    }

    // Hang up — only when call active
    if (callState != CallState::IDLE) {
        JsonArray r = rows.createNestedArray();
        JsonObject btn = r.createNestedObject();
        btn["text"]          = "🔴 Hang Up";
        btn["callback_data"] = "hangup";
    }

    // Status + Echo row
    JsonArray actionRow = rows.createNestedArray();
    JsonObject statusBtn = actionRow.createNestedObject();
    statusBtn["text"]          = "📊 Status";
    statusBtn["callback_data"] = "status";
    if (cfg.ussdCode.length()) {
        JsonObject echoBtn = actionRow.createNestedObject();
        echoBtn["text"]          = "💵 Balance";
        echoBtn["callback_data"] = "echo";
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void sendMenu(const String& chatId, long userId, const String& text) {
    String kb   = buildKeyboard(userId);
    String body = "{\"chat_id\":\"" + chatId + "\","
                  "\"text\":\"" + text + "\","
                  "\"reply_markup\":" + kb + "}";
    tgPost("sendMessage", body);
}

// ─── Telegram command handlers ───────────────────────────────────────────────

void hangupCall(const String& chatId) {
    modem.callHangup();
    callState = CallState::IDLE;
}

void handleCall(const String& chatId, const Alias& alias) {
    if (callState != CallState::IDLE) {
        bot->sendMessage(chatId, "Call already in progress.", "");
        return;
    }
    if (modem.callNumber(alias.phone.c_str())) {
        callState    = CallState::CALLING;
        callStartMs  = millis();
        lastGsmStat  = GsmCallStat::NONE;
        lastClccMs   = millis();
        lastChatId   = chatId;
        lastCmdLabel = alias.label.length() ? alias.label : alias.command;
        lastCmdTime  = getTimeStr();
        displayCalling(lastCmdLabel);
    } else {
        bot->sendMessage(chatId, "Failed to dial. Check modem/SIM.", "");
    }
}

void sendStatus(const String& chatId) {
    int csq = modem.getSignalQuality();

    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_READ2);
    Wire.endTransmission(false);
    Wire.requestFrom(IP5306_ADDR, 1);
    uint8_t reg = Wire.available() ? Wire.read() : 0;
    bool charging = (reg >> 3) & 0x01;
    bool full     = (reg >> 2) & 0x01;

    int battPct = modem.getBattPercent();

    String msg = "📶 Signal: " + String(csq) + "/31\n";
    if (battPct >= 0) msg += "🔋 Battery: " + String(battPct) + "%";
    else              msg += "🔋 Battery: n/a";
    if (charging) msg += " ⚡";
    else if (full) msg += " ✅";
    msg += "\n📞 Call: ";
    msg += (callState == CallState::IDLE) ? "idle" : "active";

    bot->sendMessage(chatId, msg, "");
}

void sendUSSD(const String& chatId) {
    if (!cfg.ussdCode.length()) {
        bot->sendMessage(chatId, "USSD code not set. Configure via web admin.", "");
        return;
    }
    bot->sendMessage(chatId, "⏳ Sending USSD " + cfg.ussdCode + "...", "");
    String response = modem.sendUSSD(cfg.ussdCode);
    bot->sendMessage(chatId, response.length() ? response : "No response from operator.", "");
}

void handleMessage(const telegramMessage& msg) {
    long   userId     = msg.from_id.toInt();
    bool   isCallback = (msg.type == "callback_query");
    String input      = msg.text;

    if (!isCallback) {
        input.trim();
        int at = input.indexOf('@');
        if (at > 0) input = input.substring(0, at);
    }

    auto answerCb = [&](const String& notice = "") {
        if (isCallback) bot->answerCallbackQuery(msg.query_id, notice);
    };

    if (input == "/start" || input == "/help") {
        sendMenu(msg.chat_id, userId, "Choose action:");
        answerCb();
        return;
    }

    if (input == "/getMyId") {
        bot->sendMessage(msg.chat_id,
            "Your Telegram ID: " + msg.from_id + "\n"
            "Name: " + msg.from_name, "");
        answerCb();
        return;
    }

    if (!isAnyAllowed(userId)) {
        answerCb("Unauthorized");
        if (!isCallback) bot->sendMessage(msg.chat_id, "Unauthorized.", "");
        return;
    }

    if (input == "/status" || input == "status") {
        sendStatus(msg.chat_id);
        answerCb();
        return;
    }

    if (input == "/echo" || input == "echo") {
        sendUSSD(msg.chat_id);
        answerCb();
        return;
    }

    if (input == "/hangup" || input == "hangup") {
        hangupCall(msg.chat_id);
        answerCb("Call ended");
        sendMenu(msg.chat_id, userId, "📵 Call ended.");
        return;
    }

    // call:alias (callback) or /alias (text command)
    String target = "";
    if (input.startsWith("call:"))  target = input.substring(5);
    else if (input.startsWith("/")) target = input.substring(1);

    if (target.length()) {
        for (const auto& alias : cfg.aliases) {
            if (alias.command == target) {
                if (!isAllowedForAlias(userId, alias)) {
                    answerCb("Unauthorized");
                    if (!isCallback) bot->sendMessage(msg.chat_id, "Unauthorized for this command.", "");
                } else {
                    lastUserId = userId;
                    handleCall(msg.chat_id, alias);
                    if (callState == CallState::CALLING) {
                        answerCb("Dialling...");
                        sendMenu(msg.chat_id, userId, "📞 Ringing " + alias.phone + "...");
                    }
                }
                return;
            }
        }
    }

    answerCb("Unknown");
    if (!isCallback) bot->sendMessage(msg.chat_id, "Unknown command. Try /help", "");
}

// ─── Web admin ────────────────────────────────────────────────────────────────

bool checkAuth(AsyncWebServerRequest* req) {
    if (!req->authenticate("admin", cfg.adminPassword.c_str())) {
        req->requestAuthentication();
        return false;
    }
    return true;
}

void initWebServer() {
    // API routes must be registered BEFORE serveStatic — handlers checked in order of addition

    webServer.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        req->send(LittleFS, "/config.json", "application/json");
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!checkAuth(req)) return;
        DynamicJsonDocument doc(256);
        doc["csq"]  = modem.getSignalQuality();
        doc["call"] = (callState == CallState::IDLE) ? "idle" : "calling";
        int battPct = modem.getBattPercent();
        if (battPct >= 0) doc["battery_pct"] = battPct;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    AsyncCallbackJsonWebHandler* saveHandler = new AsyncCallbackJsonWebHandler(
        "/api/config",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            if (!checkAuth(req)) return;
            saveConfigFromJson(json);
            req->send(200, "application/json", "{\"ok\":true}");
            shouldRestart  = true;
            restartAfterMs = millis();
        }
    );
    webServer.addHandler(saveHandler);

    // Static files last — catches everything not matched above
    webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // OTA firmware update — protected by admin credentials
    ElegantOTA.setAuth("admin", cfg.adminPassword.c_str());
    ElegantOTA.begin(&webServer);
}

// ─── NTP helpers ─────────────────────────────────────────────────────────────

void initNTP() {
    if (!WiFi.isConnected()) return;
    configTime((long)cfg.gmtOffsetH * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("NTP sync");
    struct tm t;
    for (int i = 0; i < 20; i++) {
        if (getLocalTime(&t, 500)) { Serial.println(" OK"); return; }
        Serial.print(".");
    }
    Serial.println(" FAIL");
}

String getTimeStr() {
    struct tm t;
    if (!getLocalTime(&t, 50)) return "--:--";
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    return String(buf);
}

String getDateStr() {
    struct tm t;
    if (!getLocalTime(&t, 50)) return "--.--.--";
    char buf[9];
    strftime(buf, sizeof(buf), "%d.%m.%y", &t);
    return String(buf);
}

// ─── E-Paper display ──────────────────────────────────────────────────────────

// Transliterate Cyrillic UTF-8 → ASCII; strip emojis and other non-Latin.
String toDisplayStr(const String& s) {
    static const char* tbl[32] = {
        "A","B","V","G","D","E","ZH","Z","I","Y","K","L","M","N","O","P",   // А-П
        "R","S","T","U","F","KH","TS","CH","SH","SHCH","","Y","","E","YU","YA" // Р-Я
    };
    String out;
    const uint8_t* p = (const uint8_t*)s.c_str();
    int n = s.length();
    for (int i = 0; i < n; ) {
        uint8_t b = p[i];
        if (b < 0x80) { out += (char)b; i++; continue; }
        if (i + 1 < n) {
            uint8_t b2 = p[i + 1];
            if (b == 0xD0) {
                if      (b2 >= 0x90 && b2 <= 0xAF) out += tbl[b2 - 0x90];
                else if (b2 >= 0xB0)               out += tbl[b2 - 0xB0];
                else if (b2 == 0x84)               out += "YE";
                else if (b2 == 0x86 || b2 == 0x87) out += "I";
                i += 2; continue;
            }
            if (b == 0xD1) {
                if      (b2 >= 0x80 && b2 <= 0x8F) out += tbl[(b2 - 0x80) + 16];
                else if (b2 == 0x94)               out += "ye";
                else if (b2 == 0x96 || b2 == 0x97) out += "i";
                i += 2; continue;
            }
        }
        if      (b >= 0xF0) i += 4;
        else if (b >= 0xE0) i += 3;
        else                i += 2;
    }
    return out;
}

void drawDisplay(const String& statusText, const String& subtextLine, bool useColor) {
    int16_t tbx, tby; uint16_t tbw, tbh;

    // Cache all modem/system values before entering render loop
    int wifiBars = 0;
    if (!apMode && WiFi.isConnected()) {
        int rssi = WiFi.RSSI();
        wifiBars = rssi >= -50 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    }
    int csq     = modem.getSignalQuality();
    int gsmBars = (csq == 99 || csq == 0) ? 0 : max(1, min(4, csq * 4 / 31 + 1));
    int battPct = modem.getBattPercent();
    String dateStr = getDateStr();

    String footerRow1, footerRow2;
    if (apMode) {
        footerRow1 = "CallerBot-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
        footerRow2 = WiFi.softAPIP().toString();
    } else {
        footerRow1 = WiFi.isConnected() ? WiFi.localIP().toString() : "offline";
        footerRow2 = lastCmdLabel.length()
            ? "Last: " + toDisplayStr(lastCmdLabel) + " @" + lastCmdTime
            : "No calls yet";
    }

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // ── Header: WiFi bars | GSM bars | battery icon ──────────────────────
        // Signal bars: 4 bars, baseline y=15, growing upward
        auto drawBars = [&](int ox, int filled) {
            for (int i = 0; i < 4; i++) {
                int bh = (i + 1) * 3 + 1;  // 4, 7, 10, 13 px tall
                int bx = ox + i * 7;
                int by = 15 - bh;
                display.fillRect(bx, by, 5, bh, GxEPD_BLACK);
                if (i >= filled)
                    display.fillRect(bx + 1, by + 1, 3, bh - 2, GxEPD_WHITE);
            }
        };
        drawBars(2,  wifiBars);  // WiFi: x=2..28
        drawBars(32, gsmBars);   // GSM:  x=32..58

        // Battery icon: outline + nub + proportional fill
        display.drawRect(62, 3, 22, 12, GxEPD_BLACK);   // body
        display.fillRect(84, 6,  3,  6, GxEPD_BLACK);   // positive nub
        if (battPct > 0) {
            int fw = max(1, battPct * 20 / 100);
            display.fillRect(63, 4, fw, 10, GxEPD_BLACK);
        }

        display.drawFastHLine(0, 17, 212, GxEPD_BLACK);

        // ── Main status ───────────────────────────────────────────────────────
        display.setFont(&FreeSansBold12pt7b);
        display.setTextColor(useColor ? GxEPD_RED : GxEPD_BLACK);
        display.getTextBounds(statusText.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((212 - tbw) / 2 - tbx, 48);
        display.print(statusText);

        // ── Subtext ───────────────────────────────────────────────────────────
        if (subtextLine.length()) {
            String sub = toDisplayStr(subtextLine);
            display.setFont(&FreeSans9pt7b);
            display.setTextColor(GxEPD_BLACK);
            display.getTextBounds(sub.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
            display.setCursor((212 - tbw) / 2 - tbx, 65);
            display.print(sub);
        }

        // ── Footer ────────────────────────────────────────────────────────────
        display.drawFastHLine(0, 72, 212, GxEPD_BLACK);
        display.setFont(nullptr);
        display.setTextSize(1);
        display.setTextColor(GxEPD_BLACK);

        if (apMode) {
            // AP mode: row1=SSID, row2=IP
            display.setCursor(2, 83);
            display.print(footerRow1);
            display.setCursor(2, 97);
            display.print(footerRow2);
        } else {
            // STA mode: row1 = date | IP, row2 = last command
            display.setCursor(2, 83);
            display.print(dateStr);
            display.getTextBounds(footerRow1.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
            display.setCursor(210 - tbw, 83);
            display.print(footerRow1);
            display.setCursor(2, 97);
            display.print(footerRow2);
        }

    } while (display.nextPage());
}

void displayIdle(const String& sub) {
    drawDisplay("IDLE", sub, false);
}

void displayCalling(const String& label) {
    drawDisplay("CALLING", label, true);
}

void initDisplay() {
    epaperSPI.begin(EPAPER_CLK, -1, EPAPER_MOSI, EPAPER_CS);
    display.epd2.selectSPI(epaperSPI, SPISettings(1000000, MSBFIRST, SPI_MODE0));
    display.init(0, true, 200, false);
    display.setRotation(cfg.displayFlip ? 3 : 1);
    drawDisplay("Starting...", "", false);
    Serial.println("Display OK.");
}

// ─── Hardware init ────────────────────────────────────────────────────────────

void initPowerManagement() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_SYS_CTL0);
    Wire.write(0x37);  // always-on boost; prevents SIM800L TX burst brown-out
    if (Wire.endTransmission() == 0)
        Serial.println("IP5306: boost enabled");
    else
        Serial.println("IP5306: not found (OK on some revisions)");
    delay(100);
}

void powerCycleModem() {
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    delay(100);

    pinMode(MODEM_RST, OUTPUT);
    digitalWrite(MODEM_RST, HIGH); delay(200);
    digitalWrite(MODEM_RST, LOW);  delay(200);
    digitalWrite(MODEM_RST, HIGH); delay(200);

    // PWRKEY LOW pulse ≥1 s (SIM800L datasheet requirement)
    pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(1500);
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(3000);  // wait for RDY + Call Ready + SMS Ready
}

void initModem() {
    Serial.println("Initialising modem...");

    modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(100);

    // Probe first — modem may already be running after SW reset
    if (!modem.init()) {
        Serial.println("Modem not responding — power cycling...");
        powerCycleModem();
        if (!modem.init()) {
            Serial.println("FATAL: modem.init() failed after power cycle — halting");
            while (true) delay(1000);
        }
    }
    Serial.println("Modem OK.");

    Serial.print("Waiting for network");
    for (int i = 0; i < 60; i++) {
        if (modem.isNetworkConnected()) { Serial.println(" OK"); break; }
        Serial.print(".");
        delay(1000);
    }
    if (!modem.isNetworkConnected())
        Serial.println("\nWARN: not registered after 60s — check SIM/antenna");

    int csq = modem.getSignalQuality();
    Serial.printf("CSQ: %d (%s)\n", csq,
        csq == 99 ? "unknown" : csq < 10 ? "poor" : csq < 20 ? "fair" : "good");
}

void startAP() {
    apMode = true;
    // SSID unique per chip, no password — open network for initial setup
    String ssid = "CallerBot-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid.c_str());
    Serial.println("AP mode: SSID=" + ssid + "  IP=" + WiFi.softAPIP().toString());
}

void initWiFi() {
    if (!cfg.wifiSsid.length()) {
        Serial.println("WiFi SSID empty — starting AP");
        startAP();
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
    Serial.print("Connecting to WiFi");
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > 30000) {
            Serial.println("\nWiFi timeout — starting AP");
            startAP();
            return;
        }
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi: " + WiFi.localIP().toString());
    secureClient.setInsecure();
}

// ─── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32 Caller Bot ===");

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS mount failed — halting");
        while (true) delay(1000);
    }
    loadConfig();

    initDisplay();        // show "Starting..." early
    initPowerManagement();
    initModem();
    initWiFi();
    initNTP();

    if (cfg.botToken.length()) {
        bot = new UniversalTelegramBot(cfg.botToken, secureClient);
        bot->getUpdates(bot->last_message_received + 1);
        Serial.println("Telegram bot ready.");
    } else {
        Serial.println("WARN: bot_token empty — Telegram disabled");
    }

    initWebServer();
    webServer.begin();
    if (apMode)
        Serial.println("Web admin: http://" + WiFi.softAPIP().toString() + "  (AP mode)");
    else
        Serial.println("Web admin: http://" + WiFi.localIP().toString());
    Serial.println("Ready.");

    displayIdle();
}

void loop() {
    // delayed restart after config save
    if (shouldRestart && millis() - restartAfterMs > 2000) {
        Serial.println("Restarting...");
        ESP.restart();
    }

    // call progress polling
    if (callState == CallState::CALLING && millis() - lastClccMs >= CLCC_INTERVAL_MS) {
        lastClccMs = millis();
        tickCallProgress();
    }

    // auto-hangup
    if (callState == CallState::CALLING && millis() - callStartMs > cfg.callTimeoutMs) {
        modem.callHangup();
        callState   = CallState::IDLE;
        lastGsmStat = GsmCallStat::NONE;
        if (lastChatId.length() && bot)
            sendMenu(lastChatId, lastUserId, "⏱ Auto-hangup: timeout reached.");
        displayIdle("Auto-hangup");
    }

    ElegantOTA.loop();

    // Telegram polling (STA mode only)
    if (!apMode && bot && WiFi.isConnected() && millis() - lastPollMs >= BOT_POLL_MS) {
        lastPollMs = millis();
        int numMsg = bot->getUpdates(bot->last_message_received + 1);
        if (numMsg > 0) Serial.printf("Bot: %d message(s)\n", numMsg);
        for (int i = 0; i < numMsg; i++) {
            Serial.printf("  [%s] from:%s text:%s\n",
                bot->messages[i].type.c_str(),
                bot->messages[i].from_id.c_str(),
                bot->messages[i].text.c_str());
            handleMessage(bot->messages[i]);
        }
    }
}
