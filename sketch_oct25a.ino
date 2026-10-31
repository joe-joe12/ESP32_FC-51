/*
 * ESP32 + FC-51 雙隊自動計分系統（v3：計時自動結算版）
 *
 * 網頁說明：
 *  /          選手看板：只顯示分數與倒數計時
 *  /referee   裁判頁面：開始比賽、記錄罰分、提前結算、重設
 *
 * 比賽流程（三種狀態）：
 *  1. 等待中（WAITING） ：尚未開始，感測器不計分，等裁判按「開始比賽」
 *  2. 進行中（RUNNING） ：倒數 150 秒，感測器計分，裁判可記錄罰分
 *  3. 已結算（SETTLED） ：時間到自動結算（也可提前結算）
 *                         最終分數 = 感測器得分 - 罰分次數（最低為 0）
 *                         裁判再按一次「開始下一輪」，才會清空並開始新的一輪
 *
 * 接線：
 *  FC-51 VCC -> ESP32 3V3
 *  FC-51 GND -> ESP32 GND
 *  FC-51 OUT -> GPIO 18（紅隊）／GPIO 19（藍隊）
 */

#include <WiFi.h>
#include <WebServer.h>

// ======================================================
// 1. Wi-Fi 設定
// ======================================================

const char* ssid = "QQQ";
const char* password = "QQQ";

// ======================================================
// 2. 腳位與比賽設定
// ======================================================

#define RED_SENSOR_PIN  18   // 紅隊感測器 OUT
#define BLUE_SENSOR_PIN 19   // 藍隊感測器 OUT

// 每一輪的比賽時間（秒），想改時間只要改這裡
const unsigned long ROUND_SECONDS = 150;
const unsigned long ROUND_MS = ROUND_SECONDS * 1000UL;

// ======================================================
// 3. 全域變數
// ======================================================

WebServer server(80);

// 比賽狀態
enum GameState { WAITING, RUNNING, SETTLED };
GameState gameState = WAITING;

unsigned long roundStartTime = 0;   // 本輪開始的時間（millis）

int redScore = 0;       // 紅隊感測器得分
int blueScore = 0;      // 藍隊感測器得分
int redPenalty = 0;     // 紅隊累計罰分次數（結算時才扣除）
int bluePenalty = 0;    // 藍隊累計罰分次數（結算時才扣除）

// 感測器訊號需穩定多久才算有效（單位：毫秒）
const unsigned long SENSOR_ON_TIME = 5;     // 偵測到球
const unsigned long SENSOR_OFF_TIME = 100;  // 球離開（避免彈跳重複計分）

// ---- 紅隊感測器狀態 ----
int redRawState = HIGH;
int redStableState = HIGH;
unsigned long redStateChangeTime = 0;
bool redLocked = false;

// ---- 藍隊感測器狀態 ----
int blueRawState = HIGH;
int blueStableState = HIGH;
unsigned long blueStateChangeTime = 0;
bool blueLocked = false;

// ======================================================
// 4. 工具函式
// ======================================================

// 計算最終分數：得分 - 罰分，最低為 0
int finalScore(int score, int penalty) {
  int result = score - penalty;
  return (result < 0) ? 0 : result;
}

// 把狀態轉成文字，給網頁判斷用
const char* stateName() {
  switch (gameState) {
    case RUNNING: return "running";
    case SETTLED: return "settled";
    default:      return "waiting";
  }
}

// 剩餘秒數（無條件進位）
// 等待中顯示完整時間，已結算顯示 0
unsigned long remainingSeconds() {
  if (gameState == WAITING) return ROUND_SECONDS;
  if (gameState == SETTLED) return 0;

  unsigned long elapsed = millis() - roundStartTime;
  if (elapsed >= ROUND_MS) return 0;
  return (ROUND_MS - elapsed + 999) / 1000;
}

// 同步感測器目前狀態，避免開始瞬間球正好擋在感測器前而誤觸發
void syncSensors() {
  redRawState = digitalRead(RED_SENSOR_PIN);
  redStableState = redRawState;
  redStateChangeTime = millis();
  redLocked = (redRawState == LOW);

  blueRawState = digitalRead(BLUE_SENSOR_PIN);
  blueStableState = blueRawState;
  blueStateChangeTime = millis();
  blueLocked = (blueRawState == LOW);
}

// 結算：切換為已結算狀態（時間到或裁判提前結算都會呼叫）
void settleRound() {
  gameState = SETTLED;

  Serial.printf("[結算] 紅隊 %d - %d = %d | 藍隊 %d - %d = %d\n",
                redScore, redPenalty, finalScore(redScore, redPenalty),
                blueScore, bluePenalty, finalScore(blueScore, bluePenalty));
}

// ======================================================
// 5. 選手看板網頁（分數＋倒數計時）
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
    <div class="number" id="red">0</div>
  </div>

  <div class="team blue">
    <div class="name">藍隊</div>
    <div class="number" id="blue">0</div>
  </div>

