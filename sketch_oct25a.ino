/*
 * ESP32 乒乓球自動計分系統
 */

#include <WiFi.h>       // 引入 ESP32 Wi-Fi 官方函式庫
#include <WebServer.h>  // 引入 ESP32 網頁伺服器函式庫

// ======================================================
// 1. Wi-Fi 無線網路連線設定
// ======================================================
const char* ssid     = "AAAA";    // 請替換為您的 Wi-Fi 名稱
const char* password = "AAAA";  // 請替換為您的 Wi-Fi 密碼

// 建立 WebServer 物件，使用標準 HTTP Port 80
WebServer server(80);

// ======================================================
// 2. 硬體腳位與變數宣告
// ======================================================
const int RED_SENSOR_PIN = 25;   // 紅隊感測器 OUT 接 GPIO 18
const int BLUE_SENSOR_PIN = 26;  // 藍隊感測器 OUT 接 GPIO 19

int redScore = 0;
int blueScore = 0;

int lastRedState = HIGH;
int lastBlueState = HIGH;

// ======================================================
// 3. 網頁 HTML / CSS / JavaScript 介面
// ======================================================
const char HTML_CONTENT[] = 
"<!DOCTYPE html><html lang='zh-TW'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>🏓 乒乓球對抗賽 - 裁判與即時看板</title><style>"
"* { box-sizing: border-box; margin: 0; padding: 0; }"
"body { font-family: sans-serif; background: #121212; color: #fff; text-align: center; padding: 20px; }"
"h1 { font-size: 2rem; margin-bottom: 5px; color: #f1f1f1; }"
"p.subtitle { color: #aaa; margin-bottom: 25px; font-size: 0.95rem; }"
".scoreboard { display: flex; justify-content: center; gap: 20px; max-width: 650px; margin: 0 auto; }"
".card { flex: 1; padding: 25px; border-radius: 20px; box-shadow: 0 10px 20px rgba(0,0,0,0.5); }"
".red { background: linear-gradient(135deg, #ff416c, #ff4b2b); }"
".blue { background: linear-gradient(135deg, #2193b0, #6dd5ed); }"
".team-title { font-size: 1.5rem; font-weight: bold; margin-bottom: 15px; }"
".score { font-size: 5rem; font-weight: 800; text-shadow: 2px 4px 10px rgba(0,0,0,0.3); }"
".btn-group { display: flex; justify-content: center; gap: 10px; margin-top: 15px; }"
".btn-minus { background-color: rgba(0, 0, 0, 0.4); color: white; border: 2px solid rgba(255,255,255,0.8); padding: 8px 16px; font-size: 0.9rem; font-weight: bold; border-radius: 12px; cursor: pointer; transition: 0.2s; }"
".btn-minus:hover { background-color: rgba(0, 0, 0, 0.7); transform: scale(1.05); }"
".btn-container { margin-top: 30px; }"
".btn-reset { background-color: #e50914; color: white; border: none; padding: 12px 30px; font-size: 1.1rem; font-weight: bold; border-radius: 30px; cursor: pointer; transition: 0.2s; }"
".btn-reset:hover { background-color: #ff1e27; transform: scale(1.05); }"
"</style></head><body>"
"<h1>🏓 乒乓球自動計分系統</h1>"
"<p class='subtitle'>🔒 得分由 FC-51 感測器自動判定 | 網頁僅提供裁判違規扣分與重置</p>"
"<div class='scoreboard'>"
"<div class='card red'><div class='team-title'>🔴 紅隊</div><div class='score' id='red-score'>0</div><div class='btn-group'><button class='btn-minus' onclick=\"penalty('red')\">⚠️ 違規扣 1 分</button></div></div>"
"<div class='card blue'><div class='team-title'>🔵 藍隊</div><div class='score' id='blue-score'>0</div><div class='btn-group'><button class='btn-minus' onclick=\"penalty('blue')\">⚠️ 違規扣 1 分</button></div></div>"
"</div>"
"<div class='btn-container'><button class='btn-reset' onclick='resetScore()'>重新開始 (Reset)</button></div>"
"<script>"
"setInterval(fetchScore, 300);"
"function fetchScore() { fetch('/api/score').then(r => r.json()).then(d => { document.getElementById('red-score').innerText = d.red; document.getElementById('blue-score').innerText = d.blue; }); }"
"function penalty(t) { fetch('/api/penalty?team=' + t, { method: 'POST' }).then(r => r.json()).then(d => { document.getElementById('red-score').innerText = d.red; document.getElementById('blue-score').innerText = d.blue; }); }"
"function resetScore() { fetch('/api/reset', { method: 'POST' }).then(r => r.json()).then(d => { document.getElementById('red-score').innerText = d.red; document.getElementById('blue-score').innerText = d.blue; }); }"
"</script></body></html>";

// ======================================================
// 4. HTTP API 路由處理副程式
// ======================================================
void handleRoot() {
  server.send(200, "text/html", HTML_CONTENT);
}

void handleGetScore() {
  String json = "{\"red\":" + String(redScore) + ",\"blue\":" + String(blueScore) + "}";
  server.send(200, "application/json", json);
}

void handlePenalty() {
  if (server.hasArg("team")) {
    String team = server.arg("team");
    if (team == "red") {
      if (redScore > 0) redScore--;
      Serial.println("[裁判操作] 紅隊違規，扣 1 分！");
    } else if (team == "blue") {
      if (blueScore > 0) blueScore--;
      Serial.println("[裁判操作] 藍隊違規，扣 1 分！");
    }
  }
  handleGetScore();
}

void handleResetScore() {
  redScore = 0;
  blueScore = 0;
  Serial.println("[裁判操作] 比分已成功重置歸零！");
  handleGetScore();
}

// ======================================================
// 5. 初始化設定 (setup)
// ======================================================
void setup() {
  Serial.begin(115200);

  pinMode(RED_SENSOR_PIN, INPUT);
  pinMode(BLUE_SENSOR_PIN, INPUT);

  WiFi.begin(ssid, password);
  Serial.print("正在連接 Wi-Fi 網路");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[系統通知] Wi-Fi 連接成功！");
  Serial.print("👉 請在瀏覽器輸入本機 IP 位址： http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/score", HTTP_GET, handleGetScore);
  server.on("/api/penalty", HTTP_POST, handlePenalty);
  server.on("/api/penalty", HTTP_GET, handlePenalty);
  server.on("/api/reset", HTTP_POST, handleResetScore);
  server.on("/api/reset", HTTP_GET, handleResetScore);
  
  server.begin();
  Serial.println("[系統通知] 網頁伺服器已成功啟動！");
}

// ======================================================
// 6. 主程式重覆執行迴圈 (loop)
// ======================================================
void loop() {
  server.handleClient();

  int currentRedState = digitalRead(RED_SENSOR_PIN);
  int currentBlueState = digitalRead(BLUE_SENSOR_PIN);

  if (lastRedState == HIGH && currentRedState == LOW) {
    redScore++;
    Serial.printf("[感測器進球] 🔴 紅隊進球！目前比分 - 紅: %d | 藍: %d\n", redScore, blueScore);
    delay(200);
  }

  if (lastBlueState == HIGH && currentBlueState == LOW) {
    blueScore++;
    Serial.printf("[感測器進球] 🔵 藍隊進球！目前比分 - 紅: %d | 藍: %d\n", redScore, blueScore);
    delay(200);
  }

  lastRedState = currentRedState;
  lastBlueState = currentBlueState;
}
