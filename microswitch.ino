/*
 * ESP32 + FC-51 雙隊自動計分系統（v6：天秤開關板 SWITCH）
 *
 * 本程式功能：
 *  - 讀取兩個微動開關（V-15 類型），各裝在一個天秤上
 *      紅隊天秤開關 -> GPIO 25
 *      藍隊天秤開關 -> GPIO 26
 *  - 每按一次（天秤壓下開關）就用 ESP-NOW 傳「按壓事件」給主機
 *  - 每秒送一次心跳，讓主機的裁判頁面顯示「天秤開關板已連線」
 *  - 「按 2 次 -> 該隊 10 秒內進球算兩倍」的規則由主機判斷，這裡只負責回報按壓
 *
 * 接線（微動開關有三支腳：COM、NO、NC）：
 *  開關 COM  -> GND
 *  開關 NO   -> GPIO 25（紅隊）／GPIO 26（藍隊）
 *  NC 不接
 *  （使用 ESP32 內建上拉電阻：沒壓住 = HIGH，壓下 = LOW，不需要外接電阻）
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

#define RED_SWITCH_PIN   25   // 紅隊天秤開關
#define BLUE_SWITCH_PIN  26   // 藍隊天秤開關
#define STATUS_LED        2   // 板載 LED：亮 = 已連上主機

// 開關防彈跳（毫秒）
const unsigned long SWITCH_PRESS_TIME   = 20;   // 壓下需穩定多久才算一次按壓
const unsigned long SWITCH_RELEASE_TIME = 50;   // 放開需穩定多久才算放開

const unsigned long HEARTBEAT_MS      = 1000;   // 已連線時，心跳間隔
const unsigned long SEARCH_INTERVAL   = 30;     // 搜尋頻道時，每次嘗試的間隔
const unsigned long PRESS_RETRY_MS    = 10;     // 按壓封包送失敗時，重送間隔
const unsigned long SEND_TIMEOUT_MS   = 100;    // 等不到送出結果就視為失敗
const int           FAILS_BEFORE_SCAN = 5;      // 連續失敗幾次就重新搜尋頻道
const uint8_t       MAX_CHANNEL       = 13;     // 台灣為 1~13

// ======================================================
// 2. ESP-NOW 封包格式（必須與主機完全一致！）
// ======================================================

#define PKT_MAGIC         0xA5
#define PKT_GOAL          1    // 藍隊感測節點用（本板不使用）
#define PKT_HEARTBEAT     2    // 藍隊感測節點用（本板不使用）
#define PKT_SW_RED        3    // 紅隊天秤被按一次
#define PKT_SW_BLUE       4    // 藍隊天秤被按一次
#define PKT_SW_HEARTBEAT  5    // 天秤開關板心跳

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;
};

// ======================================================
// 3. 開關狀態
// ======================================================

struct SwitchState {
  uint8_t pin;                  // 腳位
  uint8_t pktType;              // 按壓時要送的封包類型
  int raw;                      // 目前讀到的原始訊號
  int stable;                   // 穩定後的訊號
  unsigned long changeTime;     // 訊號最後一次變化的時間
  bool locked;                  // 壓住期間只算一次
};

SwitchState sw[2] = {
  {RED_SWITCH_PIN,  PKT_SW_RED,  HIGH, HIGH, 0, false},
  {BLUE_SWITCH_PIN, PKT_SW_BLUE, HIGH, HIGH, 0, false}
};

// 按壓事件佇列（避免連按太快漏掉）
const int QUEUE_SIZE = 16;
uint8_t pressQueue[QUEUE_SIZE];
int qHead = 0;
int qCount = 0;

// 放入一筆按壓
void enqueuePress(uint8_t type) {
  if (qCount >= QUEUE_SIZE) return;   // 佇列滿了就丟棄（正常使用不會發生）
  pressQueue[(qHead + qCount) % QUEUE_SIZE] = type;
  qCount++;
}

// 取出一筆按壓
uint8_t dequeuePress() {
  uint8_t t = pressQueue[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  qCount--;
  return t;
}

// ======================================================
// 4. 無線傳送狀態
// ======================================================

volatile bool sendBusy = false;      // 正在等待送出結果
volatile bool resultReady = false;   // 回呼函式已給出結果
volatile bool resultOk = false;      // 送出結果（true = 主機有確認收到）

bool linked = false;                 // 是否已連上主機
uint8_t channel = 1;                 // 目前使用的頻道
int failCount = 0;                   // 連續失敗次數

bool pressOutstanding = false;       // 目前有一筆按壓正在送（尚未確認）
uint8_t outType = 0;                 // 正在送的按壓類型
uint16_t pressSeq = 0;               // 按壓流水號（重送時相同，主機用它去重）

uint8_t inflightType = 0;
unsigned long sendStartTime = 0;
unsigned long lastPressTry = 0;
unsigned long lastHeartbeat = 0;

// ======================================================
// 5. 無線傳送
// ======================================================

// 不同版本的 Arduino-ESP32 核心，回呼函式的參數不同（3.3.0 起改了）
#if defined(ESP_ARDUINO_VERSION_VAL) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
void onEspNowSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
#endif
  resultOk = (status == ESP_NOW_SEND_SUCCESS);
  resultReady = true;
  sendBusy = false;
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
    if (inflightType == PKT_SW_RED || inflightType == PKT_SW_BLUE) {
      pressOutstanding = false;   // 這筆按壓已確認
    }
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

// 每圈呼叫一次：非阻塞，不會卡住開關偵測
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

  // 沒有按壓在送時，從佇列取出下一筆
  if (!pressOutstanding && qCount > 0) {
    outType = dequeuePress();
    pressSeq++;
    pressOutstanding = true;
    lastPressTry = 0;
  }

  // 按壓優先；沒有按壓才送心跳
  unsigned long retryGap = linked ? PRESS_RETRY_MS : SEARCH_INTERVAL;
  if (pressOutstanding) {
    if (now - lastPressTry >= retryGap) {
      lastPressTry = now;
      transmit(outType, pressSeq);
    }
  } else {
    unsigned long hbGap = linked ? HEARTBEAT_MS : SEARCH_INTERVAL;
    if (now - lastHeartbeat >= hbGap) {
      lastHeartbeat = now;
      transmit(PKT_SW_HEARTBEAT, 0);
    }
  }
}

// ======================================================
// 6. 開關處理（防彈跳＋壓住只算一次）
// ======================================================

void handleSwitch(SwitchState& s, unsigned long now) {

  int reading = digitalRead(s.pin);

  // 訊號變化：記錄新狀態並重新計時（防彈跳）
  if (reading != s.raw) {
    s.raw = reading;
    s.changeTime = now;
  }

  // 壓下 = LOW；壓下與放開使用不同的穩定時間
  unsigned long requiredTime =
    (s.raw == LOW) ? SWITCH_PRESS_TIME : SWITCH_RELEASE_TIME;

  if (s.raw != s.stable && now - s.changeTime >= requiredTime) {

    s.stable = s.raw;

    if (s.stable == LOW && !s.locked) {
      enqueuePress(s.pktType);   // 記一次按壓，等著傳給主機
      Serial.printf("[天秤] %s 天秤被按了一次\n",
                    s.pktType == PKT_SW_RED ? "紅隊" : "藍隊");
      s.locked = true;           // 壓住期間只算一次
    }

    if (s.stable == HIGH) {
      s.locked = false;          // 放開後才能再算下一次
    }
  }
}

// ======================================================
// 7. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(RED_SWITCH_PIN, INPUT_PULLUP);    // 使用內建上拉電阻
  pinMode(BLUE_SWITCH_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  // 同步開關初始狀態（開機時天秤剛好壓著開關，要先放開才會計下一次）
  for (int i = 0; i < 2; i++) {
    sw[i].raw = digitalRead(sw[i].pin);
    sw[i].stable = sw[i].raw;
    sw[i].changeTime = millis();
    sw[i].locked = (sw[i].raw == LOW);
  }

  // 流水號用隨機起點：開關板重新開機後，主機不會把新的按壓誤判成重複
  pressSeq = (uint16_t)(esp_random() & 0xFFFF);

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
  Serial.println("[系統通知] 天秤開關板已啟動，正在搜尋主機…");
}

// ======================================================
// 8. 主迴圈 loop()
// ======================================================

void loop() {

  unsigned long now = millis();

  handleSwitch(sw[0], now);   // 紅隊天秤
  handleSwitch(sw[1], now);   // 藍隊天秤
  serviceRadio(now);          // 處理無線傳送

  delay(1);                   // 讓出一點時間給系統
}
