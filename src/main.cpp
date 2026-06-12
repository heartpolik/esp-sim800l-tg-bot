#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "config.h"
#include <Arduino.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
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

// UC8151 3-colour full refresh takes 15-20 s; default busy_timeout is 20 s
// which is too tight. Override via subclass (protected field, can't set externally).
struct GxEPD2_213c_30s : public GxEPD2_213c {
    GxEPD2_213c_30s(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
        : GxEPD2_213c(cs, dc, rst, busy) { _busy_timeout = 40000000; }
};

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
    long   adminUserId  = 0;
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
long          bootTimeUnix  = 0;   // Unix time recorded after NTP; stale-message filter
unsigned long lastClccMs    = 0;
const unsigned long CLCC_INTERVAL_MS = 4000UL;

bool          shouldRestart  = false;
unsigned long restartAfterMs = 0;
bool          apMode         = false;
bool          displayOk      = false;
bool          modemOk        = false;

struct BalanceInfo {
    String minutes;  // "15" (parsed from "15hv")
    String nextPay;  // "04.09.26" (after "poslug ")
    String expiry;   // "12.06.27" (after "diye do ")
};
static BalanceInfo lastBalance;

// Display task (Core 0) — all SPI render work runs here so Core 1 never blocks
struct DisplaySnapshot {
    String statusText;
    String subtextLine;  // pre-transliterated via toDisplayStr()
    bool   useColor;
    int    wifiBars;
    int    gsmBars;
    int    battLevel;
    bool   charging;
    String dateStr;
    String footerRow1;
    String footerRow2;
    String operatorName;
    bool   apModeSnap;
    // Parsed USSD balance fields (empty until first balance query)
    String balMinutes;
    String balNextPay;
    String balExpiry;
};
static DisplaySnapshot   pendingSnap;
static SemaphoreHandle_t snapMutex  = nullptr;  // protects pendingSnap
static SemaphoreHandle_t snapSignal = nullptr;  // binary: "render requested"

// E-Paper display (HSPI — avoids GPIO 23 conflict with MODEM_POWER_ON)
SPIClass epaperSPI(HSPI);
GxEPD2_3C<GxEPD2_213c_30s, GxEPD2_213c_30s::HEIGHT> display(
    GxEPD2_213c_30s(EPAPER_CS, EPAPER_DC, EPAPER_RST, EPAPER_BUSY));

// Last-call tracking
String lastCmdLabel = "";
String lastCmdTime  = "";
String lastCmdUser  = "";

// IP5306 battery state tracking (for display refresh on change)
int           lastBattLevel   = -1;
bool          lastCharging    = false;
unsigned long lastBattCheckMs = 0;

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
    cfg.adminUserId    = doc["admin_user_id"]    | 0;
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

bool isAdmin(long userId) {
    return cfg.adminUserId != 0 && userId == cfg.adminUserId;
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

    // Admin-only row: Status, Balance, Reboot
    if (isAdmin(userId)) {
        JsonArray adminRow = rows.createNestedArray();
        JsonObject statusBtn = adminRow.createNestedObject();
        statusBtn["text"]          = "📊 Status";
        statusBtn["callback_data"] = "status";
        if (cfg.ussdCode.length()) {
            JsonObject echoBtn = adminRow.createNestedObject();
            echoBtn["text"]          = "💵 Balance";
            echoBtn["callback_data"] = "echo";
        }
        JsonObject rebootBtn = adminRow.createNestedObject();
        rebootBtn["text"]          = "🔄 Reboot";
        rebootBtn["callback_data"] = "reboot";
    }

    // My ID row — visible to everyone
    {
        JsonArray idRow = rows.createNestedArray();
        JsonObject idBtn = idRow.createNestedObject();
        idBtn["text"]          = "🪪 My ID";
        idBtn["callback_data"] = "getmyid";
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
    Serial.printf("[handleCall] phone=%s state=%d modemOk=%d\n",
        alias.phone.c_str(), (int)callState, (int)modemOk);
    if (callState != CallState::IDLE) {
        bot->sendMessage(chatId, "Call already in progress.", "");
        return;
    }
    if (!modem.isNetworkConnected()) {
        bot->sendMessage(chatId, "❌ GSM not registered. Check SIM/antenna.", "");
        return;
    }
    bool dialOk = modem.callNumber(alias.phone.c_str());
    Serial.printf("[handleCall] callNumber=%d registered=%d\n",
        (int)dialOk, (int)modem.isNetworkConnected());
    if (dialOk) {
        callState    = CallState::CALLING;
        callStartMs  = millis();
        lastGsmStat  = GsmCallStat::NONE;
        lastClccMs   = millis();
        lastChatId   = chatId;
        lastCmdLabel = alias.label.length() ? alias.label : alias.command;
        lastCmdTime  = getTimeStr();
        displayCalling(lastCmdLabel);
    } else {
        Serial.println("[handleCall] dial FAILED — not registered or modem error");
        bot->sendMessage(chatId, "Failed to dial. Check modem/SIM.", "");
    }
}

struct IP5306State {
    int  battLevel;   // 0=25%, 1=50%, 2=75%, 3=100% (IP5306 LED indicator bits[1:0])
    bool charging;    // VIN present and not yet full
    bool chargeFull;  // charge complete
    bool vinPresent;  // external power connected
};

IP5306State readIP5306() {
    IP5306State s = {0, false, false, false};

    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_READ0);
    Wire.endTransmission(false);
    Wire.requestFrom(IP5306_ADDR, 1);
    uint8_t r0 = Wire.available() ? Wire.read() : 0;
    s.vinPresent = (r0 >> 3) & 0x01;

    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_READ2);
    Wire.endTransmission(false);
    Wire.requestFrom(IP5306_ADDR, 1);
    uint8_t r2 = Wire.available() ? Wire.read() : 0;
    s.chargeFull = (r2 >> 3) & 0x01;
    s.charging   = s.vinPresent && !s.chargeFull;
    s.battLevel  = (~r2) & 0x03;  // 0=25%, 1=50%, 2=75%, 3=100%
    return s;
}

