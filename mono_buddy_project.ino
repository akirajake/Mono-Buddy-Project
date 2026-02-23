#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <time.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ================= ユーザー設定 =================

const char* ssid = "YOUR_SSID";                      // Wi-FiのSSID
const char* password = "YOUR_PASSWORD";              // Wi-Fiのパスワード
const char* weatherApiKey = "OPENWEATHERMAP_APIKEY"; // OpenWeatherMapのAPIキー

// ================= オブジェクトと同期制御 =================

TaskHandle_t taskHandle;      // バックグラウンドタスクのハンドル
SemaphoreHandle_t dataMutex;  // マルチコア間でデータを共有する際の衝突防止用（排他制御）

// ディスプレイ設定 (SSD1306, I2C接続)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
WebServer server(80);         // Webサーバー(ポート80)
Preferences preferences;      // ESP32の内部メモリ（フラッシュ）保存用

// ================= ピン割り当て =================

#define DHTPIN 4              // 温湿度センサーのデータピン
#define DHTTYPE DHT11         // センサーの種類
DHT dht(DHTPIN, DHTTYPE);

#define BTN_LEFT 12           // 左ボタン
#define BTN_RIGHT 13          // 右ボタン
#define BTN_SELECT 14         // 決定（ピン留め）ボタン
#define LED_PIN 2             // 通知用LED（内蔵LED）

// ================= 画面サイズ定義 =================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ================= データとステータス変数 =================

// 設定用変数
String settings_mcAddress = "mc.hypixel.net"; // マイクラサーバーのアドレス
String settings_weatherCity = "Sapporo,jp";   // 天気を取得する都市
const char* weatherURLBase = "http://api.openweathermap.org/data/2.5/weather?units=metric&appid=";

// 共有データ (バックグラウンドで更新し、メインで描画するもの)
volatile float shared_outTemp = 0;           // 外気温
String shared_weatherMain = "--";             // 天気状態
volatile float shared_roomTemp = 0;          // 室温
volatile float shared_roomHum = 0;           // 室内湿度

// グラフ描画用データ
#define GRAPH_LEN 64                          // グラフの履歴数
float tempHistory[GRAPH_LEN];                 // 温度履歴の配列
int tempHistoryIdx = 0;                       // 次に保存する配列のインデックス
unsigned long lastTempRec = 0;                // 最後に記録した時間
unsigned long nextTempRec = 0;                // 次回記録予定の時間

// ================= ニュースシステム =================

struct NewsSource {
  const char* name;                           // ジャンル名
  const char* url;                            // RSSのURL
};

// NHK RSS配信リスト
const NewsSource newsSources[] = {
  { "NHK 主要", "https://www3.nhk.or.jp/rss/news/cat0.xml" },
  { "NHK 社会", "https://www3.nhk.or.jp/rss/news/cat1.xml" },
  { "NHK 科学", "https://www3.nhk.or.jp/rss/news/cat3.xml" },
  { "NHK 国際", "https://www3.nhk.or.jp/rss/news/cat6.xml" },
  { "NHK 経済", "https://www3.nhk.or.jp/rss/news/cat5.xml" }
};
const int newsSourceCount = 5;

int currentNewsIdx = 0;                       // 現在表示中のニュースジャンル番号
String shared_newsTitle = "ニュースを取得中..."; // ニュース見出し
String shared_newsCategory = "NHK NEWS";      // カテゴリ名
bool reqNewsUpdate = true;                    // 更新が必要かどうかのフラグ
bool isNewsLoading = false;                   // 通信中フラグ

// Minecraftサーバー情報
const char* mcApiUrl = "https://api.mcsrvstat.us/3/";
bool shared_mcOnline = false;                 // サーバー稼働状況
int shared_mcPlayers = 0;                     // 現在の人数
int shared_mcMaxPlayers = 0;                  // 最大人数
String shared_mcMotd = "Loading...";          // サーバーメッセージ

// 通知システム
bool hasNotification = false;                 // 通知があるか
String notificationMsg = "";                  // 通知メッセージ内容
unsigned long notificationTime = 0;           // 通知が発生した時刻

