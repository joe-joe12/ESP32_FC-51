/*
 * ESP32 + FC-51 雙隊自動計分系統（v5：三塊 ESP32 —— 主機端 MASTER）
 *
 * 架構：
 *   主機 MASTER   ：紅隊 FC-51(GPIO18) + 網站 + 全部比賽邏輯
 *   藍隊節點 NODE ：藍隊 FC-51(GPIO19)，用 ESP-NOW 傳進球給主機
 *   天秤開關板    ：兩個微動開關(GPIO25/26)，用 ESP-NOW 傳按壓給主機
 *
 * 天秤加倍規則：某隊天秤累計按 2 次 -> 該隊接下來 10 秒內每球算 2 分
 *
 * 網頁：
 *  /          選手看板：分數與倒數計時
 *  /referee   裁判頁面：開始比賽、記錄罰分、提前結算、重設
 *
 * 接線（主機）：
 *  FC-51 VCC -> 3V3 ； GND -> GND ； OUT -> GPIO 18（紅隊）
 *
 * 使用方式：
 *  1. 先燒錄本程式，開啟序列埠監控視窗（115200），記下「主機 MAC 位址」
 *  2. 把該 MAC 填入節點與開關板程式的 MASTER_MAC
 *  3. 再燒錄節點與開關板程式
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif

// ======================================================
// 1. Wi-Fi 設定
// ======================================================

const char* ssid = "NNK-Temp";
const char* password = "nt123456";

// ======================================================
// 2. 腳位與比賽設定
// ======================================================

#define RED_SENSOR_PIN  18   // 紅隊感測器 OUT

// 每一輪的比賽時間（秒）
const unsigned long ROUND_SECONDS = 150;
const unsigned long ROUND_MS = ROUND_SECONDS * 1000UL;

// 節點超過多久沒收到任何訊號，就視為離線（毫秒）
const unsigned long NODE_TIMEOUT_MS = 3000;

// 天秤開關：累計按幾次觸發加倍、加倍持續多久（毫秒）
const int PRESSES_FOR_BONUS = 2;
const unsigned long BONUS_MS = 10000;

// ======================================================
// 3. ESP-NOW 封包格式（三個程式必須完全一致！）
// ======================================================

#define PKT_MAGIC         0xA5   // 辨識碼
#define PKT_GOAL          1      // 藍隊進球
#define PKT_HEARTBEAT     2      // 藍隊節點心跳
#define PKT_SW_RED        3      // 紅隊天秤被按一次
#define PKT_SW_BLUE       4      // 藍隊天秤被按一次
#define PKT_SW_HEARTBEAT  5      // 天秤開關板心跳

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;   // 流水號（重送同一事件時相同，主機用它去除重複）
};

// ======================================================
// 4. 全域變數
// ======================================================

WebServer server(80);

enum GameState { WAITING, RUNNING, SETTLED };
GameState gameState = WAITING;

unsigned long roundStartTime = 0;

int redScore = 0;
int blueScore = 0;
int redPenalty = 0;
int bluePenalty = 0;

// 感測器訊號需穩定多久才算有效（毫秒）
const unsigned long SENSOR_ON_TIME = 5;
const unsigned long SENSOR_OFF_TIME = 100;

// ---- 紅隊感測器狀態 ----
int redRawState = HIGH;
int redStableState = HIGH;
unsigned long redStateChangeTime = 0;
bool redLocked = false;

// ---- 藍隊節點（由 ESP-NOW 回呼更新，所以用 volatile + 臨界區保護）----
portMUX_TYPE nodeMux = portMUX_INITIALIZER_UNLOCKED;
volatile int pendingBlueGoals = 0;
volatile unsigned long blueLastSeen = 0;
volatile bool blueEverSeen = false;
int32_t lastBlueSeq = -1;

// ---- 天秤開關板（0=紅隊 1=藍隊）----
#define TEAM_RED  0
#define TEAM_BLUE 1
volatile int pendingPress[2] = {0, 0};       // 已收到、尚未處理的按壓次數
volatile unsigned long switchLastSeen = 0;
volatile bool switchEverSeen = false;
int32_t lastSwitchSeq = -1;
int  pressCount[2]  = {0, 0};                // 目前累計按壓次數
bool bonusActive[2] = {false, false};        // 是否正在加倍中
unsigned long bonusUntil[2] = {0, 0};        // 加倍結束時間（millis）

// ======================================================
// 5. 工具函式
// ======================================================

int finalScore(int score, int penalty) {
  int result = score - penalty;
  return (result < 0) ? 0 : result;
}

const char* stateName() {
  switch (gameState) {
    case RUNNING: return "running";
    case SETTLED: return "settled";
    default:      return "waiting";
  }
}

unsigned long remainingSeconds() {
  if (gameState == WAITING) return ROUND_SECONDS;
  if (gameState == SETTLED) return 0;

  unsigned long elapsed = millis() - roundStartTime;
  if (elapsed >= ROUND_MS) return 0;
  return (ROUND_MS - elapsed + 999) / 1000;
}

// 藍隊節點是否在線
bool blueNodeOnline() {
  bool seen;
  unsigned long last;
  portENTER_CRITICAL(&nodeMux);
  seen = blueEverSeen;
  last = blueLastSeen;
  portEXIT_CRITICAL(&nodeMux);
  return seen && (millis() - last < NODE_TIMEOUT_MS);
}

// 天秤開關板是否在線
bool switchBoardOnline() {
  bool seen;
  unsigned long last;
  portENTER_CRITICAL(&nodeMux);
  seen = switchEverSeen;
  last = switchLastSeen;
  portEXIT_CRITICAL(&nodeMux);
  return seen && (millis() - last < NODE_TIMEOUT_MS);
}

// 清除所有加倍與按壓狀態（開始、重設、結算時呼叫）
void clearBonusState() {
  portENTER_CRITICAL(&nodeMux);
  pendingBlueGoals = 0;
  pendingPress[0] = 0;
  pendingPress[1] = 0;
  portEXIT_CRITICAL(&nodeMux);

  for (int i = 0; i < 2; i++) {
    pressCount[i] = 0;
    bonusActive[i] = false;
    bonusUntil[i] = 0;
  }
}

// 加倍時間到就關閉
void updateBonus(unsigned long now) {
  for (int i = 0; i < 2; i++) {
    if (bonusActive[i] && (long)(now - bonusUntil[i]) >= 0) {
      bonusActive[i] = false;
      Serial.printf("[天秤] %s 加倍結束\n", i == TEAM_RED ? "紅隊" : "藍隊");
    }
  }
}

// 這一球值幾分：加倍中 = 2，平常 = 1
int goalValue(int team) {
  return bonusActive[team] ? 2 : 1;
}

// 加倍剩餘秒數（沒加倍 = 0）
unsigned long bonusRemaining(int team) {
  if (!bonusActive[team]) return 0;
  long left = (long)(bonusUntil[team] - millis());
  if (left <= 0) return 0;
  return (unsigned long)((left + 999) / 1000);
}

// 記錄一次天秤按壓：累計滿 PRESSES_FOR_BONUS 次就開始加倍 BONUS_MS
void registerPress(int team, unsigned long now) {
  if (bonusActive[team]) return;   // 加倍期間的按壓不計入

  pressCount[team]++;
  Serial.printf("[天秤] %s 按壓 %d/%d\n",
                team == TEAM_RED ? "紅隊" : "藍隊",
                pressCount[team], PRESSES_FOR_BONUS);

  if (pressCount[team] >= PRESSES_FOR_BONUS) {
    pressCount[team] = 0;
    bonusActive[team] = true;
    bonusUntil[team] = now + BONUS_MS;
    Serial.printf("[天秤] ⚡ %s 進球 %lu 秒內算兩倍！\n",
                  team == TEAM_RED ? "紅隊" : "藍隊", BONUS_MS / 1000);
  }
}

// 在 loop() 中處理天秤按壓：只有比賽進行中才有效
void processSwitchPresses() {
  int n[2];
  portENTER_CRITICAL(&nodeMux);
  n[0] = pendingPress[0];
  n[1] = pendingPress[1];
  pendingPress[0] = 0;
  pendingPress[1] = 0;
  portEXIT_CRITICAL(&nodeMux);

  if (gameState != RUNNING) return;

  unsigned long now = millis();
  for (int team = 0; team < 2; team++) {
    for (int k = 0; k < n[team]; k++) {
      registerPress(team, now);
    }
  }
}

// 同步紅隊感測器目前狀態
void syncSensors() {
  redRawState = digitalRead(RED_SENSOR_PIN);
  redStableState = redRawState;
  redStateChangeTime = millis();
  redLocked = (redRawState == LOW);
}

void settleRound() {
  gameState = SETTLED;
  clearBonusState();

  Serial.printf("[結算] 紅隊 %d - %d = %d | 藍隊 %d - %d = %d\n",
                redScore, redPenalty, finalScore(redScore, redPenalty),
                blueScore, bluePenalty, finalScore(blueScore, bluePenalty));
}

// ======================================================
// 6. ESP-NOW 接收（藍隊節點與天秤開關板傳來的訊息）
// ======================================================

// 注意：這個函式在 Wi-Fi 背景工作中執行，不要在裡面做太多事
void handleEspNowPacket(const uint8_t* data, int len) {
  if (len != (int)sizeof(Packet)) return;

  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PKT_MAGIC) return;

  portENTER_CRITICAL_ISR(&nodeMux);
  unsigned long t = millis();

  switch (p.type) {

    // 藍隊感測節點：進球事件／心跳
    case PKT_GOAL:
    case PKT_HEARTBEAT:
      blueLastSeen = t;
      blueEverSeen = true;
      // 流水號與上一次不同才算新的一球（節點重送時流水號相同）
      if (p.type == PKT_GOAL && (int32_t)p.seq != lastBlueSeq) {
        lastBlueSeq = p.seq;
        pendingBlueGoals++;
      }
      break;

    // 天秤開關板：按壓事件／心跳
    case PKT_SW_RED:
    case PKT_SW_BLUE:
    case PKT_SW_HEARTBEAT:
      switchLastSeen = t;
      switchEverSeen = true;
      if (p.type != PKT_SW_HEARTBEAT && (int32_t)p.seq != lastSwitchSeq) {
        lastSwitchSeq = p.seq;
        pendingPress[p.type == PKT_SW_RED ? TEAM_RED : TEAM_BLUE]++;
      }
      break;
  }
  portEXIT_CRITICAL_ISR(&nodeMux);
}

// 不同版本的 Arduino-ESP32 核心，回呼函式的參數不同
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  handleEspNowPacket(data, len);
}
#else
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  handleEspNowPacket(data, len);
}
#endif

// 在 loop() 中處理藍隊進球：只有比賽進行中才加分，其他狀態直接丟棄
void processBlueGoals() {
  int n;
  portENTER_CRITICAL(&nodeMux);
  n = pendingBlueGoals;
  pendingBlueGoals = 0;
  portEXIT_CRITICAL(&nodeMux);

  if (n > 0 && gameState == RUNNING) {
    blueScore += n * goalValue(TEAM_BLUE);
    Serial.printf("[感測器進球] 🔵 藍隊進球（節點）！紅: %d | 藍: %d\n",
                  redScore, blueScore);
  }
}

// ======================================================
// 7. 選手看板網頁（分數＋倒數計時＋加倍提示）
// ======================================================

void handleDisplayPage() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>比賽計分板</title>

<style>
body {
  margin: 0;
  background: #0f172a;
  color: white;
  font-family: Arial, sans-serif;
  text-align: center;
}

/* 最上方：倒數計時與狀態文字 */
.top {
  background: #0f172a;
  padding: 10px 0;
}