void sendStatus(const String& chatId) {
    int csq = modem.getSignalQuality();
    IP5306State ip5 = readIP5306();

    int pct = (ip5.battLevel + 1) * 25;
    String msg = "📶 Signal: " + String(csq) + "/31\n";
    msg += "🔋 Battery: " + String(pct) + "%";
    if (ip5.chargeFull)    msg += " ✅ full";
    else if (ip5.charging) msg += " ⚡";
    msg += "\n📞 Call: ";
    msg += (callState == CallState::IDLE) ? "idle" : "active";

    bot->sendMessage(chatId, msg, "");
}

static BalanceInfo parseBalance(const String& s) {
    BalanceInfo b;
    // Minutes: digits immediately before "hv" (e.g. "15hv")
    int hv = s.indexOf("hv");
    if (hv > 0) {
        int start = hv;
        while (start > 0 && isdigit(s[start - 1])) start--;
        if (start < hv) b.minutes = s.substring(start, hv);
    }
    // Next payment date: DD.MM only (5 chars after "poslug ") — year dropped to save display space
    int ps = s.indexOf("poslug ");
    if (ps >= 0 && ps + 12 <= (int)s.length())
        b.nextPay = s.substring(ps + 7, ps + 12);
    // SIM expiry date: 8 chars after "diye do "
    int dy = s.indexOf("diye do ");
    if (dy >= 0 && dy + 16 <= (int)s.length())
        b.expiry = s.substring(dy + 8, dy + 16);
    return b;
}