// 画面遷移（ステートマシン）の定義
enum Screen { SCR_FACE,      // 0: 顔（待機）
              SCR_CLOCK,     // 1: 時計
              SCR_WEATHER,   // 2: 外の天気
              SCR_TEMP,      // 3: 部屋の温湿度・グラフ
              SCR_NEWS,      // 4: ニュース
              SCR_MINECRAFT, // 5: マイクラ情報
              SCR_INFO,      // 6: ネットワーク情報
              SCR_MAX };     // 画面の総数

Screen currentScreen = SCR_FACE; // 現在の画面
Screen targetScreen = SCR_FACE;  // 遷移先の画面

// アニメーション用変数
bool isAnimating = false;        // スライド中か
int16_t animOffset = 0;          // 横方向のズレ量
int animDir = 1;                 // スライド方向 (1 or -1)
bool isPinned = false;           // 画面を固定（オートモードOFF）
unsigned long lastInputTime = 0; // 最後に操作した時間
const unsigned long IDLE_TIMEOUT = 15000; // 15秒放置で「顔」に戻る

// 顔の表情・アニメーション用
unsigned long lastBlink = 0;     // まばたき管理
bool isBlinking = false;
unsigned long nextGazeMove = 0;  // 視線移動管理
int curGazeX = 0, curGazeY = 0;  // 現在の黒目位置
int gazeX = 0, gazeY = 0;        // 目標の黒目位置

// テキストスクロール用
int newsScrollX = SCREEN_WIDTH;  // ニュースの見出し位置
int newsTextWidth = 0;
int mcScrollX = SCREEN_WIDTH;    // マイクラメッセージの位置

// ボタン制御用
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200; // チャタリング防止時間

// ================= プロトタイプ宣言 (関数があることを先に伝える) =================
void drawScreenContent(Screen, int16_t, int16_t);
void drawFace(int16_t, int16_t);
void drawClock(int16_t, int16_t);
void drawWeather(int16_t, int16_t);
void drawTemp(int16_t, int16_t);
void drawNews(int16_t, int16_t);
void drawMinecraft(int16_t, int16_t);
void drawNetworkInfo(int16_t, int16_t);
void drawNotificationOverlay();
void drawCenterString(String, int, const uint8_t*, int16_t);
void updateFacePhysics();
void drawHeader();
void drawPagination();
void handleButtons();
void startTransition(Screen, int);
void setupWebServer();
void loadSettings();
void saveSettings(String, String);