#timer {
  font-size: 9vw;
  font-weight: bold;
  line-height: 1.1;
}

#msg {
  font-size: 3.5vw;
  color: #fde047;
  min-height: 1.3em;
}

/* 兩隊並排 */
.board {
  display: flex;
  min-height: calc(100vh - 20vw);
}

.team {
  flex: 1;
  display: flex;
  flex-direction: column;
  justify-content: center;
}

.red  { background: #b91c1c; }
.blue { background: #1d4ed8; }

.name   { font-size: 5vw; font-weight: bold; }
.number { font-size: 25vw; font-weight: bold; line-height: 1; }

/* 剩餘 10 秒內，計時變紅色警示 */
.warn { color: #f87171; }

/* 天秤加倍提示 */
.bonus { font-size: 4vw; font-weight: bold; color: #fde047; min-height: 1.3em; }
</style>
</head>

<body>

<div class="top">
  <div id="timer">2:30</div>
  <div id="msg">等待裁判開始</div>
</div>

<div class="board">

  <div class="team red">
    <div class="name">紅隊</div>
    <div class="bonus" id="redBonus"></div>
    <div class="number" id="red">0</div>
  </div>

  <div class="team blue">
    <div class="name">藍隊</div>
    <div class="bonus" id="blueBonus"></div>
    <div class="number" id="blue">0</div>
  </div>

</div>

<script>

// 把秒數轉成 m:ss 格式
function fmt(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

// 顯示加倍提示（秒數 > 0 才顯示）
function setBonus(id, s) {
  document.getElementById(id).innerText = (s > 0) ? ('⚡ 分數 ×2！ ' + s + ' 秒') : '';
}

async function update() {
  try {
    const res = await fetch('/api/score');
    const d = await res.json();

    document.getElementById('red').innerText = d.red;
    document.getElementById('blue').innerText = d.blue;
    setBonus('redBonus', d.redBonus);
    setBonus('blueBonus', d.blueBonus);
    document.getElementById('timer').innerText = fmt(d.remaining);

    // 剩餘 10 秒內變紅色
    document.getElementById('timer').className =
      (d.state === 'running' && d.remaining <= 10) ? 'warn' : '';

    // 依狀態顯示提示文字
    const msg = document.getElementById('msg');
    if (d.state === 'waiting') msg.innerText = '等待裁判開始';
    else if (d.state === 'settled') msg.innerText = '時間到｜最終結果';
    else msg.innerText = '';

  } catch (e) {
    // 連線失敗時保持原畫面，下次再試
  }
}

setInterval(update, 300);
update();

</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

// ======================================================
// 8. 裁判網頁（含節點／開關板連線狀態、天秤按壓與加倍資訊）
// ======================================================

void handleRefereePage() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>裁判控制台</title>

<style>
body {
  font-family: Arial, sans-serif;
  text-align: center;
  background: #f1f5f9;
  margin: 0;
  padding: 15px;
}

.container {
  max-width: 520px;
  margin: auto;
  background: white;
  padding: 20px;
  border-radius: 15px;
}

#timer  { font-size: 48px; font-weight: bold; }
#status { margin: 6px 0 4px; font-weight: bold; color: #475569; }

/* 連線狀態 */
#node { margin: 0 0 12px; font-size: 14px; font-weight: bold; }
.nodeOk  { color: #16a34a; }
.nodeBad { color: #dc2626; }

.teams { display: flex; gap: 12px; margin: 15px 0; }

.card { flex: 1; padding: 15px 8px; border-radius: 12px; }
.card.red  { background: #fee2e2; color: #b91c1c; }
.card.blue { background: #dbeafe; color: #1d4ed8; }

.title { font-size: 20px; font-weight: bold; }
.label { font-size: 14px; margin-top: 10px; }
.value { font-size: 40px; font-weight: bold; }

button {
  padding: 12px 16px;
  margin: 5px 3px;
  border: none;
  border-radius: 8px;
  font-size: 16px;
  color: white;
  cursor: pointer;
}

button:disabled { opacity: 0.35; cursor: not-allowed; }

.penalty { background: #d97706; }
.undo    { background: #64748b; }
.main    { background: #16a34a; width: 100%; font-size: 20px; padding: 16px; }
.reset   { background: #dc2626; width: 100%; }
</style>
</head>

<body>
<div class="container">

<h2>⚖️ 裁判控制台</h2>

<div id="timer">2:30</div>
<div id="status">等待開始</div>
<div id="node" class="nodeBad">藍隊感測節點：確認中…</div>
<div id="switchStatus" class="nodeBad" style="margin:0 0 6px;font-size:14px;font-weight:bold;">天秤開關板：確認中…</div>
<div id="bonusInfo" style="margin:0 0 12px;font-size:14px;font-weight:bold;"></div>

<div class="teams">

  <div class="card red">
    <div class="title">紅隊</div>
    <div class="label">目前得分</div>
    <div class="value" id="redScore">0</div>
    <div class="label">罰分次數</div>
    <div class="value" id="redPenalty">0</div>
    <button class="penalty" id="redAdd" onclick="penalty('red', 1)">＋ 罰分</button>
    <button class="undo" id="redUndo" onclick="penalty('red', -1)">撤銷 1 次</button>
  </div>

  <div class="card blue">
    <div class="title">藍隊</div>
    <div class="label">目前得分</div>
    <div class="value" id="blueScore">0</div>
    <div class="label">罰分次數</div>
    <div class="value" id="bluePenalty">0</div>
    <button class="penalty" id="blueAdd" onclick="penalty('blue', 1)">＋ 罰分</button>
    <button class="undo" id="blueUndo" onclick="penalty('blue', -1)">撤銷 1 次</button>
  </div>

</div>

<!-- 主按鈕：依狀態切換「開始比賽／提前結算／開始下一輪」 -->
<button class="main" id="mainBtn" onclick="mainAction()">開始比賽</button>

<hr>
<button class="reset" onclick="resetAll()">重設全部（回到等待開始）</button>

</div>

<script>

let state = 'waiting';  // 目前狀態

function fmt(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

async function update() {
  try {
    const res = await fetch('/api/state');
    const d = await res.json();

    state = d.state;

    document.getElementById('timer').innerText = fmt(d.remaining);
    document.getElementById('redScore').innerText = d.red;
    document.getElementById('blueScore').innerText = d.blue;
    document.getElementById('redPenalty').innerText = d.redPenalty;
    document.getElementById('bluePenalty').innerText = d.bluePenalty;

    // 藍隊節點連線狀態
    const node = document.getElementById('node');
    if (d.blueNode) {
      node.innerText = '藍隊感測節點：🟢 已連線';
      node.className = 'nodeOk';
    } else {
      node.innerText = '藍隊感測節點：🔴 離線（藍隊暫時無法計分）';
      node.className = 'nodeBad';
    }

    // 天秤開關板連線狀態與加倍資訊
    const sw = document.getElementById('switchStatus');
    if (d.switchBoard) {
      sw.innerText = '天秤開關板：🟢 已連線';
      sw.className = 'nodeOk';
    } else {
      sw.innerText = '天秤開關板：🔴 離線（加倍功能暫停）';
      sw.className = 'nodeBad';
    }
    document.getElementById('bonusInfo').innerText =
      '🔴 ' + (d.redBonus > 0 ? '×2 剩 ' + d.redBonus + ' 秒' : '天秤 ' + d.redPress + '/' + d.pressNeeded) +
      '　🔵 ' + (d.blueBonus > 0 ? '×2 剩 ' + d.blueBonus + ' 秒' : '天秤 ' + d.bluePress + '/' + d.pressNeeded);

    // 只有「進行中」才能記錄罰分
    const running = (state === 'running');
    ['redAdd', 'redUndo', 'blueAdd', 'blueUndo'].forEach(id => {
      document.getElementById(id).disabled = !running;
    });

    // 依狀態切換主按鈕文字與狀態說明
    const btn = document.getElementById('mainBtn');
    const status = document.getElementById('status');

    if (state === 'waiting') {
      btn.innerText = '開始比賽';
      status.innerText = '等待開始';
    } else if (state === 'running') {
      btn.innerText = '提前結算';
      status.innerText = '比賽進行中';
    } else {
      btn.innerText = '開始下一輪';
      status.innerText = '已結算｜最終 紅 ' + d.redFinal + ' : ' + d.blueFinal + ' 藍';
    }

  } catch (e) {
    document.getElementById('status').innerText = '連線錯誤';
  }
}

// 主按鈕：依目前狀態執行對應動作
async function mainAction() {
  if (state === 'running') {
    if (!confirm('確定要提前結算嗎？')) return;
    await fetch('/api/settle', { method: 'POST' });
  } else {
    // 等待中或已結算：開始（下一）輪，會清空分數與罰分
    await fetch('/api/start', { method: 'POST' });
  }
  update();
}

// 增加（delta = 1）或撤銷（delta = -1）罰分次數，不會立刻扣分
async function penalty(team, delta) {
  await fetch('/api/penalty?team=' + team + '&delta=' + delta, { method: 'POST' });
  update();
}

// 重設全部，回到等待開始
async function resetAll() {
  if (!confirm('確定要重設全部嗎？')) return;
  await fetch('/api/reset', { method: 'POST' });
  update();
}

setInterval(update, 500);
update();

</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

// ======================================================
// 9. API：選手看板用
// ======================================================

void handleScoreApi() {

  bool settled = (gameState == SETTLED);
  int r = settled ? finalScore(redScore, redPenalty)  : redScore;
  int b = settled ? finalScore(blueScore, bluePenalty) : blueScore;

  String json = "{";
  json += "\"red\":" + String(r) + ",";
  json += "\"blue\":" + String(b) + ",";
  json += "\"state\":\"" + String(stateName()) + "\",";
  json += "\"remaining\":" + String(remainingSeconds()) + ",";
  json += "\"redBonus\":" + String(bonusRemaining(TEAM_RED)) + ",";
  json += "\"blueBonus\":" + String(bonusRemaining(TEAM_BLUE));
  json += "}";

  server.send(200, "application/json", json);
}

// ======================================================
// 10. API：裁判用（完整狀態）
// ======================================================

void handleStateApi() {

  String json = "{";
  json += "\"red\":" + String(redScore) + ",";
  json += "\"blue\":" + String(blueScore) + ",";
  json += "\"redPenalty\":" + String(redPenalty) + ",";
  json += "\"bluePenalty\":" + String(bluePenalty) + ",";
  json += "\"redFinal\":" + String(finalScore(redScore, redPenalty)) + ",";
  json += "\"blueFinal\":" + String(finalScore(blueScore, bluePenalty)) + ",";
  json += "\"state\":\"" + String(stateName()) + "\",";
  json += "\"remaining\":" + String(remainingSeconds()) + ",";
  json += "\"blueNode\":" + String(blueNodeOnline() ? "true" : "false") + ",";
  json += "\"switchBoard\":" + String(switchBoardOnline() ? "true" : "false") + ",";
  json += "\"pressNeeded\":" + String(PRESSES_FOR_BONUS) + ",";
  json += "\"redPress\":" + String(pressCount[TEAM_RED]) + ",";
  json += "\"bluePress\":" + String(pressCount[TEAM_BLUE]) + ",";
  json += "\"redBonus\":" + String(bonusRemaining(TEAM_RED)) + ",";
  json += "\"blueBonus\":" + String(bonusRemaining(TEAM_BLUE));
  json += "}";

  server.send(200, "application/json", json);
}

// ======================================================
// 11. API：開始（下一）輪
// ======================================================

void handleStart() {

  if (gameState == RUNNING) {
    server.send(409, "application/json", "{\"error\":\"Already running\"}");
    return;
  }

  redScore = 0;
  blueScore = 0;
  redPenalty = 0;
  bluePenalty = 0;

  clearBonusState();   // 清掉殘留的藍隊進球、天秤按壓與加倍
  syncSensors();
  roundStartTime = millis();
  gameState = RUNNING;

  Serial.printf("[系統通知] 比賽開始！時間 %lu 秒\n", ROUND_SECONDS);

  handleStateApi();
}

// ======================================================
// 12. API：提前結算
// ======================================================

void handleSettle() {

  if (gameState != RUNNING) {
    server.send(409, "application/json", "{\"error\":\"Not running\"}");
    return;
  }

  settleRound();
  handleStateApi();
}

// ======================================================
// 13. API：罰分（只記錄次數，不立刻扣分）
// ======================================================

void handlePenalty() {

  if (gameState != RUNNING) {
    server.send(409, "application/json", "{\"error\":\"Not running\"}");
    return;
  }

  if (!server.hasArg("team")) {
    server.send(400, "application/json", "{\"error\":\"Missing team\"}");
    return;
  }

  String team = server.arg("team");

  int delta = server.hasArg("delta") ? server.arg("delta").toInt() : 1;
  if (delta != 1 && delta != -1) {
    server.send(400, "application/json", "{\"error\":\"Invalid delta\"}");
    return;
  }

  if (team == "red") {
    redPenalty += delta;
    if (redPenalty < 0) redPenalty = 0;
  } else if (team == "blue") {
    bluePenalty += delta;
    if (bluePenalty < 0) bluePenalty = 0;
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid team\"}");
    return;
  }

  Serial.printf("[罰分記錄] 紅隊罰分: %d | 藍隊罰分: %d\n",
                redPenalty, bluePenalty);

  handleStateApi();
}

// ======================================================
// 14. API：重設全部（回到等待開始）
// ======================================================

void handleReset() {

  redScore = 0;
  blueScore = 0;
  redPenalty = 0;
  bluePenalty = 0;
  gameState = WAITING;
  clearBonusState();

  syncSensors();

  Serial.println("[系統通知] 全部資料已重設，等待開始");

  handleStateApi();
}

// ======================================================
// 15. 紅隊感測器處理
// ======================================================

void handleRedSensor(unsigned long now) {

  int reading = digitalRead(RED_SENSOR_PIN);

  if (reading != redRawState) {
    redRawState = reading;
    redStateChangeTime = now;
  }

  unsigned long requiredTime =
    (redRawState == LOW) ? SENSOR_ON_TIME : SENSOR_OFF_TIME;

  if (redRawState != redStableState &&
      now - redStateChangeTime >= requiredTime) {

    redStableState = redRawState;

    if (redStableState == LOW && !redLocked) {

      if (gameState == RUNNING) {
        redScore += goalValue(TEAM_RED);
        Serial.printf("[感測器進球] 🔴 紅隊進球！紅: %d | 藍: %d\n",
                      redScore, blueScore);
      }
      redLocked = true;
    }

    if (redStableState == HIGH) {
      redLocked = false;
    }
  }
}

// ======================================================
// 16. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(RED_SENSOR_PIN, INPUT);
  syncSensors();

  // 必須是 STATION 模式，ESP-NOW 才能和 Wi-Fi 同時使用
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // 關閉省電模式，否則可能漏收 ESP-NOW 封包

  // 先印出 MAC 位址，填進節點與開關板程式的 MASTER_MAC
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("\n[重要] 主機 MAC 位址：%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[重要] 填入節點：{0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // 連接 Wi-Fi（最多等 15 秒）
  WiFi.begin(ssid, password);
  Serial.print("正在連接 Wi-Fi 網路");

  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStartTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[系統通知] Wi-Fi 連接成功！");
    Serial.print("選手看板：http://");
    Serial.println(WiFi.localIP());
    Serial.print("裁判頁面：http://");
    Serial.print(WiFi.localIP());
    Serial.println("/referee");
    Serial.printf("Wi-Fi 頻道：%d（節點會自動搜尋這個頻道）\n", WiFi.channel());
  } else {
    Serial.println("\n[錯誤] Wi-Fi 連接失敗！");
  }

  // 初始化 ESP-NOW（必須在 Wi-Fi 連線之後，這樣才會使用路由器的頻道）
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[系統通知] ESP-NOW 已啟動，等待藍隊節點與天秤開關板");
  } else {
    Serial.println("[錯誤] ESP-NOW 初始化失敗！");
  }

  // 網頁
  server.on("/", HTTP_GET, handleDisplayPage);
  server.on("/referee", HTTP_GET, handleRefereePage);

  // API
  server.on("/api/score", HTTP_GET, handleScoreApi);
  server.on("/api/state", HTTP_GET, handleStateApi);
  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/settle", HTTP_POST, handleSettle);
  server.on("/api/penalty", HTTP_POST, handlePenalty);
  server.on("/api/reset", HTTP_POST, handleReset);

  server.begin();
  Serial.println("[系統通知] 網頁伺服器已啟動！等待裁判開始比賽");
}

// ======================================================
// 17. 主迴圈 loop()
// ======================================================

void loop() {

  server.handleClient();

  unsigned long now = millis();

  updateBonus(now);       // 加倍時間到就關閉

  // 時間到 -> 自動結算
  if (gameState == RUNNING && now - roundStartTime >= ROUND_MS) {
    Serial.println("[系統通知] 時間到，自動結算！");
    settleRound();
  }

  handleRedSensor(now);   // 紅隊感測器（本機）
  processBlueGoals();     // 藍隊進球（來自節點）
  processSwitchPresses(); // 天秤按壓（來自開關板）
}