// TinyGSM's sendUSSD fails on SIM800 when +CUSD: arrives before OK (some firmware
// versions send them in that order), because waitResponse() consumes +CUSD: while
// looking for OK, leaving the second waitResponse() with nothing.
// Fix: send AT+CUSD raw and capture all output for up to 15 s, then parse manually.
static String ussdRaw(const String& code) {
    // Flush pending input
    while (modemSerial.available()) modemSerial.read();

    modem.sendAT(GF("+CUSD=1,\""), code.c_str(), GF("\""));

    String raw;
    unsigned long t = millis();
    while (millis() - t < 15000) {
        while (modemSerial.available()) {
            raw += (char)modemSerial.read();
            t = millis();
        }
        if (raw.indexOf("ERROR") >= 0) break;
        if (raw.indexOf("+CUSD:") >= 0 &&
            (raw.indexOf("OK") >= 0 || raw.indexOf("\n", raw.indexOf("+CUSD:") + 6) >= 0))
            break;
        delay(5);
    }
    Serial.println("[USSD raw] " + raw);

    int cusd = raw.indexOf("+CUSD:");
    if (cusd < 0) return "";
    int q1 = raw.indexOf('"', cusd);
    if (q1 < 0) return "";
    int q2 = raw.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    String msg = raw.substring(q1 + 1, q2);

    int dcsComma = raw.indexOf(',', q2);
    int dcs = (dcsComma >= 0) ? raw.substring(dcsComma + 1).toInt() : -1;

    // DCS=72 → UCS2 hex; decode to UTF-8
    if (dcs == 72 && msg.length() % 4 == 0) {
        String out;
        for (int i = 0; i < (int)msg.length(); i += 4) {
            uint16_t cp = (uint16_t)strtol(msg.substring(i, i + 4).c_str(), nullptr, 16);
            if      (cp < 0x80)  { out += (char)cp; }
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6));
                                   out += (char)(0x80 | (cp & 0x3F)); }
            else                 { out += (char)(0xE0 | (cp >> 12));
                                   out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                   out += (char)(0x80 | (cp & 0x3F)); }
        }
        return out;
    }
    return msg;
}