// ================= バックグラウンドタスク (Core 0で動作) =================
// 描画を止めないように、Wi-Fi通信などの重い処理はここで行います
void backgroundTask(void* pv) {
  unsigned long lastWeather = 0;
  unsigned long lastMcFetch = 0;

  // 起動時にグラフ配列を初期化
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < GRAPH_LEN; i++) tempHistory[i] = 0;
  xSemaphoreGive(dataMutex);

  while (true) {
    unsigned long now = millis();
    // グラフ画面表示中は更新を早める(5秒)、それ以外は1分おき
    unsigned long tempInterval = (currentScreen == SCR_TEMP) ? 5000 : 60000;

    // --- DHT11 センサー計測 ---
    if (now - lastTempRec > tempInterval || lastTempRec == 0) {
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      xSemaphoreTake(dataMutex, portMAX_DELAY); // 共有データをいじる前にロック
      if (!isnan(t)) {
        shared_roomTemp = t;
        tempHistory[tempHistoryIdx] = t;
        tempHistoryIdx = (tempHistoryIdx + 1) % GRAPH_LEN;
      }
      if (!isnan(h)) shared_roomHum = h;
      xSemaphoreGive(dataMutex); // ロック解除
      lastTempRec = now;
      nextTempRec = now + tempInterval;
    }

    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // --- 天気情報取得 (30分おき) ---
    if (now - lastWeather > 1800000 || lastWeather == 0) {
      HTTPClient http;
      http.begin(String(weatherURLBase) + weatherApiKey + "&q=" + settings_weatherCity);
      if (http.GET() == 200) {
        String payload = http.getString();
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        int p = payload.indexOf("\"temp\":");
        if (p > 0) shared_outTemp = payload.substring(p + 7).toFloat();
        int w = payload.indexOf("\"main\":\"");
        if (w > 0) {
          int e = payload.indexOf("\"", w + 8);
          shared_weatherMain = payload.substring(w + 8, e);
        }
        xSemaphoreGive(dataMutex);
      }
      http.end();
      lastWeather = now;
    }

    // --- ニュース取得 (フラグが立った時) ---
    if (reqNewsUpdate) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      isNewsLoading = true;
      xSemaphoreGive(dataMutex);

      WiFiClientSecure client;
      client.setInsecure(); // SSL証明書の検証をスキップ
      HTTPClient https;
      const char* url = newsSources[currentNewsIdx].url;
      const char* name = newsSources[currentNewsIdx].name;

      if (https.begin(client, url)) {
        if (https.GET() == 200) {
          String xml = https.getString();
          int item = xml.indexOf("<item>"); // 最初の記事を探す
          int t1 = xml.indexOf("<title>", item);
          int t2 = xml.indexOf("</title>", t1);
          String title = "取得エラー";
          if (t1 > 0 && t2 > t1) {
            title = xml.substring(t1 + 7, t2);
            // 特殊文字の置換
            title.replace("&amp;", "&");
            title.replace("&#039;", "'");
            title.replace("&quot;", "\"");
          }
          xSemaphoreTake(dataMutex, portMAX_DELAY);
          shared_newsTitle = title;
          shared_newsCategory = String(name);
          reqNewsUpdate = false;
          isNewsLoading = false;
          xSemaphoreGive(dataMutex);
        } else {
          xSemaphoreTake(dataMutex, portMAX_DELAY);
          shared_newsTitle = "News Fetch Error";
          reqNewsUpdate = false;
          isNewsLoading = false;
          xSemaphoreGive(dataMutex);
        }
        https.end();
      }
    }

    // --- Minecraft サーバー情報取得 (5分おき) ---
    if (lastMcFetch == 0 || now - lastMcFetch > 300000) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient https;
      String url = String(mcApiUrl) + settings_mcAddress;
      if (https.begin(client, url)) {
        if (https.GET() == 200) {
          String json = https.getString();
          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, json);
          if (!error) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            shared_mcOnline = doc["online"];
            if (shared_mcOnline) {
              shared_mcPlayers = doc["players"]["online"];
              shared_mcMaxPlayers = doc["players"]["max"];
              const char* cleanMotd = doc["motd"]["clean"][0];
              if (cleanMotd) shared_mcMotd = String(cleanMotd);
              else shared_mcMotd = "Online";
            } else {
              shared_mcMotd = "Offline";
            }
            xSemaphoreGive(dataMutex);
          }
        }
        https.end();
        lastMcFetch = now;
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS); // タスクを少し休ませる（重要）
  }
}

// ================= 設定の読み書き (Preferences) =================
void loadSettings() {
  preferences.begin("my-app", false);
  String savedAddr = preferences.getString("mc_addr", "");
  String savedCity = preferences.getString("city", "");
  if (savedAddr != "") settings_mcAddress = savedAddr;
  if (savedCity != "") settings_weatherCity = savedCity;
  preferences.end();
}

void saveSettings(String addr, String city) {
  preferences.begin("my-app", false);
  if (addr.length() > 0) { preferences.putString("mc_addr", addr); settings_mcAddress = addr; }
  if (city.length() > 0) { preferences.putString("city", city); settings_weatherCity = city; }
  preferences.end();
}