</div>

<script>

// 把秒數轉成 m:ss 格式
function fmt(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

async function update() {
  try {
    const res = await fetch('/api/score');
    const d = await res.json();

    document.getElementById('red').innerText = d.red;
    document.getElementById('blue').innerText = d.blue;
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
// 6. 裁判網頁
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
#status { margin: 6px 0 12px; font-weight: bold; color: #475569; }

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
// 7. API：選手看板用
// ======================================================

// 未結算：回傳感測器原始得分；已結算：回傳扣除罰分後的最終分數
// 格式：{"red":3,"blue":5,"state":"running","remaining":120}
void handleScoreApi() {

  bool settled = (gameState == SETTLED);
  int r = settled ? finalScore(redScore, redPenalty)  : redScore;
  int b = settled ? finalScore(blueScore, bluePenalty) : blueScore;

  String json = "{";
  json += "\"red\":" + String(r) + ",";
  json += "\"blue\":" + String(b) + ",";
  json += "\"state\":\"" + String(stateName()) + "\",";
  json += "\"remaining\":" + String(remainingSeconds());
  json += "}";

  server.send(200, "application/json", json);
}

// ======================================================
// 8. API：裁判用（完整狀態）
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
  json += "\"remaining\":" + String(remainingSeconds());
  json += "}";

  server.send(200, "application/json", json);
}

// ======================================================
// 9. API：開始（下一）輪
// ======================================================

// 等待中或已結算時才可開始；會清空分數與罰分並開始倒數
void handleStart() {

  if (gameState == RUNNING) {
    server.send(409, "application/json", "{\"error\":\"Already running\"}");
    return;
  }

  redScore = 0;
  blueScore = 0;
  redPenalty = 0;
  bluePenalty = 0;

  syncSensors();                 // 同步感測器狀態，避免誤觸發
  roundStartTime = millis();     // 開始計時
  gameState = RUNNING;

  Serial.printf("[系統通知] 比賽開始！時間 %lu 秒\n", ROUND_SECONDS);

  handleStateApi();
}

// ======================================================
// 10. API：提前結算
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
// 11. API：罰分（只記錄次數，不立刻扣分）
// ======================================================

// 用法：POST /api/penalty?team=red&delta=1   （增加一次罰分）
//       POST /api/penalty?team=red&delta=-1  （撤銷一次罰分）
void handlePenalty() {

  // 只有比賽進行中才能修改罰分
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
// 12. API：重設全部（回到等待開始）
// ======================================================

void handleReset() {

  redScore = 0;
  blueScore = 0;
  redPenalty = 0;
  bluePenalty = 0;
  gameState = WAITING;

  syncSensors();

  Serial.println("[系統通知] 全部資料已重設，等待開始");

  handleStateApi();
}

// ======================================================
// 13. 紅隊感測器處理
// ======================================================

void handleRedSensor(unsigned long now) {

  int reading = digitalRead(RED_SENSOR_PIN);

  // 訊號變化：記錄新狀態並重新計時（防彈跳）
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

      // 只有比賽進行中才加分
      if (gameState == RUNNING) {
        redScore++;
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
// 14. 藍隊感測器處理
// ======================================================

void handleBlueSensor(unsigned long now) {

  int reading = digitalRead(BLUE_SENSOR_PIN);

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

      if (gameState == RUNNING) {
        blueScore++;
        Serial.printf("[感測器進球] 🔵 藍隊進球！紅: %d | 藍: %d\n",
                      redScore, blueScore);
      }
      blueLocked = true;
    }

    if (blueStableState == HIGH) {
      blueLocked = false;
    }
  }
}

// ======================================================
// 15. 初始化 setup()
// ======================================================

void setup() {

  Serial.begin(115200);

  pinMode(RED_SENSOR_PIN, INPUT);
  pinMode(BLUE_SENSOR_PIN, INPUT);

  syncSensors();  // 讀取感測器初始狀態

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
  } else {
    Serial.println("\n[錯誤] Wi-Fi 連接失敗！");
  }

  // 網頁
  server.on("/", HTTP_GET, handleDisplayPage);
  server.on("/referee", HTTP_GET, handleRefereePage);

  // API（裁判的操作只開放 POST，避免誤觸）
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
// 16. 主迴圈 loop()
// ======================================================

void loop() {

  server.handleClient();       // 處理網頁請求

  unsigned long now = millis();

  // 計時檢查：比賽進行中且時間到 -> 自動結算，並停在結算狀態等裁判開始下一輪
  if (gameState == RUNNING && now - roundStartTime >= ROUND_MS) {
    Serial.println("[系統通知] 時間到，自動結算！");
    settleRound();
  }

  handleRedSensor(now);        // 紅隊感測器
  handleBlueSensor(now);       // 藍隊感測器
}