void sendUSSD(const String& chatId) {
    if (!cfg.ussdCode.length()) {
        bot->sendMessage(chatId, "USSD code not set. Configure via web admin.", "");
        return;
    }
    bool reg = modem.isNetworkConnected();
    Serial.printf("[USSD] registered=%d code=%s\n", (int)reg, cfg.ussdCode.c_str());
    if (!reg) {
        bot->sendMessage(chatId, "❌ GSM not registered. Check SIM/antenna.", "");
        return;
    }
    bot->sendMessage(chatId, "⏳ Sending USSD " + cfg.ussdCode + "...", "");
    String response = ussdRaw(cfg.ussdCode);
    bot->sendMessage(chatId, response.length() ? response : "No response from operator.", "");
    if (response.length() && callState == CallState::IDLE) {
        lastBalance = parseBalance(response);
        displayIdle("");
    }
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

    if (input == "/getMyId" || input == "getmyid") {
        bot->sendMessage(msg.chat_id,
            "Your Telegram ID: " + msg.from_id + "\n"
            "Name: " + msg.from_name, "");
        answerCb();
        return;
    }

    if (!isAnyAllowed(userId)) {
        Serial.printf("[auth] DENIED userId=%ld aliases=%d\n", userId, (int)cfg.aliases.size());
        answerCb("Unauthorized");
        if (!isCallback) bot->sendMessage(msg.chat_id, "Unauthorized.", "");
        return;
    }
    Serial.printf("[auth] OK userId=%ld\n", userId);

    if (input == "/status" || input == "status") {
        if (!isAdmin(userId)) { answerCb("Unauthorized"); return; }
        sendStatus(msg.chat_id);
        answerCb();
        return;
    }

    if (input == "/echo" || input == "echo") {
        if (!isAdmin(userId)) { answerCb("Unauthorized"); return; }
        sendUSSD(msg.chat_id);
        answerCb();
        return;
    }

    if (input == "/reboot" || input == "reboot") {
        if (!isAdmin(userId)) { answerCb("Unauthorized"); return; }
        answerCb("Rebooting...");
        bot->sendMessage(msg.chat_id, "🔄 Rebooting...", "");
        delay(500);
        ESP.restart();
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
        Serial.printf("[call] target=%s aliases=%d\n", target.c_str(), (int)cfg.aliases.size());
        for (const auto& alias : cfg.aliases) {
            if (alias.command == target) {
                if (!isAllowedForAlias(userId, alias)) {
                    Serial.printf("[call] alias found but userId=%ld not in allowedUsers\n", userId);
                    answerCb("Unauthorized");
                    if (!isCallback) bot->sendMessage(msg.chat_id, "Unauthorized for this command.", "");
                } else {
                    lastUserId  = userId;
                    lastCmdUser = msg.from_name;
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

// Render loop — runs on Core 0 display task only. All values pre-cached in snap.
// Layout:
//   Row 1 (y=0-10):  [GSM icon][GSM bars] · [operator] · [WiFi icon][WiFi bars][IP]
//   Row 2 (y=12-22): [date] [⏱min] [📅DD.MM] [💾DD.MM.YY] [Nokia battery]
//   Body  (y=24-85): status text + subtext
//   Footer (y=95):   last call
static void renderSnapshot(const DisplaySnapshot& s) {
    int16_t tbx, tby; uint16_t tbw, tbh;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        display.setFont(nullptr);
        display.setTextSize(1);
        display.setTextColor(GxEPD_BLACK);

        // Nokia-style 4-bar signal indicator (shared for GSM and WiFi)
        auto drawNokiaBars = [&](int ox, int filled) {
            for (int i = 0; i < 4; i++) {
                int bh   = (i + 1) * 2;
                int barX = ox + i * 4;
                int barY = 10 - bh;
                if (i < filled)
                    display.fillRect(barX, barY, 3, bh, GxEPD_BLACK);
                else
                    display.drawRect(barX, barY, 3, bh, GxEPD_RED);
            }
        };

        // WiFibroadcast-tower icon (5×5, top-left at (bx, 2))
        auto drawWiFiIcon = [&](int bx) {
            display.drawPixel(bx+2, 2, GxEPD_BLACK);
            display.drawPixel(bx+1, 3, GxEPD_BLACK);
            display.drawPixel(bx+3, 3, GxEPD_BLACK);
            display.drawPixel(bx,   4, GxEPD_BLACK);
            display.drawPixel(bx+4, 4, GxEPD_BLACK);
            display.drawPixel(bx+2, 5, GxEPD_BLACK);
            display.drawFastHLine(bx+1, 6, 3, GxEPD_BLACK);
        };

        // GSM  fan-arc icon (7×5, top-left at (bx, 2)) — same pixel art as before
        auto drawGSMIcon = [&](int bx) {
            display.drawPixel(bx,   2, GxEPD_BLACK);
            display.drawPixel(bx+3, 2, GxEPD_BLACK);
            display.drawPixel(bx+6, 2, GxEPD_BLACK);
            display.drawPixel(bx+1, 3, GxEPD_BLACK);
            display.drawPixel(bx+3, 3, GxEPD_BLACK);
            display.drawPixel(bx+5, 3, GxEPD_BLACK);
            display.drawFastHLine(bx+2, 4, 3, GxEPD_BLACK);
            display.drawPixel(bx+3, 5, GxEPD_BLACK);
            display.drawPixel(bx+3, 6, GxEPD_BLACK);
        };

        // Balance icons (5×5, oy = top y)
        auto drawClockIcon = [&](int ox, int oy) {
            display.drawFastHLine(ox+1, oy,   3, GxEPD_BLACK);
            display.drawPixel(ox,   oy+1, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+1, GxEPD_BLACK);
            display.drawPixel(ox,   oy+2, GxEPD_BLACK);
            display.drawPixel(ox+2, oy+2, GxEPD_BLACK);
            display.drawPixel(ox+3, oy+2, GxEPD_BLACK);
            display.drawPixel(ox,   oy+3, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+3, GxEPD_BLACK);
            display.drawFastHLine(ox+1, oy+4, 3, GxEPD_BLACK);
        };
        auto drawCalendarIcon = [&](int ox, int oy) {
            display.drawPixel(ox,   oy,   GxEPD_BLACK);
            display.drawPixel(ox+2, oy,   GxEPD_BLACK);
            display.drawPixel(ox+4, oy,   GxEPD_BLACK);
            display.drawFastHLine(ox, oy+1, 5, GxEPD_BLACK);
            display.drawPixel(ox,   oy+2, GxEPD_BLACK);
            display.drawPixel(ox+2, oy+2, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+2, GxEPD_BLACK);
            display.drawFastHLine(ox, oy+3, 5, GxEPD_BLACK);
            display.drawPixel(ox,   oy+4, GxEPD_BLACK);
            display.drawPixel(ox+2, oy+4, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+4, GxEPD_BLACK);
        };
        auto drawSIMIcon = [&](int ox, int oy) {
            display.drawFastHLine(ox+1, oy,   4, GxEPD_BLACK);
            display.drawPixel(ox,   oy+1, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+1, GxEPD_BLACK);
            display.drawFastHLine(ox+1, oy+2, 3, GxEPD_BLACK);
            display.drawPixel(ox,   oy+3, GxEPD_BLACK);
            display.drawPixel(ox+4, oy+3, GxEPD_BLACK);
            display.drawFastHLine(ox, oy+4, 5, GxEPD_BLACK);
        };

        if (s.apModeSnap) {
            // ── AP mode: SSID in row 1, IP in row 2 ───────────────────────
            display.setCursor(2, 9);
            display.print(s.footerRow1);  // "CallerBot-XXXXXXXX"
            display.drawFastHLine(0, 11, 212, GxEPD_BLACK);
            display.setCursor(2, 21);
            display.print(s.footerRow2);  // AP IP
            display.drawFastHLine(0, 23, 212, GxEPD_BLACK);
        } else {
            // ── Row 1: [GSM icon][GSM bars] · [operator] · [WiFi icon][WiFi bars][IP] ──
            drawGSMIcon(1);
            drawNokiaBars(6, s.gsmBars);

            // Nokia battery at original position (top-right, row 1)
            const int battX = 190, battY = 1;
            display.drawRect(battX,      battY,     16, 9, GxEPD_BLACK);
            display.fillRect(battX + 16, battY + 2,  3,  5, GxEPD_BLACK);
            for (int i = 0; i < s.battLevel + 1; i++)
                if (s.charging) {
                    display.fillRect(battX + 2 + i * 3, battY + 2, 2, 5, GxEPD_RED);
                } else {
                    display.fillRect(battX + 2 + i * 3, battY + 2, 2, 5, GxEPD_BLACK);
                }

            // if (s.charging) {
            //     const int lx = battX - 11, ly = battY + 1;
            //     display.drawFastHLine(lx + 4, ly,     2, GxEPD_RED);
            //     display.drawFastHLine(lx + 3, ly + 1, 2, GxEPD_RED);
            //     display.drawFastHLine(lx + 2, ly + 2, 2, GxEPD_RED);
            //     display.drawFastHLine(lx + 1, ly + 3, 5, GxEPD_RED);
            //     display.drawFastHLine(lx,     ly + 4, 5, GxEPD_RED);
            //     display.drawFastHLine(lx + 2, ly + 5, 2, GxEPD_RED);
            //     display.drawFastHLine(lx + 1, ly + 6, 2, GxEPD_RED);
            //     display.drawFastHLine(lx,     ly + 7, 2, GxEPD_RED);
            // }

            // IP: right-aligned, ending 12px before battery (x=188)
            display.getTextBounds(s.footerRow1.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
            int x_ip = battX - 2 - (int)tbw;  // ends at battX-2=188
            display.setCursor(x_ip, 3);
            display.print(s.footerRow1);

            // WiFi bars + icon just before IP (2px gap)
            int wifiBarsStart = x_ip - 17;
            drawNokiaBars(wifiBarsStart, s.wifiBars);
            drawWiFiIcon(wifiBarsStart - 5);

            // Operator centred between left block (x=23) and WiFi icon
            if (s.operatorName.length()) {
                int midLeft  = 24;
                int midRight = wifiBarsStart - 11;
                display.getTextBounds(s.operatorName.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
                int opX = midLeft + max(0, (midRight - midLeft - (int)tbw) / 2) - tbx;
                display.setCursor(max(midLeft, opX), 3);
                display.print(s.operatorName);
            }

            display.drawFastHLine(0, 11, 212, GxEPD_RED);

            // ── Row 2: [date] [balance fields] ────────────────────────────
            display.setCursor(2, 21);
            display.print(s.dateStr);  // "DD.MM.YY"

            // Balance items packed from x=52 (after 48px date + 2px gap)
            int bx = 52;
            if (s.balMinutes.length()) {
                drawClockIcon(bx, 15);
                display.setCursor(bx + 7, 21);
                display.print(s.balMinutes + "m");
                bx += 7 + (int)(s.balMinutes.length() + 1) * 6 + 1;
            }
            if (s.balNextPay.length()) {
                drawCalendarIcon(bx, 15);
                display.setCursor(bx + 7, 21);
                display.print(s.balNextPay);
                bx += 7 + (int)s.balNextPay.length() * 6 + 1;
            }
            if (s.balExpiry.length()) {
                drawSIMIcon(bx, 15);
                display.setCursor(bx + 7, 21);
                display.print(s.balExpiry);
            }

            // display.drawFastHLine(0, 23, 212, GxEPD_BLACK);
        }

        // ── Main status ────────────────────────────────────────────────────────
        display.setFont(&FreeSansBold12pt7b);
        display.setTextColor(s.useColor ? GxEPD_RED : GxEPD_BLACK);
        display.getTextBounds(s.statusText.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((212 - tbw) / 2 - tbx, 55);
        display.print(s.statusText);

        // ── Subtext ────────────────────────────────────────────────────────────
        if (s.subtextLine.length()) {
            display.setFont(&FreeSans9pt7b);
            display.setTextColor(GxEPD_BLACK);
            display.getTextBounds(s.subtextLine.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
            display.setCursor((212 - tbw) / 2 - tbx, 73);
            display.print(s.subtextLine);
        }

        // ── Footer: last call ──────────────────────────────────────────────────
        if (!s.apModeSnap) {
            display.setFont(nullptr);
            display.setTextSize(1);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(2, 95);
            display.print(s.footerRow2);
        }

    } while (display.nextPage());
}

// FreeRTOS display task pinned to Core 0.
// Core 1 (loop()) posts snapshots and returns immediately — never blocks on SPI.
static void displayTask(void*) {
    DisplaySnapshot local;
    while (true) {
        xSemaphoreTake(snapSignal, portMAX_DELAY);
        xSemaphoreTake(snapMutex, portMAX_DELAY);
        local = pendingSnap;
        xSemaphoreGive(snapMutex);
        renderSnapshot(local);
    }
}

// Capture current system state into a snapshot, signal display task, return immediately.
// I2C (IP5306) and AT (modem) reads happen here on Core 1 — safe and sequential.
// The 15-20 s SPI render runs on Core 0 without blocking loop().
void drawDisplay(const String& statusText, const String& subtextLine, bool useColor) {
    if (!displayOk) return;

    DisplaySnapshot snap;
    snap.statusText  = statusText;
    snap.subtextLine = toDisplayStr(subtextLine);
    snap.useColor    = useColor;
    snap.apModeSnap  = apMode;

    snap.wifiBars = 0;
    if (!apMode && WiFi.isConnected()) {
        int rssi = WiFi.RSSI();
        snap.wifiBars = rssi >= -50 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    }
    if (modemOk) {
        int csq       = modem.getSignalQuality();
        snap.gsmBars  = (csq == 99 || csq == 0) ? 0 : max(1, min(4, csq * 4 / 31 + 1));
        snap.operatorName = modem.getOperator();
    }
    IP5306State ip5 = readIP5306();
    snap.battLevel = ip5.battLevel;
    snap.charging  = ip5.charging;
    snap.dateStr   = getDateStr();

    if (apMode) {
        snap.footerRow1 = "CallerBot-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
        snap.footerRow2 = WiFi.softAPIP().toString();
    } else {
        snap.footerRow1 = WiFi.isConnected() ? WiFi.localIP().toString() : "offline";
        snap.footerRow2 = lastCmdLabel.length()
            ? toDisplayStr(lastCmdLabel) + " @" + lastCmdTime
                + (lastCmdUser.length() ? " " + toDisplayStr(lastCmdUser) : "")
            : "No calls yet";
    }

    snap.balMinutes = lastBalance.minutes;
    snap.balNextPay = lastBalance.nextPay;
    snap.balExpiry  = lastBalance.expiry;

    xSemaphoreTake(snapMutex, portMAX_DELAY);
    pendingSnap = snap;
    xSemaphoreGive(snapMutex);
    xSemaphoreGive(snapSignal);  // binary: stays given if task busy — "latest wins"
}

void displayIdle(const String& sub) {
    drawDisplay("IDLE", sub, false);
}

void displayCalling(const String& label) {
    drawDisplay("CALLING", label, true);
}

void initDisplay() {
    // Pre-check BUSY: if HIGH before init the UC8151 is wedged (e.g. from a
    // brownout mid-refresh). display.init() would block forever waiting for it
    // to clear → Interrupt WDT kills both cores. Skip display instead.
    pinMode(EPAPER_BUSY, INPUT);
    if (digitalRead(EPAPER_BUSY) == HIGH) {
        Serial.println("WARN: e-paper BUSY stuck HIGH — display skipped (power-cycle to recover)");
        return;
    }

    epaperSPI.begin(EPAPER_CLK, -1, EPAPER_MOSI, EPAPER_CS);
    display.epd2.selectSPI(epaperSPI, SPISettings(1000000, MSBFIRST, SPI_MODE0));
    // Use 50 ms busy-poll interval instead of GxEPD2's default 1 ms.
    // 1 ms → ~30 000 vTaskDelay calls per 30 s refresh → inter-core FreeRTOS
    // spinlock contention between display task (Core 0) and loop (Core 1) → INT WDT.
    display.epd2.setBusyCallback([](const void*){ delay(50); }, nullptr);
    display.init(0, true, 200, false);
    display.setRotation(cfg.displayFlip ? 3 : 1);
    displayOk = true;
    drawDisplay("Starting...", "", false);
    Serial.println("Display OK.");
}

// ─── Hardware init ────────────────────────────────────────────────────────────

void initPowerManagement() {
    // I2C bus recovery: IP5306 can clock-stretch-hold SDA low after a dirty
    // (brownout) reset, making Wire.begin() fail with "could not acquire lock".
    // Manually clock SCL up to 9 times to let the slave release SDA.
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
        digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
        digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    }
    pinMode(I2C_SCL, INPUT_PULLUP);

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
            drawDisplay("MODEM FAIL", "Power cycle device", true);
            while (true) delay(1000);
        }
    }
    modemOk = true;
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

// Non-fatal modem recovery for use from loop() (unlike initModem which halts on failure).
// Called when watchdog detects modem is unresponsive (e.g. SIM800L lost power while
// ESP32 stayed alive via USB, then VIN was reconnected without an ESP32 reset).
void recoverModem() {
    Serial.println("Modem watchdog: recovery start");
    modemOk = false;
    modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(100);
    if (!modem.init()) {
        Serial.println("Modem watchdog: soft init failed — power cycling");
        powerCycleModem();
        if (!modem.init()) {
            Serial.println("ERROR: modem recovery failed — will retry next cycle");
            return;
        }
    }
    modemOk = true;
    for (int i = 0; i < 15 && !modem.isNetworkConnected(); i++)
        delay(1000);
    Serial.printf("Modem recovery OK, CSQ=%d\n", modem.getSignalQuality());
    displayIdle("GSM restored");
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
    // IP5306 regulates VOUT; ESP32 brownout detector fires on USB→battery transition
    // (brief voltage dip during switchover). Disable it — IP5306 handles undervoltage.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32 Caller Bot ===");

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS mount failed — halting");
        while (true) delay(1000);
    }
    loadConfig();

    initPowerManagement(); // enable IP5306 boost FIRST — display refresh draws peak
                           // current and browns out on battery if boost not always-on

    snapMutex  = xSemaphoreCreateMutex();
    snapSignal = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);

    initDisplay();         // show "Starting..." early — posts first snapshot to task
    {   // seed battery state so first keep-alive check doesn't trigger false display refresh
        IP5306State ip5 = readIP5306();
        lastBattLevel = ip5.battLevel;
        lastCharging  = ip5.charging;
    }
    initModem();
    initWiFi();
    initNTP();
    bootTimeUnix = time(nullptr);  // used to skip stale Telegram callbacks after reboot

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

    // Fetch balance at boot so display shows it immediately on first idle render
    if (!apMode && modemOk && cfg.ussdCode.length() && modem.isNetworkConnected()) {
        Serial.println("Boot: fetching USSD balance...");
        String resp = ussdRaw(cfg.ussdCode);
        if (resp.length()) lastBalance = parseBalance(resp);
    }

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

    // Modem watchdog: AT ping every 30 s when idle.
    // Catches silent SIM800L crash (e.g. VIN removed → SIM800L power lost while ESP32
    // survived via USB → VIN reconnected but ESP32 never rebooted → modem unresponsive).
    if (callState == CallState::IDLE) {
        static unsigned long lastModemCheckMs = 0;
        static int modemFailCount = 0;
        if (millis() - lastModemCheckMs >= 30000UL) {
            lastModemCheckMs = millis();
            modem.sendAT(GF(""));
            if (modem.waitResponse(1000) != 1) {
                Serial.printf("WARN: modem AT ping failed (%d/2)\n", ++modemFailCount);
                if (modemFailCount >= 2) {
                    modemFailCount = 0;
                    recoverModem();
                }
            } else {
                modemFailCount = 0;
            }
        }
    }

    // IP5306 keep-alive every 20 s: re-write SYS_CTL0 to prevent auto power-off on battery.
    // IP5306 shuts down boost after ~32 s of light load when VIN is absent; periodic write resets
    // that timer even if the load dips momentarily (e.g. during WiFi TX idle or sleep).
    if (millis() - lastBattCheckMs >= 20000UL) {
        lastBattCheckMs = millis();
        Wire.beginTransmission(IP5306_ADDR);
        Wire.write(IP5306_REG_SYS_CTL0);
        Wire.write(0x37);
        Wire.endTransmission();
        // Battery level change → refresh display (charging-state change does NOT refresh —
        // avoids a 15-20 s e-paper cycle triggered right when VIN is removed, which risks
        // a brownout mid-SPI leaving BUSY stuck HIGH and the render loop spinning forever).
        if (callState == CallState::IDLE) {
            IP5306State ip5 = readIP5306();
            lastCharging = ip5.charging;
            if (ip5.battLevel != lastBattLevel) {
                lastBattLevel = ip5.battLevel;
                displayIdle(ip5.battLevel == 0 ? "Charge now! 25%" : "");
            }
        }
    }

    // Telegram polling (STA mode only)
    if (!apMode && bot && WiFi.isConnected() && millis() - lastPollMs >= BOT_POLL_MS) {
        lastPollMs = millis();
        int numMsg = bot->getUpdates(bot->last_message_received + 1);
        if (numMsg > 0) Serial.printf("Bot: %d message(s)\n", numMsg);
        for (int i = 0; i < numMsg; i++) {
            // Skip messages that predate this boot — stale callbacks replayed after
            // an unexpected reboot (e.g. brownout) would otherwise be processed twice.
            long msgDate = bot->messages[i].date.toInt();
            if (bootTimeUnix > 0 && msgDate > 0 && msgDate < bootTimeUnix - 60) {
                Serial.printf("  skip stale [%s] date=%ld boot=%ld\n",
                    bot->messages[i].text.c_str(), msgDate, bootTimeUnix);
                if (bot->messages[i].type == "callback_query")
                    bot->answerCallbackQuery(bot->messages[i].query_id, "");
                continue;
            }
            Serial.printf("  [%s] from:%s text:%s\n",
                bot->messages[i].type.c_str(),
                bot->messages[i].from_id.c_str(),
                bot->messages[i].text.c_str());
            handleMessage(bot->messages[i]);
        }
    }
}
