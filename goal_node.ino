/*
 * ESP32 + FC-51 進球節點（紅隊／藍隊共用）
 * 接線：FC-51 VCC -> 3V3 ； GND -> GND ； OUT -> GPIO 18（紅）／GPIO 19（藍）
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ======================================================
// 1. 設定
// ======================================================

// ★ 1 = 紅隊節點，0 = 藍隊節點（燒錄前記得確認！）
#define IS_RED_NODE 1

// ★ 必須改成「主控」序列埠印出的「填入節點：{...}」那一行！
uint8_t MASTER_MAC[6] = {0x1C, 0x9D, 0xC2, 0xF8, 0xB2, 0x51};

#if IS_RED_NODE
  #define SENSOR_PIN       18
  #define PKT_GOAL_TYPE    3      // PKT_RED_GOAL
  #define PKT_BEAT_TYPE    4      // PKT_RED_HEARTBEAT
  #define TEAM_NAME        "紅隊"
  #define TEAM_ICON        "🔴"
#else
  #define SENSOR_PIN       19
  #define PKT_GOAL_TYPE    1      // PKT_BLUE_GOAL
  #define PKT_BEAT_TYPE    2      // PKT_BLUE_HEARTBEAT
  #define TEAM_NAME        "藍隊"
  #define TEAM_ICON        "🔵"
#endif

#define STATUS_LED 2   // 板載 LED：亮 = 已連上主控

const unsigned long SENSOR_ON_TIME  = 5;     // 偵測到球需穩定多久（毫秒）
const unsigned long SENSOR_OFF_TIME = 100;   // 球離開需穩定多久（毫秒）
const unsigned long GOAL_COOLDOWN_MS = 500;  // 進球後 0.5 秒內不再計分

const unsigned long HEARTBEAT_MS      = 1000;  // 已連線時，心跳間隔
const unsigned long SEARCH_INTERVAL   = 30;    // 搜尋頻道時，每次嘗試的間隔
const unsigned long GOAL_RETRY_MS     = 10;    // 進球封包送失敗時，重送間隔
const unsigned long SEND_TIMEOUT_MS   = 100;   // 等不到送出結果就視為失敗
const int           FAILS_BEFORE_SCAN = 5;     // 連續失敗幾次就重新搜尋頻道
const uint8_t       MAX_CHANNEL       = 13;

// ======================================================
// 2. ESP-NOW 封包格式（必須與主控完全一致！）
// ======================================================

#define PKT_MAGIC  0xA5

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;
};

// ======================================================
// 3. 全域變數
// ======================================================

int rawState = HIGH;
int stableState = HIGH;
unsigned long stateChangeTime = 0;
bool locked = false;
unsigned long cooldownUntil = 0;

volatile bool sendBusy = false;
volatile bool resultReady = false;
volatile bool resultOk = false;

bool linked = false;
uint8_t channel = 1;
int failCount = 0;

int goalQueue = 0;
bool goalOutstanding = false;
uint16_t goalSeq = 0;

uint8_t inflightType = 0;
unsigned long sendStartTime = 0;
unsigned long lastGoalTry = 0;
unsigned long lastHeartbeat = 0;

// ======================================================
// 4. 無線傳送
// ======================================================

#if defined(ESP_ARDUINO_VERSION_VAL) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
void onEspNowSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
#endif
  resultOk = (status == ESP_NOW_SEND_SUCCESS);
  resultReady = true;
  sendBusy = false;
}

void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  channel = ch;
}

void registerResult(bool ok) {
  if (ok) {
    if (!linked) Serial.printf("[連線] 已連上主控，頻道 %d\n", channel);
    linked = true;
    failCount = 0;
    if (inflightType == PKT_GOAL_TYPE) goalOutstanding = false;
  } else {
    failCount++;
    if (!linked || failCount >= FAILS_BEFORE_SCAN) {
      if (linked) Serial.println("[連線] 與主控失聯，重新搜尋頻道…");
      linked = false;
      failCount = 0;
      setChannel(channel >= MAX_CHANNEL ? 1 : channel + 1);
    }
  }
  digitalWrite(STATUS_LED, linked ? HIGH : LOW);
}

void transmit(uint8_t type, uint16_t seq) {
  Packet p = {PKT_MAGIC, type, seq};

  inflightType = type;
  sendStartTime = millis();
  resultReady = false;
  sendBusy = true;

  if (esp_now_send(MASTER_MAC, (uint8_t*)&p, sizeof(p)) != ESP_OK) {
    sendBusy = false;
    registerResult(false);
  }
}

void serviceRadio(unsigned long now) {

  if (sendBusy) {
    if (now - sendStartTime > SEND_TIMEOUT_MS) {
      sendBusy = false;
      registerResult(false);
    }
    return;
  }

  if (resultReady) {
    resultReady = false;
    registerResult(resultOk);
  }

  if (!goalOutstanding && goalQueue > 0) {
    goalQueue--;
    goalSeq++;
    goalOutstanding = true;
    lastGoalTry = 0;
  }

  unsigned long retryGap = linked ? GOAL_RETRY_MS : SEARCH_INTERVAL;
  if (goalOutstanding) {
    if (now - lastGoalTry >= retryGap) {
      lastGoalTry = now;
      transmit(PKT_GOAL_TYPE, goalSeq);
    }
  } else {
    unsigned long hbGap = linked ? HEARTBEAT_MS : SEARCH_INTERVAL;
    if (now - lastHeartbeat >= hbGap) {
      lastHeartbeat = now;
      transmit(PKT_BEAT_TYPE, 0);
    }
  }
}

// ======================================================
// 5. 感測器處理（防彈跳 + 鎖定 + 0.5 秒冷卻）
// ======================================================

void handleSensor(unsigned long now) {

  int reading = digitalRead(SENSOR_PIN);

  if (reading != rawState) {
    rawState = reading;
    stateChangeTime = now;
  }

  unsigned long requiredTime =
    (rawState == LOW) ? SENSOR_ON_TIME : SENSOR_OFF_TIME;

  if (rawState != stableState &&
      now - stateChangeTime >= requiredTime) {

    stableState = rawState;

    if (stableState == LOW && !locked) {

      if ((long)(now - cooldownUntil) >= 0) {
        goalQueue++;
        cooldownUntil = now + GOAL_COOLDOWN_MS;
        Serial.printf("[感測器進球] %s %s偵測到球，傳送給主控\n", TEAM_ICON, TEAM_NAME);
      } else {
        Serial.println("[冷卻中] 忽略反彈球");
      }
      locked = true;
    }

    if (stableState == HIGH) {
      locked = false;
    }
  }
}

// ======================================================
// 6. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(SENSOR_PIN, INPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  rawState = digitalRead(SENSOR_PIN);
  stableState = rawState;
  stateChangeTime = millis();
  locked = (rawState == LOW);

  goalSeq = (uint16_t)(esp_random() & 0xFFFF);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[錯誤] ESP-NOW 初始化失敗！");
    return;
  }
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[錯誤] 加入主控失敗！請確認 MASTER_MAC");
  }

  setChannel(1);
  Serial.printf("[系統通知] %s節點已啟動，正在搜尋主控…\n", TEAM_NAME);
}

// ======================================================
// 7. 主迴圈 loop()
// ======================================================

void loop() {

  unsigned long now = millis();

  handleSensor(now);
  serviceRadio(now);

  delay(1);
}
