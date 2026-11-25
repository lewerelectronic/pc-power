/*
 * PC 원격 전원 스위치 v2 — ESP32-C3  (PC Remote Power v2)
 * ================================================================
 * - 제어: 텔레그램 봇 (하단 고정 버튼 + 끄기/강제 종료 확인 버튼), 한국어/영어
 * - 설정: PC USB에 꽂힌 상태에서 크롬/엣지 설정 페이지(Web Serial)로만 가능
 *         (설정용 와이파이 AP 없음 → USB에 물리적으로 꽂혀 있어야만 설정 변경)
 * - 주인 등록: 설정 페이지가 보여주는 QR(텔레그램 딥링크 + 10분짜리 등록 코드)
 * - PC 켜짐 감지: USB 호스트의 SOF 패킷 유무 (추가 배선 없음)
 * - 오래된 명령 무시(보드 부팅 이전 또는 2분 이상 지난 명령), 정전 후 재시작 알림
 * - 버튼을 누른 뒤 결과 확인 전까지 켜기/끄기 재입력 거절 (연타로 부팅 중 꺼지는 것 방지)
 * - 와이파이 또는 텔레그램이 30분 넘게 안 되면 스스로 재시작
 * - 인증서: 코어 내장 Mozilla CA 묶음 → 텔레그램 인증서가 바뀌어도 계속 동작
 * - 폰 터치패드 마우스: PC 프로그램(pc-agent)이 USB로 자기 주소(IP·포트·토큰)를 알려 주면 RAM에만 보관,
 *   🖱️ 마우스 버튼을 누르면 그 링크를 답장 (PC가 꺼지거나 10분 넘게 소식 없으면 지움)
 * - 펌웨어 업데이트: 기본은 설정 페이지에서 USB로.
 *   개발자 모드(설정 페이지에서 USB로만 켤 수 있음)를 켜면 원격 업데이트도 가능:
 *     텔레그램 /update → 확인 버튼 → GitHub Pages의 ota.json·앱 이미지를 받아 MD5 확인 후 설치
 *     자동 업데이트(선택): PC가 켜졌다 꺼지고 10분 동안 PC도 꺼져 있고 명령도 없으면 확인
 *     새 펌웨어는 5분 안에 텔레그램 연결에 성공해야 확정, 못 하면(또는 재부팅 반복) 이전 버전으로 롤백
 *

 * 하드웨어: ESP32-C3 (SuperMini / XIAO ESP32C3 등, 칩 USB 직결 보드)
 *   GPIO4 ─[330Ω]─ PC817 1번, GND ─ PC817 2번, GPIO4 ─[10kΩ]─ GND (부팅 순간 오작동 방지용 풀다운)
 *   PC817 4번 ─ PWR_SW(+), 3번 ─ PWR_SW(−)  (케이스 버튼과 병렬)
 *   C3를 쓰는 이유: USB가 시리얼/JTAG 고정 기능이라 키보드(HID)로 위장할 수 없음
 *
 * 빌드: PlatformIO (pioarduino, arduino-esp32 4.x) — firmware/platformio.ini
 *
 * USB 설정 프로토콜 (115200, 줄 단위)
 *   요청: {"id":1,"cmd":"info|scan|wifi|token|pair|unpair|set|reboot|reset|agent", ...}
 *         set: {"txLow":bool} / {"lang":"ko"|"en"} / {"dev":bool} / {"autoUpd":bool}
 *         agent: {"ip":"192.168.0.10","port":8765,"token":"..."} — PC 프로그램이 주기적으로 보냄
 *                (설정 페이지 사용으로 치지 않음 → 자동 업데이트·통신 재시작 보류 안 함)
 *   응답: @{"id":1,"ok":true,...}   이벤트: @{"evt":"hello|wifi|paired|pair_expired",...}
 *   '@'로 시작하지 않는 줄은 로그 (영어)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdarg.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include "soc/soc_caps.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 4
#error "arduino-esp32 4.x is required (use the pioarduino stable platform)"
#endif
#if !SOC_USB_SERIAL_JTAG_SUPPORTED
#error "An ESP32-C3 with USB Serial/JTAG is required for PC power detection"
#endif
#include "HWCDC.h"

// ===================== 고정 설정 =====================
static const char* FW_VERSION = "2.3.0";
static const char* OTA_BASE   = "https://lewerelectronic.github.io/pc-power/firmware/";  // ota.json + 앱 이미지

constexpr int PIN_PWR_SW = 4;              // → 330Ω → PC817 1번 (XIAO는 D2)
constexpr int PIN_LED    = 8;              // SuperMini 내장 LED (LOW = 켜짐). XIAO는 연결된 것 없음

constexpr uint32_t PRESS_MS       = 500;   // 짧게 누름 = 켜기 / 정상 종료
constexpr uint32_t FORCE_MS       = 6000;  // 길게 누름 = 강제 종료
constexpr uint32_t POLL_MS        = 1500;  // 텔레그램 확인 주기 (주인 있음)
constexpr uint32_t POLL_IDLE_MS   = 5000;  // 주인 없을 때
constexpr uint32_t ON_CHECK_MS    = 40000;
constexpr uint32_t OFF_CHECK_MS   = 90000;
constexpr uint32_t FORCE_CHECK_MS = 10000;
constexpr uint32_t CONFIRM_TTL_MS = 60000; // 확인 버튼 유효 시간
constexpr uint32_t PAIR_TTL_MS    = 10UL * 60 * 1000;
constexpr uint32_t WIFI_REPORT_MS = 20000;
constexpr uint32_t WIFI_RETRY_MS  = 60000;
constexpr uint32_t STUCK_MS       = 30UL * 60 * 1000;  // 이 시간 동안 통신이 안 되면 재시작
constexpr uint32_t SERIAL_QUIET_MS = 10UL * 60 * 1000; // 설정 페이지 사용 중엔 재시작 보류
constexpr long     MAX_AGE_S      = 120;   // 이보다 오래된 명령은 실행 안 함
constexpr uint32_t OTA_VERIFY_MS  = 5UL * 60 * 1000;   // 새 펌웨어가 이 안에 텔레그램 연결 못 하면 롤백
constexpr uint32_t AUTO_IDLE_MS   = 10UL * 60 * 1000;  // 자동 업데이트: PC 꺼짐·명령 없음 유지 시간
constexpr uint32_t AUTO_GAP_MS    = 6UL * 3600 * 1000; // 자동 업데이트 확인 최소 간격
constexpr uint32_t PC_SAMPLE_MS   = 30000;             // 자동 업데이트용 PC 상태 확인 주기
constexpr uint32_t AGENT_TTL_MS   = 10UL * 60 * 1000;  // PC 프로그램 주소 유효 시간 (프로그램은 3분마다 다시 보냄)

// 텔레그램 버튼 (언어별)
static const char* KB_MAIN_KO =
    R"({"keyboard":[[{"text":"🟢 켜기"},{"text":"⚪ 끄기"}],[{"text":"📊 상태"},{"text":"⛔ 강제 종료"}],[{"text":"🖱️ 마우스"}]],)"
    R"("resize_keyboard":true,"is_persistent":true})";
static const char* KB_MAIN_EN =
    R"({"keyboard":[[{"text":"🟢 Power on"},{"text":"⚪ Shut down"}],[{"text":"📊 Status"},{"text":"⛔ Force off"}],[{"text":"🖱️ Mouse"}]],)"
    R"("resize_keyboard":true,"is_persistent":true})";
static const char* KB_OFF_KO =
    R"({"inline_keyboard":[[{"text":"끄기","callback_data":"off"},{"text":"취소","callback_data":"cancel"}]]})";
static const char* KB_OFF_EN =
    R"({"inline_keyboard":[[{"text":"Shut down","callback_data":"off"},{"text":"Cancel","callback_data":"cancel"}]]})";
static const char* KB_FORCE_KO =
    R"({"inline_keyboard":[[{"text":"강제 종료","callback_data":"force"},{"text":"취소","callback_data":"cancel"}]]})";
static const char* KB_FORCE_EN =
    R"({"inline_keyboard":[[{"text":"Force off","callback_data":"force"},{"text":"Cancel","callback_data":"cancel"}]]})";
static const char* KB_UPD_KO =
    R"({"inline_keyboard":[[{"text":"업데이트","callback_data":"upd"},{"text":"취소","callback_data":"cancel"}]]})";
static const char* KB_UPD_EN =
    R"({"inline_keyboard":[[{"text":"Update","callback_data":"upd"},{"text":"Cancel","callback_data":"cancel"}]]})";

// ===================== 상태 =====================
enum Act { A_NONE, A_ON, A_OFF, A_FORCE, A_STATUS, A_HELP, A_UPDATE, A_MOUSE };

Preferences prefs;
NetworkClientSecure tls;
HTTPClient http;

struct Config {
  String ssid, pass, token, bot, owner, ownerName, lang;
  bool txLow = false;
  bool dev = false;              // 개발자 모드: 원격 펌웨어 업데이트 허용
  bool autoUpd = true;           // 개발자 모드일 때 자동 업데이트
} cfg;

time_t bootEpoch = 0;          // 보드가 켜진 시각 (NTP 맞춘 뒤 계산)
bool bootNotified = false;
long long updateOffset = 0;
int staleCount = 0;

int tgLastCode = 0;            // 마지막 텔레그램 응답 코드 (200 = 정상)
bool tgBlocked = false;        // 토큰 무효 → 토큰 바뀔 때까지 중지
uint32_t tgBackoffUntil = 0;
uint32_t tgRetryAfterS = 0;   // 429 응답의 retry_after (초)
uint32_t tgOkAt = 0;          // 마지막으로 텔레그램이 200을 준 시각
uint32_t wifiOkAt = 0;        // 마지막으로 와이파이가 연결돼 있던 시각
uint32_t serialAt = 0;        // 마지막 설정 페이지 요청 시각
constexpr uint32_t RESTART_COMM = 0xC0AA5E01;
constexpr uint32_t RESTART_OTA  = 0xC0AA5E02;
RTC_NOINIT_ATTR uint32_t restartWhy;   // 소프트웨어 재시작 이유 (재시작해도 유지)
uint32_t bootWhy = 0;                  // 이번 부팅에서 읽어 둔 restartWhy

// 원격 업데이트 (개발자 모드)
struct OtaInfo { String version, url, md5; size_t size = 0; } otaOffer;  // /update 확인 대기 중인 버전
bool otaVerifying = false;     // 방금 설치된 새 펌웨어: 텔레그램 연결 확인 전
uint32_t otaVerifyUntil = 0;
String otaNote;                // 업데이트 결과 알림 (부팅 알림 대신 보냄)
uint32_t lastCmdAt = 0;        // 주인이 마지막으로 명령한 시각
bool pcWasOn = false;          // 자동 업데이트: PC가 켜졌다 꺼진 걸 감지
bool autoArmed = false;
uint32_t pcOffSince = 0;
uint32_t lastAutoCheck = 0;

String pairCode;
uint32_t pairUntil = 0;
int pairFails = 0;

// 폰 터치패드: PC 프로그램이 알려 준 주소 (RAM에만, 저장 안 함)
struct Agent { String ip, token; uint16_t port = 0; uint32_t at = 0; } agent;

struct Pending { bool active; uint32_t due; bool expectOn; } pending = {false, 0, false};
struct Confirm { bool active; long long msgId; uint32_t until; char action; } confirmState = {false, 0, 0, 0};

bool wifiWasConnected = false;
bool wifiReport = false;
uint32_t wifiReportStart = 0;
uint32_t lastWifiBegin = 0;

// ===================== 유틸 =====================
bool en() { return cfg.lang == "en"; }
const char* L(const char* ko, const char* enText) { return en() ? enText : ko; }
const char* kbMain() { return en() ? KB_MAIN_EN : KB_MAIN_KO; }

void logf(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

void emit(JsonDocument& d) {            // 설정 페이지로 보내는 응답/이벤트
  Serial.print('@');
  serializeJson(d, Serial);
  Serial.println();
}

String i64str(long long v) {
  char b[24];
  snprintf(b, sizeof b, "%lld", v);
  return String(b);
}

bool alive(uint32_t until) { return (int32_t)(until - millis()) > 0; }

void led(bool on) { digitalWrite(PIN_LED, on ? LOW : HIGH); }

// USB SOF 패킷이 들어오는지 = PC(USB 호스트)가 켜져 있는지
bool pcIsOn() {
  int hits = 0;
  for (int i = 0; i < 5; i++) {
    if (HWCDC::isPlugged()) hits++;
    delay(20);
  }
  return hits >= 3;
}

void pressButton(uint32_t ms) {
  logf("[PWR] pressing button for %lu ms", (unsigned long)ms);
  digitalWrite(PIN_PWR_SW, HIGH);
  delay(ms);
  digitalWrite(PIN_PWR_SW, LOW);
}

uint32_t uptimeSec() { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return L("전원이 새로 들어왔어요 (정전 복구 등)", "power was restored (e.g. after an outage)");
    case ESP_RST_BROWNOUT: return L("USB 전압이 잠깐 떨어졌어요", "the USB voltage dipped briefly");
    case ESP_RST_SW:
      if (bootWhy == RESTART_COMM)
        return L("통신이 30분 넘게 안 돼서 스스로 재시작했어요", "it restarted itself after 30+ minutes without a connection");
      if (bootWhy == RESTART_OTA)
        return L("펌웨어 업데이트로 재시작했어요", "it restarted for a firmware update");
      return L("설정 변경으로 재시작했어요", "it restarted after a settings change");
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return L("오류로 자동 재시작했어요", "it restarted automatically after an error");
    default:               return L("기타 이유로 재시작했어요", "it restarted for another reason");
  }
}

// ===================== 설정 저장 =====================
void loadConfig() {
  cfg.ssid      = prefs.getString("ssid", "");
  cfg.pass      = prefs.getString("pass", "");
  cfg.token     = prefs.getString("token", "");
  cfg.bot       = prefs.getString("bot", "");
  cfg.owner     = prefs.getString("owner", "");
  cfg.ownerName = prefs.getString("ownerName", "");
  cfg.lang      = prefs.getString("lang", "ko");
  cfg.txLow     = prefs.getBool("txLow", false);
  cfg.dev       = prefs.getBool("dev", false);
  cfg.autoUpd   = prefs.getBool("autoUpd", true);
}

void applyTxPower() {
  WiFi.setTxPower(cfg.txLow ? WIFI_POWER_8_5dBm : WIFI_POWER_19_5dBm);
}

// ===================== 와이파이 =====================
void wifiBegin() {
  if (cfg.ssid.isEmpty()) return;
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  applyTxPower();
  lastWifiBegin = millis();
}

const char* wifiFailReason() {
  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL:  return "no_ap";
    case WL_CONNECT_FAILED: return "auth";
    default:                return "timeout";
  }
}

void checkWifi() {
  bool conn = WiFi.isConnected();
  if (conn && !wifiWasConnected) {
    logf("[WiFi] connected: %s  IP %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else if (!conn && wifiWasConnected) {
    logf("[WiFi] disconnected, retrying");
  }
  wifiWasConnected = conn;

  if (wifiReport) {
    if (conn) {
      wifiReport = false;
      JsonDocument e;
      e["evt"] = "wifi";
      e["connected"] = true;
      e["ssid"] = WiFi.SSID();
      e["ip"] = WiFi.localIP().toString();
      emit(e);
    } else if (millis() - wifiReportStart > WIFI_REPORT_MS) {
      wifiReport = false;
      JsonDocument e;
      e["evt"] = "wifi";
      e["connected"] = false;
      e["reason"] = wifiFailReason();
      emit(e);
    }
  }
  if (!conn && cfg.ssid.length() && millis() - lastWifiBegin > WIFI_RETRY_MS) wifiBegin();
}

void syncClock() {
  if (bootEpoch) return;
  time_t now = time(nullptr);
  if (now > 1700000000) {
    bootEpoch = now - (time_t)uptimeSec();
    logf("[Time] clock synced");
  }
}

// ===================== 텔레그램 =====================
// 반환: HTTP 코드 (200 정상), 음수 = 연결/파싱 실패
int tgCall(const char* method, JsonDocument& body, JsonDocument* out = nullptr, JsonDocument* filter = nullptr) {
  if (cfg.token.isEmpty() || !WiFi.isConnected()) return -10;
  String url = "https://api.telegram.org/bot" + cfg.token + "/" + method;
  String payload;
  serializeJson(body, payload);
  if (!http.begin(tls, url)) return -11;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code > 0) {
    String resp = http.getString();       // 다음 요청 재사용을 위해 항상 끝까지 읽음
    if (code == 429) {
      JsonDocument f, e;
      f["parameters"]["retry_after"] = true;
      if (!deserializeJson(e, resp, DeserializationOption::Filter(f)))
        tgRetryAfterS = e["parameters"]["retry_after"] | 0;
    }
    if (out) {
      DeserializationError err = filter
          ? deserializeJson(*out, resp, DeserializationOption::Filter(*filter))
          : deserializeJson(*out, resp);
      if (err) code = -12;
    }
  }
  http.end();
  return code;
}

void recordTg(int code) {
  if (code != tgLastCode) logf("[Telegram] response code %d", code);
  tgLastCode = code;
  if (code == 200) tgOkAt = millis();
  if (code == 401 || code == 404) {
    tgBlocked = true;                     // 토큰 무효 → 설정 페이지에서 토큰 다시 넣을 때까지 중지
    logf("[Telegram] token rejected. Enter the token again on the settings page");
  } else if (code == 409) {
    tgBackoffUntil = millis() + 30000;    // 다른 곳에서 같은 봇을 쓰는 중
  } else if (code == 429) {
    uint32_t s = tgRetryAfterS < 10 ? 10 : (tgRetryAfterS > 300 ? 300 : tgRetryAfterS);
    tgRetryAfterS = 0;
    tgBackoffUntil = millis() + s * 1000UL;  // 텔레그램이 알려준 대기 시간만큼 쉬기
  }
}

long long tgSend(const String& chat, const String& text, const char* markup = nullptr, bool noPreview = false) {
  JsonDocument body;
  body["chat_id"] = chat;
  body["text"] = text;
  if (markup) body["reply_markup"] = serialized(markup);
  if (noPreview) body["link_preview_options"]["is_disabled"] = true;
  JsonDocument filter;
  filter["ok"] = true;
  filter["result"]["message_id"] = true;
  JsonDocument out;
  int code = tgCall("sendMessage", body, &out, &filter);
  if (code == 200 && out["ok"].as<bool>()) return out["result"]["message_id"].as<long long>();
  logf("[Telegram] sendMessage failed (%d)", code);
  return 0;
}

void send(const String& text, const char* markup = nullptr) {
  if (cfg.owner.length()) tgSend(cfg.owner, text, markup);
}

void tgAnswerCallback(const String& id, const char* text) {
  JsonDocument body;
  body["callback_query_id"] = id;
  if (text && *text) body["text"] = text;
  tgCall("answerCallbackQuery", body);
}

void tgEditText(long long msgId, const String& text) {
  JsonDocument body;
  body["chat_id"] = cfg.owner;
  body["message_id"] = msgId;
  body["text"] = text;                    // reply_markup 생략 → 확인 버튼 제거
  tgCall("editMessageText", body);
}

void tgSetCommands() {                    // / 메뉴에 보이는 명령 설명 (현재 언어)
  JsonDocument b;
  JsonArray cmds = b["commands"].to<JsonArray>();
  const char* list[][3] = {{"on", "PC 켜기", "Turn the PC on"},
                           {"off", "PC 끄기 (확인 후)", "Shut down (with confirmation)"},
                           {"status", "상태 보기", "Show status"},
                           {"force", "강제 종료 (확인 후)", "Force off (with confirmation)"},
                           {"mouse", "폰 터치패드 링크", "Phone touchpad link"},
                           {"help", "도움말", "Help"}};
  for (auto& c : list) {
    JsonObject o = cmds.add<JsonObject>();
    o["command"] = c[0];
    o["description"] = en() ? c[2] : c[1];
  }
  if (cfg.dev) {
    JsonObject o = cmds.add<JsonObject>();
    o["command"] = "update";
    o["description"] = L("펌웨어 업데이트 (개발자 모드)", "Firmware update (developer mode)");
  }
  tgCall("setMyCommands", b);
}

void tgSetup() {                          // 토큰 확인 직후 1회
  JsonDocument b1;
  b1["drop_pending_updates"] = true;
  tgCall("deleteWebhook", b1);
  tgSetCommands();
}

String helpText() {
  String s = L("아래 버튼으로 PC를 켜고 끌 수 있어요.\n"
               "🟢 켜기 · ⚪ 끄기(확인 후 정상 종료) · 📊 상태 · ⛔ 강제 종료(확인 후 6초 누름)\n"
               "🖱️ 마우스: 폰을 터치패드로 (PC 프로그램 필요, 같은 와이파이에서)",
               "Use the buttons below to control your PC.\n"
               "🟢 Power on · ⚪ Shut down (after confirming) · 📊 Status · ⛔ Force off (after confirming, holds 6 s)\n"
               "🖱️ Mouse: use your phone as a touchpad (needs the PC app, same Wi-Fi)");
  if (cfg.dev) s += L("\n🛠 개발자 모드: /update 로 펌웨어 업데이트", "\n🛠 Developer mode: /update to update the firmware");
  return s;
}

String statusText() {
  uint32_t up = uptimeSec();
  String s = "PC: ";
  s += pcIsOn() ? L("켜짐 ✅", "on ✅") : L("꺼짐 또는 절전 ⏻", "off or asleep ⏻");
  s += String(L("\n와이파이: ", "\nWi-Fi: ")) + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)";
  if (en()) {
    s += "\nModule uptime: " + String(up / 86400) + "d " + String(up / 3600 % 24) + "h " + String(up / 60 % 60) + "m";
  } else {
    s += "\n보드 가동: " + String(up / 86400) + "일 " + String(up / 3600 % 24) + "시간 " + String(up / 60 % 60) + "분";
  }
  s += String(L("\n펌웨어: v", "\nFirmware: v")) + FW_VERSION;
  if (cfg.dev) {
    s += cfg.autoUpd ? L("\n개발자 모드: 켜짐 (자동 업데이트 켜짐)", "\nDeveloper mode: on (auto-update on)")
                     : L("\n개발자 모드: 켜짐 (자동 업데이트 꺼짐)", "\nDeveloper mode: on (auto-update off)");
  }
  return s;
}

void schedule(uint32_t ms, bool expectOn) {
  pending.active = true;
  pending.due = millis() + ms;
  pending.expectOn = expectOn;
}

void reportPending() {
  pending.active = false;
  bool on = pcIsOn();
  if (on == pending.expectOn) {
    send(on ? L("PC가 켜진 걸 확인했어요 ✅", "Confirmed: the PC is on ✅")
            : L("PC가 꺼진 걸 확인했어요 ⏻", "Confirmed: the PC is off ⏻"));
  } else if (on) {
    send(L("아직 켜져 있어요. 윈도우가 종료를 막고 있을 수 있어요 → 필요하면 ⛔ 강제 종료",
           "Still on. Windows may be blocking shutdown → use ⛔ Force off if needed"));
  } else {
    send(L("아직 켜진 신호가 없어요. 부팅이 느리거나 배선·BIOS 확인이 필요할 수 있어요 (📊 상태로 다시 확인)",
           "No power-on signal yet. Boot may be slow, or check the wiring and BIOS (tap 📊 Status to check again)"));
  }
}

bool pairActive() { return pairCode.length() && alive(pairUntil); }

void endPairing(bool expired) {
  pairCode = "";
  pairUntil = 0;
  pairFails = 0;
  if (expired) {
    JsonDocument e;
    e["evt"] = "pair_expired";
    emit(e);
  }
}

void setOwner(const String& chat, const String& name) {
  cfg.owner = chat;
  cfg.ownerName = name;
  prefs.putString("owner", chat);
  prefs.putString("ownerName", name);
  endPairing(false);
  confirmState.active = false;
  logf("[Pairing] owner registered");
  send(String(L("등록됐어요! 이제 이 채팅에서만 PC를 켜고 끌 수 있어요.\n\n",
                "You're registered! Only this chat can now turn the PC on and off.\n\n")) + helpText(),
       kbMain());
  JsonDocument e;
  e["evt"] = "paired";
  e["name"] = name;
  emit(e);
}

Act parseAct(String t) {
  if (t.startsWith("/")) {
    int at = t.indexOf('@');
    if (at > 0) t = t.substring(0, at);
  }
  // 두 언어 버튼을 모두 받음 (언어를 바꾼 직후 예전 버튼을 눌러도 동작)
  if (t == "/on" || t.indexOf("켜기") >= 0 || t.indexOf("Power on") >= 0) return A_ON;
  if (t == "/force" || t.indexOf("강제") >= 0 || t.indexOf("Force") >= 0) return A_FORCE;
  if (t == "/off" || t.indexOf("끄기") >= 0 || t.indexOf("Shut down") >= 0) return A_OFF;
  if (t == "/status" || t.indexOf("상태") >= 0 || t.indexOf("Status") >= 0) return A_STATUS;
  if (t == "/help" || t.indexOf("도움") >= 0 || t.indexOf("Help") >= 0) return A_HELP;
  if (t == "/mouse" || t.indexOf("마우스") >= 0 || t.indexOf("Mouse") >= 0) return A_MOUSE;
  if (t == "/update" || t == "업데이트" || t == "Update") return A_UPDATE;
  return A_NONE;
}

bool freshCommand(long msgDate) {
  time_t now = time(nullptr);
  return bootEpoch && msgDate >= (long)bootEpoch && (long)(now - msgDate) <= MAX_AGE_S;
}

void askConfirm(char action) {
  const bool isForce = action == 'f';
  const char* text = isForce
      ? L("정말 강제 종료할까요? 저장 안 한 작업은 사라져요. (1분 안에 눌러 주세요)",
          "Force the PC off? Unsaved work will be lost. (Confirm within 1 minute)")
      : L("PC를 끌까요? (1분 안에 눌러 주세요)", "Shut down the PC? (Confirm within 1 minute)");
  const char* kb = isForce ? (en() ? KB_FORCE_EN : KB_FORCE_KO) : (en() ? KB_OFF_EN : KB_OFF_KO);
  long long id = tgSend(cfg.owner, text, kb);
  if (id) confirmState = {true, id, millis() + CONFIRM_TTL_MS, action};
}

// ===================== 원격 업데이트 (개발자 모드) =====================
// 새 앱 이미지가 자기 검사(텔레그램 연결)를 끝낼 때까지 롤백 대기 상태로 둠 (코어 기본은 즉시 확정)
extern "C" bool verifyRollbackLater() { return true; }

int verCmp(const String& a, const String& b) {   // "2.10.0" > "2.9.3"
  int ia = 0, ib = 0;
  for (int i = 0; i < 3; i++) {
    long va = ia < (int)a.length() ? a.substring(ia).toInt() : 0;
    long vb = ib < (int)b.length() ? b.substring(ib).toInt() : 0;
    if (va != vb) return va < vb ? -1 : 1;
    int na = a.indexOf('.', ia), nb = b.indexOf('.', ib);
    ia = na < 0 ? a.length() : na + 1;
    ib = nb < 0 ? b.length() : nb + 1;
  }
  return 0;
}

// HTTPClient는 연결이 살아 있으면 주소가 달라도 그대로 재사용함 → 다른 서버로 갈 땐 끊고 감
void netSwitch() {
  http.end();
  tls.stop();
}

bool otaFetchInfo(OtaInfo& info) {
  if (!WiFi.isConnected()) return false;
  netSwitch();
  bool ok = false;
  int code = -11;
  if (http.begin(tls, String(OTA_BASE) + "ota.json")) {
    code = http.GET();
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, http.getString())) {
        info.version = d["version"] | "";
        info.md5 = d["md5"] | "";
        info.size = d["size"] | 0;
        info.url = String(OTA_BASE) + (d["file"] | "pcpower-app.bin");
        ok = info.version.length() && info.md5.length() == 32 && info.size > 0;
      }
    } else if (code > 0) {
      http.getString();
    }
  }
  netSwitch();
  logf("[OTA] check: code %d, latest %s", code, ok ? info.version.c_str() : "?");
  return ok;
}

// 성공하면 재시작해서 돌아오지 않음. 실패하면 이유를 돌려줌 (지금 펌웨어 그대로 동작)
String otaInstall(const OtaInfo& info) {
  if (!esp_ota_get_next_update_partition(nullptr)) return "no OTA slot (update once over USB)";
  logf("[OTA] downloading %s (%u bytes)", info.version.c_str(), (unsigned)info.size);
  netSwitch();
  String err;
  if (!http.begin(tls, info.url)) {
    err = "connect";
  } else {
    int code = http.GET();
    int len = http.getSize();
    if (code != 200) {
      err = "HTTP " + String(code);
    } else if (len > 0 && (size_t)len != info.size) {
      err = "size mismatch";
    } else if (!Update.begin(info.size)) {
      err = String("begin: ") + Update.errorString();
    } else {
      Update.setMD5(info.md5.c_str());
      NetworkClient* s = http.getStreamPtr();
      uint32_t t0 = millis();
      while (s->available() <= 0 && millis() - t0 < 10000) delay(10);   // 첫 바이트(0xE9) 확인 전에 데이터 대기
      size_t w = Update.writeStream(*s);
      if (w != info.size) {
        err = "download " + String(w) + "/" + String(info.size) + " " + Update.errorString();
        Update.abort();
      } else if (!Update.end()) {
        err = String("verify: ") + Update.errorString();   // MD5 불일치 등 → 부팅 영역 안 바꿈
      }
    }
  }
  netSwitch();
  if (err.length()) {
    logf("[OTA] failed: %s", err.c_str());
    return err;
  }
  prefs.putString("otaFrom", FW_VERSION);
  prefs.putString("otaTo", info.version);
  // 처리한 텔레그램 업데이트를 확인해 둠 → 재시작 뒤 같은 '업데이트' 버튼이 다시 들어오지 않게
  JsonDocument ack;
  ack["offset"] = updateOffset;
  ack["timeout"] = 0;
  ack["limit"] = 1;
  tgCall("getUpdates", ack);
  logf("[OTA] installed %s, restarting", info.version.c_str());
  restartWhy = RESTART_OTA;
  delay(300);
  ESP.restart();
  return "";
}

// 부팅 때 지난 업데이트 결과 정리 (성공 알림 또는 롤백 알림)
void otaBootCheck() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    otaVerifying = true;
    otaVerifyUntil = millis() + OTA_VERIFY_MS;
    logf("[OTA] new firmware: confirming after Telegram connects (rollback in 5 min otherwise)");
  }
  String to = prefs.getString("otaTo", "");
  if (to.isEmpty()) return;
  String from = prefs.getString("otaFrom", "");
  if (to == FW_VERSION) {
    otaNote = String(L("펌웨어를 업데이트했어요 ✅ v", "Firmware updated ✅ v")) + from + " → v" + to;
  } else {
    prefs.putString("otaBad", to);          // 자동 업데이트는 이 버전을 건너뜀
    otaNote = String(L("새 펌웨어(v", "The new firmware (v")) + to +
              L(")가 제대로 동작하지 않아서 이전 버전(v", ") didn't work properly, so it rolled back to v") + FW_VERSION +
              L(")으로 되돌아왔어요. 자동 업데이트는 이 버전을 건너뛰어요.", ". Auto-update will skip that version.");
    logf("[OTA] rolled back from %s", to.c_str());
  }
  prefs.remove("otaTo");
  prefs.remove("otaFrom");
}

void otaConfirmIfReady() {
  if (!otaVerifying) return;
  if (tgLastCode == 200) {
    esp_ota_mark_app_valid_cancel_rollback();
    otaVerifying = false;
    if (prefs.getString("otaBad", "") == FW_VERSION) prefs.remove("otaBad");
    logf("[OTA] new firmware confirmed");
  } else if (!alive(otaVerifyUntil)) {
    logf("[OTA] no Telegram connection in 5 min, rolling back");
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void otaAsk() {
  OtaInfo info;
  if (!otaFetchInfo(info)) {
    send(L("업데이트 정보를 못 가져왔어요. 잠시 뒤 다시 해 주세요.", "Couldn't get update info. Please try again later."));
    return;
  }
  if (verCmp(FW_VERSION, info.version) >= 0) {
    send(String(L("이미 최신 버전이에요 (v", "Already up to date (v")) + FW_VERSION + ")");
    return;
  }
  otaOffer = info;
  String t = String(L("새 펌웨어가 나왔어요: v", "New firmware: v")) + FW_VERSION + " → v" + info.version + "\n" +
             L("업데이트할까요? 1분쯤 원격 켜기가 안 되고, 문제가 생기면 이전 버전으로 돌아와요. (1분 안에 눌러 주세요)",
               "Update now? Remote power-on is unavailable for about a minute, and it rolls back if something goes wrong. (Confirm within 1 minute)");
  if (prefs.getString("otaBad", "") == info.version)
    t += L("\n⚠ 전에 문제가 있어서 되돌렸던 버전이에요.", "\n⚠ This version was rolled back before.");
  long long id = tgSend(cfg.owner, t, en() ? KB_UPD_EN : KB_UPD_KO);
  if (id) confirmState = {true, id, millis() + CONFIRM_TTL_MS, 'u'};
}

// PC가 켜졌다 꺼지고, 10분 동안 PC도 꺼져 있고 명령도 없으면 새 버전 확인 (6시간에 한 번까지)
void checkAutoUpdate() {
  if (!cfg.dev || !cfg.autoUpd || cfg.owner.isEmpty() || otaVerifying) return;
  static uint32_t lastSample = 0;
  uint32_t now = millis();
  if (lastSample && now - lastSample < PC_SAMPLE_MS) return;
  lastSample = now;
  if (pcIsOn()) {
    pcWasOn = true;
    autoArmed = false;
    return;
  }
  if (pcWasOn) {
    pcWasOn = false;
    autoArmed = true;
    pcOffSince = now;
    logf("[OTA] PC turned off: auto-update check in 10 min if idle");
  }
  if (!autoArmed || now - pcOffSince < AUTO_IDLE_MS) return;
  if (lastCmdAt && now - lastCmdAt < AUTO_IDLE_MS) return;
  if (serialAt && now - serialAt < SERIAL_QUIET_MS) return;
  if (pending.active || confirmState.active || tgLastCode != 200) return;
  if (lastAutoCheck && now - lastAutoCheck < AUTO_GAP_MS) return;
  autoArmed = false;
  lastAutoCheck = now;
  OtaInfo info;
  if (!otaFetchInfo(info) || verCmp(FW_VERSION, info.version) >= 0) return;
  if (prefs.getString("otaBad", "") == info.version) {
    logf("[OTA] skipping %s (rolled back before)", info.version.c_str());
    return;
  }
  send(String(L("PC가 꺼져 있어서 펌웨어를 업데이트할게요: v", "The PC is off, so I'm updating the firmware: v")) +
       FW_VERSION + " → v" + info.version + "\n" +
       L("1분쯤 원격 켜기가 안 돼요. 끝나면 알려 드릴게요.", "Remote power-on is unavailable for about a minute. I'll report back."));
  String err = otaInstall(info);              // 성공하면 재시작
  send(String(L("자동 업데이트에 실패했어요 (", "Auto-update failed (")) + err +
       L("). 지금 버전 그대로 동작해요.", "). Still running the current version."));
}

bool agentFresh() { return agent.at && agent.ip.length() && millis() - agent.at < AGENT_TTL_MS; }

void agentClear() {
  if (agent.at) logf("[Mouse] PC app address cleared");
  agent = Agent();
}

void sendMouseLink() {
  if (!pcIsOn()) {
    agentClear();
    send(L("PC가 꺼져 있어요. 🟢 켜기로 먼저 켜 주세요.", "The PC is off. Turn it on with 🟢 Power on first."));
    return;
  }
  if (!agentFresh()) {
    send(L("PC 프로그램이 실행 중이 아니에요.\n"
           "PC에서 'PC 원격 터치패드'를 켜 주세요. 방금 켰거나 로그인했다면 10초쯤 뒤에 다시 눌러 주세요.",
           "The PC app isn't running.\n"
           "Start 'PC Remote Touchpad' on the PC. If you just started it or signed in, try again in about 10 seconds."));
    return;
  }
  String url = "http://" + agent.ip + ":" + String(agent.port) + "/?t=" + agent.token;
  if (en()) url += "&l=en";
  tgSend(cfg.owner,
         String(L("🖱️ 링크를 누르면 폰이 터치패드가 돼요 (PC와 같은 와이파이에서만)\n",
                  "🖱️ Tap the link to use your phone as a touchpad (same Wi-Fi as the PC only)\n")) + url,
         nullptr, true);
}

void handleMessage(JsonObject m) {
  String chat = i64str(m["chat"]["id"].as<long long>());
  String ctype = m["chat"]["type"] | "";
  String text = m["text"] | "";
  text.trim();
  long msgDate = m["date"].as<long>();
  String fromName = m["from"]["first_name"] | "";

  // /start <등록코드> — 설정 페이지 QR(t.me/봇?start=코드)로 들어오면 자동으로 이 형태가 옴
  if (text.startsWith("/start")) {
    String arg = text.substring(6);
    arg.trim();
    if (arg.length() && pairActive() && ctype == "private") {
      if (arg == pairCode) {
        setOwner(chat, fromName);
        return;
      }
      if (++pairFails >= 5) endPairing(true);
    }
    if (chat == cfg.owner) {
      send(helpText(), kbMain());
    } else if (cfg.owner.isEmpty() && ctype == "private") {
      tgSend(chat, L("설정 페이지의 '내 텔레그램 등록'에서 QR 코드를 찍어서 들어와 주세요.",
                     "Scan the QR code under 'Register my Telegram' on the settings page to get started."));
    }
    return;
  }

  if (cfg.owner.isEmpty() || chat != cfg.owner) return;   // 주인 말고는 무시
  lastCmdAt = millis();

  Act a = parseAct(text);
  if ((a == A_ON || a == A_OFF || a == A_FORCE) && !freshCommand(msgDate)) {
    staleCount++;                         // 한 번에 모아서 알림 (pollTelegram)
    return;
  }

  if ((a == A_ON || a == A_OFF) && pending.active) {   // 방금 누른 결과를 기다리는 중 → 연타 방지
    send(L("방금 전원 버튼을 눌렀어요. 결과를 알려 드릴 때까지 잠시만 기다려 주세요.",
           "I just pressed the power button. Please wait until I report the result."));
    return;
  }

  switch (a) {
    case A_ON:
      if (pcIsOn()) {
        send(L("이미 켜져 있어요 ✅", "The PC is already on ✅"));
      } else {
        pressButton(PRESS_MS);
        schedule(ON_CHECK_MS, true);
        send(L("전원 버튼을 눌렀어요. 40초 뒤에 켜졌는지 알려 드릴게요.",
               "Pressed the power button. I'll confirm in 40 seconds."));
      }
      break;
    case A_OFF:
      if (!pcIsOn()) send(L("이미 꺼져 있어요 (또는 절전 중)", "The PC is already off (or asleep)"));
      else askConfirm('o');
      break;
    case A_FORCE:
      if (!pcIsOn()) send(L("이미 꺼져 있어서 강제 종료하지 않았어요.", "The PC is already off, so nothing was forced."));
      else askConfirm('f');
      break;
    case A_STATUS:
      send(statusText());
      break;
    case A_HELP:
      send(helpText(), kbMain());
      break;
    case A_MOUSE:
      sendMouseLink();
      break;
    case A_UPDATE:
      if (!cfg.dev) send(L("개발자 모드가 꺼져 있어요. PC에 USB로 연결한 설정 페이지에서만 켤 수 있어요.",
                           "Developer mode is off. It can only be turned on from the settings page over USB."));
      else if (otaVerifying) send(L("방금 설치한 펌웨어를 확인하는 중이에요. 잠시 뒤 다시 해 주세요.",
                                    "Still checking the firmware that was just installed. Please try again shortly."));
      else otaAsk();
      break;
    default:
      send(L("아래 버튼을 눌러 주세요.", "Please use the buttons below."), kbMain());
      break;
  }
}

void handleCallback(JsonObject cq) {
  String id = cq["id"] | "";
  String from = i64str(cq["from"]["id"].as<long long>());
  String chat = i64str(cq["message"]["chat"]["id"].as<long long>());
  long long msgId = cq["message"]["message_id"].as<long long>();
  String data = cq["data"] | "";

  if (cfg.owner.isEmpty() || from != cfg.owner || chat != cfg.owner) {
    tgAnswerCallback(id, "");
    return;
  }
  lastCmdAt = millis();
  if (data == "cancel") {
    if (confirmState.active && confirmState.msgId == msgId) confirmState.active = false;
    tgAnswerCallback(id, L("취소했어요", "Cancelled"));
    tgEditText(msgId, L("취소했어요.", "Cancelled."));
    return;
  }
  bool valid = confirmState.active && confirmState.msgId == msgId && alive(confirmState.until);
  char action = confirmState.action;
  if (!valid) {
    tgAnswerCallback(id, L("확인 시간이 지났어요", "Confirmation expired"));
    tgEditText(msgId, L("확인 시간이 지나서 실행하지 않았어요. 다시 눌러 주세요.",
                        "The confirmation expired, so nothing was done. Please try again."));
    return;
  }
  confirmState.active = false;

  if (data == "off" && action == 'o') {
    if (pending.active) {
      tgAnswerCallback(id, L("결과 확인 중이에요", "Still checking"));
      tgEditText(msgId, L("방금 누른 결과를 확인하는 중이라 실행하지 않았어요. 결과가 나온 뒤 다시 눌러 주세요.",
                          "I'm still checking the last press, so nothing was done. Try again after the result."));
      return;
    }
    if (!pcIsOn()) {
      tgAnswerCallback(id, L("이미 꺼져 있어요", "Already off"));
      tgEditText(msgId, L("이미 꺼져 있어요.", "The PC is already off."));
      return;
    }
    pressButton(PRESS_MS);
    schedule(OFF_CHECK_MS, false);
    tgAnswerCallback(id, L("종료 신호를 보냈어요", "Shutdown signal sent"));
    tgEditText(msgId, L("종료 신호를 보냈어요. 90초 뒤에 꺼졌는지 알려 드릴게요.",
                        "Shutdown signal sent. I'll confirm in 90 seconds."));
  } else if (data == "force" && action == 'f') {
    if (!pcIsOn()) {
      tgAnswerCallback(id, L("이미 꺼져 있어요", "Already off"));
      tgEditText(msgId, L("이미 꺼져 있어서 강제 종료하지 않았어요.", "The PC is already off, so nothing was forced."));
      return;
    }
    tgAnswerCallback(id, L("강제 종료 중", "Forcing off"));
    tgEditText(msgId, L("전원 버튼을 6초 누르는 중이에요…", "Holding the power button for 6 seconds…"));
    pressButton(FORCE_MS);
    schedule(FORCE_CHECK_MS, false);
  } else if (data == "upd" && action == 'u') {
    if (!cfg.dev || pending.active) {
      tgAnswerCallback(id, L("지금은 할 수 없어요", "Not now"));
      tgEditText(msgId, cfg.dev ? L("방금 누른 전원 버튼 결과를 확인하는 중이라 업데이트하지 않았어요. 결과가 나온 뒤 다시 해 주세요.",
                                    "I'm still checking the last power press, so nothing was updated. Try again after the result.")
                                : L("개발자 모드가 꺼져 있어서 업데이트하지 않았어요.", "Developer mode is off, so nothing was updated."));
      return;
    }
    tgAnswerCallback(id, L("업데이트 시작", "Updating"));
    tgEditText(msgId, String(L("v", "Downloading v")) + otaOffer.version +
                      L(" 펌웨어를 받는 중이에요… 1분쯤 뒤 다시 켜지면 알려 드릴게요.", "… I'll report back after the restart in about a minute."));
    String err = otaInstall(otaOffer);       // 성공하면 재시작해서 여기로 안 돌아옴
    send(String(L("업데이트에 실패했어요 (", "Update failed (")) + err +
         L("). 지금 버전 그대로 동작해요.", "). Still running the current version."));
  } else {
    tgAnswerCallback(id, "");
  }
}

void pollTelegram() {
  static JsonDocument filter;
  static bool filterReady = false;
  if (!filterReady) {
    filterReady = true;
    filter["ok"] = true;
    JsonObject r = filter["result"][0].to<JsonObject>();
    r["update_id"] = true;
    JsonObject m = r["message"].to<JsonObject>();
    m["message_id"] = true;
    m["date"] = true;
    m["text"] = true;
    m["chat"]["id"] = true;
    m["chat"]["type"] = true;
    m["from"]["first_name"] = true;
    JsonObject cq = r["callback_query"].to<JsonObject>();
    cq["id"] = true;
    cq["data"] = true;
    cq["from"]["id"] = true;
    cq["message"]["message_id"] = true;
    cq["message"]["chat"]["id"] = true;
  }

  JsonDocument body;
  body["offset"] = updateOffset;
  body["timeout"] = 0;
  body["limit"] = 5;
  JsonArray au = body["allowed_updates"].to<JsonArray>();
  au.add("message");
  au.add("callback_query");

  JsonDocument out;
  int code = tgCall("getUpdates", body, &out, &filter);
  recordTg(code);
  if (code != 200 || !out["ok"].as<bool>()) return;

  staleCount = 0;
  for (JsonObject u : out["result"].as<JsonArray>()) {
    long long uid = u["update_id"].as<long long>();
    if (uid >= updateOffset) updateOffset = uid + 1;
    if (u["message"].is<JsonObject>()) handleMessage(u["message"].as<JsonObject>());
    else if (u["callback_query"].is<JsonObject>()) handleCallback(u["callback_query"].as<JsonObject>());
  }
  if (staleCount == 1) {
    send(L("예전에 보낸 명령이라 실행하지 않았어요. 필요하면 다시 눌러 주세요.",
           "That command was sent earlier, so it wasn't run. Tap again if you still need it."));
  } else if (staleCount > 1) {
    send(en() ? String(staleCount) + " earlier commands weren't run. Tap again if you still need them."
              : "예전에 보낸 명령 " + String(staleCount) + "개는 실행하지 않았어요. 필요하면 다시 눌러 주세요.");
  }
}

// ===================== USB 설정 프로토콜 =====================
void cmdInfo(JsonDocument& res) {
  res["fw"] = FW_VERSION;
  res["chip"] = "ESP32-C3";
  res["mac"] = WiFi.macAddress();
  res["pc"] = pcIsOn() ? "on" : "off";
  res["txLow"] = cfg.txLow;
  res["lang"] = cfg.lang;
  res["dev"] = cfg.dev;
  res["autoUpd"] = cfg.autoUpd;
  res["otaReady"] = esp_ota_get_next_update_partition(nullptr) != nullptr;   // 원격 업데이트용 두 번째 영역 있음
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (run) res["slot"] = run->label;
  res["time"] = bootEpoch != 0;
  res["uptime"] = uptimeSec();
  res["agent"] = agentFresh();
  JsonObject w = res["wifi"].to<JsonObject>();
  w["configured"] = cfg.ssid.length() > 0;
  w["ssid"] = cfg.ssid;
  w["connected"] = WiFi.isConnected();
  if (WiFi.isConnected()) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  JsonObject t = res["tg"].to<JsonObject>();
  t["configured"] = cfg.token.length() > 0;
  t["bot"] = cfg.bot;
  t["ok"] = tgLastCode == 200;
  t["code"] = tgLastCode;
  t["blocked"] = tgBlocked;
  t["owner"] = cfg.owner.length() > 0;
  t["ownerName"] = cfg.ownerName;
  t["pairing"] = pairActive();
}

void cmdScan(JsonDocument& res) {
  int n = WiFi.scanNetworks(false, false);
  JsonArray arr = res["networks"].to<JsonArray>();
  // 같은 이름은 신호 센 것 하나만, 센 순서로 최대 20개
  for (int pick = 0; pick < 20; pick++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;
      bool used = false;
      for (JsonObject o : arr) if (s == o["ssid"].as<String>()) { used = true; break; }
      if (used) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(best);
    o["rssi"] = WiFi.RSSI(best);
    o["open"] = WiFi.encryptionType(best) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  if (n < 0) {
    res["ok"] = false;
    res["error"] = "scan_failed";
  }
}

void cmdWifi(JsonDocument& req, JsonDocument& res) {
  String ssid = req["ssid"] | "";
  String pass = req["pass"] | "";
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
    res["ok"] = false;
    res["error"] = "invalid";
    return;
  }
  cfg.ssid = ssid;
  cfg.pass = pass;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  WiFi.disconnect(false, false);
  delay(100);
  wifiBegin();
  wifiReport = true;
  wifiReportStart = millis();
  logf("[WiFi] connecting to '%s'", ssid.c_str());
}

void cmdToken(JsonDocument& req, JsonDocument& res) {
  String t = req["token"] | "";
  t.trim();
  if (t.isEmpty() || t.indexOf(':') < 0 || t.length() > 80) {
    res["ok"] = false;
    res["error"] = "invalid";
    return;
  }
  if (!WiFi.isConnected()) {
    res["ok"] = false;
    res["error"] = "wifi";
    return;
  }
  String old = cfg.token;
  cfg.token = t;
  JsonDocument body, out, filter;
  filter["ok"] = true;
  filter["result"]["username"] = true;
  int code = tgCall("getMe", body, &out, &filter);
  if (code == 200 && out["ok"].as<bool>()) {
    cfg.bot = out["result"]["username"].as<String>();
    prefs.putString("token", t);
    prefs.putString("bot", cfg.bot);
    if (old != t) {                       // 다른 봇이면 주인 등록도 새로
      cfg.owner = "";
      cfg.ownerName = "";
      prefs.remove("owner");
      prefs.remove("ownerName");
      updateOffset = 0;
    }
    tgBlocked = false;
    tgLastCode = 200;
    tgSetup();
    res["bot"] = cfg.bot;
    logf("[Telegram] bot verified: @%s", cfg.bot.c_str());
  } else {
    cfg.token = old;
    res["ok"] = false;
    res["error"] = (code == 401 || code == 404) ? "invalid" : "network";
    res["code"] = code;
  }
}

void cmdPair(JsonDocument& res) {
  if (cfg.token.isEmpty() || cfg.bot.isEmpty() || tgBlocked) {
    res["ok"] = false;
    res["error"] = "no_bot";
    return;
  }
  char c[8];
  snprintf(c, sizeof c, "%06lu", (unsigned long)(esp_random() % 1000000UL));
  pairCode = c;
  pairUntil = millis() + PAIR_TTL_MS;
  pairFails = 0;
  res["code"] = pairCode;
  res["bot"] = cfg.bot;
  res["ttl"] = PAIR_TTL_MS / 1000;
  logf("[Pairing] code issued (valid 10 min)");
}

void cmdSet(JsonDocument& req) {
  if (req["txLow"].is<bool>()) {
    cfg.txLow = req["txLow"].as<bool>();
    prefs.putBool("txLow", cfg.txLow);
    applyTxPower();
  }
  String lang = req["lang"] | "";
  if ((lang == "ko" || lang == "en") && lang != cfg.lang) {
    cfg.lang = lang;
    prefs.putString("lang", lang);
    logf("[Settings] bot language: %s", lang.c_str());
    if (cfg.token.length() && !tgBlocked && WiFi.isConnected()) {
      tgSetCommands();
      send(L("봇 언어를 한국어로 바꿨어요.", "Bot language set to English."), kbMain());
    }
  }
  if (req["autoUpd"].is<bool>()) {
    cfg.autoUpd = req["autoUpd"].as<bool>();
    prefs.putBool("autoUpd", cfg.autoUpd);
    logf("[Settings] auto-update: %s", cfg.autoUpd ? "on" : "off");
  }
  if (req["dev"].is<bool>() && req["dev"].as<bool>() != cfg.dev) {
    cfg.dev = req["dev"].as<bool>();
    prefs.putBool("dev", cfg.dev);
    logf("[Settings] developer mode: %s", cfg.dev ? "on" : "off");
    if (cfg.token.length() && !tgBlocked && WiFi.isConnected()) {
      tgSetCommands();
      send(cfg.dev ? L("개발자 모드를 켰어요. /update 로 펌웨어를 원격 업데이트할 수 있어요.",
                       "Developer mode is on. Use /update to update the firmware remotely.")
                   : L("개발자 모드를 껐어요. 펌웨어 업데이트는 설정 페이지에서 USB로만 돼요.",
                       "Developer mode is off. Firmware updates now work only from the settings page over USB."),
           kbMain());
    }
  }
}

bool privateIp(const IPAddress& a) {
  return a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168);
}

void cmdAgent(JsonDocument& req, JsonDocument& res) {
  String ip = req["ip"] | "";
  long port = req["port"] | 0L;
  String token = req["token"] | "";
  IPAddress a;
  bool tokOk = token.length() >= 16 && token.length() <= 64;
  for (size_t i = 0; tokOk && i < token.length(); i++) {
    char c = token[i];
    tokOk = isalnum((unsigned char)c) || c == '-' || c == '_';
  }
  if (!a.fromString(ip) || !privateIp(a) || port < 1024 || port > 65535 || !tokOk) {
    res["ok"] = false;
    res["error"] = "invalid";
    return;
  }
  bool changed = !agentFresh() || ip != agent.ip || port != agent.port || token != agent.token;
  agent.ip = ip;
  agent.port = (uint16_t)port;
  agent.token = token;
  agent.at = millis();
  if (changed) logf("[Mouse] PC app at %s:%ld", ip.c_str(), port);
  res["fw"] = FW_VERSION;
  res["pc"] = pcIsOn() ? "on" : "off";
  if (WiFi.isConnected()) res["boardIp"] = WiFi.localIP().toString();   // PC 프로그램이 같은 망 주소를 고르는 데 씀
}

void handleRequest(const String& line) {
  JsonDocument req;
  JsonDocument res;
  if (deserializeJson(req, line)) {
    serialAt = millis();
    res["ok"] = false;
    res["error"] = "bad_json";
    emit(res);
    return;
  }
  res["id"] = req["id"];
  res["ok"] = true;
  String cmd = req["cmd"] | "";
  if (cmd != "agent") serialAt = millis();   // PC 프로그램의 주기적 신호는 설정 페이지 사용으로 안 침

  if (cmd == "agent") cmdAgent(req, res);
  else if (cmd == "info") cmdInfo(res);
  else if (cmd == "scan") cmdScan(res);
  else if (cmd == "wifi") cmdWifi(req, res);
  else if (cmd == "token") cmdToken(req, res);
  else if (cmd == "pair") cmdPair(res);
  else if (cmd == "set") cmdSet(req);
  else if (cmd == "unpair") {
    cfg.owner = "";
    cfg.ownerName = "";
    prefs.remove("owner");
    prefs.remove("ownerName");
    endPairing(false);
    logf("[Pairing] owner removed");
  } else if (cmd == "reboot" || cmd == "reset") {
    if (cmd == "reset") {
      prefs.clear();
      WiFi.disconnect(true, true);
      logf("[Settings] factory reset");
    }
    emit(res);
    Serial.flush();
    delay(300);
    ESP.restart();
  } else {
    res["ok"] = false;
    res["error"] = "unknown_cmd";
  }
  emit(res);
}

void handleSerial() {
  static String rx;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      rx.trim();
      if (rx.startsWith("{")) handleRequest(rx);
      else if (rx.length()) logf("Use the settings page (this port only accepts JSON commands)");
      rx = "";
    } else if (c != '\r') {
      if (rx.length() < 1024) rx += c;
    }
  }
}

// 와이파이나 텔레그램이 30분 넘게 안 되면 재시작 (드라이버·TLS가 꼬인 경우 회복)
// 재시작해도 setup()이 버튼 핀부터 LOW로 잡으므로 PC에는 영향 없음
void checkStuck() {
  uint32_t now = millis();
  if (WiFi.isConnected()) wifiOkAt = now;
  if (serialAt && now - serialAt < SERIAL_QUIET_MS) return;   // 설정 페이지 사용 중
  if (pending.active) return;                                  // 버튼 결과 확인 중
  const char* why = nullptr;
  if (cfg.ssid.length() && now - wifiOkAt > STUCK_MS) why = "Wi-Fi";
  else if (cfg.token.length() && !tgBlocked && WiFi.isConnected() && now - tgOkAt > STUCK_MS) why = "Telegram";
  if (!why) return;
  logf("[Watchdog] no %s for 30 min, restarting", why);
  restartWhy = RESTART_COMM;
  delay(200);
  ESP.restart();
}

// ===================== setup / loop =====================
void setup() {
  pinMode(PIN_PWR_SW, OUTPUT);
  digitalWrite(PIN_PWR_SW, LOW);           // 제일 먼저: 부팅 중 버튼 눌림 방지
  bootWhy = restartWhy;                    // 재시작 이유를 읽고 바로 지움
  restartWhy = 0;
  pinMode(PIN_LED, OUTPUT);
  led(false);

  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  delay(200);

  prefs.begin("pcpower", false);
  loadConfig();
  otaBootCheck();                          // 지난 원격 업데이트 결과 (성공/롤백) 정리

  WiFi.persistent(false);                  // 와이파이 정보는 우리 설정에만 저장
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("pc-power");
  WiFi.setAutoReconnect(true);
  wifiBegin();

  configTzTime("KST-9", "kr.pool.ntp.org", "time.google.com", "pool.ntp.org");
  tls.useBuiltinCACertBundle();
  tls.setHandshakeTimeout(15);
  http.setReuse(true);
  http.setConnectTimeout(8000);
  http.setTimeout(10000);

  logf("[PC Remote Power v%s] started", FW_VERSION);
  JsonDocument hello;
  hello["evt"] = "hello";
  hello["fw"] = FW_VERSION;
  emit(hello);
}

void loop() {
  handleSerial();
  checkWifi();
  syncClock();

  checkStuck();
  if (pairCode.length() && !alive(pairUntil)) endPairing(true);
  if (pending.active && !alive(pending.due)) reportPending();

  bool tgReady = cfg.token.length() && !tgBlocked && WiFi.isConnected() && bootEpoch && !alive(tgBackoffUntil);
  led(WiFi.isConnected() && tgLastCode == 200);
  otaConfirmIfReady();                     // 새 펌웨어: 텔레그램 연결되면 확정, 5분 넘으면 롤백

  if (tgReady && !bootNotified && cfg.owner.length() && tgLastCode == 200) {
    bootNotified = true;
    if (otaNote.length()) send(otaNote + "\n" + statusText(), kbMain());
    else send(String(L("보드가 다시 켜졌어요: ", "The module restarted: ")) + resetReasonText() + "\n" + statusText(), kbMain());
    otaNote = "";
  }
  checkAutoUpdate();

  static uint32_t lastAgentCheck = 0;       // PC가 꺼지거나 잠들면 예전 링크를 지움
  if (agent.at && millis() - lastAgentCheck >= 5000) {
    lastAgentCheck = millis();
    if (!agentFresh() || !pcIsOn()) agentClear();
  }

  static uint32_t lastPoll = 0;
  uint32_t interval = (cfg.owner.length() || pairActive()) ? POLL_MS : POLL_IDLE_MS;
  if (tgReady && millis() - lastPoll >= interval) {
    lastPoll = millis();
    pollTelegram();
  }
  delay(5);
}