// ================= 初期設定 (SETUP) =================
void setup() {
  Serial.begin(115200);
  dataMutex = xSemaphoreCreateMutex(); // ロック用のミューテックス作成

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  loadSettings(); // 保存された設定を読み込む
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  dht.begin();
  u8g2.begin();
  u8g2.enableUTF8Print(); // 日本語表示（UTF8）を有効化
  u8g2.setBitmapMode(1);

  // Wi-Fi接続
  WiFi.begin(ssid, password);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, "Connecting WiFi...");
  u8g2.sendBuffer();
  while (WiFi.status() != WL_CONNECTED) delay(500);

  // mDNS ( http://esp32.local でアクセス可能にする)
  if (!MDNS.begin("esp32")) Serial.println("Error starting mDNS");
  setupWebServer();
  server.begin();

  // NTP時刻同期 (日本時間)
  configTime(9 * 3600, 0, "ntp.nict.jp");

  // バックグラウンドタスクの起動 (Core 0を指定)
  xTaskCreatePinnedToCore(backgroundTask, "BG", 8192, NULL, 1, &taskHandle, 0);
}

// ================= メインループ (LOOP) =================
void loop() {
  server.handleClient(); // Webサーバーへのリクエスト処理
  handleButtons();       // ボタン入力の処理

  // 通知LEDの点滅処理
  if (hasNotification) {
    if ((millis() / 250) % 2 == 0) digitalWrite(LED_PIN, HIGH);
    else digitalWrite(LED_PIN, LOW);

    if (millis() - notificationTime > 5000) { // 5秒で通知消去
      hasNotification = false;
      digitalWrite(LED_PIN, LOW);
    }
  }

  // 自動的に「顔」に戻る処理
  if (!isPinned && !isAnimating && !hasNotification &&
      currentScreen != SCR_FACE &&
      millis() - lastInputTime > IDLE_TIMEOUT) {
    startTransition(SCR_FACE, -1);
  }

  // 画面遷移アニメーションの計算
  if (isAnimating) {
    animOffset += animDir * 16; // 16pxずつ動かす
    if (abs(animOffset) >= SCREEN_WIDTH) {
      isAnimating = false;
      currentScreen = targetScreen;
      animOffset = 0;
    }
  }

  updateFacePhysics(); // 表情の更新
  u8g2.clearBuffer();

  // 描画処理
  if (isAnimating) {
    drawScreenContent(currentScreen, -animOffset, 0);
    drawScreenContent(targetScreen, (animDir == 1 ? SCREEN_WIDTH : -SCREEN_WIDTH) - animOffset, 0);
  } else {
    drawScreenContent(currentScreen, 0, 0);
  }

  if (hasNotification) drawNotificationOverlay();

  drawHeader();     // アイコンなど上部パーツ
  drawPagination(); // ページドット
  u8g2.sendBuffer();
}

