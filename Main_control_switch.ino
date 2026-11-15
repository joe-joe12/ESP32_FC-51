/*
 * ESP32 + FC-51 雙隊自動計分系統（v9：主控 = 天秤開關板 ESP32）
 * 接線：開關 COM -> GND，NO -> GPIO25（紅）／GPIO26（藍），NC 不接
 * 手機連「Score-ESP32」後：選手看板 http://192.168.4.1
 *                          裁判頁面 http://192.168.4.1/referee
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif

// ======================================================
// 1. Wi-Fi 熱點設定
// ======================================================

const char* AP_SSID = "Score-ESP32";
const char* AP_PASSWORD = "12345678";   // 至少 8 個字元，建議自己改掉
const int   AP_CHANNEL = 1;             // 固定頻道：兩個節點會自動搜尋到它

// ======================================================
// 2. 腳位與比賽設定
// ======================================================

#define RED_SWITCH_PIN   25   // 紅隊天秤開關
#define BLUE_SWITCH_PIN  26   // 藍隊天秤開關

// 開機時，各隊實體天秤開關是否啟用（1 = 啟用，0 = 停用）
#define RED_SWITCH_DEFAULT_ENABLED   0
#define BLUE_SWITCH_DEFAULT_ENABLED  1

const unsigned long ROUND_SECONDS = 150;
const unsigned long ROUND_MS = ROUND_SECONDS * 1000UL;

const unsigned long NODE_TIMEOUT_MS = 3000;   // 節點多久沒訊號視為離線

const int PRESSES_FOR_BONUS = 2;              // 天秤累計按幾次觸發加倍
const unsigned long BONUS_MS = 60000;         // 加倍持續時間（60 秒）

const int PARKING_POINTS = 3;                 // 完美停車每次加幾分

const unsigned long SWITCH_PRESS_TIME   = 20; // 開關防彈跳
const unsigned long SWITCH_RELEASE_TIME = 50;

// ======================================================
// 3. ESP-NOW 封包格式（必須與 goal_node.ino 完全一致！）
// ======================================================

#define PKT_MAGIC           0xA5
#define PKT_BLUE_GOAL       1
#define PKT_BLUE_HEARTBEAT  2
#define PKT_RED_GOAL        3
#define PKT_RED_HEARTBEAT   4

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint16_t seq;
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

#define TEAM_RED  0
#define TEAM_BLUE 1

int parkCount[2] = {0, 0};   // 各隊已加幾次完美停車

portMUX_TYPE nodeMux = portMUX_INITIALIZER_UNLOCKED;
volatile int pendingGoals[2] = {0, 0};
volatile unsigned long nodeLastSeen[2] = {0, 0};
volatile bool nodeEverSeen[2] = {false, false};
int32_t lastGoalSeq[2] = {-1, -1};

struct SwitchState {
  uint8_t pin;
  uint8_t team;
  int raw;
  int stable;
  unsigned long changeTime;
  bool locked;
};

SwitchState sw[2] = {
  {RED_SWITCH_PIN,  TEAM_RED,  HIGH, HIGH, 0, false},
  {BLUE_SWITCH_PIN, TEAM_BLUE, HIGH, HIGH, 0, false}
};

bool switchEnabled[2] = {
  (RED_SWITCH_DEFAULT_ENABLED != 0),
  (BLUE_SWITCH_DEFAULT_ENABLED != 0)
};

int  pressCount[2]  = {0, 0};
bool bonusActive[2] = {false, false};
unsigned long bonusUntil[2] = {0, 0};

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

bool nodeOnline(int team) {
  bool seen;
  unsigned long last;
  portENTER_CRITICAL(&nodeMux);
  seen = nodeEverSeen[team];
  last = nodeLastSeen[team];
  portEXIT_CRITICAL(&nodeMux);
  return seen && (millis() - last < NODE_TIMEOUT_MS);
}

// 清除加倍、按壓與未處理進球（不動 switchEnabled 與 parkCount）
void clearBonusState() {
  portENTER_CRITICAL(&nodeMux);
  pendingGoals[0] = 0;
  pendingGoals[1] = 0;
  portEXIT_CRITICAL(&nodeMux);

  for (int i = 0; i < 2; i++) {
    pressCount[i] = 0;
    bonusActive[i] = false;
    bonusUntil[i] = 0;
  }
}

void updateBonus(unsigned long now) {
  for (int i = 0; i < 2; i++) {
    if (bonusActive[i] && (long)(now - bonusUntil[i]) >= 0) {
      bonusActive[i] = false;
      Serial.printf("[天秤] %s 加倍結束\n", i == TEAM_RED ? "紅隊" : "藍隊");
    }
  }
}

int goalValue(int team) {
  return bonusActive[team] ? 2 : 1;
}

unsigned long bonusRemaining(int team) {
  if (!bonusActive[team]) return 0;
  long left = (long)(bonusUntil[team] - millis());
  if (left <= 0) return 0;
  return (unsigned long)((left + 999) / 1000);
}

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

void settleRound() {
  gameState = SETTLED;
  clearBonusState();

  Serial.printf("[結算] 紅隊 %d - %d = %d | 藍隊 %d - %d = %d\n",
                redScore, redPenalty, finalScore(redScore, redPenalty),
                blueScore, bluePenalty, finalScore(blueScore, bluePenalty));
}

// ======================================================
// 6. 天秤開關處理（防彈跳＋壓住只算一次＋可停用）
// ======================================================

void handleSwitch(SwitchState& s, unsigned long now) {

  int reading = digitalRead(s.pin);

  if (reading != s.raw) {
    s.raw = reading;
    s.changeTime = now;
  }

  unsigned long requiredTime =
    (s.raw == LOW) ? SWITCH_PRESS_TIME : SWITCH_RELEASE_TIME;

  if (s.raw != s.stable && now - s.changeTime >= requiredTime) {

    s.stable = s.raw;

    if (s.stable == LOW && !s.locked) {
      if (gameState == RUNNING && switchEnabled[s.team]) {
        registerPress(s.team, now);
      }
      s.locked = true;
    }

    if (s.stable == HIGH) {
      s.locked = false;
    }
  }
}

// ======================================================
// 7. ESP-NOW 接收
// ======================================================

void handleEspNowPacket(const uint8_t* data, int len) {
  if (len != (int)sizeof(Packet)) return;

  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != PKT_MAGIC) return;

  int team;
  bool isGoal;
  switch (p.type) {
    case PKT_RED_GOAL:       team = TEAM_RED;  isGoal = true;  break;
    case PKT_RED_HEARTBEAT:  team = TEAM_RED;  isGoal = false; break;
    case PKT_BLUE_GOAL:      team = TEAM_BLUE; isGoal = true;  break;
    case PKT_BLUE_HEARTBEAT: team = TEAM_BLUE; isGoal = false; break;
    default: return;
  }

  portENTER_CRITICAL_ISR(&nodeMux);
  nodeLastSeen[team] = millis();
  nodeEverSeen[team] = true;
  if (isGoal && (int32_t)p.seq != lastGoalSeq[team]) {
    lastGoalSeq[team] = p.seq;
    pendingGoals[team]++;
  }
  portEXIT_CRITICAL_ISR(&nodeMux);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  handleEspNowPacket(data, len);
}
#else
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  handleEspNowPacket(data, len);
}
#endif

void processGoals() {
  int n[2];
  portENTER_CRITICAL(&nodeMux);
  n[0] = pendingGoals[0];
  n[1] = pendingGoals[1];
  pendingGoals[0] = 0;
  pendingGoals[1] = 0;
  portEXIT_CRITICAL(&nodeMux);

  if (gameState != RUNNING) return;

  if (n[TEAM_RED] > 0) {
    redScore += n[TEAM_RED] * goalValue(TEAM_RED);
    Serial.printf("[感測器進球] 🔴 紅隊進球！紅: %d | 藍: %d\n", redScore, blueScore);
  }
  if (n[TEAM_BLUE] > 0) {
    blueScore += n[TEAM_BLUE] * goalValue(TEAM_BLUE);
    Serial.printf("[感測器進球] 🔵 藍隊進球！紅: %d | 藍: %d\n", redScore, blueScore);
  }
}

// ======================================================
// 8. 選手看板網頁
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

.warn { color: #f87171; }

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

function fmt(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

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

    document.getElementById('timer').className =
      (d.state === 'running' && d.remaining <= 10) ? 'warn' : '';

    const msg = document.getElementById('msg');
    if (d.state === 'waiting') msg.innerText = '等待裁判開始';
    else if (d.state === 'settled') msg.innerText = '時間到｜最終結果';
    else msg.innerText = '';

  } catch (e) {
  }
}

setInterval(update, 300);
update();

</script>

</body>
</html>
)rawliteral";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

// ======================================================
// 9. 裁判網頁
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

.node { margin: 0 0 6px; font-size: 14px; font-weight: bold; }
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

.penalty  { background: #d97706; }
.undo     { background: #64748b; }
.bonusBtn { background: #7c3aed; }
.parkBtn  { background: #0891b2; }
.swBtn    { background: #16a34a; font-size: 14px; }
.main     { background: #16a34a; width: 100%; font-size: 20px; padding: 16px; }
.reset    { background: #dc2626; width: 100%; }
</style>
</head>

<body>
<div class="container">

<h2>⚖️ 裁判控制台</h2>

<div id="timer">2:30</div>
<div id="status">等待開始</div>
<div id="redNode" class="node nodeBad">紅隊感測節點：確認中…</div>
<div id="blueNode" class="node nodeBad">藍隊感測節點：確認中…</div>
<div id="bonusInfo" style="margin:6px 0 12px;font-size:14px;font-weight:bold;"></div>

<div class="teams">

  <div class="card red">
    <div class="title">紅隊</div>
    <div class="label">目前得分</div>
    <div class="value" id="redScore">0</div>
    <div class="label">罰分次數</div>
    <div class="value" id="redPenalty">0</div>
    <button class="penalty" id="redAdd" onclick="penalty('red', 1)">＋ 罰分</button>
    <button class="undo" id="redUndo" onclick="penalty('red', -1)">撤銷 1 次</button>
    <hr>
    <div class="label" id="redParkInfo">完美停車：0 次</div>
    <button class="parkBtn" id="redParkAdd" onclick="park('red', 1)">🅿️ 完美停車 +3</button>
    <button class="undo" id="redParkUndo" onclick="park('red', -1)">撤銷停車</button>
    <hr>
    <div class="label">手動加倍</div>
    <button class="bonusBtn" id="redBonusStart" onclick="bonus('red', 'start')">⚡ 加倍 60 秒</button>
    <button class="undo" id="redBonusStop" onclick="bonus('red', 'stop')">取消加倍</button>
    <div class="label">天秤開關</div>
    <button class="swBtn" id="redSwitch" onclick="toggleSwitch('red')">確認中…</button>
  </div>

  <div class="card blue">
    <div class="title">藍隊</div>
    <div class="label">目前得分</div>
    <div class="value" id="blueScore">0</div>
    <div class="label">罰分次數</div>
    <div class="value" id="bluePenalty">0</div>
    <button class="penalty" id="blueAdd" onclick="penalty('blue', 1)">＋ 罰分</button>
    <button class="undo" id="blueUndo" onclick="penalty('blue', -1)">撤銷 1 次</button>
    <hr>
    <div class="label" id="blueParkInfo">完美停車：0 次</div>
    <button class="parkBtn" id="blueParkAdd" onclick="park('blue', 1)">🅿️ 完美停車 +3</button>
    <button class="undo" id="blueParkUndo" onclick="park('blue', -1)">撤銷停車</button>
    <hr>
    <div class="label">手動加倍</div>
    <button class="bonusBtn" id="blueBonusStart" onclick="bonus('blue', 'start')">⚡ 加倍 60 秒</button>
    <button class="undo" id="blueBonusStop" onclick="bonus('blue', 'stop')">取消加倍</button>
    <div class="label">天秤開關</div>
    <button class="swBtn" id="blueSwitch" onclick="toggleSwitch('blue')">確認中…</button>
  </div>

</div>

<button class="main" id="mainBtn" onclick="mainAction()">開始比賽</button>

<hr>
<button class="reset" onclick="resetAll()">重設全部（回到等待開始）</button>

</div>

<script>

let state = 'waiting';
let switchOn = { red: true, blue: true };

function fmt(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

function showNode(id, label, online, offlineNote) {
  const el = document.getElementById(id);
  el.innerText = label + (online ? '：🟢 已連線' : '：🔴 離線（' + offlineNote + '）');
  el.className = 'node ' + (online ? 'nodeOk' : 'nodeBad');
}

function showSwitch(team, enabled) {
  switchOn[team] = enabled;
  const b = document.getElementById(team + 'Switch');
  b.innerText = enabled ? '已啟用（點擊停用）' : '已停用（點擊啟用）';
  b.style.background = enabled ? '#16a34a' : '#64748b';
}

function bonusText(bonus, enabled, press, needed) {
  if (bonus > 0) return '×2 剩 ' + bonus + ' 秒';
  if (!enabled) return '天秤停用';
  return '天秤 ' + press + '/' + needed;
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

    document.getElementById('redParkInfo').innerText =
      '完美停車：' + d.redPark + ' 次（+' + (d.redPark * d.parkPoints) + ' 分）';
    document.getElementById('blueParkInfo').innerText =
      '完美停車：' + d.bluePark + ' 次（+' + (d.bluePark * d.parkPoints) + ' 分）';

    showNode('redNode', '紅隊感測節點', d.redNode, '紅隊暫時無法計分');
    showNode('blueNode', '藍隊感測節點', d.blueNode, '藍隊暫時無法計分');

    showSwitch('red', d.redSwitchOn);
    showSwitch('blue', d.blueSwitchOn);

    document.getElementById('bonusInfo').innerText =
      '🔴 ' + bonusText(d.redBonus, d.redSwitchOn, d.redPress, d.pressNeeded) +
      '　🔵 ' + bonusText(d.blueBonus, d.blueSwitchOn, d.bluePress, d.pressNeeded);

    const running = (state === 'running');
    ['redAdd', 'redUndo', 'blueAdd', 'blueUndo',
     'redBonusStart', 'redBonusStop', 'blueBonusStart', 'blueBonusStop'].forEach(id => {
      document.getElementById(id).disabled = !running;
    });

    const canPark = (state !== 'waiting');
    ['redParkAdd', 'redParkUndo', 'blueParkAdd', 'blueParkUndo'].forEach(id => {
      document.getElementById(id).disabled = !canPark;
    });

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

async function mainAction() {
  if (state === 'running') {
    if (!confirm('確定要提前結算嗎？')) return;
    await fetch('/api/settle', { method: 'POST' });
  } else {
    await fetch('/api/start', { method: 'POST' });
  }
  update();
}

async function penalty(team, delta) {
  await fetch('/api/penalty?team=' + team + '&delta=' + delta, { method: 'POST' });
  update();
}

async function park(team, delta) {
  await fetch('/api/park?team=' + team + '&delta=' + delta, { method: 'POST' });
  update();
}

async function bonus(team, action) {
  await fetch('/api/bonus?team=' + team + '&action=' + action, { method: 'POST' });
  update();
}

async function toggleSwitch(team) {
  await fetch('/api/switch?team=' + team + '&enable=' + (switchOn[team] ? 0 : 1), { method: 'POST' });
  update();
}

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

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

// ======================================================
// 10. API：選手看板用
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

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// ======================================================
// 11. API：裁判用（完整狀態）
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
  json += "\"redNode\":" + String(nodeOnline(TEAM_RED) ? "true" : "false") + ",";
  json += "\"blueNode\":" + String(nodeOnline(TEAM_BLUE) ? "true" : "false") + ",";
  json += "\"pressNeeded\":" + String(PRESSES_FOR_BONUS) + ",";
  json += "\"redPress\":" + String(pressCount[TEAM_RED]) + ",";
  json += "\"bluePress\":" + String(pressCount[TEAM_BLUE]) + ",";
  json += "\"redBonus\":" + String(bonusRemaining(TEAM_RED)) + ",";
  json += "\"blueBonus\":" + String(bonusRemaining(TEAM_BLUE)) + ",";
  json += "\"redSwitchOn\":" + String(switchEnabled[TEAM_RED] ? "true" : "false") + ",";
  json += "\"blueSwitchOn\":" + String(switchEnabled[TEAM_BLUE] ? "true" : "false") + ",";
  json += "\"parkPoints\":" + String(PARKING_POINTS) + ",";
  json += "\"redPark\":" + String(parkCount[TEAM_RED]) + ",";
  json += "\"bluePark\":" + String(parkCount[TEAM_BLUE]);
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// ======================================================
// 12. API：開始（下一）輪
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
  parkCount[0] = 0;
  parkCount[1] = 0;

  clearBonusState();
  roundStartTime = millis();
  gameState = RUNNING;

  Serial.printf("[系統通知] 比賽開始！時間 %lu 秒\n", ROUND_SECONDS);

  handleStateApi();
}

// ======================================================
// 13. API：提前結算
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
// 14. API：罰分（只記錄次數，結算時才扣）
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
// 14-2. API：完美停車（每次 +PARKING_POINTS 分，可撤銷）
// ======================================================

void handlePark() {

  if (gameState == WAITING) {
    server.send(409, "application/json", "{\"error\":\"Not started\"}");
    return;
  }

  if (!server.hasArg("team")) {
    server.send(400, "application/json", "{\"error\":\"Missing team\"}");
    return;
  }

  String teamStr = server.arg("team");
  int team;
  if (teamStr == "red")       team = TEAM_RED;
  else if (teamStr == "blue") team = TEAM_BLUE;
  else {
    server.send(400, "application/json", "{\"error\":\"Invalid team\"}");
    return;
  }

  int delta = server.hasArg("delta") ? server.arg("delta").toInt() : 1;
  if (delta != 1 && delta != -1) {
    server.send(400, "application/json", "{\"error\":\"Invalid delta\"}");
    return;
  }

  int* score = (team == TEAM_RED) ? &redScore : &blueScore;

  if (delta == 1) {
    *score += PARKING_POINTS;
    parkCount[team]++;
  } else {
    if (parkCount[team] <= 0) {
      server.send(409, "application/json", "{\"error\":\"Nothing to undo\"}");
      return;
    }
    parkCount[team]--;
    *score -= PARKING_POINTS;
    if (*score < 0) *score = 0;
  }

  Serial.printf("[完美停車] %s %s %d 分 | 紅: %d（停車 %d 次）| 藍: %d（停車 %d 次）\n",
                team == TEAM_RED ? "紅隊" : "藍隊",
                delta == 1 ? "+" : "撤銷 -", PARKING_POINTS,
                redScore, parkCount[TEAM_RED], blueScore, parkCount[TEAM_BLUE]);

  handleStateApi();
}

// ======================================================
// 15. API：重設全部
// ======================================================

void handleReset() {

  redScore = 0;
  blueScore = 0;
  redPenalty = 0;
  bluePenalty = 0;
  parkCount[0] = 0;
  parkCount[1] = 0;
  gameState = WAITING;
  clearBonusState();

  Serial.println("[系統通知] 全部資料已重設，等待開始");

  handleStateApi();
}

// ======================================================
// 15-2. API：手動開啟／取消加倍
// ======================================================

void handleBonus() {

  if (gameState != RUNNING) {
    server.send(409, "application/json", "{\"error\":\"Not running\"}");
    return;
  }

  if (!server.hasArg("team")) {
    server.send(400, "application/json", "{\"error\":\"Missing team\"}");
    return;
  }

  String teamStr = server.arg("team");
  int team;
  if (teamStr == "red")       team = TEAM_RED;
  else if (teamStr == "blue") team = TEAM_BLUE;
  else {
    server.send(400, "application/json", "{\"error\":\"Invalid team\"}");
    return;
  }

  String action = server.hasArg("action") ? server.arg("action") : "start";

  if (action == "start") {
    bonusActive[team] = true;
    bonusUntil[team] = millis() + BONUS_MS;
    pressCount[team] = 0;
    Serial.printf("[裁判] 手動開啟 %s 加倍 %lu 秒\n",
                  team == TEAM_RED ? "紅隊" : "藍隊", BONUS_MS / 1000);
  } else if (action == "stop") {
    bonusActive[team] = false;
    bonusUntil[team] = 0;
    Serial.printf("[裁判] 手動取消 %s 加倍\n",
                  team == TEAM_RED ? "紅隊" : "藍隊");
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid action\"}");
    return;
  }

  handleStateApi();
}

// ======================================================
// 15-3. API：啟用／停用某隊實體天秤開關
// ======================================================

void handleSwitchEnable() {

  if (!server.hasArg("team") || !server.hasArg("enable")) {
    server.send(400, "application/json", "{\"error\":\"Missing team or enable\"}");
    return;
  }

  String teamStr = server.arg("team");
  bool enable = (server.arg("enable").toInt() != 0);

  int team;
  if (teamStr == "red")       team = TEAM_RED;
  else if (teamStr == "blue") team = TEAM_BLUE;
  else {
    server.send(400, "application/json", "{\"error\":\"Invalid team\"}");
    return;
  }

  switchEnabled[team] = enable;
  if (!enable) pressCount[team] = 0;

  Serial.printf("[裁判] %s天秤開關已%s\n",
                team == TEAM_RED ? "紅隊" : "藍隊", enable ? "啟用" : "停用");

  handleStateApi();
}

// ======================================================
// 16. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(RED_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BLUE_SWITCH_PIN, INPUT_PULLUP);

  for (int i = 0; i < 2; i++) {
    sw[i].raw = digitalRead(sw[i].pin);
    sw[i].stable = sw[i].raw;
    sw[i].changeTime = millis();
    sw[i].locked = (sw[i].raw == LOW);
  }

  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, 8);
  delay(100);

  if (apOk) {
    Serial.println("\n[系統通知] Wi-Fi 熱點已建立！");
    Serial.printf("Wi-Fi 名稱：%s\n", AP_SSID);
    Serial.printf("Wi-Fi 密碼：%s\n", AP_PASSWORD);
    Serial.print("選手看板：http://");
    Serial.println(WiFi.softAPIP());
    Serial.print("裁判頁面：http://");
    Serial.print(WiFi.softAPIP());
    Serial.println("/referee");
  } else {
    Serial.println("\n[錯誤] Wi-Fi 熱點建立失敗！");
  }

  Serial.printf("[天秤] 開機設定：紅隊開關%s，藍隊開關%s\n",
                switchEnabled[TEAM_RED] ? "啟用" : "停用",
                switchEnabled[TEAM_BLUE] ? "啟用" : "停用");

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  Serial.printf("[重要] 主控 MAC 位址：%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[重要] 填入節點：{0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[系統通知] ESP-NOW 已啟動，等待紅隊、藍隊節點");
  } else {
    Serial.println("[錯誤] ESP-NOW 初始化失敗！");
  }

  server.on("/", HTTP_GET, handleDisplayPage);
  server.on("/referee", HTTP_GET, handleRefereePage);

  server.on("/api/score", HTTP_GET, handleScoreApi);
  server.on("/api/state", HTTP_GET, handleStateApi);
  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/settle", HTTP_POST, handleSettle);
  server.on("/api/penalty", HTTP_POST, handlePenalty);
  server.on("/api/park", HTTP_POST, handlePark);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/bonus", HTTP_POST, handleBonus);
  server.on("/api/switch", HTTP_POST, handleSwitchEnable);

  server.begin();
  Serial.println("[系統通知] 網頁伺服器已啟動！等待裁判開始比賽");
}

// ======================================================
// 17. 主迴圈 loop()
// ======================================================

void loop() {

  server.handleClient();

  unsigned long now = millis();

  updateBonus(now);

  if (gameState == RUNNING && now - roundStartTime >= ROUND_MS) {
    Serial.println("[系統通知] 時間到，自動結算！");
    settleRound();
  }

  handleSwitch(sw[0], now);
  handleSwitch(sw[1], now);
  processGoals();
}
