/*
 * 1. 自動加分：只能透過 FC-51 紅外線感測器偵測乒乓球通過
 * 2. 違規扣分：裁判可透過手機網頁進行手動違規扣分
 * 3. 網頁看板：ESP32 建立 Web Server，即時呈現紅藍比分並提供重置與扣分操作。
 */

#include <WiFi.h>       // 引入 ESP32 Wi-Fi 官方函式庫
#include <WebServer.h>  // 引入 ESP32 網頁伺服器函式庫

// ======================================================
// 1. Wi-Fi 無線網路連線設定
// ======================================================
const char* ssid     = "YOUR_WIFI_SSID";      // 請替換為您的 Wi-Fi 名稱
const char* password = "YOUR_WIFI_PASSWORD";  // 請替換為您的 Wi-Fi 密碼

// 建立 WebServer 物件，使用標準 HTTP Port 80
WebServer server(80);

// ======================================================
// 2. 硬體腳位與變數宣告
// ======================================================
// 定義 FC-51 感測器 OUT 訊號線連到的 ESP32 GPIO 腳位
const int RED_SENSOR_PIN = 18;   // 紅隊感測器 OUT 接 GPIO 18
const int BLUE_SENSOR_PIN = 19;  // 藍隊感測器 OUT 接 GPIO 19

// 記錄兩隊的分數變數
int redScore = 0;
int blueScore = 0;

// 記錄感測器上一次的讀取狀態（HIGH 代表無球遮檔，LOW 代表球體經過）
int lastRedState = HIGH;
int lastBlueState = HIGH;