// ================= WEB サーバー設定 =================
// ブラウザから設定を変更したり通知を送ったりするためのHTMLを生成
void setupWebServer() {
  server.on("/", []() {
    String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
    html += "<style>body{font-family:sans-serif;text-align:center;background:#f4f4f4;padding:10px;margin:0;}";
    html += ".card{background:#fff;padding:15px;border-radius:12px;box-shadow:0 2px 5px rgba(0,0,0,0.1);margin:10px auto;max-width:400px;}";
    html += "canvas{width:100%;height:180px;} input,button{padding:10px;margin:5px;border-radius:5px;border:1px solid #ddd;width:90%;}";
    html += ".btn-grid{display:grid;grid-template-columns:1fr 1fr;gap:5px;}";
    html += ".btn-action{background:#007bff;color:white;border:none;cursor:pointer;}";
    html += ".btn-quick{background:#28a745;color:white;border:none;cursor:pointer;font-size:0.9em;}";
    html += "</style></head><body>";

    html += "<h3>ESP32 Dashboard</h3>";

    // 部屋の温湿度
    html += "<div class='card'><h4>Room Temp</h4>";
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    html += "<p>" + String(shared_roomTemp, 1) + " C / " + String(shared_roomHum, 1) + " %</p>";
    String dataArr = "[";
    for (int i = 0; i < GRAPH_LEN; i++) {
      int idx = (tempHistoryIdx + i) % GRAPH_LEN;
      if (tempHistory[idx] != 0) {
        dataArr += String(tempHistory[idx], 1);
        if (i < GRAPH_LEN - 1) dataArr += ",";
      }
    }
    dataArr += "]";
    xSemaphoreGive(dataMutex);
    html += "<canvas id='tempChart'></canvas></div>";
    // Chart.jsによるグラフ生成スクリプト
    html += "<script>new Chart(document.getElementById('tempChart'),{type:'line',data:{labels:Array(" + String(GRAPH_LEN) + ").fill(''),datasets:[{label:'Temp',data:" + dataArr + ",borderColor:'#ff6384',tension:0.1,pointRadius:0}]}});</script>";

    // 通知送信フォーム
    html += "<div class='card'><h4>Send Notification</h4>";
    html += "<input type='text' id='msg' placeholder='Type message...'>";
    html += "<button class='btn-action' onclick='sendMsg()'>Send Text</button>";
    html += "<div class='btn-grid'>";
    html += "<button class='btn-quick' onclick=\"sendPre('Tea Ready')\">🍵 Tea</button>";
    html += "<button class='btn-quick' onclick=\"sendPre('Bath Ready')\">🛁 Bath</button>";
    html += "<button class='btn-quick' onclick=\"sendPre('Meeting')\">📅 Meeting</button>";
    html += "<button class='btn-quick' onclick=\"sendPre('Love You')\">❤️ Love</button>";
    html += "</div></div>";

    // マイクラ状況
    html += "<div class='card'><h4>Minecraft</h4>";
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    html += "<small>" + settings_mcAddress + "</small><br>";
    html += "<b>" + String(shared_mcOnline ? "ONLINE" : "OFFLINE") + "</b>";
    if (shared_mcOnline) html += "<p>" + String(shared_mcPlayers) + " / " + String(shared_mcMaxPlayers) + " Players</p>";
    xSemaphoreGive(dataMutex);
    html += "</div>";

    // 設定保存
    html += "<div class='card'><h4>Settings</h4><form action='/save' method='POST'>";
    html += "<input type='text' name='mc' value='" + settings_mcAddress + "' placeholder='MC Server'>";
    html += "<input type='text' name='city' value='" + settings_weatherCity + "' placeholder='City'>";
    html += "<button type='submit' class='btn-action'>Save Settings</button></form></div>";

    html += "<script>function sendMsg(){var m=document.getElementById('msg').value;if(m) fetch('/notify?msg='+encodeURIComponent(m)); document.getElementById('msg').value='';}";
    html += "function sendPre(m){fetch('/notify?msg='+encodeURIComponent(m));}</script>";

    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // 設定保存リクエストのハンドラ
  server.on("/save", []() {
    if (server.hasArg("mc") || server.hasArg("city")) {
      saveSettings(server.arg("mc"), server.arg("city"));
      server.sendHeader("Location", "/");
      server.send(303);
    } else server.send(400, "text/plain", "Bad Request");
  });

  // 通知リクエストのハンドラ
  server.on("/notify", []() {
    if (server.hasArg("msg")) {
      notificationMsg = server.arg("msg");
      hasNotification = true;
      notificationTime = millis();
      server.send(200, "text/plain", "OK");
    } else server.send(400, "text/plain", "Missing msg");
  });
}

// ================= 各画面の描画関数 =================

// 天気アイコン（フォント内のグリフ番号）を返す
char getWeatherIconChar(String main) {
  main.toLowerCase();
  if (main.indexOf("clear") >= 0) return 69;
  if (main.indexOf("cloud") >= 0) return 65;
  if (main.indexOf("rain") >= 0) return 67;
  if (main.indexOf("drizzle") >= 0) return 67;
  if (main.indexOf("thunder") >= 0) return 72;
  if (main.indexOf("snow") >= 0) return 68;
  return 66; // その他
}

// 外の天気画面
void drawWeather(int16_t x, int16_t y) {
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x + 2, y + 2, 124, 60, 5); // 枠

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  float temp = shared_outTemp;
  String cond = shared_weatherMain;
  xSemaphoreGive(dataMutex);

  u8g2.setFont(u8g2_font_open_iconic_weather_4x_t);
  u8g2.drawGlyph(x + 10, y + 45, getWeatherIconChar(cond));

  u8g2.setFont(u8g2_font_helvB24_tf);
  u8g2.setCursor(x + 50, y + 38);
  u8g2.print((int)temp);
  u8g2.setFont(u8g2_font_helvB12_tf);
  u8g2.print("C");

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(x + 50, y + 55);
  u8g2.print(cond);
}

// 室内温湿度＆グラフ画面
void drawTemp(int16_t x, int16_t y) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  float rt = shared_roomTemp;
  float rh = shared_roomHum;
  xSemaphoreGive(dataMutex);

  u8g2.setFont(u8g2_font_helvB14_tf);
  u8g2.setCursor(x + 4, y + 18);
  u8g2.print(rt, 1); u8g2.print("C");
  u8g2.setCursor(x + 70, y + 18);
  u8g2.print(rh, 0); u8g2.print("%");

  int gy = y + 22; // グラフのY開始位置
  int gh = 40;     // グラフの高さ
  u8g2.drawFrame(x, gy, 128, gh + 2); // グラフ外枠

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  float minT = 10.0, maxT = 35.0; // グラフの上下限値
  for (int i = 0; i < GRAPH_LEN - 1; i++) {
    int idx = (tempHistoryIdx + i) % GRAPH_LEN;
    int nextIdx = (tempHistoryIdx + i + 1) % GRAPH_LEN;
    float v1 = tempHistory[idx];
    float v2 = tempHistory[nextIdx];
    if (v1 == 0 || v2 == 0) continue;
    int py1 = (gy + gh) - (int)((v1 - minT) / (maxT - minT) * gh);
    int py2 = (gy + gh) - (int)((v2 - minT) / (maxT - minT) * gh);
    // 範囲外のクリッピング
    if (py1 < gy) py1 = gy; if (py1 > gy + gh) py1 = gy + gh;
    if (py2 < gy) py2 = gy; if (py2 > gy + gh) py2 = gy + gh;
    u8g2.drawLine(x + i * 2, py1, x + (i + 1) * 2, py2);
  }
  xSemaphoreGive(dataMutex);

  // 次回更新までのカウントダウン表示
  if (!isAnimating) {
    long remaining = (long)(nextTempRec - millis()) / 1000;
    if (remaining < 0) remaining = 0;
    u8g2.setFont(u8g2_font_micro_tr);
    u8g2.setCursor(x + 115, y + 62);
    u8g2.print(remaining);
  }
}

