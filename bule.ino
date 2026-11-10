/*
 * ESP32 + FC-51 雙隊自動計分系統（v6：藍隊感測節點 NODE）
 *
 * 本程式功能：
 *  - 讀取藍隊 FC-51（GPIO 19），做防彈跳與鎖定
 *  - 偵測到進球 -> 用 ESP-NOW 傳給主機（有確認機制，失敗會自動重送）
 *  - 每秒送一次心跳，讓主機的裁判頁面顯示「節點已連線」
 *  - 不需要連 Wi-Fi、不需要帳密
 *  - 自動搜尋主機所在的頻道（主機固定在頻道 1，通常第一次就連上）
 *
 * 接線（節點）：
 *  FC-51 VCC -> 3V3 ； GND -> GND ； OUT -> GPIO 19（藍隊）
 *
 * 注意：節點永遠都會回報進球；「比賽進行中才算分」由主機判斷。
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ======================================================
// 1. 設定
// ======================================================

// ★ 必須改成主機序列埠印出的「填入節點：{...}」那一行！
//   （舊值 1C:9D:C2:F8:AD:00 是路由器模式的 MAC，熱點版主機不能用）
uint8_t MASTER_MAC[6] = {0x1C, 0x9D, 0xC2, 0xF8, 0x94, 0x59};

#define BLUE_SENSOR_PIN 19   // 藍隊感測器 OUT
#define STATUS_LED       2   // 板載 LED：亮 = 已連上主機

const unsigned long SENSOR_ON_TIME  = 5;     // 偵測到球需穩定多久（毫秒）
const unsigned long SENSOR_OFF_TIME = 100;   // 球離開需穩定多久（毫秒，避免彈跳重複計分）

const unsigned long HEARTBEAT_MS      = 1000;  // 已連線時，心跳間隔
const unsigned long SEARCH_INTERVAL   = 30;    // 搜尋頻道時，每次嘗試的間隔
const unsigned long GOAL_RETRY_MS     = 10;    // 進球封包送失敗時，重送間隔
const unsigned long SEND_TIMEOUT_MS   = 100;   // 等不到送出結果就視為失敗
const int           FAILS_BEFORE_SCAN = 5;     // 連續失敗幾次就重新搜尋頻道
const uint8_t       MAX_CHANNEL       = 13;    // 台灣為 1~13

// ======================================================
// 2. ESP-NOW 封包格式（必須與主機完全一致！）
// ======================================================

#define PKT_MAGIC      0xA5   // 辨識碼
#define PKT_GOAL       1      // 進球事件
#define PKT_HEARTBEAT  2      // 心跳

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;   // 進球流水號（重送同一球時相同，主機用它去除重複）
};

// ======================================================
// 3. 全域變數
// ======================================================

// ---- 感測器狀態 ----
int blueRawState = HIGH;               // 目前讀到的原始訊號
int blueStableState = HIGH;            // 穩定後的訊號
unsigned long blueStateChangeTime = 0; // 訊號最後一次變化的時間
bool blueLocked = false;               // 球停在感測器前時鎖定，只算一次

// ---- 無線傳送狀態 ----
volatile bool sendBusy = false;     // 正在等待送出結果
volatile bool resultReady = false;  // 回呼函式已給出結果
volatile bool resultOk = false;     // 送出結果（true = 主機有確認收到）

bool linked = false;                // 是否已連上主機
uint8_t channel = 1;                // 目前使用的頻道
int failCount = 0;                  // 連續失敗次數

int goalQueue = 0;                  // 還沒開始送的進球數
bool goalOutstanding = false;       // 目前有一筆進球正在送（尚未確認）
uint16_t goalSeq = 0;               // 進球流水號

uint8_t inflightType = 0;           // 正在送的封包類型
unsigned long sendStartTime = 0;
unsigned long lastGoalTry = 0;
unsigned long lastHeartbeat = 0;

// ======================================================
// 4. 無線傳送
// ======================================================

// 不同版本的 Arduino-ESP32 核心，回呼函式的參數不同（3.3.0 起改了）
#if defined(ESP_ARDUINO_VERSION_VAL) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
void onEspNowSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
#endif
  resultOk = (status == ESP_NOW_SEND_SUCCESS);
  resultReady = true;
  sendBusy = false;   // 最後才放開，主迴圈看到 false 時結果一定已寫好
}

// 切換 Wi-Fi 頻道
void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  channel = ch;
}

// 處理一次送出的結果
void registerResult(bool ok) {
  if (ok) {
    if (!linked) Serial.printf("[連線] 已連上主機，頻道 %d\n", channel);
    linked = true;
    failCount = 0;
    if (inflightType == PKT_GOAL) goalOutstanding = false;   // 這筆進球已確認
  } else {
    failCount++;
    // 還沒連上：失敗就立刻換下一個頻道；已連上：連續失敗多次才重新搜尋
    if (!linked || failCount >= FAILS_BEFORE_SCAN) {
      if (linked) Serial.println("[連線] 與主機失聯，重新搜尋頻道…");
      linked = false;
      failCount = 0;
      setChannel(channel >= MAX_CHANNEL ? 1 : channel + 1);
    }
  }
  digitalWrite(STATUS_LED, linked ? HIGH : LOW);
}

// 送出一個封包（非阻塞，結果由回呼函式回報）
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

// 每圈呼叫一次：非阻塞，不會卡住感測器偵測
void serviceRadio(unsigned long now) {

  // 正在等結果：太久沒回應就視為失敗
  if (sendBusy) {
    if (now - sendStartTime > SEND_TIMEOUT_MS) {
      sendBusy = false;
      registerResult(false);
    }
    return;
  }

  // 處理上一次送出的結果
  if (resultReady) {
    resultReady = false;
    registerResult(resultOk);
  }

  // 沒有進球在送時，從佇列取出下一筆
  if (!goalOutstanding && goalQueue > 0) {
    goalQueue--;
    goalSeq++;
    goalOutstanding = true;
    lastGoalTry = 0;
  }

  // 進球優先；沒有進球才送心跳
  unsigned long retryGap = linked ? GOAL_RETRY_MS : SEARCH_INTERVAL;
  if (goalOutstanding) {
    if (now - lastGoalTry >= retryGap) {
      lastGoalTry = now;
      transmit(PKT_GOAL, goalSeq);
    }
  } else {
    unsigned long hbGap = linked ? HEARTBEAT_MS : SEARCH_INTERVAL;
    if (now - lastHeartbeat >= hbGap) {
      lastHeartbeat = now;
      transmit(PKT_HEARTBEAT, 0);
    }
  }
}

// ======================================================
// 5. 藍隊感測器處理（防彈跳 + 鎖定，進球時排入傳送佇列）
// ======================================================

void handleBlueSensor(unsigned long now) {

  int reading = digitalRead(BLUE_SENSOR_PIN);

  // 訊號變化：記錄新狀態並重新計時（防彈跳）
  if (reading != blueRawState) {
    blueRawState = reading;
    blueStateChangeTime = now;
  }

  unsigned long requiredTime =
    (blueRawState == LOW) ? SENSOR_ON_TIME : SENSOR_OFF_TIME;

  if (blueRawState != blueStableState &&
      now - blueStateChangeTime >= requiredTime) {

    blueStableState = blueRawState;

    if (blueStableState == LOW && !blueLocked) {
      goalQueue++;   // 有進球！排入傳送佇列
      Serial.println("[感測器進球] 🔵 藍隊偵測到球，傳送給主機");
      blueLocked = true;   // 球停在感測器前只算一次
    }

    if (blueStableState == HIGH) {
      blueLocked = false;
    }
  }
}

// ======================================================
// 6. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(BLUE_SENSOR_PIN, INPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  // 同步感測器初始狀態（開機時球就擋在前面不會誤觸發）
  blueRawState = digitalRead(BLUE_SENSOR_PIN);
  blueStableState = blueRawState;
  blueStateChangeTime = millis();
  blueLocked = (blueRawState == LOW);

  // 進球流水號用隨機起點：節點重新開機後，主機不會把新的一球誤判成重複
  goalSeq = (uint16_t)(esp_random() & 0xFFFF);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[錯誤] ESP-NOW 初始化失敗！");
    return;
  }
  esp_now_register_send_cb(onEspNowSent);

  // 把主機加入通訊對象
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = 0;          // 0 = 使用目前頻道（之後會自動搜尋切換）
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[錯誤] 加入主機失敗！請確認 MASTER_MAC");
  }

  setChannel(1);
  Serial.println("[系統通知] 藍隊節點已啟動，正在搜尋主機…");
}

// ======================================================
// 7. 主迴圈 loop()
// ======================================================

void loop() {

  unsigned long now = millis();

  handleBlueSensor(now);   // 先偵測感測器（最重要，確保不漏球）
  serviceRadio(now);       // 再處理無線傳送

  delay(1);                // 讓出一點時間給系統
}