// ======================================================
// 3. 網頁 HTML / CSS / JavaScript 介面 (存放在 Flash 記憶體中)
// ======================================================
const char HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>🏓 乒乓球對抗賽 - 裁判與即時看板</title>
  <style>
    /* CSS 全局美化與佈局設定 */
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #121212; color: #fff; text-align: center; padding: 20px; }
    h1 { font-size: 2rem; margin-bottom: 5px; color: #f1f1f1; }
    p.subtitle { color: #aaa; margin-bottom: 25px; font-size: 0.95rem; }
    
    /* 計分板容器（卡片並排） */
    .scoreboard { display: flex; justify-content: center; gap: 20px; max-width: 650px; margin: 0 auto; }
    .card { flex: 1; padding: 25px; border-radius: 20px; box-shadow: 0 10px 20px rgba(0,0,0,0.5); }
    .red { background: linear-gradient(135deg, #ff416c, #ff4b2b); }   /* 紅隊漸層色 */
    .blue { background: linear-gradient(135deg, #2193b0, #6dd5ed); }  /* 藍隊漸層色 */
    .team-title { font-size: 1.5rem; font-weight: bold; margin-bottom: 15px; }
    .score { font-size: 5rem; font-weight: 800; text-shadow: 2px 4px 10px rgba(0,0,0,0.3); }
    
    /* 扣分按鈕樣式 */
    .btn-group { display: flex; justify-content: center; gap: 10px; margin-top: 15px; }
    .btn-minus { background-color: rgba(0, 0, 0, 0.4); color: white; border: 2px solid rgba(255,255,255,0.8); padding: 8px 16px; font-size: 0.9rem; font-weight: bold; border-radius: 12px; cursor: pointer; transition: 0.2s; }
    .btn-minus:hover { background-color: rgba(0, 0, 0, 0.7); transform: scale(1.05); }
    .btn-minus:active { transform: scale(0.95); }
    
    /* 重新開始按鈕樣式 */
    .btn-container { margin-top: 30px; }
    .btn-reset { background-color: #e50914; color: white; border: none; padding: 12px 30px; font-size: 1.1rem; font-weight: bold; border-radius: 30px; cursor: pointer; transition: 0.2s; box-shadow: 0 4px 10px rgba(229,9,20,0.4); }
    .btn-reset:hover { background-color: #ff1e27; transform: scale(1.05); }
    .btn-reset:active { transform: scale(0.95); }
  </style>
</head>
<body>
  <h1>🏓 乒乓球自動計分系統</h1>
  <p class="subtitle">🔒 得分由 FC-51 感測器自動判定 | 網頁僅提供裁判違規扣分與重置</p>

  <!-- 即時計分卡片區 -->
  <div class="scoreboard">
    <!-- 紅隊卡片 -->
    <div class="card red">
      <div class="team-title">🔴 紅隊</div>
      <div class="score" id="red-score">0</div>
      <div class="btn-group">
        <button class="btn-minus" onclick="penalty('red')">⚠️ 違規扣 1 分</button>
      </div>
    </div>
    
    <!-- 藍隊卡片 -->
    <div class="card blue">
      <div class="team-title">🔵 藍隊</div>
      <div class="score" id="blue-score">0</div>
      <div class="btn-group">
        <button class="btn-minus" onclick="penalty('blue')">⚠️ 違規扣 1 分</button>
      </div>
    </div>
  </div>

  <!-- 重置比分按鈕區 -->
  <div class="btn-container">
    <button class="btn-reset" onclick="resetScore()">重新開始 (Reset)</button>
  </div>

  <!-- 前端 JavaScript 腳本：負責即時向 ESP32 查詢分數與發送指令 -->
  <script>
    // 設定定時器：每 300 毫秒 (0.3 秒) 自動向 ESP32 發送 API 請求最新分數
    setInterval(fetchScore, 300);

    // 取得最新分數的函式
    function fetchScore() {
      fetch('/api/score')
        .then(res => res.json())
        .then(data => {
          // 更新 DOM 畫面的分數文字
          document.getElementById('red-score').innerText = data.red;
          document.getElementById('blue-score').innerText = data.blue;
        })
        .catch(err => console.error(err));
    }

    // 發送扣分請求的函式
    function penalty(team) {
      fetch('/api/penalty?team=' + team, { method: 'POST' })
        .then(res => res.json())
        .then(data => {
          document.getElementById('red-score').innerText = data.red;
          document.getElementById('blue-score').innerText = data.blue;
        });
    }

    // 發送重置（歸零）請求的函式
    function resetScore() {
      fetch('/api/reset', { method: 'POST' })
        .then(res => res.json())
        .then(data => {
          document.getElementById('red-score').innerText = data.red;
          document.getElementById('blue-score').innerText = data.blue;
        });
    }
  </script>
</body>
</html>
)rawliteral";

// ======================================================
// 4. HTTP API 路由處理副程式 (Web Server Routes)
// ======================================================

// [路由 1] 當使用者訪問首頁 (http://IP/) 時，傳送 HTML 網頁
void handleRoot() {
  server.send(200, "text/html", HTML_CONTENT);
}

// [路由 2] 傳回當前分數 JSON 格式 (/api/score)
void handleGetScore() {
  String json = "{\"red\":" + String(redScore) + ",\"blue\":" + String(blueScore) + "}";
  server.send(200, "application/json", json);
}

// [路由 3] 裁判違規扣分 API 處理 (/api/penalty)
void handlePenalty() {
  if (server.hasArg("team")) {
    String team = server.arg("team");
    
    // 紅隊扣分 logic (加入防呆限制：最低只扣到 0 分)
    if (team == "red") {
      if (redScore > 0) redScore--;
      Serial.println("[裁判操作] 紅隊違規，扣 1 分！");
    } 
    // 藍隊扣分 logic
    else if (team == "blue") {
      if (blueScore > 0) blueScore--;
      Serial.println("[裁判操作] 藍隊違規，扣 1 分！");
    }
  }
  // 處理完畢後直接回傳最新 JSON 分數
  handleGetScore();
}

// [路由 4] 分數歸零重置 API 處理 (/api/reset)
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
  // 1. 初始化序列埠（通訊速率 115200 baud）
  Serial.begin(115200);

  // 2. 設定 FC-51 感測器輸入腳位
  pinMode(RED_SENSOR_PIN, INPUT);
  pinMode(BLUE_SENSOR_PIN, INPUT);

  // 3. 連接 Wi-Fi 網路
  WiFi.begin(ssid, password);
  Serial.print("正在連接 Wi-Fi 網路");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[系統通知] Wi-Fi 連接成功！");
  Serial.print("👉 請在瀏覽器輸入本機 IP 位址： http://");
  Serial.println(WiFi.localIP());

  // 4. 綁定網頁與 API 的 HTTP 路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/score", HTTP_GET, handleGetScore);
  server.on("/api/penalty", HTTP_POST, handlePenalty);
  server.on("/api/penalty", HTTP_GET, handlePenalty); // 兼容量取的 GET 請求
  server.on("/api/reset", HTTP_POST, handleResetScore);
  server.on("/api/reset", HTTP_GET, handleResetScore);
  
  // 5. 啟動 Web 伺服器
  server.begin();
  Serial.println("[系統通知] 網頁伺服器已成功啟動！");
}

// ======================================================
// 6. 主程式重覆執行迴圈 (loop)
// ======================================================
void loop() {
  // 1. 持續處理來自網頁的 HTTP 請求
  server.handleClient();

  // 2. 讀取兩組 FC-51 感測器的數位電位 (HIGH/LOW)
  int currentRedState = digitalRead(RED_SENSOR_PIN);
  int currentBlueState = digitalRead(BLUE_SENSOR_PIN);

  // ------------------ 紅隊感測加分 ------------------
  // 下降緣觸發判斷：上一次是 HIGH (無球)，這一次變 LOW (球體遮檔)
  if (lastRedState == HIGH && currentRedState == LOW) {
    redScore++;  // 分數加 1
    Serial.printf("[感測器進球] 🔴 紅隊進球！目前比分 - 紅: %d | 藍: %d\n", redScore, blueScore);
    delay(200);  // 防手震與重複計分延遲 200ms
  }

  // ------------------ 藍隊感測加分 ------------------
  if (lastBlueState == HIGH && currentBlueState == LOW) {
    blueScore++; // 分數加 1
    Serial.printf("[感測器進球] 🔵 藍隊進球！目前比分 - 紅: %d | 藍: %d\n", redScore, blueScore);
    delay(200);  // 防手震延遲 200ms
  }

  // 3. 記錄當前的感測狀態，作為下一輪 loop 的比對依據
  lastRedState = currentRedState;
  lastBlueState = currentBlueState;
}