// ニュース画面 (自動的にジャンルを切り替える)
void drawNews(int16_t x, int16_t y) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  String title = shared_newsTitle;
  String cat = shared_newsCategory;
  bool loading = isNewsLoading;
  xSemaphoreGive(dataMutex);

  // 上部のカテゴリバー
  u8g2.setDrawColor(1);
  u8g2.drawBox(x, y, 128, 16);
  u8g2.setDrawColor(0);
  drawCenterString(cat, y + 13, u8g2_font_7x14B_tf, x);

  u8g2.setDrawColor(1);
  if (loading) {
    drawCenterString("Loading next...", y + 45, u8g2_font_6x10_tf, x);
    newsScrollX = SCREEN_WIDTH;
  } else {
    // 日本語フォント（Unifont）を使用
    u8g2.setFont(u8g2_font_unifont_t_japanese1);
    newsTextWidth = u8g2.getUTF8Width(title.c_str());

    u8g2.setClipWindow(x, y + 18, x + 128, y + 64); // 描画範囲を制限
    u8g2.setCursor(x + newsScrollX, y + 45);
    u8g2.print(title);
    u8g2.setMaxClipWindow();

    if (!isAnimating && !hasNotification) {
      newsScrollX -= 2; // スクロール
      if (newsScrollX < -newsTextWidth) { // 端まで行ったらジャンル変更
        currentNewsIdx = (currentNewsIdx + 1) % newsSourceCount;
        reqNewsUpdate = true;
        newsScrollX = SCREEN_WIDTH;
      }
    }
  }
}

// ネットワーク設定情報画面
void drawNetworkInfo(int16_t x, int16_t y) {
  u8g2.drawBox(x, y, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(x + 4, y + 10, "DASHBOARD");
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenterString("Access via Browser:", y + 30, u8g2_font_6x10_tf, x);
  String ip = WiFi.localIP().toString();
  u8g2.setFont(u8g2_font_helvB10_tf);
  drawCenterString(ip, y + 50, u8g2_font_helvB10_tf, x);
  u8g2.setFont(u8g2_font_5x7_tf);
  drawCenterString("http://" + ip, y + 62, u8g2_font_5x7_tf, x);
}

// 通知オーバーレイ (すべての画面の上に重なる)
void drawNotificationOverlay() {
  u8g2.setDrawColor(0);
  u8g2.drawBox(10, 15, 108, 34); // 背景を塗りつぶして後ろを消す
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(10, 15, 108, 34, 4);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenterString("NOTIFICATION", 26, u8g2_font_6x10_tf, 0);
  drawCenterString(notificationMsg, 42, u8g2_font_helvB10_tf, 0);
}

// Minecraft 情報画面
void drawMinecraft(int16_t x, int16_t y) {
  u8g2.drawBox(x, y, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(x + 2, y + 10, "MINECRAFT");
  u8g2.setDrawColor(1);

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  bool online = shared_mcOnline;
  int players = shared_mcPlayers;
  int maxP = shared_mcMaxPlayers;
  String motd = shared_mcMotd;
  xSemaphoreGive(dataMutex);

  if (!online) {
    u8g2.setFont(u8g2_font_helvB10_tf);
    drawCenterString("OFFLINE", y + 40, u8g2_font_helvB10_tf, x);
    return;
  }

  u8g2.setFont(u8g2_font_helvB14_tf);
  String p = String(players) + "/" + String(maxP);
  drawCenterString(p, y + 35, u8g2_font_helvB14_tf, x);

  u8g2.setFont(u8g2_font_6x10_tf);
  int w = u8g2.getStrWidth(motd.c_str());
  u8g2.setClipWindow(x, y + 40, x + 128, y + 64);
  if (w > 120) {
    u8g2.setCursor(x + mcScrollX, y + 55);
    u8g2.print(motd);
    if (!isAnimating) {
      mcScrollX -= 2;
      if (mcScrollX < -w) mcScrollX = SCREEN_WIDTH;
    }
  } else {
    drawCenterString(motd, y + 55, u8g2_font_6x10_tf, x);
    mcScrollX = SCREEN_WIDTH;
  }
  u8g2.setMaxClipWindow();
}

// 顔描画 (物理演算もどきで視線が動く)
void drawFace(int16_t x, int16_t y) {
  int eyeW = 20, eyeH = 18, eyeGap = 40, cx = 64 + x, cy = 32 + y;
  if (hasNotification) { eyeH = 26; eyeW = 22; } // 通知時は目を見開く

  if (isBlinking && !hasNotification) {
    u8g2.drawLine(cx - eyeGap / 2 - eyeW / 2, cy, cx - eyeGap / 2 + eyeW / 2, cy);
    u8g2.drawLine(cx + eyeGap / 2 - eyeW / 2, cy, cx + eyeGap / 2 + eyeW / 2, cy);
  } else {
    u8g2.drawRFrame(cx - eyeGap / 2 - eyeW / 2, cy - eyeH / 2, eyeW, eyeH, 4);
    u8g2.drawRFrame(cx + eyeGap / 2 - eyeW / 2, cy - eyeH / 2, eyeW, eyeH, 4);
    int gx = hasNotification ? 0 : curGazeX;
    int gy = hasNotification ? 0 : curGazeY;
    u8g2.drawBox(cx - eyeGap / 2 - 3 + gx, cy - 3 + gy, 6, 6); // 黒目
    u8g2.drawBox(cx + eyeGap / 2 - 3 + gx, cy - 3 + gy, 6, 6);
  }
}

// デジタル時計画面
void drawClock(int16_t x, int16_t y) {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  char t[9], d[20];
  strftime(t, sizeof(t), "%H:%M:%S", &ti);
  strftime(d, sizeof(d), "%Y/%m/%d", &ti);
  u8g2.drawRFrame(x + 8, y + 10, 112, 44, 4);
  drawCenterString(t, y + 35, u8g2_font_helvB18_tf, x);
  drawCenterString(d, y + 49, u8g2_font_6x10_tf, x);
}

// 現在の画面に応じたコンテンツの呼び出し分岐
void drawScreenContent(Screen s, int16_t x, int16_t y) {
  if (s == SCR_FACE) drawFace(x, y);
  else if (s == SCR_CLOCK) drawClock(x, y);
  else if (s == SCR_WEATHER) drawWeather(x, y);
  else if (s == SCR_TEMP) drawTemp(x, y);
  else if (s == SCR_NEWS) drawNews(x, y);
  else if (s == SCR_MINECRAFT) drawMinecraft(x, y);
  else if (s == SCR_INFO) drawNetworkInfo(x, y);
}

// ================= 入力・制御ロジック =================

// ボタン入力の監視
void handleButtons() {
  bool l = digitalRead(BTN_LEFT) == LOW;
  bool r = digitalRead(BTN_RIGHT) == LOW;
  bool s = digitalRead(BTN_SELECT) == LOW;

  if (millis() - lastButtonPress < DEBOUNCE_DELAY) return;

  if (s) { // セレクトボタン：通知消去 or 画面固定
    if (hasNotification) hasNotification = false;
    else isPinned = !isPinned;
    lastButtonPress = millis();
    lastInputTime = millis();
  }
  if (isPinned || hasNotification) return;

  if (l) { // 左ボタン：前の画面へ
    startTransition((Screen)((currentScreen + SCR_MAX - 1) % SCR_MAX), -1);
    lastButtonPress = millis();
    lastInputTime = millis();
  }
  if (r) { // 右ボタン：次の画面へ
    startTransition((Screen)((currentScreen + 1) % SCR_MAX), 1);
    lastButtonPress = millis();
    lastInputTime = millis();
  }
}

// 画面切り替えアニメーションの開始設定
void startTransition(Screen next, int dir) {
  if (isAnimating || next == currentScreen) return;
  targetScreen = next;
  animDir = dir;
  animOffset = 0;
  isAnimating = true;
  if (next == SCR_NEWS) newsScrollX = SCREEN_WIDTH;
  if (next == SCR_MINECRAFT) mcScrollX = SCREEN_WIDTH;
}

// 顔のまばたき・視線移動の物理計算
void updateFacePhysics() {
  unsigned long now = millis();
  if (now - lastBlink > 3000 + random(2000)) { isBlinking = true; lastBlink = now; }
  if (isBlinking && now - lastBlink > 150) isBlinking = false;
  if (now > nextGazeMove) {
    gazeX = random(-8, 9); gazeY = random(-4, 5);
    nextGazeMove = now + random(1000, 4000);
  }
  if (curGazeX < gazeX) curGazeX++; if (curGazeX > gazeX) curGazeX--;
}

// 画面上部の共通ヘッダー（Wi-Fi、ロック状態）
void drawHeader() {
  if (WiFi.status() == WL_CONNECTED) {
    int x = 110, y = 2;
    u8g2.drawDisc(x + 6, y + 6, 1);
    u8g2.drawCircle(x + 6, y + 6, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.drawCircle(x + 6, y + 6, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  }
  if (isPinned) {
    u8g2.drawBox(0, 0, 25, 11);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(2, 8, "LOCK");
    u8g2.setDrawColor(1);
  }
}

// 画面下部のページドット
void drawPagination() {
  int sx = (SCREEN_WIDTH - (SCR_MAX * 8)) / 2;
  for (int i = 0; i < SCR_MAX; i++) {
    if (i == currentScreen && !isAnimating) u8g2.drawBox(sx + i * 8, 60, 4, 2);
    else u8g2.drawPixel(sx + i * 8 + 1, 61);
  }
}

// 文字列を中央揃えで描画するヘルパー関数
void drawCenterString(String text, int y, const uint8_t* font, int16_t ox) {
  u8g2.setFont(font);
  int w = u8g2.getUTF8Width(text.c_str());
  u8g2.setCursor((SCREEN_WIDTH - w) / 2 + ox, y);
  u8g2.print(text);
}
